/*
 * The cameras of the Xiaomi 14 Pro (shennong), first steps: each sensor powered up as Android powers
 * it, its chip ID read, and powered down again. Every address and number below belongs to this phone,
 * from its device tree (qcom,cam-sensor0..3 and what they point to: the CCI masters, the pins, the
 * regulators), the sensors' module files of Android's camera stack (odm/lib64/camera/
 * com.qti.sensormodule.shennong_*.bin of the phone's HyperOS: the slave address, where the chip ID is
 * read and what it is, the power sequences), and Linux's camcc-sm8650.c, pinctrl-sm8650.c and
 * gcc-sm8650.c for the clock controller's and the pins' registers:
 *
 *   wide    cam-sensor0, OmniVision OVX9000 ("shennong_sunny_ovx9000_wide", the semco one the same):
 *           CCI 0 master 1 (GPIO 115 data, 116 clock), slave 0x10 (Android's 8-bit 0x20); chip ID
 *           0x5650 in the 16-bit register 0x300a; MCLK0 (GPIO 100), reset GPIO 108; VANA a fixed
 *           2.9 V regulator GPIO 127 switches, VIO ldob10 1.8 V, VDIG ldom2 (PM8010) 1.2 V, VAF ldom6
 *           3.3 V, CUSTOM1 ldom7 3.3 V, BOB2 3.296 V; CSIPHY 3
 *   ultra   cam-sensor2, Samsung S5KJN1 ("shennong_ofilm_s5kjn1_ultra"): CCI 0 master 0 (GPIO 113,
 *           114), slave 0x3d (0x7a); chip ID 0x38e1 at 0x0000; MCLK1 (GPIO 101), reset GPIO 109; VANA
 *           2.8 V from GPIO 126, VIO ldob10, VDIG ldoe1 1.056 V, VAF ldom4 2.8 V, BOB2; CSIPHY 1
 *   tele    cam-sensor3, Samsung S5KJN1 ("shennong_ofilm_s5kjn1_tele"): CCI 1 master 0 (GPIO 117,
 *           118), slave 0x2d (0x5a); chip ID 0x38e1 at 0x0000; MCLK3 (GPIO 103), reset GPIO 110;
 *           VANA 2.8 V from GPIO 125, VIO ldob10, VDIG ldoc2 1.056 V, VAF ldob9 3.104 V, BOB2; CSIPHY 0
 *   front   cam-sensor1, OmniVision OV32B40 ("shennong_sunny_ov32b40_front"): not on a CCI but on
 *           QUP2 SE1's I2C (GPIO 4 data, 5 clock, both eGPIOs), slave 0x10; chip ID 0x3242 at
 *           0x300b; MCLK4 (GPIO 104, cam_aon_mclk4), reset GPIO 7 (an eGPIO); VANA ldom3 2.8 V, VIO
 *           ldom5 1.8 V, VDIG ldom1 1.104 V, BOB2; CSIPHY 4
 *
 * The I2C is done by hand on the pins (as redwood_camera.c does): each pin taken as a GPIO for the
 * transaction (released high by the pull-ups, driven low) and given back as it was found. MCLK is the
 * crystal's 19.2 MHz (camcc's MCLK RCG at its source 0, Android's own frequency), with the camera's
 * top power domain (titan_top_gdsc, "cam_clk" of every sensor) on first, and camcc's always-on
 * clocks and GCC's camera AHB and XO clocks, which Linux switches on at probe. The LDOs are RPMh's
 * (through the application processor's RSC, as in shennong_wlan.c, command set cam_tcs, default 1:
 * the touch screen's is 0, the radio's 2): the voltage of the device tree and the module file, then
 * on (LDOs in high-power mode, as qcom,init-mode 4 says).
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   cam=1   a look, reading only: the pins of every sensor, GCC's camera clocks, camcc's power
 *           domain and MCLK clocks, where RPMh has the supplies
 *   cam=2   ... and one sensor (cam_sensor=wide|ultra|tele|front, default wide; cam_sensor=all: each
 *           in turn) powered up in its module file's order, its chip ID read (cam_tries times, default
 *           3), its first registers shown, and powered down in the module file's order
 *
 *   cam=3   ... the camera's blocks (CPAS, CSID-lite 0, IFE-lite 0, TPG 0) up to their clocks at
 *           19.2 MHz, with MMCX and the camera's buses voted for, their versions read, all down again
 *   cam=4   TPG 0's colour bars through CSID-lite 0's RDI 0 and IFE-lite 0 into memory, through a
 *           context of ours in the SMMU; the loader hands the memory to the host (xl camera)
 *   cam=5   the wide sensor's own frames the same way: camcc's PLL0 and the blocks' clocks at their
 *           Android rates, CSIPHY 3 as three C-PHY trios at 3.5 Gsps, the sensor initialised and
 *           set to 1920x1080 RAW10 by its module file's tables, streaming into CSID-lite 0 (RX from
 *           PHY 3), RDI 0, IFE-lite 0, memory. cam_vts (frame length in lines, default 2768: about
 *           30 frames a second in place of 240), cam_exp (exposure, lines, default 2700), cam_gain
 *           (0x3508, default 4), cam_mhz (400 or 480, default 480: CSID, IFE-lite, CPHY RX),
 *           cam_mmcx (an MMCX level to vote for, only ever upward; default: none voted). Now with
 *           cam_once=1 only: by default cam=5 gives the frames to iOS (/dev/redwood-camera, as
 *           redwood_camera.c's cam=5: Camera.app through libredwood-media in mediaserverd), the full
 *           path (CSID 0, IFE 0), a ring of three frames, the address at every start of frame
 *
 * cam_pause=MS (default 1500) before each group of writes, for the loader to note the log (as
 * wlan_pause). cam_keep=1: the sensor is left powered after cam=2 (until the run's end).
 *
 * What stays when it is over: RPMh's votes for the shared supplies (BOB2, ldob10, ldob9, ldoc2,
 * ldoe1: taking a vote back could take a rail from something else voted for through the same
 * channel); the PM8010's LDOs (ldom1..7: the cameras' own PMIC) are voted off again. At the run's end
 * (and on a panic's way out) a sensor still powered is powered down.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <kern/locks.h>
#include <vm/vm_kern.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <sys/redwood_camera.h>

#include "shennong_ovx9000.h"
#include "shennong_s5kjn1.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level, pause_ms = 1500, keep, tries = 3, our_tcs = 1;
static bool broken, leaving;

static int say(const char *format, ...) __printflike(1, 2);

/*
 * cam=5 keeps its lines in the frames' memory too (the last 64 KB, which the host gets with xl camera): the
 * loader's log loses its middle on a busy boot, and the sweep's lines were lost twice
 */
static char *diag;
static uint32_t diag_len;
#define DIAG_BYTES              0x10000u

/* every line to the loader's log as well (and to the console when that may be written) */
static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != '\0'; c++) {
		loader_log_putc(*c);
		if (diag != NULL && diag_len < DIAG_BYTES - 1) {
			diag[diag_len++] = *c;
		}
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (ms == 0) {
		return;
	}
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

/* until (*address & mask) == want, `us' at most; the last value read to *last */
static bool
wait_for(vm_offset_t address, uint32_t mask, uint32_t want, uint32_t us, uint32_t *last)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t value;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &interval);
	do {
		value = rd(address);
	} while ((value & mask) != want && mach_absolute_time() - start < interval);
	if (last != NULL) {
		*last = value;
	}
	return (value & mask) == want;
}

/* before a group of writes: said, and a pause for the loader to note the log (a silent reset leaves only that) */
static bool announce_quiet;     /* (the same writes again and again: no pause, no line) */

static void
announce(const char *what)
{
	if (announce_quiet) {
		return;
	}
	say("camera: WRITES: %s%s\n", what, pause_ms != 0 && !leaving ? " (in a moment)" : "");
	if (!leaving) {
		wait_ms(pause_ms);
	}
}

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("camera: %s: nothing more is written in this run (but the power-down)\n", why);
	}
}

/* ---- the address map ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL    /* pinctrl@f000000 + 0x100000 (pinctrl-sm8650.c) */
#define TLMM_SIZE               0x000c0000UL    /* up to GPIO 191 */
#define GCC_CAMERA_PHYS         0x00126000UL    /* GCC_CAMERA_BCR's page: the AHB (0x26004) and XO (0x26028) clocks */
#define CAMCC_PHYS              0x0ade0000UL    /* clock-controller@ade0000, "qcom,pineapple-camcc-v2" */
#define CAMCC_SIZE              0x00020000UL
#define CMD_DB_PHYS             0x81c60000UL    /* reserved-memory aop_cmd_db_region */
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x17a20000UL    /* apps_rsc "drv-2", the application processor's */
#define RSC_SIZE                0x10000UL

static vm_offset_t tlmm, gcc_camera, camcc, cmd_db, rsc;

/* ---- pins ---------------------------------------------------------------------------------------------- */

/* [1:0] pull (0 none, 1 down, 2 keeper, 3 up), [5:2] function, [8:6] drive (mA / 2 - 1), [9] output */
#define CFG_PULL_DOWN           1
#define CFG_PULL_UP             3
#define CFG_FUNC(f)             ((uint32_t)(f) << 2)
#define CFG_DRIVE_MA(ma)        ((uint32_t)((ma) / 2 - 1) << 6)
#define CFG_OUTPUT              (1u << 9)
#define CFG_EGPIO_PRESENT       (1u << 11)      /* a pin another subsystem may own ... */
#define CFG_EGPIO_ENABLE        (1u << 12)      /* ... and is APPS's only while this is set */
#define IO_IN                   1u
#define IO_OUT                  2u

static vm_offset_t
pin(unsigned n)
{
	return tlmm + 0x1000UL * n;
}

static void
pin_set(unsigned n, uint32_t cfg, int out)
{
	if (rd(pin(n)) & CFG_EGPIO_PRESENT) {
		cfg |= CFG_EGPIO_PRESENT | CFG_EGPIO_ENABLE;
	}
	if (out >= 0) {
		wr(pin(n) + 4, out ? IO_OUT : 0);       /* the level first, so that the pin does not glitch */
	}
	wr(pin(n), cfg);
}

static void
pin_show(unsigned n, const char *name)
{
	uint32_t cfg = rd(pin(n)), io = rd(pin(n) + 4);

	say("camera:   gpio%-3u %-12s cfg %#06x (function %u, pull %u, %u mA, %s%s), reads %u, drives %u\n", n, name, cfg,
	    cfg >> 2 & 0xf, cfg & 3, ((cfg >> 6 & 7) + 1) * 2, cfg & CFG_OUTPUT ? "output" : "input",
	    cfg & CFG_EGPIO_PRESENT ? (cfg & CFG_EGPIO_ENABLE ? ", eGPIO: ours" : ", eGPIO: NOT ours") : "", io & IO_IN, io >> 1 & 1);
}

/* ---- RPMh: the supplies (as shennong_wlan.c) ------------------------------------------------------- */

#define RSC_TCS_BASE            0xd00           /* qcom,tcs-offset; TCS 0..2 are "active" ones */
#define RSC_TCS_STRIDE          672             /* rpmh-rsc.c RSC_DRV_TCS_OFFSET */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

static struct {
	uint32_t wait_for_completion, control, status, cmd_enable, msgid, addr, data, cmd_stride;
} tcs_reg;
#define DRV(reg)                (rsc + RSC_TCS_BASE + (reg))
#define TCS(reg)                (rsc + RSC_TCS_BASE + RSC_TCS_STRIDE * our_tcs + (reg))

#define VRM_VOLTAGE             0
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_entry *
rpmh_entry(const char *name, const uint8_t **aux)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };
	size_t length = strnlen(name, sizeof(id));

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	memcpy(id, name, length);      /* (eight bytes, no NUL needed: "mmcx.lvl" fills them; strlcpy cut it) */
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512 ||
		    sizeof(*db) + slave->header_offset + slave->count * sizeof(*entry) > CMD_DB_SIZE) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				if (aux != NULL) {
					*aux = db->data + slave->data_offset + entry[i].offset;
				}
				return &entry[i];
			}
		}
	}
	return NULL;
}

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_entry *e = rpmh_entry(name, NULL);

	return e != NULL ? e->address : 0;
}

static bool
rpmh_wait(vm_offset_t address, uint32_t mask, uint32_t value, const char *what)
{
	uint32_t last;

	if (wait_for(address, mask, value, 200000, &last)) {
		return true;
	}
	say("camera: RPMh: timeout waiting for %s (%#x)\n", what, last);
	return false;
}

/* Up to three register writes to RPMh through our command set, and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t bit = 1u << our_tcs, irq_enable;
	bool done;

	if (!rpmh_wait(TCS(tcs_reg.status), 1, 1, "our command set to be idle")) {
		return false;
	}
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_ENABLE);
	wr(DRV(TCS_IRQ_CLEAR), bit);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(tcs_reg.msgid + tcs_reg.cmd_stride * i), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(tcs_reg.addr + tcs_reg.cmd_stride * i), address[i]);
		wr(TCS(tcs_reg.data + tcs_reg.cmd_stride * i), data[i]);
	}
	wr(TCS(tcs_reg.wait_for_completion), (1u << count) - 1);
	wr(TCS(tcs_reg.cmd_enable), (1u << count) - 1);
	irq_enable = rd(DRV(TCS_IRQ_ENABLE));
	wr(DRV(TCS_IRQ_ENABLE), irq_enable | bit);      /* the status bit follows the enable; the interrupt stays masked */
	wr(TCS(tcs_reg.control), TCS_CONTROL_ENABLE);
	wr(TCS(tcs_reg.control), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(DRV(TCS_IRQ_STATUS), bit, bit, "the request");
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(tcs_reg.cmd_enable), 0);
	wr(TCS(tcs_reg.wait_for_completion), 0);
	wr(DRV(TCS_IRQ_CLEAR), bit);
	wr(DRV(TCS_IRQ_ENABLE), rd(DRV(TCS_IRQ_ENABLE)) & ~bit);
	return done;
}

static void
rpmh_setup(void)
{
	uint32_t id = rd(rsc), major = id >> 16 & 0xff, minor = id >> 8 & 0xff;

	if (major >= 3) {
		tcs_reg.wait_for_completion = 0x20; tcs_reg.control = 0x24; tcs_reg.status = 0x28; tcs_reg.cmd_enable = 0x2c;
		tcs_reg.msgid = 0x34; tcs_reg.addr = 0x38; tcs_reg.data = 0x3c; tcs_reg.cmd_stride = 24;
	} else {
		tcs_reg.wait_for_completion = 0x10; tcs_reg.control = 0x14; tcs_reg.status = 0x18; tcs_reg.cmd_enable = 0x1c;
		tcs_reg.msgid = 0x30; tcs_reg.addr = 0x34; tcs_reg.data = 0x38; tcs_reg.cmd_stride = 20;
	}
	say("camera: RPMh %u.%u; our command set %u: status %#x, control %#x; the DRV's irq enable %#x status %#x\n", major, minor,
	    our_tcs, rd(TCS(tcs_reg.status)), rd(TCS(tcs_reg.control)), rd(DRV(TCS_IRQ_ENABLE)), rd(DRV(TCS_IRQ_STATUS)));
}

/* ---- the sensors ------------------------------------------------------------------------------------ */

/* the power sequences' kinds, Android's cam_sensor kinds as the module files have them */
enum { MCLK = 0, VANA = 1, VDIG = 2, VIO = 3, VAF = 4, CUSTOM1 = 6, RESET = 8, BOB2 = 14 };

struct rail {
	const char *    rpmh;           /* RPMh's name ("ldom2") ... */
	uint16_t        mv;             /* ... at the device tree's voltage */
	uint16_t        gpio;           /* or else: the pin that switches a fixed regulator (0: none) */
};
struct step {
	uint8_t         kind;
	uint8_t         ms;             /* the delay after it */
};
struct sensor {
	const char *    name;
	const char *    chip;
	unsigned        sda, scl;       /* the I2C's pins, done by hand */
	uint8_t         slave;          /* 7-bit */
	uint16_t        id_reg, id;     /* the chip ID: a 16-bit register at a 16-bit address */
	unsigned        mclk;           /* camcc's MCLKn ... */
	unsigned        mclk_pin;       /* ... on this pin, function 1 (cam_mclk or cam_aon_mclk4) */
	unsigned        reset;
	struct rail     vana, vio, vdig, vaf, custom1, bob2;
	struct step     up[10], down[10];
	unsigned        nup, ndown;
};

#define BOB2_RAIL               { "bobb2", 3296, 0 }
#define NONE                    { NULL, 0, 0 }

static const struct sensor sensors[] = {
	{
		.name = "wide", .chip = "OmniVision OVX9000", .sda = 115, .scl = 116, .slave = 0x10, .id_reg = 0x300a, .id = 0x5650,
		.mclk = 0, .mclk_pin = 100, .reset = 108,
		.vana = { NULL, 2900, 127 }, .vio = { "ldob10", 1800, 0 }, .vdig = { "ldom2", 1200, 0 },
		.vaf = { "ldom6", 3300, 0 }, .custom1 = { "ldom7", 3300, 0 }, .bob2 = BOB2_RAIL,
		.up = { { BOB2, 2 }, { VAF, 1 }, { VANA, 0 }, { VIO, 1 }, { CUSTOM1, 1 }, { VDIG, 1 }, { MCLK, 1 }, { RESET, 5 } }, .nup = 8,
		.down = { { MCLK, 1 }, { RESET, 1 }, { VDIG, 0 }, { CUSTOM1, 3 }, { VIO, 0 }, { VANA, 0 }, { VAF, 1 }, { BOB2, 2 } },
		.ndown = 8,
	},
	{
		.name = "ultra", .chip = "Samsung S5KJN1", .sda = 113, .scl = 114, .slave = 0x3d, .id_reg = 0x0000, .id = 0x38e1,
		.mclk = 1, .mclk_pin = 101, .reset = 109,
		.vana = { NULL, 2800, 126 }, .vio = { "ldob10", 1800, 0 }, .vdig = { "ldoe1", 1056, 0 },
		.vaf = { "ldom4", 2800, 0 }, .custom1 = NONE, .bob2 = BOB2_RAIL,
		.up = { { BOB2, 2 }, { VAF, 1 }, { VIO, 1 }, { VDIG, 5 }, { VANA, 3 }, { RESET, 5 }, { MCLK, 10 } }, .nup = 7,
		.down = { { MCLK, 1 }, { RESET, 1 }, { VANA, 8 }, { VDIG, 1 }, { VIO, 1 }, { VAF, 1 }, { BOB2, 1 } }, .ndown = 7,
	},
	{
		.name = "tele", .chip = "Samsung S5KJN1", .sda = 117, .scl = 118, .slave = 0x2d, .id_reg = 0x0000, .id = 0x38e1,
		.mclk = 3, .mclk_pin = 103, .reset = 110,
		.vana = { NULL, 2800, 125 }, .vio = { "ldob10", 1800, 0 }, .vdig = { "ldoc2", 1056, 0 },
		.vaf = { "ldob9", 3104, 0 }, .custom1 = NONE, .bob2 = BOB2_RAIL,
		.up = { { BOB2, 2 }, { VAF, 1 }, { VIO, 1 }, { VDIG, 5 }, { VANA, 3 }, { RESET, 5 }, { MCLK, 10 } }, .nup = 7,
		.down = { { MCLK, 1 }, { RESET, 1 }, { VANA, 8 }, { VDIG, 1 }, { VIO, 1 }, { VAF, 1 }, { BOB2, 2 } }, .ndown = 7,
	},
	{
		.name = "front", .chip = "OmniVision OV32B40", .sda = 4, .scl = 5, .slave = 0x10, .id_reg = 0x300b, .id = 0x3242,
		.mclk = 4, .mclk_pin = 104, .reset = 7,
		.vana = { "ldom3", 2800, 0 }, .vio = { "ldom5", 1800, 0 }, .vdig = { "ldom1", 1104, 0 },
		.vaf = NONE, .custom1 = NONE, .bob2 = BOB2_RAIL,
		.up = { { BOB2, 2 }, { VANA, 1 }, { VIO, 1 }, { VDIG, 1 }, { MCLK, 1 }, { RESET, 5 } }, .nup = 6,
		.down = { { RESET, 0 }, { MCLK, 1 }, { VDIG, 0 }, { VIO, 0 }, { VANA, 0 }, { BOB2, 2 } }, .ndown = 6,
	},
};
#define SENSORS                 (sizeof(sensors) / sizeof(sensors[0]))

/*
 * cam=5's sensor (cam_sensor=wide, ultra or tele) and its CSIPHY, as the device tree has them: the wide on 3
 * (C-PHY, three trios), the ultra-wide on 1 and the telephoto on 0 (D-PHY, four lanes), the front on 4
 */
static unsigned sel;
static const unsigned sensor_phy[SENSORS] = { 3, 1, 0, 4 };
#define SEL                     (&sensors[sel])
#define S5K                     (sel == 1 || sel == 2)
#define PHY_N                   sensor_phy[sel]
#define DPHY                    (sel != 0)
static uint32_t cam_crop_x0, cam_crop_y0;  /* (cam_crop_x0, cam_crop_y0: the crop window's first pixel and line) */
static uint32_t cam_phy_again;
static uint32_t cam_crop = 1;          /* (the S5KJN1: RDI 0 cropped to the picture; cam_crop=0 off) */
static uint32_t cam_width, cam_height;  /* (the sensor's output size; set below) */

static const char *
kind_name(unsigned kind)
{
	switch (kind) {
	case MCLK: return "MCLK";
	case VANA: return "VANA";
	case VDIG: return "VDIG";
	case VIO: return "VIO";
	case VAF: return "VAF";
	case CUSTOM1: return "CUSTOM1";
	case RESET: return "RESET";
	case BOB2: return "BOB2";
	default: return "?";
	}
}

static const struct rail *
rail_of(const struct sensor *s, unsigned kind)
{
	switch (kind) {
	case VANA: return &s->vana;
	case VDIG: return &s->vdig;
	case VIO: return &s->vio;
	case VAF: return &s->vaf;
	case CUSTOM1: return &s->custom1;
	case BOB2: return &s->bob2;
	default: return NULL;
	}
}

/* ---- the clocks: GCC's camera clocks, camcc's power domain and MCLK ----------------------------------- */

#define GCC_CAMERA_AHB_CBCR     0x004
#define GCC_CAMERA_XO_CBCR      0x028
#define CBCR_ENABLE             (1u << 0)
#define CBCR_HW_CTL             (1u << 1)       /* gated by the hardware while idle: then CLK_OFF says nothing of it */
#define CBCR_OFF                (1u << 31)

#define CAMCC_MCLK_RCG(n)       (0x15000 + 0x1c * (n))  /* CMD_RCGR; CFG_RCGR at +4, M N D at +8 +0xc +0x10 */
#define CAMCC_MCLK_CBCR(n)      (CAMCC_MCLK_RCG(n) + 0x18)
#define RCG_CMD_UPDATE          (1u << 0)
#define RCG_CMD_ROOT_OFF        (1u << 31)
#define CAMCC_TITAN_TOP_GDSCR   0x132bc
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_PWR_ON             (1u << 31)

/* camcc's clocks Linux keeps on (cam_cc_sm8650_critical_cbcrs): the GDSCs' own clock, sleep, the DRV's XO and AHB */
static const uint32_t camcc_always_on[] = { 0x132ec, 0x13308, 0x13314, 0x13318 };

static bool gdsc_was_on, gdsc_ours;

static void
clocks_show(void)
{
	say("camera: GCC: camera AHB %#x, XO %#x\n", rd(gcc_camera + GCC_CAMERA_AHB_CBCR), rd(gcc_camera + GCC_CAMERA_XO_CBCR));
	if ((rd(gcc_camera + GCC_CAMERA_AHB_CBCR) & (CBCR_OFF | CBCR_HW_CTL)) == CBCR_OFF) {
		say("camera: (camcc's AHB clock is off: camcc is not read)\n");
		return;
	}
	say("camera: camcc: titan_top_gdsc %#x (%s); always-on clocks %#x %#x %#x %#x\n", rd(camcc + CAMCC_TITAN_TOP_GDSCR),
	    rd(camcc + CAMCC_TITAN_TOP_GDSCR) & GDSC_PWR_ON ? "on" : "off", rd(camcc + camcc_always_on[0]), rd(camcc + camcc_always_on[1]),
	    rd(camcc + camcc_always_on[2]), rd(camcc + camcc_always_on[3]));
	for (unsigned n = 0; n < 5; n++) {
		vm_offset_t r = camcc + CAMCC_MCLK_RCG(n);

		say("camera: camcc: MCLK%u RCG cmd %#x cfg %#x m %#x n %#x d %#x, branch %#x\n", n, rd(r), rd(r + 4), rd(r + 8),
		    rd(r + 0xc), rd(r + 0x10), rd(camcc + CAMCC_MCLK_CBCR(n)));
	}
}

static bool
branch_on(vm_offset_t cbcr, const char *name)
{
	uint32_t last;

	if (!(rd(cbcr) & CBCR_ENABLE)) {
		wr(cbcr, rd(cbcr) | CBCR_ENABLE);
	}
	if (rd(cbcr) & CBCR_HW_CTL) {
		/*
		 * (hardware-gated, as GCC's camera AHB clock is on SM8650 (0x80000003 here): it runs when its
		 * bus is used, and CLK_OFF stays set while it is idle -- Linux does not wait on such a branch)
		 */
		say("camera: %s: enabled, hardware-gated (%#x)\n", name, rd(cbcr));
		return true;
	}
	if (!wait_for(cbcr, CBCR_OFF, 0, 2000, &last)) {
		say("camera: %s does not come on (%#x)\n", name, last);
		return false;
	}
	return true;
}

/* GCC's camera clocks, camcc's always-on clocks, the camera's top power domain */
static bool
titan_up(void)
{
	uint32_t last;

	if (!branch_on(gcc_camera + GCC_CAMERA_AHB_CBCR, "GCC's camera AHB clock") ||
	    !branch_on(gcc_camera + GCC_CAMERA_XO_CBCR, "GCC's camera XO clock")) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(camcc_always_on) / sizeof(camcc_always_on[0]); i++) {
		if (!branch_on(camcc + camcc_always_on[i], "one of camcc's always-on clocks")) {
			return false;
		}
	}
	gdsc_was_on = (rd(camcc + CAMCC_TITAN_TOP_GDSCR) & GDSC_PWR_ON) != 0;
	if (!gdsc_was_on) {
		wr(camcc + CAMCC_TITAN_TOP_GDSCR, rd(camcc + CAMCC_TITAN_TOP_GDSCR) & ~GDSC_SW_COLLAPSE);
		gdsc_ours = true;
		if (!wait_for(camcc + CAMCC_TITAN_TOP_GDSCR, GDSC_PWR_ON, GDSC_PWR_ON, 10000, &last)) {
			say("camera: titan_top_gdsc does not come on (%#x)\n", last);
			return false;
		}
	}
	say("camera: GCC's camera clocks and camcc's on; titan_top_gdsc %s (%#x)\n", gdsc_was_on ? "was on already" : "switched on",
	    rd(camcc + CAMCC_TITAN_TOP_GDSCR));
	return true;
}

static void
titan_down(void)
{
	if (gdsc_ours) {
		wr(camcc + CAMCC_TITAN_TOP_GDSCR, rd(camcc + CAMCC_TITAN_TOP_GDSCR) | GDSC_SW_COLLAPSE);
		gdsc_ours = false;
		say("camera: titan_top_gdsc switched off again (%#x)\n", rd(camcc + CAMCC_TITAN_TOP_GDSCR));
	}
}

/* MCLKn at the crystal's 19.2 MHz: the RCG at source 0 (BI_TCXO), no divider, no M/N; then its branch */
static bool
mclk_on(unsigned n)
{
	vm_offset_t r = camcc + CAMCC_MCLK_RCG(n);
	uint32_t last;

	wr(r + 4, 0);                                   /* CFG: source 0, divider bypassed, mode 0 */
	wr(r, rd(r) | RCG_CMD_UPDATE);
	if (!wait_for(r, RCG_CMD_UPDATE, 0, 2000, &last)) {
		say("camera: MCLK%u's RCG does not take its configuration (%#x)\n", n, last);
		return false;
	}
	if (!branch_on(camcc + CAMCC_MCLK_CBCR(n), "MCLK's branch")) {
		return false;
	}
	say("camera: MCLK%u at 19.2 MHz: RCG cmd %#x cfg %#x, branch %#x\n", n, rd(r), rd(r + 4), rd(camcc + CAMCC_MCLK_CBCR(n)));
	return true;
}

static void
mclk_off(unsigned n)
{
	vm_offset_t cbcr = camcc + CAMCC_MCLK_CBCR(n);

	wr(cbcr, rd(cbcr) & ~CBCR_ENABLE);
}

/* ---- the supplies ------------------------------------------------------------------------------------ */

static bool
rail_on(const struct rail *r)
{
	if (r->rpmh == NULL && r->gpio == 0) {
		return true;
	}
	if (r->rpmh == NULL) {
		pin_set(r->gpio, CFG_DRIVE_MA(2) | CFG_OUTPUT, 1);
		say("camera:   gpio%u high (a fixed %u mV regulator)\n", r->gpio, r->mv);
		return true;
	}
	{
		uint32_t base = rpmh_address(r->rpmh), address[3], data[3], n = 0;
		bool ldo = r->rpmh[0] == 'l';

		if (base == 0) {
			say("camera:   %s is not in RPMh's command database\n", r->rpmh);
			return false;
		}
		address[n] = base + VRM_VOLTAGE; data[n++] = r->mv;
		address[n] = base + VRM_ENABLE; data[n++] = 1;
		if (ldo) {
			address[n] = base + VRM_MODE; data[n++] = VRM_MODE_LDO_HPM;
		}
		if (!rpmh_write(address, data, n)) {
			say("camera:   RPMh did not take %s\n", r->rpmh);
			return false;
		}
		say("camera:   %s (RPMh %#x) on, %u mV%s\n", r->rpmh, base, r->mv, ldo ? ", high-power mode" : "");
	}
	return true;
}

/* off: a fixed regulator's pin low; of RPMh's only the PM8010's LDOs (the cameras' own) */
static void
rail_off(const struct rail *r)
{
	if (r->rpmh == NULL && r->gpio != 0) {
		pin_set(r->gpio, CFG_DRIVE_MA(2) | CFG_PULL_DOWN | CFG_OUTPUT, 0);
		say("camera:   gpio%u low\n", r->gpio);
	} else if (r->rpmh != NULL && strncmp(r->rpmh, "ldom", 4) == 0) {
		uint32_t address[1] = { rpmh_address(r->rpmh) + VRM_ENABLE }, data[1] = { 0 };

		say("camera:   %s %s\n", r->rpmh, rpmh_write(address, data, 1) ? "off" : "NOT TAKEN OFF");
	} else if (r->rpmh != NULL) {
		say("camera:   %s: its vote stays (a shared supply)\n", r->rpmh);
	}
}

/* ---- I2C by hand ------------------------------------------------------------------------------------ */

#define HALF_US                 5               /* 100 kHz */

static unsigned sda_pin, scl_pin;
static uint32_t sda_was[2], scl_was[2];

static void
line(unsigned n, int high)
{
	/* high: let go (an input, the pull-ups take it up); low: driven low */
	pin_set(n, CFG_DRIVE_MA(2) | CFG_PULL_UP | (high ? 0 : CFG_OUTPUT), high ? -1 : 0);
}

static int
sda_reads(void)
{
	return rd(pin(sda_pin) + 4) & IO_IN;
}

static bool
scl_high(void)
{
	line(scl_pin, 1);
	for (unsigned i = 0; i < 1000; i++) {     /* (a device that stretches the clock) */
		if (rd(pin(scl_pin) + 4) & IO_IN) {
			return true;
		}
		spin_us(1);
	}
	return false;
}

static void
i2c_take(const struct sensor *s)
{
	sda_pin = s->sda;
	scl_pin = s->scl;
	sda_was[0] = rd(pin(sda_pin)); sda_was[1] = rd(pin(sda_pin) + 4);
	scl_was[0] = rd(pin(scl_pin)); scl_was[1] = rd(pin(scl_pin) + 4);
	line(sda_pin, 1);
	line(scl_pin, 1);
	spin_us(HALF_US);
}

static void
i2c_give_back(void)
{
	wr(pin(sda_pin) + 4, sda_was[1]);
	wr(pin(sda_pin), sda_was[0]);
	wr(pin(scl_pin) + 4, scl_was[1]);
	wr(pin(scl_pin), scl_was[0]);
}

static void
i2c_start(void)
{
	line(sda_pin, 1);
	(void)scl_high();
	spin_us(HALF_US);
	line(sda_pin, 0);
	spin_us(HALF_US);
	line(scl_pin, 0);
	spin_us(HALF_US);
}

static void
i2c_stop(void)
{
	line(sda_pin, 0);
	spin_us(HALF_US);
	(void)scl_high();
	spin_us(HALF_US);
	line(sda_pin, 1);
	spin_us(HALF_US);
}

/* a byte out; true if it was acknowledged */
static bool
i2c_out(uint8_t byte)
{
	bool ack;

	for (int bit = 7; bit >= 0; bit--) {
		line(sda_pin, byte >> bit & 1);
		spin_us(HALF_US);
		(void)scl_high();
		spin_us(HALF_US);
		line(scl_pin, 0);
	}
	line(sda_pin, 1);
	spin_us(HALF_US);
	(void)scl_high();
	spin_us(HALF_US / 2);
	ack = !sda_reads();
	spin_us(HALF_US / 2);
	line(scl_pin, 0);
	spin_us(HALF_US);
	return ack;
}

static uint8_t
i2c_in(bool more)
{
	uint8_t byte = 0;

	line(sda_pin, 1);
	for (int bit = 7; bit >= 0; bit--) {
		spin_us(HALF_US);
		(void)scl_high();
		spin_us(HALF_US / 2);
		byte = (uint8_t)(byte << 1 | sda_reads());
		spin_us(HALF_US / 2);
		line(scl_pin, 0);
	}
	line(sda_pin, more ? 0 : 1);            /* ACK for more, NACK for the last */
	spin_us(HALF_US);
	(void)scl_high();
	spin_us(HALF_US);
	line(scl_pin, 0);
	line(sda_pin, 1);
	spin_us(HALF_US);
	return byte;
}

/* `count' bytes from the 16-bit register address `reg' on; NULL, or who did not answer */
static const char *
i2c_read16(uint8_t slave, uint16_t reg, uint8_t *bytes, unsigned count)
{
	const char *problem = NULL;

	i2c_start();
	if (!i2c_out((uint8_t)(slave << 1))) {
		problem = "nobody acknowledges the address";
	} else if (!i2c_out((uint8_t)(reg >> 8)) || !i2c_out((uint8_t)reg)) {
		problem = "the register's address is not acknowledged";
	} else {
		i2c_start();
		if (!i2c_out((uint8_t)(slave << 1 | 1))) {
			problem = "nobody acknowledges the address for the read";
		} else {
			for (unsigned i = 0; i < count; i++) {
				bytes[i] = i2c_in(i + 1 < count);
			}
		}
	}
	i2c_stop();
	return problem;
}

/* one 8-bit register at a 16-bit address; true if every byte was acknowledged */
static bool
i2c_write8(uint8_t slave, uint16_t reg, uint8_t value)
{
	bool ok;

	i2c_start();
	ok = i2c_out((uint8_t)(slave << 1)) && i2c_out((uint8_t)(reg >> 8)) && i2c_out((uint8_t)reg) && i2c_out(value);
	i2c_stop();
	return ok;
}

/* a 16-bit register (or an 8-bit one: bytes 1) at a 16-bit address, the high byte first (the S5KJN1's) */
static bool
i2c_write16(uint8_t slave, uint16_t reg, uint16_t value, unsigned bytes)
{
	bool ok;

	i2c_start();
	ok = i2c_out((uint8_t)(slave << 1)) && i2c_out((uint8_t)(reg >> 8)) && i2c_out((uint8_t)reg) &&
	    (bytes == 1 || i2c_out((uint8_t)(value >> 8))) && i2c_out((uint8_t)value);
	i2c_stop();
	return ok;
}

/* ---- the power sequence ------------------------------------------------------------------------------ */

static const struct sensor *powered;    /* the sensor powered up now, if any */
static bool blocks_up;                  /* the blocks' clocks (cam=3 and up): they hold the top power domain */
static bool done_kind[16];              /* which of its steps are done (for a power-down after a failed power-up) */

static bool
do_step(const struct sensor *s, const struct step *st, bool up)
{
	const struct rail *r = rail_of(s, st->kind);
	bool ok = true;

	say("camera: %s %s%s\n", up ? "up:" : "down:", kind_name(st->kind), st->ms ? "" : " (no delay)");
	switch (st->kind) {
	case MCLK:
		if (up) {
			pin_set(s->mclk_pin, CFG_FUNC(1) | CFG_DRIVE_MA(2), -1);
			ok = mclk_on(s->mclk);
		} else {
			mclk_off(s->mclk);
			pin_set(s->mclk_pin, CFG_FUNC(0) | CFG_DRIVE_MA(2) | CFG_PULL_DOWN, -1);
		}
		break;
	case RESET:
		pin_set(s->reset, CFG_DRIVE_MA(2) | CFG_OUTPUT, up ? 1 : 0);
		break;
	default:
		if (up) {
			ok = rail_on(r);
		} else {
			rail_off(r);
		}
		break;
	}
	wait_ms(st->ms);
	if (ok && st->kind < 16) {
		done_kind[st->kind] = up;
	}
	return ok;
}

static void
power_down(void)
{
	const struct sensor *s = powered;

	if (s == NULL) {
		return;
	}
	say("camera: %s: powering down\n", s->name);
	for (unsigned i = 0; i < s->ndown; i++) {
		if (done_kind[s->down[i].kind] || s->down[i].kind == RESET) {
			(void)do_step(s, &s->down[i], false);
		}
	}
	powered = NULL;
	if (!blocks_up) {
		titan_down();   /* (with the blocks up, blocks_down() does it) */
	}
}

static bool
power_up(const struct sensor *s)
{
	memset(done_kind, 0, sizeof(done_kind));
	powered = s;
	/* (reset low first, as Android's pinctrl "cam_suspend" leaves it, before any supply) */
	pin_set(s->reset, CFG_DRIVE_MA(2) | CFG_OUTPUT, 0);
	if (!blocks_up && !titan_up()) {
		return false;
	}
	for (unsigned i = 0; i < s->nup; i++) {
		if (!do_step(s, &s->up[i], true)) {
			return false;
		}
	}
	return true;
}

/* ---- the steps ---------------------------------------------------------------------------------------- */

static void
pins_show(const struct sensor *s)
{
	say("camera: %s (%s):\n", s->name, s->chip);
	pin_show(s->sda, "I2C data");
	pin_show(s->scl, "I2C clock");
	pin_show(s->mclk_pin, "MCLK");
	pin_show(s->reset, "reset");
	if (s->vana.gpio != 0) {
		pin_show(s->vana.gpio, "VANA enable");
	}
}

static void
supplies_show(void)
{
	static const char *const names[] = { "bobb2", "ldob9", "ldob10", "ldoc2", "ldoe1", "ldom1", "ldom2", "ldom3", "ldom4", "ldom5",
		                             "ldom6", "ldom7" };

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uint32_t a = rpmh_address(names[i]);

		say("camera:   %-6s RPMh %#07x%s\n", names[i], a, a == 0 ? ": NOT in the command database" :
		    (a >> 16 & 0xf) != 4 ? ": not a regulator's address" : "");
	}
}

static void
probe(const struct sensor *s)
{
	uint8_t id[2] = { 0, 0 }, first[8];
	const char *problem = "not tried";
	bool found = false;

	announce(s->name);
	say("camera: %s (%s): power-up as its module file has it\n", s->name, s->chip);
	if (!power_up(s)) {
		give_up("the power-up failed");
		power_down();
		return;
	}
	i2c_take(s);
	say("camera: %s: I2C on gpio%u/gpio%u: data reads %u, clock reads %u (both should be 1: the pull-ups)\n", s->name, s->sda,
	    s->scl, rd(pin(s->sda) + 4) & IO_IN, rd(pin(s->scl) + 4) & IO_IN);
	for (unsigned t = 0; t < tries && !found; t++) {
		problem = i2c_read16(s->slave, s->id_reg, id, 2);
		if (problem == NULL) {
			found = ((uint16_t)(id[0] << 8 | id[1])) == s->id;
			say("camera: %s: chip ID at %#06x of 0x%02x: %#06x%s\n", s->name, s->id_reg, s->slave, id[0] << 8 | id[1],
			    found ? " -- IT IS THERE" : " (not the one Android wants)");
		} else {
			say("camera: %s: try %u: 0x%02x: %s\n", s->name, t + 1, s->slave, problem);
			wait_ms(5);
		}
	}
	if (found) {
		for (uint16_t reg = s->id_reg & ~0xfu; reg < (s->id_reg & ~0xfu) + 0x10; reg += 8) {
			if (i2c_read16(s->slave, reg, first, 8) == NULL) {
				say("camera: %s: %#06x: %02x %02x %02x %02x %02x %02x %02x %02x\n", s->name, reg, first[0], first[1], first[2],
				    first[3], first[4], first[5], first[6], first[7]);
			}
		}
	} else if (problem != NULL) {
		/* who is on this bus at all */
		char seen[160];
		int n = 0;

		seen[0] = '\0';
		for (uint8_t a = 0x08; a < 0x78; a++) {
			i2c_start();
			if (i2c_out((uint8_t)(a << 1)) && n < (int)sizeof(seen) - 6) {
				n += snprintf(seen + n, sizeof(seen) - (size_t)n, " 0x%02x", a);
			}
			i2c_stop();
		}
		say("camera: %s: on this bus answer:%s\n", s->name, n ? seen : " nobody");
	}
	i2c_give_back();
	say("camera: %s: %s\n", s->name, found ? "FOUND" : "NOT FOUND");
	if (!keep) {
		power_down();
	} else {
		say("camera: %s: left powered (cam_keep=1) until the run's end\n", s->name);
	}
}

/* ---- cam=3: the camera's own blocks, a first look ------------------------------------------------- */

/*
 * The way a frame takes to memory on SM8650 (Titan 780, "qcom,csid-lite880" and "qcom,vfe-lite880" in
 * the phone's device tree, Linux's camss-csid-gen3.c and camss-vfe-gen3.c): CSIPHY -> CSID-lite 0 ->
 * IFE-lite 0's write master -> the SMMU -> memory, or the test pattern generator (TPG 0, "qcom,cam-tpg104")
 * in place of the CSIPHY. First their clocks, every root at the crystal's 19.2 MHz (each RCG of camcc's
 * parent map 0 has it as its source 0: slow, but enough for a look and no PLL to start), and each block's
 * version read. Nothing of the blocks is written yet.
 */
#define CPAS_TOP_PHYS           0x0ac13000UL    /* qcom,cam-cpas: "cam_cpas_top" */
#define CSID_LITE0_PHYS         0x0accb000UL    /* qcom,csid-lite0 (reg 0xaccb000, 0xa00) */
#define IFE_LITE0_PHYS          0x0accc000UL    /* qcom,ife-lite0's registers past the CSID's; its bus at +0x200 */
#define TPG0_PHYS               0x0acf6000UL    /* qcom,tpg13 */
#define CSID0_PHYS              0x0acb8000UL    /* qcom,csid0: "qcom,csid880", reg "csid" 0xacb8000 (0xd00) ... */
#define CSID_TOP_PHYS           0x0acb6000UL    /* ... and "csid_top" 0xacb6000 */
#define IFE0_PHYS               0x0ac62000UL    /* qcom,ife0: "qcom,vfe880", reg "ife" 0xac62000 (0xf000) */
#define GCC_CAMERA_HF_AXI_CBCR  0x010           /* hardware-gated (BRANCH_HALT_SKIP in gcc-sm8650.c) */

struct cc_rcg {
	const char *    name;
	uint32_t        rcg;
};
static const struct cc_rcg block_rcgs[] = {
	{ "slow_ahb", 0x10034 }, { "fast_ahb", 0x10018 }, { "camnoc_axi_rt", 0x1325c }, { "ife_lite", 0x13000 },
	{ "ife_lite_csid", 0x13028 }, { "cphy_rx", 0x1104c }, { "csid", 0x13238 },
};
struct cc_branch {
	const char *    name;
	uint32_t        cbcr;
};
static const struct cc_branch block_branches[] = {
	{ "core_ahb", 0x132d0 }, { "cpas_ahb", 0x13220 }, { "cpas_fast_ahb", 0x1322c }, { "camnoc_axi_rt", 0x13274 },
	{ "camnoc_dcd_xo", 0x13290 }, { "cpas_ife_lite", 0x13024 }, { "ife_lite", 0x13018 }, { "ife_lite_ahb", 0x13050 },
	{ "ife_lite_csid", 0x13040 }, { "ife_lite_cphy_rx", 0x1304c }, { "csid_csiphy_rx", 0x15100 },
	{ "camnoc_axi_nrt", 0x13284 }, { "qdss_debug_xo", 0x132b8 },
};
#define BLOCK_BRANCHES          (sizeof(block_branches) / sizeof(block_branches[0]))
static bool branch_ours[BLOCK_BRANCHES], hf_axi_ours;

/*
 * What Linux holds for the camera besides its clocks: the MMCX rail and two bus paths, "ahb" (the CPU
 * to the camera's registers: config NoC, BCM CN0) and "hf_mnoc" (the camera's NoC to memory: MM0/MM1).
 * But a vote through our DRV (the application processors') REPLACES what the boot loader voted
 * through the same DRV for the display, which scans out by MMCX and MM0/MM1: a small vote, or one
 * taken back, leaves the panel dark (an earlier version did that). So: MMCX and MM0/MM1 are not
 * touched (the display holds them up), and CN0, without which the camera's registers do not answer,
 * gets the largest vote and keeps it until the run's end, as the shared supplies do.
 */
#define BCM_VOTE(commit, valid, x, y)   ((commit) << 30 | (valid) << 29 | (x) << 14 | (y))
#define BCM_MAX                 0x3fffu
static bool cn0_voted;

static bool
camera_votes(bool on)
{
	uint32_t a, d = BCM_VOTE(1u, 1u, BCM_MAX, BCM_MAX);

	if (!on || cn0_voted) {
		return true;            /* (nothing taken back: see above) */
	}
	a = rpmh_address("CN0");
	if (a == 0) {
		say("camera: cmd-db has no BCM CN0\n");
		return false;
	}
	if (!rpmh_write(&a, &d, 1)) {
		say("camera: bus BCM CN0 (RPMh %#x): vote NOT TAKEN\n", a);
		return false;
	}
	cn0_voted = true;
	say("camera: bus BCM CN0 (RPMh %#x): the largest vote, kept; MMCX and MM0/MM1 left to the display\n", a);
	return true;
}

static bool
rcg_at_xo(const struct cc_rcg *r)
{
	vm_offset_t a = camcc + r->rcg;
	uint32_t last;

	say("camera:   %-14s RCG %#07x: cmd %#x cfg %#x", r->name, r->rcg, rd(a), rd(a + 4));
	if ((rd(a + 4) & 0x7ff) != 0) {         /* (another source or divider: left as it is, it runs already) */
		say(", left as it is\n");
		return true;
	}
	wr(a, rd(a) | RCG_CMD_UPDATE);
	if (!wait_for(a, RCG_CMD_UPDATE, 0, 2000, &last)) {
		say(", DOES NOT TAKE IT (%#x)\n", last);
		return false;
	}
	say(", 19.2 MHz\n");
	return true;
}

static void
peek(const char *what, vm_offset_t a)
{
	say("camera: reading %s...\n", what);
	wait_ms(20);            /* (time for the line to get out, should the read never come back) */
	say("camera:   %s: %#x\n", what, rd(a));
}

static void capture_down(void);
static bool fast_clocks_up(void);
static void ife_show(const char *when);
static void fast_clocks_down(void);
static void csiphy_down(void);

static void
blocks_down(void)
{
	if (!blocks_up) {
		return;
	}
	capture_down();
	fast_clocks_down();
	for (unsigned i = BLOCK_BRANCHES; i-- > 0;) {
		if (branch_ours[i]) {
			wr(camcc + block_branches[i].cbcr, rd(camcc + block_branches[i].cbcr) & ~CBCR_ENABLE);
			branch_ours[i] = false;
		}
	}
	if (hf_axi_ours) {
		wr(gcc_camera + GCC_CAMERA_HF_AXI_CBCR, rd(gcc_camera + GCC_CAMERA_HF_AXI_CBCR) & ~CBCR_ENABLE);
		hf_axi_ours = false;
	}
	blocks_up = false;
	titan_down();
	(void)camera_votes(false);
	say("camera: the blocks' clocks off again\n");
}

static vm_offset_t cam_cpas, cam_csid, cam_ife, cam_tpg, cam_csid_top, csid0, ife0;

/* votes, the top power domain, the blocks' clocks at 19.2 MHz; false: everything taken back */
static bool
blocks_clocks_up(void)
{
	announce("the camera's blocks' clocks");
	if (!camera_votes(true)) {
		give_up("the camera's rail and bus votes");
		(void)camera_votes(false);
		return false;
	}
	if (!titan_up()) {
		give_up("the camera's top power domain");
		titan_down();
		(void)camera_votes(false);
		return false;
	}
	blocks_up = true;
	if (!(rd(gcc_camera + GCC_CAMERA_HF_AXI_CBCR) & CBCR_ENABLE)) {
		wr(gcc_camera + GCC_CAMERA_HF_AXI_CBCR, rd(gcc_camera + GCC_CAMERA_HF_AXI_CBCR) | CBCR_ENABLE);
		hf_axi_ours = true;
	}
	say("camera: GCC's camera HF AXI clock %#x\n", rd(gcc_camera + GCC_CAMERA_HF_AXI_CBCR));
	for (unsigned i = 0; i < sizeof(block_rcgs) / sizeof(block_rcgs[0]); i++) {
		if (!rcg_at_xo(&block_rcgs[i])) {
			give_up("a root clock that does not take its configuration");
			blocks_down();
			return false;
		}
	}
	for (unsigned i = 0; i < BLOCK_BRANCHES; i++) {
		vm_offset_t c = camcc + block_branches[i].cbcr;

		branch_ours[i] = !(rd(c) & CBCR_ENABLE);
		if (!branch_on(c, block_branches[i].name)) {
			give_up("a clock of the camera's blocks that does not come on");
			blocks_down();
			return false;
		}
		say("camera:   %-16s %#07x: %#x\n", block_branches[i].name, block_branches[i].cbcr, rd(c));
	}
	if (cam_cpas == 0) {
		cam_cpas = ml_io_map(CPAS_TOP_PHYS, 0x1000);
		cam_csid = ml_io_map(CSID_LITE0_PHYS, 0x1000);
		cam_ife = ml_io_map(IFE_LITE0_PHYS, 0x2000);
		cam_tpg = ml_io_map(TPG0_PHYS, 0x1000);
	}
	return true;
}

static void
blocks_look(void)
{
	if (!blocks_clocks_up()) {
		return;
	}
	/* (one read at a time, each announced first) */
	peek("CPAS top version", cam_cpas);
	peek("CPAS top +4", cam_cpas + 4);
	peek("CSID-lite 0 version", cam_csid);
	peek("IFE-lite 0 version", cam_ife);
	peek("IFE-lite 0 bus version", cam_ife + 0x200);
	peek("TPG 0 version", cam_tpg);
	blocks_down();
}

/* ---- cam=4: the test pattern generator's frames through CSID-lite 0 and IFE-lite 0 into memory ---------- */

/*
 * The way Linux's camss streams a test pattern on Titan 780 (camss-tpg-gen1.c, camss-csid-gen3.c,
 * camss-vfe-gen3.c): TPG 0 ("qcom,cam-tpg104", version 2.2) makes RAW10 colour bars on four lanes,
 * CSID-lite 0 takes them through its RX's test pattern mux and sends them down RDI 0 as they come
 * (payload only, MIPI packing), and IFE-lite 0's write master 0 (the bus at +0x200) writes each frame
 * whole (frame mode) through the SMMU, as stream 0x800 with mask 0x20 (dt: msm_cam_smmu_ife), into a
 * context of ours with page tables of ours over one piece of memory that the loader hands to the host
 * after the run (xl camera; tools/camraw.py makes pictures of it). On Titan gen3 the register update
 * and the "buffer done" are the CSID's, not the IFE's. Every clock stays at 19.2 MHz: slow, but a
 * 640x480 frame is small. Everything is taken back in the reverse order, also on any fault.
 */
#define CAM_HEADER_BYTES        4096
#define CAM_FRAMES              2
#define TPG_WIDTH               640
#define TPG_HEIGHT              480
#define TPG_LANES               4
#define RAW10_DT                0x2b
#define FRAME_ADDRESSES         0x40000000ull   /* the IFE's addresses for the memory (in dt's iova range) */
#define IFE_STREAM              0x800
#define IFE_STREAM_MASK         0x20

/* CSID gen3 (camss-csid-gen3.c), the lite one */
#define CSID_RST_CFG            0x0c
#define CSID_RST_CMD            0x10
#define CSID_IRQ_CMD            0x14
#define CSID_RUP_AUP_CMD        0x18
#define CSID_TOP_IRQ_STATUS     0x7c
#define CSID_TOP_IRQ_MASK       0x80
#define CSID_TOP_IRQ_CLEAR      0x84
#define CSID_BUF_DONE_STATUS    0x8c
#define CSID_BUF_DONE_MASK      0x90
#define CSID_BUF_DONE_CLEAR     0x94
#define CSID_RX_IRQ_STATUS      0x9c
#define CSID_RX_IRQ_MASK        0xa0
#define CSID_RX_IRQ_CLEAR       0xa4
#define CSID_RDI0_IRQ_STATUS    0xec
#define CSID_RDI0_IRQ_CLEAR     0xf4
#define CSID_RX_CFG0            0x200
#define CSID_RX_CFG1            0x204
#define CSID_RDI0_CFG0          0x500
#define CSID_RDI0_CTRL          0x504
#define CSID_RDI0_CFG1          0x510
#define CSID_RDI0_SUB_PATTERN   0x548
#define CSID_RDI0_SUB_PERIOD    0x54c
/*
 * the virtual channel RDI 0 takes (cam_vc): mode 9 sends the picture (1920x1080) on VC 0 and PDAF (1920x540) on
 * VC 1, both RAW10; with the RX in EPD mode both frame properly (without it VC 0 seemed to have one frame start)
 */
static uint32_t cam_vc = 0, cam_epd = 1;
static bool rdi_smooth = true;          /* RDI 0's line smoothing (cfg1 bit 10, full path); cam_smooth=0: off */
static uint32_t cam_smooth_flip = 1;    /* cam_smooth_flip=0: a retry keeps the smoothing as it was */
static uint32_t cam_rec = 1u | 8u;      /* RDI 0's err_recovery_cfg0 (cam_rec=0: no overflow recovery) */
static uint32_t buf_done_rdi0 = 1u << 1;        /* the lite CSID's: RDI n at bit 1 + n; the full one's: any (logged) */
#define BUF_DONE_RDI0           buf_done_rdi0

/* IFE-lite 780's bus (camss-vfe-gen3.c), write master 0 = RDI 0 */
/*
 * The bus and the write master for RDI 0: IFE-lite 0's (bus at 0x200, write master 0 at 0x400, its address
 * status at +0x74) or, for cam=5 on the full path, IFE 0's (cam_vfe880.h: bus at 0xc00, client 23 "RDI0" at
 * 0x2500, its address status at +0x78)
 */
static uint32_t ife_bus = 0x200, ife_wm = 0x400, ife_wm_status = 0x74;
static uint32_t ife_top[5] = { 0x1c, 0x20, 0x44, 0x48, 0x54 };  /* irq status 0 1, diag 0 1, violation */
static uint32_t wm_lines;               /* 0: frame mode (the lite's); else line mode, this many lines */
static uint32_t wm_stride;              /* (line mode's stride in bytes) */
static uint32_t buf_lines;              /* the lines of a frame's place in memory */
static bool full_path;                  /* cam=5: CSID 0 and IFE 0 in place of the lite ones */
#define BUS                     ife_bus
#define BUS_CGC_OVERRIDE        (BUS + 0x08)
#define BUS_TEST_CTRL           (BUS + 0xdc)
#define WM0_CFG                 (ife_wm + 0x00)
#define WM0_IMAGE_ADDR          (ife_wm + 0x04)
#define WM0_FRAME_INCR          (ife_wm + 0x08)
#define WM0_IMAGE_CFG_0         (ife_wm + 0x0c)
#define WM0_IMAGE_CFG_2         (ife_wm + 0x14)
#define WM0_PACKER_CFG          (ife_wm + 0x18)
#define WM0_SUB_PERIOD          (ife_wm + 0x30)
#define WM0_SUB_PATTERN         (ife_wm + 0x34)
#define WM0_DROP_PERIOD         (ife_wm + 0x38)
#define WM0_DROP_PATTERN        (ife_wm + 0x3c)
#define WM0_PREFETCH_CFG        (ife_wm + 0x60)
#define WM0_PREFETCH_MAX        (ife_wm + 0x64)

/*
 * TPG v1.4 (Android's camera-kernel, cam_tpg/tpg_hw/tpg_hw_v_1_4: cam_tpg104_reg), virtual channel 0,
 * data type 0. Unlike Linux's gen1 TPG, its TPG_CTRL has no enable bit: it starts by "test enable"
 * (bit 4) in TPG_CTRL_CMD (0x1f4), and stops by 0 there.
 */
#define TPG_CTRL                0x64
#define TPG_VC0_CFG0            0x68
#define TPG_VC0_LFSR_SEED       0x6c
#define TPG_VC0_HBI             0x70
#define TPG_VC0_VBI             0x74
#define TPG_VC0_COLOR_BARS      0x78
#define TPG_DT0_CFG0            0x7c
#define TPG_DT0_CFG1            0x80
#define TPG_DT0_CFG2            0x84
#define TPG_TOP_IRQ_STATUS      0x1e0
#define TPG_TOP_IRQ_MASK        0x1e4
#define TPG_TOP_IRQ_CLEAR       0x1e8
#define TPG_IRQ_CMD             0x1f0
#define TPG_CTRL_CMD            0x1f4           /* bit 0 the hardware's reset, bit 4 "test enable": the start (v1.4) */

/* the application processors' SMMU (as shennong_wlan.c's and redwood_camera.c's) */
#define SMMU_PHYS               0x15000000UL
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define CAM_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)

/* What the loader hands to the host (tools/camraw.py reads it): redwood_camera.c's header. */
struct cam_header {
	char            magic[8];       /* "RWCAM001" */
	uint32_t        width, height;
	uint32_t        stride;
	uint32_t        format;         /* 1: MIPI RAW10, packed */
	uint32_t        bayer;          /* 0 RGGB */
	uint32_t        frames;
	uint32_t        frame_bytes;
	uint32_t        source;         /* 0: a test pattern */
	char            sensor[16];
	uint32_t        last;
};

extern uint64_t loader_recording[3];
extern uint64_t kvtophys(vm_offset_t va);
extern lck_mtx_t redwood_smmu_lock;

static vm_offset_t smmu, frames, smmu_second, smmu_ctx, page_tables;
static uint64_t frames_bytes;
static uint32_t smmu_match = ~0u, smmu_context_n, smmu_old_smr, smmu_old_s2cr;
static bool smmu_on, tpg_running, csid_running, ife_running;

static void
flush_range(vm_offset_t from, uint64_t bytes, bool invalidate)
{
	for (vm_offset_t line = from & ~63UL; line < from + bytes; line += 64) {
		if (invalidate) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		} else {
			__asm__ volatile ("dc cvac, %0" : : "r" (line));
		}
	}
	__asm__ volatile ("dsb sy");
}

static bool
frames_alloc(uint32_t width, uint32_t height, uint32_t stride, const char *sensor, uint32_t bayer)
{
	struct cam_header *h;
	uint32_t frame_bytes = stride * height;

	frames_bytes = round_page(CAM_HEADER_BYTES + (uint64_t)CAM_FRAMES * frame_bytes) + DIAG_BYTES;     /* (cam=5's lines at the end) */
	if (kmem_alloc_contig(kernel_map, &frames, frames_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory in one piece for %u frames (%llu KB)\n", CAM_FRAMES, frames_bytes >> 10);
		frames = 0;
		return false;
	}
	h = (struct cam_header *)frames;
	memcpy(h->magic, "RWCAM001", 8);
	h->width = width;
	h->height = height;
	h->stride = stride;
	h->format = 1;
	h->bayer = bayer;
	h->frame_bytes = frame_bytes;
	h->source = strcmp(sensor, "TPG 0") == 0 ? 0 : 1;
	strlcpy(h->sensor, sensor, sizeof(h->sensor));
	/* (a mark in every frame's place, to tell what the IFE wrote from what it did not) */
	for (unsigned f = 0; f < CAM_FRAMES; f++) {
		memset((void *)(frames + CAM_HEADER_BYTES + (vm_offset_t)f * frame_bytes), 0x5a, 64);
	}
	flush_range(frames, frames_bytes, true);
	loader_recording[0] = kvtophys(frames);
	loader_recording[1] = frames_bytes;
	loader_recording[2] = (uint64_t)frames;
	say("camera: %llu KB for %u frames at %#llx, handed to the loader for the host (xl camera)\n", frames_bytes >> 10,
	    CAM_FRAMES, loader_recording[0]);
	return true;
}

static bool
smmu_put(vm_offset_t base, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("camera: SMMU %#x <- %#x, but it reads %#x\n", offset, value, rd(base + offset));
		return false;
	}
	return true;
}

static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1);
	uint32_t page = idr1 >> 31 ? 0x10000 : 0x1000, pages = 2u << (idr1 >> 28 & 7);

	if (rd(smmu) & 1u << 3) {
		say("camera: the SMMU numbers its streams with 16 bits, which nothing here is written for\n");
		return false;
	}
	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if ((((smr & 0x7fff) ^ IFE_STREAM) & ~((smr >> 16 & 0x7fff) | IFE_STREAM_MASK)) == 0) {
				say("camera: SMMU match register %u (%#x, s2cr %#x) is somebody's for the IFE's streams: left alone\n", n, smr,
				    s2cr);
				return false;
			}
		} else if (smmu_match == ~0u) {
			smmu_match = n;
			smmu_old_smr = smr;
			smmu_old_s2cr = s2cr;
		}
	}
	for (smmu_context_n = 0; smmu_context_n < (idr1 & 0xff) && used[smmu_context_n]; smmu_context_n++) {
	}
	if (smmu_match == ~0u || smmu_context_n >= (idr1 & 0xff)) {
		say("camera: the SMMU has no match register or context left\n");
		smmu_match = ~0u;
		return false;
	}
	smmu_second = ml_io_map(SMMU_PHYS + page, 0x1000);
	smmu_ctx = ml_io_map(SMMU_PHYS + (vm_offset_t)(pages + smmu_context_n) * page, 0x1000);
	if (rd(smmu_ctx + SMMU_CB_SCTLR) & 1) {
		say("camera: SMMU context %u is switched on although no stream uses it: somebody's\n", smmu_context_n);
		smmu_match = ~0u;
		return false;
	}
	return true;
}

/* The frames' memory behind FRAME_ADDRESSES for the IFE's streams, through a free context with tables of ours. */
static bool
smmu_up(void)
{
	uint64_t *root, *second, table_root;
	uint32_t thirds = (uint32_t)((frames_bytes + 0x1fffff) >> 21), pages = (uint32_t)(frames_bytes >> 12);
	bool ok;

	if (smmu == 0) {
		smmu = ml_io_map(SMMU_PHYS, 0x1000);
	}
	lck_mtx_lock(&redwood_smmu_lock);
	if (!smmu_find()) {
		lck_mtx_unlock(&redwood_smmu_lock);
		give_up("no place in the SMMU");
		return false;
	}
	if (kmem_alloc(kernel_map, &page_tables, (2 + thirds) * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		smmu_match = ~0u;
		lck_mtx_unlock(&redwood_smmu_lock);
		give_up("no memory for page tables");
		return false;
	}
	root = (uint64_t *)page_tables;
	second = (uint64_t *)(page_tables + 4096);
	root[FRAME_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		uint64_t *third = (uint64_t *)(page_tables + (2 + t) * 4096);

		second[(FRAME_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < pages; p++) {
			third[p] = kvtophys(frames + 4096ull * (t * 512 + p)) | CAM_PTE;
		}
	}
	flush_range(page_tables, (2 + thirds) * 4096ull, true);
	table_root = kvtophys(page_tables) | (uint64_t)smmu_context_n << 48;
	announce("the SMMU: a context of ours for the IFE's streams");
	say("camera: SMMU: context %u gets our tables for %llu KB at %#llx; match register %u sends streams %#x/%#x through it\n",
	    smmu_context_n, frames_bytes >> 10, FRAME_ADDRESSES, smmu_match, IFE_STREAM, IFE_STREAM_MASK);
	ok = smmu_put(smmu_ctx, SMMU_CB_SCTLR, 0, ~0u) && smmu_put(smmu_second, SMMU_CBA2R(smmu_context_n), 1, 1) &&
	    smmu_put(smmu_second, SMMU_CBAR(smmu_context_n), SMMU_CBAR_STAGE_1, 0x30000) &&
	    smmu_put(smmu_ctx, SMMU_CB_TCR2, SMMU_TCR2, 0) && smmu_put(smmu_ctx, SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) &&
	    smmu_put(smmu_ctx, SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) &&
	    smmu_put(smmu_ctx, SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) &&
	    smmu_put(smmu_ctx, SMMU_CB_TTBR1, 0, ~0u) && smmu_put(smmu_ctx, SMMU_CB_TTBR1 + 4, 0, ~0u) &&
	    smmu_put(smmu_ctx, SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) && smmu_put(smmu_ctx, SMMU_CB_MAIR1, 0, ~0u);
	if (ok) {
		uint32_t last;

		wr(smmu_ctx + SMMU_CB_FSR, SMMU_FSR_ALL);
		wr(smmu_ctx + SMMU_CB_TLBIASID, smmu_context_n);
		wr(smmu_ctx + SMMU_CB_TLBSYNC, 1);
		ok = wait_for(smmu_ctx + SMMU_CB_TLBSTATUS, 1, 0, 10000, &last);
		if (!ok) {
			say("camera: the SMMU does not finish forgetting (%#x)\n", last);
		}
	}
	ok = ok && smmu_put(smmu_ctx, SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1);
	smmu_on = true;
	ok = ok && smmu_put(smmu, SMMU_S2CR(smmu_match), smmu_context_n, 0x300ff) &&
	    smmu_put(smmu, SMMU_SMR(smmu_match), 1u << 31 | (uint32_t)IFE_STREAM_MASK << 16 | IFE_STREAM, ~0u);
	lck_mtx_unlock(&redwood_smmu_lock);
	if (!ok) {
		give_up("an SMMU register that did not read back");
	}
	return ok;
}

static void
smmu_faults(void)
{
	uint32_t status, global;

	if (!smmu_on) {
		return;
	}
	status = rd(smmu_ctx + SMMU_CB_FSR);
	global = rd(smmu + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0) {
		say("camera: the SMMU has refused the IFE nothing (context %u; global status %#x)\n", smmu_context_n, global);
		return;
	}
	say("camera: THE SMMU HAS REFUSED A REQUEST: context %u status %#x, address %#llx, syndrome %#x; global status %#x, stream %#x\n",
	    smmu_context_n, status, (uint64_t)rd(smmu_ctx + SMMU_CB_FAR + 4) << 32 | rd(smmu_ctx + SMMU_CB_FAR),
	    rd(smmu_ctx + SMMU_CB_FSYNR0), global, rd(smmu + SMMU_SGFSYNR1));
}

static void
smmu_down(void)
{
	if (smmu_match == ~0u) {
		return;
	}
	smmu_faults();
	if (!leaving) {
		lck_mtx_lock(&redwood_smmu_lock);
	}
	wr(smmu + SMMU_SMR(smmu_match), smmu_old_smr);
	wr(smmu + SMMU_S2CR(smmu_match), smmu_old_s2cr);
	if (smmu_on) {
		wr(smmu_ctx + SMMU_CB_SCTLR, 0);
		wr(smmu_ctx + SMMU_CB_TLBIASID, smmu_context_n);
		wr(smmu_ctx + SMMU_CB_TLBSYNC, 1);
	}
	if (!leaving) {
		lck_mtx_unlock(&redwood_smmu_lock);
	}
	smmu_on = false;
	smmu_match = ~0u;
	say("camera: the SMMU's match register and context put back as they were found\n");
}

/* A register of the camera's blocks: written, and read back where it keeps what it is given. */
static bool
blk_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, bool check)
{
	wr(base + offset, value);
	__asm__ volatile ("dsb sy");
	if (check && rd(base + offset) != value) {
		/* (noted, and gone on: a block's register may keep fewer bits than it is given) */
		say("camera: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
	}
	return true;
}

/* csid_reset() of camss-csid-gen3.c, polled: the top interrupt's reset-done bit */
static bool
csid_reset(void)
{
	uint32_t last;

	announce("CSID-lite 0: its reset");
	wr(cam_csid + CSID_TOP_IRQ_CLEAR, 1);
	wr(cam_csid + CSID_IRQ_CMD, 1);
	wr(cam_csid + CSID_TOP_IRQ_MASK, 1);
	wr(cam_csid + CSID_BUF_DONE_CLEAR, BUF_DONE_RDI0);
	wr(cam_csid + CSID_IRQ_CMD, 1);
	wr(cam_csid + CSID_BUF_DONE_MASK, BUF_DONE_RDI0);
	wr(cam_csid + CSID_RST_CFG, 1u << 4 | 1u << 0);        /* RST_LOCATION | RST_MODE */
	wr(cam_csid + CSID_RST_CMD, 1u << 2 | 1u << 0);        /* SELECT_IRQ_RST | SELECT_HW_RST */
	if (!wait_for(cam_csid + CSID_TOP_IRQ_STATUS, 1, 1, 100000, &last)) {
		say("camera: CSID-lite 0's reset does not finish (top irq %#x)\n", last);
		give_up("a CSID that does not reset");
		return false;
	}
	wr(cam_csid + CSID_TOP_IRQ_CLEAR, 1);
	wr(cam_csid + CSID_IRQ_CMD, 1);
	say("camera: CSID-lite 0 reset (version %#x)\n", rd(cam_csid));
	return true;
}

/* vfe_wm_start() and vfe_wm_update() of camss-vfe-gen3.c for RDI 0 (write master 0 of the lite IFE) */
static bool
ife_config(uint64_t address, uint32_t frame_bytes)
{
	/*
	 * frame mode (width 0xffff, no height: what comes is written) on the lite IFE; on IFE 0 its RDI 0's
	 * client is line based (cam_vfe880.h line_based, cam_vfe_bus_ver3_config_rdi_wm()): the height in
	 * lines, the width in 16-byte words, the stride in bytes, enabled without the frame mode bit
	 */
	uint32_t stride = wm_stride;
	uint32_t cfg0 = wm_lines != 0 ? wm_lines << 16 | (stride + 15) / 16 : 0xffff, cfg2 = wm_lines != 0 ? stride : 0xffff;
	uint32_t en = wm_lines != 0 ? 1u : 1u << 0 | 1u << 16;

	announce(full_path ? "IFE 0: write master 23 (RDI 0)" : "IFE-lite 0: write master 0 for RDI 0");
	if (!blk_put(cam_ife, "IFE", BUS_CGC_OVERRIDE, 0x7ffffff, false) || !blk_put(cam_ife, "IFE", BUS_TEST_CTRL, 0, false) ||
	    !blk_put(cam_ife, "IFE", WM0_FRAME_INCR, frame_bytes >> 8, true) ||
	    !blk_put(cam_ife, "IFE", WM0_IMAGE_CFG_0, cfg0, true) || !blk_put(cam_ife, "IFE", WM0_IMAGE_CFG_2, cfg2, true) ||
	    !blk_put(cam_ife, "IFE", WM0_PACKER_CFG, 0, true) || !blk_put(cam_ife, "IFE", WM0_DROP_PERIOD, 0, true) ||
	    !blk_put(cam_ife, "IFE", WM0_DROP_PATTERN, 1, true) || !blk_put(cam_ife, "IFE", WM0_SUB_PERIOD, 0, true) ||
	    !blk_put(cam_ife, "IFE", WM0_SUB_PATTERN, 1, true) || !blk_put(cam_ife, "IFE", WM0_PREFETCH_CFG, 1, false) ||
	    !blk_put(cam_ife, "IFE", WM0_PREFETCH_MAX, 0xffffffff, false) ||
	    !blk_put(cam_ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)(address >> 8), false)) {
		return false;
	}
	ife_running = true;
	if (!blk_put(cam_ife, "IFE", WM0_CFG, en, false)) {
		return false;
	}
	/* the write master's constraint error detection */
	return blk_put(cam_ife, "IFE", ife_wm + ife_wm_status + 0x10, 11u << 8, false);
}

/*
 * __csid_configure_rdi_stream() and __csid_configure_rx() of camss-csid-gen3.c: RDI 0 from TPG 0 (rx_cfg0 == 0), or
 * from a PHY (rx_cfg0 given), RAW10 on vc 0
 */
/*
 * the CSID's RX from the sensor's PHY: the wide's three C-PHY trios (lane map 0x210, PHY_TYPE_SEL 1), or four
 * D-PHY lanes (lane map 0x3210, PHY_TYPE_SEL 0); PHY_NUM_SEL the PHY's number + 1
 */
#define RX_CFG0_SEL             (DPHY ? 3u | 0x3210u << 4 | (PHY_N + 1) << 20 : 2u | 0x210u << 4 | (PHY_N + 1) << 20 | 1u << 24)

static bool
csid_config(uint32_t rx_cfg0, uint32_t rx_cfg1)
{
	uint32_t cfg0 = 1u << 6 | 1u << 8 | 0xfu << 12 | (uint32_t)RAW10_DT << 16 | 0u << 22 | 0u << 27;
	uint32_t cfg1 = 1u << 15 | 1u << 10 | 1u << 5 | 1u << 6 | 1u << 7 | 1u << 8;
	bool sensor = rx_cfg0 != 0;

	announce(rx_cfg0 == 0 ? "CSID-lite 0: RDI 0 from the test pattern generator" : full_path ? "CSID 0: RDI 0 from a CSIPHY" :
	    "CSID-lite 0: RDI 0 from a CSIPHY");
	if (rx_cfg0 == 0) {
		/* the RX: four lanes in order, the test pattern mux on, TPG 0 (its select is the TPG's number + 1) */
		rx_cfg0 = (TPG_LANES - 1) | 0x3210u << 4 | 1u << 27 | 1u << 28;
		rx_cfg1 = 1;
	} else {
		/*
		 * as cam_ife_csid_ver2_init_config_rdi_path() for a lite 780's RDI: the timestamp with strobe 2 in
		 * cfg0; in cfg1 only the MIPI packing -- no crop, no drop (enabled without a window, they cut the
		 * first line short and the CSID stopped the path: CCIF violation); the overflow recovery on
		 */
		cfg0 = 1u << 6 | 2u << 8 | 0xfu << 12 | (uint32_t)RAW10_DT << 16 | (cam_vc & 0x1f) << 22;
		/*
		 * the RX's cfg1 as cam_ife_csid_ver2_init_config_rx() writes it: ECC correction, the MISR, and the
		 * EPD mode (bit 8) for a C-PHY sensor that sends its packets with embedded delimiters, without LP
		 * states between them -- the CSID's constant ERROR_CPHY_EOT_RECEPTION (rx irq bit 11) is what
		 * Android ignores "for EPD sensor"; without the mode the packets' ends are not seen (cam_epd=0: off)
		 */
		rx_cfg1 = 1u | 1u << 6 | (cam_epd && !DPHY ? 1u << 8 : 0);
		cfg1 = 1u << 15;
		if (full_path && S5K && cam_crop) {
			/*
			 * (the S5KJN1: RDI 0 cropped to the picture -- cfg1 bits 7 and 8, hcrop 0x550 and vcrop 0x554 as
			 * end << 16 | start: each frame brought a piece of about 512 pixels more, which shifted the
			 * frames after it; cam_crop=0: off)
			 */
			cfg1 |= 1u << 7 | 1u << 8;
			wr(cam_csid + 0x550, (cam_crop_x0 + cam_width - 1) << 16 | cam_crop_x0);
			wr(cam_csid + 0x554, (cam_crop_y0 + cam_height - 1) << 16 | cam_crop_y0);
			if (!announce_quiet) {
				say("camera: CSID: RDI 0 cropped: pixels %u..%u (%#x), lines %u..%u (%#x)\n", cam_crop_x0,
				    cam_crop_x0 + cam_width - 1, rd(cam_csid + 0x550), cam_crop_y0, cam_crop_y0 + cam_height - 1,
				    rd(cam_csid + 0x554));
			}
		}
		if (full_path) {
			/*
			 * the full CSID 880's RDI 0 smooths its lines (CAP_LINE_SMOOTHING_IN_RDI: pix_store_en,
			 * cfg1 bit 10 -- "we use line smoothing only on RDI_0 in all usecases"); and its top
			 * (csid_top 0xacb6000, io_path_cfg0 of CSID 0 at +0): input core "internal" (IFE, 1),
			 * out_ife_en (bit 8), as cam_ife_csid_ver2_program_top() for CAM_IFE_CSID_INPUT_CORE_IFE
			 */
			if (rdi_smooth) {
				cfg1 |= 1u << 10;
			}
			wr(cam_csid_top, 1u | 1u << 8);
		}
		if (full_path) {
			/* the format measure (cfg1 bit 3; its cfg0: pixel and line counts, hbi and vbi counts) */
			wr(cam_csid + 0x580, 0xf);
			wr(cam_csid + 0x584, 0);
			__asm__ volatile ("dsb sy");
		}
	}
	if (!blk_put(cam_csid, "CSID", CSID_RDI0_CFG0, cfg0, true) ||
	    !blk_put(cam_csid, "CSID", CSID_RDI0_CFG1, cfg1, true) ||
	    (sensor && !blk_put(cam_csid, "CSID", 0x514, cam_rec, true)) ||
	    !blk_put(cam_csid, "CSID", CSID_RDI0_SUB_PERIOD, 0, true) || !blk_put(cam_csid, "CSID", CSID_RDI0_SUB_PATTERN, 1, true) ||
	    !blk_put(cam_csid, "CSID", CSID_RDI0_CTRL, 0, false) || !blk_put(cam_csid, "CSID", CSID_RDI0_CFG0, cfg0 | 1u << 31, true) ||
	    !blk_put(cam_csid, "CSID", CSID_RX_CFG0, rx_cfg0, true) || !blk_put(cam_csid, "CSID", CSID_RX_CFG1, rx_cfg1, true)) {
		return false;
	}
	csid_running = true;
	if (!blk_put(cam_csid, "CSID", CSID_RDI0_CTRL, 1, false)) {
		return false;
	}
	/* the register and address update for RDI 0 (gen3: the CSID's, for the IFE's write master too) */
	return blk_put(cam_csid, "CSID", CSID_RUP_AUP_CMD, 1u << 4 | 1u << 20, false);
}

/* tpg_stream_on() of camss-tpg-gen1.c: colour bars, RAW10, one virtual channel and data type, frames without end */
static bool
tpg_start(void)
{
	uint32_t version = rd(cam_tpg), enc_shift = version >= 0x20010000 ? 28 : 20;
	uint32_t hbi = (TPG_WIDTH * 10 * 545 + 8 * TPG_LANES * 100 - 1) / (8 * TPG_LANES * 100), vbi = TPG_HEIGHT * 10 / 100;

	announce("TPG 0: colour bars");
	/* tpg_hw_v_1_4_reset(): control and interrupts cleared, then the hardware's reset */
	wr(cam_tpg + TPG_CTRL, 0);
	wr(cam_tpg + TPG_TOP_IRQ_MASK, 0);
	wr(cam_tpg + TPG_TOP_IRQ_CLEAR, 1);
	wr(cam_tpg + TPG_IRQ_CMD, 1);
	wr(cam_tpg + TPG_CTRL_CMD, 1);
	spin_us(100);
	if (!blk_put(cam_tpg, "TPG", TPG_VC0_CFG0, 0, true) || !blk_put(cam_tpg, "TPG", TPG_VC0_LFSR_SEED, 0x12345678, true) ||
	    !blk_put(cam_tpg, "TPG", TPG_VC0_HBI, hbi, true) || !blk_put(cam_tpg, "TPG", TPG_VC0_VBI, vbi, true) ||
	    !blk_put(cam_tpg, "TPG", TPG_VC0_COLOR_BARS, 0xau << 8, true) ||
	    !blk_put(cam_tpg, "TPG", TPG_DT0_CFG0, TPG_HEIGHT | (uint32_t)TPG_WIDTH << 16, true) ||
	    !blk_put(cam_tpg, "TPG", TPG_DT0_CFG1, RAW10_DT, true) ||
	    /* payload mode 8 (colour bars: Linux's mode 9 less one), user payload 0xbe, encoding 2 (10 bits) */
	    !blk_put(cam_tpg, "TPG", TPG_DT0_CFG2, 8u | 0xbeu << 4 | 2u << enc_shift, true)) {
		return false;
	}
	/* configure_global_configs(): the TPG's done and RUP-done interrupts (status only), one VC, four lanes, D-PHY; then the start */
	wr(cam_tpg + TPG_TOP_IRQ_MASK, 1u << 1 | 1u << 0);
	if (!blk_put(cam_tpg, "TPG", TPG_CTRL, (TPG_LANES - 1u) << 4, true)) {
		return false;
	}
	tpg_running = true;
	return blk_put(cam_tpg, "TPG", TPG_CTRL_CMD, 1u << 4, false);
}

static void
show_paths(const char *when)
{
	say("camera: %s: CSID top irq %#x, rx irq %#x, buf done %#x, RDI0 irq %#x, rx cfg0 %#x, RDI0 cfg0 %#x ctrl %#x; "
	    "IFE WM0 cfg %#x addr %#x; TPG status %#x ctrl %#x cmd %#x irq %#x\n", when, rd(cam_csid + CSID_TOP_IRQ_STATUS),
	    rd(cam_csid + CSID_RX_IRQ_STATUS), rd(cam_csid + CSID_BUF_DONE_STATUS), rd(cam_csid + CSID_RDI0_IRQ_STATUS),
	    rd(cam_csid + CSID_RX_CFG0), rd(cam_csid + CSID_RDI0_CFG0), rd(cam_csid + CSID_RDI0_CTRL), rd(cam_ife + WM0_CFG),
	    rd(cam_ife + WM0_IMAGE_ADDR), rd(cam_tpg + 4), rd(cam_tpg + TPG_CTRL),
	    rd(cam_tpg + TPG_CTRL_CMD), rd(cam_tpg + TPG_TOP_IRQ_STATUS));
}

/* the paths stopped, the SMMU put back (blocks_down() calls it first) */
static void
capture_down(void)
{
	csiphy_down();
	if (tpg_running) {
		wr(cam_tpg + TPG_CTRL_CMD, 0);
		wr(cam_tpg + TPG_CTRL, 0);
		wr(cam_tpg + TPG_TOP_IRQ_MASK, 0);
		tpg_running = false;
	}
	if (csid_running) {
		wr(cam_csid + CSID_RDI0_CTRL, 0);
		wr(cam_csid + CSID_RDI0_CFG0, rd(cam_csid + CSID_RDI0_CFG0) & ~(1u << 31));
		wr(cam_csid + CSID_RX_CFG0, 0);
		wr(cam_csid + CSID_BUF_DONE_MASK, 0);
		wr(cam_csid + CSID_TOP_IRQ_MASK, 0);
		if (full_path) {
			wr(cam_csid_top, 1);    /* (io_path_cfg_rst_val) */
		}
		csid_running = false;
	}
	if (ife_running) {
		wr(cam_ife + WM0_CFG, 0);
		ife_running = false;
		spin_us(1000);
	}
	smmu_down();
}

/* cam=4: a few frames of TPG 0's colour bars through CSID-lite 0's RDI 0 and IFE-lite 0 into memory */
static void
capture_tpg(void)
{
	const uint32_t stride = TPG_WIDTH * 10 / 8, frame_bytes = stride * TPG_HEIGHT;
	struct cam_header *h;
	const uint8_t *p;
	unsigned done = 0;

	if (!frames_alloc(TPG_WIDTH, TPG_HEIGHT, stride, "TPG 0", 0)) {
		return;
	}
	uint32_t fast = 0;

	if (!blocks_clocks_up()) {
		return;
	}
	/* cam_fast=1: the blocks at cam=5's rates (PLL0, the bus votes): is the way to memory as fast as it needs? */
	PE_parse_boot_argn("cam_fast", &fast, sizeof(fast));
	if (fast && !fast_clocks_up()) {
		blocks_down();
		return;
	}
	if (!smmu_up() || !csid_reset() || !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, frame_bytes) || !csid_config(0, 0)) {
		show_paths("at the fault");
		blocks_down();
		return;
	}
	show_paths("configured");
	if (!tpg_start()) {
		blocks_down();
		return;
	}
	h = (struct cam_header *)frames;
	for (unsigned ms = 0; ms < 3000; ms += 50) {
		uint32_t buf = rd(cam_csid + CSID_BUF_DONE_STATUS);

		wait_ms(50);
		if (buf & BUF_DONE_RDI0) {
			done++;
			say("camera: RDI 0's buffer done after %u ms (%u)\n", ms + 50, done);
			wr(cam_csid + CSID_BUF_DONE_CLEAR, BUF_DONE_RDI0);
			wr(cam_csid + CSID_IRQ_CMD, 1);
			if (done == 2) {
				break;
			}
		}
		if (ms % 500 == 0) {
			show_paths("streaming");
		}
	}
	show_paths("after the frames");
	ife_show("after the frames");
	smmu_faults();
	blocks_down();
	flush_range(frames, CAM_HEADER_BYTES + frame_bytes, true);
	h->frames = done != 0 ? 1 : 0;
	h->last = 0;
	p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; at its middle: %02x %02x %02x %02x "
	    "(0x5a: not written) -- buffers done: %u\n", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
	    p[frame_bytes / 2], p[frame_bytes / 2 + 1], p[frame_bytes / 2 + 2], p[frame_bytes / 2 + 3], done);
	if (p[0] != 0x5a || p[1] != 0x5a) {
		h->frames = 1;          /* (written, even if no "done" was seen: worth a look) */
	}
	flush_range(frames, CAM_HEADER_BYTES, false);
}

/* ---- cam=5: the wide sensor's frames ------------------------------------------------------------------- */

/*
 * The way of cam=4 with the sensor in place of the TPG. What more it takes:
 *
 * camcc's PLL0 (Lucid OLE, at camcc + 0: 1200 MHz; main/3 400, main/2.5 480, odd 400, even 600), as
 * clk_lucid_ole_pll_configure() and alpha_pll_lucid_evo_enable() of Linux's clk-alpha-pll.c start it,
 * and the blocks' roots on it at camcc-sm8650.c's rates (an RCG's CFG: source << 8 | (2 x divider - 1)).
 * Everything goes back to the crystal and PLL0 off again at the end (if it was we who started it).
 *
 * MMCX: our DRV's vote would REPLACE the display's (see camera_votes()), so it is voted for only when
 * cam_mmcx=N asks, and then only if N is above what our DRV's command sets show was voted last.
 *
 * CSIPHY 3 (0xacea000, "qcom,csiphy-v2.2.0"): as Android's cam_csiphy_core.c starts it for a C-PHY
 * sensor: reset, the trios' lane enable (lane assignment 0x210 of the module file: CPHY_LANE_0..2),
 * the common registers, the data rate's table (OVX9000: 3 trios at 3.5 Gsps -- its output pixel clock
 * 2394000000 x 10 bits / 3 is exactly that table's bandwidth), the three-phase table with the module
 * file's settle count 30, the interrupt masks, out of reset.
 *
 * The CSID's RX from PHY 3: three lanes, lane map 0x210, PHY_NUM_SEL 3 + 1, PHY_TYPE_SEL 1 (C-PHY).
 */
#define CSIPHY_PHYS(n)          (0x0ace4000UL + 0x2000UL * (n))         /* csiphy@ace4000 + n x 0x2000 */
static const uint32_t csiphy_cbcr[4] = { 0x150fc, 0x15120, 0x15140, 0x15160 };         /* camcc-sm8650.c */
static const uint32_t phytimer_cbcr[4] = { 0x150f8, 0x1511c, 0x1513c, 0x1515c };
static const uint32_t phytimer_rcg[4] = { 0x150e0, 0x15104, 0x15124, 0x15144 };
#define CAMCC_CSIPHY3_CBCR      csiphy_cbcr[PHY_N]
#define CAMCC_CSI3PHYTIMER_CBCR phytimer_cbcr[PHY_N]
#define PHY_RESET               0x1000
#define PHY_LANE_ENABLE         0x1014
#define PHY_IRQ_STATUS(n)       (0x10b0 + 4 * (n))
#define PHY_IRQ_CLEAR(n)        (0x1058 + 4 * (n))
#define PHY_IRQ_CMD             0x1028
#define PHY_LANES_CPHY_012      (1u << 1 | 1u << 3 | 1u << 5)
#define SENSOR_WIDTH            cam_width       /* (the sensor's output width: 0x3808; mode 9's own is 1920) */
#define SENSOR_HEIGHT           cam_height      /* (0x380a: mode 9's 1080, mode 1's 3072) */

#define PLL_MODE                0x00
#define PLL_OPMODE              0x04
#define PLL_L_VAL               0x10
#define PLL_ALPHA_VAL           0x14
#define PLL_USER_CTL            0x18
#define PLL_USER_CTL_U          0x1c
#define PLL_CONFIG_CTL          0x20
#define PLL_CONFIG_CTL_U        0x24
#define PLL_CONFIG_CTL_U1       0x28
#define PLL_TEST_CTL            0x2c
#define PLL_TEST_CTL_U          0x30
#define PLL_TEST_CTL_U1         0x34
#define PLL_TEST_CTL_U2         0x38
#define PLL_OUTCTRL             (1u << 0)
#define PLL_RESET_N             (1u << 2)
#define PLL_LOCK_DET            (1u << 31)
#define PLL_OUT_MASK            7u

static vm_offset_t csiphy;
static bool i2c_held, pll0_ours, phy_running, phy_clocks_ours[2], sensor_streaming;
struct fast_rate {
	const char *    name;
	uint32_t        rcg, cfg, was;
	bool            set;
};
static struct fast_rate fast_rates[] = {
	{ "camnoc_axi_rt", 0x1325c, 3u << 8 | 1, 0, false },       /* 400 MHz: PLL0 odd / 1 */
	{ "csi3phytimer", 0x15144, 1u << 8 | 5, 0, false },        /* 400 MHz: main / 3 */
	{ "cphy_rx", 0x1104c, 0, 0, false },                      /* (cam_mhz: 400 main / 3, 480 main / 2.5) */
	{ "csid", 0x13238, 0, 0, false },
	{ "ife_lite", 0x13000, 0, 0, false },
	{ "ife_lite_csid", 0x13028, 0, 0, false },
};
#define FAST_RATES              (sizeof(fast_rates) / sizeof(fast_rates[0]))
/*
 * cam_width (default 1600): every try before wrote 2304 bytes of the first line and no more, at 3.5, 1.75 Gsps,
 * camnoc fast or slow, while the TPG's 800-byte lines went through at the same clocks: a line of 2400 bytes
 * (1920 RAW10) seems more than IFE-lite's RDI takes. 1600 pixels are 2000 bytes.
 */
static uint32_t cam_width = 1600, cam_height = 1080;
/* the sensor's mode (cam_mode): 9 1920x1080 (the one frame's, cam_once), 1 4096x3072 (iOS's by default) */
static uint32_t cam_mode = 9;
static uint32_t cam_vts = 2768, cam_exp = 2700, cam_gain = 15, cam_mhz = 480, cam_mmcx, cam_noc = 1, cam_bus = 1;

static bool rcg_set(vm_offset_t a, uint32_t cfg);

/* a Lucid OLE PLL of camcc's (PLL n at camcc + n x 0x1000) at L and alpha; *ours: started by us */
static bool
pll_up(unsigned n, uint32_t l, uint32_t alpha, uint32_t user, bool *ours)
{
	vm_offset_t p = camcc + 0x1000 * n;
	uint32_t last;

	say("camera: camcc PLL%u: mode %#x opmode %#x l %#x alpha %#x user %#x\n", n, rd(p + PLL_MODE), rd(p + PLL_OPMODE),
	    rd(p + PLL_L_VAL), rd(p + PLL_ALPHA_VAL), rd(p + PLL_USER_CTL));
	if ((rd(p + PLL_MODE) & (PLL_OUTCTRL | PLL_LOCK_DET)) == (PLL_OUTCTRL | PLL_LOCK_DET)) {
		say("camera: camcc PLL%u runs already: left as it is\n", n);
		return (rd(p + PLL_L_VAL) & 0xffff) == l;
	}
	wr(p + PLL_L_VAL, l | 0x44u << 16 | 0x44u << 24);
	wr(p + PLL_ALPHA_VAL, alpha);
	wr(p + PLL_CONFIG_CTL, 0x20485699);
	wr(p + PLL_CONFIG_CTL_U, 0x00182261);
	wr(p + PLL_CONFIG_CTL_U1, 0x82aa299c);
	wr(p + PLL_TEST_CTL, 0);
	wr(p + PLL_TEST_CTL_U, 3);
	wr(p + PLL_TEST_CTL_U1, 0x9000);
	wr(p + PLL_TEST_CTL_U2, 0x34);
	wr(p + PLL_USER_CTL, user);
	wr(p + PLL_USER_CTL_U, 5);
	wr(p + PLL_MODE, rd(p + PLL_MODE) & ~PLL_OUTCTRL);
	wr(p + PLL_OPMODE, 0);                          /* standby */
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_RESET_N);
	*ours = true;
	wr(p + PLL_OPMODE, 1);                          /* run */
	__asm__ volatile ("dsb sy");
	if (!wait_for(p + PLL_MODE, PLL_LOCK_DET, PLL_LOCK_DET, 2000, &last)) {
		say("camera: camcc PLL%u does not lock (mode %#x)\n", n, last);
		return false;
	}
	wr(p + PLL_USER_CTL, rd(p + PLL_USER_CTL) | PLL_OUT_MASK);
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_OUTCTRL);
	__asm__ volatile ("dsb sy");
	say("camera: camcc PLL%u locked (L %#x alpha %#x): mode %#x user %#x\n", n, l, alpha, rd(p + PLL_MODE), rd(p + PLL_USER_CTL));
	return true;
}

static void
pll_down(unsigned n, bool *ours)
{
	vm_offset_t p = camcc + 0x1000 * n;

	if (!*ours) {
		return;
	}
	wr(p + PLL_MODE, rd(p + PLL_MODE) & ~PLL_OUTCTRL);
	wr(p + PLL_USER_CTL, rd(p + PLL_USER_CTL) & ~PLL_OUT_MASK);
	wr(p + PLL_OPMODE, 0);
	wr(p + PLL_MODE, rd(p + PLL_MODE) & ~PLL_RESET_N);
	*ours = false;
	say("camera: camcc PLL%u off again\n", n);
}

/* PLL0: 19.2 MHz x 62.5 = 1200 MHz (camcc-sm8650.c's cam_cc_pll0_config) */
static bool
pll0_up(void)
{
	return pll_up(0, 0x3e, 0x8000, 0x8400, &pll0_ours);
}

/*
 * IFE 0 (cam=5's full path): its own power domain (cam_cc_ife_0_gdsc 0x11004, under titan_top), its root
 * ife_0_clk_src (0x11018) on PLL3's even output (19.2 MHz x 48.54 = 932, / 2 = 466 MHz: the device tree's
 * lowsvs rate; cam_cc_pll3_config), its clocks ife_0, ife_0_fast_ahb and cpas_ife_0, and CSID 0's own
 * csid clock (cam_cc_csid_clk 0x13250, on the csid root that fast_clocks_up() sets). Taken back in turn.
 */
#define CAMCC_IFE0_GDSCR        0x11004
#define CAMCC_IFE0_RCG          0x11018
static const struct cc_branch ife0_branches[] = {
	{ "ife_0_fast_ahb", 0x11048 }, { "cpas_ife_0", 0x1103c }, { "ife_0", 0x11030 }, { "csid", 0x13250 },
};
#define IFE0_BRANCHES           (sizeof(ife0_branches) / sizeof(ife0_branches[0]))
static bool pll3_ours, ife0_gdsc_ours, ife0_rcg_set, ife0_branch_ours[IFE0_BRANCHES];
static uint32_t ife0_rcg_was;
static vm_offset_t lite_csid, lite_ife;

static bool
ife0_up(void)
{
	uint32_t last;

	announce("IFE 0: PLL3, its root at 466 MHz, its power domain and clocks");
	if (!pll_up(3, 0x30, 0x8aaa, 0x400, &pll3_ours)) {
		give_up("camcc's PLL3");
		return false;
	}
	if (!(rd(camcc + CAMCC_IFE0_GDSCR) & GDSC_PWR_ON)) {
		wr(camcc + CAMCC_IFE0_GDSCR, rd(camcc + CAMCC_IFE0_GDSCR) & ~GDSC_SW_COLLAPSE);
		ife0_gdsc_ours = true;
		if (!wait_for(camcc + CAMCC_IFE0_GDSCR, GDSC_PWR_ON, GDSC_PWR_ON, 10000, &last)) {
			say("camera: ife_0_gdsc does not come on (%#x)\n", last);
			give_up("IFE 0's power domain");
			return false;
		}
	}
	say("camera: ife_0_gdsc %s (%#x)\n", ife0_gdsc_ours ? "switched on" : "was on already", rd(camcc + CAMCC_IFE0_GDSCR));
	for (unsigned i = 0; i < IFE0_BRANCHES; i++) {
		vm_offset_t c = camcc + ife0_branches[i].cbcr;

		ife0_branch_ours[i] = !(rd(c) & CBCR_ENABLE);
		if (!branch_on(c, ife0_branches[i].name)) {
			give_up("a clock of IFE 0 or CSID 0 that does not come on");
			return false;
		}
		say("camera:   %-16s %#07x: %#x\n", ife0_branches[i].name, ife0_branches[i].cbcr, rd(c));
	}
	/*
	 * the root on PLL3 only now: an RCG takes a new configuration only while a branch of it runs (the
	 * update stayed pending, cmd 0x80000001, when it came first on a warm start)
	 */
	ife0_rcg_was = rd(camcc + CAMCC_IFE0_RCG + 4) & 0x7ff;
	ife0_rcg_set = true;
	if (!rcg_set(camcc + CAMCC_IFE0_RCG, 6u << 8 | 1)) {
		say("camera:   ife_0 RCG DOES NOT TAKE PLL3 even (cmd %#x cfg %#x)\n", rd(camcc + CAMCC_IFE0_RCG),
		    rd(camcc + CAMCC_IFE0_RCG + 4));
		give_up("IFE 0's root clock");
		return false;
	}
	say("camera:   ife_0 RCG %#07x: cfg %#x (was %#x)\n", CAMCC_IFE0_RCG, rd(camcc + CAMCC_IFE0_RCG + 4), ife0_rcg_was);
	if (csid0 == 0) {
		csid0 = ml_io_map(CSID0_PHYS, 0x1000);
		cam_csid_top = ml_io_map(CSID_TOP_PHYS, 0x1000);
		ife0 = ml_io_map(IFE0_PHYS, 0xf000);
	}
	lite_csid = cam_csid;
	lite_ife = cam_ife;
	cam_csid = csid0;
	cam_ife = ife0;
	peek("CSID 0 version", cam_csid);
	peek("CSID top io path 0", cam_csid_top);
	peek("IFE 0 version", cam_ife);
	peek("IFE 0 bus version", cam_ife + BUS);
	return true;
}

/* (fast_clocks_down() calls it) */
static void
ife0_down(void)
{
	if (ife0_rcg_set) {
		(void)rcg_set(camcc + CAMCC_IFE0_RCG, ife0_rcg_was);     /* (while its branches still run) */
		ife0_rcg_set = false;
	}
	for (unsigned i = IFE0_BRANCHES; i-- > 0;) {
		if (ife0_branch_ours[i]) {
			wr(camcc + ife0_branches[i].cbcr, rd(camcc + ife0_branches[i].cbcr) & ~CBCR_ENABLE);
			ife0_branch_ours[i] = false;
		}
	}
	if (ife0_gdsc_ours) {
		wr(camcc + CAMCC_IFE0_GDSCR, rd(camcc + CAMCC_IFE0_GDSCR) | GDSC_SW_COLLAPSE);
		ife0_gdsc_ours = false;
		say("camera: ife_0_gdsc switched off again (%#x)\n", rd(camcc + CAMCC_IFE0_GDSCR));
	}
	pll_down(3, &pll3_ours);
	if (cam_csid == csid0 && csid0 != 0) {
		cam_csid = lite_csid;
		cam_ife = lite_ife;
	}
}

static bool
rcg_set(vm_offset_t a, uint32_t cfg)
{
	uint32_t last;

	wr(a + 4, (rd(a + 4) & ~0x7ffu) | cfg);
	wr(a, rd(a) | RCG_CMD_UPDATE);
	return wait_for(a, RCG_CMD_UPDATE, 0, 2000, &last);
}

/* the most MMCX level our DRV's command sets show voted, or -1 */
static int
mmcx_seen(uint32_t address)
{
	int level = -1;

	for (unsigned t = 0; RSC_TCS_BASE + RSC_TCS_STRIDE * (t + 1) <= RSC_SIZE && t < 12; t++) {
		vm_offset_t base = rsc + RSC_TCS_BASE + RSC_TCS_STRIDE * t;

		for (unsigned c = 0; c < 16; c++) {
			if ((rd(base + tcs_reg.addr + tcs_reg.cmd_stride * c) & 0xfffff) == address) {
				uint32_t d = rd(base + tcs_reg.data + tcs_reg.cmd_stride * c);

				say("camera:   MMCX in command set %u, command %u: %u\n", t, c, d);
				if ((int)d > level && d < 16) {
					level = (int)d;
				}
			}
		}
	}
	return level;
}

/*
 * The way to memory at its most: a stream of 3 C-PHY trios at 3.5 Gsps comes in bursts of ~3 GB/s, and the
 * write master's FIFO overflowed after the first 1024 (camnoc at 19.2 MHz) or 2304 bytes (at 400 MHz) with
 * the buses as the display voted them. The largest vote for the camera's NoC to memory (MM0 and MM1 for
 * the hf and sf paths, SH0 the shared NoC, MC0 the memory controller), kept until the run's end: the
 * largest is never less than what the display voted through the same DRV, so its panel keeps its way.
 */
static bool bus_max_voted;

static void
bus_votes_max(void)
{
	static const char *const names[] = { "MM0", "MM1", "SH0", "MC0" };
	uint32_t d = BCM_VOTE(1u, 1u, BCM_MAX, BCM_MAX);

	if (bus_max_voted) {
		return;
	}
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uint32_t a = rpmh_address(names[i]);

		if (a == 0) {
			say("camera: cmd-db has no BCM %s\n", names[i]);
			continue;
		}
		say("camera: bus BCM %s (RPMh %#x): the largest vote%s\n", names[i], a, rpmh_write(&a, &d, 1) ? ", kept" : " NOT TAKEN");
	}
	bus_max_voted = true;
}

static bool
fast_clocks_up(void)
{
	uint32_t mmcx = rpmh_address("mmcx.lvl"), cfg = cam_mhz == 400 ? 1u << 8 | 5 : 1u << 8 | 4;

	announce("camcc's PLL0 and the blocks' clocks at their rates");
	if (mmcx != 0) {
		int seen = mmcx_seen(mmcx);

		say("camera: MMCX (RPMh %#x): the most our DRV shows voted is %d\n", mmcx, seen);
		if (cam_mmcx != 0 && (int)cam_mmcx > seen && cam_mmcx < 8) {
			uint32_t d = cam_mmcx;

			say("camera: MMCX: level %u voted (cam_mmcx; only ever up)%s\n", cam_mmcx,
			    rpmh_write(&mmcx, &d, 1) ? "" : ": NOT TAKEN");
		}
	}
	if (cam_bus) {
		bus_votes_max();
	}
	if (!pll0_up()) {
		give_up("camcc's PLL0");
		return false;
	}
	for (unsigned i = 0; i < FAST_RATES; i++) {
		struct fast_rate *r = &fast_rates[i];
		vm_offset_t a = camcc + r->rcg;

		if (r->cfg == 0 || i >= 2) {
			r->cfg = cfg;
		}
		if (i == 0 && !cam_noc) {
			/*
			 * (cam_noc=0: camnoc_axi_rt stays at the crystal -- the write master overflowed sooner so)
			 */
			say("camera:   %-14s RCG %#07x: left at %#x (cam_noc=0)\n", r->name, r->rcg, rd(a + 4));
			continue;
		}
		r->was = rd(a + 4) & 0x7ff;
		r->set = true;
		if (!rcg_set(a, r->cfg)) {
			say("camera:   %-14s RCG %#07x DOES NOT TAKE cfg %#x (cmd %#x)\n", r->name, r->rcg, r->cfg, rd(a));
			give_up("a root clock that does not take PLL0");
			return false;
		}
		say("camera:   %-14s RCG %#07x: cfg %#x (was %#x)\n", r->name, r->rcg, rd(a + 4), r->was);
	}
	phy_clocks_ours[0] = !(rd(camcc + CAMCC_CSI3PHYTIMER_CBCR) & CBCR_ENABLE);
	phy_clocks_ours[1] = !(rd(camcc + CAMCC_CSIPHY3_CBCR) & CBCR_ENABLE);
	if (!branch_on(camcc + CAMCC_CSI3PHYTIMER_CBCR, "csi3phytimer") || !branch_on(camcc + CAMCC_CSIPHY3_CBCR, "csiphy3")) {
		give_up("CSIPHY 3's clocks");
		return false;
	}
	say("camera: csi3phytimer %#x, csiphy3 %#x\n", rd(camcc + CAMCC_CSI3PHYTIMER_CBCR), rd(camcc + CAMCC_CSIPHY3_CBCR));
	return true;
}

/* back to the crystal, PLL0 off if it was ours (blocks_down() calls it) */
static void
fast_clocks_down(void)
{
	ife0_down();
	if (phy_clocks_ours[1]) {
		wr(camcc + CAMCC_CSIPHY3_CBCR, rd(camcc + CAMCC_CSIPHY3_CBCR) & ~CBCR_ENABLE);
		phy_clocks_ours[1] = false;
	}
	if (phy_clocks_ours[0]) {
		wr(camcc + CAMCC_CSI3PHYTIMER_CBCR, rd(camcc + CAMCC_CSI3PHYTIMER_CBCR) & ~CBCR_ENABLE);
		phy_clocks_ours[0] = false;
	}
	for (unsigned i = FAST_RATES; i-- > 0;) {
		if (fast_rates[i].set) {
			(void)rcg_set(camcc + fast_rates[i].rcg, fast_rates[i].was);
			fast_rates[i].set = false;
		}
	}
	if (pll0_ours) {
		pll_down(0, &pll0_ours);
		say("camera: the roots back at the crystal\n");
	}
}

static void
phy_table(const struct phy_write *t, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		wr(csiphy + t[i].address, t[i].data);
		__asm__ volatile ("dsb sy");
		if (t[i].delay_us) {
			spin_us(t[i].delay_us);
		}
	}
}

static void dphy_show(const char *when);

static void
phy_show(const char *when)
{
	char line_[200];
	int n = 0;

	if (DPHY) {
		dphy_show(when);
		return;
	}

	for (unsigned i = 0; i < 11; i++) {
		n += snprintf(line_ + n, sizeof(line_) - (size_t)n, " %x", rd(csiphy + PHY_IRQ_STATUS(i)));
	}
	say("camera: CSIPHY 3 %s: trios' status %#x %#x %#x, lanes %#x, irq status%s\n", when, rd(csiphy + 0x358) & 0x1f,
	    rd(csiphy + 0x758) & 0x1f, rd(csiphy + 0xb58) & 0x1f, rd(csiphy + PHY_LANE_ENABLE), line_);
}

/* the trios' status registers (cam_csiphy_dump_status_reg(): 24 from 0x340 of each) */
static void
phy_status_dump(void)
{
	for (unsigned t = 0; t < 3; t++) {
		char line_[260];
		int n = 0;

		for (unsigned i = 0; i < 24; i++) {
			n += snprintf(line_ + n, sizeof(line_) - (size_t)n, " %x", rd(csiphy + 0x400 * t + 0x340 + 4 * i));
		}
		say("camera:   trio %u status:%s\n", t, line_);
	}
}

/*
 * D-PHY, four data lanes and the clock lane (the ultra-wide's and the telephoto's S5KJN1), as
 * cam_csiphy_config_dev() does it with csiphy_v2_2_0 (cam_csiphy_2_2_0_hwreg.h), not combo: reset enter, the
 * lanes enabled (data lanes 0..3 at bits 0, 2, 4, 6, the clock lane bit 7), the common registers,
 * csiphy_2ph_v2_2_0_reg with the module file's settle count (38; cam_settle=N) in each lane's 0x08, the
 * skew calibration's writes only with cam_skew=1, the interrupt masks, reset exit for two-phase (0x02).
 */
static uint32_t cam_settle = 38, cam_skew;

static const struct phy_write csiphy_2ph[] = {
	{ 0x0e94, 0x00, 0 }, { 0x0ea0, 0x00, 0 }, { 0x0e90, 0x0f, 0 }, { 0x0e98, 0x08, 0 }, { 0x0e94, 0x07, 209 },
	{ 0x0094, 0x00, 0 }, { 0x00a0, 0x00, 0 }, { 0x0090, 0x0f, 0 }, { 0x0098, 0x08, 0 }, { 0x0094, 0x07, 209 },
	{ 0x0494, 0x00, 0 }, { 0x04a0, 0x00, 0 }, { 0x0490, 0x0f, 0 }, { 0x0498, 0x08, 0 }, { 0x0494, 0x07, 209 },
	{ 0x0894, 0x00, 0 }, { 0x08a0, 0x00, 0 }, { 0x0890, 0x0f, 0 }, { 0x0898, 0x08, 0 }, { 0x0894, 0x07, 209 },
	{ 0x0c94, 0x00, 0 }, { 0x0ca0, 0x00, 0 }, { 0x0c90, 0x0f, 0 }, { 0x0c98, 0x08, 0 }, { 0x0c94, 0x07, 209 },
	{ 0x0e30, 0x00, 0 }, { 0x0e28, 0x04, 0 }, { 0x0e00, 0x80, 0 }, { 0x0e0c, 0xff, 0 }, { 0x0e38, 0x1f, 0 },
	{ 0x0e2c, 0x01, 0 }, { 0x0e34, 0x0f, 0 }, { 0x0e1c, 0x0a, 0 }, { 0x0e14, 0x60, 0 }, { 0x0e3c, 0xb8, 0 },
	{ 0x0e04, 0x0c, 0 }, { 0x0e20, 0x00, 0 }, { 0x0e08, 0xff, 0 }, { 0x0e10, 0x52, 0 },
};
/* each data lane's (at 0x400 x lane; 0xff at 0x08: the settle count) */
static const struct phy_write csiphy_2ph_lane[] = {
	{ 0x0030, 0x00, 0 }, { 0x0000, 0x8e, 0 }, { 0x0038, 0xfe, 0 }, { 0x002c, 0x01, 0 }, { 0x0034, 0x0f, 0 },
	{ 0x001c, 0x0a, 0 }, { 0x0014, 0x60, 0 }, { 0x003c, 0xb8, 0 }, { 0x0004, 0x0c, 0 }, { 0x0020, 0x00, 0 },
	{ 0x0008, 0xff, 0 }, { 0x0010, 0x52, 0 },
};
static const struct phy_write csiphy_2ph_skew[] = {
	{ 0x0094, 0xd7, 0 }, { 0x005c, 0x04, 0 }, { 0x0060, 0xbd, 0 }, { 0x0064, 0x7f, 0 },
};
#define PHY_LANES_DPHY_4        (1u << 0 | 1u << 2 | 1u << 4 | 1u << 6 | 1u << 7)

static void
dphy_show(const char *when)
{
	char line_[200];
	int n = 0;

	for (unsigned i = 0; i < 11; i++) {
		n += snprintf(line_ + n, sizeof(line_) - (size_t)n, " %x", rd(csiphy + PHY_IRQ_STATUS(i)));
	}
	/* (each lane's status from 0xc0: cam_csiphy_dump_status_reg()'s csiphy_2ph_status0_offset) */
	say("camera: CSIPHY %u %s: lanes %#x, lane status %#x %#x %#x %#x clock %#x, irq status%s\n", PHY_N, when,
	    rd(csiphy + PHY_LANE_ENABLE), rd(csiphy + 0x0c0), rd(csiphy + 0x4c0), rd(csiphy + 0x8c0), rd(csiphy + 0xcc0),
	    rd(csiphy + 0xec0), line_);
}

static bool
csiphy_program_dphy(void)
{
	say("camera: CSIPHY %u: four D-PHY lanes, settle count %u%s\n", PHY_N, cam_settle, cam_skew ? ", skew calibration" : "");
	for (unsigned i = 0; i < 11; i++) {
		wr(csiphy + PHY_IRQ_CLEAR(i), rd(csiphy + PHY_IRQ_STATUS(i)));
	}
	wr(csiphy + PHY_IRQ_CMD, 1);
	spin_us(2);
	wr(csiphy + PHY_IRQ_CMD, 0);
	wr(csiphy + PHY_RESET, 1);                      /* reset enter */
	spin_us(2);
	phy_running = true;
	wr(csiphy + PHY_LANE_ENABLE, PHY_LANES_DPHY_4);
	wr(csiphy + 0x1084, 0);
	wr(csiphy + 0x108c, 0);
	spin_us(1);
	wr(csiphy + 0x101c, 0x7a);
	wr(csiphy + 0x1018, 1);
	for (unsigned i = 0; i < sizeof(csiphy_2ph) / sizeof(csiphy_2ph[0]); i++) {
		wr(csiphy + csiphy_2ph[i].address, csiphy_2ph[i].address == 0x0e08 ? (cam_settle & 0xff) : csiphy_2ph[i].data);
		__asm__ volatile ("dsb sy");
		if (csiphy_2ph[i].delay_us) {
			spin_us(csiphy_2ph[i].delay_us);
		}
	}
	for (unsigned lane = 0; lane < 4; lane++) {
		for (unsigned i = 0; i < sizeof(csiphy_2ph_lane) / sizeof(csiphy_2ph_lane[0]); i++) {
			const struct phy_write *w = &csiphy_2ph_lane[i];

			wr(csiphy + 0x400 * lane + w->address, w->address == 0x0008 ? (cam_settle & 0xff) : w->data);
		}
	}
	for (unsigned lane = 0; cam_skew && lane < 4; lane++) {
		for (unsigned i = 0; i < sizeof(csiphy_2ph_skew) / sizeof(csiphy_2ph_skew[0]); i++) {
			wr(csiphy + 0x400 * lane + csiphy_2ph_skew[i].address, csiphy_2ph_skew[i].data);
		}
	}
	__asm__ volatile ("dsb sy");
	phy_table(csiphy_irq_masks, sizeof(csiphy_irq_masks) / sizeof(csiphy_irq_masks[0]));
	wr(csiphy + PHY_RESET, 0x02);                   /* out of reset, two-phase */
	spin_us(100);
	dphy_show("configured");
	return true;
}

static bool
csiphy_program(unsigned rate)
{
	if (csiphy == 0) {
		csiphy = ml_io_map(CSIPHY_PHYS(PHY_N), 0x2000);
	}
	if (DPHY) {
		return csiphy_program_dphy();
	}
	say("camera: CSIPHY 3: three C-PHY trios at %s Gsps\n", phy_rates[rate].name);
	for (unsigned i = 0; i < 11; i++) {
		wr(csiphy + PHY_IRQ_CLEAR(i), rd(csiphy + PHY_IRQ_STATUS(i)));
	}
	wr(csiphy + PHY_IRQ_CMD, 1);
	spin_us(2);
	wr(csiphy + PHY_IRQ_CMD, 0);
	wr(csiphy + PHY_RESET, 1);                      /* reset enter */
	spin_us(2);
	phy_running = true;
	wr(csiphy + PHY_LANE_ENABLE, PHY_LANES_CPHY_012);
	/* the common registers */
	wr(csiphy + 0x1084, 0);
	wr(csiphy + 0x108c, 0);
	spin_us(1);
	wr(csiphy + 0x101c, 0x7a);
	wr(csiphy + 0x1018, 1);
	phy_table(phy_rates[rate].table, phy_rates[rate].count);
	phy_table(csiphy_3ph, sizeof(csiphy_3ph) / sizeof(csiphy_3ph[0]));
	phy_table(csiphy_irq_masks, sizeof(csiphy_irq_masks) / sizeof(csiphy_irq_masks[0]));
	wr(csiphy + PHY_RESET, 0x0e);                   /* out of reset, three-phase */
	spin_us(3048);
	phy_show("configured");
	return true;
}

/*
 * The sensor's MIPI rate: its PLL1 (0x0303 pre-divider, 0x0304/0x0305 multiplier 729: 19.2 MHz x 729 / 4 =
 * 3.4992 Gsps a trio) divided by cam_mipi_div (default 3: 1.166 Gsps) -- at 3.5 Gsps the line comes in
 * at ~2.4 Gpixel/s, more than CSID-lite at 480 MHz takes: it overflowed after 2304 bytes of the first line.
 * A line still fits its 12 us at a third. The MIPI timing unit 0x4837 (4: the period in ns x 4 ...) grows
 * with it. The PHY's tables are tried from the one Android would pick (the first not below the rate) on.
 */
static uint32_t cam_rate = ~0u;         /* cam_rate=N: only phy_rates[N] */
static uint32_t cam_mipi_div = 3;
static unsigned rate_order[PHY_RATES];

static void
rate_order_make(void)
{
	unsigned target = 350 / (cam_mipi_div ? cam_mipi_div : 1), n = 0;
	bool used[PHY_RATES] = { false };

	if (cam_rate < PHY_RATES) {
		rate_order[n++] = cam_rate;
		used[cam_rate] = true;
	}
	while (n < PHY_RATES) {
		unsigned best = PHY_RATES, best_d = ~0u;

		for (unsigned r = 0; r < PHY_RATES; r++) {
			unsigned c = phy_rates[r].centi_gsps, d = c >= target ? c - target : (target - c) * 4 + 1;

			if (!used[r] && d < best_d) {
				best = r;
				best_d = d;
			}
		}
		used[best] = true;
		rate_order[n++] = best;
	}
}

static bool
csiphy_start(void)
{
	/*
	 * The PHY's own supplies (csi-vdd-1p2: ldoi3 1.2 V, csi-vdd-0p9: ldoi2 0.88..0.912 V in the device tree):
	 * shared with the display's DSI PHY, so voted at the top of their range and never taken back
	 */
	static const struct rail vdd_1p2 = { "ldoi3", 1200, 0 }, vdd_0p9 = { "ldoi2", 912, 0 };

	announce("CSIPHY 3: its supplies, three C-PHY trios");
	if (!rail_on(&vdd_1p2) || !rail_on(&vdd_0p9)) {
		give_up("CSIPHY 3's supplies");
		return false;
	}
	spin_us(100);
	return csiphy_program(rate_order[0]);
}

/* (capture_down() calls it) */
static void
csiphy_down(void)
{
	if (!phy_running) {
		return;
	}
	phy_show("at the end");
	wr(csiphy + PHY_RESET, 1);
	wr(csiphy + PHY_LANE_ENABLE, 0);
	wr(csiphy + 0x1084, 0);
	wr(csiphy + 0x108c, 0);
	wr(csiphy + 0x101c, 0);
	wr(csiphy + 0x1018, 0);
	phy_running = false;
}

static bool
sensor_writes(const char *what, const struct sensor_write *t, unsigned n)
{
	const struct sensor *s = SEL;
	unsigned failed = 0;

	say("camera: %s: %s: %u writes\n", s->name, what, n);
	for (unsigned i = 0; i < n; i++) {
		if (!i2c_write8(s->slave, t[i].address, t[i].data) && !i2c_write8(s->slave, t[i].address, t[i].data)) {
			if (failed++ < 4) {
				say("camera: wide: %#06x <- %#04x NOT ACKNOWLEDGED\n", t[i].address, t[i].data);
			}
		}
		if (t[i].address == 0x0103 && t[i].data == 1) {
			wait_ms(5);     /* (the software reset) */
		}
		if (t[i].delay_ms) {
			wait_ms(t[i].delay_ms);
		}
	}
	if (failed) {
		say("camera: wide: %s: %u writes not acknowledged\n", what, failed);
	}
	return failed == 0;
}

static uint32_t
sensor_read16(uint16_t reg)
{
	uint8_t b[2] = { 0, 0 };

	return i2c_read16(SEL->slave, reg, b, 2) == NULL ? (uint32_t)(b[0] << 8 | b[1]) : 0xffffffffu;
}

static bool s5k_writes(const char *what, const struct s5k_write *t, unsigned n);
#define S5K_WRITES(what, t)     s5k_writes(what, t, sizeof(t) / sizeof(t[0]))

static void
sensor_stop(void)
{
	if (sensor_streaming && S5K) {
		(void)S5K_WRITES("stream off", s5kjn1_off);
		sensor_streaming = false;
	} else if (sensor_streaming) {
		(void)sensor_writes("stream off", ovx9000_off, sizeof(ovx9000_off) / sizeof(ovx9000_off[0]));
		sensor_streaming = false;
	}
}

/*
 * cam_verify=1 (default on in mode 1 for now): the tables read back before the stream starts -- each
 * register's last written value against what it reads. Mode 1 came up dark in some starts and fine in
 * others with the same tables: a write the bit-banged I2C got wrong would show here.
 */
static uint32_t sensor_verify = 2;

static void
verify_one(const struct sensor_write *t, unsigned n, const struct sensor_write *later[], const unsigned nlater[], unsigned nl,
    unsigned *checked, unsigned *bad, unsigned *fails)
{
	for (unsigned i = 0; i < n; i++) {
		uint16_t a = t[i].address;
		bool overwritten = false;
		uint32_t v;

		if (a == 0x0103 || a == 0x0100 || a == 0x3208 || a == 0x3016 || (a >= 0x6a00 && a < 0x6b00)) {
			continue;
		}
		for (unsigned j = i + 1; j < n && !overwritten; j++) {
			overwritten = t[j].address == a;
		}
		for (unsigned l = 0; l < nl && !overwritten; l++) {
			for (unsigned j = 0; j < nlater[l] && !overwritten; j++) {
				overwritten = later[l][j].address == a;
			}
		}
		if (overwritten) {
			continue;
		}
		v = sensor_read16(a);
		(*checked)++;
		if (v == 0xffffffffu) {
			(*fails)++;
		} else if ((v >> 8) != t[i].data) {
			if ((*bad)++ < 24) {
				say("camera: wide: verify: %#06x wrote %#04x reads %#04x\n", a, t[i].data, v >> 8);
			}
		}
	}
}

static void
verify_tables(const struct sensor_write *extra, unsigned nextra)
{
	const struct sensor_write *mode = cam_mode == 1 ? ovx9000_mode1 : ovx9000_mode9;
	unsigned nmode = cam_mode == 1 ? sizeof(ovx9000_mode1) / sizeof(ovx9000_mode1[0]) : sizeof(ovx9000_mode9) / sizeof(ovx9000_mode9[0]);
	const struct sensor_write *after_init[2] = { mode, extra }, *after_mode[1] = { extra };
	const unsigned n_after_init[2] = { nmode, nextra }, n_after_mode[1] = { nextra };
	unsigned checked = 0, bad = 0, fails = 0;

	verify_one(ovx9000_init, sizeof(ovx9000_init) / sizeof(ovx9000_init[0]), after_init, n_after_init, 2, &checked, &bad, &fails);
	verify_one(mode, nmode, after_mode, n_after_mode, 1, &checked, &bad, &fails);
	verify_one(extra, nextra, NULL, NULL, 0, &checked, &bad, &fails);
	say("camera: wide: verify: %u registers read back, %u differ, %u reads failed\n", checked, bad, fails);
}

/* the S5KJN1's tables: 16-bit data (or 8-bit where the module file says so), each with its delay */
static bool
s5k_writes(const char *what, const struct s5k_write *t, unsigned n)
{
	const struct sensor *s = SEL;
	unsigned failed = 0;

	say("camera: %s: %s: %u writes\n", s->name, what, n);
	for (unsigned i = 0; i < n; i++) {
		if (!i2c_write16(s->slave, t[i].address, t[i].data, t[i].bytes) &&
		    !i2c_write16(s->slave, t[i].address, t[i].data, t[i].bytes)) {
			if (failed++ < 4) {
				say("camera: %s: %#06x <- %#06x NOT ACKNOWLEDGED\n", s->name, t[i].address, t[i].data);
			}
		}
		if (t[i].delay_us >= 1000) {
			wait_ms((t[i].delay_us + 999) / 1000);
		} else if (t[i].delay_us) {
			spin_us(t[i].delay_us);
		}
	}
	if (failed) {
		say("camera: %s: %s: %u writes not acknowledged\n", s->name, what, failed);
	}
	return failed == 0;
}

/*
 * The ultra-wide's or the telephoto's S5KJN1: power, the chip ID (0x0000: 0x38e1), the module file's init,
 * resolution 4 (1920x1080 RAW10 at 240/s; cam_mode=1: resolution 1, 4080x3072 at 30/s), then the frame
 * length (0x0340), the coarse exposure (0x0202) and the analog gain (0x0204: 0x20 is 1x) as SMIA has them,
 * stream on
 */
static uint32_t s5k_vts, s5k_exp, s5k_gain = 0x20;
/*
 * cam_dg_r / cam_dg_g / cam_dg_b: SMIA's per-colour digital gains (0x0210 red, 0x020e and 0x0214 the greens,
 * 0x0212 blue; 0x100 is 1x): the ultra-wide came pink in Camera.app with the wide's tuning, either Bayer order
 */
static uint32_t s5k_dg_r = 0x100, s5k_dg_g = 0x100, s5k_dg_b = 0x100;

static bool
s5k_start(void)
{
	const struct sensor *s = SEL;
	const struct s5k_write extra[] = {
		{ 0x0340, (uint16_t)s5k_vts, 2, 0 }, { 0x0202, (uint16_t)s5k_exp, 2, 0 }, { 0x0204, (uint16_t)s5k_gain, 2, 0 },
		{ 0x020e, (uint16_t)s5k_dg_g, 2, 0 }, { 0x0210, (uint16_t)s5k_dg_r, 2, 0 }, { 0x0212, (uint16_t)s5k_dg_b, 2, 0 },
		{ 0x0214, (uint16_t)s5k_dg_g, 2, 0 },
	};
	uint32_t id;

	announce("the S5KJN1: power, its tables, stream on");
	if (!power_up(s)) {
		give_up("the S5KJN1's power-up");
		return false;
	}
	i2c_take(s);
	i2c_held = true;
	id = sensor_read16(s->id_reg);
	say("camera: %s: chip ID %#x%s\n", s->name, id, id == s->id ? "" : " -- NOT THE S5KJN1");
	if (id != s->id) {
		give_up("the S5KJN1 is not there");
		return false;
	}
	if (!S5K_WRITES("init", s5kjn1_init) ||
	    (cam_mode == 1 ? !S5K_WRITES("resolution 1 (4080x3072 RAW10)", s5kjn1_mode1) :
	    !S5K_WRITES("resolution 4 (1920x1080 RAW10)", s5kjn1_mode4)) ||
	    !s5k_writes("frame length, exposure, gain", extra, sizeof(extra) / sizeof(extra[0]))) {
		give_up("the S5KJN1's tables");
		return false;
	}
	for (unsigned i = 1; i <= 8; i++) {
		/* cam_wrN=0xAAAAVVVV (N 1..8): register AAAA <- VVVV (16 bits) after the tables */
		char name[16];
		uint32_t v = 0;

		snprintf(name, sizeof(name), "cam_wr%u", i);
		if (PE_parse_boot_argn(name, &v, sizeof(v)) && v != 0) {
			struct s5k_write one = { (uint16_t)(v >> 16), (uint16_t)v, 2, 0 };

			(void)s5k_writes(name, &one, 1);
			say("camera: %s: %s: %#06x <- %#06x (reads %#06x)\n", s->name, name, one.address, one.data,
			    sensor_read16(one.address));
		}
	}
	say("camera: %s: size %u x %u, line length %u, frame length %u (asked %u), exposure %u, gain %#x, "
	    "PLL %#x %#x %#x, MIPI PLL %#x %#x, digital gains R %#x G %#x B %#x\n", s->name, sensor_read16(0x034c), sensor_read16(0x034e), sensor_read16(0x0342),
	    sensor_read16(0x0340), s5k_vts, sensor_read16(0x0202), sensor_read16(0x0204), sensor_read16(0x0304),
	    sensor_read16(0x0306), sensor_read16(0x0302), sensor_read16(0x030e), sensor_read16(0x0310), sensor_read16(0x0210),
	    sensor_read16(0x020e), sensor_read16(0x0212));
	sensor_streaming = true;
	if (!S5K_WRITES("stream on", s5kjn1_on)) {
		give_up("the S5KJN1's stream on");
		return false;
	}
	wait_ms(2);
	say("camera: %s: streaming (0x0100 reads %#x, frame count 0x0005 %#x)\n", s->name, sensor_read16(0x0100),
	    sensor_read16(0x0004));
	return true;
}

static bool
sensor_start(void)
{
	const struct sensor *s = &sensors[0];
	struct sensor_write extra[] = {
		{ 0x380e, (uint8_t)(cam_vts >> 8), 0 }, { 0x380f, (uint8_t)cam_vts, 0 },
		{ 0x3501, (uint8_t)(cam_exp >> 8), 0 }, { 0x3502, (uint8_t)cam_exp, 0 },
		{ 0x3508, (uint8_t)cam_gain, 0 }, { 0x3509, 0, 0 },
		{ 0x0304, (uint8_t)((729 / cam_mipi_div) >> 8), 0 }, { 0x0305, (uint8_t)(729 / cam_mipi_div), 0 },
		{ 0x4837, (uint8_t)(4 * cam_mipi_div), 0 },
		{ 0x3808, (uint8_t)(cam_width >> 8), 0 }, { 0x3809, (uint8_t)cam_width, 0 },
	};
	uint32_t id;

	if (S5K) {
		return s5k_start();
	}
	announce("the wide sensor: power, its tables, stream on");
	if (!power_up(s)) {
		give_up("the wide sensor's power-up");
		return false;
	}
	i2c_take(s);
	i2c_held = true;
	id = sensor_read16(s->id_reg);
	say("camera: wide: chip ID %#x%s\n", id, id == s->id ? "" : " -- NOT THE OVX9000");
	if (id != s->id) {
		give_up("the wide sensor is not there");
		return false;
	}
	if (!sensor_writes("init", ovx9000_init, sizeof(ovx9000_init) / sizeof(ovx9000_init[0])) ||
	    (cam_mode == 1 ? !sensor_writes("mode 1 (4096x3072 RAW10)", ovx9000_mode1, sizeof(ovx9000_mode1) / sizeof(ovx9000_mode1[0])) :
	    !sensor_writes("mode 9 (1920x1080 RAW10)", ovx9000_mode9, sizeof(ovx9000_mode9) / sizeof(ovx9000_mode9[0]))) ||
	    !sensor_writes("frame length, exposure, gain", extra, sizeof(extra) / sizeof(extra[0]))) {
		give_up("the wide sensor's tables");
		return false;
	}
	for (unsigned i = 1; i <= 8; i++) {
		/* cam_wrN=0xAAAAVV (N 1..8): register AAAA <- VV after the tables, to try one setting at a time */
		char name[16];
		uint32_t v = 0;

		snprintf(name, sizeof(name), "cam_wr%u", i);
		if (PE_parse_boot_argn(name, &v, sizeof(v)) && v != 0) {
			struct sensor_write one = { (uint16_t)(v >> 8), (uint8_t)v, 0 };

			(void)sensor_writes(name, &one, 1);
			say("camera: wide: %s: %#06x <- %#04x (reads %#04x)\n", name, one.address, one.data,
			    sensor_read16(one.address) >> 8);
		}
	}
	say("camera: wide: MIPI PLL multiplier %#x (729 / %u), timing unit %#x\n", sensor_read16(0x0304), cam_mipi_div,
	    sensor_read16(0x4837) >> 8);
	say("camera: wide: frame length %#x (asked %u), line length %#x, size %#x x %#x, exposure %#x, gain %#x\n",
	    sensor_read16(0x380e), cam_vts, sensor_read16(0x380c), sensor_read16(0x3808), sensor_read16(0x380a),
	    sensor_read16(0x3501), sensor_read16(0x3508));
	if (sensor_verify) {
		verify_tables(extra, sizeof(extra) / sizeof(extra[0]));
	}
	sensor_streaming = true;
	if (!sensor_writes("stream on", ovx9000_on, sizeof(ovx9000_on) / sizeof(ovx9000_on[0]))) {
		give_up("the wide sensor's stream on");
		return false;
	}
	say("camera: wide: streaming (0x0100 reads %#x)\n", sensor_read16(0x0100) >> 8);
	return true;
}

/* the I2C's pins given back, the sensor powered down (the blocks stay: blocks_down() after) */
static void
sensor_release(void)
{
	if (i2c_held) {
		i2c_give_back();
		i2c_held = false;
	}
	power_down();
}

/*
 * IFE-lite 0's own view (cam_vfe_lite78x.h, less its 0x1000: our mapping starts there): the top's interrupts,
 * diagnostics and IPP violations; the bus's interrupt, CCIF/overflow/image size violations; write master 0's
 * address and debug status (constraint errors enabled as cam_vfe_bus_ver3_start_wm() does)
 */
static void
ife_show(const char *when)
{
	say("camera: IFE %s: top irq %#x %#x, diag %#x %#x, ipp violation %#x; bus irq %#x, ccif violation %#x, overflow %#x, "
	    "size violation %#x, debug %#x\n", when, rd(cam_ife + ife_top[0]), rd(cam_ife + ife_top[1]), rd(cam_ife + ife_top[2]),
	    rd(cam_ife + ife_top[3]), rd(cam_ife + ife_top[4]), rd(cam_ife + BUS + 0x28), rd(cam_ife + BUS + 0x64),
	    rd(cam_ife + BUS + 0x68), rd(cam_ife + BUS + 0x70), rd(cam_ife + BUS + 0xd8));
	say("camera: IFE %s: WM cfg %#x addr %#x incr %#x cfg0 %#x; addr status %#x %#x %#x %#x; debug status %#x %#x\n", when,
	    rd(cam_ife + WM0_CFG), rd(cam_ife + WM0_IMAGE_ADDR), rd(cam_ife + WM0_FRAME_INCR), rd(cam_ife + WM0_IMAGE_CFG_0),
	    rd(cam_ife + ife_wm + ife_wm_status), rd(cam_ife + ife_wm + ife_wm_status + 4), rd(cam_ife + ife_wm + ife_wm_status + 8),
	    rd(cam_ife + ife_wm + ife_wm_status + 0xc), rd(cam_ife + ife_wm + ife_wm_status + 0x14),
	    rd(cam_ife + ife_wm + ife_wm_status + 0x18));
}

/*
 * With the sensor streaming: its MIPI rate and timing unit, and the PHY's tables, in turn (stream off, the
 * sensor's registers, stream on, the PHY), 300 ms each: what the CSID makes of them -- packets, CRC errors,
 * RDI 0's start and end of frame, a buffer done. The best is kept (a buffer done ends it), and the CSID and
 * the IFE are reset and configured afresh for the frames. cam_mipi_div=N: that divider only.
 */
struct mipi_try {
	uint32_t        div, unit;
};

static bool
sensor_mipi(uint32_t div, uint32_t unit)
{
	struct sensor_write w[] = {
		{ 0x0304, (uint8_t)((729 / div) >> 8), 0 }, { 0x0305, (uint8_t)(729 / div), 0 }, { 0x4837, (uint8_t)unit, 0 },
	};

	sensor_stop();
	if (!sensor_writes("MIPI rate", w, sizeof(w) / sizeof(w[0]))) {
		return false;
	}
	sensor_streaming = true;
	return sensor_writes("stream on", ovx9000_on, sizeof(ovx9000_on) / sizeof(ovx9000_on[0]));
}

/* the CSID and the IFE afresh: an overflow leaves the path stopped until a reset */
static void
path_afresh(void)
{
	announce_quiet = true;
	wr(cam_ife + WM0_CFG, 0);
	spin_us(100);
	(void)csid_reset();
	(void)ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, wm_stride * buf_lines);
	(void)csid_config(RX_CFG0_SEL, 0);
	wr(cam_csid + CSID_RX_IRQ_MASK, 0xffffffffu);
	wr(cam_csid + 0xf0, 0xffffffffu);                       /* RDI 0's interrupts all shown */
	announce_quiet = false;
}

static unsigned
link_score(const char *what)
{
	uint32_t p0, c0, p1, c1, rdi, buf, irq;
	unsigned score;

	wr(cam_csid + CSID_RDI0_IRQ_CLEAR, rd(cam_csid + CSID_RDI0_IRQ_STATUS));
	wr(cam_csid + CSID_RX_IRQ_CLEAR, rd(cam_csid + CSID_RX_IRQ_STATUS));
	wr(cam_csid + CSID_BUF_DONE_CLEAR, BUF_DONE_RDI0);
	wr(cam_csid + CSID_IRQ_CMD, 1);
	p0 = rd(cam_csid + 0x240);
	c0 = rd(cam_csid + 0x248);
	wait_ms(300);
	p1 = rd(cam_csid + 0x240);
	c1 = rd(cam_csid + 0x248);
	rdi = rd(cam_csid + CSID_RDI0_IRQ_STATUS);
	buf = rd(cam_csid + CSID_BUF_DONE_STATUS);
	irq = rd(cam_csid + CSID_RX_IRQ_STATUS);
	score = (buf & BUF_DONE_RDI0) ? 1000 : (rdi & 1u << 3) ? 500 : (rdi & 1u << 4) ? 200 : 0;
	score += p1 != p0 ? 50 : 0;
	score += p1 != p0 && c1 == c0 ? 100 : 0;
	score -= (rdi & 1u << 19) && score >= 50 ? 30 : 0;
	say("camera: try %s: packets %u, CRC %u, rx irq %#x, RDI0 irq %#x, buf done %#x: score %u\n", what, p1 - p0, c1 - c0, irq, rdi,
	    buf, score);
	return score;
}

static void
link_sweep(void)
{
	/* (the clean one first: 1.75 Gsps had no CRC errors; then full speed, then slower ones) */
	static const struct mipi_try tries_[] = { { 2, 8 }, { 1, 4 }, { 3, 4 }, { 4, 16 }, { 2, 4 } };
	uint32_t asked = 0;
	unsigned best = 0, best_t = 0, best_r = 0;

	PE_parse_boot_argn("cam_mipi_div", &asked, sizeof(asked));
	for (unsigned t = 0; t < sizeof(tries_) / sizeof(tries_[0]) && best < 1000; t++) {
		if (asked != 0 && tries_[t].div != asked) {
			continue;
		}
		cam_mipi_div = tries_[t].div;
		rate_order_make();
		if (!sensor_mipi(tries_[t].div, tries_[t].unit)) {
			return;
		}
		for (unsigned i = 0; i < 3 && best < 1000; i++) {
			char what[64];
			unsigned score;

			(void)csiphy_program(rate_order[i]);
			path_afresh();
			snprintf(what, sizeof(what), "MIPI /%u unit %u, PHY %s Gsps", tries_[t].div, tries_[t].unit,
			    phy_rates[rate_order[i]].name);
			score = link_score(what);
			if (score > best) {
				best = score;
				best_t = t;
				best_r = rate_order[i];
			}
		}
	}
	say("camera: the best: MIPI /%u unit %u, PHY %s Gsps (score %u)\n", tries_[best_t].div, tries_[best_t].unit,
	    phy_rates[best_r].name, best);
	phy_status_dump();
	(void)sensor_mipi(tries_[best_t].div, tries_[best_t].unit);
	(void)csiphy_program(best_r);
	path_afresh();
}

static void
csid_rx_show(const char *when)
{
	uint32_t m0 = rd(cam_csid + 0x588);

	say("camera: CSID %s: rx irq %#x, RDI0 irq %#x\n", when, rd(cam_csid + CSID_RX_IRQ_STATUS), rd(cam_csid + CSID_RDI0_IRQ_STATUS));
	say("camera: CSID %s: RDI0 measured %u lines of %u pixels (%#x; hbi/vbi %#x %#x), byte counters %#x %#x, camif debug %#x %#x, "
	    "halt %#x\n", when, m0 >> 16, m0 & 0xffff, m0, rd(cam_csid + 0x58c), rd(cam_csid + 0x590), rd(cam_csid + 0x520),
	    rd(cam_csid + 0x524), rd(cam_csid + 0x538), rd(cam_csid + 0x53c), rd(cam_csid + 0x568));
	for (uint32_t base = 0x200; base < 0x280; base += 0x40) {
		char line_[200];
		int n = 0;

		for (uint32_t r = base; r < base + 0x40; r += 4) {
			n += snprintf(line_ + n, sizeof(line_) - (size_t)n, " %x", rd(cam_csid + r));
		}
		say("camera:   CSID %#x..:%s\n", base, line_);
	}
}

/*
 * The camera's NoC (cam_camnoc 0xac19000, cpastop_v880_100.h) as Android's CPAS sets it at power-up
 * (cam_cpastop_poweron()): the real-time ports' priority, urgency, danger and safe LUTs -- RT1 IFE UBWC/linear,
 * RT2 stats, RT3 PDAF/linear/IFE-lite, RT4 IFE RDI/SFE (IFE 0's write master 23). And its Q-channel
 * (cpas_top 0x5c control, 0x60 status: bit 0 QACCEPTN, bit 1 QDENY), forced on if it does not accept.
 * camnoc_show(): the NoC's error logger (ERRVLD 0x10, ERRLOG0..3 0x20..0x3c), the SBM's fault status
 * 0x248, the RT ports' outstanding writes (MAXWR 0x9n20).
 */
#define CAMNOC_PHYS             0x0ac19000UL
static vm_offset_t camnoc;

static void
camnoc_show(const char *when)
{
	if (camnoc == 0) {
		return;
	}
	say("camera: camnoc %s: errvld %#x errlog %#x %#x %#x %#x %#x %#x %#x %#x; sbm fault %#x; maxwr RT1 %#x RT3 %#x RT4 %#x; "
	    "qchannel ctrl %#x status %#x\n", when, rd(camnoc + 0x10), rd(camnoc + 0x20), rd(camnoc + 0x24), rd(camnoc + 0x28),
	    rd(camnoc + 0x2c), rd(camnoc + 0x30), rd(camnoc + 0x34), rd(camnoc + 0x38), rd(camnoc + 0x3c), rd(camnoc + 0x248),
	    rd(camnoc + 0x9220), rd(camnoc + 0x9620), rd(camnoc + 0x9820), rd(cam_cpas + 0x5c), rd(cam_cpas + 0x60));
}

static void
camnoc_setup(void)
{
	static const struct {
		uint32_t        offset, value;
	} qos[] = {
		{ 0x9230, 0x65555544 }, { 0x9234, 0x66666666 }, { 0x9238, 0x1e40 }, { 0x9240, 0xffffff00 }, { 0x9248, 0xf },
		{ 0x9430, 0x65555544 }, { 0x9434, 0x66666666 }, { 0x9438, 0x1c40 }, { 0x9440, 0xffffff00 }, { 0x9448, 0xf },
		{ 0x9630, 0x65555544 }, { 0x9634, 0x66666666 }, { 0x9638, 0x1c40 }, { 0x9640, 0xffffff00 }, { 0x9648, 0xf },
		{ 0x9830, 0x65555544 }, { 0x9834, 0x66666666 }, { 0x9838, 0x1e40 }, { 0x9840, 0xffffff00 }, { 0x9848, 0xf },
	};
	uint32_t last, noqos = 0;

	if (camnoc == 0) {
		camnoc = ml_io_map(CAMNOC_PHYS, 0xb000);
	}
	announce("the camera NoC's real-time ports as Android's CPAS sets them");
	camnoc_show("before");
	PE_parse_boot_argn("cam_noqos", &noqos, sizeof(noqos));
	for (unsigned i = 0; i < sizeof(qos) / sizeof(qos[0]) && !noqos; i++) {
		say("camera:   camnoc %#x: %#x -> %#x\n", qos[i].offset, rd(camnoc + qos[i].offset), qos[i].value);
		wr(camnoc + qos[i].offset, qos[i].value);
	}
	wr(camnoc + 0x8, 1);                    /* the error logger on (ERL_MAINCTL) */
	if (!(rd(cam_cpas + 0x60) & 1)) {
		/*
		 * the handshake takes the ICP's clock (errata enable_icp_clk_for_qchannel of cpastop_v880_100.h:
		 * cam_cc_icp_clk 0x131bc and its AHB 0x131c8, the root 0x131a4 at the crystal), on for it only
		 */
		bool icp_ours = !(rd(camcc + 0x131bc) & CBCR_ENABLE), ahb_ours = !(rd(camcc + 0x131c8) & CBCR_ENABLE);
		const struct cc_rcg icp_rcg = { "icp", 0x131a4 };

		(void)rcg_at_xo(&icp_rcg);
		(void)branch_on(camcc + 0x131c8, "icp_ahb");
		(void)branch_on(camcc + 0x131bc, "icp");
		say("camera: icp clocks %#x %#x; Q-channel ctrl %#x status %#x\n", rd(camcc + 0x131bc), rd(camcc + 0x131c8),
		    rd(cam_cpas + 0x5c), rd(cam_cpas + 0x60));
		wr(cam_cpas + 0x5c, 0);
		__asm__ volatile ("dsb sy");
		spin_us(10);
		wr(cam_cpas + 0x5c, 1);
		__asm__ volatile ("dsb sy");
		say("camera: camnoc Q-channel forced on: %s (status %#x)\n",
		    wait_for(cam_cpas + 0x60, 1, 1, 10000, &last) ? "accepted" : "NOT ACCEPTED", rd(cam_cpas + 0x60));
		if (icp_ours) {
			wr(camcc + 0x131bc, rd(camcc + 0x131bc) & ~CBCR_ENABLE);
		}
		if (ahb_ours) {
			wr(camcc + 0x131c8, rd(camcc + 0x131c8) & ~CBCR_ENABLE);
		}
	}
	camnoc_show("after");
}

/*
 * What the sensor sends, as the CSID's RX catches it (capture_ctrl 0x208: the next long packet of RAW10, the next
 * short packet, the next C-PHY packet header, each of one virtual channel; rst_strobes 0x20c arms them again):
 * which channel carries the frame start and end, and what the second channel is.
 */
static void
rx_capture(void)
{
	for (uint32_t vc = 0; vc < 4; vc++) {
		for (unsigned k = 0; k < 3; k++) {
			uint32_t irq;

			wr(cam_csid + 0x20c, 0xf);
			wr(cam_csid + CSID_RX_IRQ_CLEAR, 7u << 8);
			wr(cam_csid + CSID_IRQ_CMD, 1);
			wr(cam_csid + 0x208, 1u << 0 | 1u << 1 | 1u << 2 | (uint32_t)RAW10_DT << 4 | vc << 10 | vc << 15 |
			    (uint32_t)RAW10_DT << 20 | vc << 26);
			wait_ms(40);
			irq = rd(cam_csid + CSID_RX_IRQ_STATUS);
			say("camera: RX capture vc %u (%u): long %s %#x %#x %#x ftr %#x; short %s %#x %#x; cphy hdr %s %#x\n", vc, k,
			    irq & 1u << 8 ? "yes" : "no", rd(cam_csid + 0x220), rd(cam_csid + 0x224), rd(cam_csid + 0x210),
			    rd(cam_csid + 0x228), irq & 1u << 9 ? "yes" : "no", rd(cam_csid + 0x218), rd(cam_csid + 0x21c),
			    irq & 1u << 10 ? "yes" : "no", rd(cam_csid + 0x22c));
		}
	}
	wr(cam_csid + 0x208, 0);
}

/* IFE 0's and CSID 0's places for the full path (cam_full, the default) */
static void
full_path_regs(void)
{
	if (full_path) {
		/* IFE 0's bus and its RDI 0's write master (client 23), top registers (cam_vfe880.h), line mode */
		ife_bus = 0xc00;
		ife_wm = 0x2500;
		ife_wm_status = 0x78;
		ife_top[0] = 0x44;
		ife_top[1] = 0x48;
		ife_top[2] = 0x54;
		ife_top[3] = 0x58;
		ife_top[4] = 0x64;
		/*
		 * frame mode by default (cam_lines=N: line mode, N lines): mode 9 sends two streams on VC 1,
		 * both RAW10 1920 wide -- the picture (1080 lines) and PDAF (540) -- so a frame has more lines
		 * than the picture's, and the line-mode write master stopped at a CCIF violation (bus irq
		 * 0x40000000, ccif 0x800000: client 23) after 3 lines
		 */
		/*
		 * (the S5KJN1: line mode, its picture's lines -- in frame mode every frame came shifted by a piece
		 * of about 512 pixels at 1920 (2416 at 4080), its lines wrapped round; line by line it is whole)
		 */
		wm_lines = S5K ? SENSOR_HEIGHT : 0;
		PE_parse_boot_argn("cam_lines", &wm_lines, sizeof(wm_lines));
		buf_done_rdi0 = ~0u;
	}
}

/* cam=5's boot-args (the one frame's and iOS's alike) */
static void
wide_args(void)
{
	uint32_t full = 1;

	/* cam_full=0: the lite path of before (CSID-lite 0, IFE-lite 0) */
	PE_parse_boot_argn("cam_full", &full, sizeof(full));
	full_path = full != 0;
	if (S5K) {
		/* (the S5KJN1's resolution 1: 4080x3072, 4: 1920x1080; 30 frames a second: 4092 or 9600 lines of it) */
		cam_width = cam_mode == 1 ? 4080 : 1920;
		cam_height = cam_mode == 1 ? 3072 : 1080;
		s5k_vts = cam_mode == 1 ? 4092 : 9600;
		PE_parse_boot_argn("cam_vts", &s5k_vts, sizeof(s5k_vts));
		s5k_vts = s5k_vts < (cam_mode == 1 ? 4092u : 1200u) ? (cam_mode == 1 ? 4092u : 1200u) : s5k_vts > 0xfff0 ? 0xfff0 : s5k_vts;
		s5k_exp = s5k_vts / 2;
		PE_parse_boot_argn("cam_exp", &s5k_exp, sizeof(s5k_exp));
		s5k_exp = s5k_exp > s5k_vts - 8 ? s5k_vts - 8 : s5k_exp;
		PE_parse_boot_argn("cam_s5k_gain", &s5k_gain, sizeof(s5k_gain));
		PE_parse_boot_argn("cam_crop", &cam_crop, sizeof(cam_crop));
		PE_parse_boot_argn("cam_phy_again", &cam_phy_again, sizeof(cam_phy_again));
		PE_parse_boot_argn("cam_crop_x0", &cam_crop_x0, sizeof(cam_crop_x0));
		PE_parse_boot_argn("cam_crop_y0", &cam_crop_y0, sizeof(cam_crop_y0));
		PE_parse_boot_argn("cam_dg_r", &s5k_dg_r, sizeof(s5k_dg_r));
		PE_parse_boot_argn("cam_dg_g", &s5k_dg_g, sizeof(s5k_dg_g));
		PE_parse_boot_argn("cam_dg_b", &s5k_dg_b, sizeof(s5k_dg_b));
		PE_parse_boot_argn("cam_settle", &cam_settle, sizeof(cam_settle));
		PE_parse_boot_argn("cam_skew", &cam_skew, sizeof(cam_skew));
	} else if (cam_mode == 1) {
		cam_width = 4096;       /* (the mode's own; cam_width only for mode 9) */
		cam_height = 3072;
	} else {
		cam_width = full_path ? 1920 : cam_width;
		cam_height = 1080;
		PE_parse_boot_argn("cam_width", &cam_width, sizeof(cam_width));
		cam_width = cam_width < 64 ? 64 : cam_width > 1920 ? 1920 : cam_width & ~15u;
	}
	wm_stride = SENSOR_WIDTH * 10 / 8;
	if (S5K) {
		/*
		 * (the S5KJN1 pads each line's payload to 16 bytes: 4080 RAW10 pixels come as 5104 bytes, not 5100 --
		 * resolution 1's test bars sheared by 4 bytes a line at 5100; cam_stride=N another)
		 */
		wm_stride = (wm_stride + 15) & ~15u;
		PE_parse_boot_argn("cam_stride", &wm_stride, sizeof(wm_stride));
	}
	/* (on the full path room for 1620 lines a frame: the picture's 1080 and PDAF's 540, both on VC 1) */
	buf_lines = full_path ? SENSOR_HEIGHT * 3 / 2 : SENSOR_HEIGHT;
	PE_parse_boot_argn("cam_vts", &cam_vts, sizeof(cam_vts));
	PE_parse_boot_argn("cam_exp", &cam_exp, sizeof(cam_exp));
	PE_parse_boot_argn("cam_gain", &cam_gain, sizeof(cam_gain));
	PE_parse_boot_argn("cam_mhz", &cam_mhz, sizeof(cam_mhz));
	PE_parse_boot_argn("cam_mmcx", &cam_mmcx, sizeof(cam_mmcx));
	PE_parse_boot_argn("cam_noc", &cam_noc, sizeof(cam_noc));
	PE_parse_boot_argn("cam_bus", &cam_bus, sizeof(cam_bus));
	PE_parse_boot_argn("cam_rec", &cam_rec, sizeof(cam_rec));
	PE_parse_boot_argn("cam_vc", &cam_vc, sizeof(cam_vc));
	PE_parse_boot_argn("cam_epd", &cam_epd, sizeof(cam_epd));
	PE_parse_boot_argn("cam_rate", &cam_rate, sizeof(cam_rate));
	PE_parse_boot_argn("cam_mipi_div", &cam_mipi_div, sizeof(cam_mipi_div));
	cam_mipi_div = cam_mipi_div < 1 ? 1 : cam_mipi_div > 8 ? 8 : cam_mipi_div;
	rate_order_make();
	cam_vts = cam_vts < 346 ? 346 : cam_vts > 0xffff ? 0xffff : cam_vts;
	cam_exp = cam_exp > cam_vts - 8 ? cam_vts - 8 : cam_exp;
}

/*
 * cam=5: frames of the wide sensor through CSIPHY 3, CSID 0's RDI 0 and IFE 0 into memory (cam_full=0: CSID-lite 0
 * and IFE-lite 0, which overflowed within the first line at every rate and width: 0159..0169)
 */
static void
capture_sensor(void)
{
	uint32_t stride, frame_bytes;
	struct cam_header *h;
	const uint8_t *p;
	unsigned done = 0;
	bool ok;

	wide_args();
	stride = wm_stride;
	frame_bytes = stride * buf_lines;
	if (!frames_alloc(SENSOR_WIDTH, buf_lines, stride, S5K ? SEL->chip : "wide OVX9000", 3)) {   /* (CamX's BGGR: camraw.py's 3) */
		return;
	}
	diag = (char *)(frames + frames_bytes - DIAG_BYTES);
	diag_len = 0;
	memcpy(diag, "CAMDIAG\n", 8);
	diag_len = 8;
	if (!blocks_clocks_up()) {
		return;
	}
	full_path_regs();
	ok = fast_clocks_up() && (!full_path || ife0_up()) && (camnoc_setup(), true) && smmu_up() && csid_reset() &&
	    ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, frame_bytes) &&
	    csid_config(RX_CFG0_SEL, 0) && csiphy_start();
	if (!ok) {
		show_paths("at the fault");
		blocks_down();
		return;
	}
	show_paths("configured");
	if (!sensor_start()) {
		sensor_stop();
		sensor_release();
		blocks_down();
		return;
	}
	ife_show("before the stream");
	if (S5K) {
		path_afresh();          /* (the S5KJN1's one rate: no sweep -- its RX afresh, as the sweep leaves it) */
	} else {
		link_sweep();
	}
	if (full_path) {
		rx_capture();
		csid_rx_show("after the capture");
	}
	h = (struct cam_header *)frames;
	for (unsigned ms = 0; ms < 2000; ms += 50) {
		uint32_t buf;

		wait_ms(50);
		buf = rd(cam_csid + CSID_BUF_DONE_STATUS);
		if (buf & BUF_DONE_RDI0) {
			done++;
			say("camera: RDI 0's buffer done after %u ms (%u; status %#x)\n", ms + 50, done, buf);
			wr(cam_csid + CSID_BUF_DONE_CLEAR, BUF_DONE_RDI0);
			wr(cam_csid + CSID_IRQ_CMD, 1);
			if (done == 3) {
				break;
			}
		}
		if (ms % 500 == 0) {
			show_paths("streaming");
			csid_rx_show("streaming");
			ife_show("streaming");
			camnoc_show("streaming");
			phy_show("streaming");
		}
	}
	show_paths("after the frames");
	csid_rx_show("after the frames");
	ife_show("after the frames");
	camnoc_show("after the frames");
	sensor_stop();
	sensor_release();
	smmu_faults();
	blocks_down();
	flush_range(frames, CAM_HEADER_BYTES + frame_bytes, true);
	h->frames = done != 0 ? 1 : 0;
	h->last = 0;
	p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("camera: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; at its middle: %02x %02x %02x %02x "
	    "(0x5a: not written) -- buffers done: %u\n", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
	    p[frame_bytes / 2], p[frame_bytes / 2 + 1], p[frame_bytes / 2 + 2], p[frame_bytes / 2 + 3], done);
	if (p[0] != 0x5a || p[1] != 0x5a) {
		h->frames = 1;
	}
	flush_range(frames, CAM_HEADER_BYTES, false);
	diag[diag_len] = '\0';
	flush_range((vm_offset_t)diag, DIAG_BYTES, false);
	diag = NULL;
}

/* ---- cam=5: the wide camera for iOS (/dev/redwood-camera) ------------------------------------------- */

/*
 * The wide sensor's frames for iOS's camera stack (libredwood-media.dylib in mediaserverd, camera_test), as
 * redwood_camera.c gives its phone's, through the same device and the same frame header
 * (sys/redwood_camera.h): the sensor and the path up at the first read after an open, down again at the
 * last close, after IDLE_MS without a read, and on a fault (and then for the rest of the run). The path
 * is cam_once=1's (the one frame for the host): CSIPHY 3, CSID 0's RDI 0 (VC 0, the picture), IFE 0's
 * write master 23 in frame mode, the SMMU, memory -- here three places in turn: one the IFE writes,
 * one a read may be copying, one ready.
 *
 * Which place the IFE writes: at each start of frame (CSID 0's RDI 0 SOF, polled every millisecond) the
 * address of the place for the frame after it goes to the write master, with the CSID's register and
 * address update for RDI 0, which it takes at the next SOF: a whole frame's time to give it in. The
 * frame increment is 0, so a frame without an address waiting goes where the last one went (counted
 * as written over). A frame is done at RDI 0's buffer done. The exposure the program asks for is
 * written between two frames, in the sensor's group hold (cam_hold=0: without), and counts from the
 * second frame after.
 *
 * cam_vts_min (default 2768, 30 frames a second): the shortest frame the program may ask for (mode 9's
 * own is 346 lines, 240 a second). A picture for the host (REDWOOD_CAMERA_KEEP) goes into memory in one
 * piece taken at boot (cam_keep_mb, default 24), handed to the loader (xl camera).
 */
#define RING                    3
#define PLACE_SPARE             (256u << 10)    /* room after each frame: some may come longer than the mode's lines */
#define IDLE_MS                 1000
#define FRAME_TIMEOUT_MS        1500
#define IOS_LINE_NS             (S5K ? (cam_mode == 1 ? 8143u : 3472u) : cam_mode == 1 ? 11999u : 12042u)
	/* (OVX9000 mode 1: 2776 lines at 30.02/s; mode 9: 346 at 240/s; S5KJN1 resolution 1: 4092 at 30.01/s, 4: 1200 at 240/s) */
#define IOS_MARGIN              8
#define IOS_GAIN_UNITY          0x100           /* 0x3508/0x3509: 0x100 is 1x */
#define IOS_GAIN_MAX            ios_gain_max
#define IOS_FRAME_LINES_MAX     ios_vts_max
#define RDI_SOF                 0x10            /* RDI 0's irq status (cam_ife_csid880.h: sof_irq_mask) */
#define RDI_EOF                 0x08

enum { PLACE_FREE, PLACE_WRITING, PLACE_READY, PLACE_READING };

static LCK_GRP_DECLARE(ios_lck_grp, "shennong-camera");
static LCK_MTX_DECLARE(ios_lock, &ios_lck_grp);
static struct {
	uint8_t         state;
	uint64_t        sequence, time;
	uint32_t        exposure_lines, gain, frame_lines;
} place[RING];
static vm_offset_t ring, keep_mem;
static uint64_t ring_bytes, ring_place_bytes, keep_mem_bytes, ios_tables_bytes;
/*
 * Mode 1 broke when iOS's AE went past what the mode starts with: a frame longer than its 2776 lines and
 * the gain at 0xf80 -- three pixels of four near black, the fourth's high byte junk, until the sensor is
 * started again. So mode 1 keeps its frame length and 0xf00 (cam_vts_max, cam_gain_max to try more).
 */
static uint32_t ios_gain_max = 0xf80, ios_vts_max = 0xfff0;
static uint32_t ring_frame_bytes, ios_vts_min = 2768, ios_hold = 1, ios_buf_bit = 1u << 14;
/*
 * the frames' Bayer order (cam_bayer: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR): RGGB, not the module file's BGGR -- the green
 * sites are where it says (the cell's top right and bottom left), but mode 9 flips the sensor (0x3820 0x47) and
 * red and blue change places
 */
static uint32_t ios_bayer = 0;     /* (RGGB: Camera.app's colours right with it, wrong with BGGR) */
static volatile bool ios_open, ios_streaming, ios_wanted;
static uint64_t ios_last_read, ios_given, ios_frames_done, ios_writes_late;
static struct { uint32_t lines, gain, frame_lines; bool pending; uint32_t focus; bool focus_pending; } ios_ask;

/*
 * The lens motor: an AKM AK7316 (the module file's shennong_sunny_ovx9000_ak7316_wide_actuator) on the wide sensor's
 * I2C, powered with it (VAF, ldom6 3.3 V). Its tables there: register 0x02 the mode (0x00 active, 0x40 asleep),
 * 0x00/0x01 the position, 10 bits from bit 6 of the pair (0x7d00 at the start: 500), 0x84 where it is (read).
 * Its address is not in the file: 0x0c, AKM's (cam_af_addr=N another; 0: none). The program's 0..1023 is the
 * code as it is.
 */
static uint32_t af_addr = 0x0c, af_position = REDWOOD_CAMERA_NO_FOCUS;
static bool af_scanned;

static bool
af_write(uint8_t reg, const uint8_t *bytes, unsigned n)
{
	bool ok;

	i2c_start();
	ok = i2c_out((uint8_t)(af_addr << 1)) && i2c_out(reg);
	for (unsigned i = 0; ok && i < n; i++) {
		ok = i2c_out(bytes[i]);
	}
	i2c_stop();
	return ok;
}

static bool
af_read(uint8_t reg, uint8_t *bytes, unsigned n)
{
	bool ok;

	i2c_start();
	ok = i2c_out((uint8_t)(af_addr << 1)) && i2c_out(reg);
	if (ok) {
		i2c_start();
		ok = i2c_out((uint8_t)(af_addr << 1 | 1));
		for (unsigned i = 0; ok && i < n; i++) {
			bytes[i] = i2c_in(i + 1 < n);
		}
	}
	i2c_stop();
	return ok;
}

static bool
af_move(uint32_t code)
{
	uint8_t b[2];

	code = code > 1023 ? 1023 : code;
	b[0] = (uint8_t)(code >> 2);
	b[1] = (uint8_t)((code & 3) << 6);
	if (!af_write(0x00, b, 2) && !af_write(0x00, b, 2)) {
		say("camera: the lens motor does not take position %u: no focus from now on\n", code);
		af_position = REDWOOD_CAMERA_NO_FOCUS;
		return false;
	}
	af_position = code;
	return true;
}

/* with the sensor up (its I2C ours): who answers on the bus (once), the motor active, the lens at 500 */
static void
af_up(void)
{
	uint8_t zero = 0, at[2] = { 0, 0 };

	af_position = REDWOOD_CAMERA_NO_FOCUS;
	if (!af_scanned) {
		char list[160];
		size_t len = 0;

		af_scanned = true;
		list[0] = '\0';
		for (uint8_t a = 0x08; a < 0x78; a++) {
			bool ack;

			i2c_start();
			ack = i2c_out((uint8_t)(a << 1));
			i2c_stop();
			if (ack && len + 6 < sizeof(list)) {
				len += (size_t)snprintf(list + len, sizeof(list) - len, " %#04x", a);
			}
		}
		say("camera: the wide sensor's I2C: answering at%s\n", len ? list : " nothing");
	}
	if (af_addr == 0 || af_addr >= 0x78) {
		return;
	}
	if (!af_write(0x02, &zero, 1)) {
		say("camera: no lens motor at %#04x: no focus\n", af_addr);
		return;
	}
	spin_us(2200);
	if (!af_move(500)) {
		return;
	}
	wait_ms(10);
	(void)af_write(0x02, &zero, 1);
	(void)af_read(0x84, at, 2);
	say("camera: the lens motor (AK7316 at %#04x): active, at 500 (it reads %#06x)\n", af_addr, at[0] << 8 | at[1]);
}

static void
af_down(void)
{
	uint8_t sleep_ = 0x40;

	if (af_position == REDWOOD_CAMERA_NO_FOCUS) {
		return;
	}
	/* (towards the rest in steps: no knock) */
	for (uint32_t p = af_position; p > 100; p = p > 200 ? p - 100 : 100) {
		(void)af_move(p);
		wait_ms(5);
	}
	(void)af_write(0x02, &sleep_, 1);
	af_position = REDWOOD_CAMERA_NO_FOCUS;
}
static struct { uint32_t lines, gain, frame_lines; uint64_t from; } ios_now, ios_next;
static bool ios_said_sensor;

static uint64_t
ios_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
exposure_limits(uint32_t *lines, uint32_t *gain, uint32_t *frame_lines)
{
	uint32_t shortest = ios_vts_min;

	*gain = *gain < IOS_GAIN_UNITY ? IOS_GAIN_UNITY : *gain > IOS_GAIN_MAX ? IOS_GAIN_MAX : *gain;
	*lines = *lines < 4 ? 4 : *lines > IOS_FRAME_LINES_MAX - IOS_MARGIN ? IOS_FRAME_LINES_MAX - IOS_MARGIN : *lines;
	if (*frame_lines == 0) {
		*frame_lines = *lines + IOS_MARGIN > shortest ? *lines + IOS_MARGIN : shortest;
	}
	*frame_lines = *frame_lines < shortest ? shortest : *frame_lines > IOS_FRAME_LINES_MAX ? IOS_FRAME_LINES_MAX : *frame_lines;
	if (*lines > *frame_lines - IOS_MARGIN) {
		*lines = *frame_lines - IOS_MARGIN;
	}
}

/* frame length (0x380e/f), exposure of the long set (0x3500..0x3502), its gain (0x3508/9), in group 0's hold */
static bool
exposure_write(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	const struct sensor *s = SEL;
	const struct sensor_write w[] = {
		{ 0x3208, 0x00, 0 },
		{ 0x380e, (uint8_t)(frame_lines >> 8), 0 }, { 0x380f, (uint8_t)frame_lines, 0 },
		{ 0x3500, (uint8_t)(lines >> 16 & 0xf), 0 }, { 0x3501, (uint8_t)(lines >> 8), 0 }, { 0x3502, (uint8_t)lines, 0 },
		{ 0x3508, (uint8_t)(gain >> 8 & 0x1f), 0 }, { 0x3509, (uint8_t)gain, 0 },
		{ 0x3208, 0x10, 0 }, { 0x3208, 0xa0, 0 },
	};
	unsigned from = ios_hold ? 0 : 1, to = ios_hold ? sizeof(w) / sizeof(w[0]) : sizeof(w) / sizeof(w[0]) - 2;

	if (S5K) {
		/* (the S5KJN1: SMIA's frame length, coarse exposure, analog gain 0x20 for 1x, in the group hold 0x0104) */
		uint32_t g = gain / 8;
		const struct s5k_write e[] = {
			{ 0x0104, 1, 1, 0 }, { 0x0340, (uint16_t)frame_lines, 2, 0 }, { 0x0202, (uint16_t)lines, 2, 0 },
			{ 0x0204, (uint16_t)(g < 0x20 ? 0x20 : g > 0x200 ? 0x200 : g), 2, 0 }, { 0x0104, 0, 1, 0 },
		};

		for (unsigned i = ios_hold ? 0 : 1; i < (ios_hold ? 5u : 4u); i++) {
			if (!i2c_write16(s->slave, e[i].address, e[i].data, e[i].bytes) &&
			    !i2c_write16(s->slave, e[i].address, e[i].data, e[i].bytes)) {
				say("camera: %s: %#06x <- %#06x NOT ACKNOWLEDGED (the exposure)\n", s->name, e[i].address, e[i].data);
				give_up("a sensor write nobody acknowledged");
				return false;
			}
		}
		return true;
	}
	for (unsigned i = from; i < to; i++) {
		if (!i2c_write8(s->slave, w[i].address, w[i].data) && !i2c_write8(s->slave, w[i].address, w[i].data)) {
			say("camera: wide: %#06x <- %#04x NOT ACKNOWLEDGED (the exposure)\n", w[i].address, w[i].data);
			give_up("a sensor write nobody acknowledged");
			return false;
		}
	}
	return true;
}

/* the CSID and the IFE afresh for place 0 (after the sensor is streaming, as cam_once's sweep left them) */
static void
ios_afresh(void)
{
	announce_quiet = true;
	wr(cam_ife + WM0_CFG, 0);
	spin_us(100);
	(void)csid_reset();
	(void)ife_config(FRAME_ADDRESSES, 0);
	(void)csid_config(RX_CFG0_SEL, 0);
	wr(cam_csid + CSID_RX_IRQ_MASK, 0xffffffffu);
	wr(cam_csid + 0xf0, 0xffffffffu);                       /* RDI 0's interrupts all shown */
	wr(cam_csid + CSID_BUF_DONE_MASK, 0xffffffffu);
	announce_quiet = false;
}

static void
ring_free(void)
{
	if (page_tables != 0) {
		kmem_free(kernel_map, page_tables, ios_tables_bytes);
		page_tables = 0;
	}
	if (ring != 0) {
		kmem_free(kernel_map, ring, ring_bytes);
		ring = 0;
	}
	frames = 0;
	frames_bytes = 0;
}

static bool
ring_alloc(uint32_t frame_bytes)
{
	ring_frame_bytes = frame_bytes;
	ring_place_bytes = round_page(frame_bytes) + PLACE_SPARE;
	ring_bytes = RING * ring_place_bytes;
	if (kmem_alloc(kernel_map, &ring, ring_bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("camera: no memory for %u frames (%llu KB)\n", RING, ring_bytes >> 10);
		ring = 0;
		return false;
	}
	flush_range(ring, ring_bytes, true);
	frames = ring;                  /* (what smmu_up() maps) */
	frames_bytes = ring_bytes;
	ios_tables_bytes = (2 + ((ring_bytes + 0x1fffff) >> 21)) * 4096;
	return true;
}

static void
ios_stop(const char *why)
{
	lck_mtx_lock(&ios_lock);
	ios_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&ios_lock);
	wakeup(&ios_frames_done);
	if (csid_running || ife_running) {
		show_paths("at the end");
		csid_rx_show("at the end");
		ife_show("at the end");
	}
	if (i2c_held) {
		af_down();
	}
	sensor_stop();
	sensor_release();
	smmu_faults();
	blocks_down();
	ring_free();
	say("camera: the frames for iOS stopped (%s): %llu done, %llu given, %llu written over\n", why, ios_frames_done, ios_given,
	    ios_writes_late);
}

/*
 * cam_sweep=1: before the frames, each MIPI divider with the PHY's two nearest tables, 300 ms each: packets,
 * CRC errors, RDI 0's overflow, a buffer done. Mode 1's lines came with CRC errors (4 in 100 at /2 and /4,
 * none in mode 9 at /2): the one with a buffer done and the fewest CRC errors a packet is kept.
 */
static uint32_t ios_sweep;
static uint32_t ios_af = 1;      /* cam_af=0: the lens motor left alone (no I2C to it) */
static uint32_t ios_ae = 1;      /* cam_ae=0: iOS's exposure asks not written (the start's kept) */
static uint64_t ios_keep_raw;    /* cam_keep_raw=N: frame N kept for the host */

static void
ios_link_sweep(void)
{
	static const uint32_t divs[] = { 1, 2, 3, 4, 5, 6, 8 };
	uint32_t best_div = cam_mipi_div, best_r = rate_order[0];
	uint64_t best = ~0ull;

	for (unsigned d = 0; d < sizeof(divs) / sizeof(divs[0]); d++) {
		cam_mipi_div = divs[d];
		rate_order_make();
		if (!sensor_mipi(divs[d], 4 * divs[d])) {
			break;
		}
		for (unsigned i = 0; i < 2; i++) {
			uint32_t p0, c0, p1, c1, rdi, buf, rx;
			uint64_t cost;

			(void)csiphy_program(rate_order[i]);
			ios_afresh();           /* (place 0 again and again: no frame increment) */
			wr(cam_csid + CSID_RDI0_IRQ_CLEAR, rd(cam_csid + CSID_RDI0_IRQ_STATUS));
			wr(cam_csid + CSID_RX_IRQ_CLEAR, rd(cam_csid + CSID_RX_IRQ_STATUS));
			wr(cam_csid + CSID_BUF_DONE_CLEAR, 0xffffffffu);
			wr(cam_csid + CSID_IRQ_CMD, 1);
			p0 = rd(cam_csid + 0x240);
			c0 = rd(cam_csid + 0x248);
			wait_ms(300);
			p1 = rd(cam_csid + 0x240);
			c1 = rd(cam_csid + 0x248);
			rdi = rd(cam_csid + CSID_RDI0_IRQ_STATUS);
			buf = rd(cam_csid + CSID_BUF_DONE_STATUS);
			rx = rd(cam_csid + CSID_RX_IRQ_STATUS);
			/* (in 1/10000 of the packets; no packets or no buffer done: worse than any) */
			cost = p1 == p0 ? ~0ull - 1 : (uint64_t)(c1 - c0) * 10000 / (p1 - p0);
			cost += (buf & ios_buf_bit) ? 0 : 1ull << 40;
			cost += (rdi & 1u << 19) ? 1ull << 32 : 0;
			say("camera: sweep MIPI /%u, PHY %s Gsps: packets %u, CRC %u, rx irq %#x, RDI0 irq %#x, buf done %#x\n",
			    divs[d], phy_rates[rate_order[i]].name, p1 - p0, c1 - c0, rx, rdi, buf);
			if (cost < best) {
				best = cost;
				best_div = divs[d];
				best_r = rate_order[i];
			}
		}
	}
	cam_mipi_div = best_div;
	rate_order_make();
	(void)sensor_mipi(best_div, 4 * best_div);
	(void)csiphy_program(best_r);
	say("camera: sweep: the best MIPI /%u, PHY %s Gsps\n", best_div, phy_rates[best_r].name);
}

static bool
ios_start(void)
{
	uint32_t lines, gain, frame_lines;

	ios_frames_done = ios_given = ios_writes_late = 0;
	lck_mtx_lock(&ios_lock);
	lines = ios_ask.lines;
	gain = ios_ask.gain;
	frame_lines = ios_ask.frame_lines;
	ios_ask.pending = false;
	lck_mtx_unlock(&ios_lock);
	exposure_limits(&lines, &gain, &frame_lines);
	/* (sensor_start() writes these: the frame length, exposure, gain's high byte; the MIPI rate /2, unit 8) */
	cam_vts = frame_lines;
	cam_exp = lines;
	cam_gain = gain >> 8;
	if (!ring_alloc(wm_stride * SENSOR_HEIGHT)) {
		return false;
	}
	if (!blocks_clocks_up()) {
		return false;
	}
	full_path_regs();
	if (!(fast_clocks_up() && (!full_path || ife0_up()) && (camnoc_setup(), true) && smmu_up() && csid_reset() &&
	    ife_config(FRAME_ADDRESSES, 0) && csid_config(RX_CFG0_SEL, 0) && csiphy_start())) {
		show_paths("at the fault");
		return false;
	}
	if (!sensor_start() || !exposure_write(lines, gain, frame_lines)) {
		return false;
	}
	if (ios_af) {
		af_up();
	}
	if (ios_sweep) {
		ios_link_sweep();
	} else if (!S5K || cam_phy_again) {
		/*
		 * (not for the S5KJN1: its D-PHY programmed again while the sensor streams left every line ~500
		 * pixels late against the CSID's line marks, a strip of the line before at the left -- the one
		 * frame's path, which does not, came whole; cam_phy_again=1: as before)
		 */
		(void)csiphy_program(rate_order[0]);
	}
	ios_afresh();
	lck_mtx_lock(&ios_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
		place[i].sequence = 0;
	}
	place[0].state = PLACE_WRITING;
	lck_mtx_unlock(&ios_lock);
	ios_now.lines = ios_next.lines = lines;
	ios_now.gain = ios_next.gain = gain;
	ios_now.frame_lines = ios_next.frame_lines = frame_lines;
	ios_next.from = 0;
	say("camera: the frames for iOS: mode %u, %ux%u RAW10, MIPI /%u, PHY %s Gsps; exposure %u lines, gain %#x, frame %u lines "
	    "(%u.%u frames/s), Bayer %u, line smoothing %s\n", cam_mode, SENSOR_WIDTH, SENSOR_HEIGHT, cam_mipi_div, phy_rates[rate_order[0]].name, lines, gain, frame_lines,
	    (uint32_t)(10000000000ull / ((uint64_t)frame_lines * IOS_LINE_NS) / 10),
	    (uint32_t)(10000000000ull / ((uint64_t)frame_lines * IOS_LINE_NS) % 10), ios_bayer,
	    rdi_smooth ? "on" : "off");
	return true;
}

static void
ios_session(void)
{
	int writing = 0, next = 0;      /* (place 0's address is the write master's from ios_afresh(): taken at the first SOF) */
	uint64_t last_frame;
	unsigned restarts = 0, odd = 0;

	if (!ios_start()) {
		give_up("the camera did not start");
		ios_stop("it did not start");
		return;
	}
	last_frame = ios_ms();
	lck_mtx_lock(&ios_lock);
	ios_streaming = true;
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	while (!broken && ios_open) {
		uint32_t rdi, buf;
		uint64_t now;
		bool done;

		wait_ms(1);
		now = ios_ms();
		rdi = rd(cam_csid + CSID_RDI0_IRQ_STATUS);
		buf = rd(cam_csid + CSID_BUF_DONE_STATUS);
		done = (buf & ios_buf_bit) != 0;
		if (!(rdi & RDI_SOF) && !done) {
			if (now - last_frame > FRAME_TIMEOUT_MS) {
				show_paths("no frame");
				csid_rx_show("no frame");
				ife_show("no frame");
				if (restarts++ >= 3) {
					give_up("no frame for 1.5 s, four times");
					break;
				}
				/* the path afresh (an overflow stops it until a reset), from place 0 */
				if (cam_smooth_flip) {
					/* (an overflow with the line smoothing on, and one with it off: which one stops the frames) */
					rdi_smooth = !rdi_smooth;
				}
				say("camera: no frame for 1.5 s: the CSID and the IFE afresh (%u), RDI 0's line smoothing %s\n", restarts,
				    rdi_smooth ? "on" : "off");
				ios_afresh();
				lck_mtx_lock(&ios_lock);
				for (int i = 0; i < RING; i++) {
					if (place[i].state == PLACE_WRITING) {
						place[i].state = PLACE_FREE;
					}
				}
				if (place[0].state != PLACE_READING) {
					place[0].state = PLACE_WRITING;
				}
				lck_mtx_unlock(&ios_lock);
				writing = 0;
				next = 0;
				last_frame = now;
				continue;
			}
			if (now - ios_last_read > IDLE_MS) {
				break;
			}
			continue;
		}
		wr(cam_csid + CSID_RDI0_IRQ_CLEAR, rdi);
		wr(cam_csid + CSID_BUF_DONE_CLEAR, buf);
		wr(cam_csid + CSID_IRQ_CMD, 1);
		if ((rdi & ~(uint32_t)(RDI_SOF | RDI_EOF | 1u << 23)) != 0 && odd++ < 8) {
			say("camera: frame %llu: RDI 0's irq %#x, buf done %#x\n", ios_frames_done + 1, rdi, buf);
		}
		lck_mtx_lock(&ios_lock);
		if (done) {
			last_frame = now;
			ios_frames_done++;
			if (ios_next.from != 0 && ios_frames_done >= ios_next.from) {
				ios_now = ios_next;
				ios_next.from = 0;
			}
			if (place[writing].state == PLACE_WRITING) {
				place[writing].state = PLACE_READY;
				place[writing].sequence = ios_frames_done;
				place[writing].time = mach_absolute_time();
				place[writing].exposure_lines = ios_now.lines;
				place[writing].gain = ios_now.gain;
				place[writing].frame_lines = ios_now.frame_lines;
			}
		}
		if (rdi & RDI_SOF) {
			if (next >= 0) {
				writing = next;         /* (the address given at the last SOF, taken at this one) */
				next = -1;
			} else {
				ios_writes_late++;      /* no address waiting: this frame goes where the last one went */
			}
			place[writing].state = PLACE_WRITING;
			for (int i = 0; i < RING; i++) {
				int c = (writing + 1 + i) % RING;

				if (c != writing && place[c].state != PLACE_READING && (next < 0 || place[c].sequence < place[next].sequence)) {
					next = c;
				}
			}
			if (next >= 0) {
				place[next].state = PLACE_FREE;        /* (not to be read while the IFE may be writing it) */
				wr(cam_ife + WM0_IMAGE_ADDR, (uint32_t)((FRAME_ADDRESSES + (uint64_t)next * ring_place_bytes) >> 8));
				wr(cam_csid + CSID_RUP_AUP_CMD, 1u << 4 | 1u << 20);
			}
		}
		bool ask = done && ios_ask.pending, focus = done && ios_ask.focus_pending;
		uint32_t lines = ios_ask.lines, gain = ios_ask.gain, frame_lines = ios_ask.frame_lines, position = ios_ask.focus;

		ios_ask.pending = ios_ask.pending && !ask;
		ios_ask.focus_pending = ios_ask.focus_pending && !focus;
		lck_mtx_unlock(&ios_lock);
		if (focus && af_position != REDWOOD_CAMERA_NO_FOCUS) {
			static unsigned moves;
			uint8_t at[2] = { 0, 0 };

			(void)af_move(position);
			if (moves++ < 8) {
				spin_us(3000);
				(void)af_read(0x84, at, 2);
				say("camera: focus %u (frame %llu): it reads %#06x\n", position, ios_frames_done, at[0] << 8 | at[1]);
			}
		}
		if (done && ios_frames_done <= 120 && ios_frames_done % 3 == 0) {
			/* (each third frame's middle line: its four pixel phases' high bytes, the junk byte's spread) */
			vm_offset_t line = ring + (uint64_t)writing * ring_place_bytes + (uint64_t)wm_stride * (SENSOR_HEIGHT / 2);
			const uint8_t *b = (const uint8_t *)line;
			uint32_t sum[5] = { 0 }, jumps = 0;

			flush_range(line, wm_stride, true);
			for (uint32_t x = 0; x + 5 <= wm_stride; x += 5) {
				for (int i = 0; i < 5; i++) {
					sum[i] += b[x + i];
				}
				jumps += x >= 5 && (b[x + 1] > b[x - 4] ? b[x + 1] - b[x - 4] : b[x - 4] - b[x + 1]) > 32;
			}
			say("camera: frame %llu: middle line's bytes 0..4 average %u %u %u %u %u, byte 1 jumps %u of %u (%llu ms)\n",
			    ios_frames_done, sum[0] * 5 / wm_stride, sum[1] * 5 / wm_stride, sum[2] * 5 / wm_stride, sum[3] * 5 / wm_stride,
			    sum[4] * 5 / wm_stride, jumps, wm_stride / 5, now);
		}
		if (done && ios_frames_done == ios_keep_raw && keep_mem != 0 &&
		    keep_mem_bytes >= round_page(CAM_HEADER_BYTES + (uint64_t)ring_frame_bytes)) {
			/* cam_keep_raw=N: frame N as the sensor sent it, for the host (xl camera) */
			struct cam_header *h = (struct cam_header *)keep_mem;
			vm_offset_t at = ring + (uint64_t)writing * ring_place_bytes;

			flush_range(at, ring_frame_bytes, true);
			bzero(h, CAM_HEADER_BYTES);
			memcpy(h->magic, "RWCAM001", 8);
			h->width = SENSOR_WIDTH;
			h->height = SENSOR_HEIGHT;
			h->stride = wm_stride;
			h->format = 1;
			h->bayer = ios_bayer;
			h->frames = 1;
			h->frame_bytes = ring_frame_bytes;
			h->source = 1;
			strlcpy(h->sensor, "ovx9000 raw", sizeof(h->sensor));
			memcpy((void *)(keep_mem + CAM_HEADER_BYTES), (const void *)at, ring_frame_bytes);
			flush_range(keep_mem, round_page(CAM_HEADER_BYTES + (uint64_t)ring_frame_bytes), false);
			loader_recording[0] = kvtophys(keep_mem);
			loader_recording[1] = round_page(CAM_HEADER_BYTES + (uint64_t)ring_frame_bytes);
			loader_recording[2] = (uint64_t)keep_mem;
			say("camera: frame %llu kept raw for the host (%u bytes; xl camera)\n", ios_frames_done, ring_frame_bytes);
		}
		if (done) {
			wakeup(&ios_frames_done);
			if (ios_frames_done <= 3 || ios_frames_done % 300 == 0) {
				say("camera: frame %llu done (place %d; %llu given, %llu written over)\n", ios_frames_done, writing, ios_given,
				    ios_writes_late);
			}
		}
		if (ask && ios_ae) {
			exposure_limits(&lines, &gain, &frame_lines);
			if (lines != ios_now.lines || gain != ios_now.gain || frame_lines != ios_now.frame_lines) {
				if (!exposure_write(lines, gain, frame_lines)) {
					break;
				}
				ios_next.lines = lines;
				ios_next.gain = gain;
				ios_next.frame_lines = frame_lines;
				ios_next.from = ios_frames_done + 2;
			}
		}
		if (done && ios_frames_done % 30 == 0) {
			if (rd(smmu_ctx + SMMU_CB_FSR) & SMMU_FSR_ALL) {
				smmu_faults();
				give_up("the SMMU refused the IFE");
				break;
			}
			if (ios_frames_done == 30 || ios_frames_done % 900 == 0) {
				show_paths("streaming");
				ife_show("streaming");
			}
		}
	}
	ios_stop(broken ? "a fault" : !ios_open ? "closed" : "nobody reads");
}

static int
camdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (broken) {
		return EIO;
	}
	ios_open = true;
	return 0;
}

/* (the last close) */
static int
camdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	ios_open = false;
	ios_wanted = false;
	wakeup(&ios_open);
	return 0;
}

static int
camdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_camera_frame f;
	uint64_t since = ios_ms();
	int b = -1, error = 0;

	lck_mtx_lock(&ios_lock);
	ios_last_read = ios_ms();
	if (!ios_streaming && !ios_wanted) {
		ios_wanted = true;
		wakeup(&ios_open);
	}
	for (;;) {
		if (broken) {
			lck_mtx_unlock(&ios_lock);
			return EIO;
		}
		b = -1;
		for (int i = 0; i < RING; i++) {
			if (place[i].state == PLACE_READY && place[i].sequence > ios_given && (b < 0 || place[i].sequence > place[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0) {
			break;
		}
		if (ios_ms() - since > (ios_streaming ? 3000u : 20000u)) {      /* (the sensor's tables take seconds) */
			lck_mtx_unlock(&ios_lock);
			return EAGAIN;
		}
		if (!ios_streaming && !ios_wanted) {
			ios_wanted = true;
			wakeup(&ios_open);
		}
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };
		error = msleep(&ios_frames_done, &ios_lock, PZERO | PCATCH, "shennong-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&ios_lock);
			return error;
		}
		ios_last_read = ios_ms();
	}
	place[b].state = PLACE_READING;
	ios_given = place[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, REDWOOD_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = SENSOR_WIDTH;
	f.height = SENSOR_HEIGHT;
	f.stride = wm_stride;
	f.format = 1;
	f.bayer = ios_bayer;
	f.black = 64;
	f.sequence = place[b].sequence;
	f.time = place[b].time;
	f.exposure_lines = place[b].exposure_lines;
	f.line_ns = IOS_LINE_NS;
	f.gain = place[b].gain;
	f.gain_unity = IOS_GAIN_UNITY;
	f.gain_max = IOS_GAIN_MAX;
	f.frame_lines = place[b].frame_lines;
	f.frame_lines_min = ios_vts_min;
	f.frame_lines_max = IOS_FRAME_LINES_MAX;
	f.margin = IOS_MARGIN;
	f.sensor = 1;                   /* the wide camera: the back camera of the media library */
	f.focus = af_position;
	lck_mtx_unlock(&ios_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		vm_offset_t at = ring + (uint64_t)b * ring_place_bytes;
		uint32_t n = (uint32_t)MIN((uint64_t)ring_frame_bytes, (uint64_t)uio_resid(uio));

		flush_range(at, n, true);       /* (what the IFE wrote went past the processor's caches) */
		error = uiomove((caddr_t)at, (int)n, uio);
		if (f.sequence <= 3) {
			const uint8_t *px = (const uint8_t *)at;

			say("camera: frame %llu read: %u bytes; first %02x %02x %02x %02x %02x, middle %02x %02x %02x %02x %02x\n",
			    f.sequence, n, px[0], px[1], px[2], px[3], px[4], px[n / 2], px[n / 2 + 1], px[n / 2 + 2], px[n / 2 + 3],
			    px[n / 2 + 4]);
		}
	}
	lck_mtx_lock(&ios_lock);
	if (place[b].state == PLACE_READING) {
		place[b].state = PLACE_READY;
	}
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	return error;
}

/* REDWOOD_CAMERA_KEEP: a picture for the host, in the memory in one piece the loader hands over */
static int
keep_picture(const struct redwood_camera_command *c, struct uio *uio)
{
	struct cam_header *h;
	uint64_t want = round_page(CAM_HEADER_BYTES + (uint64_t)c->bytes);

	if (c->bytes == 0 || c->d < 1 || c->d > 3 || (uint64_t)c->c * c->b > (uint64_t)c->bytes * 2) {
		return EINVAL;
	}
	if (keep_mem == 0 || keep_mem_bytes < want) {
		return ENOSPC;
	}
	h = (struct cam_header *)keep_mem;
	bzero(h, CAM_HEADER_BYTES);
	memcpy(h->magic, "RWCAM001", 8);
	h->width = c->a;
	h->height = c->b;
	h->stride = c->c;
	h->format = c->d;
	h->bayer = c->d == 1 ? ios_bayer : 0;
	h->frame_bytes = c->bytes;
	h->source = 2;
	strlcpy(h->sensor, "iOS", sizeof(h->sensor));
	int error = uiomove((caddr_t)(keep_mem + CAM_HEADER_BYTES), (int)MIN((uint64_t)c->bytes, (uint64_t)uio_resid(uio)), uio);
	h->frames = error == 0 ? 1 : 0;
	flush_range(keep_mem, want, false);
	loader_recording[0] = kvtophys(keep_mem);
	loader_recording[1] = want;
	loader_recording[2] = (uint64_t)keep_mem;
	say("camera: a picture from iOS for the host: %ux%u, format %u, %u bytes\n", c->a, c->b, c->d, c->bytes);
	return error;
}

static int
camdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_camera_command c;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || uiomove((caddr_t)&c, sizeof(c), uio) != 0) {
		return EINVAL;
	}
	switch (c.command) {
	case REDWOOD_CAMERA_EXPOSURE:
		lck_mtx_lock(&ios_lock);
		ios_ask.lines = c.a;
		ios_ask.gain = c.b;
		ios_ask.frame_lines = c.c;
		ios_ask.pending = true;
		lck_mtx_unlock(&ios_lock);
		return 0;
	case REDWOOD_CAMERA_KEEP:
		return keep_picture(&c, uio);
	case REDWOOD_CAMERA_FOCUS:
		lck_mtx_lock(&ios_lock);
		ios_ask.focus = c.a;
		ios_ask.focus_pending = true;
		lck_mtx_unlock(&ios_lock);
		return 0;
	case REDWOOD_CAMERA_SENSOR:
		if (c.a == 1) {
			return 0;
		}
		if (!ios_said_sensor) {
			ios_said_sensor = true;
			say("camera: sensor %u asked for: only the wide one (1) is here yet\n", c.a);
		}
		return ENODEV;
	default:
		return EINVAL;
	}
}

static const struct cdevsw camdev_cdevsw = {
	.d_open = camdev_open, .d_close = camdev_close, .d_read = camdev_read, .d_write = camdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* (made as the thread starts: before mediaserverd, whose capture stack looks for the camera once) */
static bool
camdev_make(void)
{
	int major = cdevsw_add(-1, &camdev_cdevsw);

	return major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-camera") != NULL;
}

/* cam=5: a session whenever a read wants frames, until a fault */
static void
serve_ios(bool made)
{
	if (!made) {
		say("camera: /dev/redwood-camera cannot be made\n");
		return;
	}
	/*
	 * mode 9 (1920x1080) by default again: mode 1 (4096x3072) mostly comes up dark -- the sensor's pixels near
	 * black, or one in four junk -- with its tables read back right, its link clean, at any exposure, with
	 * or without the lens motor; once in about fifteen starts it was fine. cam_mode=1 to try it.
	 */
	cam_mode = 9;
	PE_parse_boot_argn("cam_mode", &cam_mode, sizeof(cam_mode));
	cam_mode = cam_mode == 1 ? 1 : 9;
	ios_vts_min = S5K ? (cam_mode == 1 ? 4092 : 9600) : cam_mode == 1 ? 2776 : 2768;
	ios_vts_max = cam_mode == 1 && !S5K ? 2776 : 0xfff0;
	ios_gain_max = cam_mode == 1 && !S5K ? 0xf00 : 0xf80;
	PE_parse_boot_argn("cam_vts_max", &ios_vts_max, sizeof(ios_vts_max));
	PE_parse_boot_argn("cam_gain_max", &ios_gain_max, sizeof(ios_gain_max));
	ios_vts_max = ios_vts_max > 0xfff0 ? 0xfff0 : ios_vts_max;
	ios_gain_max = ios_gain_max < 0x100 ? 0x100 : ios_gain_max > 0xf80 ? 0xf80 : ios_gain_max;
	wide_args();
	/* the link that gave the first photo: MIPI /2 (timing unit 8), its PHY table (1.7 Gsps) */
	/* (mode 1's lines came clean only at the sensor's own rate, 3.5 Gsps a trio: 4 in 100 with CRC errors at /2) */
	cam_mipi_div = cam_mode == 1 ? 1 : 2;
	PE_parse_boot_argn("cam_mipi_div", &cam_mipi_div, sizeof(cam_mipi_div));
	cam_mipi_div = cam_mipi_div < 1 ? 1 : cam_mipi_div > 8 ? 8 : cam_mipi_div;
	rate_order_make();
	PE_parse_boot_argn("cam_vts_min", &ios_vts_min, sizeof(ios_vts_min));
	ios_vts_min = ios_vts_min < 346 ? 346 : ios_vts_min > IOS_FRAME_LINES_MAX ? IOS_FRAME_LINES_MAX : ios_vts_min;
	PE_parse_boot_argn("cam_hold", &ios_hold, sizeof(ios_hold));
	PE_parse_boot_argn("cam_bufbit", &ios_buf_bit, sizeof(ios_buf_bit));
	ios_bayer = S5K ? 2 : 0;        /* (the S5KJN1's raw: GBRG -- its blue, the weakest, second in the first line) */
	PE_parse_boot_argn("cam_bayer", &ios_bayer, sizeof(ios_bayer));
	PE_parse_boot_argn("cam_sweep", &ios_sweep, sizeof(ios_sweep));
	PE_parse_boot_argn("cam_af", &ios_af, sizeof(ios_af));
	PE_parse_boot_argn("cam_verify", &sensor_verify, sizeof(sensor_verify));
	sensor_verify = sensor_verify == 2 ? cam_mode == 1 : sensor_verify;
	if (S5K) {
		/* (the S5KJN1: no OVX9000 MIPI sweep, read-back or lens motor yet) */
		ios_sweep = 0;
		ios_af = 0;
		sensor_verify = 0;
	}
	PE_parse_boot_argn("cam_keep_raw", &ios_keep_raw, sizeof(ios_keep_raw));
	PE_parse_boot_argn("cam_ae", &ios_ae, sizeof(ios_ae));
	{
		uint32_t smooth = 1;

		PE_parse_boot_argn("cam_smooth", &smooth, sizeof(smooth));
		rdi_smooth = smooth != 0;
		PE_parse_boot_argn("cam_smooth_flip", &cam_smooth_flip, sizeof(cam_smooth_flip));
	}
	PE_parse_boot_argn("cam_af_addr", &af_addr, sizeof(af_addr));
	ios_bayer &= 3;
	ios_ask.lines = S5K ? s5k_exp : cam_exp;
	ios_ask.gain = S5K ? s5k_gain * 8 : cam_gain << 8;
	ios_ask.frame_lines = S5K ? s5k_vts : cam_vts;
	say("camera: /dev/redwood-camera: the %s camera's frames (mode %u: %ux%u, at most %u frames/s), whenever it is read\n",
	    SEL->name, cam_mode, SENSOR_WIDTH, SENSOR_HEIGHT, (uint32_t)(1000000000000ull / ((uint64_t)ios_vts_min * IOS_LINE_NS) / 1000));
	while (!broken) {
		lck_mtx_lock(&ios_lock);
		while (!broken && !(ios_open && ios_wanted)) {
			(void)msleep(&ios_open, &ios_lock, PZERO, "shennong-camera-idle", NULL);
		}
		ios_wanted = false;
		lck_mtx_unlock(&ios_lock);
		if (broken) {
			break;
		}
		ios_session();
	}
	say("camera: /dev/redwood-camera: no more frames in this run\n");
}

static uint32_t cam_once;      /* cam=5 cam_once=1: one frame for the host (xl camera), not iOS's frames */

static void
camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	char which[16] = "wide";
	bool made = level == 5 && !cam_once && camdev_make();

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc_camera = ml_io_map(GCC_CAMERA_PHYS, 0x1000);
	camcc = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	cmd_db = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	(void)PE_parse_boot_arg_str("cam_sensor", which, sizeof(which));
	if (level == 5) {
		/* cam=5's sensor: the wide (by default), the ultra-wide or the telephoto; its PHY's timer clock */
		for (unsigned i = 0; i < 3; i++) {
			if (strcmp(which, sensors[i].name) == 0) {
				sel = i;
			}
		}
		fast_rates[1].rcg = phytimer_rcg[PHY_N];
	}
	say("camera: shennong, level %u: %s; cam_sensor=%s\n", level, level == 1 ? "a look, reading only" : level == 2 ?
	    "a sensor powered up, its chip ID read, powered down" : level == 3 ? "the camera's blocks' clocks and versions" :
	    level == 4 ? "the test pattern generator's frames into memory" : cam_once ? "one sensor's frames into memory" :
	    "one sensor's frames for iOS (/dev/redwood-camera)", which);
	for (unsigned i = 0; i < SENSORS; i++) {
		pins_show(&sensors[i]);
	}
	clocks_show();
	supplies_show();
	if (level < 2) {
		goto out;
	}
	if (rpmh_address("ldom2") == 0) {
		say("camera: no command database: nothing written\n");
		goto out;
	}
	rpmh_setup();
	if (level == 3) {
		blocks_look();
		goto out;
	}
	if (level == 4) {
		capture_tpg();
		goto out;
	}
	if (level == 5 && cam_once) {
		capture_sensor();
		goto out;
	}
	if (level == 5) {
		serve_ios(made);
		goto out;
	}
	for (unsigned i = 0; i < SENSORS && !broken; i++) {
		if (strcmp(which, "all") == 0 || strcmp(which, sensors[i].name) == 0) {
			probe(&sensors[i]);
			if (keep && powered != NULL) {
				break;          /* (one sensor kept powered at most) */
			}
		}
	}
out:
	say("camera: done\n");
	thread_terminate(current_thread());
}

void
shennong_camera_init(void)
{
	thread_t thread;

	if (strcmp(redwood_phone(), "shennong") != 0 || !PE_parse_boot_argn("cam", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 5 ? 5 : level;
	PE_parse_boot_argn("cam_once", &cam_once, sizeof(cam_once));
	if (level == 5 && !cam_once) {
		/*
		 * iOS's frames: no pauses for the loader's log (a session starts at a read, which waits), unless cam_pause
		 * asks; and memory in one piece for a picture for the host (cam_keep_mb, default 24), taken now, while
		 * the system's memory is still in large pieces
		 */
		uint32_t keep_mb = 24;

		pause_ms = 0;
		PE_parse_boot_argn("cam_keep_mb", &keep_mb, sizeof(keep_mb));
		keep_mem_bytes = (uint64_t)(keep_mb > 64 ? 64 : keep_mb) << 20;
		if (keep_mem_bytes != 0 && kmem_alloc_contig(kernel_map, &keep_mem, keep_mem_bytes, PAGE_MASK, 0, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("camera: no memory in one piece for a picture for the host (%u MB)\n", keep_mb);
			keep_mem = 0;
			keep_mem_bytes = 0;
		}
	}
	PE_parse_boot_argn("cam_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("cam_keep", &keep, sizeof(keep));
	PE_parse_boot_argn("cam_tries", &tries, sizeof(tries));
	PE_parse_boot_argn("cam_tcs", &our_tcs, sizeof(our_tcs));
	our_tcs = our_tcs > 2 ? 2 : our_tcs;
	tries = tries < 1 ? 1 : tries > 20 ? 20 : tries;
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: a sensor still powered is powered down. */
void
shennong_camera_quiesce(void)
{
	if (level < 2 || (powered == NULL && !blocks_up)) {
		return;
	}
	leaving = true;
	broken = false;
	blocks_down();
	power_down();
}
