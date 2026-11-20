# TODO (shennong)

- Everything made for this phone goes into a folder of its own, named for shennong, not redwood (user's wish, 9 November 2026).
  Today the userland inherits redwood's names: /redwood/bin, /redwood/lib, /redwood/share, and the Metal plug-in
  ships as RedwoodMetal.bundle (with hakios-metal's HakiosMetalDevice inside) only because
  /System/Library/Extensions/RedwoodMetal.bundle is the link system.apfs already has. To do: a shennong
  folder (and HakiosMetal.bundle / HakiosMetalDevice for the plug-in),
  one system.apfs rebuild (SYSTEM=1 bash build-gl.sh) and a re-upload.

## После стабильных 120 fps (просьба пользователя, 2026-11-13)
- Динамическая частота GPU (DCVS): по загрузке GPU выбирать уровень из gpu_speeds (231..903 МГц) через GMU, с потолком по температуре. Сейчас частота фиксированная (sgpu_speed), на 903 МГц телефон греется.
- Динамическая развёртка панели 1–120 Гц: панель командного режима с autorefresh; снижать частоту, когда кадры не меняются (команды смены режима есть в dt: timing-switch-command), вплоть до 1 Гц в простое.
- GPU Stress app (gpustress/: CA layers + blurs, 4 levels, fps HUD; build.sh -> build/trollstore/GPUStress.ipa; tsboot installs it via trollstorehelper install ... force at every boot). Next: show temperatures/GPU MHz in the app (needs a sysctl from the kernel).
