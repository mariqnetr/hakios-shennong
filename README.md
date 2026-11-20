# HackiOS on the Xiaomi 14 Pro (shennong, Snapdragon 8 Gen 3)

This repository is the work done to run **iOS 16.5 on a Xiaomi 14 Pro** (codename `shennong`, SM8650-AB,
Adreno 750, 1440×3200 LTPO panel). The kernel is Apple's open-source XNU (`xnu-8796.121.2`), the user space is the
**unmodified iOS 16.5** (launchd → backboardd → SpringBoard → apps), and everything in between — the UEFI port, the
loader, and drivers for the Qualcomm hardware — is written here. It builds on
[hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel); this repo holds the **files of the shennong port**
(laid out like upstream, to be copied over it), plus build scripts, tools and notes.

Nothing is flashed to run it: the phone is started with `fastboot boot`, the whole system lives in RAM, and a reboot
brings Android back. (An "installed" variant with a boot menu next to Android exists too, see
[docs/BUILD-AND-RUN.md](docs/BUILD-AND-RUN.md#installed-boot-with-a-menu).)

> **Status in one line:** it boots to the iOS home screen and is usable: touch, GPU-drawn UI at up to 120 Hz, Wi-Fi,
> Bluetooth, battery, haptics, sensors, speakers, flash storage, sideloading. Cameras work only partly, microphones
> are in progress, and the modem, GNSS, NFC and the fingerprint reader are not started.
> This is research code written by bringing hardware up one register at a time. Expect rough edges.

**Contents:** [What works](#what-works) · [What does not work](#what-does-not-work-yet) ·
[What needs doing](#what-needs-doing) · [Quick start](#quick-start) · [Repository layout](#repository-layout) ·
[What is not included](#what-is-not-included) · [Documentation](#documentation) · [Contributing](#contributing) · [Licence](#licence)

---

## What works

"Boot-arg" is a kernel argument that switches the driver on or tunes it (most
drivers are opt-in so a bring-up step cannot hang a boot that does not need it).

| Area | State | Notes |
|---|---|---|
| **Boot chain** | works | Mu-Silicium UEFI port → HackiOS loader → XNU. 8 cores (`bootcpu=7 cpus=8`), GICv3 redistributors, about 9 GB of RAM for XNU |
| **Display** | works | Command-mode DSI panel at **120 Hz**, tear-free frame sync, panel rate follows the screen and idles down to **1 Hz** (`fb_dyn=2` is the default), scalers/GPU fit an iPhone-sized frame onto the panel (`fb_phone`, `fb_gpu`, `fb_dpu_scale`) |
| **Brightness** | works | iOS slider drives the panel (`bright=1`), auto-brightness from the light sensor with glide |
| **Touch** | works | Synaptics TCM over SPI (`syna_touch=1`), multi-touch |
| **Buttons** | works | Power and volume |
| **CPU** | works | EPSS frequency control, per-load scaling (`cpufreq=1`) |
| **Thermal** | works, tuned by use | Own thermal service: CPU/GPU kept under 80 °C, battery budget (`thermal=1 thermal_batt=45 touch_boost=600`). Sensor mapping was verified only partly, see [docs/SUBSYSTEMS.md](docs/SUBSYSTEMS.md#thermal) |
| **GPU** | works | Adreno 750 brought up in 23 documented steps, kept on for the system to draw with (`gpu=1`): Mesa freedreno (OpenGL ES for CoreAnimation) and a Metal plug-in (`hakios-metal`). GPU DVFS 231–903 MHz (`sgpu_dcvs=1`), hang recovery (`gpu_recover=1`) |
| **Wi-Fi** | works | WCN7850 on PCIe: power, SMMU, MHI/BHI firmware load, QMI, then Linux **ath12k on a LinuxKPI compatibility layer**. 5 GHz preferred, `en0` present before launchd starts |
| **Bluetooth** | works | WCN7850 over UART, H4 bridged to a kernel control so iOS's `bluetoothd` runs; keyboard input via Bluetooth |
| **Battery and charging** | works | Battery data from the DSP's battery manager, iOS battery health, Xiaomi fast-charge (`battmgr=1`, `xm_fast=1`, `chg_max=1`) |
| **Haptics** | works | PM8550B vibrator with Android's and HyperOS's click waveforms for iOS taps (`haptic=4`) |
| **Sensors** | works | Accelerometer and gyroscope (`gyro=1`), ambient light, proximity — through the sensor DSP's FastRPC (`snsrpc=1`) |
| **Sound (output)** | works | **Both speakers**, full iOS volume: ADSP boot, AudioReach graph, TDM, CS35L43 amplifiers with Cirrus protection firmware, amps off after 3 s of silence (`adsp_step=11 sound=4 pd_wait=1000`) |
| **Flash storage** | works | UFS read and write; writable APFS (own implementation); `/private/var` lives on flash so installed apps and data survive reboots |
| **Sideloading** | works | TrollStore installs automatically; `.ipa` files install on the phone ([docs/sideload.md](docs/sideload.md)) |
| **Files over USB** | works | The phone's file system mounted on a Mac over the USB cable (`src/usbfs`) |
| **Flashlight** | works | PM8550 flash LED |
| **Boot menu** | works | Installed boot with a menu, Android beside it, last choice remembered |
| **GPU stress app** | works | `src/gpustress` (CoreAnimation layers + blurs, fps overlay), installed through TrollStore |

## What does not work (yet)

| Area | State | What is known |
|---|---|---|
| **Cameras** | **partial** | The wide sensor streams: first photo through CSIPHY 3 (C-PHY) → CSID → IFE → memory, delivered to iOS's Camera app (`/dev/redwood-camera`). Mode 9 (1920×1080) is the default and works. **Mode 1 (4096×3072 RAW10 @ 30) comes up dark or corrupt in some starts** (root cause not found). The ultra-wide and telephoto (S5KJN1) deliver frames but are still being cropped, padded and colour-tuned. **Front camera, video recording, and a real ISP pipeline (colour, noise, 3A) are not done**; autofocus is the lens-motor driver only |
| **Microphones** | in progress | `mic=1` (read-only look at the path) and `mic=2` (codec on SoundWire, chip ID read) exist. No capture yet |
| **Cellular modem / SIM** | not started | Next after the DSPs below |
| **cDSP** | not started | Roadmap order: microphones → cDSP → modem |
| **GNSS, NFC, UWB, fingerprint reader** | not started | |
| **Hardware video codecs** | not started | |
| **Wired audio / call audio paths** | not started | only the speaker path is done |
| **USB host / OTG, DisplayPort** | not started | the USB-C port is used for the loader's fastboot link and `usbfs` only |
| **Suspend / deep idle, charging while the system is "off"** | not covered | nothing handles them yet; behaviour is unknown |
| **Wi-Fi upload speed** | limited | around 12 Mbit/s up when last investigated; cause not found |
| **Long-lived installed use** | not hardened | the usual way to run is a time-limited RAM session; the installed boot exists but is lightly used |
| **Naming** | cosmetic debt | the userland still carries `redwood` names (`/redwood/bin`, `RedwoodMetal.bundle`, `redwood_*.c`), inherited from the POCO X5 Pro port this one grew from; see `docs/TODO.md` |

## What needs doing

Roughly in the order that unblocks the most:

1. **Microphones** — finish the SoundWire codec bring-up and route capture through the same AudioReach graph code as playback.
2. **Camera quality** — find why mode 1 comes up dark (candidates: sensor tables, MIPI rate, IFE line mode), add a real ISP step (or CPU debayer with proper tuning from the camtuning files, `tools/camtuning36.py`), bring up the front camera, video.
3. **cDSP, then the modem** — the remote-processor boot code from the ADSP work is the template; modem needs QMI/RIL-level glue on the iOS side that does not exist.
4. **Rename the port off `redwood`** (user-facing names to `shennong`/`hakios`), one `system.apfs` rebuild.
5. **Wi-Fi upload throughput** and power saving.
6. **Make the installed boot a daily-driver**: suspend/resume, charging UI when off, a way to update without a PC.
7. **Verification on more phones**: only one Xiaomi 14 Pro (HyperOS `OS3.0.308.0.WNBCNXM`) was used. Other SM8650 phones should be close (the UEFI base is the OnePlus 13R's, same SoC), but nothing here was tested on them.

More small items are in [docs/TODO.md](docs/TODO.md) and in the "known issues" of each area in
[docs/SUBSYSTEMS.md](docs/SUBSYSTEMS.md).

---

## Quick start

You need a **Xiaomi 14 Pro with an unlocked bootloader**, a Mac or Linux machine, a USB-C cable, Podman or Docker, and
about 40 GB free. Full steps, flags and caveats: [docs/BUILD-AND-RUN.md](docs/BUILD-AND-RUN.md). In short:

```sh
# 1. the tree: upstream HackiOS with the port's files copied over it
./apply.sh hackios

# 2. get what is not in this repo off YOUR phone (see "What is not included"):
#    the phone's UEFI drivers (tools/fvextract.py + tools/mkbinaries.py inside that tree), its device tree (/sys/firmware/fdt),
#    firmware from the vendor partitions

# 3. build (in containers): UEFI, kernel + loader, iOS images (iOS 16.5 comes from Apple's CDN to your disk)
bash scripts/build-uefi.sh
bash scripts/build-kernel.sh
bash scripts/build-ios.sh        # or scripts/build-gl.sh for the Mesa/Metal userland

# 4. boot it
./xl enter                       # phone into the resident loader (fastboot protocol over USB)
./xl ramdisk flash               # (once) the big system image
XL_SECS=120 XL_CMDLINE="-v bootcpu=7 cpus=8 cpufreq=1 syna_touch=1 fb_phone=2796 ..." ./xl run
```

The scripts were written for the author's layout (`~/tmp/shennong`, with `port/` next to the scripts) and expect to be run
from the repository root; they have been adapted to this layout but **not re-run since**. Read the header of each script
first. Treat them as documentation that happens to run.

## Repository layout

| Path | What |
|---|---|
| `port/` | The port itself: every file it adds or changes, in the final state, laid out like upstream `hackios-xnu-kernel` (`xnu/files/bsd/dev/shennong_*.c`, `xnuloader/`, `uefi/files/Platforms/Xiaomi/shennongPkg`, `wifi/lx12`, `tools/usbfs`, ...). Based on upstream commit `d82c64a3` |
| `apply.sh` | Clones upstream, checks out the base commit, copies `port/` over it |
| `docs/` | [ARCHITECTURE](docs/ARCHITECTURE.md), [BUILD-AND-RUN](docs/BUILD-AND-RUN.md), [SUBSYSTEMS](docs/SUBSYSTEMS.md), [DEBUGGING](docs/DEBUGGING.md), plus the original bring-up notes (`TODO.md`, `sideload.md`, `apfs-rw.md`, `xm-map.txt`) |
| `scripts/` | Container build scripts (UEFI, kernel, iOS images, Mesa/Metal userland, install image) and helpers that read data off the phone |
| `src/apfs-rw/` | Writable APFS: the author's newer working copy with host-side tests. The kernel's copy is in `port/xnu/files/bsd/miscfs/apfs` and may lag behind it |
| `src/init-mg/`, `src/als/`, `src/battery/`, `src/bluetooth/`, `src/sideload/`, `src/gpustress/`, `src/ksu-module/` | Early init, light/motion helpers, battery-health notifier, Bluetooth glue, TrollStore boot helper, GPU stress app, KernelSU module that collects data on the phone |
| `tools/` | GPU report, sparse-image maker, camera tuning decoder, amplifier firmware extractor, thermal-policy unit test |

## What is not included

No Apple files and no vendor binaries.

* **Qualcomm/Xiaomi UEFI drivers** extracted from the phone's `uefi_a` partition, and the byte-patched copies of some
  of them. What the byte patches do is described in `port/uefi/files/Binaries/shennong/README.md` and in
  [docs/SUBSYSTEMS.md](docs/SUBSYSTEMS.md#uefi-and-early-boot). Re-extract them from your own phone.
* The **camera sensors' register tables** (`shennong_ovx9000.h`, `shennong_s5kjn1.h`) and the C-PHY settings, taken from the
  phone's camera module files: [docs/CAMERA-TABLES.md](docs/CAMERA-TABLES.md) says what to provide. The camera driver does not
  build without them.
* The phone's **device tree** (`shennong.dtb`): take it from `/sys/firmware/fdt` on your own phone.
* **Firmware** (GPU, ADSP, audio, camera tuning, modem, Wi-Fi/Bluetooth) and partition dumps: pull them from your own
  phone with `scripts/get-*.sh`.
* **iOS itself**: the scripts download the iOS 16.5 IPSW parts from Apple's CDN to your machine.
* **TrollStore** binaries: download them from the TrollStore project.
* Built kernels, images, logs and traces.

## Documentation

* [ARCHITECTURE](docs/ARCHITECTURE.md) — boot chain, memory map, how drivers are structured, how iOS user space is made to run
* [BUILD-AND-RUN](docs/BUILD-AND-RUN.md) — building, `xl`, the boot-arg conventions, safe-run rules, installed boot
* [SUBSYSTEMS](docs/SUBSYSTEMS.md) — per subsystem: how it works, flags, known issues, gotchas
* [DEBUGGING](docs/DEBUGGING.md) — logs, dumps, the bring-up method that was used, pitfalls

## Contributing

Issues and pull requests are welcome. If you change something:

* keep each change small and self-explanatory, one subject per commit;
* keep new drivers opt-in behind a boot-arg until they are proven;
* say in the commit message what you measured on the phone, not only what you changed;
* do not commit firmware, extracted vendor binaries, logs or dumps that identify a phone (serial numbers, MAC addresses).

## Licence

Original code and docs in `src/`, `scripts/`, `tools/` and `docs/` are under the [MIT licence](LICENSE). The kernel changes
modify XNU and are covered by the [Apple Public Source License 2.0](https://opensource.apple.com/apsl/), like upstream; files
with their own notices keep them ([NOTICE.md](NOTICE.md)). Third-party code and open licence questions are mapped in
[docs/LICENSES.md](docs/LICENSES.md); read it before making this repository public. This is an independent research
project, not affiliated with or endorsed by Apple Inc., Xiaomi or Qualcomm. iOS, iPhone and Apple are trademarks of Apple Inc.
