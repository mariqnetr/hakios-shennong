## Firmware Infos

- **Device:** Xiaomi 14 Pro (shennong)
- **Region:** China
- **Version:** `OS3.0.308.0.WNBCNXM` (HyperOS 3, Android 16) / `BOOT.MXF.2.1-01919.2-LANAI-1`
- **Extracted from:** the phone's own `uefi_a` partition, with `tools/fvextract.py` + `tools/mkbinaries.py`
  (the layout and .inf files UEFIReader writes; .inf templates from giulia, FILE_GUIDs from this volume).

102 DXE modules. 98 of them are the same modules at the same paths as giulia (OnePlus 13R, same SoC,
`BOOT.MXF.2.1-01933`); Xiaomi-only: `MiToken`, `ProjectInfoDxe`, `ConfigWordsDxe`, `SPSSDxe`.

## Patches/Fixes

### ClockDxe (applied)

- **Reason:** with the stock driver the UEFI stops right after loading ClockDxe (seen on the phone, DEBUG
  build): it switches off clocks nothing has claimed, the display controller's among them.
- **Patch:** `0xA810`: `bl DCDDisableDependencies` (`96ffff97`) -> `mov w0, wzr` (`e0031f2a`), the same patch as
  giulia's; the code around it is byte-identical in both builds.
- **Patch Creator:** [Robotix](https://github.com/Robotix22) (giulia), carried over.

### SPMIDxe + PmicDxeLa (applied, as a pair)

- **Reason:** past ClockDxe the DEBUG UEFI stops with a synchronous external abort (EC 0x25, ISS 0x10); the
  stack holds SPMI arbiter registers (0x0C400000, 0x0C440018, 0x0C500010...), the same crash these patches cure
  on giulia.
- **SPMIDxe:** `0x154C` (the SPMI transaction function) -> `mov w0, wzr; ret`. `.text` is byte-identical to giulia's.
- **PmicDxeLa:** `0x1DAB4` (the wrapper that forwards to the SPMI protocol; giulia's `0x22B14`) -> `mov w0, wzr; ret`.
- **PmicDxeLa, dead-battery check:** with SPMI reads stubbed the battery reads as 0 mV, below
  `core_dbc_threshold` (3000), and the charger code (`0x143EC`: prints `core_dbc_threshold...`, then reads the
  battery ID through the ADC) never returns. `pm_pmicdxe_init` calls it once: `0x1694` `bl 0x143EC` ->
  `mov w0, wzr`. The UEFI does not charge either way. (giulia skips its own, OnePlus-specific check at `0x8AC8`.)
- **Patch Creators:** [Kancy Joe](https://github.com/sunflower2333), [Robotix22](https://github.com/Robotix22) (giulia), carried over.

### UFSDxe (applied)

- **Reason:** the DEBUG UEFI stops in UFSDxe with sense data `70 00 02 00 ... 04 02` (NOT READY, "initializing
  command required"): the bootloader leaves the UFS device asleep, and the driver only sends TEST UNIT READY to the
  UFS Device W-LUN (0xD0).
- **Patch:** `0xB008`: `bl 0xCA4C` (TEST UNIT READY) `; cbnz w0` -> `mov w1, #1; bl 0xCB78` (START STOP UNIT,
  power condition ACTIVE). giulia's patch at `0xA468`, moved to this build: same code, other offsets.
- **Patch Creator:** [Kancy Joe](https://github.com/sunflower2333) (giulia), carried over.

### Not applied yet

| Module | giulia's patch | On shennong |
|---|---|---|
| UsbConfigDxe | `0x561C`: `cbz` -> `b` (keep USB at ExitBootServices) | same code at `0x561C`; HackiOS does not exit boot services |
| ButtonsDxe, UsbMsdDxe | see giulia's README | other build: to be found by hand if needed |
