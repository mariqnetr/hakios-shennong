/*
 * The sound of the Xiaomi 14 Pro (shennong, SM8650), step 12: what the audio DSP's AudioReach is told so
 * that samples of ours reach the speakers' interface. Every address and number below belongs to this
 * phone, and nothing here happens unless boot-arg sound=N asks for it, with adsp=1 adsp_keep=1
 * (redwood_adsp.c), which starts the DSP and calls shennong_sound_dsp_up() once its audio framework
 * has said it is up (step 11).
 *
 * On SM8650 the DSP's audio is AudioReach (SPF): a graph of modules in containers, in sub-graphs, that
 * the APM (port 1) opens, prepares and starts on GPR; the clocks are asked of the PRM (port 2). The two
 * amplifiers (shennong_speaker.c) sit on the secondary TDM interface (the device tree's sec_tdm_pinctrl:
 * GPIO 121 bit clock, 123 frame sync, 124 data out, 122 data in; all function 1, 8 mA, no pull, the
 * clocks output high), which the DSP drives as LPAIF interface 1. The graph, as Linux's q6apm builds
 * one from a topology (sound/soc/qcom/qdsp6/audioreach.c, q6apm.c, q6prm.c):
 *
 *   sub-graph 0x4001 (the stream), container 0x5001:
 *       0x6001 write shared memory end point (0x07001000)  -- our periods, 2 x 16 bits at 48 kHz
 *       0x6002 PCM decoder (0x07001005)                   -- 2 x 16 bits out
 *       0x6003 PCM converter (0x07001003)                 -- 2 x 32 bits out
 *   sub-graph 0x4002 (the device), container 0x5002:
 *       0x6004 TDM sink (0x0700100E)                      -- LPAIF 0, interface 1 (secondary), 2 slots of 32 bits
 *
 * The TDM sink is in this DSP's own table of modules (its image has 0x0700100E/F among the hardware
 * end points), as on SM8750, whose firmware has no "Audio IF" module either (Linux: the TDM back end's
 * patches, PARAM_ID_TDM_INTF_CFG 0x0800101B). The bit clock is the PRM's clock 0x202 (SEC_TDM_IBIT) at
 * 48000 x 32 x 2 = 3.072 MHz, with the device tree's attribute 4 (qcom,tdm-clk-attribute); the LPASS
 * core and the audio core are voted for first (qcom,mi2s-tdm-is-hw-vote-needed). The samples lie in
 * memory of ours that the DSP reads through the application processors' SMMU, as stream 0x1001 (mask
 * 0x80), addresses from 0x10000000 with the stream's low four bits in bits 32 to 35 (dt: qcom,msm-audio-ion,
 * iommus = <&apps_smmu 0x1001 0x80>, qcom,iommu-dma-addr-pool, qcom,smmu-sid-mask 0xf), as redwood_sound.c
 * does for redwood's.
 *
 *   sound=1  the interface alone: the pins, the votes and the bit clock asked for; whether the bit clock
 *            and the frame sync toggle at the pins is looked at; held for sound_secs (default 3)
 *   sound=2  ... and the graph, with a test tone -- 1 kHz at -20 dBFS, both channels -- written into it
 *            for sound_secs (at most 3); the DSP's "done" of every write says whether it takes the
 *            samples at 48 kHz. The amplifiers stay off: nothing is heard.
 *
 * Every step is said before it is taken, and waits sound_pause ms (default 1000) so that the loader's
 * log has it if the phone stops there. Options for the graph, to try another way if the DSP refuses
 * one: sound_cap (the containers' capability, default 3, "end point"; 1 "processing"), sound_ilv (the
 * PCM converter's interleaving: 1 interleaved, 3 deinterleaved unpacked, the default), sound_clk_attr
 * (the bit clock's attribute, default 4), sound_perf (the sub-graphs' performance mode: 1 low power,
 * the default; 2 low latency).
 *
 * What is started is stopped, and what is written is put back, when the time is over or, if the DSP is
 * to be shut down earlier (the run's end), before that (shennong_sound_dsp_down()).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/redwood_audio.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

/* The SMMU's match registers and contexts are found free and taken by others as well (redwood_wifi.c). */
extern lck_mtx_t redwood_smmu_lock;

static LCK_GRP_DECLARE(sound_lock_group, "shennong-sound");
static LCK_MTX_DECLARE(sound_lock, &sound_lock_group);  /* the path being set up or taken down */

static uint32_t level, seconds = 3, pause_ms = 1000, capability = 0x0b001001, interleave = 3, clock_attribute = 4, perf_mode = 1;
static char last_write[96] = "nothing written";

/* ---- what is said: to the loader's log as well, as it happens ---------------------------------------- */

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	return length;
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* Time for the loader to note what has just been said, before something not yet seen to be harmless. */
static void
announce(const char *what)
{
	say("sound: %s\n", what);
	if (pause_ms) {
		sleep_ms(pause_ms);
	}
}

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	value = rd(b->base + offset);
	say("sound: %s %s (%#x) = %#x\n", b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	say("sound: about to write %s %s at %#x: %#x\n", b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "wrote %#x to %s %s at %#x", value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the TDM pins ------------------------------------------------------------------------------------- */

/*
 * (the thirtieth run panicked mapping the whole pin controller, a megabyte: the kernel's room for mappings of
 * devices was full -- they are permanent here. Only the four pages of gpio121..124 are mapped now.)
 */
#define TLMM_FIRST_PIN          121
static struct block tlmm = { "pin controller", 0x0f100000 + 0x1000 * TLMM_FIRST_PIN, 0x4000 };

#define GPIO_CFG(n)             (0x1000u * ((n) - TLMM_FIRST_PIN))       /* [1:0] pull, [5:2] function, [8:6] drive (mA / 2 - 1), [9] output */
#define GPIO_IO(n)              (0x1000u * ((n) - TLMM_FIRST_PIN) + 4)     /* bit 0 in, bit 1 out */
#define CFG_I2S1                (1u << 2)               /* pinctrl-sm8650.c: i2s1_sck, i2s1_data0, i2s1_ws, i2s1_data1 */
#define CFG_8MA                 (3u << 6)
#define CFG_OUTPUT              (1u << 9)

static const struct {
	unsigned pin;
	const char *name;
	uint32_t cfg, io;               /* the device tree's active state (tdm1_*_active) */
} tdm_pins[] = {
	{ 121, "gpio121 (bit clock)", CFG_I2S1 | CFG_8MA | CFG_OUTPUT, 2 },
	{ 123, "gpio123 (frame sync)", CFG_I2S1 | CFG_8MA | CFG_OUTPUT, 2 },
	{ 122, "gpio122 (data in)", CFG_I2S1 | CFG_8MA, 0 },
	{ 124, "gpio124 (data out)", CFG_I2S1 | CFG_8MA, 0 },
};
#define TDM_PINS                (sizeof(tdm_pins) / sizeof(tdm_pins[0]))
static uint32_t found_cfg[TDM_PINS], found_io[TDM_PINS];
static bool pins_taken;

static void
pins_on(void)
{
	for (unsigned i = 0; i < TDM_PINS; i++) {
		found_cfg[i] = get(&tlmm, tdm_pins[i].name, GPIO_CFG(tdm_pins[i].pin));
		found_io[i] = get(&tlmm, tdm_pins[i].name, GPIO_IO(tdm_pins[i].pin));
	}
	pins_taken = true;
	for (unsigned i = 0; i < TDM_PINS; i++) {
		put(&tlmm, tdm_pins[i].name, GPIO_IO(tdm_pins[i].pin), tdm_pins[i].io);
		/* (bits 11 and 12, eGPIO, kept as found: these four are not among the pins the island may own) */
		put(&tlmm, tdm_pins[i].name, GPIO_CFG(tdm_pins[i].pin), tdm_pins[i].cfg | (found_cfg[i] & (3u << 11)));
	}
}

static void
pins_off(void)
{
	if (!pins_taken) {
		return;
	}
	for (unsigned i = 0; i < TDM_PINS; i++) {
		put(&tlmm, tdm_pins[i].name, GPIO_CFG(tdm_pins[i].pin), found_cfg[i]);
		put(&tlmm, tdm_pins[i].name, GPIO_IO(tdm_pins[i].pin), found_io[i] & 2);
		get(&tlmm, tdm_pins[i].name, GPIO_CFG(tdm_pins[i].pin));
	}
	pins_taken = false;
}

/* Whether a pin toggles: read many times as fast as it goes. */
static uint32_t
pin_activity(unsigned pin, const char *name)
{
	uint32_t last = rd(tlmm.base + GPIO_IO(pin)) & 1, reads = 20000, changes = 0, ones = 0;
	uint64_t began = now_us();

	for (uint32_t i = 0; i < reads; i++) {
		uint32_t now = rd(tlmm.base + GPIO_IO(pin)) & 1;

		changes += now != last;
		ones += now;
		last = now;
	}
	say("sound: %s: %u changes and %u ones in %u reads (%llu us)\n", name, changes, ones, reads, now_us() - began);
	return changes;
}

static void
pins_look(const char *when)
{
	uint32_t clock = pin_activity(121, "the bit clock (gpio121)");
	uint32_t sync = pin_activity(123, "the frame sync (gpio123)");

	pin_activity(124, "the data out (gpio124)");
	say("sound: %s: %s\n", when, clock > 100 && sync > 4 ? "THE TDM CLOCK AND FRAME SYNC RUN at the pins" :
	    "THE PINS DO NOT SHOW A RUNNING TDM CLOCK");
}

/* ---- the SMMU: our periods behind the DSP's addresses ---------------------------------------------- */

static struct block apps_smmu = { "apps smmu", 0x15000000, 0x2000 };
static struct block apps_smmu_second = { "apps smmu", 0, 0x1000 };              /* the second page: a word per context */
static struct block apps_smmu_context = { "apps smmu, the audio context", 0, 0x1000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
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
#define PTE_ADDRESS             0x0000fffffffff000ull
#define AUDIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as redwood_sound.c's */

#define AUDIO_STREAM            0x1001
#define AUDIO_STREAM_MASK       0x80            /* dt: iommus = <&apps_smmu 0x1001 0x80> */
#define AUDIO_ADDRESSES         0x10000000ull   /* dt: qcom,iommu-dma-addr-pool of qcom,msm-audio-ion */
#define AUDIO_STREAM_BITS       (1ull << 32)    /* the stream's low bits (0x1001 & 0xf), where the DSP takes them from */
#define SECOND_STREAM           0x1005          /* FastRPC's for the audio domain (dt: qcom,msm_fastrpc_compute_cb12, pd-type 2) */
#define SECOND_STREAM_MASK      0x80
#define SECOND_STREAM_BITS      (5ull << 32)    /* (0x1005 & 0xf, where the DSP takes the stream from, as for the audio's) */

#define PERIODS                 8
#define PERIOD_FRAMES           960             /* 20 ms */
#define PERIOD_BYTES            (PERIOD_FRAMES * 4)
#define COMMAND_PAGE            PERIODS                 /* behind the periods: a command too large to go inline */
#define FASTRPC_PAGE            (PERIODS + 1)           /* behind it: the arguments of a FastRPC invocation */
#define BUFFER_BYTES            ((PERIODS + 2) * 4096)  /* a page each */

static struct {
	bool found, on, passed;         /* passed: the boot chain lets the stream through untranslated */
	uint32_t page, pages, match, context, old_smr, old_s2cr, old_sctlr;
	uint32_t match2, old_smr2, old_s2cr2;   /* a second match register: FastRPC's stream (SECOND_STREAM) */
	bool second_on, second_covered;
	vm_offset_t tables;             /* three of 4K: the root, one of the second level, one of the third */
} space;
static vm_offset_t buffer;              /* BUFFER_BYTES of ours, the periods */
static uint64_t buffer_address;         /* as the DSP is to know it */

static bool
space_find(void)
{
	bool used[256] = { false };
	uint32_t scr0 = get(&apps_smmu, "scr0", SMMU_SCR0), idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
	uint32_t idr1 = get(&apps_smmu, "idr1", SMMU_IDR1), matches = idr0 & 0xff, contexts = idr1 & 0xff;

	if (scr0 & 1u << 3) {
		say("sound: this SMMU numbers its streams with 16 bits, which nothing here is written for\n");
		return false;
	}
	space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	space.pages = 2u << (idr1 >> 28 & 7);
	if (2 * space.pages * space.page != 0x100000) {
		say("sound: the SMMU says it has 2 x %u pages of %#x bytes, not a megabyte: its contexts are not looked for\n",
		    space.pages, space.page);
		return false;
	}
	space.match = ~0u;
	space.match2 = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if (((stream ^ SECOND_STREAM) & ~mask & ~SECOND_STREAM_MASK) == 0) {
				say("sound: match register %u covers stream %#x already: stream %#x mask %#x, s2cr %#x\n", n, SECOND_STREAM,
				    stream, mask, s2cr);
				space.second_covered = true;
			}
			if (((stream ^ AUDIO_STREAM) & ~mask & ~AUDIO_STREAM_MASK) == 0) {
				say("sound: match register %u covers the audio stream already: stream %#x mask %#x, s2cr %#x\n", n,
				    stream, mask, s2cr);
				if ((s2cr >> 16 & 3) == 1) {
					space.passed = true;
					return true;
				}
				say("sound: ... and does not let it through untranslated: it is somebody's, and is left alone\n");
				return false;
			}
		} else if (space.match == ~0u) {
			space.match = n;
			space.old_smr = smr;
			space.old_s2cr = s2cr;
		} else if (space.match2 == ~0u) {
			space.match2 = n;
			space.old_smr2 = smr;
			space.old_s2cr2 = s2cr;
		}
	}
	for (space.context = 0; space.context < contexts && used[space.context]; space.context++) {
	}
	if (space.match == ~0u || space.context >= contexts) {
		say("sound: the SMMU has no match register or no context left\n");
		return false;
	}
	apps_smmu_second.phys = apps_smmu.phys + space.page;
	apps_smmu_context.phys = apps_smmu.phys + (space.pages + space.context) * space.page;
	say("sound: the boot chain has set nothing up for stream %#x; free: match register %u (smr %#x, s2cr %#x) and context %u, at "
	    "%#x\n", AUDIO_STREAM, space.match, space.old_smr, space.old_s2cr, space.context, apps_smmu_context.phys);
	space.old_sctlr = get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
	if (space.old_sctlr & 1) {
		say("sound: CONTEXT %u IS SWITCHED ON although no stream is sent through it: it is somebody's, and is left alone\n",
		    space.context);
		return false;
	}
	space.found = true;
	return true;
}

static void
space_faults(void)
{
	uint32_t status, global;

	if (!space.on) {
		return;
	}
	status = rd(apps_smmu_context.base + SMMU_CB_FSR);
	global = rd(apps_smmu.base + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("sound: the SMMU has refused the DSP nothing (context %u: fsr %#x; sgfsr 0)\n", space.context, status);
		return;
	}
	wr(apps_smmu_context.base + SMMU_CB_FSR, status);       /* (cleared: the next fault is the next one's) */
	say("sound: THE SMMU HAS REFUSED A REQUEST: context %u: status %#x, address %#llx, syndrome %#x; the SMMU as a whole: status "
	    "%#x, stream %#x\n", space.context, status, (uint64_t)rd(apps_smmu_context.base + SMMU_CB_FAR + 4) << 32 |
	    rd(apps_smmu_context.base + SMMU_CB_FAR), rd(apps_smmu_context.base + SMMU_CB_FSYNR0), global,
	    rd(apps_smmu.base + SMMU_SGFSYNR1));
}

/* Our buffer behind the DSP's addresses from AUDIO_ADDRESSES on, through a context of ours. */
static bool
space_on(void)
{
	uint64_t *root, *second, *third, table_root;
	uint32_t smr = SMMU_SMR(space.match), s2cr = SMMU_S2CR(space.match);
	uint32_t sees = 1u << 31 | (uint32_t)AUDIO_STREAM_MASK << 16 | AUDIO_STREAM;

	if (kmem_alloc(kernel_map, &space.tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	root = (uint64_t *)space.tables;
	second = (uint64_t *)(space.tables + 4096);
	third = (uint64_t *)(space.tables + 8192);
	root[AUDIO_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	second[AUDIO_ADDRESSES >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	for (unsigned page = 0; page < BUFFER_BYTES / 4096; page++) {
		third[(AUDIO_ADDRESSES >> 12 & 0x1ff) + page] = kvtophys(buffer + 4096 * page) | AUDIO_PTE;
	}
	for (vm_offset_t line = space.tables; line < space.tables + 3 * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(space.tables) | (uint64_t)space.context << 48;
	say("sound: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets page tables of ours (root %#llx) for the %u KB of the periods at "
	    "%#llx, and match register %u sends stream %#x (mask %#x) through it\n", space.context, table_root & PTE_ADDRESS,
	    BUFFER_BYTES / 1024, AUDIO_ADDRESSES, space.match, AUDIO_STREAM, AUDIO_STREAM_MASK);
	if (pause_ms) {
		sleep_ms(pause_ms);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
	put(&apps_smmu_second, "cba2r", SMMU_CBA2R(space.context), SMMU_CBA2R_64_BIT);
	put(&apps_smmu_second, "cbar", SMMU_CBAR(space.context), SMMU_CBAR_STAGE_1);
	put(&apps_smmu_context, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
	put(&apps_smmu_context, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0, (uint32_t)table_root);
	put(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32));
	put(&apps_smmu_context, "ttbr1, low", SMMU_CB_TTBR1, 0);
	put(&apps_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4, 0);
	put(&apps_smmu_context, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&apps_smmu_context, "mair1", SMMU_CB_MAIR1, 0);
	put(&apps_smmu_context, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, space.context);
	put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(apps_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			say("sound: the SMMU does not finish forgetting old translations\n");
			return false;
		}
		sleep_ms(1);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	space.on = true;
	put(&apps_smmu, "s2cr", s2cr, space.context);
	put(&apps_smmu, "smr", smr, sees);
	if (get(&apps_smmu, "smr", smr) != sees || (get(&apps_smmu, "s2cr", s2cr) & 0x300ff) != space.context ||
	    !(get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR) & 1) ||
	    get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0) != (uint32_t)table_root) {
		say("sound: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x is not going through context %u\n", AUDIO_STREAM,
		    space.context);
		return false;
	}
	say("sound: THE AUDIO STREAM GOES THROUGH CONTEXT %u of the SMMU, with our page tables\n", space.context);
	return true;
}

static void
space_off(void)
{
	if (!space.on) {
		return;
	}
	space_faults();
	say("sound: the audio stream and context %u of the SMMU are put back as they were found\n", space.context);
	put(&apps_smmu, "smr", SMMU_SMR(space.match), space.old_smr);
	put(&apps_smmu, "s2cr", SMMU_S2CR(space.match), space.old_s2cr);
	if (space.second_on) {
		put(&apps_smmu, "smr", SMMU_SMR(space.match2), space.old_smr2);
		put(&apps_smmu, "s2cr", SMMU_S2CR(space.match2), space.old_s2cr2);
		space.second_on = false;
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, space.old_sctlr);
	get(&apps_smmu, "smr", SMMU_SMR(space.match));
	space.on = false;
}

/* FastRPC's stream (SECOND_STREAM) through our context too, in a second match register. With redwood_smmu_lock held. */
static bool
space_second(void)
{
	uint32_t sees = 1u << 31 | (uint32_t)SECOND_STREAM_MASK << 16 | SECOND_STREAM;

	if (space.second_on) {
		return true;
	}
	/*
	 * (the twenty-fourth run reset the phone here: the second match register was picked when the sound started,
	 * and snsrpc.c had taken that same one for the sensors' stream 0x1004 since -- written over, the sensors'
	 * stream went nowhere. Picked now, under the lock both take: the first one not valid at this moment.)
	 */
	if (space.on && !space.second_covered) {
		uint32_t matches = rd(apps_smmu.base + SMMU_IDR0) & 0xff;

		space.match2 = ~0u;
		for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
			uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n));

			if (n != space.match && !(smr >> 31)) {
				space.match2 = n;
				space.old_smr2 = smr;
				space.old_s2cr2 = rd(apps_smmu.base + SMMU_S2CR(n));
				break;
			}
		}
	}
	if (!space.on || space.second_covered || space.match2 == ~0u) {
		say("sound: stream %#x: %s\n", SECOND_STREAM, !space.on ? "our context is not on" : space.second_covered ?
		    "somebody's already, left alone" : "no second match register free");
		return false;
	}
	say("sound: match register %u sends stream %#x (mask %#x) through context %u too\n", space.match2, SECOND_STREAM,
	    SECOND_STREAM_MASK, space.context);
	put(&apps_smmu, "s2cr", SMMU_S2CR(space.match2), space.context);
	put(&apps_smmu, "smr", SMMU_SMR(space.match2), sees);
	space.second_on = true;
	return get(&apps_smmu, "smr", SMMU_SMR(space.match2)) == sees;
}

/* The periods, and the DSP's view of them. With redwood_smmu_lock held. */
static bool
shared_get(void)
{
	if (buffer == 0 && kmem_alloc(kernel_map, &buffer, BUFFER_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		buffer = 0;
		say("sound: no memory for the DSP's buffers\n");
		return false;
	}
	if (!space.found && !space.passed && !space_find()) {
		return false;
	}
	if (!space.passed && !space.on && !space_on()) {
		return false;
	}
	buffer_address = space.passed ? (kvtophys(buffer) | AUDIO_STREAM_BITS) : (AUDIO_ADDRESSES | AUDIO_STREAM_BITS);
	if (space.passed) {
		for (unsigned page = 1; page < BUFFER_BYTES / 4096; page++) {
			if (kvtophys(buffer + 4096 * page) != kvtophys(buffer) + 4096 * page) {
				say("sound: the DSP's buffers are not one piece of memory, and the stream is not translated\n");
				return false;
			}
		}
	}
	return true;
}

/* ---- GPR: commands to the APM and the PRM, and their answers ---------------------------------------- */

#define APM_PORT                                1
#define PRM_PORT                                2
#define GPR_BASIC_RSP_RESULT                    0x02001005u
#define GPR_WAIT_MS                             1000

#define APM_CMD_GRAPH_OPEN                      0x01001000u
#define APM_CMD_GRAPH_PREPARE                   0x01001001u
#define APM_CMD_GRAPH_START                     0x01001002u
#define APM_CMD_GRAPH_STOP                      0x01001003u
#define APM_CMD_GRAPH_CLOSE                     0x01001004u
#define APM_CMD_SET_CFG                         0x01001006u
#define APM_CMD_CLOSE_ALL                       0x01001013u     /* (the vendor's spf_core.c, spf_core_apm_close_all()) */
#define APM_CMD_SHARED_MEM_MAP_REGIONS          0x0100100cu
#define APM_CMD_SHARED_MEM_UNMAP_REGIONS        0x0100100du
#define APM_CMD_RSP_SHARED_MEM_MAP_REGIONS      0x02001001u
#define APM_MEMORY_MAP_SHMEM8_4K_POOL           3

#define APM_PARAM_ID_CONTAINER_CONFIG           0x08001000u
#define APM_PARAM_ID_SUB_GRAPH_CONFIG           0x08001001u
#define APM_PARAM_ID_MODULE_LIST                0x08001002u
#define APM_PARAM_ID_MODULE_PROP                0x08001003u
#define APM_PARAM_ID_MODULE_CONN                0x08001004u
#define APM_PARAM_ID_SUB_GRAPH_LIST             0x08001005u
#define APM_SUB_GRAPH_PROP_ID_PERF_MODE         0x0800100eu
#define APM_SUB_GRAPH_PROP_ID_DIRECTION         0x0800100fu
#define APM_SUB_GRAPH_PROP_ID_SCENARIO_ID       0x08001010u
#define APM_CONTAINER_PROP_ID_CAPABILITY_LIST   0x08001011u
#define APM_CONTAINER_PROP_ID_GRAPH_POS         0x08001012u
#define APM_CONTAINER_PROP_ID_STACK_SIZE        0x08001013u
#define APM_CONTAINER_PROP_ID_PROC_DOMAIN       0x08001014u
#define APM_MODULE_PROP_ID_PORT_INFO            0x08001015u
#define SUB_GRAPH_DIRECTION_RX                  2
#define SUB_GRAPH_SCENARIO_PLAYBACK             1
#define CONTAINER_POS_STREAM                    1
#define CONTAINER_POS_GLOBAL_DEVICE             4
#define PROC_DOMAIN_ADSP                        2
#define CONTAINER_STACK                         0x1000

#define MODULE_ID_WR_SHARED_MEM_EP              0x07001000u
#define MODULE_ID_PCM_CNV                       0x07001003u
#define MODULE_ID_PCM_DEC                       0x07001005u
#define MODULE_ID_TDM_SINK                      0x0700100eu
#define PARAM_ID_PCM_OUTPUT_FORMAT_CFG          0x08001008u
#define PARAM_ID_MEDIA_FORMAT                   0x0800100cu
#define PARAM_ID_HW_EP_MF_CFG                   0x08001017u
#define PARAM_ID_HW_EP_FRAME_SIZE_FACTOR        0x08001018u
#define PARAM_ID_TDM_INTF_CFG                   0x0800101bu
#define DATA_FORMAT_FIXED_POINT                 1
#define MEDIA_FMT_ID_PCM                        0x09001000u
#define PCM_LSB_ALIGNED                         1
#define PCM_LITTLE_ENDIAN                       1
#define PCM_INTERLEAVED                         1
#define PCM_CHANNEL_L                           1
#define PCM_CHANNEL_R                           2
#define PORT_OUTPUT                             1       /* a module's first output port, as Linux numbers it */
#define PORT_INPUT                              2       /* ... and its first input */

#define DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2            0x0400100au
#define DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2   0x05001004u

#define PRM_CMD_REQUEST_HW_RSC                  0x0100100fu
#define PRM_CMD_RELEASE_HW_RSC                  0x01001010u
#define PRM_CMD_RSP_REQUEST_HW_RSC              0x02001002u
#define PRM_CMD_RSP_RELEASE_HW_RSC              0x02001003u
#define PARAM_ID_RSC_AUDIO_HW_CLK               0x0800102cu
#define PARAM_ID_RSC_HW_CORE                    0x08001032u
#define HW_CORE_LPASS                           1       /* q6prm.h: Q6PRM_HW_CORE_ID_LPASS ("lpass_core_hw_vote") */
#define HW_CORE_DCODEC                          2       /* ... Q6PRM_HW_CORE_ID_DCODEC ("lpass_audio_hw_vote") */
#define CLOCK_SEC_TDM_IBIT                      0x202

#define SG_STREAM               0x4001
#define SG_DEVICE               0x4002
#define CONTAINER_STREAM        0x5001
#define CONTAINER_DEVICE        0x5002
#define IID_WR_EP               0x6001
#define IID_PCM_DEC             0x6002
#define IID_PCM_CNV             0x6003
#define IID_TDM                 0x6004

#define RATE                    48000
#define CHANNELS                2
#define STREAM_BITS             16
#define DEVICE_BITS             32
#define SLOTS                   4       /* (Android: 4 slots a frame, 2 of them used -- mask 0x3 -- at 6.144 MHz) */
#define SLOTS_USED              2
#define SLOT_BITS               32
#define LPAIF                   0       /* the LPAIF block of the MI2S/TDM interfaces (snd_ar_tokens.h: LPAIF 0, RXTX 1, WSA 2, VA 3) */
#define TDM_SECONDARY           1

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "no error", "failed", "bad parameter", "unsupported", "bad version", "unexpected",
		                             "panic", "no resource", "bad handle", "already", "not ready", "pending", "busy",
		                             "aborted", "continue", "immediate", "not implemented", "needs more", "no memory",
		                             "does not exist" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}

/* A command's packet: the payload as it is built, behind (with a command header) the APM's four words. */
static uint8_t payload[1024] __attribute__((aligned(8)));
static uint32_t payload_length;
static bool payload_header;

static bool mapped;
static uint32_t map_handle;

static void
payload_begin(bool command_header)
{
	bzero(payload, sizeof(payload));
	payload_header = command_header;
	payload_length = command_header ? 16 : 0;       /* apm_cmd_header: payload address, its map, its size (inline: 0, 0, size) */
}

static void *
payload_add(uint32_t bytes)
{
	void *p = payload + payload_length;

	payload_length += bytes;
	return p;
}

/* A parameter's header for module instance iid, its size padded to eight bytes; the data after it. */
static void *
payload_param(uint32_t iid, uint32_t param, uint32_t size)
{
	uint32_t total = (16 + size + 7) & ~7u, *words = payload_add(total);

	words[0] = iid;
	words[1] = param;
	words[2] = total - 16;
	words[3] = 0;
	return words + 4;
}

static void
payload_end(bool command_header)
{
	if (command_header) {
		((uint32_t *)payload)[3] = payload_length - 16;
	}
}

/*
 * The command now in payload, to a port of the DSP's, and its answer: the basic result (the command's
 * opcode and a status) or the answer `wanted'. True when the DSP has taken it.
 */
static bool force_out_of_band;        /* (out_of_band_probe()) the command through the map, though it would go inline */

static bool
command(const char *what, uint32_t port, uint32_t opcode, uint32_t wanted, struct redwood_apr_answer *answer)
{
	struct redwood_apr_answer local;

	if (answer == NULL) {
		answer = &local;
	}
	say("sound: > %s (opcode %#x to port %#x, %u bytes)\n", what, opcode, port, payload_length);
	snprintf(last_write, sizeof(last_write), "sent %s", what);
	answer->opcode = 0;
	if ((force_out_of_band || (!redwood_adsp_gpr(port, 0, opcode, payload, payload_length, GPR_WAIT_MS, answer) &&
	    answer->opcode == REDWOOD_GPR_NOT_SENT)) && payload_header && mapped && payload_length - 16 <= 4096) {
		/*
		 * The DSP has no room for it inline: the payload goes into our command page, which it reads
		 * through the map, and only the APM's header is sent (its address, the map's handle, its size).
		 */
		uint8_t *page = (uint8_t *)(buffer + 4096 * COMMAND_PAGE);
		uint64_t address = buffer_address + 4096 * COMMAND_PAGE;
		uint32_t header[4] = { (uint32_t)address, (uint32_t)(address >> 32), map_handle, payload_length - 16 };

		memcpy(page, payload + 16, payload_length - 16);
		for (vm_offset_t line = (vm_offset_t)page; line < (vm_offset_t)page + payload_length - 16; line += 64) {
			__asm__ volatile ("dc cvac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		say("sound: > %s: no room for it inline, so out of band: its %u bytes at %#llx (handle %#x)\n", what,
		    payload_length - 16, address, map_handle);
		answer->opcode = 0;
		if (!redwood_adsp_gpr(port, 0, opcode, header, sizeof(header), GPR_WAIT_MS, answer)) {
			say("sound: %s: NO ANSWER in %u ms%s\n", what, GPR_WAIT_MS, answer->opcode == REDWOOD_GPR_NOT_SENT ?
			    " (NOT SENT)" : "");
			return false;
		}
	} else if (answer->opcode == REDWOOD_GPR_NOT_SENT) {
		say("sound: %s: NOT SENT: the DSP has given no room for %u bytes\n", what, payload_length);
		return false;
	} else if (answer->opcode == 0 || answer->token == 0) {
		say("sound: %s: NO ANSWER in %u ms\n", what, GPR_WAIT_MS);
		return false;
	}
	if (answer->opcode == GPR_BASIC_RSP_RESULT) {
		if (answer->words[1] != 0) {
			say("sound: %s: REFUSED: status %u (%s), for opcode %#x\n", what, answer->words[1], dsp_error(answer->words[1]),
			    answer->words[0]);
			return false;
		}
		say("sound: %s: done\n", what);
		return true;
	}
	if (wanted != 0 && answer->opcode == wanted) {
		say("sound: %s: answered (opcode %#x, %u bytes: %#x %#x %#x)\n", what, answer->opcode, answer->length, answer->words[0],
		    answer->words[1], answer->words[2]);
		return true;
	}
	say("sound: %s: AN ANSWER NOT LOOKED FOR: opcode %#x, %u bytes: %#x %#x\n", what, answer->opcode, answer->length,
	    answer->words[0], answer->words[1]);
	return false;
}

/* ---- the PRM: the cores voted for, the bit clock ------------------------------------------------------ */

static bool voted_lpass, voted_dcodec, clock_on;

static bool
hw_core(uint32_t core, bool on)
{
	char what[80];
	uint32_t *id;

	payload_begin(true);
	id = payload_param(PRM_PORT, PARAM_ID_RSC_HW_CORE, 4);
	*id = core;
	payload_end(true);
	snprintf(what, sizeof(what), "the PRM: a vote for hardware core %u, %s", core, on ? "asked for" : "given back");
	return command(what, PRM_PORT, on ? PRM_CMD_REQUEST_HW_RSC : PRM_CMD_RELEASE_HW_RSC, on ? PRM_CMD_RSP_REQUEST_HW_RSC :
	           PRM_CMD_RSP_RELEASE_HW_RSC, NULL);
}

static bool
bit_clock(bool on)
{
	char what[120];
	uint32_t *words;

	payload_begin(true);
	if (on) {
		words = payload_param(PRM_PORT, PARAM_ID_RSC_AUDIO_HW_CLK, 20);
		words[0] = 1;                                   /* one clock: */
		words[1] = CLOCK_SEC_TDM_IBIT;                  /* its id, */
		words[2] = RATE * SLOT_BITS * SLOTS;            /* its rate, */
		words[3] = clock_attribute;                     /* its attribute, */
		words[4] = 0;                                   /* its root: the default */
	} else {
		words = payload_param(PRM_PORT, PARAM_ID_RSC_AUDIO_HW_CLK, 8);
		words[0] = 1;
		words[1] = CLOCK_SEC_TDM_IBIT;
	}
	payload_end(true);
	snprintf(what, sizeof(what), "the PRM: the bit clock (%#x, SEC_TDM_IBIT) %s", CLOCK_SEC_TDM_IBIT, on ?
	    "at 3.072 MHz" : "given back");
	if (on) {
		say("sound: (its attribute %u, its root the default)\n", clock_attribute);
	}
	return command(what, PRM_PORT, on ? PRM_CMD_REQUEST_HW_RSC : PRM_CMD_RELEASE_HW_RSC, on ? PRM_CMD_RSP_REQUEST_HW_RSC :
	           PRM_CMD_RSP_RELEASE_HW_RSC, NULL);
}

/*
 * (the user hears gaps of a tenth of a second, at random, about every ten seconds, which are in nothing the kernel
 * feeds nor in how the DSP takes it. Android's audio HAL asks the PRM, beside the cores, for parameter 0x08001a6e = 1
 * -- traced on the phone, the same request as a core's -- most likely the LPASS kept out of its low-power modes while
 * it plays; sound_lpm=0: not asked.)
 */
static uint32_t ask_lpm = 1;
static bool voted_lpm;

static bool
lpm_vote(bool on)
{
	char what[80];
	uint32_t *value;

	payload_begin(true);
	value = payload_param(PRM_PORT, 0x08001a6eu, 4);
	*value = 1;
	payload_end(true);
	snprintf(what, sizeof(what), "the PRM: 0x08001a6e = 1 (as Android's HAL), %s", on ? "asked for" : "given back");
	return command(what, PRM_PORT, on ? PRM_CMD_REQUEST_HW_RSC : PRM_CMD_RELEASE_HW_RSC, on ? PRM_CMD_RSP_REQUEST_HW_RSC :
	           PRM_CMD_RSP_RELEASE_HW_RSC, NULL);
}

static bool
interface_on(void)
{
	announce("the LPASS core and the audio core voted for, then the bit clock asked of the PRM");
	PE_parse_boot_argn("sound_lpm", &ask_lpm, sizeof(ask_lpm));
	if (ask_lpm && lpm_vote(true)) {
		voted_lpm = true;
	}
	if (!hw_core(HW_CORE_LPASS, true)) {
		return false;
	}
	voted_lpass = true;
	if (!hw_core(HW_CORE_DCODEC, true)) {
		return false;
	}
	voted_dcodec = true;
	if (!bit_clock(true)) {
		return false;
	}
	clock_on = true;
	return true;
}

static void
interface_off(void)
{
	if (clock_on) {
		bit_clock(false);
		clock_on = false;
	}
	if (voted_dcodec) {
		hw_core(HW_CORE_DCODEC, false);
		voted_dcodec = false;
	}
	if (voted_lpass) {
		hw_core(HW_CORE_LPASS, false);
		voted_lpass = false;
	}
	if (voted_lpm) {
		lpm_vote(false);
		voted_lpm = false;
	}
}

/* ---- the APM: the graph ------------------------------------------------------------------------------ */

static const struct {
	uint32_t sub_graph, container, module, iid, inputs, outputs, from;     /* from: the instance whose output feeds it */
} modules[] = {
	{ SG_STREAM, CONTAINER_STREAM, MODULE_ID_WR_SHARED_MEM_EP, IID_WR_EP, 0, 1, 0 },
	{ SG_STREAM, CONTAINER_STREAM, MODULE_ID_PCM_DEC, IID_PCM_DEC, 1, 1, IID_WR_EP },
	{ SG_STREAM, CONTAINER_STREAM, MODULE_ID_PCM_CNV, IID_PCM_CNV, 1, 1, IID_PCM_DEC },
	{ SG_DEVICE, CONTAINER_DEVICE, MODULE_ID_TDM_SINK, IID_TDM, 1, 0, IID_PCM_CNV },
};
#define MODULES                 (sizeof(modules) / sizeof(modules[0]))
static const struct {
	uint32_t id, sub_graph, position;
} containers[2] = {
	{ CONTAINER_STREAM, SG_STREAM, CONTAINER_POS_STREAM },
	{ CONTAINER_DEVICE, SG_DEVICE, CONTAINER_POS_GLOBAL_DEVICE },
};

static bool graph_open, graph_started;

/* What graph_open_command() puts in the sub-graphs and containers: as Linux's topologies have them, or a variant. */
struct graph_variant {
	const char *what;
	uint32_t stack, domain, scenario, position, perf;
	bool domain_prop, sg_props;
};
#define VARIANT_LINUX           { "as Linux's", CONTAINER_STACK, PROC_DOMAIN_ADSP, SUB_GRAPH_SCENARIO_PLAYBACK, 0, 1, true, true }
static struct graph_variant v = VARIANT_LINUX;
static unsigned open_stages = 5;        /* (graph_pieces()) how many of the open's five parameters are sent */
static uint32_t module_override;        /* (graph_pieces()) another module than the table's, for every instance */

/*
 * The graph's open, of the modules in mask (bit m: modules[m]) and of the containers and sub-graphs they are in; the
 * connections between the modules in it. The fifth run had the whole graph refused ("failed"); the pieces of it are
 * then tried alone (graph_pieces()), to see which the DSP does not take.
 */
#define ALL_MODULES             ((1u << MODULES) - 1)

static bool
container_in(unsigned i, uint32_t mask)
{
	for (unsigned m = 0; m < MODULES; m++) {
		if ((mask & 1u << m) && modules[m].container == containers[i].id) {
			return true;
		}
	}
	return false;
}

static bool
module_in(uint32_t iid, uint32_t mask)
{
	for (unsigned m = 0; m < MODULES; m++) {
		if ((mask & 1u << m) && modules[m].iid == iid) {
			return true;
		}
	}
	return false;
}

static bool
graph_open_command(const char *what, uint32_t mask)
{
	uint32_t *w, groups = 0, count = 0, connections = 0;

	for (unsigned i = 0; i < 2; i++) {
		groups += container_in(i, mask);
	}
	for (unsigned m = 0; m < MODULES; m++) {
		if (mask & 1u << m) {
			count++;
			connections += modules[m].from != 0 && module_in(modules[m].from, mask);
		}
	}
	payload_begin(true);
	/* the sub-graphs: each with its performance mode, direction and scenario (a sub-graph to a container) */
	w = payload_param(APM_PORT, APM_PARAM_ID_SUB_GRAPH_CONFIG, 4 + groups * (v.sg_props ? 44 : 8));
	*w++ = groups;
	for (unsigned i = 0; i < 2; i++) {
		if (!container_in(i, mask)) {
			continue;
		}
		*w++ = containers[i].sub_graph;
		*w++ = v.sg_props ? 3 : 0;
		if (v.sg_props) {
			*w++ = APM_SUB_GRAPH_PROP_ID_PERF_MODE; *w++ = 4; *w++ = perf_mode;
			*w++ = APM_SUB_GRAPH_PROP_ID_DIRECTION; *w++ = 4; *w++ = SUB_GRAPH_DIRECTION_RX;
			*w++ = APM_SUB_GRAPH_PROP_ID_SCENARIO_ID; *w++ = 4; *w++ = v.scenario;
		}
	}
	if (open_stages < 2) {
		goto sent;
	}
	/* the containers: each with its capability, position, stack and domain */
	/*
	 * (the thirty-first run, Android's own speaker graph read out of the audio HAL's shared memory: its containers'
	 * capability is no "end point" (3) but 0x0b001001 / 0x0b001000, their stack 0x1000, and three more properties
	 * follow -- 0x08001174 = 1, 0x080010cb = 0xffffffff, 0x0800124d = 1 -- seven in all. sound_cap=N: another.)
	 */
	w = payload_param(APM_PORT, APM_PARAM_ID_CONTAINER_CONFIG, 4 + groups * ((v.domain_prop ? 60 : 48) + 36));
	*w++ = groups;
	for (unsigned i = 0; i < 2; i++) {
		if (!container_in(i, mask)) {
			continue;
		}
		*w++ = containers[i].id;
		*w++ = (v.domain_prop ? 4 : 3) + 3;
		*w++ = APM_CONTAINER_PROP_ID_CAPABILITY_LIST; *w++ = 8; *w++ = 1; *w++ = capability;
		*w++ = APM_CONTAINER_PROP_ID_GRAPH_POS; *w++ = 4; *w++ = v.position ? v.position : containers[i].position;
		*w++ = APM_CONTAINER_PROP_ID_STACK_SIZE; *w++ = 4; *w++ = v.stack;
		if (v.domain_prop) {
			*w++ = APM_CONTAINER_PROP_ID_PROC_DOMAIN; *w++ = 4; *w++ = v.domain;
		}
		*w++ = 0x08001174; *w++ = 4; *w++ = 1;
		*w++ = 0x080010cb; *w++ = 4; *w++ = 0xffffffff;
		*w++ = 0x0800124d; *w++ = 4; *w++ = 1;
	}
	if (open_stages < 3) {
		goto sent;
	}
	/* the modules, a list for each container */
	w = payload_param(APM_PORT, APM_PARAM_ID_MODULE_LIST, 4 + groups * 12 + count * 8);
	*w++ = groups;
	for (unsigned i = 0; i < 2; i++) {
		uint32_t *listed;

		if (!container_in(i, mask)) {
			continue;
		}
		*w++ = containers[i].sub_graph;
		*w++ = containers[i].id;
		listed = w++;
		*listed = 0;
		for (unsigned m = 0; m < MODULES; m++) {
			if ((mask & 1u << m) && modules[m].container == containers[i].id) {
				*w++ = module_override ? module_override : modules[m].module;
				*w++ = modules[m].iid;
				(*listed)++;
			}
		}
	}
	if (open_stages < 4) {
		goto sent;
	}
	/* their ports */
	w = payload_param(APM_PORT, APM_PARAM_ID_MODULE_PROP, 4 + count * 24);
	*w++ = count;
	for (unsigned m = 0; m < MODULES; m++) {
		if (mask & 1u << m) {
			*w++ = modules[m].iid;
			*w++ = 1;
			*w++ = APM_MODULE_PROP_ID_PORT_INFO; *w++ = 8; *w++ = modules[m].inputs; *w++ = modules[m].outputs;
		}
	}
	/* and what feeds what */
	if (connections != 0 && open_stages >= 5) {
		w = payload_param(APM_PORT, APM_PARAM_ID_MODULE_CONN, 4 + connections * 16);
		*w++ = connections;
		for (unsigned m = 0; m < MODULES; m++) {
			if ((mask & 1u << m) && modules[m].from != 0 && module_in(modules[m].from, mask)) {
				*w++ = modules[m].from;
				*w++ = PORT_OUTPUT;
				*w++ = modules[m].iid;
				*w++ = PORT_INPUT;
			}
		}
	}
sent:
	payload_end(true);
	return command(what, APM_PORT, APM_CMD_GRAPH_OPEN, 0, NULL);
}

/* A command for the sub-graphs of the modules in mask: prepare, start, stop, close. */
static bool
graph_command_of(const char *what, uint32_t opcode, uint32_t mask)
{
	uint32_t *w, groups = 0;

	for (unsigned i = 0; i < 2; i++) {
		groups += container_in(i, mask);
	}
	payload_begin(true);
	w = payload_param(APM_PORT, APM_PARAM_ID_SUB_GRAPH_LIST, 4 + groups * 4);
	*w++ = groups;
	for (unsigned i = 0; i < 2; i++) {
		if (container_in(i, mask)) {
			*w++ = containers[i].sub_graph;
		}
	}
	payload_end(true);
	return command(what, APM_PORT, opcode, 0, NULL);
}

static bool
graph_command(const char *what, uint32_t opcode)
{
	return graph_command_of(what, opcode, ALL_MODULES);
}

/*
 * The whole graph refused: its pieces opened alone (and closed again), to say which the DSP takes: the stream's
 * sub-graph, the shared memory end point alone, the end point and the decoder, the device's (the TDM sink) alone.
 */
static uint32_t pieces;                 /* boot-arg sound_pieces=1: the pieces tried */

static void
graph_pieces(void)
{
	static const struct {
		uint32_t mask;
		const char *what;
	} pieces[] = {
		{ 0x7, "the stream's sub-graph alone (end point, decoder, converter)" },
		{ 0x1, "the shared memory end point alone" },
		{ 0x3, "the end point and the PCM decoder" },
		{ 0x8, "the device's sub-graph alone (the TDM sink)" },
	};
	static const struct {
		unsigned stages;
		uint32_t module, capability;
		const char *what;
	} probes[] = {
		{ 2, 0, 3, "the sub-graph and its container (no module)" },
		{ 2, 0, 1, "the sub-graph and its container, capability 1 (processing)" },
		{ 2, 0, 2, "the sub-graph and its container, capability 2" },
		{ 3, 0, 3, "... and the module list (no ports)" },
		{ 4, 0, 3, "... and the modules' ports (all but the connections)" },
		{ 5, 0, 3, "the whole of the end point's open" },
		{ 5, 0x07fff000, 3, "a module the DSP cannot have (0x07fff000)" },
		/* (last: the fourteenth run had the sub-graph alone opened, and then neither closed nor closed with all) */
		{ 1, 0, 3, "the sub-graph alone (no container, no module)" },
	};
	uint32_t was = capability;
	char what[160];
	unsigned taken = 0;

	/*
	 * The twelfth run: a container on processor domain 1 is "a bad parameter", on ours (2) the open goes on and
	 * "fails" -- so the open is read whole, and what fails is making something it asks for. The end point's open is
	 * cut short at each of its parameters in turn (what is made first fails first), and opened with a module the DSP
	 * cannot have (if that is said the same, "failed", the modules are not even looked at).
	 */
	say("sound: the whole graph refused: the end point's open, cut short and with a module that is not there\n");
	for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		/*
		 * (the thirteenth run: the sub-graph alone was opened, its close "failed", and every open after it was
		 * refused with 24 -- the sub-graph was still there. So everything is closed before each.)
		 */
		payload_begin(false);
		payload_end(false);
		command("the APM: everything closed (CLOSE_ALL)", APM_PORT, APM_CMD_CLOSE_ALL, 0, NULL);
		open_stages = probes[i].stages;
		module_override = probes[i].module;
		capability = probes[i].capability;
		snprintf(what, sizeof(what), "the APM: the end point's open, %s", probes[i].what);
		if (graph_open_command(what, pieces[1].mask)) {
			taken |= 1u << i;
			say("sound: THE DSP TAKES IT: %s\n", probes[i].what);
		}
	}
	payload_begin(false);
	payload_end(false);
	command("the APM: everything closed (CLOSE_ALL)", APM_PORT, APM_CMD_CLOSE_ALL, 0, NULL);
	open_stages = 5;
	module_override = 0;
	capability = was;
	say("sound: THE CUT-SHORT OPENS: %s\n", taken ? "SOME TAKEN, above" : "NONE TAKEN");
}

/*
 * The sixth run had every piece refused alike, the shared memory end point alone too: what is common to them is
 * their way in, out of band. So a command the DSP takes inline (the LPASS core's vote, to the PRM) is sent through
 * the map instead: taken, the DSP reads our command page; refused, it does not (or not where we wrote it). Then the
 * map is made again without the stream's bits in the address (as Linux has it) and both are tried again.
 */
static bool
out_of_band_probe(const char *how)
{
	bool taken;

	say("sound: out of band, checked (%s): the LPASS core's vote, which the PRM takes inline, through the map\n", how);
	force_out_of_band = true;
	taken = hw_core(HW_CORE_LPASS, true);
	force_out_of_band = false;
	if (taken) {
		hw_core(HW_CORE_LPASS, false);
	}
	space_faults();
	say("sound: OUT OF BAND (%s): %s\n", how, taken ? "THE DSP READS OUR COMMAND PAGE" : "THE DSP DOES NOT TAKE IT");
	return taken;
}

static void
unmap(void)
{
	if (mapped) {
		payload_begin(false);
		*(uint32_t *)payload_add(4) = map_handle;
		payload_end(false);
		command("the APM: our periods, unmapped", APM_PORT, APM_CMD_SHARED_MEM_UNMAP_REGIONS, 0, NULL);
		mapped = false;
	}
}

static bool memory_map_from(uint32_t port);
static uint32_t map_way;

/*
 * The eighth run: our command page taken out of our tables, the vote out of band was refused all the same and the
 * SMMU saw no fault -- nor with stream 0x1061 sent through our context too. The DSP does not come through the
 * application processors' SMMU for what it maps. So the command page is mapped by its physical address instead
 * (one page of 4K: what kvtophys() says it is), as a DSP that reads memory as it is would want it.
 */
static void
graph_probes(void)
{
	struct redwood_apr_answer answer;
	uint64_t physical = kvtophys(buffer + 4096 * COMMAND_PAGE), address = buffer_address;
	uint32_t *w;

	out_of_band_probe("the map through the SMMU, as before");
	unmap();
	payload_begin(false);
	w = payload_add(8 + 12);
	w[0] = APM_MEMORY_MAP_SHMEM8_4K_POOL | 1u << 16;
	w[1] = 0;
	w[2] = (uint32_t)physical;
	w[3] = (uint32_t)(physical >> 32);
	w[4] = 4096;
	payload_end(false);
	if (!command("the APM: our command page, mapped by its physical address", APM_PORT, APM_CMD_SHARED_MEM_MAP_REGIONS,
	    APM_CMD_RSP_SHARED_MEM_MAP_REGIONS, &answer) || answer.opcode != APM_CMD_RSP_SHARED_MEM_MAP_REGIONS) {
		say("sound: THE DSP DOES NOT MAP OUR COMMAND PAGE BY ITS PHYSICAL ADDRESS %#llx\n", physical);
		return;
	}
	mapped = true;
	map_handle = answer.words[0];
	buffer_address = physical - 4096 * COMMAND_PAGE;        /* (so that the command page is where command() puts it) */
	say("sound: our command page mapped by its physical address %#llx: handle %#x\n", physical, map_handle);
	if (out_of_band_probe("the command page by its physical address") &&
	    graph_open_command("the APM: the graph, opened (out of band, by the physical address)", ALL_MODULES)) {
		say("sound: THE GRAPH IS OPEN, out of band by the physical address; closed again\n");
		graph_command("the APM: the graph, closed", APM_CMD_GRAPH_CLOSE);
	}
	unmap();
	buffer_address = address;
}

/* What our periods hold: 2 x 16 bits at 48 kHz, left and right -- to the shared memory end point itself. */
static bool
format_stream(void)
{
	uint32_t *w;
	uint16_t *h;

	payload_begin(true);
	w = payload_param(IID_WR_EP, PARAM_ID_MEDIA_FORMAT, 12 + 16 + CHANNELS);
	w[0] = DATA_FORMAT_FIXED_POINT;
	w[1] = MEDIA_FMT_ID_PCM;
	w[2] = 16 + CHANNELS;
	w[3] = RATE;
	h = (uint16_t *)&w[4];
	h[0] = STREAM_BITS;             /* bit width */
	h[1] = PCM_LSB_ALIGNED;
	h[2] = STREAM_BITS;             /* bits per sample */
	h[3] = STREAM_BITS - 1;         /* Q factor */
	h[4] = PCM_LITTLE_ENDIAN;
	h[5] = CHANNELS;
	((uint8_t *)&h[6])[0] = PCM_CHANNEL_L;
	((uint8_t *)&h[6])[1] = PCM_CHANNEL_R;
	payload_end(true);
	return command("the shared memory end point: our periods' format, 2 x 16 bits at 48 kHz", IID_WR_EP, APM_CMD_SET_CFG, 0,
	           NULL);
}

/* What a PCM module puts out. */
static bool
format_pcm(const char *what, uint32_t iid, uint16_t bits, uint16_t interleaving)
{
	uint32_t *w;
	uint16_t *h;

	payload_begin(true);
	w = payload_param(iid, PARAM_ID_PCM_OUTPUT_FORMAT_CFG, 12 + 16 + CHANNELS);
	w[0] = DATA_FORMAT_FIXED_POINT;
	w[1] = MEDIA_FMT_ID_PCM;
	w[2] = (16 + CHANNELS + 3) & ~3u;
	h = (uint16_t *)&w[3];
	h[0] = bits;                    /* bit width */
	h[1] = PCM_LSB_ALIGNED;
	h[2] = bits;                    /* bits per sample */
	h[3] = bits - 1;                /* Q factor */
	h[4] = PCM_LITTLE_ENDIAN;
	h[5] = interleaving;
	h[6] = 0;
	h[7] = CHANNELS;
	((uint8_t *)&h[8])[0] = PCM_CHANNEL_L;
	((uint8_t *)&h[8])[1] = PCM_CHANNEL_R;
	payload_end(true);
	return command(what, APM_PORT, APM_CMD_SET_CFG, 0, NULL);
}

/* The TDM sink: the interface, the format it is fed, one frame a time. */
static bool
format_tdm(void)
{
	uint32_t *w;
	uint16_t *h;

	payload_begin(true);
	w = payload_param(IID_TDM, PARAM_ID_TDM_INTF_CFG, 28);
	w[0] = LPAIF;
	w[1] = TDM_SECONDARY;
	h = (uint16_t *)&w[2];
	h[0] = 1;                       /* frame sync: internal (we drive it) */
	h[1] = 1;                       /* data out enabled (as Android has it) */
	w[3] = (1u << SLOTS_USED) - 1;  /* the slots: 0 and 1 */
	h = (uint16_t *)&w[4];
	h[0] = SLOTS;
	h[1] = SLOT_BITS;
	h[2] = 1;                       /* frame sync: long (as Android has it) */
	h[3] = 0;                       /* ... not inverted */
	h[4] = 1;                       /* data a bit after it */
	h[5] = 0;
	w = payload_param(IID_TDM, PARAM_ID_HW_EP_MF_CFG, 12);
	w[0] = RATE;
	((uint16_t *)&w[1])[0] = DEVICE_BITS;
	((uint16_t *)&w[1])[1] = CHANNELS;
	w[2] = DATA_FORMAT_FIXED_POINT;
	w = payload_param(IID_TDM, PARAM_ID_HW_EP_FRAME_SIZE_FACTOR, 4);
	w[0] = 1;
	/*
	 * (the thirty-third run: opened and prepared, not started. Android's TDM sink is given two more parameters
	 * in its shared-memory SET_CFG, whatever they are: 0x0800106a = 2 and 0x0800113c = { 0, 0, 4, 0 }.)
	 */
	w = payload_param(IID_TDM, 0x0800106au, 4);
	w[0] = 2;
	w = payload_param(IID_TDM, 0x0800113cu, 16);
	w[0] = 0; w[1] = 0; w[2] = 4; w[3] = 0;
	payload_end(true);
	say("sound: the TDM sink: LPAIF %u, interface %u (secondary), %u slots of %u bits (mask %#x), frame sync internal, long, "
	    "not inverted, data out on, delayed a bit; 2 x %u bits at %u Hz\n", LPAIF, TDM_SECONDARY, SLOTS, SLOT_BITS, (1u << SLOTS_USED) - 1,
	    DEVICE_BITS, RATE);
	return command("the TDM sink: its interface, its format, its frame", APM_PORT, APM_CMD_SET_CFG, 0, NULL);
}

/*
 * The map. The first run had the DSP refuse it ("failed") for our addresses with the stream's bits
 * (0x1_1000_0000) and no flags: the ways below are tried in turn until one is taken (boot-arg
 * sound_map=N: only way N) -- with and without the stream's bits, with and without bit 1 of the
 * flags (uncached, as AudioReach's apm_api.h has it).
 */
static const struct {
	bool stream_bits;
	uint32_t flags;
} map_ways[] = { { true, 0 }, { false, 0 }, { true, 2 }, { false, 2 } };
#define MAP_WAYS                (sizeof(map_ways) / sizeof(map_ways[0]))
static uint32_t map_way = ~0u;

static bool
memory_map_from(uint32_t port)
{
	struct redwood_apr_answer answer;
	uint64_t base = buffer_address & ~AUDIO_STREAM_BITS;

	for (uint32_t way = 0; way < MAP_WAYS; way++) {
		uint64_t address = map_ways[way].stream_bits ? base | AUDIO_STREAM_BITS : base;
		char what[160];
		uint32_t *w;

		if (map_way != ~0u && way != map_way) {
			continue;
		}
		payload_begin(false);
		w = payload_add(8 + 12);       /* {pool, regions, flags} and one region {lsw, msw, size} */
		w[0] = APM_MEMORY_MAP_SHMEM8_4K_POOL | 1u << 16;        /* the pool; one region */
		w[1] = map_ways[way].flags;
		w[2] = (uint32_t)address;
		w[3] = (uint32_t)(address >> 32);
		w[4] = BUFFER_BYTES;
		payload_end(false);
		snprintf(what, sizeof(what), "the APM: our periods, mapped for the DSP (from port %#x, way %u: at %#llx, flags %#x)", port,
		    way, address, map_ways[way].flags);
		if (!command(what, APM_PORT, APM_CMD_SHARED_MEM_MAP_REGIONS, APM_CMD_RSP_SHARED_MEM_MAP_REGIONS, &answer)) {
			continue;
		}
		if (answer.opcode != APM_CMD_RSP_SHARED_MEM_MAP_REGIONS) {
			say("sound: the map was answered without a handle\n");
			continue;
		}
		buffer_address = address;
		map_handle = answer.words[0];
		mapped = true;
		say("sound: THE DSP HAS MAPPED OUR %u KB at %#llx (from port %#x, way %u): handle %#x\n", BUFFER_BYTES / 1024,
		    buffer_address, port, way, map_handle);
		return true;
	}
	return false;
}

/*
 * The third run had the DSP refuse every way (status 1, within a millisecond) from our port 3, spf_core's. Whose
 * port a map is from may matter to it: the vendor's sends maps and graphs from audio-pkt's (dt: reg = <0x17>),
 * Linux's q6apm from a graph's own, dynamic one (0x10000000 and on). The ports below are tried in turn (boot-arg
 * sound_port=N: only that one); the one the DSP takes is kept for the rest of the graph's commands. (The fourth
 * run had every port refused too: the map's payload was four bytes short, its region's size left out -- what the
 * DSP refused it for, whatever the port.)
 */
static const uint32_t map_ports[] = { 3, 0x17, 0x10000001 };
#define MAP_PORTS               (sizeof(map_ports) / sizeof(map_ports[0]))
static uint32_t map_port = 0;

static bool
memory_map(void)
{
	for (uint32_t i = 0; i < MAP_PORTS; i++) {
		uint32_t port = map_port != 0 ? map_port : map_ports[i];

		redwood_adsp_gpr_port(port);
		if (memory_map_from(port)) {
			return true;
		}
		if (map_port != 0) {
			break;
		}
	}
	redwood_adsp_gpr_port(0);
	say("sound: THE DSP HAS TAKEN NO WAY OF MAPPING OUR PERIODS, FROM NO PORT\n");
	return false;
}

static bool
graph_on(void)
{
	announce("the graph: our periods mapped, the graph opened, its modules' formats set, prepared, started (the APM)");
	if (!memory_map()) {
		return false;
	}
	say("sound: the graph: the stream (sub-graph %#x, container %#x: shared memory %#x, PCM decoder %#x, PCM converter %#x) "
	    "into the device (sub-graph %#x, container %#x: TDM sink %#x); containers' capability %u, performance mode %u\n",
	    SG_STREAM, CONTAINER_STREAM, IID_WR_EP, IID_PCM_DEC, IID_PCM_CNV, SG_DEVICE, CONTAINER_DEVICE, IID_TDM, capability,
	    perf_mode);
	if (!graph_open_command("the APM: the graph, opened", ALL_MODULES)) {
		if (pieces & 2) {
			graph_probes();         /* (boot-arg sound_pieces=2: the ways out of band, the eighth run's) */
		}
		graph_pieces();
		return false;
	}
	graph_open = true;
	if (!format_stream() ||
	    !format_pcm("the PCM decoder: 2 x 16 bits, interleaved", IID_PCM_DEC, STREAM_BITS, PCM_INTERLEAVED) ||
	    !format_pcm("the PCM converter: 2 x 32 bits", IID_PCM_CNV, DEVICE_BITS, (uint16_t)interleave) ||
	    !format_tdm()) {
		return false;
	}
	if (!graph_command("the APM: the graph, prepared", APM_CMD_GRAPH_PREPARE)) {
		return false;
	}
	if (!graph_command("the APM: the graph, started", APM_CMD_GRAPH_START)) {
		return false;
	}
	graph_started = true;
	return true;
}

static void
graph_off(void)
{
	if (graph_started) {
		graph_command("the APM: the graph, stopped", APM_CMD_GRAPH_STOP);
		graph_started = false;
	}
	if (graph_open) {
		graph_command("the APM: the graph, closed", APM_CMD_GRAPH_CLOSE);
		graph_open = false;
	}
	unmap();
}

/* ---- the test tone ----------------------------------------------------------------------------------- */

#define WRITE_TOKEN             0x80000000u     /* | the period: what a write's "done" carries back */

static volatile bool stopping;          /* the run is ending: whatever plays stops */
static volatile uint32_t returned;      /* the periods the DSP has said it is done with (a bit each) */
static volatile uint32_t written_done, write_errors, other_events;

/* The DSP's events: the "done" of every write. On the link's thread. */
static bool
events(uint32_t from_port, uint32_t opcode, uint32_t token, const uint32_t *words, uint32_t length)
{
	if (opcode == DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2 && (token & WRITE_TOKEN)) {
		if (length >= 16 && words[3] != 0) {
			OSIncrementAtomic((volatile SInt32 *)&write_errors);
		}
		OSIncrementAtomic((volatile SInt32 *)&written_done);
		OSBitOrAtomic(1u << (token & 31), &returned);
		return true;
	}
	if (opcode == GPR_BASIC_RSP_RESULT && (token & WRITE_TOKEN)) {
		/* (a write the DSP refused outright: it answers it as a command) */
		if (write_errors < 3) {
			say("sound: a write refused: status %u (%s), from port %#x\n", length >= 8 ? words[1] : 0,
			    dsp_error(length >= 8 ? words[1] : 0), from_port);
		}
		OSIncrementAtomic((volatile SInt32 *)&write_errors);
		OSIncrementAtomic((volatile SInt32 *)&written_done);
		OSBitOrAtomic(1u << (token & 31), &returned);
		return true;
	}
	if (other_events < 10) {
		say("sound: an event from port %#x: opcode %#x, token %#x, %u bytes: %#x %#x\n", from_port, opcode, token, length,
		    length >= 4 ? words[0] : 0, length >= 8 ? words[1] : 0);
	}
	OSIncrementAtomic((volatile SInt32 *)&other_events);
	return true;
}

/* 1 kHz at 48 kHz, -20 dBFS (a peak of 3277): one cycle */
static const int16_t tone[48] = {
	0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3028, 3165, 3249,
	3277, 3249, 3165, 3028, 2838, 2600, 2317, 1995, 1638, 1254, 848, 428,
	0, -428, -848, -1254, -1638, -1995, -2317, -2600, -2838, -3028, -3165, -3249,
	-3277, -3249, -3165, -3028, -2838, -2600, -2317, -1995, -1638, -1254, -848, -428,
};
static uint32_t tone_at;

/*
 * sound=4: what iOS plays (/dev/redwood-audio, redwood_audio.c), for as long as the run lasts, the amplifiers on
 * throughout. The writer (the audio server's real-time thread) puts frames into a ring and never waits; each period
 * the DSP gives back is filled from it, silence when there is nothing. Left goes to TDM slot 0 (the top amplifier
 * takes it), right to slot 1 (the bottom one). With no speaker protection loaded into the amplifiers (Android has
 * Cirrus's in their DSP), every sample is taken sound_atten 6 dB steps down (default 2: 12 dB); iOS muted: silence.
 */
#define RING_FRAMES             32768           /* 0.68 s; a power of two, the indices run on and wrap */
static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_head, ring_tail;
static bool ios_mode;
static uint32_t ios_atten = 2;
static uint64_t ios_frames, fed_frames;
static volatile uint32_t ios_peak;
static uint64_t ios_dropped, ios_short_periods, ios_writes, ios_report_us;  /* (said every 5 s while there is sound) */
static uint64_t ios_last_write_us, ios_longest_gap_us;
static volatile uint64_t ios_last_sound_us;    /* when the latest write that was not silence came */
#define SILENCE                 8               /* samples no larger than this are silence (as redwood_audio.c's) */
#define QUIET_BEFORE_OFF_US     3000000ull
static bool amps_started, amps_running;
static uint32_t sound_poll;  /* (declared again with the run's state below: one variable each) */

static uint32_t
ios_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = ring_head, room = RING_FRAMES - (head - ring_tail), taken = count < room ? count : room;
	uint64_t t = now_us();
	bool sound_here = false;

	ios_dropped += count - taken;
	ios_writes++;
	if (ios_last_write_us != 0 && t - ios_last_write_us > ios_longest_gap_us) {
		ios_longest_gap_us = t - ios_last_write_us;
	}
	ios_last_write_us = t;
	for (uint32_t i = 0; i < taken; i++) {
		int16_t left = frames[2 * i], right = frames[2 * i + 1];
		uint32_t l = left < 0 ? (uint32_t)-(int32_t)left : (uint32_t)left, r = right < 0 ? (uint32_t)-(int32_t)right :
		    (uint32_t)right;

		ring[2 * ((head + i) % RING_FRAMES)] = left;
		ring[2 * ((head + i) % RING_FRAMES) + 1] = right;
		if (l > ios_peak || r > ios_peak) {
			ios_peak = l > r ? l : r;
		}
		if (l > SILENCE || r > SILENCE) {
			sound_here = true;
		}
	}
	OSMemoryBarrier();
	ring_head = head + taken;
	ios_frames += taken;
	/*
	 * (0505: it was marked 1 first and the time written after -- and the feeder, reading 1 in between, took it for
	 * no sound at all and stopped the amplifiers, then started them again: the user's gaps of a tenth of a second,
	 * at random, with iOS's sound only; never with the kernel's own tone, which does not stop them.)
	 */
	if (sound_here) {
		ios_last_sound_us = t;
	}
	return taken;
}

static void
period_fill(unsigned period)
{
	int16_t *samples = (int16_t *)(buffer + 4096 * period);

	if (ios_mode) {
		uint32_t tail = ring_tail, available = ring_head - tail, n = available < PERIOD_FRAMES ? available : PERIOD_FRAMES;
		uint32_t below = 0, generation = 0;
		bool muted = false;
		static bool primed;

		/*
		 * (0499: the twenty-fourth run, with 0498, let the DSP down to two periods: it stopped giving any back, and
		 * the ring filled up. Instead, a cushion: when sound begins (or the ring has run dry), nothing is taken out
		 * of the ring until it holds three periods; from then on every period given back takes what there is.)
		 */
		if (!primed && available < 3 * PERIOD_FRAMES) {
			n = 0;
		} else {
			primed = available != 0;
		}
		(void)redwood_audio_volume(&below, &muted, &generation);
		OSMemoryBarrier();
		for (uint32_t i = 0; i < n; i++) {
			samples[2 * i] = muted ? 0 : (int16_t)(ring[2 * ((tail + i) % RING_FRAMES)] >> ios_atten);
			samples[2 * i + 1] = muted ? 0 : (int16_t)(ring[2 * ((tail + i) % RING_FRAMES) + 1] >> ios_atten);
		}
		bzero(samples + 2 * n, (PERIOD_FRAMES - n) * 4);
		OSMemoryBarrier();
		ring_tail = tail + n;
		fed_frames += n;
		if (n != 0 && n < PERIOD_FRAMES) {
			ios_short_periods++;    /* (sound ran out within a period: the writer was late) */
		}
		/* rec_sound=N rec_sound_fed=1: the recording has what goes to the DSP, every period, silence too */
		if (redwood_audio_recording_fed()) {
			redwood_audio_record_fed(samples, PERIOD_FRAMES);
		}
	} else {
		for (unsigned frame = 0; frame < PERIOD_FRAMES; frame++) {
			int16_t sample = tone[tone_at++ % 48];

			samples[2 * frame] = sample;
			samples[2 * frame + 1] = sample;
		}
	}
	for (vm_offset_t line = (vm_offset_t)samples; line < (vm_offset_t)samples + PERIOD_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));        /* the DSP reads memory, not our caches */
	}
	__asm__ volatile ("dsb sy");
}

static bool
period_write(unsigned period)
{
	uint64_t address = buffer_address + 4096 * period;
	uint32_t write[11] = { (uint32_t)address, (uint32_t)(address >> 32), map_handle, PERIOD_BYTES, 0, 0, 0, 0, 0, 0, 0 };

	return redwood_adsp_gpr(IID_WR_EP, WRITE_TOKEN | period, DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2, write, sizeof(write), 0,
	           NULL);
}

/* The tone, written period after period as the DSP gives them back, for `ms'; then until every period is back. */
static bool
play(uint32_t ms)
{
	uint64_t began, ended;
	uint32_t sent = 0, done_before = written_done, pending = 0;
	uint64_t last_back_us = 0, dsp_longest_wait_us = 0;
	unsigned dsp_fewest_left = PERIODS, dsp_backs = 0, dsp_bursts = 0, long_waits_said = 0;
	bool ok = true, tone_over = false, looked = false;

	returned = 0;
	tone_at = 0;
	for (unsigned p = 0; p < PERIODS; p++) {
		period_fill(p);
	}
	began = now_us();
	for (unsigned p = 0; p < PERIODS; p++) {
		if (!period_write(p)) {
			say("sound: the write of period %u could not be sent\n", p);
			return false;
		}
		sent++;
	}
	for (;;) {
		uint32_t back = returned;
		uint64_t now = now_us();

		/*
		 * (the user hears a pause of about a tenth of a second now and then: a period given back while the ring held
		 * less than a period was filled at once, the rest silence -- iOS writes 1024 frames every 21 ms, and the ring
		 * held one period or less. Now, with iOS's sound, a period given back waits for a whole period of it in the
		 * ring, unless the DSP is down to two periods it has not played.)
		 */
		if (back != 0) {
			OSBitAndAtomic(~back, &returned);
			pending |= back;
			/* (how the DSP gives periods back: the longest wait for one, and the fewest it was left with) */
			if (last_back_us != 0 && now - last_back_us > dsp_longest_wait_us) {
				dsp_longest_wait_us = now - last_back_us;
			}
			if (last_back_us != 0 && now - last_back_us > 35000 && long_waits_said < 40) {
				long_waits_said++;
				say("sound: the DSP gave nothing back for %llu ms, %llu.%01llu s into the sound\n",
				    (now - last_back_us) / 1000, (now - began) / 1000000, (now - began) / 100000 % 10);
			}
			last_back_us = now;
			{
				unsigned left = PERIODS - (unsigned)__builtin_popcount(pending);

				if (left < dsp_fewest_left) {
					dsp_fewest_left = left;
				}
				dsp_backs += (unsigned)__builtin_popcount(back);
				if (__builtin_popcount(back) > 1) {
					dsp_bursts++;
				}
			}
		}
		if (pending != 0) {
			unsigned out = PERIODS - (unsigned)__builtin_popcount(pending);
			bool enough = true;     /* (0499: as before 0498 -- the DSP let down to two periods stopped giving any back) */

			back = enough ? pending : 0;
			pending &= ~back;
			for (unsigned p = 0; p < PERIODS; p++) {
				if (!(back & 1u << p) || tone_over) {
					continue;
				}
				if (false && ios_mode && ring_head - ring_tail < PERIOD_FRAMES) {
					back &= ~(1u << p);
					pending |= 1u << p;     /* (one period at a time when it is short: the rest wait) */
					if (out > 2) {
						continue;
					}
				}
				period_fill(p);
				if (!period_write(p)) {
					say("sound: the write of period %u could not be sent\n", p);
					ok = false;
					tone_over = true;
					break;
				}
				sent++;
			}
		}
		/* sound=4: what came and went, every 5 s while there is sound */
		if (ios_mode && now - ios_report_us >= 5000000 && ios_last_sound_us > 1 && now - ios_last_sound_us < 5000000) {
			ios_report_us = now;
			say("sound: iOS: %llu frames in %llu writes (the longest gap between two %llu ms), %llu dropped (the ring "
			    "full), %llu periods short (the writer late), %u in the ring; the DSP: %u periods back, the longest "
			    "wait for one %llu ms, %u times several at once, left with %u of %u at the fewest\n", ios_frames,
			    ios_writes, ios_longest_gap_us / 1000, ios_dropped, ios_short_periods, ring_head - ring_tail, dsp_backs,
			    dsp_longest_wait_us / 1000, dsp_bursts, dsp_fewest_left, PERIODS);
			/*
			 * (0506: run 32 -- the amplifiers no longer stopped amid the music, and the user still heard gaps; the
			 * only waits of the DSP's came every 5 s, with these I2C reads. Only with sound_poll=1 now.)
			 */
			if (amps_running && sound_poll) {
				shennong_speaker_poll();
			}
			ios_longest_gap_us = 0;
			dsp_longest_wait_us = 0;
			dsp_fewest_left = PERIODS;
			dsp_backs = dsp_bursts = 0;
		}
		/* sound=4: the amplifiers on while there is sound, off after three seconds of silence */
		if (ios_mode && amps_started) {
			uint64_t last = ios_last_sound_us;
			bool sound = last > 1 && now - last < QUIET_BEFORE_OFF_US;

			if (sound && !amps_running) {
				shennong_speaker_start();
				amps_running = true;
			} else if (!sound && amps_running) {
				shennong_speaker_stop();
				amps_running = false;
			}
		}
		if (!looked && now - began >= 500000) {
			looked = true;
			pins_look("half a second into the tone");
		}
		if (!tone_over && (now - began >= ms * 1000ull || stopping)) {
			tone_over = true;
		}
		if (tone_over && written_done - done_before >= sent) {
			break;
		}
		if (now - began > ms * 1000ull + 2000000) {
			say("sound: THE DSP HAS NOT GIVEN EVERY PERIOD BACK 2 s after the end (%u of %u)\n", written_done - done_before,
			    sent);
			ok = false;
			break;
		}
		if (written_done == done_before && now - began > 1000000) {
			say("sound: THE DSP HAS GIVEN NO PERIOD BACK IN A SECOND\n");
			ok = false;
			break;
		}
		sleep_ms(2);
	}
	ended = now_us();
	{
		uint32_t done = written_done - done_before;
		uint64_t frames = (uint64_t)done * PERIOD_FRAMES, us = ended - began;

		say("sound: %u periods written, %u given back (%u with an error), %u other events, in %llu ms: the DSP took %llu frames a "
		    "second (the amplifiers off: nothing to hear)\n", sent, done, write_errors, other_events, us / 1000,
		    us ? frames * 1000000 / us : 0);
		if (write_errors == 0 && done >= sent && us && frames * 1000000 / us > 40000 && frames * 1000000 / us < 56000) {
			say("sound: THE DSP TAKES OUR SAMPLES AT 48 kHz through the graph to the TDM interface\n");
		}
	}
	return ok;
}

/* ---- the audio domain's heap (FastRPC) ---------------------------------------------------------------- */

/*
 * The fifteenth run: the DSP opens a sub-graph, but no container, not even one with no module in it ("failed",
 * whatever its capability) -- a container is a thread with its own stack, and the audio domain has no heap to make
 * it in. On Android the heap is ours to give: the device tree reserves 12 MB for it (adsp_heap_region), handed by the
 * hypervisor to LPASS (0x16) and the DSP's heap (0x25) (qcom,adsp-remoteheap-vmid), and given to the static process
 * "audiopd" with FastRPC's INIT_CREATE_STATIC when audioadsprpcd starts (qcom,fastrpc-adsp-audio-pdr). As Linux's
 * fastrpc_init_create_static_process() does it: the invocation's arguments (the process's group, the name's length,
 * one page list; the name; the heap's one region) lie in a page of ours that the DSP reads through FastRPC's stream
 * for the audio domain, 0x1005, sent through our context (boot-arg sound_pd=0: not done).
 */
#define HEAP_BYTES              (12u << 20)     /* dt: adsp_heap_region, size = <0x00 0xc00000> */
#define VM_HLOS                 3
#define VM_LPASS                0x16
#define VM_ADSP_HEAP            0x25
#define FASTRPC_INIT_HANDLE     1
#define FASTRPC_USER_PD         1
#define FASTRPC_INIT_CREATE_STATIC_SCALARS      (8u << 24 | 3u << 16)   /* method 8, three buffers in, none out */
#define FASTRPC_ALIGN           128

/*
 * The sixteenth run: the phone reset itself, everything with it, a few seconds into the sound's start -- the log
 * ends before the first of these lines, so it says nothing of where. Hence step by step, each said, and the log
 * given time to get out before the next (boot-arg sound_pd=N: only the first N; pd_wait=ms, 3 s by default):
 * 1 the heap taken, 2 FastRPC's stream through our context, 3 the heap handed to the DSP, 4 "audiopd" made.
 */
static uint32_t audio_pd = 6, pd_wait = 3000;

/*
 * The seventeenth run: steps 1 and 2 done (the heap at 0x964000000, the stream through our context), and the phone
 * reset at step 3, at TrustZone's hand-over -- the line said right before the call never got out. Two things to
 * tell apart: the heap lay above 4 GB, where the device tree's adsp_heap_region may not be (alloc-ranges
 * 0..0xffffffff: the DSP's heap is below 4 GB on Android), and whether any hand-over to the DSP's machines resets
 * the phone. Hence the heap below 4 GB (pd_heap_high=1: above it too, if there is none below), and step 3 split in
 * three: one page of ours shared with LPASS and the DSP's heap (we keep it as well) and taken back, one page handed
 * to them alone and taken back, then the heap (pd_assign=0: the heap not handed over at all, as Linux does with no
 * VMIDs). The steps now: 1 the heap, 2 the stream, 3 a page shared, 4 a page handed over, 5 the heap handed over,
 * 6 "audiopd" made.
 */
static uint32_t pd_heap_high, pd_assign = 1;

/*
 * The twentieth run: no 12, 8 or 4 MB below 4 GB -- the loader gives XNU one window of memory, all of it above
 * 4 GB, so the kernel has no memory there at all. Below 4 GB the firmware leaves 0xcb800000..0xd8000000 free (its
 * memory map, type 7), and Android's memory node has it as the HLOS's (0xa2580000..0xd8000000): the heap is taken
 * there, 12 MB at 0xd0000000 (pd_heap_pa=address: elsewhere; 0: not), zeroed through a mapping of its own; it is
 * given back to the HLOS before the kernel returns to the loader (shennong_sound_dsp_gone()). The pages tried in
 * steps 3 and 4 are the heap's last page, before the heap is handed over.
 */
static uint64_t pd_heap_pa = 0xd0000000ull;
static uint32_t pd_matrix;
static uint32_t sound_rpc = 1, pd_listen_ms = 3000;  /* (sound_rpc=0: audiopd made here, no files served; pd_listen=ms: before the graph) */              /* (pd_matrix=1: when sharing is refused, every page with every machine) */
#define CHUNK_BYTES             (256u << 10)
static bool heap_fixed;

/*
 * The eighteenth and nineteenth runs: no reset, but none of the steps' lines in the log -- the kernel's log keeps its
 * first 256 KB and its last 1 MB, and SpringBoard's lines in between pushed them out. So each line of the audio
 * domain's steps is kept here too, and all of them said again when the sound stops and when the DSP is shut down,
 * both near the log's end.
 */
static char pd_recap[2048];
static size_t pd_recap_at;

static int pd_say(const char *format, ...) __printflike(1, 2);

static int
pd_say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (pd_recap_at < sizeof(pd_recap) - 1) {
		pd_recap_at += strlcpy(pd_recap + pd_recap_at, line, sizeof(pd_recap) - pd_recap_at);
		if (pd_recap_at > sizeof(pd_recap) - 1) {
			pd_recap_at = sizeof(pd_recap) - 1;
		}
	}
	return say("%s", line);
}

static void
pd_recap_say(const char *when)
{
	char line[400];
	const char *c = pd_recap;

	if (pd_recap_at == 0) {
		say("sound: the audio domain's steps, again (%s): none were taken\n", when);
		return;
	}
	say("sound: the audio domain's steps, again (%s):\n", when);
	while (*c != 0) {
		size_t n = 0;

		while (c[n] != 0 && c[n] != '\n' && n < sizeof(line) - 2) {
			n++;
		}
		memcpy(line, c, n);
		line[n] = 0;
		say("sound:   | %s\n", line + (strncmp(line, "sound: ", 7) == 0 ? 7 : 0));
		c += n;
		if (*c == '\n') {
			c++;
		}
	}
}

static bool
pd_step(unsigned step, const char *what)
{
	if (audio_pd < step) {
		pd_say("sound: audiopd: step %u (%s) not taken (sound_pd=%u)\n", step, what, audio_pd);
		return false;
	}
	pd_say("sound: audiopd: step %u: %s, in %u ms\n", step, what, pd_wait);
	sleep_ms(pd_wait);
	return true;
}
static vm_offset_t heap;
static uint64_t heap_physical, heap_bytes;
static bool heap_given, pd_made;

static bool
heap_get(void)
{
	static const uint32_t sizes[] = { HEAP_BYTES, 8u << 20, 4u << 20 };

	if (heap != 0 || heap_fixed) {
		return true;
	}
	uint64_t began = now_us();

	/*
	 * (the twenty-first run: kmem_alloc_contig() below 4 GB, where the kernel has no memory, panicked this time --
	 * "failed with 3" -- instead of failing: with pd_heap_pa set, the kernel's memory is not asked at all)
	 */
	/* below 4 GB first (dt: alloc-ranges <0 0 0 0xffffffff>), 4 MB apart (dt: alignment), then any alignment there */
	for (unsigned high = 0; pd_heap_pa == 0 && high <= (pd_heap_high ? 1 : 0) && heap == 0; high++) {
		ppnum_t below = high ? 0 : (ppnum_t)(atop_64(0x100000000ull) - 1);

		for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && heap == 0; i++) {
			if (kmem_alloc_contig(kernel_map, &heap, sizes[i], 0, below, (ppnum_t)(atop_64(4u << 20) - 1),
			    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS &&
			    kmem_alloc_contig(kernel_map, &heap, sizes[i], 0, below, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
			    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
				heap = 0;
				continue;
			}
			heap_bytes = sizes[i];
		}
		if (heap == 0 && !high) {
			pd_say("sound: no 12, 8 or 4 MB of contiguous memory below 4 GB%s\n", pd_heap_high ? "; above it, then" :
			    " (pd_heap_high=1: above it too)");
		}
	}
	if (heap == 0 && pd_heap_pa != 0) {
		/*
		 * (the twenty-second run panicked mapping all 12 MB at once, the twenty-third unmapping 256 KB of it: the
		 * kernel's mappings of devices are permanent, ml_io_unmap() panics on them. So the heap is not mapped, not
		 * zeroed, here at all: the kernel has never had it, so no line of it is in our caches, and the DSP makes
		 * its own heap in it.)
		 */
		heap_fixed = true;
		heap_physical = pd_heap_pa;
		heap_bytes = HEAP_BYTES;
		pd_say("sound: the audio domain's heap: 12 MB at %#llx, the firmware's free memory below 4 GB (found in %llu us)\n",
		    heap_physical, now_us() - began);
		return true;
	}
	if (heap == 0) {
		pd_say("sound: NO CONTIGUOUS MEMORY for the audio domain's heap (12, 8 or 4 MB)\n");
		return false;
	}
	heap_physical = kvtophys(heap);
	/* (written back and forgotten here before it is given away: no line of ours may be written into it later) */
	for (vm_offset_t line = heap; line < heap + heap_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	pd_say("sound: the audio domain's heap: %llu MB at %#llx (%llu us to find and clean)\n", heap_bytes >> 20, heap_physical,
	    now_us() - began);
	return true;
}

/*
 * One page of ours handed over and taken back (steps 3 and 4): shared, LPASS and the DSP's heap with us; or theirs
 * alone. Each hand-over said before and after, the page never touched while it is not ours alone.
 */
static bool
pd_try_page(bool shared)
{
	static const uint32_t hlos[] = { VM_HLOS }, dsp[] = { VM_LPASS, VM_ADSP_HEAP };
	static const uint32_t all[] = { VM_HLOS, VM_LPASS, VM_ADSP_HEAP };
	uint64_t physical = heap_physical + heap_bytes - 4096;     /* the heap's last page (cleaned out with it) */
	int result;

	pd_say("sound: > TrustZone: the page %#llx (4 KB, the heap's last) from HLOS to %s\n", physical,
	    shared ? "HLOS, LPASS and the DSP's heap" : "LPASS and the DSP's heap");
	sleep_ms(500);
	result = shared ? redwood_adsp_assign(physical, 4096, hlos, 1, all, 3) :
	    redwood_adsp_assign(physical, 4096, hlos, 1, dsp, 2);
	pd_say("sound: < TrustZone: %d for the page's hand-over\n", result);
	if (result != 0) {
		return false;
	}
	sleep_ms(500);
	result = shared ? redwood_adsp_assign(physical, 4096, all, 3, hlos, 1) :
	    redwood_adsp_assign(physical, 4096, dsp, 2, hlos, 1);
	pd_say("sound: < TrustZone: %d for the page taken back\n", result);
	if (result != 0) {
		pd_say("sound: THE PAGE %#llx IS NOT OURS AGAIN: the heap is not used\n", physical);
		return false;
	}
	return true;
}

/*
 * The twenty-fifth run: no reset, but TrustZone answers -2 (QCOM_SCM_EINVAL_ARG) to sharing the heap's last page
 * with LPASS and the DSP's heap. Which of the three is refused -- our memory below 4 GB, one of the two machines, or
 * the call itself -- is told apart by sharing (we keep it too, so nothing of ours can fault) a page of each kind with
 * each set of machines, every answer kept for the recap; each one shared is taken back at once.
 */
static int
pd_share_try(const char *what, uint64_t physical, const uint32_t *to, unsigned to_count)
{
	static const uint32_t hlos[] = { VM_HLOS };
	uint32_t with[4] = { VM_HLOS };
	int result, back;

	for (unsigned i = 0; i < to_count && i < 3; i++) {
		with[i + 1] = to[i];
	}
	result = redwood_adsp_assign(physical, 4096, hlos, 1, with, to_count + 1);
	back = result == 0 ? redwood_adsp_assign(physical, 4096, with, to_count + 1, hlos, 1) : 0;
	pd_say("sound: share %s %#llx with HLOS +%#x%s%s: %d%s\n", what, physical, to[0], to_count > 1 ? " +0x" : "",
	    to_count > 1 ? (to[1] == VM_ADSP_HEAP ? "25" : "?") : "", result, result == 0 ? (back == 0 ? ", taken back" :
	    ", NOT TAKEN BACK") : "");
	return result == 0 && back != 0 ? -1000 : result;
}

static void
pd_share_matrix(void)
{
	static const uint32_t both[] = { VM_LPASS, VM_ADSP_HEAP }, lpass[] = { VM_LPASS }, dheap[] = { VM_ADSP_HEAP };
	static const uint32_t q6[] = { 0x6 };   /* VMID_ADSP_Q6: what Android's adsprpc shares its region with (restrict-access) */
	uint64_t low = heap_physical + heap_bytes - 4096, high;
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		pd_say("sound: no page of the kernel's to share\n");
		return;
	}
	high = kvtophys(page);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	pd_share_try("the kernel's page", high, both, 2);
	pd_share_try("the kernel's page", high, lpass, 1);
	pd_share_try("the kernel's page", high, dheap, 1);
	pd_share_try("the kernel's page", high, q6, 1);
	pd_share_try("the low page", low, lpass, 1);
	pd_share_try("the low page", low, dheap, 1);
	pd_share_try("the low page", low, q6, 1);
	/* the same through an SHM bridge (the lists' page registered with TrustZone first, as Linux does on SM8650) */
	redwood_adsp_assign_bridge(true);
	pd_share_try("(bridged) the kernel's page", high, both, 2);
	pd_say("sound: (the bridge: %d; bridges enabled: %d)\n", redwood_adsp_bridge_result, redwood_adsp_bridge_enable_result);
	pd_share_try("(bridged) the low page", low, both, 2);
	pd_say("sound: (the bridge: %d)\n", redwood_adsp_bridge_result);
	pd_share_try("(bridged) the kernel's page", high, q6, 1);
	redwood_adsp_assign_bridge(false);
	/* (the kernel's page is not freed: if any hand-over of it was not taken back, it must never be used again) */
}

static bool
audio_pd_up(void)
{
	static const uint32_t hlos[] = { VM_HLOS }, dsp[] = { VM_LPASS, VM_ADSP_HEAP };
	static const char name[] = "audiopd";
	uint8_t *p = (uint8_t *)(buffer + 4096 * FASTRPC_PAGE);
	uint64_t iova = AUDIO_ADDRESSES + 4096 * FASTRPC_PAGE, address = iova | SECOND_STREAM_BITS;
	uint64_t *rpra = (uint64_t *)p;                         /* per argument: where it is (as the DSP's offset), its size */
	uint32_t *list = (uint32_t *)(p + 3 * 16);              /* per argument: one region, the page list's index */
	uint64_t *pages = (uint64_t *)(p + 3 * 16 + 3 * 8);     /* per argument: its page, the size of what it covers */
	uint32_t offsets[3], lengths[3] = { 12, sizeof(name), 16 }, metadata = 3 * (16 + 8 + 16) + 16 * 8 + 64 * 4;
	int32_t retval = -1;
	bool ok;

	if (!audio_pd || pd_made) {
		return true;
	}
	announce("the audio domain's heap: 12 MB handed to the DSP, and the static process \"audiopd\" made with it (FastRPC)");
	if (!pd_step(1, "12 MB of contiguous memory taken (out of the SMMU's lock)") || !heap_get()) {
		return false;
	}
	if (!pd_step(2, "stream 0x1005 (FastRPC's, the audio domain's) sent through our SMMU context too")) {
		return false;
	}
	if (sound_rpc) {
		pd_say("sound: (stream 0x1005 is shennong_audiorpc.c's: sound_rpc=1)\n");
	} else {
		lck_mtx_lock(&redwood_smmu_lock);
		ok = space_second();
		lck_mtx_unlock(&redwood_smmu_lock);
		if (!ok) {
			return false;
		}
	}
	if (!heap_given) {
		if (!pd_step(3, "a page of ours shared with LPASS and the DSP's heap by TrustZone, and taken back")) {
			return false;
		}
		/*
		 * (the twenty-seventh run: sharing -- HLOS keeping the page -- is refused with -2 whatever the page, the
		 * machines, or an SHM bridge; but on the sixteenth the heap handed to them alone, HLOS giving it up, was
		 * not refused: it reset the phone, maybe through the kernel's own mapping of that heap. The low heap is
		 * mapped by nobody here, so handing it over alone is tried next all the same, a page first.)
		 */
		if (!pd_try_page(true) && pd_matrix) {
			pd_share_matrix();
		}
		if (!pd_step(4, "a page of ours handed to LPASS and the DSP's heap alone, and taken back") || !pd_try_page(false)) {
			return false;
		}
	}
	if (!pd_step(5, "the heap handed to the DSP by TrustZone (assign_mem: from HLOS 3 to 0x16 and 0x25)")) {
		return false;
	}
	if (!pd_assign) {
		pd_say("sound: the heap is not handed over (pd_assign=0): the DSP is given it as it is\n");
	} else if (!heap_given) {
		int result = redwood_adsp_assign(heap_physical, heap_bytes, hlos, 1, dsp, 2);

		if (result != 0) {
			pd_say("sound: TRUSTZONE DOES NOT HAND THE HEAP TO THE DSP: %d\n", result);
			return false;
		}
		heap_given = true;
		pd_say("sound: the heap is the DSP's now (LPASS 0x16 and its heap 0x25)\n");
	}
	if (!pd_step(6, "INIT_CREATE_STATIC \"audiopd\" sent over FastRPC")) {
		return false;
	}
	if (sound_rpc) {
		/*
		 * (the twenty-ninth run: "audiopd" made with its heap, and still no container. On Android audioadsprpcd
		 * makes it as the process that then serves its files -- adsp_listener and adsp_default_listener -- through
		 * which the audio domain loads what it needs from /vendor. shennong_audiorpc.c does both, as
		 * shennong_snsrpc.c does for the sensors.)
		 */
		retval = shennong_audiorpc_start(heap_physical, heap_bytes);
		if (retval == INT32_MIN) {
			pd_say("sound: THE DSP HAS NOT ANSWERED THE MAKING OF \"audiopd\" (shennong_audiorpc.c)\n");
			return false;
		}
		if (retval != 0) {
			pd_say("sound: THE DSP REFUSES TO MAKE \"audiopd\": %d (%#x)\n", retval, (uint32_t)retval);
			return false;
		}
		pd_made = true;
		pd_say("sound: \"audiopd\" IS MADE, WITH ITS HEAP, AND ITS FILES ARE SERVED (shennong_audiorpc.c)\n");
		sleep_ms(pd_listen_ms);
		return true;
	}
	bzero(p, 4096);
	offsets[0] = (metadata + FASTRPC_ALIGN - 1) & ~(FASTRPC_ALIGN - 1);
	offsets[1] = (offsets[0] + lengths[0] + FASTRPC_ALIGN - 1) & ~(FASTRPC_ALIGN - 1);
	offsets[2] = (offsets[1] + lengths[1] + FASTRPC_ALIGN - 1) & ~(FASTRPC_ALIGN - 1);
	for (unsigned i = 0; i < 3; i++) {
		rpra[2 * i] = iova + offsets[i];
		rpra[2 * i + 1] = lengths[i];
		list[2 * i] = 1;
		list[2 * i + 1] = i;
		pages[2 * i] = address & ~0xfffull;
		pages[2 * i + 1] = 4096;
	}
	((uint32_t *)(p + offsets[0]))[0] = 0x1000;             /* the process's group (Linux: the caller's) */
	((uint32_t *)(p + offsets[0]))[1] = sizeof(name);       /* the name's length, its 0 included */
	((uint32_t *)(p + offsets[0]))[2] = 1;                  /* one region of heap */
	memcpy(p + offsets[1], name, sizeof(name));
	((uint64_t *)(p + offsets[2]))[0] = heap_physical;
	((uint64_t *)(p + offsets[2]))[1] = heap_bytes;
	for (vm_offset_t line = (vm_offset_t)p; line < (vm_offset_t)p + 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	pd_say("sound: > FastRPC: INIT_CREATE_STATIC \"audiopd\", its heap %#llx (%llu MB); the arguments at %#llx\n", heap_physical,
	    heap_bytes >> 20, address);
	if (!redwood_adsp_fastrpc(FASTRPC_USER_PD, FASTRPC_INIT_HANDLE, FASTRPC_INIT_CREATE_STATIC_SCALARS, address, 4096, 10000,
	    &retval)) {
		lck_mtx_lock(&redwood_smmu_lock);
		space_faults();
		lck_mtx_unlock(&redwood_smmu_lock);
		pd_say("sound: THE DSP HAS NOT ANSWERED THE MAKING OF \"audiopd\"\n");
		return false;
	}
	lck_mtx_lock(&redwood_smmu_lock);
	space_faults();
	lck_mtx_unlock(&redwood_smmu_lock);
	if (retval != 0) {
		pd_say("sound: THE DSP REFUSES TO MAKE \"audiopd\": %d (%#x)\n", retval, (uint32_t)retval);
		return false;
	}
	pd_made = true;
	pd_say("sound: \"audiopd\" IS MADE, WITH ITS HEAP\n");
	return true;
}

/* The DSP shut down: the heap taken back from it, for the next kernel to have (redwood_adsp_stop()). */
void
shennong_sound_dsp_gone(void)
{
	static const uint32_t hlos[] = { VM_HLOS }, dsp[] = { VM_LPASS, VM_ADSP_HEAP };

	pd_recap_say("the DSP shut down");

	pd_made = false;
	if (!heap_given) {
		return;
	}
	if (redwood_adsp_assign(heap_physical, heap_bytes, dsp, 2, hlos, 1) == 0) {
		heap_given = false;
		printf("sound: the audio domain's heap is ours again\n");
	} else {
		printf("sound: THE AUDIO DOMAIN'S HEAP IS NOT GIVEN BACK (%#llx, %llu MB): the next kernel must not use it\n",
		    heap_physical, heap_bytes >> 20);
	}
}

/* ---- the run ------------------------------------------------------------------------------------------ */

static bool running, taken_down, events_set, amps_started, amps_running;

static void
take_down(void)
{
	stopping = true;
	lck_mtx_lock(&sound_lock);
	if (running && !taken_down) {
		taken_down = true;
		if (amps_started) {
			shennong_speaker_off();
			amps_started = false;
		}
		graph_off();
		interface_off();
		pins_off();
		lck_mtx_lock(&redwood_smmu_lock);
		space_off();
		lck_mtx_unlock(&redwood_smmu_lock);
		if (events_set) {
			redwood_adsp_gpr_events(NULL);
			events_set = false;
		}
		redwood_adsp_gpr_port(0);
		say("sound: everything of the sound's is stopped and put back\n");
		pd_recap_say("the sound stopped");
	}
	lck_mtx_unlock(&sound_lock);
}

static void
sound_thread(__unused void *parameter, __unused wait_result_t result)
{
	bool ok = false;

	lck_mtx_lock(&sound_lock);
	if (taken_down) {
		lck_mtx_unlock(&sound_lock);
		thread_terminate(current_thread());
	}
	say("sound: shennong, level %u: %s, for %u s\n", level, level == 1 ? "the speakers' TDM interface alone" :
	    "the interface and a graph with a test tone, the amplifiers off", seconds);
	if (level >= 2) {
		lck_mtx_lock(&redwood_smmu_lock);
		ok = shared_get();
		lck_mtx_unlock(&redwood_smmu_lock);
		if (!ok) {
			goto out;
		}
		ok = false;
		redwood_adsp_gpr_events(events);
		events_set = true;
	}
	if (level >= 2 && !audio_pd_up()) {
		say("sound: (the graph is tried all the same)\n");
	}
	announce("the TDM pins (GPIO 121 to 124) to the interface, as the device tree's active state has them");
	pins_on();
	if (!interface_on()) {
		goto out;
	}
	sleep_ms(20);
	pins_look("the bit clock asked for");
	if (level == 1) {
		sleep_ms(seconds * 1000);
		ok = true;
		goto out;
	}
	if (!graph_on()) {
		goto out;
	}
	sleep_ms(20);
	pins_look("the graph started");
	if (level >= 3) {
		amps_started = true;
		shennong_speaker_on();  /* (with the TDM clock running: the amplifiers' PLL takes it) */
		amps_running = true;
	}
	if (level >= 4) {
		ios_mode = true;
		if (shennong_speaker_protected && !PE_parse_boot_argn("sound_atten", &ios_atten, sizeof(ios_atten))) {
			ios_atten = 0;  /* (the amplifiers protect their speakers themselves: iOS's own level, untouched) */
		}
		redwood_audio_set_sink("the speakers (the audio DSP, shennong_sound.c)", ios_sink);
		say("sound: WHAT iOS PLAYS GOES TO THE SPEAKERS now, until the run ends (%u dB down, left: top, right: bottom)\n",
		    ios_atten * 6);
		ok = play(24u * 3600 * 1000);
		redwood_audio_set_sink("nothing", NULL);
		say("sound: iOS wrote %llu frames (its loudest sample %u of 32768), %llu of them went to the DSP\n", ios_frames,
		    ios_peak, fed_frames);
	} else {
		say("sound: the tone is written now for %u s: 1 kHz at -20 dBFS, both channels (%s)\n", seconds, level >= 3 ?
		    "THE AMPLIFIERS ON: to be heard" : "the amplifiers off");
		ok = play(seconds * 1000);
	}
	lck_mtx_lock(&redwood_smmu_lock);
	space_faults();
	lck_mtx_unlock(&redwood_smmu_lock);
out:
	lck_mtx_unlock(&sound_lock);
	take_down();
	say("sound: %s; last: %s\n", ok ? "LEVEL DONE" : "STOPPED SHORT", last_write);
	thread_terminate(current_thread());
}

void
shennong_sound_dsp_up(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("sound", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 4) {
		level = 4;              /* (3: the amplifiers on too, shennong_speaker.c; 4: and what iOS plays, until the run ends) */
	}
	PE_parse_boot_argn("sound_atten", &ios_atten, sizeof(ios_atten));
	PE_parse_boot_argn("sound_poll", &sound_poll, sizeof(sound_poll));
	if (ios_atten > 15) {
		ios_atten = 15;
	}
	PE_parse_boot_argn("sound_secs", &seconds, sizeof(seconds));
	PE_parse_boot_argn("sound_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("sound_cap", &capability, sizeof(capability));
	PE_parse_boot_argn("sound_ilv", &interleave, sizeof(interleave));
	PE_parse_boot_argn("sound_clk_attr", &clock_attribute, sizeof(clock_attribute));
	PE_parse_boot_argn("sound_perf", &perf_mode, sizeof(perf_mode));
	PE_parse_boot_argn("sound_map", &map_way, sizeof(map_way));
	PE_parse_boot_argn("sound_port", &map_port, sizeof(map_port));
	PE_parse_boot_argn("sound_pieces", &pieces, sizeof(pieces));
	PE_parse_boot_argn("sound_pd", &audio_pd, sizeof(audio_pd));
	PE_parse_boot_argn("pd_wait", &pd_wait, sizeof(pd_wait));
	PE_parse_boot_argn("pd_heap_high", &pd_heap_high, sizeof(pd_heap_high));
	PE_parse_boot_argn("pd_assign", &pd_assign, sizeof(pd_assign));
	PE_parse_boot_argn("pd_heap_pa", &pd_heap_pa, sizeof(pd_heap_pa));
	PE_parse_boot_argn("pd_matrix", &pd_matrix, sizeof(pd_matrix));
	PE_parse_boot_argn("sound_rpc", &sound_rpc, sizeof(sound_rpc));
	PE_parse_boot_argn("pd_listen", &pd_listen_ms, sizeof(pd_listen_ms));
	if (level == 3 && seconds > 30) {
		seconds = 30;           /* (sound=3: a tone of up to 30 s, to listen for gaps in it) */
	} else if (level == 2 && seconds > 3) {
		seconds = 3;            /* the test tone: three seconds at most */
	}
	running = true;
	if (kernel_thread_start(sound_thread, NULL, &thread) != KERN_SUCCESS) {
		running = false;
		say("sound: no thread\n");
		return;
	}
	thread_deallocate(thread);
}

void
shennong_sound_dsp_down(void)
{
	if (running) {
		take_down();
	}
}
