# Порт HackiOS на Xiaomi 14 Pro (shennong)

Черновик порта по `docs/PORTING.md`. Snapdragon 8 Gen 3 (SM8650-AB), Adreno 750, 1440 × 3200.
На телефоне ещё ни разу не запускался. Все данные сняты с самого телефона: HyperOS `OS3.0.308.0.WNBCNXM`
(Android 16), root через KernelSU, `tools/shennong-collect.sh`.

## Что сделано

| Файл | Что |
|---|---|
| `common/devices/shennong.conf` | профиль устройства |
| `common/devices/shennong.platform.c`, `xnuloader/loader.c` | `platform_shennong` в загрузчике |
| `xnuloader/loader.c`, `xnuloader/build.sh`, `common/build.sh` | `PAYLOAD_BASE` берётся из профиля при сборке (раньше был зашит адрес redwood) |
| `mkxnuimg.py` | не подсовывает prebuilt-UEFI redwood другому телефону; версия заголовка boot.img из `Resources/Configs/<target>.toml` |
| `uefi/files/Platforms/Xiaomi/shennongPkg` | пакет Mu-Silicium на основе `giulia` (OnePlus 13R, тот же SM8650), карта памяти сверена с телефоном |
| `uefi/files/Binaries/shennong` | 102 DXE-драйвера и 74 RAW-файла из раздела `uefi_a` телефона, раскладка как у UEFIReader |
| `uefi/files/Resources/DTBs/shennong.dtb` | device tree телефона (`/sys/firmware/fdt`, без серийных номеров) |
| `uefi/files/Resources/Configs/shennong.toml` | адрес/размер FD, boot header v4 |
| `tools/shennong-collect.sh` | снять данные с телефона по adb (с root — ещё dtb и разделы прошивки) |
| `tools/fdtdump.py` | dtb → текст без `dtc`; годится как `dtc` для `common/new-device.sh` |
| `tools/fvextract.py`, `tools/mkbinaries.py` | замена UEFIReader без .NET: FV → драйверы → `Binaries/<device>` + списки DXE/APRIORI/RAW |

## Значения, сверенные с телефоном

| Что | Значение | Откуда |
|---|---|---|
| GIC | `0x17100000` / `0x17180000` | device tree, `/proc/iomem`; `new-device.sh` на этом dtb даёт то же |
| ядра | 8, `core_shift` 8, `small_cores = 0x3` | dtb (`capacity-dmips-mhz` 0x400 ×2, 0x700 ×5, 0x766), cpuinfo (`0xd80/d81/d82`) |
| фреймбуфер | **`0xFC400000`**, 43 МБ | `splash_region` («cont_splash_region») в dtb. У OnePlus он на `0xD5100000` — у Xiaomi другой |
| `FD_BASE` | `0xA7100000`, 3 МБ | как у giulia; в Android там `ramoops` — под UEFI не важно |
| `PAYLOAD_BASE` | `0xC7800000`, 64 МБ | свободная RAM: Android держит `0xCB800000–0xD7FFFFFF`. Шим берёт только биты 16–31 адреса — ниже 4 ГБ, кратно 64 КБ |
| SMEM, AOP, PIL, TZ | как у giulia | совпадают с reserved-memory dtb |

## Железо

* **Загрузчик разблокирован** (`device_state=unlocked`).
* **Тач — Synaptics TCM** (`synaptics,tcm-spi`, TouchComm v1): SPI `spi@a90000` (QUP, 15 МГц),
  IRQ — GPIO 162, сброс — GPIO 161, 1440 × 3200 с `super-resolution-factor` 100
  (оси 0…143999 × 0…319999), 10 касаний. В ядре Xiaomi `shennong-u-oss` драйвера нет.
* **Вибро**: не отдельный чип (как AW86927 у redwood), а модуль haptics в PM8550B (`qcom,hv-haptics@f000`, SPMI SID 7); boost ревизии 0.1 включается через SDAM PMK8550 `0x9d00` (PBS), поздние — сами.
* **Кнопки**: питание и громкость− — PMK8550 PON (`pon_hlos@1300`, SPMI), громкость+ — `gpio-keys` →
  GPIO6 PM8550 (SID 1, `0x8d00`). Те же периферии и адреса SPMI, что у redwood (PMK8350/PM7325), поэтому
  драйвер общий: `bsd/dev/redwood_buttons.c` выбирает раскладку арбитра по имени платформы от загрузчика
  (`/arm-io device_type`). У SM8650 арбитр версии 7: core `0xC400000` (таблица каналов с `0x2000`),
  cnfg шины 0 `0xC42D000`, observer `0xC440000` (0x20 на канал). Драйвер только читает.
* **Wi-Fi**: на PCIe (`0x60300000`, `qcom,pcie@1c00000`), не WCN6750.
* **Прошивка UEFI**: `BOOT.MXF.2.1-01919.2-LANAI-1` (у giulia `01933`).

## Где порт сейчас (проверено на телефоне)

| Рубеж | Статус |
|---|---|
| UEFI Mu-Silicium: логотип, UFS, разделы, BDS | ✅ (патчи драйверов: `Binaries/shennong/README.md`) |
| загрузчик HackiOS: `platform shennong`, `display 1440 x 3200 at fc400000`, USB (`product: xnuloader`) | ✅ |
| ядро XNU с `-v cpus=1`: GICv3, VM (окно `0x8C0000000`, 6 ГБ), IOKit, `bsd_init`, `MiniIOMobileFramebuffer` 1125x2436 на панели 1440x3200 | ✅ доходит до поиска корневой ФС (`setconf`), без образа iOS дальше и не должно |
| образ iOS 16.5: SpringBoard, рабочий стол | ✅ (`build-ios.sh`, `./xl ramdisk` перед `./xl run`) |
| тач: драйвер Synaptics S3910P (`syna_touch=1`) | на телефоне чип отвечает (питание, сброс, SPI работают); первая версия теряла сообщения: вторая передача шла через микросекунды после первой |
| несколько ядер (`cpus=8`, `bootcpu=7`) | ✅ 7 ядер, ядро на X4 (кадр в драйвере экрана 3 мс вместо 46). Падало в настройке GIC (внешний abort): у SM8650 редистрибьюторы через 0x40000 (`redistributor-stride` в DT), а ядро шагало по 0x20000 и читало несуществующий кадр. Загрузчик теперь передаёт шаг и весь регион (8 × 256 КБ), ядро идёт по нему |
| кнопки (`buttons=1`, включаются с `syna_touch=1`) | ✅ питание, громкость+ и громкость−: драйвер redwood с раскладкой арбитра v7 |
| частоты ядер (`cpufreq=1`) | EPSS SM8650: 4 группы с `0x17D91000` — ядра 0-1 → группа 0, 2-4 → 3, 5-6 → 1, 7 → 2 (`qcom,freq-domain` в DT); каждая группа на верхнюю строку своей таблицы. ✅ На телефоне: прошивка уже держит большие ядра на максимуме (X4 3302, A720 3148 и 2956 МГц), поднимаются только A520: 12-я строка → 17-я, 2265 МГц |
| тепловая служба (`bsd/dev/shennong_thermal.c`, вкл. сама на shennong; `thermal=0` выкл., `thermal=2` только смотреть) | каждые 20 мс частота каждой группы по нагрузке (как schedutil: 1,25 × занятость, от 85% сразу на разрешённый верх; `cpu_dvfs=0` — как раньше, верх); каждые 100 мс пределы по tsens: ядра `thermal_cpu=80`, GPU `thermal_gpu=80` (потолок DCVS, только с `sgpu_dcvs=1`) — строкой ниже той, где работает, двумя при +5 °C, обратно через секунду при −5 °C; каждые 10 с «бюджет» 0..10 по АКБ `thermal_batt=40`: шаг −7% верха каждой группы и −1 скорость GPU; от `thermal_crit=105` всё на минимум. `sysctl kern.redwood.thermal` — таблица; уровень для iOS `kern.redwood.thermal_pressure` (его постит `thermald`); строка `thermal: ...` в лог раз в `thermal_log=10` с |
| вибро (`haptic=1..4`, `bsd/dev/shennong_haptic.c`) | модуль haptics PM8550B (HAP525_HV: SID 7, `0xF000` настройка, `0xF100` паттерны, `0xF200` boost) через запись в арбитр SPMI v7 (`chnls` `0xC500000`, `0x1000` на канал). Настройка как у Android для постоянной вибрации (`qcom-hv-haptics.c` Xiaomi): VMAX 1600 мВ, синус, тормоз ff 3f 1f, 130 Гц, direct play. 1 — только чтение, 2 — настройка, 3 — `haptic_count` × `haptic_ms` (`haptic_amp`, `haptic_mv` ≤ 3600), 4 — для iOS (`/dev/redwood-haptic`). На телефоне ещё не запускался |
| GPU Adreno 750 (`sgpu=N`) | шаги 1–2 ✅ (тактирование GPU в GCC включено, под аппаратным гейтингом; рельсы gfx/cx/mx/mxc/ebi в RPMh, gmxc нет; speed bin 5); шаги 1–3 (`bsd/dev/shennong_adreno.c`): тактирование GPU в GCC и имена рельсов в RPMh, фьюз speed bin, контроллер тактирования GPU (`0x3d90000`) — ✅ читается; блок GPU выключен целиком (CX и GX свёрнуты, PLL не настроены). Шаги 4–5 ✅: CX включается (`cx_gdscr` → `0xf822f800`), голоса за GPLL0, `gmu_clk_src`/`hub_clk_src` 200 МГц, `cxo`, `hub_cx_int`, `cx_gmu` работают (`gpu_cc_memnoc_gfx` идёт от `gcc_gpu_memnoc_gfx` под аппаратным гейтингом и показывает «выкл» — не мешает); в конце всё выключается обратно (`sgpu_keep=1` — оставить). Шаг 6 ✅: GMU отвечает (`cm3_sysreset` 1 — процессор в сбросе, `cm3_cfg` 0x4052), GBIF (`halt_ack` 0x3) и RSCC читаются; старый always-on счётчик GMU (`0x1f888`) стоит — у A750 свой (`GMU_CX_AO_COUNTER`, `0x1f880`, Linux `a6xx_gmu_get_timestamp`). Шаги 7–10: вызов TrustZone `qcom_scm_gpu_init_regs(ALWAYS_EN|TSENSE_EN)` (svc 0x28 cmd 1; Linux делает его на A750 до старта GMU) и счётчик A750, SMMU GPU (ID-регистры, таблица потоков, контекст 0), запись двух слов в DTCM GMU. `sgpu_new=K` — паузы перед каждым доступом у шагов K..N, а не только у последнего. Шаги 7–10 ✅: TrustZone отвечает как Linux, `gpu_init_regs` → 0, fuse 0x7 (blending, LPAC, ray tracing); счётчик A750 идёт на 19.2 МГц; SMMU GPU: 24 SMR, 22 контекста, 0x40000, таблица потоков пуста — SMMU наш; контекст 0 — настоящее железо; DTCM GMU принимает запись. Шаги 11–15: память и контекст SMMU 1 для GMU (поток 5), загрузка `fw/gmu_gen70900.bin`, RSCC/PDC (как `a6xx_gmu_rpmh_init` для A740-семейства), старт GMU (`a6xx_gmu_fw_start` для A7xx: chipid 0x7090100, лог в `GENERAL_8`), `HFI_CTRL_INIT`. Прошивки — `build/fw-gpu/` на Mac, `build-ios.sh` кладёт их в `root.apfs`. Шаги 11–15 ✅: прошивка GMU из linux-firmware (`qcom/gmu_gen70900.bin`, 78588 байт, как на телефоне: HyperOS не отдаёт `/vendor/firmware` даже shell'у) — 6 областей, контекст SMMU 1, `fw_init_result` 0x100 (**прошивка GMU работает**), `hfi_ctrl_status` 1, HFI 0x20060001; в конце RSC засыпает, CX выключается. Шаги 16–17: таблицы скоростей (231–720 МГц, голоса как `a6xx_gmu_rpmh_arc_votes_init`) и шины, `CORE_FW_START`, `START`, скорость (`sgpu_speed=N`), `GPU_SET` — GMU включает 3D-ядро, первые регистры. Zap-шейдер подписан Xiaomi: только с телефона. Прошивки на телефоне: `/vendor/firmware/gmu_gen70900.bin`, `gen70900_sqe.fw`, `gen70900_aqe.fw`, `/odm/firmware/gen70900_zap.mbn` |

Как запускалось: `MODE=DEBUG bash build-uefi.sh` (сборка в x86-контейнере Podman на Mac), затем
`fastboot boot shennong-boot-debug.img`; ядро — `XL_RAMDISK=none XL_SECS=20 XL_CMDLINE="-v" ./xl run`.
`xl` на macOS: `brew install coreutils flock` и `PATH="$(brew --prefix coreutils)/libexec/gnubin:<platform-tools>:$PATH"`.

## Что дальше (по порядку)

1. **Собрать UEFI** на Linux (на Mac — в VM/контейнере), `uefi/README.md`:
   `uefi/apply.sh` → `build_uefi.py -d shennong -r RELEASE`. Сабмодуль `cmocka` с gitlab для сборки не нужен.
2. **Первый запуск — без HackiOS**: `fastboot boot` образа Mu-Silicium. Рубеж: меню UEFI на экране.
   Если падает — это патчи драйверов, `uefi/files/Binaries/shennong/README.md` (PmicDxe/SPMIDxe первыми).
   Патчи giulia по смещениям не переносятся: все модули другой сборки.
3. `common/build.sh shennong SKIP="images"` → `out/shennong-boot.img` → `fastboot boot`. Рубеж: экран загрузчика и
   `./xl log` по USB. `./xl enter` собирает образ из переменных окружения — перед ним
   `set -a; . common/devices/shennong.conf; set +a`, иначе возьмёт адреса redwood.
4. Ядро с `BOOTARGS="-v cpus=1"`: текст загрузки XNU на экране. Главный неизвестный — XNU 8796 на
   Cortex-X4/A720/A520 (ARMv9.2).

## Тач (`bsd/dev/shennong_synaptics.c`)

Synaptics S3910P, TouchComm v1, SPI в режиме 0. Шина — ноги serial engine 4 у QUP 1, но сам engine не
используется: SPI вручную через TLMM, как у `redwood_goodix.c`. Всё из device tree телефона:

| Что | Значение |
|---|---|
| SPI | GPIO 48 MISO, 49 MOSI, 50 CLK, 51 CS (`qup1_se4_l0..l3`), MISO с подтяжкой вниз |
| сброс / attention | GPIO 161 (0 = сброс) / GPIO 162 (0 = у чипа сообщение) |
| питание | AVDD — ключ на GPIO 164 (`touch_avdd_vreg`); IOVDD — LDO12 PM8550 (`ldob12`, 1,8 В), голосом в RPMh: apps RSC DRV2 `0x17A20000`, TCS 0, cmd-db `0x81C60000`, раскладка регистров RSC 3.0 |
| задержки | питание 50 мс, сброс 10 мс, после сброса 200 мс |
| координаты | из GET_APPLICATION_INFO (по DT 1440 × 3200); если поля шире — в 1/100 пикселя (`super-resolution-factor` 100) |

Протокол — по драйверу Qualcomm `synaptics_tcm` (тот, чей `compatible` стоит в DT): после сброса ждём identify,
если чип в загрузчике — RUN_APPLICATION_FIRMWARE, затем GET_APPLICATION_INFO (0x20) и GET_TOUCH_REPORT_CONFIG
(0x25); отчёты 0x11 разбираются по этой конфигурации. Проверка без телефона: модель чипа на уровне ног
(`tools/syna-sim/`), пять случаев — загрузчик → приложение, координаты ×100, сразу приложение, чипа нет,
и как на телефоне: недочитанный кадр, поток кадров 0xC0, касания только после ENABLE_REPORT. Модель отдаёт
мусор передаче, начатой раньше 50 мкс после предыдущей.

Что выяснилось на телефоне: после чтения заголовка чип готовит продолжение (A5 03 …) не сразу — передача,
начатая через микросекунды, получает мусор из сдвигового регистра (`01 ff`). Поэтому перед каждой передачей
пауза (`syna_gap_us`, 200 мкс), и сообщения читаются, как у обоих вендорских драйверов, «предсказанием»:
первая передача длиной с прошлое сообщение. Кроме касаний чип шлёт отчёты 0xC0 (кадры сырых данных
сенсора, ~10000 на узел — для обработки на хосте у Xiaomi); отчёт, приходящий чаще 30 раз в секунду,
выключается DISABLE_REPORT, отчёт касаний включается ENABLE_REPORT 0x11.

Boot-args: `syna_touch=1` включает; `syna_iovdd=0` не трогать LDO12; `syna_flip=1|2|3` отразить по X/Y;
`syna_swap=1` поменять оси; `syna_poll_us=N` как часто смотреть на attention (4000); `syna_dump=N` первые N
сообщений в hex; `syna_gap_us=N` пауза перед передачей (200); `syna_keep=1` не выключать частые отчёты.
В логе всё с префиксом `syna:`.

## Чего НЕ включать

`touchscreen=1` (тач этого телефона — `syna_touch=1`), `gpu=1`, `wifi=1`, `adsp=1` — драйверы с адресами redwood (GPIO/SPI тача
`redwood_goodix.c`, GPU, удалённые процессоры). На SM8650 они дёргают чужие регистры. (`cpufreq=1` и кнопки
выбирают адреса по имени телефона от загрузчика и на shennong свои; голос за полосу памяти `ddr=` — только redwood.)
Каждый — отдельная работа:

* **watchdog**: свой у SM8650;
* **GPU**: Adreno 750 (A7xx) — `redwood_adreno.c` под A6xx по сути переписывать;
* **Wi-Fi**: PCIe в XNU + ath12k на LinuxKPI;
* **заряд**: под ядром телефон только разряжается — начинать с полной батареей.

### Память

ОЗУ телефона (12 ГБ, `/proc/iomem` Android) лежит кусками: ~1,5 ГБ ниже 4 ГБ (дробно, между регионами прошивок), 0x880000000–0x8b9efffff (~0,9 ГБ, тоже с дырами) и один сплошной блок 0x8c0000000–0xaffffefff (~9 ГБ). XNU на arm64 берёт один непрерывный диапазон `[physBase, physBase + memSize)`, поэтому загрузчик отдаёт ему самый большой свободный блок UEFI. Раньше он обрезался до 6 ГБ (`MAX_MEM_SIZE`); теперь предел 16 ГБ, а начальные таблицы страниц ядра (`BOOTSTRAP_TABLE_SIZE` в `redwood.patch`) — 34 страницы вместо 20. Остальные ~3 ГБ потребуют поддержки нескольких диапазонов в VM ядра.

