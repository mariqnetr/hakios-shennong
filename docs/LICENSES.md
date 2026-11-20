# Licences and third-party code

This is a map of what is under which licence, made by searching the repository for copyright, SPDX and licence notices
and for places where the code says it is taken from another project. It is **not legal advice** and it is
not exhaustive: a file with no notice is listed as "author's, no licence file yet". The author's own code is under MIT (`LICENSE`, scope in `NOTICE.md`), chosen on 2026-11-20.

| What | Licence | Basis | What to do |
|---|---|---|---|
| Files of `port/` that change XNU, BSD, IOKit, libkern files of upstream HackiOS | **APSL 2.0** | upstream's `LICENSE` is the Apple Public Source License 2.0; changes to APSL files are covered by it | keep under APSL 2.0 |
| New kernel files the port adds (`shennong_*.c`, `redwood_*.c` additions, `MiniIO*.cpp` changes, ...) | author's; no notice in the files. Built into the XNU kernel, so the kernel as a whole is distributed under APSL | no header in the files | decide: APSL, or a permissive licence for the file, stated in a header |
| `uefi/files/Platforms/Xiaomi/shennongPkg/*` (`DeviceBuild.py`, `.dsc`, `.fdf`) | **BSD-2-Clause-Patent** | headers say so: Microsoft, ARM Limited, Linaro, Intel, Bingxing Wang (derived from the Mu-Silicium `giulia` package) | keep those headers as they are |
| `src/apfs-rw/` (writable APFS) | author's, plus: `apfs_deflate.c` wraps **libdeflate** (**MIT**, Eric Biggers, Google LLC); `apfs_zbitmap.c` wraps **libzbitmap** from linux-apfs-rw (Corellium LLC, Ernesto A. Fernandez, "GPL-2.0+ OR MIT", used here **under MIT**) | notices in those two files | keep the notices; ship libdeflate's `COPYING` and libzbitmap's licence text wherever their sources are shipped |
| `tools/mk-ath12k.sh` and the `wifi/lx12` glue | glue is author's. At build time it downloads **Linux's ath12k** (**BSD-3-Clause-Clear**, one header **ISC**) and **FreeBSD 15.1**'s LinuxKPI (**BSD-2-Clause**) and patches the latter. Neither is stored in this repo | the script checks for those SPDX lines itself | patches to FreeBSD files are under FreeBSD's licence |
| Mesa 25.0.7 (changes in the userland GL scripts) | **MIT** (Mesa) | Mesa is downloaded at build time | keep Mesa's licence with any shipped binary |
| `src/usbfs/bridge/` (Go) | author's. Dependencies in `go.mod`: go-billy, gousb, go-nfs (Apache-2.0); google/uuid, golang.org/x/sys (BSD-3-Clause); golang-lru/v2 (MPL-2.0); go-xdr (ISC); go-nfs-client (no licence file found) | checked against the modules' LICENSE files on 2026-11-20, except `golang.org/x/sys` (BSD-3-Clause, Go authors) and **`willscott/go-nfs-client`, which has no LICENSE file on its default branch: look into this** | keep the licence texts if binaries are shipped |
| `src/sideload/` (`tsboot.c`, `sign.py`, `libSystem.tbd`) | author's. `libSystem.tbd` is a hand-written list of exported symbol names, not an SDK file | | TrollStore itself is **not** included; download it from its project and follow its licence |
| `scripts/`, `tools/`, `src/als`, `src/init-mg`, `src/battery`, `src/bluetooth`, `src/gpustress`, `src/ksu-module`, `docs/` | author's, no notice | | MIT (`LICENSE`) |
| Firmware, extracted UEFI drivers, device dumps | **proprietary** (Qualcomm, Xiaomi, Cirrus, others) | | **not in this repo; do not add** |
| Apple's iOS 16.5 | **proprietary** | downloaded from Apple's CDN to the user's machine by the scripts | not in this repo; do not add |

## Vendor data (left out)

`shennong_ovx9000.h` held the OmniVision OVX9000 register tables copied from the
phone's camera module file, plus the C-PHY tables from Android's `cam_csiphy_2_2_0_hwreg.h` (**GPL-2.0**);
`shennong_s5kjn1.h` held the Samsung S5KJN1 tables from the same kind of file. Sensor register init tables are the
sensor and module makers' data, not ours to relicense, and the PHY tables are GPL-2.0. Both files are **left out** of this repository; what has to be supplied from the user's own phone is in [CAMERA-TABLES.md](CAMERA-TABLES.md).

## Places that need a closer look before a public release

Register values and sequences were taken from Linux and Android kernel sources, which are **GPL-2.0**. Facts such as an
address or a magic number are not creative expression, but a copied table or a copied structure can be. The comments in the code name
these sources:

* the camera code cites Android's `cam_csiphy_2_2_0_hwreg.h` and says **GPL-2.0** (the C-PHY tables); it also cites `cam_vfe880.h`, `cam_ife_csid880.h`, `cam_vfe_lite78x.h`, `cam_csiphy_core.c`;
* clock tables and sequences: `gcc-sm8650.c`, `camcc-sm8650.c`, `dispcc-sm8550.c`, `sm8650.dtsi`, `qcom_geni_serial.c` (Linux);
* GPU: `a7xx_catalog.c`, `a6xx_gpu.c`, `a6xx_gmu.c`, `msm_gpu.c`, `dsi_host.c`, `msm_drm.h` (Linux msm);
* audio and PM: `q6apm.c`, `q6prm.c`, `spf_core.c`, `apr.c`, `lpass-*-macro.c`, `wcd939x.c`, `qcom-hv-haptics.c`,
  `qti_battery_charger.c`, `qcom_battmgr.c`, `qcom_scm*.c`, `qcom_q6v5_pas.c`, `pinctrl-sm8650.c`.

Mostly they are mentioned as "the same values as". For each one that was **copied rather than re-derived from the hardware or
from a capture**, either re-write it, or make the file GPL-2.0 (which is not compatible with putting it into XNU's APSL code
as one binary), or ask the author to confirm that the values came from captures. Until that is done, keep the repository private.

## Suggested next step

1. Review the list above file by file and record the outcome here.
2. Add SPDX headers to new files.
