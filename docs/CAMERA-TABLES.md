# Camera register tables (not in this repository)

`bsd/dev/shennong_camera.c` includes two headers that are **not** distributed here:

* `shennong_ovx9000.h` — the wide sensor (OmniVision OVX9000) and the C-PHY receiver settings;
* `shennong_s5kjn1.h` — the ultra-wide and telephoto sensor (Samsung S5KJN1).

Their content is sensor-vendor tuning data and register tables taken from Xiaomi's Android camera stack (and, for the C-PHY
settings, from a GPL-2.0 kernel header), so it is not ours to publish. These two files were left out.
**Without them `cam=` drivers do not build.** Everything else builds; leave
the camera out of your kernel (`shennong_camera.c`) if you do not want it.

## What you have to provide

Make the two headers from **your own phone's files** and put them in `xnu/files/bsd/dev/`.

### `shennong_ovx9000.h`

```c
struct sensor_write { uint16_t address; uint8_t data; uint8_t delay_ms; };   /* delay after the write */
struct phy_write    { uint16_t address; uint8_t data; uint16_t delay_us; };

static const struct sensor_write ovx9000_init[];    /* initSettings */
static const struct sensor_write ovx9000_mode9[];   /* resolution 9: 1920x1080 RAW10, 3 C-PHY trios at 3.5 Gsps, 240 fps */
static const struct sensor_write ovx9000_mode1[];   /* resolution 1: 4096x3072 RAW10, 30 fps */
static const struct sensor_write ovx9000_on[];      /* streamOnSettings (a 2 ms delay after register 0x0100) */
static const struct sensor_write ovx9000_off[];     /* streamOffSettings */
static const struct phy_write csiphy_3ph[];         /* CSIPHY 2.2.0, C-PHY three-phase settings */
static const struct phy_write csiphy_3p5gsps[];     /* the 3.5 Gsps data rate's, standard channel */
static const struct phy_write csiphy_irq_masks[];
```

* The sensor tables come from the phone's camera module file, `com.qti.sensormodule.shennong_*_ovx9000_wide*.bin`
  ("Parameter Parser V3.6.2", on the phone under `/odm` or `/vendor` camera data): each write is a 16-bit address and
  8-bit data.
* The three `csiphy_*` tables are the CSIPHY v2.2.0 C-PHY settings of Android's camera kernel driver
  (`cam_csiphy_2_2_0_hwreg.h`, GPL-2.0), as `{ address, value, delay us }` — take them from the kernel source of **your own
  phone's Android build** (Xiaomi publishes it) and keep its licence in mind.

### `shennong_s5kjn1.h`

```c
struct s5k_write { uint16_t address; uint16_t data; uint8_t bytes; uint16_t delay_us; };

static const struct s5k_write s5kjn1_init[];        /* initSettings: 39 writes (soft reset 0x6010 then 5 ms, 0x6226 then 10 ms) */
static const struct s5k_write s5kjn1_mode4[];       /* resolution 4: 1920x1080 RAW10, 240 fps */
static const struct s5k_write s5kjn1_mode1[];       /* resolution 1: 4080x3072 RAW10, 30 fps */
static const struct s5k_write s5kjn1_on[], s5kjn1_off[], s5kjn1_hold_on[], s5kjn1_hold_off[];
```

* From `com.qti.sensormodule.shennong_ofilm_s5kjn1_{ultra,tele}.bin` ("sensor driver data V3.6"; both files have the same
  tables). Address and data are 16-bit; `bytes` is 2, or 1 where the entry's size says so; the delay follows each write in
  microseconds. Writes to `0x6028`, `0x602a`, `0x6f12` are the sensor's indirect page/address/data triples.
* MCLK 19.2 MHz, four D-PHY lanes, settle count 38.

## Why this is documented rather than scripted

The module-file format ("Parameter Parser") is Qualcomm's and was read by hand. A converter would be a welcome
contribution (`tools/`), provided it works from the user's own files and ships none of their content.
