# Architecture

How the pieces fit together. For per-subsystem detail see [SUBSYSTEMS.md](SUBSYSTEMS.md).

## Boot chain

```
Android (HyperOS)  ──adb reboot bootloader──▶  stock fastboot
        │
        └─ fastboot boot <image>      (RAM only, nothing is flashed)
                 │
                 ▼
   Mu-Silicium UEFI  (shennongPkg, built from edk2 + the phone's own DXE drivers, byte-patched)
                 │   the boot shim copies the "XNUP" payload container to PAYLOAD_BASE (0xC7800000, 64 MB)
                 ▼
   HackiOS loader  (xnuloader/loader.efi, an EFI application)
                 │   - speaks the fastboot protocol over USB: this is what `./xl` talks to
                 │   - holds the kernel, a small ramdisk (root.apfs) and, resident in RAM, the big one (system.apfs)
                 │   - builds the XNU boot arguments, device tree and memory map
                 ▼
   XNU 8796 (iOS 16.5 kernel)  + drivers in-kernel (bsd/dev/*.c, iokit/Kernel/*.cpp)
                 │
                 ▼
   launchd → our init → backboardd → SpringBoard   (iOS 16.5 user space, unmodified)
```

* The UEFI is the **only** firmware stage replaced. The phone's own XBL, TrustZone, hypervisor and RPM/AOP firmware keep
  running; the kernel talks to them (SCM calls to TrustZone, RPMh to the AOP, GLINK/QMI to the DSPs).
* A run is a RAM session. The loader returns to fastboot when the kernel's time is up (`XL_SECS`,
  `loader_return_after`), when `./xl stop` is issued, or when the kernel ends it; the log is fetched afterwards.
* The *installed* variant (`build-install.sh`) packs UEFI + loader (with boot menu) + kernel + root.apfs in a boot image
  for slot `boot_b`, so the phone shows a menu at power-on, with Android one choice away.

## Memory

* DRAM starts at `0x80000000`. XNU gets the largest free block, about 9 GB on this phone; the kernel zones
  claim 12 GB of kernel address space.
* `PAYLOAD_BASE 0xC7800000` (64 MB) holds the payload container. It sits below Android's reserved region
  (`0xCB800000–0xD7FFFFFF`). The boot shim only keeps address bits 16–31, so it must be below 4 GB and 64 KB aligned.
* The framebuffer is the firmware's splash region, `0xFC400000`, 43 MB.
* Several drivers need DMA-able memory below 4 GB that the kernel's allocators do not hand out; they carve fixed areas
  (e.g. the audio domain's heap) from firmware-free memory.

## Storage

* The phone's UFS has its normal Android partitions; HackiOS uses one extra GPT partition, **`darwin`**, carved at
  install time (`scripts/ufs-carve.sh`, `ufs-layout.sh`).
* `system.apfs` (the iOS system volume, about 2.5 GB) either goes into RAM with `xl ramdisk FILE` or is read by the loader
  from `darwin` (`xl ramdisk flash`, no upload).
* `/private/var` is a **writable APFS on flash** (`var_apfs=<blocks>`, `ufs_write=1`, `ufs_bwrite=1`), served by our
  own APFS implementation (`src/apfs-rw`). Apps and data therefore survive reboots.
* `root.apfs` (small, built in seconds) carries our own programs, firmware pulled from the phone (`fw/`), and the
  userland glue; it is mounted at `/redwood`.
* The last blocks of `darwin` are reserved by the loader (boot-menu log from block 2091520 up, menu kernel arguments at
  block 2096128): **never write raw blocks there**.

## Kernel drivers

XNU is not Linux: there is no device-tree driver model for this hardware, and Apple's own drivers do not support
Qualcomm silicon. So each block is a **small purpose-written driver compiled into the kernel**, switched on by a
boot-arg. Files live in `xnu/files/` of the HackiOS tree (the port adds them to upstream):

| File | Block |
|---|---|
| `bsd/dev/shennong_*.c` | Things written for this phone: `adreno` (GPU), `synaptics` (touch), `wlan` (Wi-Fi bring-up), `bt`, `camera`, `sound`, `speaker`, `haptic`, `torch`, `sensors`, `thermal`, `mic` |
| `bsd/dev/redwood_*.c` | The older POCO X5 Pro (redwood) drivers this port reuses: UFS host controller, ADSP/GLINK/QMI, cpufreq, buttons, battery, Wi-Fi LinuxKPI glue (`redwood_lx.c`), the ath12k bridge, and so on. Where the port changes them it makes them chip-aware by the phone's name (`redwood_phone()`) |
| `iokit/Kernel/RedwoodGPU.cpp`, `MiniIOMobileFramebuffer.cpp`, `RedwoodBattery.cpp`, `RedwoodWLAN.cpp`, `ShennongBacklight` | IOKit-facing services that give iOS something it understands (IOAccelerator, IOMobileFramebuffer, power source, Wi-Fi interface, backlight) |
| `libkern/amfi/miniamfi.c` | AMFI stub: no code-signing checks (any signature accepted), which is why sideloading is easy |

Conventions worth knowing before reading the code:

* **Bring-up steps are numbered boot-args.** `sgpu=N`, `cam=N`, `wlan=N`, `sound=N`, `bt=N`, `battmgr=N`, `gyro=N` run the
  driver up to step N and stop. Each step prints what it read or wrote. This is how every block here was brought up (see
  [DEBUGGING.md](DEBUGGING.md)).
* Hardware is accessed with **physical addresses and `io_map`**. Kernel I/O mappings are permanent in this kernel, so never
  map large or repeated regions (map the few pages you need, once).
* Clocks/regulators/bandwidth are voted **directly** through RPMh (cmd-db names) and GCC/CAMCC/DISPCC registers; there is
  no clock framework. Hardware-gated branches must not be waited on.
* The Wi-Fi radio driver is Linux's **ath12k**, compiled against a LinuxKPI-style compatibility layer (`wifi/lx12/` in the
  tree, `redwood_lx.c` in the kernel). Only its bus and the phone's side of the driver object are new.
* Audio uses the DSP's **AudioReach/GPR** graph (APM) like Android's HAL does; the packet sequences were captured from
  Android and replayed (see `build/android-capture` in the author's tree, not in this repo).

## iOS user space

iOS 16.5 (iPhone X build) is taken from Apple's IPSW on your machine. The user space is **unmodified** except for:

* our own `init` (`src/init-mg`) and a launchd job list (`mkjobs`), injected libraries and a few shims
  (BlueTool, SpringBoard hooks);
* **MobileGestalt**: iOS decides gestures and layout by a cache built for a given screen class. The cache here was built at
  1125×2436 (iPhone X class → Face-ID style gestures). Forcing it (`mg_reset=1`) at 1290×2796 or `fbscale=2` gives home-button
  (iPhone 8 class) gestures. To restore: boot once without `fb_phone` with `mg_reset=1`, then normally.
* **Graphics**: CoreAnimation renders with OpenGL ES through **Mesa 25.0.7 (freedreno, with our changes)**; a Metal
  plug-in (`hakios-metal`, shipped as `RedwoodMetal.bundle`) is loaded by the kernel-named IOAccelerator
  (`metal_plugin=1`, now the default for every process, `metal_all=0` restores the old behaviour).
* **Frame path**: iOS draws a 1125×2436 (or 1290×2796 with `fb_phone=2796`) frame; `MiniIOMobileFramebuffer` scales it to
  1440×3200 with the display's scalers or the GPU and pushes it into the command-mode panel behind its refresh.
* **TrollStore** is installed by `tsboot` at first boot ([sideload.md](sideload.md)).
