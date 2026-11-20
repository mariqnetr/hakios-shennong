# Subsystems

One section per block of hardware: what the driver does, the boot-args that control it, the state, and what bit people
while writing it. "Known issue" means observed, not guessed.

Contents: [UEFI and early boot](#uefi-and-early-boot) · [CPU and thermal](#cpu-and-thermal) · [GPU](#gpu) ·
[Display and brightness](#display-and-brightness) · [Touch and buttons](#touch-and-buttons) ·
[Storage and APFS](#storage-and-apfs) · [Wi-Fi](#wi-fi) · [Bluetooth](#bluetooth) · [Sound](#sound) ·
[Microphones](#microphones) · [Cameras](#cameras) · [Battery and charging](#battery-and-charging) ·
[Haptics and torch](#haptics-and-torch) · [Sensors](#sensors) · [usbfs](#usbfs) · [Sideloading](#sideloading) ·
[Loader and boot menu](#loader-and-boot-menu)

---

## UEFI and early boot

* Base: Mu-Silicium with `shennongPkg`, derived from `giulia` (OnePlus 13R, same SM8650). The UEFI image sits at
  `FD_BASE 0xA7100000` (3 MB); the memory map was checked against the phone's own device tree.
* The phone's own DXE drivers are used (102 of them plus 74 raw files from `uefi_a`), with **four byte-patches**:
  * `ClockDxe` — the display keeps its clocks (otherwise it goes dark when UEFI takes over);
  * `SPMIDxe` and `PmicDxeLa` — an SPMI external abort on first access; `PmicDxe` also skips the dead-battery
    charger check;
  * `UFSDxe` — wakes the UFS device with START STOP UNIT (the stock driver only tests it and stops on NOT READY).
  The offsets are in `uefi/files/Binaries/shennong/README.md`. They are specific to firmware `BOOT.MXF.2.1-01919.2-LANAI-1`; other
  firmware needs the same byte patches found again.
* GICv3: the redistributors are `0x40000` apart on this SoC, not `0x20000`; without it only one core wakes.
* Memory: XNU takes the largest free DRAM block (about 9 GB), not just the first 6 GB.
* The debug UEFI scrolls its console instead of clearing it; `MODE=DEBUG` prints its log on the screen.

## CPU and thermal

* **cpufreq**: SM8650 EPSS, four groups (little, A720 ×2 groups, X4), requested by writing the EPSS rows; `cpufreq=1`.
  `bootcpu=7` runs the kernel on a big core (the firmware starts on a small one); `cpus=8` brings up all cores.
* **Thermal service** (`shennong_thermal.c`): every 20 ms CPU frequency follows load (like schedutil: 1.25 × the
  busiest core's load; straight to the allowed top at 85 %, down after 60 ms); every 100 ms the 41 tsens sensors limit it
  (`thermal_cpu=80`, `thermal_gpu=80` °C; cap pulled to the running row + 1, then down a step, two at +5 °C, back up after 1 s
  at −5 °C); every 10 s a **battery budget** 0–10 against `thermal_batt` (each step takes 7 % off every group's top and one GPU
  speed); `thermal_crit=105` drops everything to its lowest. A touch boost raises the allowed top while a finger is down and
  for `touch_boost` ms after.
  Flags: `thermal=1` (0: off, 2: watch only), `thermal_batt=45`, `touch_boost=600`, `thermal_log=10`, `cpu_dvfs=0`
  (old fixed top frequency). Sysctls: `kern.redwood.thermal`, `thermal_pressure`, `thermal_hottest`, `thermal_throttles`,
  `thermal_running`; the existing `thermald` posts the level to iOS.
* A host-side unit test of the policy: `tools/thermal-test/thermal-test.c` (11 scenarios).
* **Known issues / unverified**: the sensor-to-core mapping ("cpu" in the thermal line is tsens0 5–10 = the A720 cores 2–4; the X4 is
  tsens1 1–3) and EPSS row writes at runtime were only partly confirmed on hardware when the service was handed over. If UI
  smoothness suffers, try `cpu_dvfs=0`.
* The loader idles all CPUs at their lowest speed while it waits in `xl` mode (`fastboot oem cpu check|read|low|back`).
* The phone gets **hot at 903 MHz GPU**; DVFS (below) and the thermal service exist because of it.

## GPU

**Bring-up in 23 steps** (`sgpu=N` runs to step N; `shennong_adreno.c`): 1–3 clocks and reset, 4–5 CX power and the GMU's
clocks, 6 first words from the GPU block, 7–10 TrustZone's GPU init, the SMMU, the GMU's memory, 11–15 GMU firmware,
RPMh channel, start, HFI, 16–17 first messages to the GMU, the 3D core powered, 18–21 ring + microcode + the **zap shader**
+ first commands, **22 a picture filled by the 2D engine on the panel**, **23 a triangle from the 3D pipeline with our own
shaders**. `gpu=1` then keeps the GPU on for the system.

* iOS side: **Mesa 25.0.7 freedreno** provides OpenGL ES to CoreAnimation, with our changes; the kernel's `RedwoodGPU`
  (IOAccelerator) takes submissions; the **Metal plug-in** `hakios-metal` is named by `IOAcceleratorES` under `metal_plugin=1`.
* Frequency: `sgpu_speed=10..12` → 770/834/903 MHz; `sgpu_dcvs=1` chooses by load every 50 ms from 231–903 MHz, with
  the thermal cap.
* Recovery: `gpu_recover=1` revives the 3D core if it stops answering.
* Memory bandwidth vote: `ddr_raise`.
* **Flushing**: submissions that write a surface wait until the panel has it; the display forgets a surface's
  cached pixels once the GPU has drawn them; Mesa's commands are read through a word cache to keep submit cost down.
* Diagnostics: a hung submission is reported with its command buffers and the BV pipe, draw state and events;
  `gpu_nop`, `gpu_chicken_skip` and `sgpu_counters=1` helped find what hung the first frames; the
  5-second line shows GPU speed, temperatures, counters and who holds the GPU lock and how long.
* **History worth knowing**: the first Mesa frames hung the pipeline. The trials (chicken bits, turnip's
  register values, cache-clean events, per-command bisection, both threads like Linux's `a7xx_submit`) narrowed it; the
  fixes ended up in the Mesa changes and in a barrier before every draw that reads the framebuffer).
* **Known issues**: 903 MHz is hot (use DVFS and the thermal service); a GPU hang is reported in the log with its command
  buffers, which is the first place to look. How much of the Metal API the plug-in covers was not measured.

## Display and brightness

* The panel is **command mode**, 1440×3200, up to 120 Hz. Frames go into the panel's memory **behind its refresh**
  (`fb_sync`, on by default) so that there is no tearing: the rows are copied in pieces of 64 by whichever copier is free,
  right behind the scan-out, by real-time threads (`fb_threads=4`, `fb_rt`).
* `fb_panel_hz=120` sends the panel the timing commands from the device tree, from the splash region's last page between
  two frames, as the firmware's own controller does. `fb_hz=120` gives CoreAnimation a 120 Hz beat; `fb_vsync_panel`
  follows the panel's own frames.
* **Dynamic refresh** `fb_dyn`: 1 = rate follows the screen (120 on a frame or a touch, 60, 30); **2 (default) leaves the
  panel on its own 60-to-1 Hz timing with autorefresh stopped when idle**; `0` disables. Panel commands go under
  `shennong_dsi_lock` because the backlight sends through the same registers.
* **iPhone-shaped output**: `fb_phone=2796` (iPhone 14 Pro Max screen), other `fb_phone` values for the 14 Pro/14 Plus, `fbscale=2` (860×1864 at 2×). The frame is scaled to the
  panel by the display's own scalers (`fb_dpu_scale=1`) or by the GPU (`fb_gpu=1`), with two framebuffers (`fb_flip=1`).
  Changing the screen class changes MobileGestalt's gestures, see [ARCHITECTURE.md](ARCHITECTURE.md#ios-user-space).
* **Brightness** (`bright=1`, `ShennongBacklight`): iOS's slider → DCS 0x51 to the panel. **Auto-brightness**
  from the front light sensor (`bright_auto=1`, default; `als_from=3`), glides at 20 steps/s; the slider follows the glide,
  and iOS's own fades and echoes are not taken for a hand on the slider. It reads the screen above the sensor
  pixel by pixel (no mapping) to cancel the screen's own light.
* Probes: `fb_probe=<mask>` reads DSI/pipe/control state; `fb_panel_test=S` tries each timing
  and measures it.
* **Known issues**: the wallpaper editor needs `poster_switcher=1`.

## Touch and buttons

* Touch: Synaptics S3910P (TouchComm v1) over SPI (`spi@a90000`, 15 MHz), IRQ GPIO 162, reset GPIO 161; `syna_touch=1`. Coordinates are 0…143999 × 0…319999 (super-resolution factor 100). A gap between SPI transfers, predictive reads
  and report flood control were needed. `touch_script=S` replays taps (opens dock apps, pages, App Library) as a stress test.
* Buttons: power and volume− are the PMK8550 PON (SPMI arbiter v7), volume+ is a `gpio-keys` GPIO. A Bluetooth keyboard's keys go through `/dev/redwood-touch`.

## Storage and APFS

* UFS: own host controller driver (`ufs=2`), read and write (`ufs_write=1`, `ufs_bwrite=1` for the darwin block device).
* **Writable APFS** (`src/apfs-rw`, [apfs-rw.md](apfs-rw.md)): a from-scratch implementation: B-trees, object map, space manager,
  transactions, hard links, xattrs, ACLs, document ids, extents with holes, flush barriers and F_FULLFSYNC. It is
  what `/private/var` runs on. Pieces were driven by what iOS daemons ask (FileProvider fsctls, containermanagerd names).
  `apfs_watch=1` logs every failing operation for bring-up.
* **Known issues**: write speed is limited (device-write batching and commit batching are the tuning); crash
  consistency relies on the transaction code, which has no crash-injection testing here; do not trust it with irreplaceable data.
* The `darwin` partition and its reserved blocks: see [ARCHITECTURE.md](ARCHITECTURE.md#storage).

## Wi-Fi

Boot-args: `wlan=N` (steps), `wlan_link=1`, `wlan_driver=1`, `wifi=1`, `wifi_station=1`, `wlan_fw/m3/board2`.

* Chip: **WCN7850** (Wi-Fi 7) on PCIe0 (`0x60300000`). Steps 1–6 power and PCIe link; 7–9 SMMU context and the radio's
  SBL through BHI; 10–11 MHI host and the full image through BHIe; 12 IPCR channels and QRTR HELLO; 13–14 QMI to WLFW
  (host cap, radio memory, target cap, regdb, board data, M3).
* Then **Linux ath12k** runs on a LinuxKPI layer (`wlan_driver=1`); `wifi/lx12` supplies what LinuxKPI lacks. Frames go as
  native 802.11 (the firmware takes no Ethernet), `en0` is created before the system starts so that `configd` has a network
  service, 5 GHz is preferred by 15 dB when heard at −75 dBm or better, WMM frames go as QoS data so the
  firmware can aggregate.
* Firmware: use the phone's `amss`, `m3`, `board-2` (the `_lf` names), or Linux's own with `wlan_m3=NAME` / `wlan_board2=NAME`.
* If the firmware stops, the radio's RDDM dump is fetched and readable runs of text printed.
* **Known issues**: upload was about 12 Mbit/s when investigated (the driver prints per-frame timings to find where the time goes);
  `ratelimit` on ath12k's debug – its debug lines can flood the log; the pool is 72 MB.
* Gotcha: `net_trace=60` adds many log lines and uses boot-line room (see the 480-character rule).

## Bluetooth

Boot-args: `bt=1..4`, `bt_trace_le=N`.

* WCN7850's Bluetooth over UART, firmware from the `bluetooth_a` partition (a FAT volume, read by our code).
* iOS's `bluetoothd` expects a POSIX H4 driver: we expose `/dev/cu.bluetooth` backed by a **kernel control** and the
  registry's `bluetooth` entry. `BlueTool` is injected and several vendor/Olympic commands the chip does not have are
  **answered here** with the two bytes the parser wants. LE random address before the first scan.
* Starts after Wi-Fi's bring-up.
* **Known issues**: only what `bluetoothd` asks was made to work (scanning, a keyboard); audio profiles and a thorough
  pairing matrix were not tested.

## Sound

Boot-args: `adsp_step=11 sound=4 pd_wait=1000` (early, after
`battmgr=1`), `amp_fw=1`.

* The audio DSP (**ADSP**) is started by our code: segments of `adsp.mdt` (up to 64; ours has 55), the DSP device tree
  image through PAS 0x24, RPMh version 3, TrustZone's `interrupted` answered for up to a second, GLINK intents.
* Control path: **GPR/APM** packets (the APM asked on `adsp_apps` whether the audio framework is up), then an
  **AudioReach graph** built as Android's speaker graph is (containers, TDM sink: 4 slots, long sync, 6.144 MHz,
  PRM `0x08001a6e=1` as the HAL does).
* The audio **protection domain** needs FastRPC services and a heap below 4 GB: `shennong_audiorpc.c` serves the
  files as `audioadsprpcd` does; the heap sits in firmware-free memory; TrustZone's memory hand-over
  is tried one page first, with an SHM bridge fallback.
* Speakers: **two CS35L43 amplifiers** on a bus whose pins are eGPIOs claimed for APPS (bit 12). `sound=3` plays a tone;
  `sound=4` plays what iOS sends to the speakers until the run ends, powers the amps only while there is sound (off after 3 s
  of silence), and loads **Cirrus protection firmware** into the amplifiers' DSP (`amp_fw=1`, `tools/amp-fw.py`).
  A cushion of three periods before iOS's sound is taken removed the stutter.
* Debug: `rec_sound=N` records the first N seconds iOS played (`xl sound`), `rec_sound_fed=1` records what went to the DSP;
  `sound_poll=1` is an I²C poll that **causes stutter** — keep it off.
* **Known issues**: no microphones, no headphones/earpiece/call routes; `sound=4` is a fixed speaker path, not a full
  audio HAL (volume ranges come from iOS only).
* **The out-of-band map**: graph commands too large for GLINK go through a shared-memory map the DSP reads
  through its SMMU stream; getting its mapping accepted took many tries recorded in the comments of the code.

## Microphones

`mic=1` read-only look at the path; `mic=2` the codec on SoundWire, chip ID read.
No capture yet. Roadmap: SoundWire master bring-up → codec routing → AudioReach capture graph → iOS `AudioQueue` input.

## Cameras

Boot-args `cam=1..5`, plus many `cam_*` tuning flags. Files `shennong_camera.c`, sensor tables
`shennong_ovx9000.h`, `shennong_s5kjn1.h`.

* **Register tables are not in this repo**: see [CAMERA-TABLES.md](CAMERA-TABLES.md); the camera driver does not build without them.
* Sensors: wide **OVX9000**, ultra-wide and telephoto **Samsung S5KJN1** (`cam_sensor=ultra|tele`), front OV32B40.
  Tuning files and module data come from the phone's `vendor` (not in this repo).
* Steps: `cam=1..2` power the four sensors and read chip IDs; `cam=3` clocks, voltage votes for MMCX and camera buses
  (cmd-db names); `cam=4` the **TPG** colour bars through CSID-lite 0 and IFE-lite 0 into memory;
  `cam=5` the wide sensor's real frames through **CSIPHY 3 (C-PHY)** → CSID → IFE and then to iOS
  through `/dev/redwood-camera`, a ring of three buffers, exposure from iOS's media library.
* What bit people: the camera NoC's real-time ports and Q-channel, the CSID RX in EPD mode, VC 0 vs VC 1
  (PDAF), lines of `cam_width` pixels, IFE root on PLL3 on a warm start, the frame length and gain limits
  of mode 1, and the lens motor (AK7316 at I²C 0x0c, with a walk across its travel at start).
* Modes: **mode 9 (1920×1080) is the default and works**; **mode 1 (4096×3072 RAW10, 30 fps, `cam_mode=1`) often comes up
  dark** or with broken columns (table read-back `cam_verify`, `cam_sweep`, `cam_wr1..8` to try registers
  one at a time, `cam_keep_raw=N`). Bayer order RGGB by default (`cam_bayer`).
* S5KJN1: GBRG, per-channel digital gains (`cam_dg_r/g/b`), lines padded to 16 bytes (4080 RAW10 = 5104), write master in
  line mode, `cam_crop`, `cam_crop_x0/y0`, D-PHY not programmed again while streaming.
* Getting pictures out: `xl camera` fetches frames the run left (they are kept in the last 64 KB of the frame memory).
* **Not done**: front camera streaming, video, an ISP (the frames are raw Bayer; colour processing is minimal), 3A, flash sync.

## Battery and charging

`RedwoodBattery.cpp`. `battmgr=1` (charge from the DSP's battery manager), `3` (battery and charger on the
screen), `4` (Xiaomi's properties 0..99 on screen). Xiaomi reports the current with an inverted sign; "charging while losing
charge" is shown as not charging. `serial` for the power source lets `powerd` compute **battery health** with a
weighted resistance. Xiaomi fast charge: `xm_fast=1` with our own **42 °C guard**; `chg_max=1` is the
fastest charge **without Xiaomi's check**. `docs/xm-map.txt` is the map of Xiaomi's property numbers.

## Haptics and torch

* Vibration motor: PM8550B haptics module (`haptic=1..4`, `haptic_mv=` drive voltage in mV). iOS's taps map to Android's click
  or HyperOS's own taps.
* Flashlight: PM8550 flash LED (`torch=N`).

## Sensors

The DSP's **sensor framework** is reached over FastRPC with a
listener we implement (`snsrpc=1`, `shennong_snsrpc`: constant-handle listener first, the default listener's handle has its
top bit set). Streams: accelerometer and gyro (`gyro=1`, `gyro_log=0`), ambient light (`kern.redwood.als_mlux`,
streamed at `als_hz` Hz, Xiaomi's light data types, front light sensor with the screen's own light subtracted),
proximity (`kern.redwood.prox`, from the raw reflection `proximity_cal_strm`). `src/als` has host-side helpers.

## usbfs

The loader (0.6+) exposes a host pipe (`oem pipe-w/pipe-r`), the kernel exposes `/dev/hostpipe`, `usbfsd`
(in `root.apfs`) serves the phone's files and the Mac's Go bridge (`src/usbfs/bridge`) mounts them (about 1.6 MB/s write).
Use it to pull screenshots, logs and photos or push files during a run. The loader must be the new one: boot it
temporarily with `fastboot boot` from Android then `xl enter`.

## Sideloading

`src/sideload`, [sideload.md](sideload.md). The kernel has **no code-signing enforcement** (AMFI stub), so TrollStore's CoreTrust
bypass is moot; the only thing that broke it was `posix_spawn` into a persona that did not exist. `tsboot` runs first at
boot, creates **persona 99**, installs TrollStore once into `/var/containers` and re-registers its apps on later boots.

## Loader and boot menu

The loader is an EFI application with a fastboot server over USB; see
[BUILD-AND-RUN.md](BUILD-AND-RUN.md). It reads the resident image from `darwin` (`xl ramdisk flash`), stores menu kernel
arguments there (`xl menu-args`), shows a boot menu for an installed phone (3 s timeout, last choice remembered), idles the
CPUs at their lowest speed while it waits, and logs to a reserved block range.
