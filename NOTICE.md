# Notice

* **`port/`** changes and extends Apple's XNU and related code as shipped in upstream
  [hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel), which is under the **Apple Public Source License 2.0**
  (the text is in upstream's `LICENSE`; `apply.sh` fetches it). Changes to those files are under the same licence.
* Files with their own notice keep it: the UEFI platform files in `port/uefi/files/Platforms/Xiaomi/shennongPkg` (BSD-2-Clause-Patent, Microsoft, ARM, Linaro, Intel,
  Bingxing Wang), `src/apfs-rw/src/apfs_deflate.c` (libdeflate, MIT) and `apfs_zbitmap.c` (libzbitmap, MIT).
* Everything else is under the MIT licence in `LICENSE`.
* Open questions about code taken from Linux/Android sources are in [docs/LICENSES.md](docs/LICENSES.md).
* No Apple, Qualcomm or Xiaomi proprietary files are included.
