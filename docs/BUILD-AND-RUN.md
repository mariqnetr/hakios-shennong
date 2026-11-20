# Building and running

The author builds on a Mac (Apple silicon) with Podman; everything heavy runs in containers. Linux works the same way.
Nothing here writes to the phone unless a command says so explicitly.

## Requirements

* Xiaomi 14 Pro (shennong), **bootloader unlocked**, USB-C cable. Optional: root (KernelSU) on Android for the
  data-collection helpers.
* Podman or Docker, `fastboot`, Python 3, about 40 GB of disk (iOS files alone are 7.4 GB and are unpacked in a container
  volume because iOS has file names that differ only in case, which a Mac file system cannot hold).
* Network for the first build: Apple's CDN (iOS 16.5), Mesa, Mu-Silicium, edk2.

## 1. The tree

```sh
./apply.sh hackios         # upstream @ d82c64a3 with port/ copied over it
```

The build scripts in `scripts/` were written for the author's layout, with `port/` next to them: they copy `port/` into
their build directory and from there into the container's tree. Put `port/` next to the scripts first
(`ln -s ../port scripts/port`) and run them from `scripts/`. They were adapted to this layout after the author's last run and
are **not re-tested**.

## Getting what is not in the repo

Do this on **your** phone (these files are Qualcomm/Xiaomi's and are not distributed here):

| Need | How |
|---|---|
| UEFI drivers (`uefi/files/Binaries/shennong/`) | pull `uefi_a` from the phone (root: `dd`), then `tools/fvextract.py` and `tools/mkbinaries.py` from the tree (replacement for UEFIReader, no .NET). See `port/docs/PORTING-shennong.md` |
| The 4 byte patches | `uefi/files/Binaries/shennong/README.md` names the driver and the offsets: ClockDxe (display keeps its clocks), SPMIDxe/PmicDxeLa (SPMI external abort, dead-battery check), UFSDxe (START STOP UNIT to wake the device) |
| Device tree | `adb pull /sys/firmware/fdt` → `uefi/files/Resources/DTBs/shennong.dtb` |
| Firmware for GPU, ADSP, audio, camera, Wi-Fi, Bluetooth, haptics, amplifier | `scripts/get-adsp.sh`, `get-haptic.sh`, `get-zap.sh`, `tools/amp-fw.py`, `src/ksu-module/` (KernelSU module that copies what is needed). They end up in `build/fw-*` and go into `root.apfs` as `fw/` |
| Phone data for the profile | `tools/shennong-collect.sh` (added by the port) collects dtb, cpuinfo, iomem, partitions over adb |

## 2. Build

Run in this order; each script says what it needs in its header.

| Script | Result |
|---|---|
| `scripts/build-uefi.sh` (`MODE=DEBUG` for a UEFI that prints its log on screen) | Mu-Silicium UEFI and `shennong-boot.img` (UEFI + loader). x86-64 container |
| `scripts/build-kernel.sh` | the XNU kernel (`mach.development.bcm2837`), the loader, `shennong-xnu.img`. Native arm64 container |
| `scripts/build-ios.sh` | `root.apfs` (small, seconds on repeat) and `system.apfs` (iOS 16.5 + our userland, 2.5 GB) from Apple's CDN |
| `scripts/build-gl.sh` | the same images **with Mesa/Metal**: needed for a GPU-drawn UI (`gpu=1`). Run `build-ios.sh` and `build-kernel.sh` once first |
| `scripts/build-install.sh` | the installed-boot image (see below) |

Rules from experience: root.apfs and the kernel output are **shared by everything**; do not rebuild them while someone else
is flashing or running. To see those daemons' `os_log` on the console, build with `OSLOG_JOBS="com.apple.PosterBoard com.apple.SpringBoard"`
(the build scripts add `com.apple.backboardd`).

## 3. Run: `./xl`

`xl` (in the HackiOS tree) drives the loader over USB with the fastboot protocol.

```
xl enter            phone into the resident loader (from Android or stock fastboot)
xl run [N]          upload the kernel, run it, fetch the log (N: stop at checkpoint N)
xl ramdisk [FILE]   upload the big image (stays in RAM across runs); "flash" = loader reads it from darwin; "none" = forget
xl menu-args [A|none]  kernel args for the boot menu's HackiOS, kept in darwin
xl log | stop | reboot | reload | state | memmap
xl screenshot [F]   PNG of the framebuffer (works during a run, from another terminal)
xl recording [F]    video of the screen (rec_secs=N in XL_CMDLINE)
xl sound [F]        WAV of what iOS played (rec_sound=N)
xl camera [P]       pictures of the frames the camera left (cam=3 and up)
```

A normal session:

```sh
./xl reboot                      # clean state
./xl ramdisk flash               # big image from the phone's darwin partition (or: ./xl ramdisk)
XL_SECS=120 XL_CMDLINE="..." ./xl run
./xl stop                        # ends a run early
```

Environment for `run`: `XL_CMDLINE` (extra kernel boot-args, default `-v`), `XL_SECS` (seconds before the kernel returns
to the loader on its own, default 8), `XL_SB_DELAY` (seconds SpringBoard waits, default 8), `XL_RAMDISK` (the small image,
default `userland/root.apfs`).

### Rules for running on the phone

* **Use the shortest `XL_SECS` that the test needs** (about 90 s for boot-only tests, 120 s when an app has to be opened).
  The phone heats up, and every run is a cold boot.
* **One working copy and one thread of work on the phone at a time.** A run holds a lock; `stop`/`reboot` from another copy
  are refused (`XL_FORCE=1` overrides).
* **Boot line length: about 480 characters of room.** The kernel's `cmdline:` line is cut at about 617 characters including
  the loader's own prefix (~120 characters), and the **last flags are lost silently** (`cam_s5k_gain=0x200` became
  `cam_s5k_`). Put the flags that matter first and check the end of the `cmdline:` line in the log if a flag seems ignored.
* **Thermal limits on every run**: `thermal=1 thermal_batt=45 touch_boost=600`.
* **Do not write raw blocks of `darwin` at or above block 2091520** (loader's menu log, menu args).

### A standard flag set

Everything is opt-in. This is the set the installed image uses (the numbers for `darwin_lba`, `darwin_blocks`,
`var_apfs` are **specific to the author's partition table**; get yours from `scripts/ufs-layout.sh`):

```
fb_phone=2796 syna_touch=1 cpufreq=1 bootcpu=7 cpus=8 fb_panel_hz=120 fb_hz=120 fb_threads=4
battmgr=1 wifi=1 wifi_ba=1 wifi_wide=1 wlan=13 wlan_driver=1 wlan_pause=0 wifi_station=1
wlan_fw=amss_lf.bin wlan_m3=m3_lf.bin wlan_board2=board2_lf.bin bt=4
ufs=2 ufs_write=1 ufs_bwrite=1 darwin_lba=<yours> darwin_blocks=<yours> var_apfs=<blocks>
haptic=4 haptic_mv=2000
```

Add per feature:

| Feature | Flags |
|---|---|
| Brightness, sensors | `bright=1 snsrpc=1 gyro=1 gyro_log=0` |
| Sound | `adsp_step=11 sound=4 pd_wait=1000` (early in the line, after `battmgr=1`); `sound_poll=1` is a debug I²C poll that causes stutter, keep off |
| Thermal | `thermal=1 thermal_batt=45 touch_boost=600` |
| GPU | `gpu=1` (kept on for the system), `sgpu_dcvs=1` for load-based speed, `gpu_recover=1` |
| Wallpaper editor | `poster_switcher=1` |
| Cameras | `cam=5` and friends, see [SUBSYSTEMS.md](SUBSYSTEMS.md#cameras) |
| Dynamic refresh off | `fb_dyn=0` (the default is 120 → 1 Hz idle) |
| Metal only for the plug-in process | `metal_all=0` (default: Metal goes to every process) |

## Installed boot with a menu

`scripts/build-install.sh` produces `build/out/shennong-install.img`: UEFI (RELEASE) + loader with its boot menu + kernel +
root.apfs for slot `boot_b`, command line with a one-year run limit.

```sh
fastboot boot build/out/shennong-install.img          # first: RAM only, Android's slot stays active
fastboot flash boot_b build/out/shennong-install.img
fastboot set_active b && fastboot reboot              # the menu at every start
fastboot set_active a                                 # back to Android
```

The menu waits 3 s and offers the last choice. From Android back to the menu:
`adb reboot bootloader` → `fastboot set_active b` → `fastboot reboot` → pick "USB (XL)". **Flashing `boot_b` and creating the
`darwin` partition change your phone; do this only if you understand the recovery path (stock images, `fastboot set_active a`).**

## usbfs (the phone's files on a Mac)

Needs the loader with the host pipe. During an `xl run`, run the Mac side (`src/usbfs/bridge`, Go) and the
phone's `usbfsd` (built into `root.apfs`) bridges the file system over the cable (NFS-style mount, about 1.6 MB/s write).
The loader with the pipe can be started without flashing it: `fastboot boot` it from Android, then `xl enter`.
