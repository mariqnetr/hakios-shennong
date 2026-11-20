/*
 * The vibration motor of the Xiaomi 14 Pro (shennong): a linear resonant actuator driven by the
 * haptics module of the PM8550B (SPMI slave 7), Qualcomm's "HAP525_HV": its configuration block at
 * 0xf000, its pattern block at 0xf100, its boost at 0xf200. Every address and number below belongs to
 * this phone, from its device tree (qcom,hv-haptics@f000 of qcom,pm8550b@7) and Xiaomi's driver for it
 * (drivers/input/misc/qcom-hv-haptics.c of shennong-u-oss), whose names the registers keep:
 *
 *   the motor     f0 130 Hz (qcom,lra-period-us 7692; Android accepts 120..140 Hz from the boot
 *                 loader's calibration), sine drive (qcom,drv-sig-shape 1), closed-loop brake with
 *                 the pattern ff 3f 1f (qcom,brake-mode 1, qcom,brake-pattern)
 *   its strength  Android's constant vibration (FF_CONSTANT: "direct play", one amplitude byte, 0xff
 *                 at full level) at qcom,vmax-mv 1600 mV; its effects at 3600 mV (qcom,wf-vmax-mv)
 *   the boost     revision 0.1 needs to be switched on through the PMK8550's SDAM at 0x9d00 (a PBS
 *                 sequence: argument 0x42, trigger 0xe5); later revisions follow the module by
 *                 themselves (Android's SW_CTRL_HBST)
 *
 * Nothing is driven by hand here: the module is a PMIC peripheral, reached through the SPMI arbiter
 * (version 7, as in redwood_buttons.c: "core" 0xc400000, "cnfg" 0xc42d000, "obsrvr" 0xc440000, and for
 * writes "chnls" 0xc500000, 0x1000 a channel -- Linux spmi-pmic-arb.c, pmic_arb_v7). Writing to a PMIC is
 * not like writing anywhere else, so every write goes through one function that takes only the
 * registers listed below, of the four peripherals above, through channels the arbiter says are ours;
 * every write that can be is read back, and any surprise ends all writing for the run. Transfers go one
 * at a time with the buttons' driver (redwood_spmi_lock()).
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   haptic=1   a look, reading only: the arbiter's channels for the module's three blocks and the
 *              SDAM, who may write through them, the blocks' types and revisions, their registers as
 *              the boot loader leaves them, and what they say (f0, VMAX, the motor's resistance)
 *   haptic=2   ... and Android's configuration for its constant vibration written (every write read
 *              back): the module enabled, VMAX and its headroom, sine drive, brake, the RC clock's
 *              calibration, the period if the boot loader's is not plausible. Nothing moves.
 *   haptic=3   ... and the motor driven: haptic_count times (default 3, at most 10) for haptic_ms each
 *              (default 200, at most HAPTIC_MAX_MS), 600 ms apart, at amplitude haptic_amp (1..255,
 *              default 255: Android's full level) and haptic_mv (default 1600, Android's; at most 3600,
 *              Android's effects')
 *   haptic=4   ... configured as for 2, and instead the motor for iOS until the run ends: what iOS
 *              plays on its "Actuator" device (the HAL plug-in's) is written to /dev/redwood-haptic as
 *              samples of one channel, as on redwood; while it is not silence the motor plays, as
 *              strong as iOS's waveform (Android's full level from a third of full scale up, down to
 *              five eighths of it for the softest), for HAPTIC_MAX_MS at most, after which it rests for
 *              as long. Each waveform of iOS's starts with Android's click (its effect 0, below), and only
 *              one that goes on for longer than the click goes on as direct play.
 *
 * Android's click (effect 0 of the device tree, Android's own taps and keyboard feedback): one pattern of
 * eight samples at 150 Hz (qcom,wf-pattern-period-us 6667), four driving up (0x1f, 0x3f, 0x5f, 0x7f) and four
 * driving against the motor (0x17f down to 0x11f), at qcom,wf-vmax-mv 3600 mV; the module plays it from its
 * pattern registers (the source PATTERN1), auto-resonance off, as haptics_load_predefined_effect() does, and
 * it is stopped after its 53 ms. Its strength follows iOS's waveform through VMAX, as Android's magnitude
 * does (5/8 of 3600 mV for the softest). haptic_click=0: no click, direct play only (as before);
 * haptic_click_mv: the click's VMAX (at most 3600 mV). At haptic=3 the buzzes are followed by as many clicks.
 *
 * HyperOS's own taps, in place of the click when they are there: the waveforms Xiaomi's vibrator HAL
 * plays through RichTap (odm/firmware/N_name_P_RTP.bin of this phone's HyperOS: signed 8-bit samples at
 * 24 kHz), put into fw/ of the small image as rtpN.bin (get-haptic.sh, then build-ios.sh: the vendor's,
 * never in a repository). They are played as Xiaomi's driver plays them (richtap_load_prebake(),
 * richtap_playback()): the RC clock's calibration off, VMAX 6000 mV, the module toggled, the FIFO at
 * 24 kHz flushed and filled, played from the FIFO, stopped when its samples are out, and the play rate
 * and the calibration given back. Which one, by how strong iOS's tap is (all iOS says of it): softer than
 * 2500 of 32768 haptic_fx_light (default 183, signalList), up to 9000 haptic_fx_medium (181, signalKeyUnit),
 * stronger haptic_fx_heavy (166, feedbackNeutral); haptic_fx=0: not used; haptic_fx_mv: their VMAX (at
 * most Xiaomi's 6000 mV). One longer than the FIFO is not used (the FIFO is not refilled here).
 *
 * Limits, whatever asks: Android's VMAX values only (1600 mV, 3600 mV at most; HyperOS's taps from the FIFO at
 * Xiaomi's 6000 mV, no more); no play longer than
 * HAPTIC_MAX_MS; a short circuit or a power fault the module reports, a write that does not read back
 * or that the arbiter refuses, and the play is stopped and nothing more is written for the rest of the
 * run. At the run's end (and on a panic's way out) the play is stopped.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level;
static bool broken, leaving, playing;

static int say(const char *format, ...) __printflike(1, 2);

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
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
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
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* ---- the PMIC arbiter (version 7, bus 0) ----------------------------------------------------------- */

#define ARB_CORE_PHYS           0x0c400000UL    /* the version; which peripheral each channel is, from 0x2000 */
#define ARB_CORE_SIZE           0x3000UL
#define ARB_CNFG_PHYS           0x0c42d000UL    /* which system may write through each channel, from 0 */
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0c440000UL    /* reads: 0x8000 per system, 0x20 per channel; ours is the first */
#define ARB_OBSERVER_SIZE       0x8000UL
#define ARB_CHNLS_PHYS          0x0c500000UL    /* writes: 0x1000 per channel */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_7           0x70000000u
#define ARB_VERSION_8           0x80000000u
#define ARB_FEATURES            0x004           /* [10:0] the number of channels of bus 0 */
#define ARB_CHANNEL(n)          (0x2000 + 4 * (n))      /* [19:8] slave and peripheral */
#define ARB_OWNER(n)            (4 * (n))               /* [2:0] the system that may write through channel n */
#define ARB_MAX_CHANNELS        1024

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define CHANNEL_WDATA1          0x14
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)      /* "extended register write long"; [11:4] register, [2:0] bytes - 1 */
#define COMMAND_READ            (1u << 27)      /* "extended register read long" */
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral */
	uint16_t        channel;
	bool            found, writable;
	vm_offset_t     write;          /* its write channel, mapped */
	const uint8_t * regs;           /* the registers that may be written, and how many */
	unsigned        nregs;
};

/* the registers written, each peripheral's; those that do not read back as written are marked below (no_readback()) */
static const uint8_t cfg_regs[] = { 0x46, 0x48, 0x49, 0x4c, 0x50, 0x5c, 0x5d, 0x63, 0x66, 0x67, 0x68, 0x70, 0x71, 0x72 };
static const uint8_t ptn_regs[] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x26, 0x2a, 0x2c, 0x2d, 0x30, 0x31, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
	                            0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x70, 0x71, 0x72, 0x73, 0x74,
	                            0x75, 0x76, 0x77 };
static const uint8_t sdam_regs[] = { 0x42, 0xe5 };

static struct peripheral cfg = { .name = "the haptics' configuration (PM8550B 0xf000)", .id = 0x7f0, .regs = cfg_regs,
	                         .nregs = sizeof(cfg_regs) };
static struct peripheral ptn = { .name = "the haptics' patterns (PM8550B 0xf100)", .id = 0x7f1, .regs = ptn_regs,
	                         .nregs = sizeof(ptn_regs) };
static struct peripheral hbst = { .name = "the haptics' boost (PM8550B 0xf200)", .id = 0x7f2 };
static struct peripheral sdam = { .name = "the haptics' SDAM (PMK8550 0x9d00)", .id = 0x09d, .regs = sdam_regs,
	                          .nregs = sizeof(sdam_regs) };

static vm_offset_t arb_core, arb_cnfg, arb_observer;

/* (one transfer at a time with the buttons' driver; at the run's end the others are stopped, perhaps holding it) */
static void
lock(void)
{
	if (!leaving) {
		redwood_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		redwood_spmi_unlock();
	}
}

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}

static const char *
spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x8000UL * ARB_SYSTEM + 0x20UL * p->channel;
	const char *problem;
	uint32_t data[2];

	if (!p->found) {
		return "no channel";
	}
	lock();
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = wait_done(channel);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	unlock();
	if (problem != NULL) {
		return problem;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/* `count' (1 to 8) registers from `reg' on; NULL, or what went wrong. A dropped transfer is asked for again, twice at most. */
static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		if ((problem = spmi_read_once(p, reg, bytes, count)) == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

static bool
get(const struct peripheral *p, uint8_t reg, uint8_t *value)
{
	const char *problem = spmi_read(p, reg, value, 1);

	if (problem != NULL) {
		say("haptic: reading %#04x of %s: %s\n", reg, p->name, problem);
		return false;
	}
	return true;
}

/*
 * Which channel is that peripheral's. Several can be, one per system that may write to it: as Linux
 * chooses, the first, unless a later one is ours to write through.
 */
static void
find_channel(struct peripheral *p, uint32_t channels)
{
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != p->id) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		say("haptic: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
		    owner == ARB_SYSTEM ? " (ours)" : "");
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
			p->writable = owner == ARB_SYSTEM;
		}
	}
	if (!p->found) {
		say("haptic: no channel of the arbiter is %s (slave %u, peripheral %#x)\n", p->name, p->id >> 8, p->id & 0xff);
	}
}

static void stop_now(void);

static void
give_up(const char *why)
{
	if (!broken) {
		say("haptic: %s: THE PLAY IS STOPPED and nothing more is written for the rest of the run\n", why);
		stop_now();
		broken = true;
	}
}

/* registers that do not hold what is written: the fault clear, the status selectors, the PBS trigger */
static bool
no_readback(const struct peripheral *p, uint8_t reg)
{
	return (p == &cfg && (reg == 0x66 || reg == 0x70 || reg == 0x71)) || (p == &sdam && reg == 0xe5) ||
	       (p == &ptn && ((reg >= 0x20 && reg <= 0x23) || reg == 0x2c || reg == 0x2d));   /* the FIFO's input, its flush */
}

/*
 * `count' (1 to 8) registers of `p' from `reg' on written, and read back unless they are of those that
 * do not hold what is written: false, and no more writing, if that cannot be done or they do not.
 */
static bool
put_n(struct peripheral *p, uint8_t reg, const uint8_t *values, uint32_t count)
{
	const char *problem = NULL;
	uint32_t data[2] = { 0, 0 };
	uint8_t back[8];

	for (uint32_t i = 0; i < count; i++) {
		bool allowed = false;

		for (unsigned j = 0; j < p->nregs; j++) {
			allowed |= p->regs[j] == (uint8_t)(reg + i);
		}
		if (!allowed) {
			give_up("a register that is not to be written");
			return false;
		}
		data[i / 4] |= (uint32_t)values[i] << (8 * (i % 4));
	}
	if (broken || count < 1 || count > 8 || !p->writable || p->write == 0) {
		give_up("a write that may not be made");
		return false;
	}
	/* (the channel is still the peripheral's: its entry in the arbiter's table is looked at again) */
	if (((rd(arb_core + ARB_CHANNEL(p->channel)) >> 8) & 0xfff) != p->id) {
		give_up("the arbiter's channel is no longer the peripheral's");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(p->write + CHANNEL_WDATA0, data[0]);
		if (count > 4) {
			wr(p->write + CHANNEL_WDATA1, data[1]);
		}
		wr(p->write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4 | (count - 1));
		problem = wait_done(p->write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;          /* (a dropped write has not been made: asked for again) */
		}
	}
	if (problem != NULL) {
		say("haptic: %#04x of %s <- %#04x...: %s\n", reg, p->name, values[0], problem);
		give_up("a write that failed");
		return false;
	}
	if (no_readback(p, reg)) {
		return true;
	}
	if ((problem = spmi_read(p, reg, back, count)) != NULL) {
		say("haptic: %#04x of %s read back: %s\n", reg, p->name, problem);
		give_up("a write that could not be read back");
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (back[i] != values[i]) {
			say("haptic: %#04x of %s <- %#04x, but it reads %#04x\n", reg + i, p->name, values[i], back[i]);
			give_up("a register that did not read back as written");
			return false;
		}
	}
	return true;
}

static bool
put(struct peripheral *p, uint8_t reg, uint8_t value)
{
	return put_n(p, reg, &value, 1);
}

/* only the bits of `mask' changed, as the driver's masked writes do */
static bool
put_bits(struct peripheral *p, uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	if (!get(p, reg, &now)) {
		give_up("a register that could not be read");
		return false;
	}
	return put(p, reg, (uint8_t)((now & ~mask) | (value & mask)));
}

/* ---- the haptics module ---------------------------------------------------------------------------- */

#define CFG_REVISION2           0x01            /* 4: HAP525_HV (with the pattern block's 4) */
#define CFG_STATUS_DATA_MSB     0x09            /* what MOD_STATUS_SEL selects */
#define CFG_STATUS_DATA_LSB     0x0a
#define CFG_HPWR_INTF           0x0b            /* [1:0] 0 disabled, 3 ready */
#define CFG_FAULT_STATUS        0x0c            /* [2] short circuit, [1] auto-resonance error, [0] power not ready */
#define CFG_HW_CONFIG           0x0d            /* [1] the high-voltage driver */
#define CFG_LRA_IMPEDANCE       0x0e
#define CFG_INT_RT_STS          0x10
#define CFG_EN_CTL              0x46            /* [7] the module on */
#define CFG_VMAX                0x48            /* 50 mV steps (the high-voltage driver) */
#define CFG_DRV_WF_SEL          0x49            /* [4] 2's complement, [1:0] 0 square, 1 sine */
#define CFG_SPMI_PLAY           0x4c            /* [7] play, [3] brake, [2:0] the source: 1 direct play */
#define CFG_BRAKE_MODE_CFG      0x50            /* [7:6] mode, [3:2] sine gain, [1:0] shape */
#define CFG_TLRA_OL_HIGH        0x5c            /* the open-loop period, 5 us steps, [3:0] of the high byte */
#define CFG_TLRA_OL_LOW         0x5d
#define CFG_AUTORES_CFG         0x63            /* [7] auto-resonance */
#define CFG_FAULT_CLR           0x66
#define CFG_VMAX_HDRM           0x67            /* 50 mV steps */
#define CFG_VSET_CFG            0x68            /* [1] force VSET ack, [0] force VREG ready */
#define CFG_MOD_STATUS_SEL      0x70
#define CFG_MOD_STATUS_XT       0x71
#define CFG_CAL_EN              0x72            /* [3:2] the RC clock's calibration: 0 off, 1 auto, 2 manual */

#define PTN_REVISION2           0x01
#define PTN_DIRECT_PLAY         0x26            /* the amplitude of direct play */
#define PTN_BRAKE_AMP           0x70            /* eight samples */
#define PTN_PTRN1_TLRA          0x30            /* PATTERN1's period, 5 us steps: [3:0] of 0x30, then 0x31 */
#define PTN_PTRN1_CFG           0x34            /* PATTERN1's eight samples, three bytes each: [7] f x2, [2:0] the
                                                   period (0 T_LRA); [0] the amplitude's ninth bit; its low byte */

#define HBST_REVISION1          0x00
#define HBST_REVISION2          0x01
#define HBST_STATUS4            0x0b            /* [0] busy */
#define HBST_HW_CTRL_FOLLOW     0x41            /* [7] follows the module */
#define HBST_VREG_EN            0x46            /* [7] on */
#define HBST_CLAMP              0x70            /* [0] clamped at 5 V */

#define SDAM_PBS_ARG            0x42            /* 1 the boost on, 2 off */
#define SDAM_PBS_TRIG_SET       0xe5
#define SDAM_NOMINAL_OHM        0x75            /* the motor's resistance, 250 mOhm steps */
#define SDAM_DETECTED_OHM       0x76

#define SPMI_PLAY_EN            0x80
#define SRC_DIRECT_PLAY         0x01
#define SRC_PATTERN1            0x02
#define SRC_FIFO                0x00
#define PTN_FIFO_DIN            0x20            /* four samples at a time */
#define PTN_FIFO_PLAY_RATE      0x24            /* [3:0]: 10 is 24 kHz, 0 T_LRA */
#define PTN_FIFO_EMPTY_CFG      0x2a            /* [3:0] the empty threshold, 32 samples a step */
#define PTN_FIFO_DIN_1B         0x2c            /* one sample */
#define PTN_MEM_OP_ACCESS       0x2d            /* [0] flush and reload */
#define PTN_MMAP_FIFO           0xa0            /* [7] a FIFO, [3:0] its length in 128 bytes, less one */
#define FIFO_24KHZ              10
#define HYPEROS_FX_MV           6000            /* richtap_load_prebake(): haptics_set_vmax_mv(chip, 6000) */
#define HPWR_DISABLED           0
#define HPWR_READY              3
#define FAULT_SC                0x04
#define FAULT_HPWR_RDY          0x01

#define ANDROID_TLRA_US         7692            /* qcom,lra-period-us: 130 Hz */
#define F0_MIN_HZ               120             /* qcom,lra-f0-min, -max */
#define F0_MAX_HZ               140
#define ANDROID_VMAX_MV         1600            /* qcom,vmax-mv: its constant vibration */
#define ANDROID_EFFECT_VMAX_MV  3600            /* qcom,wf-vmax-mv: its effects, the most ever set here */
#define BRAKE_CFG               0x41            /* closed-loop brake (1 << 6), sine gain x1, sine shape */
#define HAPTIC_MAX_MS           1000

static const uint8_t brake_pattern[8] = { 0xff, 0x3f, 0x1f, 0, 0, 0, 0, 0 };     /* qcom,brake-pattern */
static const uint16_t click_samples[8] = { 0x1f, 0x3f, 0x5f, 0x7f, 0x17f, 0x15f, 0x13f, 0x11f };    /* effect_0 */
#define CLICK_PERIOD_US         6667            /* effect_0's qcom,wf-pattern-period-us */
#define CLICK_US                (8 * CLICK_PERIOD_US)
static uint32_t click = 1, click_mv = ANDROID_EFFECT_VMAX_MV, constant_mv = ANDROID_VMAX_MV;
static bool click_loaded;

/* HyperOS's taps: three, by strength */
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static uint32_t fx = 1, fx_mv = HYPEROS_FX_MV, fx_number[3] = { 183, 181, 166 }, fifo_bytes;
static const uint8_t *fx_data[3];
static uint32_t fx_size[3];
static bool fx_looked;
static const char *const fx_tier[3] = { "light", "medium", "heavy" };
static uint32_t t_lra_us = ANDROID_TLRA_US;
static uint32_t nominal_ohm;            /* from the SDAM, 0 if it does not say */
static bool sw_boost;                   /* the boost is to be switched on and off through the SDAM (revision 0.1) */
static uint32_t vmax_now;

static void
show(struct peripheral *p, const char *what, const uint8_t *regs, unsigned n)
{
	char line[200];
	size_t at = 0;

	at += (size_t)snprintf(line, sizeof(line), "haptic: %s:", what);
	for (unsigned i = 0; i < n && at < sizeof(line) - 12; i++) {
		uint8_t v;

		if (spmi_read(p, regs[i], &v, 1) == NULL) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %02x=%02x", regs[i], v);
		} else {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %02x=??", regs[i]);
		}
	}
	say("%s\n", line);
}

static void
show_registers(void)
{
	static const uint8_t c1[] = { 0x01, 0x04, 0x05, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x10, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c,
		                      0x4d, 0x4e };
	static const uint8_t c2[] = { 0x50, 0x51, 0x52, 0x53, 0x5a, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x67,
		                      0x68, 0x70, 0x71, 0x72, 0x77, 0x80, 0x84, 0xa4 };
	static const uint8_t p1[] = { 0x01, 0x04, 0x05, 0x08, 0x09, 0x24, 0x26, 0x28, 0x2a, 0x2b, 0x2d, 0x70, 0x71, 0x72, 0x73, 0xa0, 0xa1 };
	static const uint8_t b1[] = { 0x00, 0x01, 0x04, 0x05, 0x0b, 0x41, 0x46, 0x70 };
	static const uint8_t s1[] = { 0x04, 0x05, 0x42, 0x46, 0x47, 0x4b, 0x64, 0x65, 0x75, 0x76, 0x7e, 0xe5 };

	show(&cfg, "cfg", c1, sizeof(c1));
	show(&cfg, "cfg", c2, sizeof(c2));
	show(&ptn, "ptn", p1, sizeof(p1));
	if (hbst.found) {
		show(&hbst, "hbst", b1, sizeof(b1));
	}
	if (sdam.found) {
		show(&sdam, "sdam", s1, sizeof(s1));
	}
}

/* What the registers say, in Android's words. False if the module is not the one this driver knows. */
static bool
look(void)
{
	uint8_t cfg_rev = 0, ptn_rev = 0, hw = 0, v[2] = { 0, 0 }, en = 0, vmax = 0, hdrm = 0, cal = 0, b[2] = { 0, 0 }, ohm = 0, det = 0;
	uint32_t tlra;

	if (!get(&cfg, CFG_REVISION2, &cfg_rev) || !get(&ptn, PTN_REVISION2, &ptn_rev)) {
		return false;
	}
	say("haptic: revisions: configuration %u, patterns %u%s\n", cfg_rev, ptn_rev, cfg_rev == 4 && ptn_rev == 4 ?
	    ": HAP525_HV, as Android's driver expects on this phone" : ": NOT HAP525_HV (4 and 4): left alone");
	show_registers();
	if (cfg_rev != 4 || ptn_rev != 4) {
		return false;
	}
	(void)get(&cfg, CFG_HW_CONFIG, &hw);
	(void)get(&cfg, CFG_EN_CTL, &en);
	(void)get(&cfg, CFG_VMAX, &vmax);
	(void)get(&cfg, CFG_VMAX_HDRM, &hdrm);
	(void)get(&cfg, CFG_CAL_EN, &cal);
	(void)spmi_read(&cfg, CFG_TLRA_OL_HIGH, v, 2);
	tlra = ((uint32_t)(v[0] & 0x0f) << 8 | v[1]) * 5;
	say("haptic: the module is %s; %s driver; VMAX %u mV, headroom %u mV; open-loop period %u us (%u Hz); RC clock "
	    "calibration %s\n", (en & 0x80) ? "on" : "OFF", (hw & 0x02) ? "the high-voltage" : "the MEDIUM-voltage", vmax * 50,
	    (hdrm & 0x7f) * 50, tlra, tlra ? 1000000 / tlra : 0, ((cal >> 2) & 3) == 1 ? "auto" : ((cal >> 2) & 3) == 0 ? "off" : "manual");
	if (!(hw & 0x02)) {
		say("haptic: not the high-voltage driver Android's VMAX steps are for: left alone\n");
		return false;
	}
	if (tlra != 0 && 1000000 / tlra >= F0_MIN_HZ && 1000000 / tlra <= F0_MAX_HZ) {
		t_lra_us = tlra;
		say("haptic: the boot loader's period is plausible (%u..%u Hz): it is kept\n", F0_MIN_HZ, F0_MAX_HZ);
	} else {
		say("haptic: the period is not within %u..%u Hz: Android's %u us is to be written\n", F0_MIN_HZ, F0_MAX_HZ, ANDROID_TLRA_US);
	}
	if (hbst.found && spmi_read(&hbst, HBST_REVISION1, b, 2) == NULL) {
		uint8_t follow = 0, vreg = 0, clamp = 0;

		(void)get(&hbst, HBST_HW_CTRL_FOLLOW, &follow);
		(void)get(&hbst, HBST_VREG_EN, &vreg);
		(void)get(&hbst, HBST_CLAMP, &clamp);
		sw_boost = (b[1] << 8 | b[0]) == 0x0001;
		say("haptic: the boost: revision %#06x (%s), %s, %s the module, %s\n", b[1] << 8 | b[0], sw_boost ?
		    "0.1: switched on and off through the SDAM, as Android does" : "follows the module by itself", (vreg & 0x80) ? "on" : "off",
		    (follow & 0x80) ? "follows" : "does NOT follow", (clamp & 1) ? "CLAMPED AT 5 V" : "not clamped");
	}
	if (sdam.found && get(&sdam, SDAM_NOMINAL_OHM, &ohm) && get(&sdam, SDAM_DETECTED_OHM, &det)) {
		nominal_ohm = ohm * 250 / 1000;
		say("haptic: the motor: nominal %u.%02u Ohm, detected %u.%02u Ohm (the SDAM)\n", ohm * 250 / 1000, ohm * 250 % 1000 / 10,
		    det * 250 / 1000, det * 250 % 1000 / 10);
	}
	return true;
}

/* the play stopped at once, without the lock if the run is ending: the source kept, the play bit cleared */
static void
stop_now(void)
{
	if (cfg.write == 0 || !cfg.writable) {
		return;
	}
	lock();
	wr(cfg.write + CHANNEL_WDATA0, SRC_DIRECT_PLAY);
	wr(cfg.write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)CFG_SPMI_PLAY << 4);
	(void)wait_done(cfg.write);
	unlock();
	playing = false;
}

/* until the module's power interface says disabled or ready, 10 ms at most; if neither, the module toggled (as Android does) */
static bool
hpwr_settled(void)
{
	uint8_t v = 0xff;

	for (int i = 0; i < 10; i++) {
		if (!get(&cfg, CFG_HPWR_INTF, &v)) {
			return false;
		}
		if ((v & 3) == HPWR_DISABLED || (v & 3) == HPWR_READY) {
			return true;
		}
		wait_ms(1);
	}
	say("haptic: the power interface stays at %u after VMAX was set: the module is toggled off and on\n", v & 3);
	spin_us(100);
	if (!put(&cfg, CFG_EN_CTL, 0)) {
		return false;
	}
	spin_us(100);
	return put(&cfg, CFG_EN_CTL, 0x80);
}

/* VMAX and its headroom as Android's haptics_set_vmax_mv() sets them */
static bool
set_vmax(uint32_t mv)
{
	uint32_t hdrm = 500;

	if (mv > HYPEROS_FX_MV) {
		mv = HYPEROS_FX_MV;
	}
	if (mv == vmax_now) {
		return true;
	}
	if (nominal_ohm != 0) {
		hdrm = (nominal_ohm >= 15 && mv > 4500) ? 1000 : (nominal_ohm < 15 && mv > 3500) ? 1000 : 500;
		if (!put(&cfg, CFG_VMAX_HDRM, (uint8_t)(hdrm / 50))) {
			return false;
		}
	}
	if (!put(&cfg, CFG_VMAX, (uint8_t)(mv / 50)) || !hpwr_settled()) {
		return false;
	}
	vmax_now = mv;
	return true;
}

/*
 * Android's configuration for its constant vibration (haptics_hw_init(), haptics_load_constant_effect()):
 * the module on, VMAX, sine drive, the brake, the RC clock's calibration on auto, the period, direct play
 * at 0 and auto-resonance off.
 */
static bool
configure(uint32_t vmax_mv)
{
	uint8_t en = 0;
	uint32_t tlra = t_lra_us / 5;

	say("haptic: Android's configuration is written\n");
	if (!get(&cfg, CFG_EN_CTL, &en) || (!(en & 0x80) && !put(&cfg, CFG_EN_CTL, 0x80))) {
		return false;
	}
	if (!set_vmax(vmax_mv) ||
	    !put_bits(&cfg, CFG_DRV_WF_SEL, 0x13, 0x01) ||
	    !put_bits(&cfg, CFG_BRAKE_MODE_CFG, 0xcf, BRAKE_CFG) ||
	    !put_n(&ptn, PTN_BRAKE_AMP, brake_pattern, 8) ||
	    !put_bits(&cfg, CFG_CAL_EN, 0x0c, 1 << 2) ||
	    !put(&cfg, CFG_TLRA_OL_HIGH, (uint8_t)((tlra >> 8) & 0x0f)) || !put(&cfg, CFG_TLRA_OL_LOW, (uint8_t)tlra) ||
	    !put(&ptn, PTN_DIRECT_PLAY, 0) ||
	    !put_bits(&cfg, CFG_AUTORES_CFG, 0x80, 0)) {
		return false;
	}
	say("haptic: configured: VMAX %u mV, sine drive, closed-loop brake ff 3f 1f, period %u us, direct play\n", vmax_now, tlra * 5);
	return true;
}

/* the boost on or off through the PMIC's sequencer, as Android's haptics_boost_vreg_enable() (revision 0.1 only) */
static bool
boost(bool on)
{
	if (!sw_boost) {
		return true;
	}
	if (!sdam.writable) {
		give_up("the boost must be switched through the SDAM, which is not ours to write");
		return false;
	}
	return put(&sdam, SDAM_PBS_ARG, on ? 1 : 2) && put(&sdam, SDAM_PBS_TRIG_SET, 1);
}

/* the module's faults: false, and the play stopped, on a short circuit or power not ready */
static bool
healthy(const char *when)
{
	uint8_t f = 0;

	if (!get(&cfg, CFG_FAULT_STATUS, &f)) {
		give_up("the faults could not be read");
		return false;
	}
	if (f & (FAULT_SC | FAULT_HPWR_RDY)) {
		say("haptic: %s: the module reports %#04x:%s%s%s\n", when, f, (f & FAULT_SC) ? " SHORT CIRCUIT" : "",
		    (f & 0x02) ? " auto-resonance error" : "", (f & FAULT_HPWR_RDY) ? " POWER NOT READY" : "");
		give_up("a fault");
		return false;
	}
	return true;
}

/* direct play on at `amplitude' (Android's haptics_enable_play(true) for DIRECT_PLAY) */
static bool
play_start(uint8_t amplitude)
{
	uint8_t clear = 0x07;

	if (!put(&ptn, PTN_DIRECT_PLAY, amplitude) || !put_n(&cfg, CFG_FAULT_CLR, &clear, 1) ||
	    !put_bits(&cfg, CFG_VSET_CFG, 0x03, 0) || !put(&cfg, CFG_SPMI_PLAY, SPMI_PLAY_EN | SRC_DIRECT_PLAY)) {
		return false;
	}
	playing = true;
	return boost(true);
}

/* stopped, and the brake waited for (Android's haptics_enable_play(false), haptics_wait_brake_complete()) */
static bool
play_stop_from(uint8_t source)
{
	uint32_t waited = 0, longest = (t_lra_us * 3 / 2 + t_lra_us) / 1000 + 2;
	uint8_t v = 0xff;

	if (!put(&cfg, CFG_SPMI_PLAY, source)) {
		return false;
	}
	playing = false;
	for (; waited <= longest; waited++) {
		if (!get(&cfg, CFG_HPWR_INTF, &v)) {
			return false;
		}
		if ((v & 3) == HPWR_DISABLED) {
			break;
		}
		wait_ms(1);
	}
	if ((v & 3) != HPWR_DISABLED) {
		say("haptic: still not disabled %u ms after the stop (the brake's %u ms): the module is toggled off and on\n", waited,
		    longest);
		if (!put(&cfg, CFG_EN_CTL, 0) || !put(&cfg, CFG_EN_CTL, 0x80)) {
			return false;
		}
	}
	return boost(false);
}

static bool
play_stop(void)
{
	return play_stop_from(SRC_DIRECT_PLAY);
}

/* Android's click put into PATTERN1, once: its period and its eight samples (haptics_set_pattern()) */
static bool
click_load(void)
{
	uint32_t tlra = CLICK_PERIOD_US / 5;
	uint8_t period[2] = { (uint8_t)((tlra >> 8) & 0x0f), (uint8_t)tlra }, v[24];

	if (click_loaded) {
		return true;
	}
	for (unsigned i = 0; i < 8; i++) {
		v[3 * i] = 0;                                   /* T_LRA, not doubled */
		v[3 * i + 1] = (uint8_t)(click_samples[i] >> 8 & 1);
		v[3 * i + 2] = (uint8_t)click_samples[i];
	}
	if (!put_n(&ptn, PTN_PTRN1_TLRA, period, 2) || !put_n(&ptn, PTN_PTRN1_CFG, v, 8) ||
	    !put_n(&ptn, PTN_PTRN1_CFG + 8, v + 8, 8) || !put_n(&ptn, PTN_PTRN1_CFG + 16, v + 16, 8)) {
		return false;
	}
	click_loaded = true;
	say("haptic: Android's click is in PATTERN1: 8 samples at %u us (%u ms), up to %u mV\n", CLICK_PERIOD_US, CLICK_US / 1000,
	    click_mv);
	return true;
}

/* the click played at `amplitude' of its VMAX (Android: magnitude * wf-vmax-mv / 0x7fff) */
static bool
click_start(uint8_t amplitude)
{
	uint8_t clear = 0x07;
	uint32_t mv = click_mv * amplitude / 255 / 50 * 50;

	mv = mv < 1000 ? 1000 : mv;
	if (!click_load() || !set_vmax(mv) || !put_n(&cfg, CFG_FAULT_CLR, &clear, 1) || !put_bits(&cfg, CFG_VSET_CFG, 0x03, 0) ||
	    !put(&cfg, CFG_SPMI_PLAY, SPMI_PLAY_EN | SRC_PATTERN1)) {
		return false;
	}
	playing = true;
	return boost(true);
}

static bool
click_once(uint8_t amplitude, unsigned n)
{
	uint64_t began;

	if (!healthy("before the click") || !click_start(amplitude)) {
		return false;
	}
	began = now_us();
	wait_ms(CLICK_US / 1000 + 1);
	if (!play_stop_from(SRC_PATTERN1) || !set_vmax(constant_mv)) {
		return false;
	}
	say("haptic: click %u at VMAX %u mV: stopped after %llu ms\n", n, click_mv * amplitude / 255 / 50 * 50,
	    (now_us() - began) / 1000);
	return healthy("after the click");
}

/* HyperOS's taps looked for in fw/ once, the first time one is wanted; those that fit the FIFO kept */
static void
fx_look(void)
{
	uint8_t mmap = 0;

	if (fx_looked) {
		return;
	}
	fx_looked = true;
	if (!get(&ptn, PTN_MMAP_FIFO, &mmap)) {
		return;
	}
	fifo_bytes = (mmap & 0x80) ? ((mmap & 0x0f) + 1u) * 128 : 0;
	for (unsigned i = 0; i < 3; i++) {
		char name[32];
		uint64_t size = 0;
		const uint8_t *data;

		snprintf(name, sizeof(name), "rtp%u.bin", fx_number[i]);
		data = redwood_image_file("fw", name, &size, NULL);
		if (data == NULL || size == 0) {
			say("haptic: HyperOS's %s tap: no fw/%s (get-haptic.sh, then build-ios.sh): Android's click instead\n", fx_tier[i],
			    name);
		} else if (size > fifo_bytes) {
			say("haptic: HyperOS's %s tap: fw/%s is %llu samples, longer than the FIFO's %u: Android's click instead\n",
			    fx_tier[i], name, size, fifo_bytes);
		} else {
			fx_data[i] = data;
			fx_size[i] = (uint32_t)size;
			say("haptic: HyperOS's %s tap: fw/%s, %u samples (%u ms at 24 kHz)\n", fx_tier[i], name, fx_size[i],
			    fx_size[i] / 24);
		}
	}
}

/* which of the three for a tap of iOS's that peaks at `peak' (of 32768) */
static int
fx_for(uint32_t peak)
{
	int i = peak < 2500 ? 0 : peak < 9000 ? 1 : 2;

	if (!fx) {
		return -1;
	}
	fx_look();
	return fx_data[i] != NULL ? i : -1;
}

/* HyperOS's tap `i' into the FIFO and played, as richtap_load_prebake() and richtap_playback() do */
static bool
fx_start(int i)
{
	uint8_t clear = 0x07, flush = 1, done = 0;
	const uint8_t *d = fx_data[i];
	uint32_t n = fx_size[i], at = 0;

	if (!put_bits(&cfg, CFG_CAL_EN, 0x0c, 0) || !set_vmax(fx_mv)) {          /* the RC clock's calibration off */
		return false;
	}
	spin_us(100);
	if (!put(&cfg, CFG_EN_CTL, 0)) {                                                /* a clear start for the FIFO */
		return false;
	}
	spin_us(100);
	if (!put(&cfg, CFG_EN_CTL, 0x80) || !put_bits(&ptn, PTN_FIFO_PLAY_RATE, 0x0f, FIFO_24KHZ) ||
	    !put_n(&ptn, PTN_MEM_OP_ACCESS, &flush, 1) || !put_n(&ptn, PTN_MEM_OP_ACCESS, &done, 1)) {
		return false;
	}
	for (; at + 4 <= n; at += 4) {
		if (!put_n(&ptn, PTN_FIFO_DIN, d + at, 4)) {
			return false;
		}
	}
	for (; at < n; at++) {
		if (!put_n(&ptn, PTN_FIFO_DIN_1B, d + at, 1)) {
			return false;
		}
	}
	if (!put_bits(&ptn, PTN_FIFO_EMPTY_CFG, 0x0f, 0) || !put_n(&cfg, CFG_FAULT_CLR, &clear, 1) ||
	    !put_bits(&cfg, CFG_VSET_CFG, 0x03, 0) || !put(&cfg, CFG_SPMI_PLAY, SPMI_PLAY_EN | SRC_FIFO)) {
		return false;
	}
	playing = true;
	return boost(true);
}

/* stopped, and what the FIFO's play changed given back (haptics_stop_fifo_play()) */
static bool
fx_stop(void)
{
	return play_stop_from(SRC_FIFO) && put_bits(&ptn, PTN_FIFO_PLAY_RATE, 0x0f, 0) && put_bits(&cfg, CFG_CAL_EN, 0x0c, 1 << 2) &&
	       set_vmax(constant_mv);
}

static bool
fx_once(int i, unsigned n)
{
	uint64_t began;

	if (!healthy("before HyperOS's tap") || !fx_start(i)) {
		return false;
	}
	began = now_us();
	wait_ms(fx_size[i] / 24 + 2);
	if (!fx_stop()) {
		return false;
	}
	say("haptic: HyperOS's %s tap %u (rtp%u) at VMAX %u mV: stopped after %llu ms\n", fx_tier[i], n, fx_number[i], fx_mv,
	    (now_us() - began) / 1000);
	return healthy("after HyperOS's tap");
}

/* what the module drives from, by its status selector (HAP_DRV_STS: [2:0] of the second byte, 1 direct play) */
static void
show_drive(const char *when)
{
	uint8_t xt = 0, sel = 5, v[2] = { 0, 0 }, hp = 0, f = 0;

	if (!put_n(&cfg, CFG_MOD_STATUS_XT, &xt, 1) || !put_n(&cfg, CFG_MOD_STATUS_SEL, &sel, 1)) {
		return;
	}
	(void)spmi_read(&cfg, CFG_STATUS_DATA_MSB, v, 2);
	(void)get(&cfg, CFG_HPWR_INTF, &hp);
	(void)get(&cfg, CFG_FAULT_STATUS, &f);
	say("haptic: %s: drive status %02x %02x (source %u), power interface %u (3 ready), faults %#04x\n", when, v[0], v[1], v[1] & 7,
	    hp & 3, f);
}

static bool
buzz(uint32_t ms, uint8_t amplitude, unsigned n)
{
	uint64_t began;

	say("haptic: buzz %u: direct play at %#04x of VMAX %u mV for %u ms\n", n, amplitude, vmax_now, ms);
	if (!healthy("before playing") || !play_start(amplitude)) {
		return false;
	}
	began = now_us();
	wait_ms(ms < 20 ? ms : 20);
	show_drive("20 ms in");
	while (now_us() - began < ms * 1000ull && playing) {
		wait_ms(10);
		if (!healthy("while playing")) {
			return false;
		}
	}
	if (!play_stop()) {
		return false;
	}
	say("haptic: buzz %u: stopped after %llu ms\n", n, (now_us() - began) / 1000);
	return healthy("after playing");
}

/* ---- haptic=4: the motor for iOS ------------------------------------------------------------------- */

#define ACTIVE_US               30000           /* a write that was not silence counts for this long */
#define SILENT_BELOW            256             /* of 32768: samples no larger are silence */
#define FOLLOW_US               20000           /* how often the strength follows iOS's */
#define TOLD_EVENTS             24

static volatile uint64_t heard_us, quiet_us, onset_us;
static volatile uint32_t heard_peak;
static uint64_t written_frames;
static unsigned plays, capped;

/* From the audio server's I/O thread: never waits. */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		if (peak > SILENT_BELOW) {
			uint64_t now = now_us();

			if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
				onset_us = now;
				heard_peak = 0;
			}
			heard_us = now;
			if (peak > heard_peak) {
				heard_peak = peak;
			}
		} else {
			quiet_us = now_us();
		}
	}
	return error;
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* Android's full level from a third of full scale up, down to five eighths of it for the softest (as on redwood) */
static uint8_t
amplitude_for(uint32_t peak, uint8_t top)
{
	uint32_t low = top * 5u / 8, third = 32767 / 3;

	return (uint8_t)(peak >= third ? top : low + (top - low) * peak / third);
}

static void
serve(uint8_t top)
{
	enum { IDLE, CLICK, AFTER, PLAY, REST } state = IDLE;
	uint64_t since = 0, rest_until = 0, followed = 0, last_look = 0, clicked = 0, tap_us = CLICK_US;
	int tap_fx = -1;
	uint8_t now_amp = 0;
	int major = cdevsw_add(-1, &haptic_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-haptic") == NULL) {
		say("haptic: /dev/redwood-haptic cannot be made\n");
		return;
	}
	say("haptic: THE MOTOR PLAYS WHAT IOS PLAYS ON ITS ACTUATOR (/dev/redwood-haptic) from here on, until the run ends\n");
	while (!broken) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;
		uint32_t peak = heard_peak;
		uint8_t amp = amplitude_for(peak, top);

		switch (state) {
		case IDLE:
			if (!active || onset_us == clicked) {
				break;
			}
			/* iOS's waveform begins: HyperOS's tap for its strength first, else Android's click, else direct play */
			clicked = onset_us;
			tap_fx = fx_for(peak);
			plays++;
			if (tap_fx >= 0 || click) {
				if (tap_fx >= 0 ? !fx_start(tap_fx) : !click_start(amp ? amp : 1)) {
					break;
				}
				tap_us = tap_fx >= 0 ? fx_size[tap_fx] * 1000ull / 24 + 1000 : CLICK_US;
				state = CLICK;
				since = now;
				if (plays <= TOLD_EVENTS && tap_fx >= 0) {
					say("haptic: play %u: iOS's waveform peaks at %u of 32768: HyperOS's %s tap (rtp%u, %u ms) at %u mV\n", plays,
					    peak, fx_tier[tap_fx], fx_number[tap_fx], fx_size[tap_fx] / 24, fx_mv);
				} else if (plays <= TOLD_EVENTS) {
					say("haptic: play %u: iOS's waveform peaks at %u of 32768: Android's click at %u mV\n", plays, peak,
					    click_mv * amp / 255 / 50 * 50);
				}
			} else {
				if (!play_start(amp ? amp : 1)) {
					break;
				}
				state = PLAY;
				since = followed = now;
				now_amp = amp;
				if (plays <= TOLD_EVENTS) {
					say("haptic: play %u: iOS's waveform peaks at %u of 32768: amplitude %#04x\n", plays, peak, amp);
				}
			}
			break;
		case CLICK:
			if (now - since >= tap_us) {
				if (tap_fx >= 0 ? !fx_stop() : (!play_stop_from(SRC_PATTERN1) || !set_vmax(constant_mv))) {
					break;
				}
				state = AFTER;
			}
			break;
		case AFTER:
			/* a waveform longer than the click goes on as direct play; a new one is clicked again */
			if (!active || onset_us != clicked) {
				state = IDLE;
				if (plays <= TOLD_EVENTS) {
					say("haptic: play %u: a tap (%llu ms)\n", plays, (now - since) / 1000);
				}
			} else if (heard_us - onset_us >= (tap_us > CLICK_US ? tap_us : CLICK_US)) {
				if (!play_start(amp ? amp : 1)) {
					break;
				}
				state = PLAY;
				followed = now;
				now_amp = amp;
				if (plays <= TOLD_EVENTS) {
					say("haptic: play %u: iOS's waveform goes on: direct play at %#04x after the tap\n", plays, amp);
				}
			}
			break;
		case PLAY:
			if (active && now - followed >= FOLLOW_US) {
				followed = now;
				heard_peak = 0;
				if (peak != 0 && (amp > now_amp + 8 || amp + 8 < now_amp)) {
					if (!put(&ptn, PTN_DIRECT_PLAY, amp)) {
						break;
					}
					now_amp = amp;
				}
			}
			if (!active || now - since > HAPTIC_MAX_MS * 1000ull) {
				bool cut = active;

				if (cut) {
					capped++;
					rest_until = now + HAPTIC_MAX_MS * 1000ull;
				}
				if (play_stop()) {
					state = cut ? REST : IDLE;
					if (plays <= TOLD_EVENTS) {
						say("haptic: play %u: %s after %llu ms\n", plays, cut ? "CUT at the limit" : "iOS stopped, and so does the motor",
						    (now - since) / 1000);
					}
				}
			}
			break;
		case REST:
			if (now >= rest_until) {
				state = IDLE;
			}
			break;
		}
		if ((state == PLAY || state == CLICK) && now - last_look > 50000) {
			last_look = now;
			if (!healthy("while playing")) {
				break;
			}
		}
		wait_ms(4);
	}
	say("haptic: the motor is given up on (%u plays, %u cut at %u ms)\n", plays, capped, HAPTIC_MAX_MS);
}

/* ---- the run ----------------------------------------------------------------------------------------- */

static void
haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t version, channels, mv = ANDROID_VMAX_MV, amp = 255, ms = 200, count = 3;

	say("haptic: shennong, level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ?
	    "Android's configuration written, nothing moves" : level == 3 ? "the motor driven a few times" : "the motor for iOS");
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (version < ARB_VERSION_7 || version >= ARB_VERSION_8) {
		say("haptic: PMIC arbiter version %#x, not 7: nothing done\n", version);
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	say("haptic: PMIC arbiter version %#x, %u channels on bus 0\n", version, channels);
	find_channel(&cfg, channels);
	find_channel(&ptn, channels);
	find_channel(&hbst, channels);
	find_channel(&sdam, channels);
	if (!cfg.found || !ptn.found || !look() || level < 2) {
		goto out;
	}
	if (!cfg.writable || !ptn.writable) {
		say("haptic: the module's channels are not ours to write through: nothing written\n");
		goto out;
	}
	cfg.write = ml_io_map(ARB_CHNLS_PHYS + 0x1000UL * cfg.channel, 0x1000);
	ptn.write = ml_io_map(ARB_CHNLS_PHYS + 0x1000UL * ptn.channel, 0x1000);
	if (sdam.writable) {
		sdam.write = ml_io_map(ARB_CHNLS_PHYS + 0x1000UL * sdam.channel, 0x1000);
	}
	PE_parse_boot_argn("haptic_mv", &mv, sizeof(mv));
	PE_parse_boot_argn("haptic_amp", &amp, sizeof(amp));
	PE_parse_boot_argn("haptic_ms", &ms, sizeof(ms));
	PE_parse_boot_argn("haptic_count", &count, sizeof(count));
	PE_parse_boot_argn("haptic_click", &click, sizeof(click));
	PE_parse_boot_argn("haptic_click_mv", &click_mv, sizeof(click_mv));
	click_mv = click_mv < 1000 ? 1000 : click_mv > ANDROID_EFFECT_VMAX_MV ? ANDROID_EFFECT_VMAX_MV : click_mv;
	PE_parse_boot_argn("haptic_fx", &fx, sizeof(fx));
	PE_parse_boot_argn("haptic_fx_mv", &fx_mv, sizeof(fx_mv));
	PE_parse_boot_argn("haptic_fx_light", &fx_number[0], sizeof(fx_number[0]));
	PE_parse_boot_argn("haptic_fx_medium", &fx_number[1], sizeof(fx_number[1]));
	PE_parse_boot_argn("haptic_fx_heavy", &fx_number[2], sizeof(fx_number[2]));
	fx_mv = fx_mv < 1000 ? 1000 : fx_mv > HYPEROS_FX_MV ? HYPEROS_FX_MV : fx_mv;
	mv = mv < 500 ? 500 : mv > ANDROID_EFFECT_VMAX_MV ? ANDROID_EFFECT_VMAX_MV : mv;
	amp = amp < 1 ? 1 : amp > 255 ? 255 : amp;
	ms = ms < 10 ? 10 : ms > HAPTIC_MAX_MS ? HAPTIC_MAX_MS : ms;
	count = count < 1 ? 1 : count > 10 ? 10 : count;
	constant_mv = mv;
	if (!configure(mv)) {
		goto out;
	}
	show_registers();
	if (level == 3) {
		for (unsigned i = 1; i <= count && !broken; i++) {
			if (!buzz(ms, (uint8_t)amp, i)) {
				break;
			}
			if (i < count) {
				wait_ms(600);
			}
		}
		for (unsigned i = 1; i <= count && click && !broken; i++) {
			wait_ms(600);
			if (!click_once((uint8_t)amp, i)) {
				break;
			}
		}
		for (int t = 0; t < 3 && fx && !broken; t++) {
			fx_look();
			if (fx_data[t] == NULL) {
				continue;
			}
			for (unsigned i = 1; i <= count && !broken; i++) {
				wait_ms(600);
				if (!fx_once(t, i)) {
					break;
				}
			}
		}
		show_registers();
	} else if (level == 4) {
		serve((uint8_t)amp);
	}
out:
	say("haptic: done\n");
	thread_terminate(current_thread());
}

void
shennong_haptic_init(void)
{
	thread_t thread;

	if (strcmp(redwood_phone(), "shennong") != 0 || !PE_parse_boot_argn("haptic", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 4 ? 4 : level;
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the play stopped, if one may be going on. */
void
shennong_haptic_quiesce(void)
{
	if (level < 2 || cfg.write == 0) {
		return;
	}
	leaving = true;
	if (playing) {
		stop_now();
		say("haptic: at the run's end: the play stopped\n");
	}
	if (level == 4) {
		say("haptic: at the run's end: %u plays (%u cut at %u ms), from %llu frames iOS wrote\n", plays, capped, HAPTIC_MAX_MS,
		    written_frames);
	}
}
