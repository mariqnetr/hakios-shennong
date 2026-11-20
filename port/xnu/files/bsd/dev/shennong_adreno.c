/*
 * The GPU of the Xiaomi 14 Pro (shennong): an Adreno 750 ("Adreno750v2", chip id 0x43051401, the
 * vendor's gen7_9_1). The first steps of bringing it up, the way bsd/dev/redwood_adreno.c brought up
 * the POCO X5 Pro's Adreno 642L: one step at a time, reads before writes, every access said in the
 * log before it is made (the loader, watching from another core with bootcpu=N, keeps the end of
 * the log through a silent reset). Nothing here runs unless boot-arg sgpu=N asks for steps 1 to N.
 *
 *   1  reads   what feeds the GPU from outside: its clocks in the global clock controller, and the
 *              names and levels RPMh knows its supplies by
 *   2  reads   the fuse that says which speed bin this GPU is
 *   3  writes  at most one bit: the clock of the GPU clock controller's registers on, should it be
 *              off (Linux turns it on for good when it starts); then reads that controller as the
 *              boot loader left it: power switches, PLLs, clocks
 *   4  writes  the CX half of the GPU block on (gpu_cx_gdsc): everything but the 3D core -- the GMU,
 *              the bus interface, the GPU's SMMU
 *   5  writes  the clocks of that half: the GPU's taps of the global 600 MHz PLL, the GMU's and the
 *              hub's clocks at 200 MHz from it (the rate Linux and the vendor start the GMU at is 260 MHz from
 *              the GPU's PLL 1; 200 is what redwood's GMU started with) (no PLL of the GPU's own needs starting), the GMU's,
 *              the hub's, the register bus's and the SMMU's clocks on
 * What steps 4 and 5 turned on is turned off again in reverse when the steps are over (boot-arg
 * sgpu_keep=1: left on), so that every run starts from what the phone's own start leaves.
 *   6  reads   the first words from the GPU block itself, now that it has power and clocks: the GMU's
 *              registers, the bus interface (GBIF, in the CX half), the GMU's own channel to RPMh (RSCC)
 *   7  asks    TrustZone what Linux asks it on an A750 before the GMU starts: to program the GPU's
 *              access controlled registers (and so start the A750's always-on counter, which the
 *              GMU's firmware keeps time by) and set the GPU's fuse word
 *   8  reads   the SMMU in front of the GPU: its ID registers, and which stream goes where
 *   9  reads   that SMMU's first translation context
 *  10  writes  two words into the GMU's data memory and reads them back: the way its firmware goes
 *  11  writes  page tables for the memory the GMU's firmware (fw/gmu_gen70900.bin of the small image)
 *              asks for, with its log and message page, and the SMMU: context 1 with those tables,
 *              the GMU's requests (stream 5) sent through it
 *  12  writes  the GMU's firmware into its own memories and into that memory
 *  13  writes  the GMU's channel to RPMh: its RSC's handshake and sequencer, the GPU's PDC started
 *  14  writes  the GMU's configuration, and lets its processor out of reset: the firmware runs
 *  15  writes  the request to take up its message queues ("HFI")
 *  16  talks   to the GMU, as Linux does to an A7xx's: the table of the 3D core's speeds with the
 *              supply votes they need (the phone's levels, 231 to 720 MHz), the bus table, "start
 *              the core firmware", "start", the speed wanted (boot-arg sgpu_speed=N, default 231 MHz)
 *  17  asks    the GMU to power the 3D core ("GPU_SET"), and reads its first registers
 *  18  writes  the 3D core's translation context (SMMU context 0, its streams 0 and 1), with its
 *              ring and its microcode (fw/gen70900_sqe.fw) in it
 *  19  asks    TrustZone to load the zap shader (fw/gen70900_zap.mbn; and first to give up the one
 *              a kernel before this one may have left)
 *  20  writes  the 3D core's registers as Linux's hw_init does for an A750; starts its command
 *              processor and gives it its first commands (a7xx_cp_init)
 *  21  commands the 3D core out of secure mode, and to write a word into a page of ours
 *  22  commands the 3D core's 2D engine to fill a picture the panel's size with three bands of
 *              colour in memory of ours, and shows it on the panel (as redwood's step 22)
 *  23  draws a triangle with the 3D pipeline into that picture, with a vertex and a fragment shader
 *              of ours (the state turnip, Mesa's Vulkan driver, gives an A750 for its shader blits),
 *              and shows it
 * What steps 11 to 23 started is stopped again before CX goes off: the GPU block's bus halted and
 * let go, the 3D core given back, the GMU told to drop its votes ("prepare slumber") and held in
 * reset, the 3D core's switch off should it have stayed on, the zap shader given up, the GMU's RSC
 * asleep, the streams sent nowhere.
 * The zap shader is /odm/firmware/gen70900_zap.mbn on the phone (Xiaomi's, signed); the core's
 * microcode is linux-firmware's qcom/gen70900_sqe.fw.
 * The firmware goes into build/fw-gpu/ on the Mac, and build-ios.sh puts it into the small image. The
 * phone's own (/vendor/firmware) is not given to adb's shell by HyperOS; linux-firmware's qcom/
 * gmu_gen70900.bin is the same size (78588 bytes: 15 blocks, version 4.2.20), and the GMU's and
 * the core's microcode are not signed. The zap shader is, with Xiaomi's key: that one has to be
 * the phone's.
 *
 * Boot-args: sgpu=N steps 1 to N; sgpu_new=K steps K to N are new, and each of their accesses is said
 * and paused for (default: step N's alone); sgpu_pause=MS that pause; sgpu_keep=1 nothing turned off;
 * sgpu_speed=N the 3D core's speed. gpu=1: steps 1 to 21 without a pause, and the GPU kept on for the
 * system to draw with, through the service RedwoodGPU and Mesa's freedreno, as on redwood (see "the GPU
 * kept on" below).
 *
 * Addresses: the phone's device tree (qcom,kgsl-3d0@3d00000, qcom,gmu@3d69000, kgsl-smmu@3da0000,
 * qcom,pineapple-gpucc at 0x3d90000, qfprom@221c2000 with gpu_speed_bin@9b, aop_cmd_db_region at
 * 0x81c60000); register offsets: Linux drivers/clk/qcom/gcc-sm8650.c and gpucc-sm8650.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

/* Into the loader's log as it is written, as well as the kernel's: a run that resets leaves that much. */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static uint32_t step, newest_step, pause_ms = 1500;    /* boot-arg sgpu_pause=MS */
static uint32_t first_new_step;         /* boot-arg sgpu_new=K: steps K to N are all new (default: N alone) */

/* A step not yet seen to work: its accesses are said before they are made, with a pause. */
static bool
fresh(void)
{
	return step >= first_new_step && step <= newest_step;
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- register access (one plain load or store, as redwood_adreno.c explains) ----------------------- */

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static uint32_t __attribute__((noinline))
rd8(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldrb %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
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

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(void)
{
	if (fresh() && pause_ms) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("sgpu: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (fresh()) {
		printf("sgpu: %u: about to read %s %s at %#x\n", step, b->name, name, b->phys + offset);
		hold();
	}
	value = rd(b->base + offset);
	printf("sgpu: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

/* The next of a row of registers of a kind whose first was just read without harm: no note, no pause. */
static uint32_t
peek(struct block *b, uint32_t offset)
{
	need(b);
	return rd(b->base + offset);
}

static uint32_t
get8(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (fresh()) {
		printf("sgpu: %u: about to read %s %s at %#x (a byte)\n", step, b->name, name, b->phys + offset);
		hold();
	}
	value = rd8(b->base + offset);
	printf("sgpu: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("sgpu: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	hold();
	wr(b->base + offset, value);
}

/* ---- step 1: the clocks from outside, and RPMh's names ------------------------------------------------ */

/*
 * The global clock controller (dt: qcom,pineapple-gcc at 0x100000); the GPU's clocks there by the names
 * the GPU's and GMU's nodes give them. Offsets: gcc-sm8650.c.
 */
static struct block gcc = { "gcc", 0x00100000, 0x200000 };
#define GCC_PLL_VOTE            0x52000         /* bit 15: gcc_gpu_gpll0_clk_src, bit 16: gcc_gpu_gpll0_div_clk_src */
#define GCC_GPU_CFG_AHB_CBCR    0x71004         /* the registers of the GPU's clock controller */
#define GCC_GPU_MEMNOC_GFX_CBCR 0x71010         /* the GPU's way to memory */
#define GCC_GPU_SNOC_DVM_GFX_CBCR 0x71018
#define GCC_DDRSS_GPU_AXI_CBCR  0x71154         /* the GMU's "axi_clk" */
#define CBCR_ENABLE             (1u << 0)
#define CBCR_HW_CTL             (1u << 1)       /* the hardware gates it: off while idle, on when what it clocks is reached */
#define CBCR_OFF                (1u << 31)

/* the command database (dt: reserved-memory aop_cmd_db_region; Linux cmd-db.c) */
#define CMD_DB_PHYS             0x81c60000UL
#define CMD_DB_SIZE             0x20000UL

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

static const struct cmd_db_header *cmd_db;

/* Where RPMh has a resource (0: nowhere) and, for a rail, the levels it knows (the GMU is told levels by position). */
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned *level_count)
{
	char id[8] = { 0 };

	*level_count = 0;
	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);       /* memory, not registers */
	}
	if (cmd_db->magic[0] != 0xdb || cmd_db->magic[1] != 0x30 || cmd_db->magic[2] != 0x03 || cmd_db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512 ||
		    sizeof(*cmd_db) + slave->header_offset + slave->count * sizeof(*entry) > CMD_DB_SIZE) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data = cmd_db->data + slave->data_offset + entry[i].offset;
			unsigned count = entry[i].length / 2u;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if (sizeof(*cmd_db) + (unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE) {
				count = 0;
			}
			for (unsigned l = 0; l < count && l < 16; l++) {
				levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
				if (l > 0 && levels[l] == 0) {
					break;
				}
				*level_count = l + 1;
			}
			return entry[i].address;
		}
	}
	return 0;
}

static void
show_rpmh_resource(const char *name, const char *what)
{
	uint16_t level[16];
	unsigned count;
	uint32_t address = rpmh_resource(name, level, &count);
	char levels[160];
	int at = 0;

	if (address == 0) {
		printf("sgpu: %u: RPMh has no \"%s\" (%s)\n", step, name, what);
		return;
	}
	levels[0] = 0;
	for (unsigned l = 0; l < count && at < (int)sizeof(levels) - 8; l++) {
		at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level[l]);
	}
	printf("sgpu: %u: RPMh has \"%s\" (%s) at %#x; levels:%s\n", step, name, what, address, levels);
}

static bool
step_outside(void)
{
	announce("read the GPU's clocks in the global clock controller (0x100000)");
	get(&gcc, "pll votes", GCC_PLL_VOTE);
	get(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
	get(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR);
	get(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR);
	get(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR);

	announce("look up the GPU's supply rails in the boot loader's list of RPMh resources (memory at 0x81c60000)");
	show_rpmh_resource("gfx.lvl", "the 3D core's own rail");
	show_rpmh_resource("cx.lvl", "the CX half, shared with most of the SoC");
	show_rpmh_resource("mx.lvl", "memories");
	show_rpmh_resource("mxc.lvl", "memories of the CX side");
	show_rpmh_resource("gmxc.lvl", "the GPU's memories (gen7_9)");
	show_rpmh_resource("ebi.lvl", "the memory bus");
	return true;
}

/* ---- step 2: the fuse --------------------------------------------------------------------------------- */

/* dt: qfprom@221c2000, gpu_speed_bin@9b: one byte, three bits from bit 4 */
static struct block qfprom = { "qfprom", 0x221c2000, 0x2000 };
#define QFPROM_GPU_SPEED_BIN    0x9b

static uint32_t speed_bin;

static bool
step_fuses(void)
{
	announce("read the GPU's speed bin in the fuses (0x221c2000)");
	speed_bin = get8(&qfprom, "speed bin", QFPROM_GPU_SPEED_BIN) >> 4 & 7;
	/* dt: qcom,gpu-pwrlevels-N have qcom,speed-bin = <...>: the table of speeds for this bin */
	printf("sgpu: %u: speed bin %u\n", step, speed_bin);
	return true;
}

/* ---- step 3: the GPU's clock controller ----------------------------------------------------------------- */

/* dt: qcom,pineapple-gpucc at 0x3d90000, 0x9800 long; offsets: gpucc-sm8650.c */
static struct block gpucc = { "gpucc", 0x03d90000, 0x9800 };
#define GPUCC_PLL0_MODE         0x0000          /* the 3D core's PLL: bit 31 locked, bit 0 output on; +4: L (times 19.2 MHz) */
#define GPUCC_PLL1_MODE         0x1000          /* the GMU's and the hub's PLL */
#define GPUCC_HLOS1_VOTE_GPU_SMMU_CBCR 0x7000   /* our vote for the SMMU's clock */
#define GPUCC_CXO_AON_CBCR      0x9004
#define GPUCC_DEMET_CBCR        0x900c
#define GPUCC_GX_GDSCR          0x905c          /* the 3D core's power switch: the GMU's to operate */
#define GPUCC_GX_GFX3D_CBCR     0x90a8
#define GPUCC_GX_GMU_CBCR       0x90bc
#define GPUCC_CX_GDSCR          0x9108          /* the CX half's power switch */
#define GPUCC_AHB_CBCR          0x911c          /* the register bus inside the GPU block */
#define GPUCC_SLEEP_CBCR        0x9134
#define GPUCC_CX_GMU_CBCR       0x913c          /* the GMU */
#define GPUCC_CXO_CBCR          0x9144          /* the crystal, for the GMU's timers */
#define GPUCC_HUB_CX_INT_CBCR   0x9148
#define GPUCC_MEMNOC_GFX_CBCR   0x9150
#define GPUCC_GMU_RCGR          0x9318          /* gpu_cc_gmu_clk_src: +0 command, +4 source and divider */
#define GPUCC_HUB_AON_CBCR      0x93e8
#define GPUCC_HUB_RCGR          0x93ec          /* gpu_cc_hub_clk_src */
#define GPUCC_FF_RCGR           0x9474          /* gpu_cc_ff_clk_src */

static bool
step_gpucc(void)
{
	uint32_t ahb;

	announce("make sure the clock of the GPU clock controller's registers runs (gcc_gpu_cfg_ahb)");
	ahb = get(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
	if (!(ahb & CBCR_ENABLE)) {
		put(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR, ahb | CBCR_ENABLE);
		for (int i = 0; i < 100 && (rd(gcc.base + GCC_GPU_CFG_AHB_CBCR) & CBCR_OFF); i++) {
			sleep_ms(1);
		}
		ahb = get(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
	}
	/*
	 * Seen on the phone: 0x80000003 -- on, under hardware control, and off at that moment: the boot loader
	 * left the gating to the hardware, which runs the clock when the registers behind it are reached.
	 */
	if ((ahb & CBCR_OFF) && !(ahb & CBCR_HW_CTL)) {
		printf("sgpu: %u: it stays off (%#x): the GPU's clock controller would not answer\n", step, ahb);
		return false;
	}
	if (ahb & CBCR_OFF) {
		printf("sgpu: %u: under hardware control (%#x): it runs when the controller's registers are reached\n", step, ahb);
	}

	announce("read the GPU's clock controller (0x3d90000) as it is: the first access to the GPU's part of the SoC");
	get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&gpucc, "pll0 mode", GPUCC_PLL0_MODE);
	get(&gpucc, "pll0 L", GPUCC_PLL0_MODE + 4);
	get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	get(&gpucc, "pll1 L", GPUCC_PLL1_MODE + 4);
	get(&gpucc, "gmu_clk_src command", GPUCC_GMU_RCGR);
	get(&gpucc, "gmu_clk_src source", GPUCC_GMU_RCGR + 4);
	get(&gpucc, "hub_clk_src command", GPUCC_HUB_RCGR);
	get(&gpucc, "hub_clk_src source", GPUCC_HUB_RCGR + 4);
	get(&gpucc, "ff_clk_src command", GPUCC_FF_RCGR);
	get(&gpucc, "cxo_aon", GPUCC_CXO_AON_CBCR);
	get(&gpucc, "demet", GPUCC_DEMET_CBCR);
	get(&gpucc, "gx_gfx3d", GPUCC_GX_GFX3D_CBCR);
	get(&gpucc, "gx_gmu", GPUCC_GX_GMU_CBCR);
	get(&gpucc, "ahb", GPUCC_AHB_CBCR);
	get(&gpucc, "sleep", GPUCC_SLEEP_CBCR);
	get(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR);
	get(&gpucc, "cxo", GPUCC_CXO_CBCR);
	get(&gpucc, "hub_cx_int", GPUCC_HUB_CX_INT_CBCR);
	get(&gpucc, "memnoc_gfx", GPUCC_MEMNOC_GFX_CBCR);
	get(&gpucc, "hub_aon", GPUCC_HUB_AON_CBCR);
	get(&gpucc, "smmu vote", GPUCC_HLOS1_VOTE_GPU_SMMU_CBCR);
	return true;
}

/* ---- step 4: CX power --------------------------------------------------------------------------------- */

#define GPUCC_CX_GDS_HW_CTRL    0x9168          /* bit 31: the CX switch's state, as its hardware controller has it */
#define GDSC_SW_COLLAPSE        (1u << 0)       /* a power switch's control register (gdsc.c): off, by software */
#define GDSC_HW_CONTROL         (1u << 1)       /* something else than software operates it */
#define GDSC_RETAIN_FF          (1u << 11)      /* keep the flip-flops' state through a collapse (RETAIN_FF_ENABLE) */
#define GDSC_POWER_ON           (1u << 31)

/* What the steps turned on, for undo() to turn off again. */
static bool cx_started;
static uint32_t pll_votes_set;
static struct { const char *name; uint32_t cbcr; } clocks_started[16];
static unsigned clocks_started_count;
static uint32_t rcgs_forced[4];
static unsigned rcgs_forced_count;

static bool
cx_is_on(void)
{
	need(&gpucc);
	return (rd(gpucc.base + GPUCC_CX_GDSCR) | rd(gpucc.base + GPUCC_CX_GDS_HW_CTRL)) & GDSC_POWER_ON;
}

static bool stale_stop(void);

/*
 * As Linux's gdsc_enable() for gpu_cx_gdsc (VOTABLE, RETAIN_FF_ENABLE, gds_hw_ctrl 0x9168): clear the
 * collapse bit, wait for the controller's status, then keep the flip-flops on. CX found on is what a
 * kernel before this one left that did not end in order (seen: one cut off in step 20 by the loader's
 * time limit, its GMU and 3D core still running): stale_stop() turns all of it off first.
 */
static bool
step_cx_power(void)
{
	uint32_t gdscr;

	announce("switch the CX half of the GPU block on (gpu_cx_gdsc): clear the collapse bit of its power switch");
	gdscr = get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_gds_hw_ctrl", GPUCC_CX_GDS_HW_CTRL);
	if (cx_is_on()) {
		printf("sgpu: %u: CX is on already: an earlier kernel left the GPU block running; turning it off first\n", step);
		if (!stale_stop()) {
			return false;
		}
		gdscr = get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	}
	if (gdscr & GDSC_HW_CONTROL) {
		printf("sgpu: %u: the switch is under hardware control: not ours to operate\n", step);
		return false;
	}
	put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
	cx_started = true;
	for (int i = 0; i < 500 && !cx_is_on(); i++) {
		delay_for_interval(100, NSEC_PER_USEC);
	}
	gdscr = get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_gds_hw_ctrl", GPUCC_CX_GDS_HW_CTRL);
	if (!cx_is_on()) {
		printf("sgpu: %u: CX does not come on\n", step);
		return false;
	}
	delay_for_interval(1, NSEC_PER_USEC);
	put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, gdscr | GDSC_RETAIN_FF);
	printf("sgpu: %u: CX is on\n", step);
	if (get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR) & GDSC_POWER_ON) {
		printf("sgpu: %u: the 3D core has power already (left on by an earlier kernel?)\n", step);
	}
	return true;
}

/* ---- step 5: clocks ------------------------------------------------------------------------------------ */

#define GCC_PLL_VOTE_GPU        (1u << 15)      /* gcc_gpu_gpll0_clk_src: the 600 MHz tap */
#define GCC_PLL_VOTE_GPU_DIV    (1u << 16)      /* gcc_gpu_gpll0_div_clk_src: the 300 MHz one */
#define RCG_UPDATE              (1u << 0)       /* command register (clk-rcg2.c): take the new source and divider */
#define RCG_ROOT_EN             (1u << 1)       /* ... keep the root on whatever its clocks say */
#define RCG_CFG_MASK            0x371fu         /* configuration: [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define RCG_GPLL0_DIV3          (5u << 8 | 5u)  /* source 5, the 600 MHz tap (gpu_cc_parent_map_0..2), divided by 3: 200 MHz */

/* A clock's source and divider. Its root is held on meanwhile (a "shared" RCG takes nothing while its root is off). */
static bool
rcg_set(const char *name, uint32_t rcgr, uint32_t cfg)
{
	uint32_t command = get(&gpucc, name, rcgr), was = get(&gpucc, name, rcgr + 4);

	put(&gpucc, name, rcgr, command | RCG_ROOT_EN);
	rcgs_forced[rcgs_forced_count++] = rcgr;
	if ((was & RCG_CFG_MASK) != cfg) {
		put(&gpucc, name, rcgr + 4, (was & ~RCG_CFG_MASK) | cfg);
	}
	put(&gpucc, name, rcgr, command | RCG_ROOT_EN | RCG_UPDATE);
	for (int i = 0; i < 100 && (rd(gpucc.base + rcgr) & RCG_UPDATE); i++) {
		delay_for_interval(100, NSEC_PER_USEC);
	}
	if (get(&gpucc, name, rcgr) & RCG_UPDATE) {
		printf("sgpu: %u: %s does not take its new source\n", step, name);
		return false;
	}
	get(&gpucc, name, rcgr + 4);
	return true;
}

/* A clock on, and seen running where its status can be believed (Linux: BRANCH_HALT, BRANCH_HALT_VOTED). */
static bool
clock_on(const char *name, uint32_t cbcr, bool must)
{
	uint32_t was = get(&gpucc, name, cbcr);

	if (!(was & CBCR_ENABLE)) {
		put(&gpucc, name, cbcr, was | CBCR_ENABLE);
		clocks_started[clocks_started_count].name = name;
		clocks_started[clocks_started_count++].cbcr = cbcr;
	}
	for (int i = 0; i < 100 && (rd(gpucc.base + cbcr) & CBCR_OFF); i++) {
		delay_for_interval(100, NSEC_PER_USEC);
	}
	if (get(&gpucc, name, cbcr) & CBCR_OFF) {
		printf("sgpu: %u: %s stays off%s\n", step, name, must ? "" : " (its status bit is not one to rely on: going on)");
		return !must;
	}
	return true;
}

static bool
step_clocks(void)
{
	uint32_t votes;

	if (!cx_is_on()) {
		printf("sgpu: %u: CX is off: step 4\n", step);
		return false;
	}
	announce("in the global clock controller: the GPU's taps of the 600 MHz PLL");
	votes = get(&gcc, "pll votes", GCC_PLL_VOTE);
	if ((votes & (GCC_PLL_VOTE_GPU | GCC_PLL_VOTE_GPU_DIV)) != (GCC_PLL_VOTE_GPU | GCC_PLL_VOTE_GPU_DIV)) {
		pll_votes_set = ~votes & (GCC_PLL_VOTE_GPU | GCC_PLL_VOTE_GPU_DIV);
		put(&gcc, "pll votes", GCC_PLL_VOTE, votes | pll_votes_set);
		sleep_ms(1);
		get(&gcc, "pll votes", GCC_PLL_VOTE);
	}

	announce("in the GPU's clock controller: the GMU's and the hub's clocks to 200 MHz from that tap");
	if (!rcg_set("gmu_clk_src", GPUCC_GMU_RCGR, RCG_GPLL0_DIV3) || !rcg_set("hub_clk_src", GPUCC_HUB_RCGR, RCG_GPLL0_DIV3)) {
		return false;
	}

	announce("in the GPU's clock controller: the clocks of the GMU, the hub, the register bus and the SMMU on");
	if (!clock_on("cxo", GPUCC_CXO_CBCR, true) || !clock_on("ahb", GPUCC_AHB_CBCR, false) ||
	    !clock_on("hub_aon", GPUCC_HUB_AON_CBCR, true) || !clock_on("hub_cx_int", GPUCC_HUB_CX_INT_CBCR, true) ||
	    !clock_on("cx_gmu", GPUCC_CX_GMU_CBCR, true) || !clock_on("memnoc_gfx", GPUCC_MEMNOC_GFX_CBCR, false) ||
	    !clock_on("smmu vote", GPUCC_HLOS1_VOTE_GPU_SMMU_CBCR, true)) {
		return false;
	}
	/* the roots now follow their clocks again */
	for (unsigned i = 0; i < rcgs_forced_count; i++) {
		put(&gpucc, "rcg command", rcgs_forced[i], rd(gpucc.base + rcgs_forced[i]) & ~RCG_ROOT_EN);
	}
	rcgs_forced_count = 0;
	printf("sgpu: %u: the GMU's clocks run\n", step);
	return true;
}

/* ---- step 6: the first words from the GPU block ------------------------------------------------------- */

/*
 * dt: qcom,gmu@3d69000 "gmu" at 0x3d68000, 0x37000 long. The offsets in Linux's a6xx_gmu.xml are words
 * from the GPU's start (0x3d00000; GMU_BYTE_OFFSET in a6xx_gmu.h), the same on A6xx and A7xx for these.
 */
static struct block gmu = { "gmu", 0x03d68000, 0x37000 };
#define GMU_WORD(n)             (4u * (n) - 0x68000u)
#define GMU_CM3_SYSRESET        GMU_WORD(0x1f800)       /* 1: the GMU's processor held in reset */
#define GMU_CM3_BOOT_CONFIG     GMU_WORD(0x1f801)
#define GMU_CM3_FW_INIT_RESULT  GMU_WORD(0x1f81c)       /* what started firmware says */
#define GMU_CM3_CFG             GMU_WORD(0x1f82d)
#define GMU_ALWAYS_ON_COUNTER_L GMU_WORD(0x1f888)       /* counts the 19.2 MHz crystal */
#define GMU_ALWAYS_ON_COUNTER_H GMU_WORD(0x1f889)
#define GMU_SPTPRAC_PWR_CLK_STATUS GMU_WORD(0x1f8d0)    /* the 3D core's power and clock, as the GMU sees them */
#define GMU_HFI_CTRL_STATUS     GMU_WORD(0x1f980)
#define GMU_HFI_VERSION_INFO    GMU_WORD(0x1f981)
#define GMU_GENERAL_0           GMU_WORD(0x1f9c5)
#define GMU_GENERAL_7           GMU_WORD(0x1f9cc)
#define GMU_AO_HOST_INTERRUPT_STATUS GMU_WORD(0x23b05)
#define GMU_RSCC_CONTROL_ACK    GMU_WORD(0x23b08)
#define GMU_AHB_FENCE_STATUS    GMU_WORD(0x23b13)

/* the GPU's bus interface: words 0x3c00 on of the 3D core's registers, but in the CX half (a6xx.xml) */
static struct block gbif = { "gbif", 0x03d00000 + 4 * 0x3c00, 0x1000 };
#define GBIF_SCACHE_CNTL0       (4 * 0x01)
#define GBIF_HALT               (4 * 0x45)
#define GBIF_HALT_ACK           (4 * 0x46)

/* dt: qcom,kgsl-3d0 "rscc" at 0x3d50000: the GMU's command slots to RPMh (words: a6xx_gmu.xml, A6XX_RSCC) */
static struct block rscc = { "rscc", 0x03d50000, 0x10000 };
#define RSCC_RSC_STATUS0_DRV0   (4 * 0x004)
#define RSCC_PDC_SLAVE_ID_DRV0  (4 * 0x00b)
#define RSCC_SEQ_BUSY_DRV0      (4 * 0x101)
#define RSCC_SEQ_MEM_0_DRV0_A7  (4 * 0x154)
#define RSCC_TCS0_DRV0_STATUS   (4 * 0x346)

/* CX on and the GMU's clock running: what steps 6 on need (steps 4 and 5 give it). */
static bool
gmu_ready(void)
{
	if (!cx_is_on() || (rd(gpucc.base + GPUCC_CX_GMU_CBCR) & CBCR_OFF)) {
		printf("sgpu: %u: CX or the GMU's clock is off: steps 4 and 5\n", step);
		return false;
	}
	return true;
}

/* How fast a counter of the GMU's counts, over 10 ms of the kernel's clock. */
static uint32_t
counter_hz(uint32_t offset)
{
	uint64_t start, nanos;
	uint32_t before, counted;

	start = mach_absolute_time();
	before = rd(gmu.base + offset);
	sleep_ms(10);
	counted = rd(gmu.base + offset) - before;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)((uint64_t)counted * NSEC_PER_SEC / nanos) : 0;
}

static const char *
crystal(uint32_t hz)
{
	return hz > 18000000 && hz < 20500000 ? "the 19.2 MHz crystal: it counts" : hz ? "not the crystal's 19.2 MHz" : "it stands";
}

static bool
step_first_words(void)
{
	uint32_t hz;

	if (!gmu_ready()) {
		return false;
	}
	announce("read registers of the GMU (0x3d68000): the first words from the GPU block itself");
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	get(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG);
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "cm3_cfg", GMU_CM3_CFG);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	get(&gmu, "general_0", GMU_GENERAL_0);
	get(&gmu, "general_7", GMU_GENERAL_7);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
	get(&gmu, "always_on_counter_h", GMU_ALWAYS_ON_COUNTER_H);
	get(&gmu, "always_on_counter_l", GMU_ALWAYS_ON_COUNTER_L);
	hz = counter_hz(GMU_ALWAYS_ON_COUNTER_L);
	printf("sgpu: %u: the GMU's older always-on counter: %u Hz, %s (the A750's own is step 7's)\n", step, hz,
	    crystal(hz));

	announce("read the GPU's bus interface (0x3d0f000) and the GMU's channel to RPMh (0x3d50000)");
	get(&gbif, "scache_cntl0", GBIF_SCACHE_CNTL0);
	get(&gbif, "halt", GBIF_HALT);
	get(&gbif, "halt_ack", GBIF_HALT_ACK);
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0_A7);
	get(&rscc, "tcs0_drv0_status", RSCC_TCS0_DRV0_STATUS);
	return true;
}

/* ---- step 7: what TrustZone does for the GPU before the GMU starts --------------------------------- */

/*
 * a6xx_gmu.c: a6xx_gmu_secure_init, the first thing Linux does on an A750 once the GMU has power
 * and clocks: it asks TrustZone, once, to program "a set of access controlled registers necessary
 * irrespective of any features" (QCOM_SCM_GPU_ALWAYS_EN_REQ) and those that let the temperature
 * sensors be read (QCOM_SCM_GPU_TSENSE_EN_REQ). The same call sets the word that says what this GPU
 * may do (CX_MISC_SW_FUSE_VALUE: ray tracing among it). The A750's always-on counter is another
 * than the one step 6 read: a6xx_gpu.c, a6xx_gmu_get_timestamp, reads GMU_CX_AO_COUNTER on the A750.
 * A call is an SMC, which the hypervisor passes on (convention: qcom_scm-smc.c, as redwood_adreno.c).
 */
#define GMU_CX_AO_COUNTER_L     GMU_WORD(0x1f880)
#define GMU_CX_AO_COUNTER_H     GMU_WORD(0x1f881)

/* dt: qcom,kgsl-3d0 "cx_misc" at 0x3d9e000 (Linux: "cx_mem"); words: a6xx.xml, A6XX_CX_MISC */
static struct block cx_misc = { "cx_misc", 0x03d9e000, 0x2000 };
#define CX_MISC_TCM_RET_CNTL    (4 * 0x039)     /* 1: the GMU's memories kept through a collapse (a warm start) */
#define CX_MISC_SW_FUSE_VALUE   (4 * 0x400)     /* bit 0 fast blending, 1 LPAC, 2 ray tracing */

#define SCM_FAST                (1ull << 31)    /* answered at once; without it a call may be preempted */
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)    /* a call of the SoC maker's, not one of the Arm standard */
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_IS_CALL_AVAILABLE   SCM_CALL(6, 1)
#define SCM_GPU_INIT_REGS       SCM_CALL(0x28, 1)       /* qcom_scm.h: QCOM_SCM_SVC_GPU, QCOM_SCM_SVC_GPU_INIT_REGS */
#define SCM_GPU_ALWAYS_EN_REQ   (1u << 0)
#define SCM_GPU_TSENSE_EN_REQ   (1u << 3)
#define SCM_ONE_VALUE           1               /* the word describing the arguments: one, a plain number */
#define SCM_INTERRUPTED         1               /* a call that was preempted says so, and is made again so */
#define SCM_WAITQ_SLEEP         2               /* "wait for my interrupt": Linux's wait queues, not done here */
#define SCM_EBUSY               (-12)           /* busy: Linux tries again after 30 ms, up to 20 times */

/* One call. What TrustZone returns: 0 or an error of its own, and a word of answer. */
static int32_t
scm_call(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer)
{
	printf("sgpu: %u: about to ask TrustZone %s: smc %#llx (%#llx, %#llx, %#llx)\n", step, what, function, first, second,
	    third);
	hold();
	for (int busy = 0; busy <= 20; busy++) {
		uint64_t session = 0;
		bool is_busy = false;

		for (int i = 0; i < 100; i++) {
			register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
			register uint64_t x1 __asm__("x1") = arguments;
			register uint64_t x2 __asm__("x2") = first;
			register uint64_t x3 __asm__("x3") = second;
			register uint64_t x4 __asm__("x4") = third;
			register uint64_t x5 __asm__("x5") = 0;
			register uint64_t x6 __asm__("x6") = session;
			register uint64_t x7 __asm__("x7") = 0;

			__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
			    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
			session = x6;
			if (x0 == SCM_INTERRUPTED) {
				continue;
			}
			if ((int32_t)x0 == SCM_EBUSY) {
				is_busy = true;
				break;
			}
			if (x0 == SCM_WAITQ_SLEEP) {
				printf("sgpu: %u: TrustZone wants to be resumed on its interrupt (wait queue %#llx, call %#llx): not "
				    "done here\n", step, x1, x2);
				return -1;
			}
			printf("sgpu: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
		if (!is_busy || busy == 20) {
			break;          /* interrupted a hundred times, or busy twenty: a new call would be another call */
		}
		printf("sgpu: %u: TrustZone is busy: again in 30 ms\n", step);
		sleep_ms(30);
	}
	printf("sgpu: %u: TrustZone stays busy, or keeps saying the call was interrupted\n", step);
	return -1;
}

/* A call with one plain number. */
static int32_t
scm(const char *what, uint64_t function, uint64_t first, uint64_t *answer)
{
	return scm_call(what, function, SCM_ONE_VALUE, first, 0, 0, answer);
}

static bool
step_trustzone(void)
{
	uint64_t answer = 0;
	uint32_t fuse, hz;
	int32_t result;

	if (!gmu_ready()) {
		return false;
	}
	announce("read the A750's always-on counter of the GMU, the GPU's fuse word, and whether its GMU's memories were kept");
	get(&gmu, "cx_ao_counter_h", GMU_CX_AO_COUNTER_H);
	get(&gmu, "cx_ao_counter_l", GMU_CX_AO_COUNTER_L);
	hz = counter_hz(GMU_CX_AO_COUNTER_L);
	printf("sgpu: %u: the A750's always-on counter: %u Hz, %s\n", step, hz, crystal(hz));
	get(&cx_misc, "tcm_ret_cntl", CX_MISC_TCM_RET_CNTL);
	get(&cx_misc, "sw_fuse_value", CX_MISC_SW_FUSE_VALUE);

	announce("ask TrustZone what Linux asks it first: is the call \"is this call available\" available");
	if (scm("whether it can be asked", SCM_FAST | SCM_64 | SCM_SIP | SCM_IS_CALL_AVAILABLE, SCM_SIP | SCM_IS_CALL_AVAILABLE,
	    &answer) != 0 || answer != 1) {
		printf("sgpu: %u: TrustZone does not answer the way it answers Linux: not asking it more\n", step);
		return false;
	}
	answer = 0;
	result = scm("whether it has the GPU's call (service 0x28, command 1)", SCM_64 | SCM_SIP | SCM_IS_CALL_AVAILABLE,
	    SCM_SIP | SCM_GPU_INIT_REGS, &answer);
	printf("sgpu: %u: %s\n", step, result == 0 && answer == 1 ? "it has" : "it says it has not: asking all the same, "
	    "as Linux does without asking first (a call it does not have only comes back with an error)");

	announce("ask TrustZone to program the GPU's access controlled registers and its temperature sensors', as Linux "
	    "does on an A750 before the GMU's firmware starts");
	answer = 0;
	result = scm("to program the GPU's registers", SCM_64 | SCM_SIP | SCM_GPU_INIT_REGS,
	    SCM_GPU_ALWAYS_EN_REQ | SCM_GPU_TSENSE_EN_REQ, &answer);

	announce("read again what that call is to have changed");
	fuse = get(&cx_misc, "sw_fuse_value", CX_MISC_SW_FUSE_VALUE);
	printf("sgpu: %u: this GPU may: fast blending %s, LPAC %s, ray tracing %s\n", step, fuse & 1 ? "yes" : "no",
	    fuse & 2 ? "yes" : "no", fuse & 4 ? "yes" : "no");
	get(&gmu, "cx_ao_counter_l", GMU_CX_AO_COUNTER_L);
	hz = counter_hz(GMU_CX_AO_COUNTER_L);
	printf("sgpu: %u: the A750's always-on counter now: %u Hz, %s\n", step, hz, crystal(hz));
	hz = counter_hz(GMU_ALWAYS_ON_COUNTER_L);
	printf("sgpu: %u: the older one: %u Hz, %s\n", step, hz, crystal(hz));
	if (result != 0) {
		printf("sgpu: %u: TrustZone did not do it (%d): without it Linux does not start the GMU\n", step, result);
		return false;
	}
	printf("sgpu: %u: TrustZone did what Linux asks of it here\n", step);
	return true;
}

/* ---- steps 8 and 9: the SMMU in front of the GPU ------------------------------------------------------- */

/*
 * dt: kgsl-smmu@3da0000 (qcom,qsmmu-v500: an Arm MMU-500), 0x40000 long; Linux sm8650.dtsi agrees.
 * Its streams (Linux sm8650.dtsi, "iommus"): 0 the GPU, 1 its LPAC twin, 5 the GMU. Its clocks are
 * step 5's (gpu_cc_hlos1_vote_gpu_smmu, gpu_cc_ahb) and the global controller's (gcc_gpu_memnoc_gfx,
 * gcc_gpu_snoc_dvm_gfx, on as the boot loader left them). Offsets: Linux arm-smmu.h.
 */
static struct block smmu = { "smmu", 0x03da0000, 0x40000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020           /* [7:0] stream match registers */
#define SMMU_IDR1               0x024           /* bit 31: 64K pages, [30:28] log2(pages)-1, [7:0] translation contexts */
#define SMMU_IDR2               0x028
#define SMMU_IDR7               0x03c
#define SMMU_SGFSR              0x048
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [15:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))               /* in the second page; [17:16] kind of context */
#define SMMU_CB_SCTLR           0x00            /* a context's own page: bit 0 = translation on */
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_FSR             0x58
#define SMMU_SMR_VALID          (1u << 31)

static uint32_t smmu_page, smmu_pages, smmu_contexts, smmu_matches;

static bool
smmu_ready(void)
{
	if (!gmu_ready()) {
		return false;
	}
	if (rd(gpucc.base + GPUCC_HLOS1_VOTE_GPU_SMMU_CBCR) & CBCR_OFF) {
		printf("sgpu: %u: the SMMU's clock is off: step 5\n", step);
		return false;
	}
	return true;
}

/* The layout of the SMMU's register space from its ID registers; false if it is not what the device tree says. */
static bool
smmu_layout(void)
{
	uint32_t idr0 = get(&smmu, "idr0", SMMU_IDR0), idr1 = get(&smmu, "idr1", SMMU_IDR1);

	smmu_page = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	smmu_contexts = idr1 & 0xff;
	smmu_matches = idr0 & 0xff;
	printf("sgpu: %u: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n", step,
	    smmu_matches, smmu_contexts, smmu_pages, smmu_page);
	if (2 * smmu_pages * smmu_page != smmu.size) {
		printf("sgpu: %u: that makes %#x of registers, the device tree says %#x: not reading further\n", step,
		    2 * smmu_pages * smmu_page, smmu.size);
		return false;
	}
	return true;
}

static bool
step_smmu(void)
{
	uint32_t smr, unused = 0;

	if (!smmu_ready()) {
		return false;
	}
	announce("read the ID registers of the GPU's SMMU (0x3da0000): global registers, which the hypervisor may answer for");
	if (!smmu_layout()) {
		return false;
	}
	get(&smmu, "idr2", SMMU_IDR2);
	get(&smmu, "idr7", SMMU_IDR7);
	get(&smmu, "scr0", SMMU_SCR0);
	get(&smmu, "sgfsr", SMMU_SGFSR);

	announce("read the SMMU's stream table and the kinds of its contexts, as the boot loader and hypervisor left them");
	get(&smmu, "smr 0", SMMU_SMR(0));
	get(&smmu, "s2cr 0", SMMU_S2CR(0));
	for (uint32_t i = 0; i < smmu_matches && i < 128; i++) {
		smr = peek(&smmu, SMMU_SMR(i));
		if (smr & SMMU_SMR_VALID) {
			printf("sgpu: %u: stream match %u: smr %#x (stream %#x, mask %#x), s2cr %#x\n", step, i, smr, smr & 0xffff,
			    smr >> 16 & 0x7fff, peek(&smmu, SMMU_S2CR(i)));
		} else {
			unused++;
		}
	}
	printf("sgpu: %u: %u stream match registers unused\n", step, unused);
	get(&smmu, "cbar 0", smmu_page + SMMU_CBAR(0));
	for (uint32_t i = 0; i < smmu_contexts && i < 32; i += 4) {
		printf("sgpu: %u: cbar %u-%u: %#x %#x %#x %#x\n", step, i, i + 3, peek(&smmu, smmu_page + SMMU_CBAR(i)),
		    i + 1 < smmu_contexts ? peek(&smmu, smmu_page + SMMU_CBAR(i + 1)) : 0,
		    i + 2 < smmu_contexts ? peek(&smmu, smmu_page + SMMU_CBAR(i + 2)) : 0,
		    i + 3 < smmu_contexts ? peek(&smmu, smmu_page + SMMU_CBAR(i + 3)) : 0);
	}
	return true;
}

/*
 * Only context 0, the one Linux gives the GPU itself: of the others at least one belongs to
 * TrustZone (secure content), and not all can be expected to answer.
 */
static bool
step_smmu_context(void)
{
	uint32_t context;

	if (!smmu_ready()) {
		return false;
	}
	announce("read translation context 0 of the SMMU: registers of the hardware itself, not the hypervisor's");
	if (!smmu_layout()) {
		return false;
	}
	context = smmu_pages * smmu_page;
	get(&smmu, "cb0 sctlr", context + SMMU_CB_SCTLR);
	get(&smmu, "cb0 ttbr0, low", context + SMMU_CB_TTBR0);
	get(&smmu, "cb0 ttbr0, high", context + SMMU_CB_TTBR0 + 4);
	get(&smmu, "cb0 ttbr1, high", context + SMMU_CB_TTBR1 + 4);
	get(&smmu, "cb0 tcr", context + SMMU_CB_TCR);
	get(&smmu, "cb0 fsr", context + SMMU_CB_FSR);
	return true;
}

/* ---- step 10: the GMU's memory -------------------------------------------------------------------------- */

/*
 * The GMU's firmware is loaded by writing it through these registers into its processor's memory
 * while the processor is held in reset (a6xx_gmu_fw_load; on the A7xx too, at the same words: the
 * data memory at GMU_CM3_DTCM_START, 0x1c400). Two words go there and back, and what was there is
 * put back.
 */
#define GMU_DTCM                GMU_WORD(0x1c400)
#define GMU_FW_RUNNING          0xbabeface      /* what started firmware writes into cm3_fw_init_result */

static bool
step_gmu_memory(void)
{
	static const uint32_t pattern[2] = { 0x600d0001, 0x0a110002 };
	uint32_t was[2], back[2];

	if (!gmu_ready()) {
		return false;
	}
	announce("hold the GMU's processor in reset, write two words into its data memory (0x3d71000), read them back");
	if (get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT) == GMU_FW_RUNNING) {
		printf("sgpu: %u: firmware is running in the GMU: leaving its memory alone\n", step);
		return false;
	}
	if (get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET) != 1) {
		put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
		get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	}
	for (int i = 0; i < 2; i++) {
		was[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, pattern[i]);
	}
	for (int i = 0; i < 2; i++) {
		back[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, was[i]);
	}
	printf("sgpu: %u: the GMU's memory %s\n", step, back[0] == pattern[0] && back[1] == pattern[1] ?
	    "TAKES WHAT IS WRITTEN: its firmware can be loaded" : "does not hold what was written");
	return back[0] == pattern[0] && back[1] == pattern[1];
}

/* ---- helpers for what follows ----------------------------------------------------------------------------- */

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

/* Push what this processor has written to a stretch of memory out of its cache, and forget it: the GPU block reads memory. */
static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
}

static bool
wait_for(struct block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms)
{
	need(b);
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd(b->base + offset) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return (rd(b->base + offset) & mask) == value;
}

/*
 * A file of the small image that travels with the kernel (root.apfs: userland/build.sh puts what is
 * in fw-gpu/ into its directory fw/), found in the memory of the RAM disk itself.
 */
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static const uint8_t *
image_file(const char *directory, const char *name, uint64_t *size)
{
	int disk = -1;
	const uint8_t *data = redwood_image_file(directory, name, size, &disk);  /* (redwood_image.c) */

	if (data != NULL) {
		printf("sgpu: %u: %s/%s: %llu bytes in the image of /dev/md%d\n", step, directory, name, *size, disk);
		return data;
	}
	printf("sgpu: %u: no %s/%s in the small image: put the phone's %s into build/fw-gpu/ on the Mac and run build-ios.sh\n",
	    step, directory, name, name);
	return NULL;
}

/* ---- memory the GPU block reaches ------------------------------------------------------------------- */

/*
 * Whatever in the GPU block reads or writes memory -- the GMU's processor, later the 3D core --
 * does it through the SMMU, which makes of the address it uses ("the GPU's address" below) a
 * physical one by walking page tables of ours, as a processor's MMU does: three levels of 4K
 * pages, 39 bits of address (dt: qcom,use-3-lvl-tables), 36 of output (step 8: idr2 0x5511). The
 * SMMU has 22 such translations ("contexts") and a table of which stream of requests -- 0 the 3D
 * core, 5 the GMU (dt: iommus, with the mask 0 here) -- goes through which. Formats and the order
 * of programming: Linux drivers/iommu/io-pgtable-arm.c and arm-smmu.c; as redwood_adreno.c.
 */
#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_CACHED              (1ull << 2)     /* memory attribute 1 of SMMU_MAIR0; without it 0, uncached */
#define PTE_UNPRIVILEGED        (1ull << 6)     /* the 3D core's accesses are; the GMU's are not */
#define PTE_SHARED              (2ull << 8)
#define PTE_ACCESSED            (1ull << 10)
#define PTE_NOT_GLOBAL          (1ull << 11)
#define PTE_ADDRESS             0x0000fffffffff000ull
#define PTE_GMU                 (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)

#define SMMU_S2CR_FAULT         (2u << 16)
#define SMMU_STREAM_MASK        0x0             /* dt: the second cell of every "iommus" of this SMMU */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)       /* our tables alone decide; weakest defaults */
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))                       /* in the second page, like CBAR */
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12) /* on; faults recorded, no interrupt */
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)   /* one table of 4K pages, cached walks */
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)       /* 36 bits of physical address, 16 of context tag */
#define SMMU_MAIR0              0xf404ff44      /* Linux's: 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001fe      /* every kind of fault, and "several" */

#define SPACE_TABLES            2048

struct gpu_space {
	const char *name;
	uint32_t context;               /* which of the SMMU's */
	uint32_t control;               /* bits of its control register besides the usual */
	uint32_t table_count;
	uint64_t *root;
	struct {
		uint64_t phys;
		uint64_t *table;
	} tables[SPACE_TABLES];
};

static uint64_t *
space_new_table(struct gpu_space *space)
{
	static vm_offset_t spare;
	static unsigned spare_left;
	uint64_t *table;

	if (space->table_count == SPACE_TABLES) {
		return NULL;
	}
	if (spare_left == 0) {
		if (kmem_alloc(kernel_map, &spare, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
		    KERN_SUCCESS) {
			return NULL;
		}
		spare_left = PAGE_SIZE / 4096;
	}
	table = (uint64_t *)(spare + PAGE_SIZE - 4096 * spare_left--);
	space->tables[space->table_count].phys = kvtophys((vm_offset_t)table);
	space->tables[space->table_count++].table = table;
	return table;
}

/* The table an entry of a higher table leads to; made if the entry is empty. */
static uint64_t *
space_next(struct gpu_space *space, uint64_t *entry)
{
	uint64_t *table;

	if (*entry & 1) {
		for (uint32_t i = 0; i < space->table_count; i++) {
			if (space->tables[i].phys == (*entry & PTE_ADDRESS)) {
				return space->tables[i].table;
			}
		}
		return NULL;
	}
	table = space_new_table(space);
	if (table != NULL) {
		*entry = kvtophys((vm_offset_t)table) | PTE_TABLE;
	}
	return table;
}

/* One 4K page. */
static bool
space_map(struct gpu_space *space, uint64_t address, uint64_t phys, uint64_t attributes)
{
	uint64_t *level2, *level3;

	if (address >> 39 || phys >> 36 || (space->root == NULL && (space->root = space_new_table(space)) == NULL)) {
		return false;
	}
	level2 = space_next(space, &space->root[address >> 30 & 0x1ff]);
	level3 = level2 == NULL ? NULL : space_next(space, &level2[address >> 21 & 0x1ff]);
	if (level3 == NULL) {
		return false;
	}
	level3[address >> 12 & 0x1ff] = phys | attributes | PTE_PAGE;
	return true;
}

/* The SMMU reads the tables from memory, not from this processor's cache. */
static void
space_commit(struct gpu_space *space)
{
	for (uint32_t i = 0; i < space->table_count; i++) {
		to_memory((vm_offset_t)space->tables[i].table, 4096);
	}
}

/* Memory of ours that the GPU block sees at an address of its own. */
struct gpu_block {
	uint64_t address;               /* the GPU's */
	uint32_t size;
	vm_offset_t memory;             /* ours */
};

static bool
block_new(struct gpu_space *space, struct gpu_block *block, uint64_t address, uint32_t size, uint64_t attributes)
{
	block->address = address;
	block->size = (size + 4095) & ~4095u;
	if (kmem_alloc(kernel_map, &block->memory, round_page(block->size), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("sgpu: %u: no %#x bytes of memory\n", step, block->size);
		return false;
	}
	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (!space_map(space, address + offset, kvtophys(block->memory + offset), attributes)) {
			printf("sgpu: %u: cannot map %#llx for %s\n", step, address + offset, space->name);
			return false;
		}
	}
	to_memory(block->memory, block->size);
	printf("sgpu: %u: %s sees %#llx..%#llx in memory of ours (from %#llx)\n", step, space->name, address,
	    address + block->size - 1, kvtophys(block->memory));
	return true;
}

/* A second view of a block without this processor's cache in between, for memory both sides write. Blocks in one piece only. */
static vm_offset_t
block_uncached(const struct gpu_block *block)
{
	uint64_t phys = kvtophys(block->memory);

	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (kvtophys(block->memory + offset) != phys + offset) {
			return 0;
		}
	}
	return ml_io_map_wcomb(phys, block->size);
}

static uint32_t
smmu_context(const struct gpu_space *space)
{
	return (smmu_pages + space->context) * smmu_page;
}

/* What a context has recorded of a request it could not serve. */
static bool
space_faulted(const struct gpu_space *space)
{
	uint32_t context = smmu_context(space), status = rd(smmu.base + context + SMMU_CB_FSR);

	if ((status & SMMU_FSR_ALL) == 0) {
		return false;
	}
	printf("sgpu: %u: THE SMMU REFUSED %s: status %#x, address %#llx, syndrome %#x\n", step, space->name, status,
	    (uint64_t)rd(smmu.base + context + SMMU_CB_FAR + 4) << 32 | rd(smmu.base + context + SMMU_CB_FAR),
	    rd(smmu.base + context + SMMU_CB_FSYNR0));
	return true;
}

/* Give a context our tables and send a stream through it; `match` is the entry of the stream table to use. */
static bool
space_on(struct gpu_space *space, uint32_t match, uint32_t stream)
{
	uint32_t context = smmu_context(space);
	uint64_t root = space->tables[0].phys | (uint64_t)space->context << 48;        /* tagged with the context's number */

	space_commit(space);
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, 0);
	put(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context), SMMU_CBA2R_64_BIT);
	put(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context), SMMU_CBAR_STAGE_1);
	put(&smmu, "cb tcr2", context + SMMU_CB_TCR2, SMMU_TCR2);
	put(&smmu, "cb tcr", context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0, (uint32_t)root);
	put(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	put(&smmu, "cb ttbr1, low", context + SMMU_CB_TTBR1, 0);
	put(&smmu, "cb ttbr1, high", context + SMMU_CB_TTBR1 + 4, 0);
	put(&smmu, "cb mair0", context + SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&smmu, "cb mair1", context + SMMU_CB_MAIR1, 0);
	put(&smmu, "cb fsr", context + SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&smmu, "cb tlbiasid", context + SMMU_CB_TLBIASID, space->context);
	put(&smmu, "cb tlbsync", context + SMMU_CB_TLBSYNC, 1);
	if (!wait_for(&smmu, context + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		printf("sgpu: %u: the SMMU does not finish forgetting old translations (%#x)\n", step,
		    rd(smmu.base + context + SMMU_CB_TLBSTATUS));
		return false;
	}
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, SMMU_SCTLR_ON | space->control);
	put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
	put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream);
	if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream) ||
	    (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context ||
	    (get(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
	    !(get(&smmu, "cb sctlr", context + SMMU_CB_SCTLR) & 1) ||
	    get(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0) != (uint32_t)root ||
	    get(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		printf("sgpu: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
		    stream, space->context);
		return false;
	}
	get(&smmu, "cb tcr", context + SMMU_CB_TCR);
	get(&smmu, "cb fsr", context + SMMU_CB_FSR);
	get(&smmu, "sgfsr", SMMU_SGFSR);
	printf("sgpu: %u: stream %u goes through context %u: %s reaches our memory\n", step, stream, space->context, space->name);
	return true;
}

/* A stream goes nowhere again, and its context translates nothing. */
static void
space_off(struct gpu_space *space, uint32_t match)
{
	wr(smmu.base + SMMU_SMR(match), 0);
	wr(smmu.base + SMMU_S2CR(match), SMMU_S2CR_FAULT);
	wr(smmu.base + smmu_context(space) + SMMU_CB_SCTLR, 0);
}

/* ---- step 11: memory for the GMU ------------------------------------------------------------------------ */

/*
 * The GMU is a Cortex-M3 with 16K of code memory and 16K of data memory of its own (step 10); the
 * rest of its firmware and everything it shares with us it reaches through the SMMU, at addresses
 * the firmware file names: a file of blocks, each "this goes at address A" or, with no data, "I
 * need N bytes at address A" (a6xx_gmu.c: a6xx_gmu_fw_load, and the vendor's gen7_gmu_parse_fw). The
 * log and the message page have no address of their own: the vendor's and Linux's driver put them
 * in the GMU's uncached range, 0x60000000 on (a6xx_gmu_memory_alloc).
 */
#define GMU_ITCM_ADDRESS        0x00000000u
#define GMU_DTCM_ADDRESS        0x10004000u     /* a6xx_gmu_fw_load: the A7xx's */
#define GMU_TCM_SIZE            0x4000u
#define GMU_UNCACHED            0x60000000u
#define GMU_UNCACHED_END        0x80000000u     /* on: the vendor's "user" range, nothing of the kernel's */
#define GMU_LOG_SIZE            0x4000u
#define GMU_HFI_SIZE            0x4000u
#define GMU_STREAM              5               /* dt: qcom,gmu "iommus" */
#define GMU_MATCH               1               /* entry of the stream table; 0 is kept for the 3D core */
#define GMU_REGIONS             16
#define GMU_REGION_MOST         0x1000000u      /* Linux gives the A7xx's "icache" 16M - 16K */

struct gmu_firmware_block {
	uint32_t address, size, type, value;
};
#define GMU_BLOCK_NEEDS         1               /* types of blocks without data: memory wanted, value = bytes */
#define GMU_BLOCK_NEEDS_KEPT    7               /* (2 to 6: the versions of its parts, in value) */

static struct gpu_space gmu_space = { .name = "the GMU", .context = 1 };
static struct gpu_block gmu_regions[GMU_REGIONS], gmu_log, gmu_hfi;
static unsigned gmu_region_count;
static const uint8_t *gmu_firmware;
static uint64_t gmu_firmware_size;
static bool gmu_space_is_on, gmu_runs, rscc_set_up, tcm_retained, hfi_up, gmu_started, core_asked;
static bool core_is_on, core_space_is_on, core_runs, zap_is_loaded;    /* steps 18 to 21 */

static bool
in_tcm(uint32_t address, uint32_t size)
{
	return (address - GMU_ITCM_ADDRESS < GMU_TCM_SIZE && address - GMU_ITCM_ADDRESS + size <= GMU_TCM_SIZE) ||
	       (address - GMU_DTCM_ADDRESS < GMU_TCM_SIZE && address - GMU_DTCM_ADDRESS + size <= GMU_TCM_SIZE);
}

/* Which region holds address..address+size; -1 none, -2 one holds part of it. */
static int
gmu_region(uint64_t address, uint64_t size)
{
	for (unsigned i = 0; i < gmu_region_count; i++) {
		const struct gpu_block *r = &gmu_regions[i];

		if (address >= r->address && address + size <= r->address + r->size) {
			return (int)i;
		}
		if (address < r->address + r->size && address + size > r->address) {
			return -2;
		}
	}
	return -1;
}

static bool
gmu_region_new(uint32_t address, uint32_t size, const char *why)
{
	uint32_t start = address & ~4095u, end = (address + size + 4095) & ~4095u;

	if (gmu_region(start, end - start) == -2 || gmu_region_count == GMU_REGIONS || end - start > GMU_REGION_MOST) {
		printf("sgpu: %u: cannot make %#x..%#x (%s): it overlaps another, is too big, or there are too many\n", step,
		    start, end - 1, why);
		return false;
	}
	printf("sgpu: %u: %#x..%#x: %s\n", step, start, end - 1, why);
	return block_new(&gmu_space, &gmu_regions[gmu_region_count++], start, end - start, PTE_GMU);
}

/* The first address of the uncached range with `size` free after it. */
static uint32_t
gmu_free(uint32_t size)
{
	for (uint32_t at = GMU_UNCACHED; at + size <= GMU_UNCACHED_END; at += 0x4000) {
		if (gmu_region(at, size) == -1 && (gmu_log.size == 0 || at + size <= gmu_log.address ||
		    at >= gmu_log.address + gmu_log.size)) {
			return at;
		}
	}
	return 0;
}

/* Every block of the firmware once, in order; false if the file is not made of whole blocks. */
static bool
gmu_firmware_next(uint64_t *at, struct gmu_firmware_block *block, const uint8_t **data)
{
	if (*at + sizeof(*block) > gmu_firmware_size) {
		return false;
	}
	memcpy(block, gmu_firmware + *at, sizeof(*block));
	*data = gmu_firmware + *at + sizeof(*block);
	*at += sizeof(*block) + block->size;
	return *at <= gmu_firmware_size;
}

static bool
step_gmu_space(void)
{
	static const char *const kinds[8] = { "data", "needs", "core version", "core dev version", "power version",
		                              "power dev version", "HFI version", "needs, kept" };
	struct gmu_firmware_block block;
	const uint8_t *data;
	uint64_t at = 0;
	unsigned count = 0;
	uint32_t log, hfi_at;

	if (!smmu_ready()) {
		return false;
	}
	announce("find the GMU's firmware, fw/gmu_gen70900.bin, in the small image, and list its blocks");
	gmu_firmware = image_file("fw", "gmu_gen70900.bin", &gmu_firmware_size);
	if (gmu_firmware == NULL) {
		return false;
	}
	while (gmu_firmware_next(&at, &block, &data)) {
		printf("sgpu: %u: block %u: at %#x, %#x bytes, %s (%#x), value %#x%s\n", step, count++, block.address, block.size,
		    block.size != 0 ? "data" : block.type < 8 ? kinds[block.type] : "?", block.type, block.value, block.size == 0 ? "" :
		    in_tcm(block.address, block.size) ? ": into the GMU's own memory" : ": into memory of ours");
	}
	if (at != gmu_firmware_size) {
		printf("sgpu: %u: the file does not end with a whole block (%llu of %llu bytes): not that kind of file\n", step,
		    at, gmu_firmware_size);
		return false;
	}

	announce("make the memory the firmware asks for, what its blocks outside the GMU's own memories need, a log and the "
	    "message page");
	for (at = 0; gmu_firmware_next(&at, &block, &data);) {
		if (block.size == 0 && (block.type == GMU_BLOCK_NEEDS || block.type == GMU_BLOCK_NEEDS_KEPT) && block.value != 0 &&
		    !in_tcm(block.address, block.value) && block.address < GMU_UNCACHED_END &&
		    gmu_region(block.address, block.value) == -1 &&
		    !gmu_region_new(block.address, block.value, "the firmware asks for it")) {
			return false;
		}
	}
	for (at = 0; gmu_firmware_next(&at, &block, &data);) {
		if (block.size != 0 && !in_tcm(block.address, block.size) && gmu_region(block.address, block.size) == -1 &&
		    !gmu_region_new(block.address, block.size, "for a block of the firmware, which asks for nothing there")) {
			return false;
		}
	}
	/* without a fixed address Linux maps memory for the unprivileged too (a6xx_gmu_memory_alloc: no MSM_BO_MAP_PRIV) */
	log = gmu_free(GMU_LOG_SIZE);
	if (log == 0 || !block_new(&gmu_space, &gmu_log, log, GMU_LOG_SIZE, PTE_GMU | PTE_UNPRIVILEGED)) {
		return false;
	}
	hfi_at = gmu_free(GMU_HFI_SIZE);
	if (hfi_at == 0 || !block_new(&gmu_space, &gmu_hfi, hfi_at, GMU_HFI_SIZE, PTE_GMU | PTE_UNPRIVILEGED)) {
		return false;
	}
	printf("sgpu: %u: %u regions for the firmware, the log at %#llx, the messages at %#llx; %u page tables\n", step,
	    gmu_region_count, gmu_log.address, gmu_hfi.address, gmu_space.table_count);

	announce("give the SMMU's context 1 these tables and send the GMU's requests (stream 5) through it: the first writes "
	    "to the SMMU");
	if (!smmu_layout()) {
		return false;
	}
	gmu_space_is_on = true;         /* whatever of it has been written by the time something fails is taken back */
	return space_on(&gmu_space, GMU_MATCH, GMU_STREAM);
}

/* ---- step 12: the GMU's firmware ----------------------------------------------------------------------- */

#define GMU_ITCM                GMU_WORD(0x1b400)
#define GMU_CORE_FW_VERSION     GMU_WORD(0x1d3f8)       /* the last words of the data memory: the loaded firmware's versions */

static bool
step_gmu_firmware(void)
{
	struct gmu_firmware_block block;
	const uint8_t *data;
	uint32_t wrong = 0, words = 0, version;
	uint64_t at;

	if (!gmu_ready() || !gmu_space_is_on) {
		return false;
	}
	announce("keep the GMU's own memories through a collapse, as Linux does before loading (cx_misc tcm_ret_cntl)");
	tcm_retained = true;
	put(&cx_misc, "tcm_ret_cntl", CX_MISC_TCM_RET_CNTL, 1);
	get(&cx_misc, "tcm_ret_cntl", CX_MISC_TCM_RET_CNTL);

	announce("load the GMU's firmware: what goes into its own code and data memory through its registers (0x3d6d000, "
	    "0x3d71000), the rest into the memory of step 11");
	if (get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET) != 1) {
		put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	}
	for (int pass = 0; pass < 2; pass++) {
		for (at = 0; gmu_firmware_next(&at, &block, &data);) {
			if (block.size == 0) {
				continue;
			}
			if (in_tcm(block.address, block.size)) {
				uint32_t registers = block.address < GMU_DTCM_ADDRESS ? GMU_ITCM + block.address - GMU_ITCM_ADDRESS :
				    GMU_DTCM + block.address - GMU_DTCM_ADDRESS;

				for (uint32_t offset = 0; offset + 4 <= block.size; offset += 4) {
					uint32_t word;

					memcpy(&word, data + offset, sizeof(word));
					if (pass == 0) {
						wr(gmu.base + registers + offset, word);
						words++;
					} else if (rd(gmu.base + registers + offset) != word) {
						wrong++;
					}
				}
			} else if (pass == 0) {
				int r = gmu_region(block.address, block.size);

				if (r < 0) {
					printf("sgpu: %u: no memory for the block at %#x\n", step, block.address);
					return false;
				}
				memcpy((void *)(gmu_regions[r].memory + (block.address - gmu_regions[r].address)), data, block.size);
				to_memory(gmu_regions[r].memory, gmu_regions[r].size);
			}
		}
		settle();
	}
	if (wrong) {
		printf("sgpu: %u: %u of %u words of the GMU's own memory do not read back as written\n", step, wrong, words);
		return false;
	}
	version = get(&gmu, "core_fw_version", GMU_CORE_FW_VERSION);
	printf("sgpu: %u: THE GMU'S FIRMWARE IS LOADED (%u words through its registers, read back right): version %u.%u.%u\n",
	    step, words, version >> 28, version >> 16 & 0xfff, version & 0xffff);
	return true;
}

/* ---- step 13: the GMU's channel to RPMh ---------------------------------------------------------------- */

/*
 * a6xx_gmu.c: a6xx_gmu_rpmh_init, for the A740 family (the A750 among it): the GMU's RSC (RSCC,
 * step 6) gets its handshake with the GPU's PDC and its sequencer's program for sleep and wakeup;
 * the PDC's own program is AOP's on the A7xx, and the PDC is only started. Linux does this once,
 * when the driver starts. dt: qcom,gmu "gmu_pdc" at 0xb280000 (Linux sm8650.dtsi agrees).
 */
static struct block pdc = { "pdc", 0x0b280000, 0x10000 };
#define PDC_GPU_ENABLE_PDC      (4 * 0x1140)
#define PDC_GPU_SEQ_START_ADDR  (4 * 0x1148)
#define RSCC_PDC_SEQ_START_ADDR (4 * 0x008)
#define RSCC_PDC_MATCH_VALUE_LO (4 * 0x009)
#define RSCC_PDC_MATCH_VALUE_HI (4 * 0x00a)
#define RSCC_HIDDEN_TCS_CMD0_ADDR (4 * 0x00d)
#define RSCC_HIDDEN_TCS_CMD0_DATA (4 * 0x00e)
#define RSCC_OVERRIDE_START_ADDR (4 * 0x100)
#define RSCC_ASLEEP             (1u << 16)      /* of rsc_status0: handed over to the always-on sequencer */

static bool
step_rpmh(void)
{
	static const uint32_t sequence[5] = { 0xeaaae5a0, 0xe1a1ebab, 0xa2e0a581, 0xecac82e2, 0x0020edad };

	if (!gmu_ready()) {
		return false;
	}
	announce("read the GPU's PDC (0xb280000) as AOP left it");
	get(&pdc, "gpu_enable_pdc", PDC_GPU_ENABLE_PDC);
	get(&pdc, "gpu_seq_start_addr", PDC_GPU_SEQ_START_ADDR);

	announce("program the GMU's RSC: its handshake with the PDC and its sequencer for sleep and wakeup");
	rscc_set_up = true;
	put(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0, 1u << 24);       /* "disable SDE clock gating" */
	put(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0, 1);
	put(&rscc, "hidden_tcs_cmd0_data", RSCC_HIDDEN_TCS_CMD0_DATA, 0);
	put(&rscc, "hidden_tcs_cmd0_addr", RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
	put(&rscc, "hidden_tcs_cmd1_data", RSCC_HIDDEN_TCS_CMD0_DATA + 8, 0);
	put(&rscc, "hidden_tcs_cmd1_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 8, 0);
	put(&rscc, "hidden_tcs_cmd2_data", RSCC_HIDDEN_TCS_CMD0_DATA + 16, 0x80000021);
	put(&rscc, "hidden_tcs_cmd2_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 16, 0);
	put(&rscc, "override_start_addr", RSCC_OVERRIDE_START_ADDR, 0);
	put(&rscc, "pdc_seq_start_addr", RSCC_PDC_SEQ_START_ADDR, 0x4520);
	put(&rscc, "pdc_match_value_lo", RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	put(&rscc, "pdc_match_value_hi", RSCC_PDC_MATCH_VALUE_HI, 0x4514);
	for (int i = 0; i < 5; i++) {
		put(&rscc, "seq_mem_drv0", RSCC_SEQ_MEM_0_DRV0_A7 + 4 * i, sequence[i]);
	}
	for (int i = 0; i < 5; i++) {
		if (get(&rscc, "seq_mem_drv0", RSCC_SEQ_MEM_0_DRV0_A7 + 4 * i) != sequence[i]) {
			printf("sgpu: %u: the RSC's sequencer does not keep its program\n", step);
			return false;
		}
	}

	announce("start the GPU's PDC (its program is AOP's on the A7xx)");
	put(&pdc, "gpu_seq_start_addr", PDC_GPU_SEQ_START_ADDR, 0);
	put(&pdc, "gpu_enable_pdc", PDC_GPU_ENABLE_PDC, 0x80000001);
	settle();
	get(&pdc, "gpu_enable_pdc", PDC_GPU_ENABLE_PDC);
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	printf("sgpu: %u: the GMU's channel to RPMh is set up\n", step);
	return true;
}

/* ---- step 14: start the GMU ---------------------------------------------------------------------------- */

#define GMU_ICACHE_CONFIG       GMU_WORD(0x1f400)
#define GMU_DCACHE_CONFIG       GMU_WORD(0x1f401)
#define GMU_SYS_BUS_CONFIG      GMU_WORD(0x1f40f)
#define GMU_CX_FAL_INTF         GMU_WORD(0x1f8f0)
#define GMU_CX_FALNEXT_INTF     GMU_WORD(0x1f8f1)
#define GMU_HFI_QTBL_INFO       GMU_WORD(0x1f984)
#define GMU_HFI_QTBL_ADDR       GMU_WORD(0x1f985)
#define GMU_HFI_CTRL_INIT       GMU_WORD(0x1f986)
#define GMU_GMU2HOST_INTR_CLR   GMU_WORD(0x1f991)
#define GMU_GMU2HOST_INTR_INFO  GMU_WORD(0x1f992)       /* bit 0: a message for us; bit 23: the GMU's processor faulted */
#define GMU_GENERAL_8           GMU_WORD(0x1f9cd)       /* A7xx: where the log is, and its pages - 1 */
#define GMU_GENERAL_9           GMU_WORD(0x1f9ce)       /* ... how far it was written */
#define GMU_GENERAL_10          GMU_WORD(0x1f9cf)       /* ... which GPU this is */
#define GMU_RSCC_CONTROL_REQ    GMU_WORD(0x23b07)
#define GMU_AHB_FENCE_RANGE_0   GMU_WORD(0x23b11)
#define GBIF_QSB_SIDE(n)        (4 * (0x03 + (n)))
#define GMU_CHIP                0x7090100       /* a7xx_catalog.c: the A750's gmu_chipid */
#define GMU_OLD_FIRMWARE        0x20010004      /* a6xx_gmu_start: a firmware no newer says 0xbabeface when up */
#define GMU_STARTED_MASK        0x1ff           /* ... newer: these bits of fw_init_result */
#define GMU_STARTED             0x100

/* The table at the head of the message page and the three queues after it, as the vendor's driver lays them out. */
struct hfi_queue {
	uint32_t status, start, type, words, message_words, unused[5], read, write;
};
struct hfi_table {
	uint32_t version, words, first_queue, queue_words, queues, active_queues;
	struct hfi_queue queue[17];
};
#define HFI_QUEUES              3               /* 0: ours to the GMU; 1: its answers; 2: its debugging messages */
#define HFI_QUEUE_WORDS         1024
#define HFI_QUEUE_AT(n)         (((uint32_t)sizeof(struct hfi_table) + 15) / 16 * 16 + 4096 * (n))

static volatile struct hfi_table *hfi;  /* the uncached view */

static void
gmu_post_mortem(void)
{
	const volatile uint32_t *log = (const volatile uint32_t *)gmu_log.memory;

	to_memory(gmu_log.memory, gmu_log.size);
	printf("sgpu: %u: the GMU's log, written up to %#x; its first words:\n", step, rd(gmu.base + GMU_GENERAL_9));
	for (uint32_t i = 0; i < 64; i += 8) {
		printf("sgpu: %u:   %x %x %x %x %x %x %x %x\n", step, log[i], log[i + 1], log[i + 2], log[i + 3], log[i + 4],
		    log[i + 5], log[i + 6], log[i + 7]);
	}
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "gmu2host_intr_info", GMU_GMU2HOST_INTR_INFO);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	get(&gbif, "halt", GBIF_HALT);
	get(&gbif, "halt_ack", GBIF_HALT_ACK);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	space_faulted(&gmu_space);
}

/*
 * What Linux writes before it lets the processor go, in its order (a6xx_gmu_fw_start for the A7xx,
 * with the vendor's GBIF halt lifted as gen7_gmu_register_config does), the message table laid out
 * first; then out of reset (a6xx_gmu_start).
 */
static bool
step_gmu_start(void)
{
	static const uint8_t priority[HFI_QUEUES] = { 10, 10, 40 };
	uint32_t result = 0, mask, started;

	if (!gmu_ready() || !gmu_space_is_on || !rscc_set_up) {
		return false;
	}
	hfi = (volatile struct hfi_table *)block_uncached(&gmu_hfi);
	if (hfi == NULL) {
		printf("sgpu: %u: the message page is not in one piece\n", step);
		return false;
	}
	hfi->version = 0;
	hfi->words = sizeof(struct hfi_table) / 4;
	hfi->first_queue = 6;
	hfi->queue_words = sizeof(struct hfi_queue) / 4;
	hfi->queues = hfi->active_queues = HFI_QUEUES;
	for (unsigned i = 0; i < HFI_QUEUES; i++) {
		hfi->queue[i].start = (uint32_t)gmu_hfi.address + HFI_QUEUE_AT(i);
		hfi->queue[i].type = i | (uint32_t)priority[i] << 8;
		hfi->queue[i].words = HFI_QUEUE_WORDS;
		hfi->queue[i].read = hfi->queue[i].write = 0;
		hfi->queue[i].status = 1;
	}
	settle();

	announce("tell the GMU where things are, by its registers: the message table, the log, which GPU this is; the bus "
	    "interface's settings; its caches");
	if (get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET) != 1) {
		printf("sgpu: %u: the GMU's processor is not held in reset: not configuring it while it runs\n", step);
		return false;
	}
	put(&gmu, "cx_falnext_intf", GMU_CX_FALNEXT_INTF, 1);
	put(&gmu, "cx_fal_intf", GMU_CX_FAL_INTF, 1);
	if (get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP) {   /* a6xx_rpmh_start */
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 2);
		if (!wait_for(&gmu, GMU_RSCC_CONTROL_ACK, 2, 2, 10) || !wait_for(&rscc, RSCC_SEQ_BUSY_DRV0, ~0u, 0, 10)) {
			printf("sgpu: %u: the GMU's RSC does not wake\n", step);
		}
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
	}
	put(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT, 0);
	put(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG, 2);
	put(&gmu, "hfi_qtbl_addr", GMU_HFI_QTBL_ADDR, (uint32_t)gmu_hfi.address);
	put(&gmu, "hfi_qtbl_info", GMU_HFI_QTBL_INFO, 1);
	put(&gmu, "ahb_fence_range_0", GMU_AHB_FENCE_RANGE_0, 1u << 31 | 0x32u << 18 | 0x8a0);
	put(&gmu, "cm3_cfg", GMU_CM3_CFG, 0x4052);
	put(&gmu, "general_10", GMU_GENERAL_10, GMU_CHIP);
	put(&gmu, "general_8", GMU_GENERAL_8, ((uint32_t)gmu_log.address & ~4095u) | (GMU_LOG_SIZE / 4096 - 1));
	for (int i = 0; i < 4; i++) {
		put(&gbif, "qsb_side", GBIF_QSB_SIDE(i), 0x00071620);
	}
	put(&gbif, "halt", GBIF_HALT, 0);
	get(&gbif, "halt_ack", GBIF_HALT_ACK);
	put(&gmu, "sys_bus_config", GMU_SYS_BUS_CONFIG, 1);
	put(&gmu, "icache_config", GMU_ICACHE_CONFIG, 1);
	put(&gmu, "dcache_config", GMU_DCACHE_CONFIG, 1);
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, 0xffffffff);

	announce("let the GMU's processor out of reset, and wait for its firmware to say it is up");
	if (rd(gmu.base + GMU_CORE_FW_VERSION) <= GMU_OLD_FIRMWARE) {
		mask = 0xffffffff;
		started = GMU_FW_RUNNING;
	} else {
		mask = GMU_STARTED_MASK;
		started = GMU_STARTED;
	}
	put(&gmu, "general_9", GMU_GENERAL_9, 0);
	gmu_runs = true;
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 0);
	for (int i = 0; i < 1000 && (result & mask) != started; i++) {
		sleep_us(100);
		result = rd(gmu.base + GMU_CM3_FW_INIT_RESULT);
	}
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "gmu2host_intr_info", GMU_GMU2HOST_INTR_INFO);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	if (space_faulted(&gmu_space) || (result & mask) != started) {
		printf("sgpu: %u: the GMU's firmware does not come up (%#x, waiting for %#x in %#x)\n", step, result, started, mask);
		gmu_post_mortem();
		return false;
	}
	printf("sgpu: %u: THE GMU'S FIRMWARE RUNS\n", step);
	return true;
}

/* ---- step 15: the GMU's message queues ---------------------------------------------------------------- */

static bool
step_hfi_start(void)
{
	if (!gmu_ready() || !gmu_runs) {
		return false;
	}
	announce("ask the GMU to take up its message queues (a6xx_gmu_hfi_start)");
	put(&gmu, "hfi_ctrl_init", GMU_HFI_CTRL_INIT, 1);
	wait_for(&gmu, GMU_HFI_CTRL_STATUS, 1, 1, 10);
	if (!(get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS) & 1)) {
		printf("sgpu: %u: the GMU does not take up its queues\n", step);
		gmu_post_mortem();
		return false;
	}
	for (unsigned i = 0; i < HFI_QUEUES; i++) {
		printf("sgpu: %u: queue %u: status %u, at %#x, read %u, write %u\n", step, i, hfi->queue[i].status,
		    hfi->queue[i].start, hfi->queue[i].read, hfi->queue[i].write);
	}
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	get(&gmu, "gmu2host_intr_info", GMU_GMU2HOST_INTR_INFO);
	space_faulted(&gmu_space);
	hfi_up = true;
	printf("sgpu: %u: THE GMU HAS TAKEN UP ITS MESSAGE QUEUES\n", step);
	return true;
}

/* ---- step 16: the first messages to the GMU ---------------------------------------------------------- */

/*
 * A message is words: the first says which (bits 7:0), how many words (15:8), a request or an
 * answer (19:16), and a number (31:20) that the answer repeats in its second word; the third word
 * of an answer is the GMU's verdict, 0 for done. On the A7xx a message takes a whole number of
 * four-word slots, the rest filled with 0xfafafafa (a6xx_hfi.c, a6xx_hfi_queue_write). Linux sends
 * no "init" and no version question to an A7xx GMU: its first message is the table of speeds.
 */
#define HFI_BW_TABLE            3
#define HFI_PERF_TABLE          4
#define HFI_START               10
#define HFI_CORE_FW_START       14
#define HFI_GX_BW_PERF_VOTE     30
#define HFI_PREPARE_SLUMBER     33
#define HFI_ERROR               100
#define HFI_ACK                 126             /* the GMU's answer (Linux: HFI_F2H_MSG_ACK) */
#define HFI_ANSWER              1               /* in bits 19:16 */
#define GMU_HOST2GMU_INTR_SET   GMU_WORD(0x1f994)
#define GMU_AO_AHB_FENCE_CTRL   GMU_WORD(0x23b10)
#define GMU_POWER_COUNTER_ENABLE GMU_WORD(0x1f840)

struct hfi_perf_table {
	uint32_t header, gpu_levels, gmu_levels;
	struct {
		uint32_t vote, acd, khz;
	} gpu[16];
	struct {
		uint32_t vote, khz;
	} gmu[4];
};
struct hfi_bw_table {
	uint32_t header, levels, cnoc_commands, ddr_commands, cnoc_wait, ddr_wait;
	uint32_t cnoc_address[6], cnoc_data[2][6], ddr_address[8], ddr_data[16][8];
};

static uint32_t hfi_number;

/* Everything waiting in a queue of the GMU's; true if the answer to the message numbered `number` was among it. */
static bool
hfi_receive(unsigned which, uint32_t number, uint32_t *verdict)
{
	volatile struct hfi_queue *queue = &hfi->queue[which];
	volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(which));
	bool answered = false;

	for (int guard = 0; guard < 64 && queue->read != queue->write; guard++) {
		uint32_t message[8] = { 0 }, at = queue->read % HFI_QUEUE_WORDS, words = data[at] >> 8 & 0xff;

		for (uint32_t i = 0; i < words; i++) {
			if (i < 8) {
				message[i] = data[at];
			}
			at = (at + 1) % HFI_QUEUE_WORDS;
		}
		queue->read = (at + 3) / 4 * 4 % HFI_QUEUE_WORDS;
		if (((message[0] & 0xff) == HFI_ACK || (message[0] >> 16 & 0xf) == HFI_ANSWER) && message[1] >> 20 == number) {
			*verdict = message[2];
			answered = true;
		} else if ((message[0] & 0xff) == HFI_ERROR) {
			printf("sgpu: %u: THE GMU REPORTS AN ERROR: code %u (%#x %#x)\n", step, message[1], message[2], message[3]);
		} else {
			printf("sgpu: %u: the GMU says (queue %u, %u words): %#x %#x %#x %#x %#x %#x %#x %#x\n", step, which, words,
			    message[0], message[1], message[2], message[3], message[4], message[5], message[6], message[7]);
		}
		if (words == 0) {
			break;
		}
	}
	return answered;
}

/* Send a message whose first word is yet to be made and wait for the GMU's verdict on it. */
static bool hfi_quiet;          /* (DCVS's votes, twenty a second: not said, nor paused for) */

static bool
hfi_send(const char *what, uint32_t id, uint32_t *message, uint32_t words)
{
	volatile struct hfi_queue *queue = &hfi->queue[0];
	volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(0));
	uint32_t at = queue->write % HFI_QUEUE_WORDS, number = ++hfi_number % 0xfff, verdict = 0, waiting = 0;
	bool answered = false;

	message[0] = id | words << 8 | number << 20;
	if (!hfi_quiet) {
		printf("sgpu: %u: about to send the GMU %s: message %u, %u words, number %u\n", step, what, id, words, number);
		hold();
	}
	for (uint32_t i = 0; i < (words + 3) / 4 * 4; i++) {
		data[at] = i < words ? message[i] : 0xfafafafa;
		at = (at + 1) % HFI_QUEUE_WORDS;
	}
	settle();
	queue->write = at;
	settle();
	wr(gmu.base + GMU_HOST2GMU_INTR_SET, 1);
	for (int i = 0; i < 10000 && !answered; i++) {
		sleep_us(100);
		waiting = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
		if (waiting & 1) {
			wr(gmu.base + GMU_GMU2HOST_INTR_CLR, 1);
			answered = hfi_receive(1, number, &verdict);
		}
		if (waiting & (1u << 23)) {     /* the GMU's processor faulted */
			break;
		}
	}
	hfi_receive(2, ~0u, &waiting);
	if (!answered || verdict != 0) {
		printf("sgpu: %u: the GMU %s (%#x) to %s; fw_init_result %#x, gmu2host_intr_info %#x\n", step,
		    answered ? "says no" : "does not answer", verdict, what, rd(gmu.base + GMU_CM3_FW_INIT_RESULT),
		    rd(gmu.base + GMU_GMU2HOST_INTR_INFO));
		gmu_post_mortem();
		return false;
	}
	if (!hfi_quiet) {
		printf("sgpu: %u: the GMU accepts %s\n", step, what);
	}
	return true;
}

/*
 * A vote for a level of a supply, as a6xx_gmu_rpmh_arc_votes_init makes it: the level RPMh has
 * at or above the one wanted, its place in RPMh's list for the rail, and the place of the level
 * the memories' rail must have with it (no "gmxc.lvl" here, step 1: so "mx.lvl").
 */
static bool
rail_vote(uint32_t level, const uint16_t *rail, unsigned rail_levels, const uint16_t *mx, unsigned mx_levels, uint32_t *vote)
{
	unsigned at, with = 0;

	for (at = 0; at < rail_levels && rail[at] < level; at++) {
	}
	if (at == rail_levels || mx_levels == 0) {
		return false;
	}
	while (with + 1 < mx_levels && mx[with] < rail[at]) {
		with++;
	}
	*vote = (uint32_t)rail[at] << 16 | with << 8 | at;
	return true;
}

/*
 * The 3D core's speeds and the level of "gfx.lvl" each needs: the phone's own (dt: qcom,gpu-pwrlevels,
 * the levels every bin of it has; Linux sm8650.dtsi has the same pairs and two more), off first. The
 * GMU's: dt qcom,gmu-freq-table (Linux the same).
 */
static const struct speed {
	uint32_t khz, level;
} gpu_speeds[] = {
	{ 0, 0 }, { 231000, 52 }, { 310000, 56 }, { 366000, 60 }, { 422000, 64 }, { 500000, 80 }, { 578000, 128 },
	{ 629000, 144 }, { 680000, 192 }, { 720000, 224 },
	/* (sgpu_speed=10 to 12: NOM, NOM_L1, TURBO. The phone's dt has them in every table but one, which stops at 720
	 * (sku-code 1); its fuses say speed bin 5. Fixed, no DCVS and no thermal limit: for measuring the GPU at speed) */
	{ 770000, 256 }, { 834000, 320 }, { 903000, 384 },
}, gmu_speeds[] = { { 0, 0 }, { 260000, 64 }, { 625000, 128 } };
#define GPU_SPEEDS              (sizeof(gpu_speeds) / sizeof(gpu_speeds[0]))
#define GMU_SPEEDS              (sizeof(gmu_speeds) / sizeof(gmu_speeds[0]))
static uint32_t speed_index = 1;        /* boot-arg sgpu_speed=N: gpu_speeds[N] (default: the lowest) */

/* a6xx_hfi_start for an A7xx, then a6xx_gmu_set_initial_freq. */
static bool
step_gmu_talk(void)
{
	static struct hfi_perf_table perf;
	static struct hfi_bw_table bw;
	uint32_t core_start[2] = { 0, 0 }, start[1] = { 0 }, vote[4] = { 0, 1, 0, 0 };
	uint16_t gfx[16], cx[16], mx[16], unused[16];
	unsigned gfx_levels, cx_levels, mx_levels, n;

	if (!gmu_ready() || !hfi_up) {
		return false;
	}
	announce("make the GMU's tables: speeds with the supply levels they need, bus votes");
	if (!rpmh_resource("gfx.lvl", gfx, &gfx_levels) || !rpmh_resource("cx.lvl", cx, &cx_levels) ||
	    !rpmh_resource("mx.lvl", mx, &mx_levels)) {
		printf("sgpu: %u: RPMh's list lacks one of the rails\n", step);
		return false;
	}
	perf.gpu_levels = GPU_SPEEDS;
	for (unsigned i = 0; i < GPU_SPEEDS; i++) {
		perf.gpu[i].khz = gpu_speeds[i].khz;
		perf.gpu[i].acd = 0xffffffff;           /* no adaptive clocking */
		if (!rail_vote(gpu_speeds[i].level, gfx, gfx_levels, mx, mx_levels, &perf.gpu[i].vote)) {
			printf("sgpu: %u: RPMh has no level %u for the 3D core's rail\n", step, gpu_speeds[i].level);
			return false;
		}
		printf("sgpu: %u: speed %u: %u MHz, vote %#x\n", step, i, perf.gpu[i].khz / 1000, perf.gpu[i].vote);
	}
	perf.gmu_levels = GMU_SPEEDS;
	for (unsigned i = 0; i < GMU_SPEEDS; i++) {
		perf.gmu[i].khz = gmu_speeds[i].khz;
		if (!rail_vote(gmu_speeds[i].level, cx, cx_levels, mx, mx_levels, &perf.gmu[i].vote)) {
			printf("sgpu: %u: RPMh has no level %u for CX\n", step, gmu_speeds[i].level);
			return false;
		}
	}
	printf("sgpu: %u: the GMU's: %#x, %#x at 260 MHz, %#x at 625 MHz\n", step, perf.gmu[0].vote, perf.gmu[1].vote,
	    perf.gmu[2].vote);

	/* a740_build_bw_table: one level, every vote "off"; the GMU only has to know the resources */
	bw.levels = 1;
	bw.ddr_commands = 3;
	bw.ddr_wait = 7;
	bw.ddr_address[0] = rpmh_resource("SH0", unused, &n);
	bw.ddr_address[1] = rpmh_resource("MC0", unused, &n);
	bw.ddr_address[2] = rpmh_resource("ACV", unused, &n);
	bw.ddr_data[0][0] = bw.ddr_data[0][1] = bw.ddr_data[0][2] = 0x40000000;
	bw.cnoc_commands = 1;
	bw.cnoc_wait = 1;
	bw.cnoc_address[0] = rpmh_resource("CN0", unused, &n);
	bw.cnoc_data[0][0] = 0x40000000;
	bw.cnoc_data[1][0] = 0x60000001;
	printf("sgpu: %u: RPMh's bus resources: SH0 %#x, MC0 %#x, ACV %#x, CN0 %#x\n", step, bw.ddr_address[0], bw.ddr_address[1],
	    bw.ddr_address[2], bw.cnoc_address[0]);
	if (bw.ddr_address[0] == 0 || bw.ddr_address[1] == 0 || bw.ddr_address[2] == 0 || bw.cnoc_address[0] == 0) {
		return false;
	}

	announce("send the GMU its tables, \"start the core firmware\" and \"start\" (Linux's order for an A7xx)");
	gmu_started = true;                     /* told to start: told to slumber at the end */
	if (!hfi_send("the table of speeds", HFI_PERF_TABLE, &perf.header, sizeof(perf) / 4) ||
	    !hfi_send("the table of bus votes", HFI_BW_TABLE, &bw.header, sizeof(bw) / 4) ||
	    !hfi_send("\"start the core firmware\"", HFI_CORE_FW_START, core_start, 2) ||
	    !hfi_send("\"start\"", HFI_START, start, 1)) {
		return false;
	}

	announce("tell the GMU the 3D core's speed wanted (a blocking vote: speed, bus level 0)");
	vote[2] = speed_index;
	printf("sgpu: %u: speed %u: %u MHz\n", step, speed_index, gpu_speeds[speed_index].khz / 1000);
	if (!hfi_send("the speed wanted", HFI_GX_BW_PERF_VOTE, vote, 4)) {
		return false;
	}
	if (space_faulted(&gmu_space)) {
		return false;
	}
	printf("sgpu: %u: THE GMU HAS ITS TABLES AND IS STARTED\n", step);
	return true;
}

/* ---- step 17: the 3D core powered ------------------------------------------------------------------------ */

/*
 * What Linux's a6xx_hw_init asks first: "keep the 3D core up for us" (a6xx_gmu_set_oob, GPU_SET:
 * bit 30 of host2gmu_intr_set; granted by bit 31 of gmu2host_intr_info). The GMU then votes for the
 * 3D core's rail in RPMh, switches its power on and starts its clock; sptprac_pwr_clk_status says
 * (A7xx: bit 0 the switch off, bit 1 the clock off). Only then are its registers read.
 */
static struct block gpu = { "gpu", 0x03d00000, 0x40000 };      /* dt: kgsl_3d0_reg_memory; a6xx.xml counts words */
#define GPU_WORD(n)             (4u * (n))
#define RBBM_CLOCK_CNTL         GPU_WORD(0x00ae)
#define RBBM_INT_0_STATUS       GPU_WORD(0x0201)
#define RBBM_STATUS             GPU_WORD(0x0210)
#define CP_RB_BASE              GPU_WORD(0x0800)
#define CP_SQE_INSTR_BASE       GPU_WORD(0x0830)
#define CP_ALWAYS_ON_COUNTER    GPU_WORD(0x0980)
#define GMU_CORE_REQUEST        (1u << 30)
#define GMU_CORE_GRANTED        (1u << 31)
#define GMU_CORE_RELEASE        (1u << 31)      /* of host2gmu_intr_set */
#define GX_OFF_BITS             3u              /* of sptprac_pwr_clk_status */

static bool
step_core_power(void)
{
	uint32_t granted = 0, before, counted;
	uint64_t start, nanos;

	if (!gmu_ready() || !gmu_started) {
		return false;
	}
	announce("ask the GMU to power the 3D core and keep it powered (\"GPU_SET\")");
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	core_asked = true;                      /* asked for, at least: to be given back */
	put(&gmu, "host2gmu_intr_set", GMU_HOST2GMU_INTR_SET, GMU_CORE_REQUEST);
	for (int i = 0; i < 1000 && !(granted & GMU_CORE_GRANTED); i++) {
		sleep_us(100);
		granted = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
	}
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, GMU_CORE_GRANTED);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	hfi_receive(2, ~0u, &before);
	if (!(granted & GMU_CORE_GRANTED) || (rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS) & GX_OFF_BITS) != 0) {
		printf("sgpu: %u: the GMU does not power the 3D core (gmu2host_intr_info %#x, fw_init_result %#x)\n", step,
		    granted, rd(gmu.base + GMU_CM3_FW_INIT_RESULT));
		gmu_post_mortem();
		return false;
	}
	printf("sgpu: %u: the GMU says the 3D core has power and its clock runs\n", step);

	announce("read registers of the 3D core itself (0x3d00000): the first time it has power");
	get(&gpu, "rbbm_status", RBBM_STATUS);
	get(&gpu, "rbbm_int_0_status", RBBM_INT_0_STATUS);
	get(&gpu, "rbbm_clock_cntl", RBBM_CLOCK_CNTL);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	get(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE);
	get(&gpu, "cp_always_on_counter", CP_ALWAYS_ON_COUNTER);
	start = mach_absolute_time();
	before = rd(gpu.base + CP_ALWAYS_ON_COUNTER);
	sleep_ms(10);
	counted = rd(gpu.base + CP_ALWAYS_ON_COUNTER) - before;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	counted = nanos ? (uint32_t)((uint64_t)counted * NSEC_PER_SEC / nanos) : 0;
	printf("sgpu: %u: the 3D core's always-on counter: %u Hz, %s\n", step, counted, crystal(counted));
	if (space_faulted(&gmu_space)) {
		return false;
	}
	printf("sgpu: %u: %s\n", step, counted > 18000000 && counted < 20500000 ? "THE 3D CORE HAS POWER AND ANSWERS" :
	    "the 3D core's registers can be read, but its counter does not count the crystal");
	core_is_on = true;
	return true;
}
/* ---- step 18: the 3D core's memory and its microcode -------------------------------------------------------- */

/*
 * The 3D core takes its orders as words in a ring of memory, which a small processor inside it
 * ("SQE", with microcode of its own: gen70900_sqe.fw, the file's first word not part of it) reads
 * up to where a register says we have written. On the A7xx that microcode runs three command
 * processors: BR (the one the ring feeds), BV (visibility) and LPAC. Like the GMU the core reaches
 * memory through the SMMU: streams 0 and 1 (dt: iommus of qcom,kgsl-3d0; Linux sm8650.dtsi the
 * same), both of which this sends through context 0, with tables of ours that hold the ring, the
 * microcode and a page the core writes back to. Stream 1 is LPAC's: on redwood a phone with
 * nothing set up for it reset the moment the command processor was started, as LPAC fetches its
 * part of the microcode at once.
 * The addresses are above the A750's GMEM range (UCHE_GMEM_RANGE, 16M to 19M, step 20): what the
 * core reaches there is its on-chip memory, not ours.
 */
#define CORE_STREAM             0
#define CORE_MATCH              0
#define LPAC_STREAM             1
#define LPAC_MATCH              2               /* (1 is the GMU's) */
#define PTE_CORE                (PTE_GMU | PTE_UNPRIVILEGED)
#define PTE_READ_ONLY           (1ull << 7)     /* Linux gives the core its microcode so (MSM_BO_GPU_READONLY) */
#define CORE_RING_ADDRESS       0x40000000u
#define CORE_RING_SIZE          0x8000u         /* Linux's (MSM_GPU_RINGBUFFER_SZ): 8192 words */
#define CORE_SQE_ADDRESS        0x40100000u
#define CORE_SQE_MOST           0x100000u
#define CORE_SCRATCH_ADDRESS    0x40200000u
#define SCRATCH_WORD            0x000           /* of the scratch page: what step 21 has the core write */
#define SCRATCH_RPTR            0x100           /* ... where the core says how far it has read the ring (BR) */
#define SCRATCH_BV_RPTR         0x108           /* ... and BV */
#define SMMU_SCTLR_SERVE_AFTER_FAULT (1u << 8)  /* Linux (qcom_adreno_smmu_write_sctlr): after a fault the core is not stalled */

static struct gpu_space core_space = { .name = "the 3D core", .context = 0, .control = SMMU_SCTLR_SERVE_AFTER_FAULT };
static struct gpu_block core_ring, core_sqe, core_scratch;
static bool lpac_stream_is_on;
#define RING_WORDS              (CORE_RING_SIZE / 4)
static uint32_t ring_at;                /* where the next word goes: the ring goes round */
static uint32_t ring_sent;              /* up to where the core has been told to read */

/* One more stream through a context that is on. */
static bool
space_stream(struct gpu_space *space, uint32_t match, uint32_t stream)
{
	put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
	put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream);
	if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream) ||
	    (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context) {
		printf("sgpu: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
		    stream, space->context);
		return false;
	}
	printf("sgpu: %u: stream %u goes through context %u too\n", step, stream, space->context);
	return true;
}

static void
put64(struct block *b, const char *name, uint32_t offset, uint64_t value)
{
	put(b, name, offset, (uint32_t)value);
	put(b, name, offset + 4, (uint32_t)(value >> 32));
}

static bool
step_core_space(void)
{
	const uint8_t *microcode;
	uint64_t size = 0;
	uint32_t first[2];

	if (!smmu_ready() || !core_is_on) {
		return false;
	}
	announce("make the 3D core's memory: its ring, its microcode (fw/gen70900_sqe.fw), a page for what it writes back");
	microcode = image_file("fw", "gen70900_sqe.fw", &size);
	if (microcode == NULL) {
		return false;
	}
	if (size < 8 || size - 4 > CORE_SQE_MOST) {
		printf("sgpu: %u: %llu bytes is not the size of such microcode\n", step, size);
		return false;
	}
	memcpy(first, microcode, sizeof(first));
	printf("sgpu: %u: the microcode's first words: %#x (not loaded), %#x\n", step, first[0], first[1]);
	if (!block_new(&core_space, &core_ring, CORE_RING_ADDRESS, CORE_RING_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_sqe, CORE_SQE_ADDRESS, (uint32_t)size - 4, PTE_CORE | PTE_READ_ONLY) ||
	    !block_new(&core_space, &core_scratch, CORE_SCRATCH_ADDRESS, 4096, PTE_CORE)) {
		return false;
	}
	memcpy((void *)core_sqe.memory, microcode + 4, size - 4);
	to_memory(core_sqe.memory, core_sqe.size);
	printf("sgpu: %u: %u page tables for the 3D core\n", step, core_space.table_count);

	announce("give the SMMU's context 0 these tables and send the 3D core's requests (streams 0 and 1) through it");
	if (!smmu_layout()) {
		return false;
	}
	core_space_is_on = true;        /* whatever of it has been written by the time something fails is taken back */
	if (!space_on(&core_space, CORE_MATCH, CORE_STREAM)) {
		return false;
	}
	lpac_stream_is_on = true;
	if (!space_stream(&core_space, LPAC_MATCH, LPAC_STREAM)) {
		return false;
	}
	get(&gpu, "rbbm_status", RBBM_STATUS);
	return !space_faulted(&core_space);
}

/* ---- step 19: the zap shader -------------------------------------------------------------------------------- */

/*
 * The 3D core comes out of reset in "secure" mode, and on a phone only TrustZone lets it out: it
 * does when handed the vendor's signed zap shader (image 13) to authenticate, and the core asks
 * the shader when told to leave secure mode (step 21). What Linux does for it (adreno_gpu.c:
 * zap_shader_load_mdt; mdt_loader.c: qcom_mdt_pas_init, qcom_mdt_load; qcom_scm.c), from a
 * single file, an .mbn: TrustZone is given the ELF header with its table of segments followed
 * by the segment of hashes and certificates (qcom_mdt_read_metadata), in memory of ours in one
 * piece, which it then protects while it works (so it is not touched again, and never freed);
 * then where the image will be, if it can go anywhere (relocatable); the image's segments are
 * put there; and TrustZone is asked to authenticate and run it. The place is the memory the
 * device tree reserves for it (the zap-shader node's memory-region): the phone's dt has
 * gpu_microcode_region@9b09a000, two pages (Linux sm8650.dtsi the same). The phone's file (12088
 * bytes) is one relocatable segment of 0x430 bytes and 0xf38 of hashes. Linux loads the shader after the command
 * processor is started, the vendor's driver before (gen7_rb_start: gen7_microcode_load);
 * as on redwood, the vendor's order is kept.
 * Our kernels come and go without the phone restarting, so a kernel leaves no zap shader behind
 * (gmu_stop()), and to be sure, the same request is made here first: TrustZone answers it with
 * success whether it had a shader or not. Should it refuse that and then refuse the header too,
 * that is taken to mean that it keeps one from before: it is used as it is, and nothing is
 * written. Having given up what it had, a refusal is a refusal.
 */
#define PAS_GPU                 13
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_VALUE_AND_BUFFER    (2 | 2u << 6)   /* two arguments: a number, and the address of memory it may read and write */
#define SCM_THREE_VALUES        3
#define ZAP_MEMORY              0x9b09a000ull   /* dt: reserved-memory gpu_microcode_region */
#define ZAP_MEMORY_SIZE         0x2000u
#define ELF_PT_LOAD             1
#define MDT_TYPE_MASK           (7u << 24)      /* of a segment's flags (qcom mdt_loader.h) */
#define MDT_TYPE_HASH           (2u << 24)
#define MDT_RELOCATABLE         (1u << 27)

struct elf32_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

/* What the .mbn holds, as read and checked once. */
static struct {
	const uint8_t *file;
	uint64_t size;
	uint32_t table, segments;       /* where the table of segments is in the file, and how many */
	uint32_t header_size, hash_offset, hash_size;
	uint32_t lowest, highest;       /* the image's addresses: the lowest, and the end of the highest, a page further */
	bool relocatable;
} zap;

static void
zap_segment(unsigned i, struct elf32_segment *segment)
{
	memcpy(segment, zap.file + zap.table + i * sizeof(*segment), sizeof(*segment));
}

static bool
zap_loadable(const struct elf32_segment *segment)
{
	return segment->type == ELF_PT_LOAD && (segment->flags & MDT_TYPE_MASK) != MDT_TYPE_HASH && segment->memory_size != 0;
}

/* A 32-bit ELF of the kind mdt_loader.c takes, its hashes in it, and an image that fits the reserved memory. */
static bool
zap_read(void)
{
	struct elf32_segment segment;
	uint16_t entry_size = 0, count = 0;
	uint32_t hash = 0;

	zap.file = image_file("fw", "gen70900_zap.mbn", &zap.size);
	if (zap.file == NULL) {
		return false;
	}
	if (zap.size >= 52) {
		memcpy(&zap.table, zap.file + 28, sizeof(zap.table));
		memcpy(&entry_size, zap.file + 42, sizeof(entry_size));
		memcpy(&count, zap.file + 44, sizeof(count));
	}
	zap.segments = count;
	if (zap.size < 52 || memcmp(zap.file, "\177ELF\1", 5) != 0 || entry_size != sizeof(segment) || count < 2 ||
	    (uint64_t)zap.table + count * sizeof(segment) > zap.size) {
		printf("sgpu: %u: not a 32-bit ELF with a table of segments (%02x %02x %02x %02x %02x, %u segments of %u bytes)\n",
		    step, zap.file[0], zap.file[1], zap.file[2], zap.file[3], zap.file[4], count, entry_size);
		return false;
	}
	zap.lowest = ~0u;
	zap.highest = 0;
	zap.relocatable = false;
	for (unsigned i = 0; i < count; i++) {
		zap_segment(i, &segment);
		printf("sgpu: %u: segment %u: type %u, flags %#x, at %#x, %#x bytes of %#x in the file from %#x\n", step, i,
		    segment.type, segment.flags, segment.paddr, segment.memory_size, segment.file_size, segment.offset);
		if (i > 0 && hash == 0 && (segment.flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
			hash = i;
		}
		if (!zap_loadable(&segment)) {
			continue;
		}
		if ((uint64_t)segment.offset + segment.file_size > zap.size || segment.file_size > segment.memory_size) {
			printf("sgpu: %u: segment %u is not all in the file: an image in several files is not taken here\n", step, i);
			return false;
		}
		zap.relocatable |= (segment.flags & MDT_RELOCATABLE) != 0;
		if (segment.paddr < zap.lowest) {
			zap.lowest = segment.paddr;
		}
		if (segment.paddr + segment.memory_size > zap.highest) {
			zap.highest = (segment.paddr + segment.memory_size + 4095) & ~4095u;
		}
	}
	zap_segment(0, &segment);
	zap.header_size = segment.file_size;
	if (segment.type == ELF_PT_LOAD || hash == 0 || zap.lowest >= zap.highest) {
		printf("sgpu: %u: not the layout of a signed image (first segment type %u, hashes in segment %u, nothing to load: %u)\n",
		    step, segment.type, hash, zap.lowest >= zap.highest);
		return false;
	}
	zap_segment(hash, &segment);
	zap.hash_size = segment.file_size;
	zap.hash_offset = zap.header_size + (uint64_t)zap.hash_size == zap.size ? zap.header_size : segment.offset;
	if ((uint64_t)zap.hash_offset + zap.hash_size > zap.size || zap.header_size > zap.size) {
		printf("sgpu: %u: the hashes are not in the file\n", step);
		return false;
	}
	if (zap.highest - zap.lowest > ZAP_MEMORY_SIZE ||
	    (!zap.relocatable && (zap.lowest < ZAP_MEMORY || zap.highest > ZAP_MEMORY + ZAP_MEMORY_SIZE))) {
		printf("sgpu: %u: the image (%#x..%#x, %s) does not fit the memory reserved for it (%#llx, %#x bytes)\n", step,
		    zap.lowest, zap.highest - 1, zap.relocatable ? "relocatable" : "at its own address", ZAP_MEMORY, ZAP_MEMORY_SIZE);
		return false;
	}
	printf("sgpu: %u: an image of %#x bytes, %s; its header %u bytes, its hashes %u from %#x\n", step,
	    zap.highest - zap.lowest, zap.relocatable ? "relocatable" : "at its own address", zap.header_size, zap.hash_size,
	    zap.hash_offset);
	return true;
}

/* The header and the hashes, one after the other, in memory of ours in one piece; where that is. */
static bool
zap_metadata(uint64_t *phys)
{
	uint32_t size = zap.header_size + zap.hash_size;
	vm_offset_t memory = 0;

	if (kmem_alloc(kernel_map, &memory, round_page(size), KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		printf("sgpu: %u: no memory\n", step);
		return false;
	}
	for (uint32_t offset = 0; offset < size; offset += 4096) {
		if (kvtophys(memory + offset) != kvtophys(memory) + offset) {
			printf("sgpu: %u: the %u bytes for TrustZone are not in one piece of memory\n", step, size);
			return false;
		}
	}
	memcpy((void *)memory, zap.file, zap.header_size);
	memcpy((void *)(memory + zap.header_size), zap.file + zap.hash_offset, zap.hash_size);
	to_memory(memory, size);                /* TrustZone reads memory, not this processor's cache */
	*phys = kvtophys(memory);
	return true;
}

/* TrustZone is to give the zap shader up, and its memory with it. */
static bool
zap_unload(void)
{
	uint64_t answer = ~0ull;

	return scm("to give up the zap shader", SCM_64 | SCM_SIP | SCM_PAS_SHUTDOWN, PAS_GPU, &answer) == 0 && answer == 0;
}

static bool
step_zap_load(void)
{
	struct elf32_segment segment;
	volatile uint8_t *memory;
	uint64_t metadata = 0, answer = ~0ull, base;
	bool given_up;

	if (!gmu_ready() || !core_is_on) {
		return false;
	}
	announce("find the zap shader, fw/gen70900_zap.mbn (the phone's own: signed with Xiaomi's key), and read its segments");
	if (!zap_read()) {
		return false;
	}

	announce("ask TrustZone to give up a zap shader that a kernel before this one may have left");
	given_up = zap_unload();
	printf("sgpu: %u: %s\n", step, given_up ? "TrustZone has given up what it had (it says so with none, too)" :
	    "TRUSTZONE DOES NOT GIVE IT UP: the next answer tells whether it keeps one");

	announce("hand TrustZone the zap shader's header and hashes, tell it where the image goes, put the image there, and "
	    "ask it to authenticate and run it");
	if (!zap_metadata(&metadata)) {
		return false;
	}
	if (scm_call("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER,
	    PAS_GPU, metadata, 0, &answer) != 0 || answer != 0) {
		if (!given_up) {
			printf("sgpu: %u: TrustZone does not take the signature either (%#llx): TAKEN TO MEAN THAT IT KEEPS A ZAP "
			    "SHADER FROM BEFORE, which is used as it is\n", step, answer);
			zap_is_loaded = true;
			return true;
		}
		printf("sgpu: %u: TrustZone does not take the zap shader's signature (%#llx)\n", step, answer);
		return false;
	}
	if (zap.relocatable && (scm_call("where the image will be", SCM_64 | SCM_SIP | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES,
	    PAS_GPU, ZAP_MEMORY, zap.highest - zap.lowest, &answer) != 0 || answer != 0)) {
		printf("sgpu: %u: TrustZone does not take the place (%#llx): nothing written there\n", step, answer);
		return false;
	}
	base = zap.relocatable ? zap.lowest : ZAP_MEMORY;
	/* (the memory is looked at only now: TrustZone has just said that it is not its own) */
	memory = (volatile uint8_t *)ml_io_map_wcomb(ZAP_MEMORY, ZAP_MEMORY_SIZE);
	printf("sgpu: %u: the reserved memory begins %02x %02x %02x %02x\n", step, memory[0], memory[1], memory[2], memory[3]);
	printf("sgpu: %u: about to write the image's %#x bytes at %#llx\n", step, zap.highest - zap.lowest, ZAP_MEMORY);
	hold();
	for (unsigned i = 0; i < zap.segments; i++) {
		zap_segment(i, &segment);
		if (!zap_loadable(&segment)) {
			continue;
		}
		for (uint32_t b = 0; b < segment.memory_size; b++) {
			memory[segment.paddr - base + b] = b < segment.file_size ? zap.file[segment.offset + b] : 0;
		}
	}
	settle();
	if (scm("to authenticate the zap shader and run it", SCM_64 | SCM_SIP | SCM_PAS_AUTH_AND_RESET, PAS_GPU, &answer) != 0 ||
	    answer != 0) {
		printf("sgpu: %u: TrustZone does not authenticate the zap shader (%#llx)\n", step, answer);
		return false;
	}
	printf("sgpu: %u: TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER\n", step);
	zap_is_loaded = true;
	return true;
}

/* ---- step 20: the 3D core's registers, its command processor started, its first commands ------------------- */

/*
 * What Linux's a6xx_gpu.c hw_init writes for an A750 before it starts the command processor, in
 * its order, less what only matters once the core draws: clock gating, the protection of
 * registers against user programs' commands, counters, interrupts (nothing here takes them),
 * UBWC's layout. Register words: a6xx.xml (A6XX-A7XX); values: a6xx_gpu.c and, for the A750,
 * a7xx_catalog.c (gmem 3M; UCHE's trap base, a6xx_gpu_init).
 */
#define RBBM_GBIF_CLIENT_QOS_CNTL GPU_WORD(0x0011)
#define RBBM_GBIF_HALT          GPU_WORD(0x0016)
#define RBBM_GBIF_HALT_ACK      GPU_WORD(0x0017)
#define RBBM_INTERFACE_HANG_INT_CNTL GPU_WORD(0x001f)
#define RBBM_PERFCTR_CNTL       GPU_WORD(0x0500)
#define RBBM_PERFCTR_GPU_BUSY_MASKED GPU_WORD(0x050b)
#define CP_RB_CNTL              GPU_WORD(0x0802)
#define CP_RB_RPTR_ADDR         GPU_WORD(0x0804)
#define CP_RB_RPTR              GPU_WORD(0x0806)
#define CP_RB_WPTR              GPU_WORD(0x0807)
#define CP_SQE_CNTL             GPU_WORD(0x0808)
#define CP_HW_FAULT             GPU_WORD(0x0821)
#define CP_INTERRUPT_STATUS     GPU_WORD(0x0823)
#define CP_PROTECT_STATUS       GPU_WORD(0x0824)
#define CP_APRIV_CNTL           GPU_WORD(0x0844)
#define CP_AHB_CNTL             GPU_WORD(0x098d)
#define CP_BV_RB_RPTR_ADDR      GPU_WORD(0x0a98)
#define CP_BV_APRIV_CNTL        GPU_WORD(0x0ad0)
#define CP_LPAC_APRIV_CNTL      GPU_WORD(0x0b31)
#define UCHE_WRITE_THRU_BASE    GPU_WORD(0x0e07)
#define UCHE_TRAP_BASE          GPU_WORD(0x0e09)
#define UCHE_GMEM_RANGE_MIN     GPU_WORD(0x0e0b)
#define UCHE_GMEM_RANGE_MAX     GPU_WORD(0x0e0d)
#define UCHE_CACHE_WAYS         GPU_WORD(0x0e17)
#define UCHE_CLIENT_PF          GPU_WORD(0x0e19)
#define UCHE_GBIF_GX_CONFIG     GPU_WORD(0x0e3a)
#define UCHE_CMDQ_CONFIG        GPU_WORD(0x0e3c)
#define RB_CMP_DBG_ECO_CNTL     GPU_WORD(0x8e28)
#define TPL1_DBG_ECO_CNTL1      GPU_WORD(0xb602)
#define RBBM_SECVID_TSB_TRUSTED_BASE GPU_WORD(0xf800)
#define RBBM_SECVID_TSB_TRUSTED_SIZE GPU_WORD(0xf802)
#define RBBM_SECVID_TSB_CNTL    GPU_WORD(0xf803)
#define UCHE_TRAP_ADDRESS       0x0001fffffffff000ull
#define GMEM_START              0x1000000u      /* the A740 family's: 16M */
#define GMEM_SIZE               0x300000u
#define APRIV_ICACHE            (1u << 0)       /* what the command processors fetch and write privileged */
#define APRIV_RBFETCH           (1u << 1)
#define APRIV_RBPRIVLEVEL       (1u << 2)
#define APRIV_RBRPWB            (1u << 3)
#define APRIV_CDREAD            (1u << 5)
#define APRIV_CDWRITE           (1u << 6)
#define APRIV_A7XX              (APRIV_ICACHE | APRIV_RBFETCH | APRIV_RBPRIVLEVEL | APRIV_RBRPWB)
#define CP_RB_CNTL_VALUE        (12u | 2u << 8) /* 2^12 pairs of words, blocks of 4; the read position copied to memory */
#define RBBM_STATUS_AHB_BUSY    (1u << 0)       /* of rbbm_status: our own register reads, not the core at work */

/* commands (Linux's adreno_pm4.xml); a packet is a header word and so many more */
#define CP_THREAD_CONTROL       0x17
#define CP_MEM_WRITE            0x3d
#define CP_ME_INIT              0x48
#define CP_SET_SECURE_MODE      0x66

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static void
ring(uint32_t word)
{
	((uint32_t *)core_ring.memory)[ring_at] = word;
	ring_at = (ring_at + 1) % RING_WORDS;
}

/* The header of a command with so many words after it (a "type 7" packet). */
static void
ring_command(uint32_t command, uint32_t words)
{
	ring(7u << 28 | words | odd_parity(words) << 15 | command << 16 | odd_parity(command) << 23);
}

/* What has been written to the ring since the last time: out to memory, and the core told how far to read. */
static void
ring_send(void)
{
	if (ring_at >= ring_sent) {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (ring_at - ring_sent));
	} else {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (RING_WORDS - ring_sent));
		to_memory(core_ring.memory, 4 * ring_at);
	}
	ring_sent = ring_at;
	wr(gpu.base + CP_RB_WPTR, ring_at);
}

/* How far the core says it has read the ring: in the scratch page, as Linux reads it on the A7xx. */
static uint32_t
ring_taken(void)
{
	to_memory(core_scratch.memory + SCRATCH_RPTR, 4);
	return *(volatile uint32_t *)(core_scratch.memory + SCRATCH_RPTR);
}

/* Let the 3D core have what is in the ring, and wait until it has taken all of it and rests. */
static bool
ring_run(const char *what)
{
	uint32_t taken = ~0u, status = ~0u;

	printf("sgpu: %u: about to have the 3D core %s: the ring up to word %u\n", step, what, ring_at);
	hold();
	ring_send();
	for (int i = 0; i < 20000 && ((taken != ring_at && rd(gpu.base + CP_RB_RPTR) != ring_at) ||
	    (status & ~RBBM_STATUS_AHB_BUSY)); i++) {
		sleep_us(100);
		taken = ring_taken();
		status = rd(gpu.base + RBBM_STATUS);
	}
	if ((taken == ring_at || rd(gpu.base + CP_RB_RPTR) == ring_at) && !(status & ~RBBM_STATUS_AHB_BUSY) &&
	    rd(gpu.base + CP_HW_FAULT) == 0 && !space_faulted(&core_space)) {
		printf("sgpu: %u: the 3D core has done it (ring read up to %u, by its register %u; status %#x)\n", step, taken,
		    rd(gpu.base + CP_RB_RPTR), status);
		return true;
	}
	printf("sgpu: %u: the 3D core does not get through: ring read up to %u (by its register %u) of %u, status %#x, "
	    "interrupts %#x, command processor: fault %#x, interrupts %#x, protection %#x\n", step, taken,
	    rd(gpu.base + CP_RB_RPTR), ring_at, status, rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT),
	    rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS));
	space_faulted(&core_space);
	return false;
}

static bool
step_core_start(void)
{
	uint32_t eco;

	if (!gmu_ready() || !core_is_on || !core_space_is_on) {
		return false;
	}
	announce("write the 3D core's registers as Linux does for an A750 before it starts the command processor: the first "
	    "writes to the core itself");
	put(&gbif, "halt", GBIF_HALT, 0);
	put(&gpu, "rbbm_gbif_halt", RBBM_GBIF_HALT, 0);
	put(&gpu, "rbbm_secvid_tsb_cntl", RBBM_SECVID_TSB_CNTL, 0);
	put64(&gpu, "rbbm_secvid_tsb_trusted_base", RBBM_SECVID_TSB_TRUSTED_BASE, 0);
	put(&gpu, "rbbm_secvid_tsb_trusted_size", RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
	put(&gpu, "rbbm_gbif_client_qos_cntl", RBBM_GBIF_CLIENT_QOS_CNTL, 0x2120212);
	put(&gpu, "uche_gbif_gx_config", UCHE_GBIF_GX_CONFIG, 0x10240e0);
	put(&gpu, "rbbm_perfctr_gpu_busy_masked", RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
	put64(&gpu, "uche_trap_base", UCHE_TRAP_BASE, UCHE_TRAP_ADDRESS);
	put64(&gpu, "uche_write_thru_base", UCHE_WRITE_THRU_BASE, UCHE_TRAP_ADDRESS);
	put64(&gpu, "uche_gmem_range_min", UCHE_GMEM_RANGE_MIN, GMEM_START);
	put64(&gpu, "uche_gmem_range_max", UCHE_GMEM_RANGE_MAX, GMEM_START + GMEM_SIZE - 1);
	put(&gpu, "uche_cache_ways", UCHE_CACHE_WAYS, 1u << 23);
	put(&gpu, "cp_ahb_cntl", CP_AHB_CNTL, 1);
	put(&gpu, "rbbm_perfctr_cntl", RBBM_PERFCTR_CNTL, 1);
	put(&gpu, "rbbm_interface_hang_int_cntl", RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0xcfffff);
	put(&gpu, "uche_client_pf", UCHE_CLIENT_PF, 1u << 7 | 1);
	put(&gpu, "uche_cmdq_config", UCHE_CMDQ_CONFIG, 6u << 16 | 6u << 12 | 9u << 8 | 1u << 3 | 1u << 2 | 2u);
	put(&gpu, "cp_apriv_cntl", CP_APRIV_CNTL, APRIV_A7XX | APRIV_CDREAD | APRIV_CDWRITE);
	put(&gpu, "cp_bv_apriv_cntl", CP_BV_APRIV_CNTL, APRIV_A7XX);
	put(&gpu, "cp_lpac_apriv_cntl", CP_LPAC_APRIV_CNTL, APRIV_A7XX);
	eco = get(&gpu, "rb_cmp_dbg_eco_cntl", RB_CMP_DBG_ECO_CNTL);
	put(&gpu, "rb_cmp_dbg_eco_cntl", RB_CMP_DBG_ECO_CNTL, eco | 1u << 19);         /* the A750's: no merged UFC requests */
	put(&gpu, "tpl1_dbg_eco_cntl1", TPL1_DBG_ECO_CNTL1, 0xc0700);
	put64(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE, CORE_SQE_ADDRESS);
	put64(&gpu, "cp_rb_base", CP_RB_BASE, CORE_RING_ADDRESS);
	put(&gpu, "cp_rb_cntl", CP_RB_CNTL, CP_RB_CNTL_VALUE);
	put(&gpu, "cp_rb_wptr", CP_RB_WPTR, 0);
	put64(&gpu, "cp_rb_rptr_addr", CP_RB_RPTR_ADDR, CORE_SCRATCH_ADDRESS + SCRATCH_RPTR);
	put64(&gpu, "cp_bv_rb_rptr_addr", CP_BV_RB_RPTR_ADDR, CORE_SCRATCH_ADDRESS + SCRATCH_BV_RPTR);
	ring_at = ring_sent = 0;
	get(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	get(&gpu, "cp_rb_cntl", CP_RB_CNTL);
	get(&gpu, "cp_apriv_cntl", CP_APRIV_CNTL);
	get(&gpu, "rbbm_status", RBBM_STATUS);

	announce("start the 3D core's command processor");
	core_runs = true;
	put(&gpu, "cp_sqe_cntl", CP_SQE_CNTL, 1);
	sleep_ms(20);
	printf("sgpu: %u: alive after it; status %#x, interrupts %#x, command processor: fault %#x, interrupts %#x, "
	    "protection %#x, ring read up to %u\n", step, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
	    rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS),
	    rd(gpu.base + CP_RB_RPTR));
	get(&smmu, "sgfsr", SMMU_SGFSR);
	if (space_faulted(&core_space)) {
		return false;
	}

	/*
	 * a7xx_cp_init as Linux had it before it gave the core a list of registers to restore after a
	 * power collapse: binning not concurrent; then "initialize" with several hardware contexts,
	 * error detection, the default reset state, no counters saved across preemption, the list
	 * "with the spinlock" -- at address 0, and not to be read (its last word 0).
	 */
	announce("give the 3D core its first commands: \"no concurrent binning\", \"initialize\"");
	ring_command(CP_THREAD_CONTROL, 1);
	ring(1u << 27);
	ring_command(CP_ME_INIT, 7);
	ring(1u << 0 | 1u << 1 | 1u << 3 | 1u << 6 | 1u << 8);
	ring(0x00000003);                       /* several hardware contexts */
	ring(0x20000000);                       /* error detection */
	ring(0x00000002);                       /* operation mode */
	ring(0);
	ring(0);
	ring(0);
	if (!ring_run("initialize its command processor")) {
		return false;
	}
	printf("sgpu: %u: THE 3D CORE TAKES COMMANDS\n", step);
	return true;
}

/* ---- step 21: out of secure mode; a word written on command ------------------------------------------------ */

static bool
step_core_commands(void)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + SCRATCH_WORD);

	if (!gmu_ready() || !core_runs || !zap_is_loaded) {
		return false;
	}
	announce("command the 3D core out of secure mode (it asks the zap shader), then to write a word into a page of ours");
	ring_command(CP_SET_SECURE_MODE, 1);
	ring(0);
	if (!ring_run("leave secure mode")) {
		return false;
	}
	ring_command(CP_MEM_WRITE, 3);
	ring(CORE_SCRATCH_ADDRESS + SCRATCH_WORD);
	ring(0);
	ring(0x600dc0de);
	if (!ring_run("write a word to memory")) {
		return false;
	}
	to_memory(core_scratch.memory + SCRATCH_WORD, 4);
	printf("sgpu: %u: the page reads %#x: %s\n", step, *word, *word == 0x600dc0de ?
	    "THE 3D CORE WRITES OUR MEMORY ON COMMAND" : "not what it was told to write");
	return *word == 0x600dc0de;
}

/* ---- step 22: a frame filled by the GPU, on the panel ------------------------------------------------------ */

/*
 * The simplest thing the 3D core draws: its 2D engine filling a rectangle with one colour (Mesa's
 * fd6_clear_surface / r2d_setup_common for the A7xx; the registers under Linux's a6xx.xml names),
 * three times for three bands, as redwood's step 22. What differs from the A6xx: the 2D engine's
 * output format is at 0xa9bf (0xacc0 on an A6xx), the texture unit is told it is a 2D blit
 * (TPL1_A2D_BLT_CNTL), the colour cache's place in GMEM has its own register (RB_CCU_CACHE_CNTL)
 * and the events that write a word when they are done are CP_EVENT_WRITE7's.
 * The picture is 32-bit pixels, the panel's size (PE_current_console; 1440x3200 if it says nothing),
 * in rows of a multiple of 64 bytes, in memory of ours; the framebuffer driver copies it to the panel.
 */
#define RB_CCU_CACHE_CNTL       0x88e5          /* register numbers: these are written by command */
#define RB_CCU_CNTL             0x8e07
#define GRAS_A2D_BLT_CNTL       0x8400
#define GRAS_A2D_DEST_TL        0x8405
#define GRAS_A2D_DEST_BR        0x8406
#define RB_A2D_BLT_CNTL         0x8c00
#define RB_A2D_PIXEL_CNTL       0x8c01
#define RB_A2D_DEST_BUFFER_INFO 0x8c17
#define RB_A2D_DEST_BUFFER_BASE 0x8c18
#define RB_A2D_DEST_BUFFER_PITCH 0x8c1a
#define RB_A2D_CLEAR_COLOR_DW0  0x8c2c
#define SP_A2D_OUTPUT_INFO      0xa9bf
#define TPL1_A2D_BLT_CNTL       0xb2d2
#define TPL1_A2D_TEX_2D         (1u << 29)
#define FORMAT_8888             0x30
#define BLIT_SOLID_8888         (1u << 7 | FORMAT_8888 << 8 | 0xfu << 20 | 0x10u << 24) /* one colour; all channels; 8-bit (redwood's) */
#define DEST_BGRA_LINEAR        (FORMAT_8888 | 1u << 10)        /* rows of pixels, blue first in memory */
#define CP_WAIT_FOR_IDLE        0x26
#define CP_BLIT                 0x2c
#define CP_EVENT_WRITE          0x46            /* CP_EVENT_WRITE7 */
#define EVENT_CCU_FLUSH_COLOR   0x1d
#define EVENT_CACHE_CLEAN       0x31
#define EVENT_WRITE_ENABLED     (1u << 27)      /* the event writes a word to an address when it is done */
#define CORE_TARGET_ADDRESS     0x41000000u
#define SCRATCH_FRAME           0x010           /* of the scratch page: what step 22's last event writes */
#define TARGET_MOST             (64u << 20)

extern void MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t row_bytes);

static struct gpu_block core_target;
static uint32_t target_width, target_height, target_row;

/* A register of the 3D core (by its word number) written by command, in the order of the other commands. */
static void
ring_register(uint32_t number, uint32_t value)
{
	ring(4u << 28 | 1 | odd_parity(1) << 7 | number << 8 | odd_parity(number) << 27);
	ring(value);
}

static void
ring_fill(uint32_t top, uint32_t bottom, uint32_t red, uint32_t green, uint32_t blue)
{
	ring_register(GRAS_A2D_DEST_TL, top << 16);
	ring_register(GRAS_A2D_DEST_BR, (target_width - 1) | (bottom - 1) << 16);
	ring_register(RB_A2D_CLEAR_COLOR_DW0, red);     /* a register for each part of the colour, 0 to 255 */
	ring_register(RB_A2D_CLEAR_COLOR_DW0 + 1, green);
	ring_register(RB_A2D_CLEAR_COLOR_DW0 + 2, blue);
	ring_register(RB_A2D_CLEAR_COLOR_DW0 + 3, 255);
	ring_register(RB_A2D_BLT_CNTL, BLIT_SOLID_8888);
	ring_register(GRAS_A2D_BLT_CNTL, BLIT_SOLID_8888);
	ring_register(SP_A2D_OUTPUT_INFO, 1 | FORMAT_8888 << 3 | 0xfu << 12);
	ring_register(TPL1_A2D_BLT_CNTL, TPL1_A2D_TEX_2D);
	ring_register(RB_A2D_PIXEL_CNTL, 0);
	ring_register(RB_A2D_DEST_BUFFER_INFO, DEST_BGRA_LINEAR);
	ring_register(RB_A2D_DEST_BUFFER_BASE, CORE_TARGET_ADDRESS);
	ring_register(RB_A2D_DEST_BUFFER_BASE + 1, 0);
	ring_register(RB_A2D_DEST_BUFFER_PITCH, target_row >> 6);
	ring_command(CP_WAIT_FOR_IDLE, 0);
	ring_command(CP_BLIT, 1);
	ring(3);                                /* "scale": the 2D engine's one operation */
}

/* The pixel at (x, y) of the picture, blue green red alpha. */
static const volatile uint8_t *
target_pixel(uint32_t x, uint32_t y)
{
	return (const volatile uint8_t *)(core_target.memory + (vm_offset_t)y * target_row + 4 * x);
}

static bool
target_pixel_is(const char *where, uint32_t x, uint32_t y, uint8_t red, uint8_t green, uint8_t blue)
{
	const volatile uint8_t *pixel = target_pixel(x, y);
	bool same = pixel[0] == blue && pixel[1] == green && pixel[2] == red;

	printf("sgpu: %u: the pixel %s (%u, %u) is %02x %02x %02x %02x (blue green red alpha; told: %02x %02x %02x ff)%s\n", step,
	    where, x, y, pixel[0], pixel[1], pixel[2], pixel[3], blue, green, red, same ? "" : ": NOT WHAT IT WAS TOLD");
	return same;
}

static bool
step_frame(void)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + SCRATCH_FRAME);
	uint32_t context = smmu_context(&core_space), third;
	PE_Video video = { 0 };
	bool right;

	if (!gmu_ready() || !core_runs || !zap_is_loaded) {
		return false;
	}
	if (PE_current_console(&video) == 0 && video.v_width >= 64 && video.v_height >= 64 && video.v_width <= 4096 &&
	    video.v_height <= 4096) {
		target_width = (uint32_t)video.v_width;
		target_height = (uint32_t)video.v_height;
	} else {
		target_width = 1440;
		target_height = 3200;
	}
	target_row = (4 * target_width + 63) & ~63u;
	printf("sgpu: %u: the panel, as the boot loader's framebuffer has it: %lux%lu, %lu bytes a row, %lu bits a pixel; "
	    "the picture: %ux%u, %u bytes a row\n", step, video.v_width, video.v_height, video.v_rowBytes, video.v_depth,
	    target_width, target_height, target_row);
	if (target_row * target_height > TARGET_MOST) {
		return false;
	}

	announce("make a picture's worth of memory for the 3D core and have its 2D engine fill three bands of colour");
	if (!block_new(&core_space, &core_target, CORE_TARGET_ADDRESS, target_row * target_height, PTE_CORE)) {
		return false;
	}
	/* the tables the SMMU has in use have grown: out to memory, and anything it may have kept of them forgotten */
	space_commit(&core_space);
	put(&smmu, "cb tlbiasid", context + SMMU_CB_TLBIASID, core_space.context);
	put(&smmu, "cb tlbsync", context + SMMU_CB_TLBSYNC, 1);
	if (!wait_for(&smmu, context + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		printf("sgpu: %u: the SMMU does not finish forgetting old translations\n", step);
		return false;
	}
	*word = 0;
	to_memory(core_scratch.memory + SCRATCH_FRAME, 4);

	ring_register(RB_CCU_CNTL, 0);          /* drawing straight into memory: no GMEM fast clear, nothing resolved */
	ring_register(RB_CCU_CACHE_CNTL, 0);    /* the colour and depth caches full size, at the start of GMEM */
	third = target_height / 3;
	ring_fill(0, third, 255, 128, 0);       /* orange */
	ring_fill(third, 2 * third, 255, 255, 255);
	ring_fill(2 * third, target_height, 0, 128, 255);
	/*
	 * What it has drawn out of its caches into memory: the colour cache flushed, then the cache in
	 * front of memory cleaned, which writes a word when it is done (Linux's a7xx_submit; Mesa's
	 * tu_emit_event_write for the A7xx).
	 */
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_CCU_FLUSH_COLOR);
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_CLEAN | EVENT_WRITE_ENABLED);
	ring(CORE_SCRATCH_ADDRESS + SCRATCH_FRAME);
	ring(0);
	ring(0x0d0e0d0e);
	ring_command(CP_WAIT_FOR_IDLE, 0);
	if (!ring_run("fill a picture")) {
		return false;
	}
	to_memory(core_scratch.memory + SCRATCH_FRAME, 4);
	printf("sgpu: %u: the word it writes when its caches are in memory: %#x (told: 0xd0e0d0e)\n", step, *word);
	to_memory(core_target.memory, core_target.size);
	right = target_pixel_is("at the top left", 0, 0, 255, 128, 0);
	right &= target_pixel_is("in the middle", target_width / 2, target_height / 2, 255, 255, 255);
	right &= target_pixel_is("at the bottom right", target_width - 1, target_height - 1, 0, 128, 255);
	if (!right) {
		printf("sgpu: %u: the picture is not what the 3D core was told to draw\n", step);
		return false;
	}
	MiniIOMobileFramebufferShow((const uint8_t *)core_target.memory, target_width, target_height, target_row);
	printf("sgpu: %u: A PICTURE DRAWN BY THE GPU IS ON THE PANEL: orange, white, blue from the top\n", step);
	return true;
}

/* ---- step 23: a triangle drawn by the 3D pipeline, with shaders of ours ------------------------------------ */

/*
 * What turnip (Mesa's Vulkan driver) programs an A750 with to draw: its start of a command buffer
 * (tu_init_hw, tu6_init_static_regs and the A750's "magic" registers of freedreno_devices.py), its
 * start of drawing straight into memory (tu6_sysmem_render_begin), and the state of its shader
 * blits (r3d_setup, r3d_common) with the two shaders below in place of its own: each register as
 * Mesa's a6xx.xml has it for the A7xx, written out by a script (number, value, name).
 *
 * The vertex shader has nothing to do: the vertex fetch puts each vertex's position into r0 and its
 * colour into r1, and those are its outputs, the colour as varyings 0 to 3. Positions are in pixels
 * already (no viewport transform, no clipping, as r3d's). The fragment shader interpolates the four
 * varyings into r1 (bary.f with the barycentrics the hardware puts into r0.x and r0.y) and that is
 * its colour. The instructions as Mesa's ir3 disassembler tests have them, 16 to a shader (the unit
 * of SP_xS_INSTR_SIZE).
 */
static const uint32_t draw_init[][2] = {
	{ 0x8e07, 0x00000068 },	/* RB_CCU_CNTL */
	{ 0xae03, 0x10000402 },	/* SP_CHICKEN_BITS (Mesa: the A750's) */
	{ 0xae08, 0x00400000 },	/* SP_CHICKEN_BITS_1 (Mesa: the A750's) */
	{ 0xae09, 0x00431800 },	/* SP_CHICKEN_BITS_2 (Mesa: the A750's) */
	{ 0xae0a, 0x00800000 },	/* SP_CHICKEN_BITS_3 (Mesa: the A750's) */
	{ 0xae6c, 0x00000000 },	/* SP_HLSQ_DBG_ECO_CNTL (Mesa: the A750's) */
	{ 0xae00, 0x10000000 },	/* SP_DBG_ECO_CNTL (Mesa: the A750's) */
	{ 0x9804, 0x00003f1f },	/* PC_MODE_CNTL (Mesa: the A750's) */
	{ 0x9e00, 0x00100000 },	/* PC_DBG_ECO_CNTL (Mesa: the A750's) */
	{ 0x9e24, 0x01585600 },	/* PC_UNKNOWN_9E24 (Mesa: the A750's) */
	{ 0xa600, 0x00008000 },	/* VFD_DBG_ECO_CNTL (Mesa: the A750's) */
	{ 0xae06, 0x00000000 },	/* SP_ISDB_CNTL (Mesa: the A750's) */
	{ 0xae6a, 0x00000000 },	/* SP_UNKNOWN_AE6A (Mesa: the A750's) */
	{ 0xae6b, 0x00000080 },	/* SP_HLSQ_TIMEOUT_THRESHOLD_DP (Mesa: the A750's) */
	{ 0xae73, 0x00000000 },	/* SP_HLSQ_DBG_ECO_CNTL_1 (Mesa: the A750's) */
	{ 0xab02, 0x00000000 },	/* SP_HLSQ_MODE_CNTL (Mesa: the A750's) */
	{ 0xab01, 0x00000000 },	/* SP_UNKNOWN_AB01 (Mesa: the A750's) */
	{ 0xab22, 0x00000000 },	/* SP_UNKNOWN_AB22 (Mesa: the A750's) */
	{ 0xb310, 0x00000000 },	/* SP_UNKNOWN_B310 (Mesa: the A750's) */
	{ 0x80a7, 0x00000000 },	/* GRAS_ROTATION_CNTL (Mesa: the A750's) */
	{ 0x8600, 0x00004800 },	/* GRAS_DBG_ECO_CNTL (Mesa: the A750's) */
	{ 0x8e01, 0x00000000 },	/* RB_RBP_CNTL (Mesa: the A750's) */
	{ 0x8899, 0x00000000 },	/* RB_LRZ_CNTL2 (Mesa: the A750's) */
	{ 0x8e06, 0x02082000 },	/* RB_CCU_DBG_ECO_CNTL (Mesa: the A750's) */
	{ 0x960a, 0x00000001 },	/* VPC_FLATSHADE_MODE_CNTL (Mesa: the A750's) */
	{ 0xaa02, 0x00000000 },	/* SP_PS_OUTPUT_CONST_CNTL (Mesa: the A750's) */
	{ 0xaa03, 0x00000000 },	/* SP_PS_OUTPUT_CONST_MASK (Mesa: the A750's) */
	{ 0xb600, 0x13100000 },	/* TPL1_DBG_ECO_CNTL (Mesa: the A750's) */
	{ 0x9600, 0x02000000 },	/* VPC_DBG_ECO_CNTL (Mesa: the A750's) */
	{ 0x8e04, 0x00000001 },	/* RB_DBG_ECO_CNTL (Mesa: the A750's) */
	{ 0xae04, 0x00000000 },	/* SP_NC_MODE_CNTL_2 */
	{ 0xae0f, 0x0000003f },	/* SP_PERFCTR_SHADER_MASK */
	{ 0xab20, 0x00000000 },	/* SP_GFX_USIZE */
	{ 0xb182, 0x00000000 },	/* TPL1_PS_ROTATION_CNTL */
	{ 0xa9a8, 0x00000000 },	/* SP_UNKNOWN_A9A8 */
	{ 0xab00, 0x00000005 },	/* SP_MODE_CNTL */
	{ 0xa009, 0x00000003 },	/* VFD_MODE_CNTL */
	{ 0x8811, 0x00000010 },	/* RB_MODE_CNTL */
	{ 0x8110, 0x00000002 },	/* GRAS_MODE_CNTL */
	{ 0x8818, 0x00000000 },	/* RB_UNKNOWN_8818 */
	{ 0x9236, 0x00000000 },	/* VPC_REPLACE_MODE_CNTL */
	{ 0x9300, 0x00000000 },	/* VPC_ROTATION_CNTL */
	{ 0x9306, 0x00000001 },	/* VPC_SO_OVERRIDE */
	{ 0xb183, 0x00000000 },	/* TPL1_PS_SWIZZLE_CNTL */
	{ 0x80af, 0x00000000 },	/* GRAS_SC_SCREEN_SCISSOR_CNTL */
	{ 0x9602, 0x00000000 },	/* VPC_LB_MODE_CNTL */
	{ 0x9e72, 0x00000000 },	/* PC_CONTEXT_SWITCH_GFX_PREEMPTION_MODE */
	{ 0xb309, 0x000000a2 },	/* TPL1_MODE_CNTL */
	{ 0xa9cb, 0x000000fc },	/* SP_REG_PROG_ID_3 */
	{ 0xa007, 0x00000000 },	/* VFD_RENDER_MODE */
	{ 0x8864, 0x00000000 },	/* RB_ALPHA_TEST_CNTL */
	{ 0x8e09, 0x00000007 },	/* RB_UNKNOWN_8E09 */
	{ 0x930a, 0x00000000 },	/* VPC_UNKNOWN_CNTL */
	{ 0x8c34, 0x00000000 },	/* RB_A2D_DEST_BUFFER_ARRAY_PITCH */
	{ 0x88e5, 0xc0000000 },	/* RB_CCU_CACHE_CNTL */
	{ 0x9308, 0x00020000 },	/* VPC_ATTR_BUF_GMEM_SIZE */
	{ 0x9309, 0x001e0000 },	/* VPC_ATTR_BUF_GMEM_BASE */
	{ 0x9b09, 0x00020000 },	/* PC_ATTR_BUF_GMEM_SIZE */
};
static const uint32_t draw_pass[][2] = {
	{ 0xb307, 0x00000000 },	/* TPL1_WINDOW_OFFSET */
	{ 0x8890, 0x00000000 },	/* RB_WINDOW_OFFSET */
	{ 0x88d4, 0x00000000 },	/* RB_RESOLVE_WINDOW_OFFSET */
	{ 0xab21, 0x00000000 },	/* SP_WINDOW_OFFSET */
	{ 0xb2d1, 0x00000000 },	/* TPL1_A2D_WINDOW_OFFSET */
	{ 0x80a1, 0x03000000 },	/* GRAS_SC_BIN_CNTL */
	{ 0x8800, 0x03000000 },	/* RB_CNTL */
	{ 0x88d3, 0x00000000 },	/* RB_RESOLVE_CNTL_3 */
	{ 0x8812, 0x000003ff },	/* RB_BUFFER_CNTL */
	{ 0x88e4, 0x00000000 },	/* RB_CLEAR_TARGET */
};
static const uint32_t draw_program[][2] = {
	{ 0xab1f, 0x01fffe9f },	/* SP_UPDATE_CNTL */
	{ 0xa827, 0x00000102 },	/* SP_VS_CONST_CONFIG */
	{ 0xa83f, 0x00000000 },	/* SP_HS_CONST_CONFIG */
	{ 0xa867, 0x00000000 },	/* SP_DS_CONST_CONFIG */
	{ 0xa898, 0x00000000 },	/* SP_GS_CONST_CONFIG */
	{ 0xab03, 0x00000102 },	/* SP_PS_CONST_CONFIG */
	{ 0xa823, 0x00000100 },	/* SP_VS_CONFIG */
	{ 0xa83b, 0x00000000 },	/* SP_HS_CONFIG */
	{ 0xa863, 0x00000000 },	/* SP_DS_CONFIG */
	{ 0xa894, 0x00000000 },	/* SP_GS_CONFIG */
	{ 0xab04, 0x00000100 },	/* SP_PS_CONFIG */
	{ 0xa800, 0x00100100 },	/* SP_VS_CNTL_0 (r0 and r1) */
	{ 0xa824, 0x00000001 },	/* SP_VS_INSTR_SIZE (16 instructions) */
	{ 0xa81b, 0x00000000 },	/* SP_VS_PROGRAM_COUNTER_OFFSET */
	{ 0xa81e, 0x00000000 },	/* SP_VS_PVT_MEM_PARAM */
	{ 0xa81f, 0x00000000 },	/* SP_VS_PVT_MEM_BASE */
	{ 0xa820, 0x00000000 },	/* SP_VS_PVT_MEM_BASE+1 */
	{ 0xa821, 0x00000000 },	/* SP_VS_PVT_MEM_SIZE */
	{ 0xa825, 0x00000000 },	/* SP_VS_PVT_MEM_STACK_OFFSET */
	{ 0xa82d, 0x00000000 },	/* SP_VS_VGS_CNTL */
	{ 0xa980, 0x81400100 },	/* SP_PS_CNTL_0 */
	{ 0xab05, 0x00000001 },	/* SP_PS_INSTR_SIZE */
	{ 0xa982, 0x00000000 },	/* SP_PS_PROGRAM_COUNTER_OFFSET */
	{ 0xa985, 0x00000000 },	/* SP_PS_PVT_MEM_PARAM */
	{ 0xa986, 0x00000000 },	/* SP_PS_PVT_MEM_BASE */
	{ 0xa987, 0x00000000 },	/* SP_PS_PVT_MEM_BASE+1 */
	{ 0xa988, 0x00000000 },	/* SP_PS_PVT_MEM_SIZE */
	{ 0xa9a9, 0x00000000 },	/* SP_PS_PVT_MEM_STACK_OFFSET */
	{ 0xaa01, 0x00000000 },	/* SP_PS_VGS_CNTL */
	{ 0x9b00, 0x00000000 },	/* PC_CNTL */
	{ 0x9109, 0x00000000 },	/* VPC_PC_CNTL */
	{ 0x9200, 0x00000000 },	/* VPC_VARYING_INTERP_MODE[0] */
	{ 0x9208, 0x00000000 },	/* VPC_VARYING_REPLACE_MODE[0] */
	{ 0xa803, 0x0f000f04 },	/* SP_VS_OUTPUT[0] */
	{ 0xa813, 0x00000400 },	/* SP_VS_VPC_DEST[0] */
	{ 0x9301, 0x00ff0408 },	/* VPC_VS_CNTL */
	{ 0x9101, 0x00ffff00 },	/* VPC_VS_CLIP_CULL_CNTL */
	{ 0x9311, 0x00ffff00 },	/* VPC_VS_CLIP_CULL_CNTL_V2 */
	{ 0x8001, 0x00000000 },	/* GRAS_CL_VS_CLIP_CULL_DISTANCE */
	{ 0x9b01, 0x00000008 },	/* PC_VS_CNTL */
	{ 0xa802, 0x00000002 },	/* SP_VS_OUTPUT_CNTL */
	{ 0x9104, 0x00ffffff },	/* VPC_VS_SIV_CNTL */
	{ 0x9314, 0x00ffffff },	/* VPC_VS_SIV_CNTL_V2 */
	{ 0x809b, 0x00000000 },	/* GRAS_SU_VS_SIV_CNTL */
	{ 0xa9aa, 0x00000000 },	/* SP_RENDER_CNTL */
	{ 0x9b07, 0x00000006 },	/* PC_STEREO_RENDERING_CNTL */
	{ 0xa008, 0x00000006 },	/* VFD_STEREO_RENDERING_CNTL */
	{ 0x910c, 0x00000006 },	/* VPC_STEREO_RENDERING_CNTL */
	{ 0x910b, 0x00000000 },	/* VPC_STEREO_RENDERING_VIEWMASK */
	{ 0xa000, 0x00000202 },	/* VFD_CNTL_0 */
	{ 0xa0d0, 0x0000000f },	/* VFD_DEST_CNTL[0] */
	{ 0xa0d1, 0x0000004f },	/* VFD_DEST_CNTL[1] */
	{ 0xa001, 0xfcfcfcfc },	/* VFD_CNTL_1 */
	{ 0xa002, 0x0000fcfc },	/* VFD_CNTL_2 */
	{ 0xa003, 0xfcfcfcfc },	/* VFD_CNTL_3 */
	{ 0xa004, 0x000000fc },	/* VFD_CNTL_4 */
	{ 0xa005, 0x0000fcfc },	/* VFD_CNTL_5 */
	{ 0xa006, 0x00000000 },	/* VFD_CNTL_6 */
	{ 0xa090, 0xc8200000 },	/* VFD_FETCH_INSTR[0].INSTR */
	{ 0xa091, 0x00000000 },	/* VFD_FETCH_INSTR[0].STEP_RATE */
	{ 0xa092, 0xc8200200 },	/* VFD_FETCH_INSTR[1].INSTR */
	{ 0xa093, 0x00000000 },	/* VFD_FETCH_INSTR[1].STEP_RATE */
	{ 0xa013, 0x00000020 },	/* VFD_VERTEX_BUFFER[0].STRIDE */
	{ 0xa00e, 0x00000000 },	/* VFD_INDEX_OFFSET */
	{ 0xa00f, 0x00000000 },	/* VFD_INSTANCE_START_OFFSET */
	{ 0x9806, 0x00000000 },	/* PC_PS_CNTL */
	{ 0x8110, 0x00000002 },	/* GRAS_MODE_CNTL */
	{ 0xa99e, 0x01ff7fc0 },	/* SP_PS_INITIAL_TEX_LOAD_CNTL */
	{ 0xa9c7, 0x00000007 },	/* SP_LB_PARAM_LIMIT */
	{ 0xa9c8, 0xfcfcfcfc },	/* SP_REG_PROG_ID_0 */
	{ 0xa9c9, 0xfcfcfc00 },	/* SP_REG_PROG_ID_1 */
	{ 0xa9ca, 0xfcfcfcfc },	/* SP_REG_PROG_ID_2 */
	{ 0xa9cb, 0x0000fcfc },	/* SP_REG_PROG_ID_3 */
	{ 0xa9ae, 0x00000302 },	/* SP_PS_CNTL_1 */
	{ 0xa9c6, 0x00000002 },	/* SP_PS_WAVE_CNTL */
	{ 0x8005, 0x00000001 },	/* GRAS_CL_INTERP_CNTL */
	{ 0x8809, 0x00000401 },	/* RB_INTERP_CNTL */
	{ 0x880a, 0x00000000 },	/* RB_PS_INPUT_CNTL */
	{ 0x8810, 0x00000000 },	/* RB_PS_SAMPLEFREQ_CNTL */
	{ 0x8101, 0x00000000 },	/* GRAS_LRZ_PS_INPUT_CNTL */
	{ 0x8109, 0x00000000 },	/* GRAS_LRZ_PS_SAMPLEFREQ_CNTL */
	{ 0x9212, 0xfffffff0 },	/* VPC_VARYING_LM_TRANSFER_CNTL[0] */
	{ 0x9213, 0xffffffff },	/* VPC_VARYING_LM_TRANSFER_CNTL[1] */
	{ 0x9214, 0xffffffff },	/* VPC_VARYING_LM_TRANSFER_CNTL[2] */
	{ 0x9215, 0xffffffff },	/* VPC_VARYING_LM_TRANSFER_CNTL[3] */
	{ 0x9304, 0xff01ff04 },	/* VPC_PS_CNTL */
	{ 0xa98c, 0xfcfcfc00 },	/* SP_PS_OUTPUT_CNTL */
	{ 0xa98e, 0x00000004 },	/* SP_PS_OUTPUT[0] */
	{ 0xa98b, 0x0000000f },	/* SP_PS_OUTPUT_MASK */
	{ 0xaa02, 0x00000000 },	/* SP_PS_OUTPUT_CONST_CNTL */
	{ 0xaa03, 0x00000000 },	/* SP_PS_OUTPUT_CONST_MASK */
	{ 0x880b, 0x00000000 },	/* RB_PS_OUTPUT_CNTL */
	{ 0x880d, 0x0000000f },	/* RB_PS_OUTPUT_MASK */
	{ 0x8000, 0x00000381 },	/* GRAS_CL_CNTL */
	{ 0x8090, 0x00000000 },	/* GRAS_SU_CNTL */
	{ 0x9108, 0x00000003 },	/* VPC_RAST_CNTL */
	{ 0x9809, 0x00000003 },	/* PC_DGEN_RAST_CNTL */
	{ 0x9307, 0x00000003 },	/* VPC_PS_RAST_CNTL */
	{ 0x9107, 0x00000000 },	/* VPC_RAST_STREAM_CNTL */
	{ 0x9317, 0x00000000 },	/* VPC_RAST_STREAM_CNTL_V2 */
	{ 0x8801, 0x00000000 },	/* RB_RENDER_CNTL */
	{ 0x8116, 0x00000000 },	/* GRAS_SU_RENDER_CNTL */
	{ 0x980a, 0x00000000 },	/* PC_DGEN_SU_CONSERVATIVE_RAS_CNTL */
	{ 0x8099, 0x00000000 },	/* GRAS_SU_CONSERVATIVE_RAS_CNTL */
	{ 0x80d0, 0x00000000 },	/* GRAS_SC_VIEWPORT_SCISSOR[0].TL */
	{ 0x80d1, 0x7fff7fff },	/* GRAS_SC_VIEWPORT_SCISSOR[0].BR */
	{ 0x80b0, 0x00000000 },	/* GRAS_SC_SCREEN_SCISSOR[0].TL */
	{ 0x80b1, 0x7fff7fff },	/* GRAS_SC_SCREEN_SCISSOR[0].BR */
	{ 0x8004, 0x00000000 },	/* GRAS_CL_ARRAY_SIZE */
	{ 0xb300, 0x00000000 },	/* TPL1_RAS_MSAA_CNTL */
	{ 0x80a2, 0x00000000 },	/* GRAS_SC_RAS_MSAA_CNTL */
	{ 0x8802, 0x00000000 },	/* RB_RAS_MSAA_CNTL */
	{ 0xb301, 0x00000004 },	/* TPL1_DEST_MSAA_CNTL */
	{ 0x80a3, 0x00000004 },	/* GRAS_SC_DEST_MSAA_CNTL */
	{ 0x8803, 0x00000004 },	/* RB_DEST_MSAA_CNTL */
	{ 0xa98d, 0x00000001 },	/* SP_PS_MRT_CNTL */
	{ 0x880c, 0x00000001 },	/* RB_PS_MRT_CNTL */
	{ 0xa989, 0x00000000 },	/* SP_BLEND_CNTL */
	{ 0x8865, 0xffff0000 },	/* RB_BLEND_CNTL */
	{ 0x8870, 0x00000000 },	/* RB_DEPTH_PLANE_CNTL */
	{ 0x8871, 0x00000000 },	/* RB_DEPTH_CNTL */
	{ 0x8114, 0x00000000 },	/* GRAS_SU_DEPTH_CNTL */
	{ 0x8094, 0x00000000 },	/* GRAS_SU_DEPTH_PLANE_CNTL */
	{ 0x8880, 0x00000000 },	/* RB_STENCIL_CNTL */
	{ 0x8115, 0x00000000 },	/* GRAS_SU_STENCIL_CNTL */
	{ 0x8888, 0x00000000 },	/* RB_STENCIL_MASK */
	{ 0x8889, 0x00000000 },	/* RB_STENCIL_WRITE_MASK */
	{ 0x8887, 0x00000000 },	/* RB_STENCIL_REF_CNTL */
	{ 0xa996, 0x00000030 },	/* SP_PS_MRT[0] */
	{ 0x8820, 0x00000780 },	/* RB_MRT[0].CONTROL */
	{ 0x8821, 0x00000000 },	/* RB_MRT[0].BLEND_CONTROL */
	{ 0x8822, 0x00002030 },	/* RB_MRT[0].BUF_INFO */
	{ 0x8824, 0x00000000 },	/* RB_MRT[0].ARRAY_PITCH */
	{ 0x8827, 0x00000000 },	/* RB_MRT[0].BASE_GMEM */
	{ 0x880f, 0x00000000 },	/* RB_SRGB_CNTL */
	{ 0xa98a, 0x00000000 },	/* SP_SRGB_CNTL */
	{ 0x8100, 0x00000000 },	/* GRAS_LRZ_CNTL */
	{ 0x8898, 0x00000000 },	/* RB_LRZ_CNTL */
	{ 0x810b, 0x00000000 },	/* GRAS_LRZ_CNTL2 */
	{ 0x8113, 0x00000000 },	/* GRAS_LRZ_DEPTH_BUFFER_INFO */
	{ 0x88f4, 0x00000000 },	/* RB_VRS_CONFIG */
	{ 0xa9ad, 0x00000000 },	/* SP_VRS_CONFIG */
	{ 0x80f4, 0x00000000 },	/* GRAS_VRS_CONFIG */
	{ 0x8102, 0x00000030 },	/* GRAS_LRZ_MRT_BUFFER_INFO_0 */
	{ 0x80a0, 0x00000002 },	/* GRAS_SC_CNTL */
	{ 0x8891, 0x00000001 },	/* RB_SAMPLE_COUNTER_CNTL */
	{ 0x880e, 0x00000000 },	/* RB_DITHER_CNTL */
	{ 0xa9ac, 0x00000000 },	/* SP_DITHER_CNTL */
};
#define GRAS_SC_WINDOW_SCISSOR_TL	0x80f0
#define GRAS_SC_WINDOW_SCISSOR_BR	0x80f1
#define GRAS_A2D_SCISSOR_TL	0x840a
#define GRAS_A2D_SCISSOR_BR	0x840b
#define SP_VS_BASE	0xa81c
#define SP_PS_BASE	0xa983
#define RB_MRT0_PITCH	0x8823
#define RB_MRT0_BASE	0x8825
#define VFD_VERTEX_BUFFER0_BASE	0xa010
#define VFD_VERTEX_BUFFER0_SIZE	0xa012

#define CP_SET_MODE             0x63
#define CP_SET_VISIBILITY_OVERRIDE 0x64
#define CP_SET_MARKER           0x65
#define CP_DRAW_INDX_OFFSET     0x38
#define MARKER_DIRECT_RENDER    1               /* RM6_DIRECT_RENDER: straight into memory, no bins */
#define DRAW_TRIANGLES_AUTO     (4u | 2u << 6)  /* DI_PT_TRILIST, indices 0, 1, 2 made up */
#define EVENT_CCU_INVALIDATE_DEPTH 0x18
#define EVENT_CCU_INVALIDATE_COLOR 0x19
#define EVENT_LRZ_CACHE_INVALIDATE 0x28
#define EVENT_CACHE_INVALIDATE  0x33
#define CORE_DRAW_ADDRESS       0x40300000u     /* the shaders and the vertices */
#define DRAW_VS                 0x000
#define DRAW_FS                 0x080
#define DRAW_VERTICES           0x100
#define SCRATCH_DRAW            0x020           /* of the scratch page: what step 23's last event writes */
#define FLOAT_ONE               0x3f800000u

static const uint32_t shader_vs[] = {
	0x00000000, 0x00000000,                 /* nop */
	0x00000000, 0x03000000,                 /* end */
};
static const uint32_t shader_fs[] = {
	0x00002000, 0x47308b04,                 /* (rpt3)bary.f (ei)r1.x, (r)0, r0.x: varyings 0 to 3 into r1 */
	0x00000000, 0x00000500,                 /* (rpt5)nop */
	0x00000000, 0x03000000,                 /* end */
};

static struct gpu_block core_draw;

static void
ring_registers(const uint32_t (*list)[2], uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		ring_register(list[i][0], list[i][1]);
	}
}

static void
ring_event(uint32_t event)
{
	ring_command(CP_EVENT_WRITE, 1);
	ring(event);
}

/* A whole number as a 32-bit float's bits, without the floating point unit the kernel does not use. */
static uint32_t
float_bits(uint32_t n)
{
	uint32_t top = 31;

	if (n == 0) {
		return 0;
	}
	while (!(n >> top)) {
		top--;
	}
	return (127 + top) << 23 | ((top > 23 ? n >> (top - 23) : n << (23 - top)) & 0x7fffff);
}

/* A vertex: its position in pixels (z 0, w 1) and its colour (red, green, blue each 0 or 1, alpha 1). */
static void
draw_vertex(uint32_t *vertex, uint32_t x, uint32_t y, uint32_t rgb)
{
	vertex[0] = float_bits(x);
	vertex[1] = float_bits(y);
	vertex[2] = 0;
	vertex[3] = FLOAT_ONE;
	vertex[4] = rgb & 4 ? FLOAT_ONE : 0;
	vertex[5] = rgb & 2 ? FLOAT_ONE : 0;
	vertex[6] = rgb & 1 ? FLOAT_ONE : 0;
	vertex[7] = FLOAT_ONE;
}

static bool
target_pixel_near(const char *where, uint32_t x, uint32_t y, uint8_t red, uint8_t green, uint8_t blue)
{
	const volatile uint8_t *pixel = target_pixel(x, y);
	bool near = pixel[0] + 12 >= blue && pixel[0] <= blue + 12 && pixel[1] + 12 >= green && pixel[1] <= green + 12 &&
	    pixel[2] + 12 >= red && pixel[2] <= red + 12;

	printf("sgpu: %u: the pixel %s (%u, %u) is %02x %02x %02x %02x (blue green red alpha; about %02x %02x %02x ff "
	    "expected)%s\n", step, where, x, y, pixel[0], pixel[1], pixel[2], pixel[3], blue, green, red,
	    near ? "" : ": NOT WHAT IT SHOULD BE");
	return near;
}

static bool
step_triangle(void)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + SCRATCH_DRAW);
	uint32_t context = smmu_context(&core_space), *vertices;
	uint32_t top_x, top_y, left_x, right_x, bottom_y, middle_x, middle_y;
	bool right;

	if (!gmu_ready() || !core_runs || core_target.memory == 0) {
		return false;
	}
	announce("make memory for two shaders of ours and three vertices; have the 3D pipeline draw a triangle with them");
	if (!block_new(&core_space, &core_draw, CORE_DRAW_ADDRESS, 4096, PTE_CORE)) {
		return false;
	}
	space_commit(&core_space);
	put(&smmu, "cb tlbiasid", context + SMMU_CB_TLBIASID, core_space.context);
	put(&smmu, "cb tlbsync", context + SMMU_CB_TLBSYNC, 1);
	if (!wait_for(&smmu, context + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		printf("sgpu: %u: the SMMU does not finish forgetting old translations\n", step);
		return false;
	}
	memcpy((void *)(core_draw.memory + DRAW_VS), shader_vs, sizeof(shader_vs));
	memcpy((void *)(core_draw.memory + DRAW_FS), shader_fs, sizeof(shader_fs));
	top_x = target_width / 2;
	top_y = target_height / 5;
	left_x = target_width / 10;
	right_x = target_width - target_width / 10;
	bottom_y = target_height * 7 / 10;
	vertices = (uint32_t *)(core_draw.memory + DRAW_VERTICES);
	draw_vertex(vertices, top_x, top_y, 4);                 /* red at the top */
	draw_vertex(vertices + 8, left_x, bottom_y, 2);         /* green at the bottom left */
	draw_vertex(vertices + 16, right_x, bottom_y, 1);       /* blue at the bottom right */
	to_memory(core_draw.memory, core_draw.size);
	*word = 0;
	to_memory(core_scratch.memory + SCRATCH_DRAW, 4);

	/* the picture of step 22 painted over dark grey by the 2D engine, out of the colour cache */
	ring_register(RB_CCU_CNTL, 0);
	ring_register(RB_CCU_CACHE_CNTL, 0);
	ring_fill(0, target_height, 32, 32, 32);
	ring_event(EVENT_CCU_FLUSH_COLOR);
	ring_command(CP_WAIT_FOR_IDLE, 0);

	/* tu_init_hw: caches forgotten (the instruction cache too: the shaders are new), the static state */
	ring_event(EVENT_CCU_INVALIDATE_COLOR);
	ring_event(EVENT_CCU_INVALIDATE_DEPTH);
	ring_event(EVENT_LRZ_CACHE_INVALIDATE);
	ring_event(EVENT_CACHE_INVALIDATE);
	ring_command(CP_WAIT_FOR_IDLE, 0);
	ring_registers(draw_init, sizeof(draw_init) / sizeof(draw_init[0]));
	ring_command(CP_WAIT_FOR_IDLE, 0);      /* RB_CCU_CNTL takes effect after one */

	/* tu6_sysmem_render_begin: the whole picture, in memory */
	ring_registers(draw_pass, sizeof(draw_pass) / sizeof(draw_pass[0]));
	ring_register(GRAS_SC_WINDOW_SCISSOR_TL, 0);
	ring_register(GRAS_SC_WINDOW_SCISSOR_BR, (target_width - 1) | (target_height - 1) << 16);
	ring_register(GRAS_A2D_SCISSOR_TL, 0);
	ring_register(GRAS_A2D_SCISSOR_BR, (target_width - 1) | (target_height - 1) << 16);
	ring_command(CP_SET_MARKER, 1);
	ring(MARKER_DIRECT_RENDER);
	ring_event(EVENT_CCU_INVALIDATE_COLOR);
	ring_event(EVENT_CCU_INVALIDATE_DEPTH);
	ring_command(CP_SET_VISIBILITY_OVERRIDE, 1);
	ring(1);
	ring_command(CP_SET_MODE, 1);
	ring(0);

	/* the program, the vertices, the target */
	ring_registers(draw_program, sizeof(draw_program) / sizeof(draw_program[0]));
	ring_register(SP_VS_BASE, CORE_DRAW_ADDRESS + DRAW_VS);
	ring_register(SP_VS_BASE + 1, 0);
	ring_register(SP_PS_BASE, CORE_DRAW_ADDRESS + DRAW_FS);
	ring_register(SP_PS_BASE + 1, 0);
	ring_register(VFD_VERTEX_BUFFER0_BASE, CORE_DRAW_ADDRESS + DRAW_VERTICES);
	ring_register(VFD_VERTEX_BUFFER0_BASE + 1, 0);
	ring_register(VFD_VERTEX_BUFFER0_SIZE, 3 * 32);
	ring_register(RB_MRT0_PITCH, target_row >> 6);
	ring_register(RB_MRT0_BASE, CORE_TARGET_ADDRESS);
	ring_register(RB_MRT0_BASE + 1, 0);

	ring_command(CP_DRAW_INDX_OFFSET, 3);
	ring(DRAW_TRIANGLES_AUTO);
	ring(1);                                /* one instance */
	ring(3);                                /* three vertices */

	ring_event(EVENT_CCU_FLUSH_COLOR);
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_CLEAN | EVENT_WRITE_ENABLED);
	ring(CORE_SCRATCH_ADDRESS + SCRATCH_DRAW);
	ring(0);
	ring(0x0d0e0d0e);
	ring_command(CP_WAIT_FOR_IDLE, 0);
	if (!ring_run("draw a triangle")) {
		return false;
	}
	to_memory(core_scratch.memory + SCRATCH_DRAW, 4);
	printf("sgpu: %u: the word it writes when its caches are in memory: %#x (told: 0xd0e0d0e)\n", step, *word);
	to_memory(core_target.memory, core_target.size);
	middle_x = (top_x + left_x + right_x) / 3;
	middle_y = (top_y + 2 * bottom_y) / 3;
	right = target_pixel_near("in a corner, outside the triangle", 2, 2, 32, 32, 32);
	right &= target_pixel_near("in the middle of the triangle (a third of each colour)", middle_x, middle_y, 85, 85, 85);
	right &= target_pixel_near("just under its top (red)", top_x, top_y + 4, 255, 0, 0);
	target_pixel_near("just inside its bottom left (green)", left_x + 8, bottom_y - 2, 0, 255, 0);
	target_pixel_near("just inside its bottom right (blue)", right_x - 8, bottom_y - 2, 0, 0, 255);
	MiniIOMobileFramebufferShow((const uint8_t *)core_target.memory, target_width, target_height, target_row);
	if (!right) {
		printf("sgpu: %u: the picture is not what the 3D pipeline was told to draw (it is on the panel as it is)\n", step);
		return false;
	}
	printf("sgpu: %u: A TRIANGLE DRAWN BY THE GPU'S 3D PIPELINE AND SHADERS OF OURS IS ON THE PANEL: red at the top, "
	    "green and blue below, the colours blended between\n", step);
	return true;
}

/*
 * The GPU block's bus told to let what is under way finish and to start nothing more, then let
 * go again (Linux's a6xx_bus_clear_pending_transactions, as a6xx_gmu_shutdown has it): a core
 * whose SMMU context is taken away under a request leaves the SMMU waiting for good (redwood).
 */
static void
core_bus_halt(void)
{
	wr(gbif.base + GBIF_HALT, 1);
	for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 1); i++) {
	}
	wr(gbif.base + GBIF_HALT, 2);
	for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 2); i++) {
	}
	wr(gbif.base + GBIF_HALT, 0);
}

/* The 3D core's power switch, should the GMU have left it on (Linux turns it off itself: a6xx_gmu_force_off). */
static void
gx_off(void)
{
	uint32_t gx = rd(gpucc.base + GPUCC_GX_GDSCR);

	if (gx & GDSC_POWER_ON) {
		wr(gpucc.base + GPUCC_GX_GDSCR, gx | GDSC_SW_COLLAPSE);
		wait_for(&gpucc, GPUCC_GX_GDSCR, GDSC_POWER_ON, 0, 100);
		printf("sgpu: the 3D core's switch was left on: turned off (%#x)\n", rd(gpucc.base + GPUCC_GX_GDSCR));
	}
}

/*
 * The 3D core given back, the GMU held in reset again, the zap shader given up, the GMU's RSC asleep,
 * the streams sent nowhere: before CX goes off (Linux: a6xx_gmu_shutdown).
 */
static void
gmu_stop(void)
{
	if (core_runs) {
		/* (what the core looks like when it is given back: a GMU that then does not answer may have its reasons here) */
		printf("sgpu: the 3D core before it is stopped: status %#x, interrupts %#x, ring read up to %u of %u\n",
		    rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_RB_RPTR), ring_at);
		core_bus_halt();
		printf("sgpu: the GPU block's bus halted and let go again (gbif halt_ack %#x)\n", rd(gbif.base + GBIF_HALT_ACK));
		core_runs = false;
	}
	if (core_asked) {               /* a6xx_gmu_clear_oob(GPU_SET) */
		wr(gmu.base + GMU_HOST2GMU_INTR_SET, GMU_CORE_RELEASE);
		sleep_ms(1);
		core_asked = core_is_on = false;
	}
	if (gmu_started) {              /* a6xx_gmu_notify_slumber for an A7xx */
		uint32_t slumber[3] = { 0, 0, 0 };

		wr(gmu.base + GMU_POWER_COUNTER_ENABLE, 0);
		printf("sgpu: telling the GMU to drop its votes (\"prepare slumber\")\n");
		hfi_send("\"prepare slumber\"", HFI_PREPARE_SLUMBER, slumber, 3);
		wr(gmu.base + GMU_AO_AHB_FENCE_CTRL, 0);
		gmu_started = false;
	}
	if (gmu_runs) {
		wr(gmu.base + GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
	}
	if (hfi_up) {
		hfi_up = false;
		gx_off();
	}
	if (zap_is_loaded) {            /* the next kernel loads its own: see step_zap_load() */
		printf("sgpu: the zap shader: %s\n", zap_unload() ? "TrustZone has given it up" :
		    "TRUSTZONE DOES NOT GIVE IT UP; the next kernel will find it there");
		zap_is_loaded = false;
	}
	if (rscc_set_up && !(rd(rscc.base + RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP)) {   /* a6xx_rpmh_stop */
		wr(gmu.base + GMU_RSCC_CONTROL_REQ, 1);
		wait_for(&rscc, RSCC_RSC_STATUS0_DRV0, RSCC_ASLEEP, RSCC_ASLEEP, 10);
		wr(gmu.base + GMU_RSCC_CONTROL_REQ, 0);
		printf("sgpu: the GMU's RSC: status %#x\n", rd(rscc.base + RSCC_RSC_STATUS0_DRV0));
	}
	if (lpac_stream_is_on) {
		wr(smmu.base + SMMU_SMR(LPAC_MATCH), 0);
		wr(smmu.base + SMMU_S2CR(LPAC_MATCH), SMMU_S2CR_FAULT);
		lpac_stream_is_on = false;
	}
	if (core_space_is_on) {
		space_faulted(&core_space);
		space_off(&core_space, CORE_MATCH);
		core_space_is_on = false;
	}
	if (gmu_space_is_on) {
		space_off(&gmu_space, GMU_MATCH);
		gmu_space_is_on = false;
	}
	if (tcm_retained) {             /* Linux leaves it on (a warm start next time); a run here starts cold */
		wr(cx_misc.base + CX_MISC_TCM_RET_CNTL, 0);
		tcm_retained = false;
	}
}

/*
 * Step 4's when it finds CX on: what a kernel before this one started and did not stop, stopped as
 * gmu_stop() would, nothing of it known here -- the bus halted, the GMU held in reset, the zap shader
 * given up, the 3D core's switch off, the GMU's RSC asleep, the SMMU's first three stream entries and
 * contexts sent nowhere, the GMU's memories not kept; then CX off. Registers behind a clock that is off
 * are not touched. True when CX has gone off.
 */
static bool
stale_stop(void)
{
	uint64_t answer;

	need(&gmu);
	need(&rscc);
	need(&gbif);
	need(&gpu);
	if (!(rd(gpucc.base + GPUCC_CX_GMU_CBCR) & CBCR_OFF)) {
		get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
		get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
		if (rd(gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON) {
			core_bus_halt();
		}
		put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
		answer = ~0ull;
		printf("sgpu: %u: a zap shader of that kernel: %s\n", step, scm("to give up the zap shader", SCM_64 | SCM_SIP |
		    SCM_PAS_SHUTDOWN, PAS_GPU, &answer) == 0 && answer == 0 ? "given up" : "none, or not given up");
		gx_off();
		if (rd(rscc.base + RSCC_SEQ_MEM_0_DRV0_A7) == 0xeaaae5a0 && !(rd(rscc.base + RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP)) {
			put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 1);
			wait_for(&rscc, RSCC_RSC_STATUS0_DRV0, RSCC_ASLEEP, RSCC_ASLEEP, 100);
			get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
			put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
		}
	} else {
		printf("sgpu: %u: the GMU's clock is off: its registers are left alone\n", step);
		gx_off();
	}
	if (!(rd(gpucc.base + GPUCC_HLOS1_VOTE_GPU_SMMU_CBCR) & CBCR_OFF) && smmu_layout()) {
		for (uint32_t i = 0; i < 3; i++) {
			put(&smmu, "smr", SMMU_SMR(i), 0);
			put(&smmu, "s2cr", SMMU_S2CR(i), SMMU_S2CR_FAULT);
		}
		put(&smmu, "cb0 sctlr", smmu_pages * smmu_page + SMMU_CB_SCTLR, 0);
		put(&smmu, "cb1 sctlr", (smmu_pages + 1) * smmu_page + SMMU_CB_SCTLR, 0);
	}
	put(&cx_misc, "tcm_ret_cntl", CX_MISC_TCM_RET_CNTL, 0);
	put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, (rd(gpucc.base + GPUCC_CX_GDSCR) & ~GDSC_RETAIN_FF) | GDSC_SW_COLLAPSE);
	for (int i = 0; i < 1000 && cx_is_on(); i++) {
		sleep_us(100);
	}
	get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_gds_hw_ctrl", GPUCC_CX_GDS_HW_CTRL);
	if (cx_is_on()) {
		printf("sgpu: %u: CX STAYS ON (someone else's vote?): a restart of the phone gets the GPU back\n", step);
		return false;
	}
	printf("sgpu: %u: what the earlier kernel left is off; going on from nothing\n", step);
	return true;
}

/* What steps 4 and 5 turned on, off again in reverse. */
static void
undo(void)
{
	if (!cx_started && clocks_started_count == 0 && pll_votes_set == 0 && rcgs_forced_count == 0 && !gmu_space_is_on &&
	    !core_space_is_on) {
		return;
	}
	step = 0;
	printf("sgpu: turning off again what the steps turned on\n");
	gmu_stop();
	for (unsigned i = clocks_started_count; i-- > 0;) {
		wr(gpucc.base + clocks_started[i].cbcr, rd(gpucc.base + clocks_started[i].cbcr) & ~CBCR_ENABLE);
	}
	for (unsigned i = 0; i < rcgs_forced_count; i++) {
		wr(gpucc.base + rcgs_forced[i], rd(gpucc.base + rcgs_forced[i]) & ~RCG_ROOT_EN);
	}
	if (pll_votes_set) {
		wr(gcc.base + GCC_PLL_VOTE, rd(gcc.base + GCC_PLL_VOTE) & ~pll_votes_set);
	}
	if (cx_started) {
		wr(gpucc.base + GPUCC_CX_GDSCR, (rd(gpucc.base + GPUCC_CX_GDSCR) & ~GDSC_RETAIN_FF) | GDSC_SW_COLLAPSE);
		delay_for_interval(100, NSEC_PER_USEC);
	}
	printf("sgpu: done: cx_gdscr %#x, cx_gds_hw_ctrl %#x, cx_gmu %#x, pll votes %#x\n", rd(gpucc.base + GPUCC_CX_GDSCR),
	    rd(gpucc.base + GPUCC_CX_GDS_HW_CTRL), rd(gpucc.base + GPUCC_CX_GMU_CBCR), rd(gcc.base + GCC_PLL_VOTE));
}

/* ---- the GPU kept on, for the system to draw with (boot-arg gpu=1) -------------------------------------- */

/*
 * With boot-arg gpu=1 on this phone the steps up to 21 are made without a pause and nothing is undone:
 * the 3D core stays powered and taking commands, and the service RedwoodGPU (iokit/Kernel/RedwoodGPU.cpp)
 * hands it to programs through the interface of Linux's msm driver, which Mesa's freedreno speaks -- as
 * redwood_adreno.c does for the POCO's GPU with the same boot-arg. RedwoodGPU calls redwood_gpu_...();
 * there, while this GPU is kept, each of those that touches the GPU comes here (shennong_gpu_...()). The
 * allocator of the 3D core's addresses is redwood_adreno.c's own (it touches nothing of the GPU's), given
 * the range below. The caller holds one lock around all of these.
 *
 * What differs from the A6xx: the command that ends a submission and writes its number is CP_EVENT_WRITE7's
 * CACHE_CLEAN with an address (Linux's a7xx_submit), which is what steps 22 and 23 end with; the core's
 * own command streams name the BR thread (CP_THREAD_CONTROL), as Linux's do; and the layout of compressed
 * pictures (UBWC) is set as Linux's a6xx_set_ubwc_config sets it for an A750, before the first client: Mesa
 * lays the pictures out by the same numbers, which are answered to it as parameters.
 */
#define GPU_CHIP_ID             0x43051401ull   /* Linux's a7xx_catalog.c; Mesa's freedreno_devices.py: FD750 */
#define GPU_ADDRESSES_START     0x50000000ull   /* above everything the steps use; below 4 GB, as Linux keeps them */
#define GPU_ADDRESSES_SIZE      0xb0000000ull
#define GPU_FENCE_AT            0x040           /* of the scratch page: the number of the last submission done */
#define GPU_PROGRESS_AT         0x080           /* ... how far BR's command processor has come: submission << 16 | buffer << 8 | 1 before it, 2 after */
#define GPU_TIMES_AT            0x400           /* ... and when the core began and ended each of its last submissions */
#define GPU_TIMES               64
#define GPU_HIGHEST_BANK_BIT    16              /* Linux's a6xx_calc_ubwc_config for the A740 family; Mesa's FD750 the same */
#define GPU_UBWC_SWIZZLE        6               /* levels 2 and 3 of the swizzle on, the mode 0 */
#define CP_REG_TO_MEM           0x3e
#define CP_INDIRECT_BUFFER      0x3f
#define CP_SCRATCH_REG2         0x0885
#define CP_IB1_BASE             GPU_WORD(0x0928)
#define CP_IB1_REM_SIZE         GPU_WORD(0x092a)
#define CP_IB2_BASE             GPU_WORD(0x092b)
#define CP_IB2_REM_SIZE         GPU_WORD(0x092d)
#define THREAD_SYNC             (1u << 31)      /* CP_THREAD_CONTROL: the threads wait for each other first */
#define THREAD_BR               1
#define THREAD_BOTH             3
#define RBBM_NC_MODE_CNTL       GPU_WORD(0x0534)
#define UCHE_MODE_CNTL          GPU_WORD(0x0e01)
#define GRAS_NC_MODE_CNTL       GPU_WORD(0x8602)
#define RB_NC_MODE_CNTL         GPU_WORD(0x8e08)
#define SP_NC_MODE_CNTL         GPU_WORD(0xae02)
#define TPL1_NC_MODE_CNTL       GPU_WORD(0xb604)

extern void RedwoodGPUStart(void);
extern void RedwoodGPUStop(void);
extern void redwood_gpu_addresses(uint64_t start, uint64_t size);
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern uint32_t gpu_va, gpu_log;        /* redwood_adreno.c's boot-args gpu_va and gpu_log, which RedwoodGPU reads too */

static bool gpu_kept;
static uint32_t gpu_fence;              /* the number of the last submission */
static uint32_t gpu_timed;              /* ... of the last whose times have been added up */
static uint64_t gpu_ticks;
static uint32_t gpu_timed_count;
static uint64_t dcvs_ticks;
static uint32_t gpu_timed_by[7];        /* how many under 0.1, 0.25, 0.5, 1, 2, 4 ms, and longer */
bool shennong_gpu_plain;                            /* the submission being given is hakios-metal's (RedwoodGPU.cpp) */
static uint32_t gpu_chicken = 1, gpu_chickened;   /* boot-arg gpu_chicken=0: see gpu_chicken_fix */
static uint32_t gpu_chicken_skip;                  /* boot-arg gpu_chicken_skip: see gpu_chicken_fix */
static uint32_t gpu_nop, gpu_nopped;      /* boot-arg gpu_nop: see gpu_chicken_fix */
static uint32_t gpu_wfi = 0;            /* boot-arg gpu_wfi=1: a wait for the pipeline after each command buffer */
static uint32_t gpu_exact = 1;          /* boot-arg gpu_exact=0: the core does not wait to be idle before it notes the time */
#define GPU_KEPT_SUBMISSIONS    8
#define GPU_KEPT_COMMANDS       16
static struct {
	uint32_t fence, count;
	uint64_t address[GPU_KEPT_COMMANDS];
	uint32_t words[GPU_KEPT_COMMANDS];
} gpu_sent[GPU_KEPT_SUBMISSIONS];       /* the command buffers of the last submissions, for a report of one that hangs */
#define GPU_DUMP_WORDS          4096    /* of one command buffer, at most, in such a report */
#define GPU_DUMP_ALL            24576   /* ... and of all of them */
#define GPU_DUMP_BUFFERS        96      /* ... in so many buffers at most */
#define CP_SET_DRAW_STATE       0x43
#define SP_VS_BASE              0xa81c
#define SP_PS_BASE              0xa983
#define CP_SDS_BASE             GPU_WORD(0x092e)
#define CP_SDS_REM_SIZE         GPU_WORD(0x0930)
#define CP_MRB_BASE             GPU_WORD(0x0931)
#define CP_MRB_REM_SIZE         GPU_WORD(0x0933)
#define CP_BV_RB_RPTR           GPU_WORD(0x0a66)
#define CP_BV_IB1_BASE          GPU_WORD(0x0a6d)
#define CP_BV_IB1_REM_SIZE      GPU_WORD(0x0a70)
#define CP_BV_IB2_BASE          GPU_WORD(0x0a71)
#define CP_BV_IB2_REM_SIZE      GPU_WORD(0x0a74)
static uint64_t gpu_dumped[GPU_DUMP_BUFFERS];
static uint32_t gpu_dumped_count, gpu_dumped_words;

extern vm_offset_t phystokv(uint64_t pa);

/*
 * a6xx_set_ubwc_config for an A750 (highest bank bit 16, so 3 above 13; the swizzle's levels 2 and 3 on;
 * AMSBC, the RGB565 predictor, UAV flag prefetch invalidation 2 and the macrotile mode of the A740 family).
 */
static void
gpu_ubwc(void)
{
	uint32_t hbb = GPU_HIGHEST_BANK_BIT - 13, hbb_hi = hbb >> 2, hbb_lo = hbb & 3;
	uint32_t mode = GPU_UBWC_SWIZZLE & 1, level2_off = !(GPU_UBWC_SWIZZLE & 2);

	put(&gpu, "rb_nc_mode_cntl", RB_NC_MODE_CNTL, level2_off << 12 | 1u << 11 | hbb_hi << 10 | 1u << 4 | hbb_lo << 1 | mode);
	put(&gpu, "tpl1_nc_mode_cntl", TPL1_NC_MODE_CNTL, level2_off << 6 | hbb_hi << 4 | hbb_lo << 1 | mode);
	put(&gpu, "sp_nc_mode_cntl", SP_NC_MODE_CNTL, level2_off << 12 | hbb_hi << 10 | 2u << 4 | hbb_lo << 1 | mode);
	put(&gpu, "gras_nc_mode_cntl", GRAS_NC_MODE_CNTL, hbb_lo << 5);
	put(&gpu, "uche_mode_cntl", UCHE_MODE_CNTL, hbb_lo << 21);
	put(&gpu, "rbbm_nc_mode_cntl", RBBM_NC_MODE_CNTL, 1);
}

/*
 * Before Mesa: commands of ours, each ending with a wait for the pipeline and a mark, to see whether the
 * pipeline comes back idle from the start of what Mesa does (true: the mark was written).
 */
static bool
gpu_selftest(uint32_t test, const char *what)
{
	volatile uint32_t *mark = (volatile uint32_t *)(core_scratch.memory + GPU_PROGRESS_AT);
	uint32_t waited;

	ring_command(CP_THREAD_CONTROL, 1);
	ring(THREAD_SYNC | THREAD_BR);
	switch (test) {
	case 2:
		ring_event(EVENT_CCU_INVALIDATE_COLOR);
		ring_event(EVENT_CCU_INVALIDATE_DEPTH);
		ring_event(EVENT_LRZ_CACHE_INVALIDATE);
		ring_event(EVENT_CACHE_INVALIDATE);
		break;
	case 3:
		ring_command(CP_SET_MODE, 1);
		ring(0);
		ring_command(CP_THREAD_CONTROL, 1);
		ring(0x08000001);                       /* BR, concurrent binning off: Mesa's */
		break;
	case 4:
		ring_register(0xab1f, 0x01fffeff);      /* SP_UPDATE_CNTL: Mesa's */
		break;
	case 5:
		ring_registers(draw_init, sizeof(draw_init) / sizeof(draw_init[0]));
		break;
	}
	ring_command(CP_WAIT_FOR_IDLE, 0);
	ring_command(CP_MEM_WRITE, 3);
	ring(CORE_SCRATCH_ADDRESS + GPU_PROGRESS_AT);
	ring(0);
	ring(0x5e1f0000 | test);
	ring_command(CP_THREAD_CONTROL, 1);
	ring(THREAD_BOTH);
	ring_send();
	for (waited = 0; waited < 1000; waited++) {
		to_memory((vm_offset_t)mark, 4);
		if (*mark == (0x5e1f0000 | test)) {
			break;
		}
		sleep_us(100);
	}
	printf("sgpu: gpu: self-test %u (%s): %s after %u us; status %#x\n", test, what,
	    *mark == (0x5e1f0000 | test) ? "idle" : "NOT IDLE", 100 * waited, rd(gpu.base + RBBM_STATUS));
	return *mark == (0x5e1f0000 | test);
}

/*
 * Mesa's first command buffer, cut short: its first 1, 2, 3... commands copied into the scratch page and run
 * there, each time followed by a wait for the pipeline and a mark, until the pipeline does not come back:
 * the last command copied is the one it stays busy after. boot-arg gpu_bisect=0: not done.
 */
static uint32_t gpu_word(uint64_t address);
static void gpu_word_forget(void);
static uint64_t space_phys(const struct gpu_space *space, uint64_t address);

#define GPU_BISECT_AT           0xc00           /* of the scratch page: the copy (256 words at most) */
static uint32_t gpu_bisect = 1;

static void
gpu_bisect_first(uint64_t address, uint32_t words)
{
	volatile uint32_t *copy = (volatile uint32_t *)(core_scratch.memory + GPU_BISECT_AT);
	volatile uint32_t *mark = (volatile uint32_t *)(core_scratch.memory + GPU_PROGRESS_AT);
	uint32_t at = 0, n = 0;

	printf("sgpu: gpu: bisect: Mesa's first command buffer %#llx, %u words, cut short\n", address, words);
	while (at < words && at < (4096 - GPU_BISECT_AT) / 4) {
		uint32_t header = gpu_word(address + 4 * at), length, waited;

		if (header >> 28 == 4) {
			length = 1 + (header & 0x7f);
		} else if (header >> 28 == 7) {
			length = 1 + (header & 0x3fff);
		} else {
			printf("sgpu: gpu: bisect: not a command at word %u: %08x\n", at, header);
			return;
		}
		if (at + length > (4096 - GPU_BISECT_AT) / 4) {
			return;
		}
		for (uint32_t i = 0; i < length; i++) {
			copy[at + i] = gpu_word(address + 4 * (at + i));
		}
		at += length;
		n++;
		to_memory((vm_offset_t)copy, 4 * at);
		ring_command(CP_THREAD_CONTROL, 1);
		ring(THREAD_SYNC | THREAD_BOTH);
		ring_command(CP_INDIRECT_BUFFER, 3);
		ring(CORE_SCRATCH_ADDRESS + GPU_BISECT_AT);
		ring(0);
		ring(at);
		ring_command(CP_THREAD_CONTROL, 1);
		ring(THREAD_SYNC | THREAD_BR);
		ring_command(CP_WAIT_FOR_IDLE, 0);
		ring_command(CP_MEM_WRITE, 3);
		ring(CORE_SCRATCH_ADDRESS + GPU_PROGRESS_AT);
		ring(0);
		ring(0xb15e0000 | n);
		ring_command(CP_THREAD_CONTROL, 1);
		ring(THREAD_BOTH);
		ring_send();
		for (waited = 0; waited < 500; waited++) {
			to_memory((vm_offset_t)mark, 4);
			if (*mark == (0xb15e0000 | n)) {
				break;
			}
			sleep_us(100);
		}
		printf("sgpu: gpu: bisect: commands 1 to %u (to word %u; the last: %08x %08x %08x %08x): %s; status %#x\n", n, at,
		    copy[at - length], length > 1 ? copy[at - length + 1] : 0, length > 2 ? copy[at - length + 2] : 0,
		    length > 3 ? copy[at - length + 3] : 0, *mark == (0xb15e0000 | n) ? "idle" : "NOT IDLE",
		    rd(gpu.base + RBBM_STATUS));
		if (*mark != (0xb15e0000 | n)) {
			return;
		}
	}
	printf("sgpu: gpu: bisect: the whole of it comes back idle\n");
}

/* After step 21: the GPU stays on, and the service is started. */
static void
gpu_keep(void)
{
	step = 0;
	if (gpu_selftest(1, "a wait, before the UBWC registers")) {
		gpu_ubwc();
		(void)(gpu_selftest(1, "a wait") && gpu_selftest(2, "Mesa's cache invalidations") &&
		    gpu_selftest(3, "Mesa's CP_SET_MODE and CP_THREAD_CONTROL") && gpu_selftest(4, "Mesa's SP_UPDATE_CNTL") &&
		    gpu_selftest(5, "turnip's state of step 23"));
	} else {
		gpu_ubwc();
	}
	bzero((void *)(core_scratch.memory + GPU_FENCE_AT), 4);        /* (not the rest: the core keeps its read position there) */
	bzero((void *)(core_scratch.memory + GPU_TIMES_AT), 16 * GPU_TIMES);
	to_memory(core_scratch.memory, 4096);
	PE_parse_boot_argn("gpu_exact", &gpu_exact, sizeof(gpu_exact));
	PE_parse_boot_argn("gpu_chicken", &gpu_chicken, sizeof(gpu_chicken));
	PE_parse_boot_argn("gpu_chicken_skip", &gpu_chicken_skip, sizeof(gpu_chicken_skip));
	PE_parse_boot_argn("gpu_wfi", &gpu_wfi, sizeof(gpu_wfi));
	PE_parse_boot_argn("gpu_nop", &gpu_nop, sizeof(gpu_nop));
	PE_parse_boot_argn("gpu_bisect", &gpu_bisect, sizeof(gpu_bisect));
	PE_parse_boot_argn("gpu_log", &gpu_log, sizeof(gpu_log));
	/* before any client exists: the size of an allocation must not change under a live buffer */
	PE_parse_boot_argn("gpu_va", &gpu_va, sizeof(gpu_va));
	redwood_gpu_addresses(GPU_ADDRESSES_START, GPU_ADDRESSES_SIZE);
	gpu_kept = true;
	printf("sgpu: THE GPU STAYS ON for the system to draw with: the service RedwoodGPU (Adreno 750 at %u MHz)\n",
	    gpu_speeds[speed_index].khz / 1000);
	RedwoodGPUStart();
}

bool
shennong_gpu_ready(void)
{
	return gpu_kept;
}

/* The run is about to end in order (redwood_adreno_stop()): what the core was given it finishes, then everything is undone. */
void
shennong_gpu_stop(void)
{
	uint32_t waited = 0;

	if (!gpu_kept) {
		return;
	}
	gpu_kept = false;
	while (!shennong_gpu_done(gpu_fence) && waited < 5000) {
		sleep_us(100);
		waited++;
	}
	printf("sgpu: the run is about to end: the GPU is given back (%s after %u us)\n",
	    shennong_gpu_done(gpu_fence) ? "it had finished what it was given" : "STILL AT WORK", 100 * waited);
	undo();
}

/* The answers to msm's GET_PARAM (Linux's adreno_get_param for an A750; the numbers are msm_drm.h's). */
bool
shennong_gpu_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01:                              /* GPU_ID: none, the chip ID says it */
	case 0x06:                              /* GMEM_BASE: 0 from the A650 on */
	case 0x08:                              /* PP_PGTABLE */
	case 0x09:                              /* FAULTS */
	case 0x0a:                              /* SUSPENDS */
	case 0x11:                              /* RAYTRACING: not offered */
		*value = 0;
		return true;
	case 0x02:                              /* GMEM_SIZE */
		*value = GMEM_SIZE;
		return true;
	case 0x03:                              /* CHIP_ID, with the speed bin above it */
		*value = GPU_CHIP_ID | (uint64_t)speed_bin << 32;
		return true;
	case 0x04:                              /* MAX_FREQ */
		*value = gpu_speeds[speed_index].khz * 1000ull;
		return true;
	case 0x05:                              /* TIMESTAMP */
		*value = mach_absolute_time();
		return true;
	case 0x07:                              /* PRIORITIES */
		*value = 1;
		return true;
	case 0x10:                              /* HIGHEST_BANK_BIT */
		*value = GPU_HIGHEST_BANK_BIT;
		return true;
	case 0x12:                              /* UBWC_SWIZZLE */
		*value = GPU_UBWC_SWIZZLE;
		return true;
	case 0x13:                              /* MACROTILE_MODE */
		*value = 1;
		return true;
	case 0x14:                              /* UCHE_TRAP_BASE */
		*value = UCHE_TRAP_ADDRESS;
		return true;
	case 0x0e:                              /* VA_START */
	case 0x0f:                              /* VA_SIZE: as redwood_adreno.c answers them (its gpu_va) */
		if (gpu_va == 0) {
			return false;
		}
		*value = which == 0x0e ? GPU_ADDRESSES_START : GPU_ADDRESSES_SIZE;
		return true;
	default:
		return false;
	}
}

/* One 4K page of ours at an address of the 3D core's; after the last of a buffer's, shennong_gpu_mapped(). */
bool
shennong_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return space_map(&core_space, address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}

/* The SMMU told to forget what it knows of the 3D core's context. */
static void
gpu_forget(void)
{
	uint32_t context = smmu_context(&core_space);

	wr(smmu.base + context + SMMU_CB_TLBIASID, core_space.context);
	wr(smmu.base + context + SMMU_CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(smmu.base + context + SMMU_CB_TLBSTATUS) & 1); i++) {
	}
}

void
shennong_gpu_mapped(void)
{
	space_commit(&core_space);
	gpu_forget();                           /* (step 22 had to: an address seen unmapped may be remembered so) */
}

void
shennong_gpu_unmap(uint64_t address, uint64_t size)
{
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		space_map(&core_space, address + offset, 0, 0);
	}
	space_commit(&core_space);
	gpu_forget();
}

/* How far the 3D core has got. */
bool
shennong_gpu_done(uint32_t fence)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);

	to_memory((vm_offset_t)word, 4);
	return (int32_t)(*word - fence) >= 0;
}

uint32_t
shennong_gpu_last(void)
{
	return gpu_fence;
}

/*
 * Mesa 25.0.7's gallium driver sets some of the A750's registers otherwise than turnip (and newer Mesa) do,
 * and turnip's values are those of step 23, whose triangle is drawn. A trial: those registers are given
 * turnip's values in the command buffers Mesa gives (and those they call) before they go.
 * boot-arg gpu_chicken=0: left as they are. gpu_chicken_skip=MASK leaves some of them as Mesa has them: bit N,
 * entry N of the table below (2, RB_CCU_CNTL: turnip's 0x68 turns on concurrent resolves, which 25.0.7 does
 * not wait for); bit 4, CACHE_FLUSH_TS.
 */
static const struct {
	uint32_t reg, value;
	const char *name;
} gpu_turnip[] = {
	{ 0xae08, 0x00400000, "SP_CHICKEN_BITS_1" },    /* 25.0.7: 0 */
	{ 0x8e07, 0x00000068, "RB_CCU_CNTL" },          /* 25.0.7: 0x1 */
	{ 0xa009, 0x00000003, "VFD_MODE_CNTL" },        /* 25.0.7: 0x1 */
	{ 0x8e09, 0x00000007, "RB_UNKNOWN_8E09" },      /* 25.0.7: 0x4 */
};

/* A word of the core's memory written by us (true: it was there to write). */
static bool
gpu_put(uint64_t address, uint32_t value)
{
	uint64_t phys = space_phys(&core_space, address);
	volatile uint32_t *word = (volatile uint32_t *)(phys != 0 ? phystokv(phys) : 0);

	if (word == NULL) {
		return false;
	}
	*word = value;
	to_memory((vm_offset_t)word, 4);
	return true;
}

/*
 * boot-arg gpu_nop: to find what in Mesa's command buffers leaves the pipeline busy, commands of theirs
 * become CP_NOPs of the same length: 1, CP_SET_AMBLE; 2, the calls of other command buffers but the first
 * (the blits, the draws); 4, the first such call (the state Mesa restores).
 */
#define CP_NOP                  0x10
#define CP_SET_AMBLE            0x55

static void
gpu_chicken_fix(uint64_t address, uint32_t words, unsigned depth)
{
	uint32_t calls = 0;

	for (uint32_t at = 0; at < words && at < GPU_DUMP_WORDS;) {
		uint32_t header = gpu_word(address + 4 * at), count;

		if (header >> 28 == 4) {
			uint32_t reg = header >> 8 & 0x7ffff, n = header & 0x7f;

			for (unsigned j = 0; j < sizeof(gpu_turnip) / sizeof(gpu_turnip[0]); j++) {
				uint64_t where = address + 4 * (at + 1 + gpu_turnip[j].reg - reg);
				uint64_t phys;
				volatile uint32_t *word;

				if ((gpu_chicken_skip & 1u << j) || gpu_turnip[j].reg < reg || gpu_turnip[j].reg >= reg + n ||
				    gpu_word(where) == gpu_turnip[j].value) {
					continue;
				}
				phys = space_phys(&core_space, where);
				word = (volatile uint32_t *)(phys != 0 ? phystokv(phys) : 0);
				if (word == NULL) {
					continue;
				}
				if (gpu_chickened++ < 16) {
					printf("sgpu: gpu: %s: %#x -> %#x in %#llx\n", gpu_turnip[j].name, *word, gpu_turnip[j].value,
					    address + 4 * at);
				}
				*word = gpu_turnip[j].value;
				to_memory((vm_offset_t)word, 4);
			}
			at += 1 + n;
			continue;
		}
		if (header >> 28 != 7) {
			break;
		}
		count = header & 0x3fff;
		if (depth > 0 && gpu_nop != 0) {
			uint32_t command = header >> 16 & 0x7f;
			bool call = command == CP_INDIRECT_BUFFER && count == 3;
			bool nop = (command == CP_SET_AMBLE && (gpu_nop & 1)) || (call && calls > 0 && (gpu_nop & 2)) ||
			    (call && calls == 0 && (gpu_nop & 4));

			calls += call;
			if (nop && gpu_put(address + 4 * at, 7u << 28 | count | odd_parity(count) << 15 | CP_NOP << 16 |
			    odd_parity(CP_NOP) << 23)) {
				if (gpu_nopped++ < 16) {
					printf("sgpu: gpu: command %#x of %u words at %#llx made a CP_NOP\n", command, count, address + 4 * at);
				}
				at += 1 + count;
				continue;
			}
		}
		if (!(gpu_chicken_skip & 16) && (header >> 16 & 0x7f) == CP_EVENT_WRITE && count >= 1 && (gpu_word(address + 4 * (at + 1)) & 0xff) == 0x04) {
			/*
			 * CACHE_FLUSH_TS, the A6xx's event for a number written once the caches are clean: Mesa 25.0.7
			 * ends each of its command buffers with one. On the A7xx Linux and newer Mesa use CACHE_CLEAN.
			 */
			uint64_t phys = space_phys(&core_space, address + 4 * (at + 1));
			volatile uint32_t *word = (volatile uint32_t *)(phys != 0 ? phystokv(phys) : 0);

			if (word != NULL) {
				if (gpu_chickened++ < 16) {
					printf("sgpu: gpu: CACHE_FLUSH_TS -> CACHE_CLEAN in %#llx\n", address + 4 * at);
				}
				*word = (*word & ~0xffu) | EVENT_CACHE_CLEAN;
				to_memory((vm_offset_t)word, 4);
			}
		}
		if (depth > 0 && (header >> 16 & 0x7f) == CP_INDIRECT_BUFFER && count == 3) {
			gpu_chicken_fix((uint64_t)gpu_word(address + 4 * (at + 2)) << 32 | gpu_word(address + 4 * (at + 1)),
			    gpu_word(address + 4 * (at + 3)) & 0xfffff, depth - 1);
		}
		at += 1 + count;
	}
}

/* The command processor is to write down where it is (not waiting for the pipeline: as soon as it gets there). */
static void
gpu_progress(uint32_t where)
{
	ring_command(CP_MEM_WRITE, 3);
	ring(CORE_SCRATCH_ADDRESS + GPU_PROGRESS_AT);
	ring(0);
	ring(where);
}

/* The core is to write down what time it is: at one end of submission `fence`. */
static void
ring_time(uint32_t fence, uint32_t end)
{
	ring_command(CP_REG_TO_MEM, 3);
	ring(CP_ALWAYS_ON_COUNTER / 4 | 2u << 18 | 1u << 30);   /* two words, a 64-bit register */
	ring(CORE_SCRATCH_ADDRESS + GPU_TIMES_AT + 16 * (fence % GPU_TIMES) + 8 * end);
	ring(0);
}

/* Add up the times of the submissions the core has finished since this was last done. */
static void
gpu_times(void)
{
	volatile uint64_t *times = (volatile uint64_t *)(core_scratch.memory + GPU_TIMES_AT);
	uint32_t done;

	to_memory(core_scratch.memory, 4096);
	done = *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);
	if (done - gpu_timed > GPU_TIMES) {
		gpu_timed = done - GPU_TIMES;           /* (the older ones have been written over) */
	}
	while (gpu_timed != done) {
		uint32_t slot = ++gpu_timed % GPU_TIMES;

		if (gpu_timed != 0 && times[2 * slot + 1] > times[2 * slot]) {
			static const uint32_t under[6] = { 1920, 4800, 9600, 19200, 38400, 76800 };     /* (19.2 ticks a microsecond) */
			uint64_t ticks = times[2 * slot + 1] - times[2 * slot];
			unsigned by = 0;

			if (gpu_log) {
				redwood_tl("tl: q %u took %llu\n", gpu_timed, ticks * 10 / 192);
			}
			while (by < 6 && ticks >= under[by]) {
				by++;
			}
			gpu_timed_by[by]++;
			gpu_ticks += ticks;
			dcvs_ticks += ticks;
			gpu_timed_count++;
		}
	}
}

void
shennong_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	gpu_times();
	*microseconds = gpu_ticks * 10 / 192;
	*submissions = gpu_timed_count;
	memcpy(by, gpu_timed_by, sizeof(gpu_timed_by));
	bzero(gpu_timed_by, sizeof(gpu_timed_by));
	gpu_ticks = 0;
	gpu_timed_count = 0;
}

/*
 * Command buffers for the 3D core to run, in order: each an address and a count of words. As Linux's
 * a7xx_submit puts them in the ring, less what is for concurrent binning (off: step 20), preemption and
 * power collapse, none of which there is here. Returns the submission's number; 0: the ring is full and
 * the core is not reading it.
 */
/*
 * Boot-arg sgpu_dcvs=1: the 3D core's speed follows its load, as msm's devfreq does, from 231 MHz up to sgpu_speed's
 * (the most it may have). Every 50 ms (shennong_gpu_dcvs, from RedwoodGPU.cpp under its lock): how much of the window
 * the core was at work (its submissions' start-to-end times, gpu_times). Over 80% two speeds up, at once; under 45% two
 * windows running, one down. The GMU is told by the vote it was given at the start (HFI GX_BW_PERF_VOTE, a6xx_hfi_
 * set_freq). A fixed 903 MHz had the phone hot as an oven with the desktop idle (15 November).
 */
static uint32_t dcvs_on = 0, dcvs_max, dcvs_low;
static uint64_t dcvs_window_start;
static uint64_t dcvs_time_at[GPU_SPEEDS];      /* mach_absolute_time spent at each speed, for the report */
static uint64_t dcvs_last_report, dcvs_since;
static uint32_t dcvs_changes, dcvs_failures;

/*
 * The thermal service's ceiling (bsd/dev/shennong_thermal.c): the highest index of gpu_speeds DCVS may pick; 0 none.
 * Set from the service's thread, carried out by the next DCVS window (under the GPU's lock). Only with sgpu_dcvs=1: a
 * fixed sgpu_speed is for measuring and stays as asked.
 */
static volatile uint32_t dcvs_thermal_cap;

void
shennong_gpu_thermal_cap(uint32_t index)
{
	dcvs_thermal_cap = index;
}

/* for the thermal service's table: the speed of index i (kHz, 0 past the end), the index now, the most DCVS may take */
uint32_t
shennong_gpu_speed_khz(uint32_t index)
{
	return index < GPU_SPEEDS ? gpu_speeds[index].khz : 0;
}

uint32_t
shennong_gpu_speed_now(uint32_t *most)
{
	if (most != NULL) {
		*most = dcvs_on ? (dcvs_max ? dcvs_max : speed_index) : 0;
	}
	return speed_index;
}

static bool
dcvs_vote(uint32_t index)
{
	uint32_t vote[4] = { 0, 1, index, 0 };
	bool ok;

	hfi_quiet = true;
	ok = hfi_send("a speed", HFI_GX_BW_PERF_VOTE, vote, 4);
	hfi_quiet = false;
	if (ok) {
		speed_index = index;
		dcvs_changes++;
	} else {
		dcvs_failures++;
	}
	return ok;
}

/*
 * How warm it is, for the 5 s line: the SoC's sensors (dt: tsens0 at 0xc228000, tsens2 at 0xc22a000, qcom,tsens-v2;
 * Linux's tsens-v2.c) each keep their last reading in a status word, the low 12 bits signed in tenths of a degree,
 * bit 21 when there is one. Read only. GPU: tsens2 sensors 1-8 (gpuss-0..7); the big CPUs: tsens0 sensors 5-10
 * (cpu-2-*). The hottest of each; -273 when none answers.
 */
static struct block tsens0 = { "tsens0", 0x0c228000, 0x1000 };
static struct block tsens2 = { "tsens2", 0x0c22a000, 0x1000 };
#define SN_TSENS_STATUS(sensor) (0xa0 + 4 * (sensor))
#define SN_TSENS_VALID          (1u << 21)

static int32_t
tsens_hottest(struct block *b, unsigned first, unsigned last)
{
	int32_t best = -2730;

	need(b);
	for (unsigned s = first; s <= last; s++) {
		for (unsigned again = 0; again < 4; again++) {
			uint32_t status = rd(b->base + SN_TSENS_STATUS(s));

			if (status & SN_TSENS_VALID) {
				int32_t t = (int32_t)(status << 20) >> 20;
				if (t > best) {
					best = t;
				}
				break;
			}
		}
	}
	return best;
}

/*
 * The A750's own performance counters, read beside the 5 s DCVS line (boot-arg sgpu_counters=1): what a frame's time
 * goes on, as Qualcomm's profiler and Mesa's fd7_perfcntr.c derive it. Each counter is chosen once by its group's select
 * register (the shader and texture groups' through the BR pipe's aperture, CP_APERTURE_CNTL_HOST, as KGSL writes them)
 * and read as a 64-bit RBBM_PERFCTR pair; the line gives what each counted in the 5 s, against the cycles the GPU was
 * busy (RBBM_STATUS_MASKED). Countables: Mesa's a7xx_perfcntrs.xml; registers: a6xx.xml's A7XX variants.
 */
#define PC_APERTURE             GPU_WORD(0x0a00)
#define PC_CNTL                 GPU_WORD(0x0500)
static const struct sgpu_counter {
	const char *name;
	uint16_t sel, ctr;              /* the select register and the counter's low word, in words */
	uint8_t countable, br;
} sgpu_counters[] = {
	{ "busy", 0x0507, 0x031c, 6, 0 },               /* RBBM 0: STATUS_MASKED */
	{ "cp", 0x08d1, 0x0302, 2, 0 },                 /* CP 1: BUSY_CYCLES (CP 0 is KGSL's) */
	{ "sp", 0xae80, 0x03a6, 0, 1 },                 /* SP 0: BUSY_CYCLES */
	{ "alu", 0xae81, 0x03a8, 1, 1 },                /* SP 1: ALU_WORKING_CYCLES */
	{ "sp-stall-tp", 0xae82, 0x03aa, 4, 1 },        /* SP 2: STALL_CYCLES_TP */
	{ "sp-noexec", 0xae83, 0x03ac, 7, 1 },          /* SP 3: NON_EXECUTION_CYCLES */
	{ "sp-stall-rb", 0xae84, 0x03ae, 6, 1 },        /* SP 4: STALL_CYCLES_RB */
	{ "waves", 0xae85, 0x03b0, 9, 1 },              /* SP 5: WAVE_CONTEXT_CYCLES */
	{ "icl1-miss", 0xae86, 0x03b2, 52, 1 },         /* SP 6: ICL1_MISSES */
	{ "fs-full", 0xae87, 0x03b4, 40, 1 },           /* SP 7: FS_STAGE_FULL_ALU_INSTRUCTIONS */
	{ "fs-half", 0xae88, 0x03b6, 41, 1 },           /* SP 8: FS_STAGE_HALF_ALU_INSTRUCTIONS */
	{ "pixels", 0xae89, 0x03b8, 101, 1 },           /* SP 9: PIXELS */
	{ "eu-fs", 0xae8a, 0x03ba, 74, 1 },             /* SP 10: ANY_EU_WORKING_FS_STAGE */
	{ "tp", 0xb610, 0x038e, 0, 1 },                 /* TP 0: BUSY_CYCLES */
	{ "tp-l1-req", 0xb611, 0x0390, 6, 1 },          /* TP 1: L1_CACHELINE_REQUESTS */
	{ "tp-l1-miss", 0xb612, 0x0392, 7, 1 },         /* TP 2: L1_CACHELINE_MISSES */
	{ "rb", 0x8e10, 0x03d6, 0, 0 },                 /* RB 0: BUSY_CYCLES */
	{ "rb-starve-sp", 0x8e11, 0x03d8, 5, 0 },       /* RB 1: STARVE_CYCLES_SP */
	{ "rb-stall-ccu", 0x8e12, 0x03da, 44, 0 },      /* RB 2: STALL_CYCLES_CCU */
	{ "rb-stall-hlsq", 0x8e13, 0x03dc, 1, 0 },      /* RB 3: STALL_CYCLES_HLSQ */
	{ "ccu", 0x8e18, 0x035c, 0, 0 },                /* CCU 0: BUSY_CYCLES */
	{ "ccu-blocks", 0x8e19, 0x035e, 4, 0 },         /* CCU 1: COLOR_BLOCKS */
	{ "ccu-partial", 0x8e1a, 0x0360, 7, 0 },        /* CCU 2: PARTIAL_BLOCK_READ */
	{ "ccu-gmem-rd", 0x8e1b, 0x0362, 8, 0 },        /* CCU 3: GMEM_READ */
	{ "ccu-gmem-wr", 0x8e1c, 0x0364, 9, 0 },        /* CCU 4: GMEM_WRITE */
	{ "rb-fifo0-full", 0x8e14, 0x03de, 2, 0 },      /* RB 4: STALL_CYCLES_FIFO0_FULL */
	{ "uche-stall", 0x0e1c, 0x0376, 1, 0 },         /* UCHE 0: STALL_CYCLES_ARBITER */
	{ "rd-ch0", 0x0e1d, 0x0378, 27, 0 },            /* UCHE 1: VBIF_READ_BEATS_CH0 */
	{ "rd-ch1", 0x0e1e, 0x037a, 28, 0 },            /* UCHE 2: VBIF_READ_BEATS_CH1 */
	{ "wr-ch0", 0x0e1f, 0x037c, 52, 0 },            /* UCHE 3: VBIF_WRITE_BEATS_CH0 */
	{ "wr-ch1", 0x0e20, 0x037e, 53, 0 },            /* UCHE 4: VBIF_WRITE_BEATS_CH1 */
};
#define SGPU_COUNTERS (sizeof(sgpu_counters) / sizeof(sgpu_counters[0]))
static uint32_t sgpu_counters_on;
static uint64_t sgpu_counter_last[SGPU_COUNTERS];

static uint64_t
sgpu_counter_read(const struct sgpu_counter *c)
{
	uint32_t hi, lo, again;

	do {
		hi = rd(gpu.base + GPU_WORD(c->ctr + 1));
		lo = rd(gpu.base + GPU_WORD(c->ctr));
		again = rd(gpu.base + GPU_WORD(c->ctr + 1));
	} while (again != hi);
	return (uint64_t)hi << 32 | lo;
}

static void
sgpu_counters_select(void)
{
	wr(gpu.base + PC_CNTL, 1);
	for (uint32_t i = 0; i < SGPU_COUNTERS; i++) {
		const struct sgpu_counter *c = &sgpu_counters[i];

		wr(gpu.base + PC_APERTURE, c->br ? 1u << 12 : 0u);     /* PIPE_BR */
		wr(gpu.base + GPU_WORD(c->sel), c->countable);
		wr(gpu.base + PC_APERTURE, 0);
		sgpu_counter_last[i] = sgpu_counter_read(c);
	}
}

static void
sgpu_counters_report(void)
{
	char line[900];
	size_t used = 0;
	uint64_t d[SGPU_COUNTERS];

	if (!sgpu_counters_on) {
		return;
	}
	for (uint32_t i = 0; i < SGPU_COUNTERS; i++) {
		uint64_t now = sgpu_counter_read(&sgpu_counters[i]);

		d[i] = now >= sgpu_counter_last[i] ? now - sgpu_counter_last[i] : 0;
		sgpu_counter_last[i] = now;
	}
	/* (each against the busy cycles, in hundredths; the beats of 32 bytes as MB/s over the 5 s) */
	used += (size_t)snprintf(line + used, sizeof(line) - used, "busy %llu Mcycles;", (unsigned long long)(d[0] / 1000000));
	for (uint32_t i = 1; i < SGPU_COUNTERS && used + 40 < sizeof(line); i++) {
		if (i == 13 || i == 22) {   /* (in three lines: one too long is cut by the console) */
			printf("sgpu: counters per busy cycle, the last 5 s: %s\n", line);
			used = (size_t)snprintf(line, sizeof(line), "(more)");
		}
		if (sgpu_counters[i].ctr >= 0x0378 && sgpu_counters[i].ctr <= 0x037e) {
			used += (size_t)snprintf(line + used, sizeof(line) - used, " %s %llu MB/s", sgpu_counters[i].name,
			    (unsigned long long)(d[i] * 32 / 5 / 1000000));
		} else {
			used += (size_t)snprintf(line + used, sizeof(line) - used, " %s %llu.%02llu", sgpu_counters[i].name,
			    (unsigned long long)(d[0] ? d[i] / d[0] : 0), (unsigned long long)(d[0] ? d[i] * 100 / d[0] % 100 : 0));
		}
	}
	printf("sgpu: counters per busy cycle, the last 5 s: %s\n", line);
}

extern bool shennong_touch_boosted(void);

void
shennong_gpu_dcvs(void)
{
	uint64_t now = mach_absolute_time(), window_ns, busy_us, period;
	uint32_t busy, want, top;

	if (!dcvs_on || !shennong_gpu_ready()) {
		return;
	}
	if (dcvs_window_start == 0) {
		dcvs_window_start = dcvs_last_report = dcvs_since = now;
		dcvs_max = speed_index;
		{
			uint32_t on = 0;

			PE_parse_boot_argn("sgpu_counters", &on, sizeof(on));
			if (on) {
				sgpu_counters_select();
				sgpu_counters_on = 1;
				printf("sgpu: %u performance counters chosen (sgpu_counters=1)\n", (unsigned)SGPU_COUNTERS);
			}
		}
		return;
	}
	gpu_times();
	absolutetime_to_nanoseconds(now - dcvs_window_start, &window_ns);
	if (window_ns < 40 * NSEC_PER_MSEC) {
		return;
	}
	busy_us = dcvs_ticks * 10 / 192;
	dcvs_ticks = 0;
	busy = (uint32_t)(window_ns ? busy_us * 1000 * 100 / window_ns : 0);
	dcvs_time_at[speed_index] += now - dcvs_window_start;
	dcvs_window_start = now;
	want = speed_index;
	top = dcvs_max;
	if (dcvs_thermal_cap != 0 && dcvs_thermal_cap < top) {
		top = dcvs_thermal_cap;
	}
	if (shennong_touch_boosted()) {
		want = top;                     /* a finger on the glass (shennong_thermal.c): the ceiling at once */
		dcvs_low = 0;
	} else if (busy > 80) {
		want = speed_index + 2 > top ? top : speed_index + 2;
		dcvs_low = 0;
	} else if (busy < 45) {
		if (++dcvs_low >= 2 && speed_index > 1) {
			want = speed_index - 1;
			dcvs_low = 0;
		}
	} else {
		dcvs_low = 0;
	}
	if (want > top) {
		want = top;                     /* the thermal ceiling came down: at once */
	}
	if (want != speed_index && dcvs_failures < 8) {
		(void)dcvs_vote(want);
	}
	/* every 5 s: how the time went by speed */
	nanoseconds_to_absolutetime(5 * NSEC_PER_SEC, &period);
	if (now - dcvs_last_report >= period) {
		char line[512];
		size_t used = 0;
		uint64_t all = now - dcvs_since;

		for (uint32_t i = 1; i < GPU_SPEEDS && used + 24 < sizeof(line); i++) {
			if (dcvs_time_at[i] != 0 && all != 0) {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " %u:%llu%%", gpu_speeds[i].khz / 1000,
				    dcvs_time_at[i] * 100 / all);
			}
			dcvs_time_at[i] = 0;
		}
		int32_t tg = tsens_hottest(&tsens2, 1, 8), tc = tsens_hottest(&tsens0, 5, 10);
		printf("sgpu: dcvs: the last 5 s by MHz:%s; now %u MHz, %u changes, %u refused; gpu %d.%d C, cpu %d.%d C\n", line,
		    gpu_speeds[speed_index].khz / 1000, dcvs_changes, dcvs_failures, tg / 10, (tg < 0 ? -tg : tg) % 10, tc / 10,
		    (tc < 0 ? -tc : tc) % 10);
		dcvs_changes = 0;
		dcvs_last_report = dcvs_since = now;
		sgpu_counters_report();
	}
}

uint32_t
shennong_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t wanted = 32 + 20 * count, room = 0;

	for (int i = 0; i < 20000; i++) {
		room = (ring_taken() + RING_WORDS - ring_at - 1) % RING_WORDS;
		if (room >= wanted) {
			break;
		}
		sleep_us(100);
	}
	if (room < wanted) {
		return 0;
	}
	gpu_times();
	gpu_word_forget();
	/* (not a stream that says it is not Mesa's: hakios-metal's, whose registers are turnip's already; the walk read
	 * every word of it through our page tables, 0.85 ms of each of the window server's submissions) */
	for (uint32_t i = 0; gpu_chicken && !shennong_gpu_plain && i < count; i++) {
		gpu_chicken_fix(addresses[i], words[i], 1);
	}
	if (gpu_bisect && gpu_fence == 0 && count > 0) {
		gpu_bisect_first(addresses[0], words[0]);
	}
	if (++gpu_fence == 0) {
		gpu_fence = 1;
	}
	gpu_sent[gpu_fence % GPU_KEPT_SUBMISSIONS].fence = gpu_fence;
	gpu_sent[gpu_fence % GPU_KEPT_SUBMISSIONS].count = count;
	for (uint32_t i = 0; i < count && i < GPU_KEPT_COMMANDS; i++) {
		gpu_sent[gpu_fence % GPU_KEPT_SUBMISSIONS].address[i] = addresses[i];
		gpu_sent[gpu_fence % GPU_KEPT_SUBMISSIONS].words[i] = words[i];
	}
	/*
	 * Mesa's command buffers go to both threads, as Linux's a7xx_submit has them (CP_SET_THREAD_BOTH): each
	 * says itself which of its commands are BR's (CP_THREAD_CONTROL), and one that turns concurrent binning
	 * off waits for BV to come there too. Given to BR alone, the first of them never came back (seen in
	 * the cut-short runs of gpu_bisect_first: its second command, CP_THREAD_CONTROL 0x08000001).
	 */
	ring_command(CP_THREAD_CONTROL, 1);
	ring(THREAD_SYNC | THREAD_BOTH);
	ring_time(gpu_fence, 0);
	ring_event(EVENT_CCU_INVALIDATE_DEPTH);
	ring_event(EVENT_CCU_INVALIDATE_COLOR);
	for (uint32_t i = 0; i < count; i++) {
		ring_command(CP_THREAD_CONTROL, 1);
		ring(THREAD_BR);
		gpu_progress(gpu_fence << 16 | (i + 1) << 8 | 1);
		ring_command(CP_THREAD_CONTROL, 1);
		ring(THREAD_BOTH);
		ring_command(CP_INDIRECT_BUFFER, 3);
		ring((uint32_t)addresses[i]);
		ring((uint32_t)(addresses[i] >> 32));
		ring(words[i]);
		ring_command(CP_THREAD_CONTROL, 1);
		ring(THREAD_BR);
		if (gpu_wfi) {
			ring_command(CP_WAIT_FOR_IDLE, 0);      /* "past" then says the pipeline has finished it too */
		}
		gpu_progress(gpu_fence << 16 | (i + 1) << 8 | 2);
	}
	/* (from here on BR's alone) */
	if (gpu_exact) {
		ring_command(CP_WAIT_FOR_IDLE, 0);      /* the time is the pipeline's, not the command processor's, which is ahead of it */
	}
	ring_time(gpu_fence, 1);
	/*
	 * The number is written by the BR thread, the one that has been through the command buffers: an event
	 * given to both threads is done by BV as soon as it gets there, ahead of what BR is still drawing (Linux's
	 * a7xx_submit writes its fence under CP_SET_THREAD_BR for that reason). Seen: submissions 1 and 2 said
	 * done while the core was still in the first.
	 */
	ring_register(CP_SCRATCH_REG2, gpu_fence);
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_CLEAN | EVENT_WRITE_ENABLED);
	ring(CORE_SCRATCH_ADDRESS + GPU_FENCE_AT);
	ring(0);
	ring(gpu_fence);
	ring_command(CP_THREAD_CONTROL, 1);
	ring(THREAD_BOTH);
	ring_send();
	return gpu_fence;
}

/* The memory behind an address of the 3D core's, by our own tables; 0: nothing there. */
static uint64_t
space_phys(const struct gpu_space *space, uint64_t address)
{
	const uint64_t *table = space->root;

	for (int level = 0; level < 3 && table != NULL; level++) {
		uint64_t entry = table[address >> (30 - 9 * level) & 0x1ff];

		if (!(entry & 1)) {
			return 0;
		}
		if (level == 2) {
			return (entry & PTE_ADDRESS) | (address & 0xfff);
		}
		table = NULL;
		for (uint32_t i = 0; i < space->table_count; i++) {
			if (space->tables[i].phys == (entry & PTE_ADDRESS)) {
				table = space->tables[i].table;
			}
		}
	}
	return 0;
}

/*
 * The words of a command buffer, into the log, eight to a line with their offset: for reading the commands
 * of a submission the core did not finish (by Mesa's adreno_pm4.xml) and finding the one it stopped at.
 */
static uint64_t gpu_word_page = ~0ull, gpu_word_line = ~0ull;
static volatile uint8_t *gpu_word_at;

static uint32_t
gpu_word(uint64_t address)
{
	uint64_t page = address & ~0xfffull, line = address & ~63ull;
	volatile uint32_t *word;

	/*
	 * gpu_chicken_fix reads every word of every command buffer of every submission: a walk of the page
	 * tables and a cache line made clean for each word made that most of the time a frame spent in the
	 * kernel. The page last walked and the line last made clean are kept (gpu_word_forget(): anew for
	 * each submission, whose buffers may be at addresses that were others' before).
	 */
	if (page != gpu_word_page) {
		uint64_t phys = space_phys(&core_space, page);

		gpu_word_page = page;
		gpu_word_at = phys != 0 ? (volatile uint8_t *)phystokv(phys) : NULL;
		gpu_word_line = ~0ull;
	}
	if (gpu_word_at == NULL) {
		return 0xdeadbeef;
	}
	word = (volatile uint32_t *)(gpu_word_at + (address & 0xfff));
	if (line != gpu_word_line) {
		to_memory((vm_offset_t)(gpu_word_at + (line & 0xfff)), 64);
		gpu_word_line = line;
	}
	return *word;
}

static void
gpu_word_forget(void)
{
	gpu_word_page = ~0ull;
	gpu_word_line = ~0ull;
	gpu_word_at = NULL;
}

/*
 * ... and, `depth` levels down, the buffers its commands call: CP_INDIRECT_BUFFER's, and the groups of
 * CP_SET_DRAW_STATE (where Mesa keeps a draw's shaders and state), each buffer once.
 */
static void
gpu_dump(uint64_t address, uint32_t words, unsigned depth)
{
	uint32_t line[8];

	gpu_word_forget();

	for (uint32_t i = 0; i < gpu_dumped_count; i++) {
		if (gpu_dumped[i] == address) {
			return;
		}
	}
	if (gpu_dumped_count == GPU_DUMP_BUFFERS || gpu_dumped_words >= GPU_DUMP_ALL) {
		printf("sgpu: gpu:   (no more: %#llx, %u words)\n", address, words);
		return;
	}
	gpu_dumped[gpu_dumped_count++] = address;
	printf("sgpu: gpu:   buffer %#llx, %u words\n", address, words);
	if (words > GPU_DUMP_WORDS) {
		printf("sgpu: gpu:   (the first %u words of %u)\n", GPU_DUMP_WORDS, words);
		words = GPU_DUMP_WORDS;
	}
	gpu_dumped_words += words;
	for (uint32_t at = 0; at < words; at += 8) {
		unsigned n = words - at < 8 ? words - at : 8;

		for (unsigned i = 0; i < 8; i++) {
			line[i] = i < n ? gpu_word(address + 4 * (at + i)) : 0xdeadbeef;
		}
		printf("sgpu: ib %#llx+%#05x: %08x %08x %08x %08x %08x %08x %08x %08x\n", address, 4 * at, line[0], line[1],
		    line[2], line[3], line[4], line[5], line[6], line[7]);
	}
	if (depth == 0) {
		return;
	}
	for (uint32_t at = 0; at < words;) {
		uint32_t header = gpu_word(address + 4 * at), count, command;

		if (header >> 28 == 4) {
			uint32_t reg = header >> 8 & 0x7ffff, n = header & 0x7f;
			static const uint32_t shaders[] = { SP_VS_BASE, SP_PS_BASE };

			for (unsigned j = 0; j < 2; j++) {
				if (reg <= shaders[j] && shaders[j] + 1 < reg + n) {
					uint64_t where = (uint64_t)gpu_word(address + 4 * (at + 2 + shaders[j] - reg)) << 32 |
					    gpu_word(address + 4 * (at + 1 + shaders[j] - reg));

					printf("sgpu: gpu:   shader at %#llx (%s): %08x %08x %08x %08x %08x %08x %08x %08x\n", where,
					    j == 0 ? "vertex" : "fragment", gpu_word(where), gpu_word(where + 4), gpu_word(where + 8),
					    gpu_word(where + 12), gpu_word(where + 16), gpu_word(where + 20), gpu_word(where + 24),
					    gpu_word(where + 28));
				}
			}
			at += 1 + n;
			continue;
		}
		if (header >> 28 != 7) {
			break;                          /* (not a command: what follows is not either) */
		}
		count = header & 0x3fff;
		command = header >> 16 & 0x7f;
		if (command == CP_EVENT_WRITE && count >= 3) {
			uint64_t where = (uint64_t)gpu_word(address + 4 * (at + 3)) << 32 | gpu_word(address + 4 * (at + 2));

			printf("sgpu: gpu:   memory an event writes, %#llx: %08x %08x %08x %08x\n", where, gpu_word(where),
			    gpu_word(where + 4), gpu_word(where + 8), gpu_word(where + 12));
		} else if (command == CP_INDIRECT_BUFFER && count == 3) {
			gpu_dump((uint64_t)gpu_word(address + 4 * (at + 2)) << 32 | gpu_word(address + 4 * (at + 1)),
			    gpu_word(address + 4 * (at + 3)) & 0xfffff, depth - 1);
		} else if (command == CP_SET_DRAW_STATE) {
			for (uint32_t g = 0; g + 3 <= count; g += 3) {
				uint32_t state = gpu_word(address + 4 * (at + 1 + g));
				uint64_t where = (uint64_t)gpu_word(address + 4 * (at + 3 + g)) << 32 |
				    gpu_word(address + 4 * (at + 2 + g));

				if ((state & 0xffff) != 0 && where != 0) {
					gpu_dump(where, state & 0xffff, depth - 1);
				}
			}
		}
		at += 1 + count;
	}
}

/* A submission does not come back: what the 3D core and its SMMU context say. */
void
shennong_gpu_report(const char *why)
{
	uint32_t done;
	uint64_t ib;

	to_memory(core_scratch.memory, 4096);
	printf("sgpu: gpu: %s: submission %u is the last, the core has done %u; ring: read up to %u (by its register %u), "
	    "written up to %u; status %#x, interrupts %#x; command processor: fault %#x, interrupts %#x, protection %#x\n", why,
	    gpu_fence, *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT), ring_taken(), rd(gpu.base + CP_RB_RPTR),
	    ring_at, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT),
	    rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS));
	printf("sgpu: gpu:   in the first command buffer at %#llx, %u words to go; in the second at %#llx, %u to go\n",
	    (uint64_t)rd(gpu.base + CP_IB1_BASE + 4) << 32 | rd(gpu.base + CP_IB1_BASE), rd(gpu.base + CP_IB1_REM_SIZE),
	    (uint64_t)rd(gpu.base + CP_IB2_BASE + 4) << 32 | rd(gpu.base + CP_IB2_BASE), rd(gpu.base + CP_IB2_REM_SIZE));
	printf("sgpu: gpu:   status 3 %#x; the ring of the submission: words %u to %u\n", rd(gpu.base + GPU_WORD(0x0213)),
	    ring_taken(), ring_at);
	printf("sgpu: gpu:   status 1 %#x, 2 %#x; draw state at %#llx, %u words to go; memory read at %#llx, %u to go\n",
	    rd(gpu.base + GPU_WORD(0x0211)), rd(gpu.base + GPU_WORD(0x0212)),
	    (uint64_t)rd(gpu.base + CP_SDS_BASE + 4) << 32 | rd(gpu.base + CP_SDS_BASE), rd(gpu.base + CP_SDS_REM_SIZE),
	    (uint64_t)rd(gpu.base + CP_MRB_BASE + 4) << 32 | rd(gpu.base + CP_MRB_BASE), rd(gpu.base + CP_MRB_REM_SIZE));
	{
		volatile uint64_t *times = (volatile uint64_t *)(core_scratch.memory + GPU_TIMES_AT);
		uint32_t slot = (*(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT) + 1) % GPU_TIMES;

		uint32_t progress = *(volatile uint32_t *)(core_scratch.memory + GPU_PROGRESS_AT);

		printf("sgpu: gpu:   the next submission's times (written by the core, 0: not): start %#llx, end %#llx\n",
		    times[2 * slot], times[2 * slot + 1]);
		printf("sgpu: gpu:   BR's command processor got to submission %u, %s command buffer %u (%#x)\n", progress >> 16,
		    (progress & 0xff) == 1 ? "into" : "past", progress >> 8 & 0xff, progress);
	}
	printf("sgpu: gpu:   the BV pipe: ring read up to %u (its scratch: %u); first command buffer at %#llx, %u to go; "
	    "second at %#llx, %u to go\n", rd(gpu.base + CP_BV_RB_RPTR),
	    *(volatile uint32_t *)(core_scratch.memory + SCRATCH_BV_RPTR),
	    (uint64_t)rd(gpu.base + CP_BV_IB1_BASE + 4) << 32 | rd(gpu.base + CP_BV_IB1_BASE), rd(gpu.base + CP_BV_IB1_REM_SIZE),
	    (uint64_t)rd(gpu.base + CP_BV_IB2_BASE + 4) << 32 | rd(gpu.base + CP_BV_IB2_BASE), rd(gpu.base + CP_BV_IB2_REM_SIZE));
	step = 0;
	if (space_faulted(&core_space)) {
		wr(smmu.base + smmu_context(&core_space) + SMMU_CB_FSR, SMMU_FSR_ALL);  /* so that the next one shows */
	}
	/*
	 * The command buffers of the submission the core is in: the one with the command buffer it is reading,
	 * or else the one after the last one done.
	 */
	gpu_dumped_count = gpu_dumped_words = 0;
	done = *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT) + 1;
	ib = (uint64_t)rd(gpu.base + CP_IB1_BASE + 4) << 32 | rd(gpu.base + CP_IB1_BASE);
	for (uint32_t k = 0; k < GPU_KEPT_SUBMISSIONS; k++) {
		for (uint32_t i = 0; i < gpu_sent[k].count && i < GPU_KEPT_COMMANDS; i++) {
			if (ib >= gpu_sent[k].address[i] && ib < gpu_sent[k].address[i] + 4ull * gpu_sent[k].words[i]) {
				done = gpu_sent[k].fence;
			}
		}
	}
	for (uint32_t k = 0; k < GPU_KEPT_SUBMISSIONS; k++) {
		if (gpu_sent[k].fence != done) {
			continue;
		}
		for (uint32_t i = 0; i < gpu_sent[k].count && i < GPU_KEPT_COMMANDS; i++) {
			printf("sgpu: gpu: submission %u, command buffer %u of %u: %#llx, %u words\n", done, i + 1, gpu_sent[k].count,
			    gpu_sent[k].address[i], gpu_sent[k].words[i]);
			gpu_dump(gpu_sent[k].address[i], gpu_sent[k].words[i], 2);
		}
	}
}

/*
 * boot-arg gpu_recover=1 on the A750: a core that has stopped answering is reset and given work again, as
 * redwood_adreno.c's redwood_gpu_recover() does for the A6xx: its bus told to finish and start nothing more,
 * the GMU asked to keep it powered meanwhile, the 3D core's half reset (RBBM_SW_RESET_CMD, a6xx.xml; the
 * A6xx's sequence, Linux's a6xx_gpu_sw_reset), the writes the GMU's fence dropped forgotten and the fence
 * opened, then steps 20 and 21 again -- the registers Linux writes before the command processor starts, the
 * ring from its start, CP_ME_INIT, out of secure mode -- and every submission until the last called done (the
 * word the waiters read set to it: msm_gpu.c's recover_worker does as much with its fence). The GMU itself,
 * its firmware, the SMMU's tables and every buffer in them are left as they are. Without the boot-arg, or past
 * GPU_RECOVERIES in one run, a core that has stopped stays stopped, as before.
 */
#define A7_RBBM_SW_RESET_CMD            GPU_WORD(0x0043)
#define A7_RBBM_INT_CLEAR_CMD           GPU_WORD(0x0038)        /* a6xx.xml */
#define A7_GMU_PWR_COL_KEEPALIVE        GMU_WORD(0x1f8c3)       /* a6xx_gmu.xml 0x50c3, the A7xx's GMU block 0x1a800 further */
#define A7_GMU_AHB_FENCE_STATUS_CLR     GMU_WORD(0x23b14)       /* ... 0x9314 */
#define A7_GPU_RECOVERIES               64

bool
shennong_gpu_recover(void)
{
	static uint32_t recoveries;
	uint32_t enabled = 0, fence_status;
	uint32_t was_step = step, was_newest = newest_step, was_pause = pause_ms;
	bool done;

	if (!PE_parse_boot_argn("gpu_recover", &enabled, sizeof(enabled)) || enabled == 0 || !gpu_kept) {
		return false;
	}
	if (recoveries >= A7_GPU_RECOVERIES) {
		printf("sgpu: the 3D core has been recovered %u times in this run already: no more\n", recoveries);
		return false;
	}
	recoveries++;
	printf("sgpu: recovering the 3D core (%u of at most %u in this run): status %#x, interrupts %#x, ring read up to %u of %u, "
	    "the last submission %u\n", recoveries, A7_GPU_RECOVERIES, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
	    rd(gpu.base + CP_RB_RPTR), ring_at, gpu_fence);
	step = 0;                               /* (no pauses, nothing new: the steps below are a boot's, done again) */
	newest_step = 0;
	pause_ms = 0;
	wr(gpu.base + CP_SQE_CNTL, 3);
	wr(gmu.base + A7_GMU_PWR_COL_KEEPALIVE, 1);
	core_bus_halt();
	wr(gpu.base + A7_RBBM_SW_RESET_CMD, 1);
	rd(gpu.base + A7_RBBM_SW_RESET_CMD);
	sleep_us(101);
	wr(gpu.base + A7_RBBM_SW_RESET_CMD, 0);
	rd(gpu.base + A7_RBBM_SW_RESET_CMD);
	sleep_us(1);
	fence_status = rd(gmu.base + GMU_AHB_FENCE_STATUS);
	wr(gmu.base + A7_GMU_AHB_FENCE_STATUS_CLR, 7);
	wr(gmu.base + GMU_AO_AHB_FENCE_CTRL, 0);
	wr(gpu.base + A7_RBBM_INT_CLEAR_CMD, rd(gpu.base + RBBM_INT_0_STATUS));
	printf("sgpu: the 3D core's half has been reset: status %#x, interrupts %#x; the GMU had dropped writes of ours: %#x\n",
	    rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), fence_status);
	core_runs = false;
	done = step_core_start() && step_core_commands();
	wr(gmu.base + A7_GMU_PWR_COL_KEEPALIVE, 0);
	step = was_step;
	newest_step = was_newest;
	pause_ms = was_pause;
	if (!done) {
		printf("sgpu: THE 3D CORE DOES NOT TAKE COMMANDS AGAIN after the reset: it is given up for this run\n");
		return false;
	}
	*(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT) = gpu_fence;
	to_memory(core_scratch.memory + GPU_FENCE_AT, 4);
	gpu_timed = gpu_fence;
	printf("sgpu: THE 3D CORE IS RECOVERED (%u time%s in this run); submissions up to %u are given up as done\n",
	    recoveries, recoveries == 1 ? "" : "s", gpu_fence);
	return true;
}

/* ---- all of it ------------------------------------------------------------------------------------------ */

static bool (*const steps[])(void) = {
	step_outside, step_fuses, step_gpucc, step_cx_power, step_clocks, step_first_words, step_trustzone, step_smmu,
	step_smmu_context, step_gmu_memory, step_gmu_space, step_gmu_firmware, step_rpmh, step_gmu_start, step_hfi_start,
	step_gmu_talk, step_core_power, step_core_space, step_zap_load, step_core_start, step_core_commands,
	step_frame, step_triangle,
};
#define STEP_CORE_COMMANDS      21              /* the last of the steps that gpu=1 makes: the 3D core takes commands */
static uint32_t keep;                   /* boot-arg sgpu_keep=1: what the steps turned on stays on */

/* Steps 1 to `last`; true when all of them were done. */
static bool
sgpu_run(uint32_t last)
{
	printf("sgpu: Adreno 750 of %s: steps 1 to %u (a pause of %u ms before each access of steps %u to %u)\n",
	    redwood_phone(), last, pause_ms, first_new_step, newest_step);
	for (step = 1; step <= last; step++) {
		if (!steps[step - 1]()) {
			printf("sgpu: step %u did not find what it needs: stopping here\n", step);
			return false;
		}
	}
	printf("sgpu: steps 1 to %u done\n", last);
	return true;
}

static void
sgpu_thread(void *parameter, __unused wait_result_t result)
{
	sgpu_run((uint32_t)(uintptr_t)parameter);
	if (!keep) {
		undo();
	}
	thread_terminate(current_thread());
}

/*
 * Boot-arg gpu=1: the GPU started and kept for the system to draw with -- steps 1 to 21, no pauses, nothing
 * said before each access -- here, before the system goes on starting, so that the service is there before
 * the first program looks for it. If a step fails, everything is undone and the system draws with its
 * software renderer.
 */
static void
gpu_start(void)
{
	pause_ms = 0;
	newest_step = 0;                        /* (no step is new) */
	first_new_step = 1;
	if (sgpu_run(STEP_CORE_COMMANDS) && core_runs) {
		gpu_keep();
		return;
	}
	undo();
	printf("sgpu: THE GPU IS NOT STARTED: step %u failed; the system draws with its software renderer\n", step);
}

void
shennong_adreno_init(void)
{
	uint32_t last = 0, by_itself = 0;
	thread_t thread;

	PE_parse_boot_argn("gpu", &by_itself, sizeof(by_itself));
	if ((!PE_parse_boot_argn("sgpu", &last, sizeof(last)) || last == 0) && !by_itself) {
		return;
	}
	if (strcmp(redwood_phone(), "shennong") != 0) {
		if (!by_itself) {
			printf("sgpu: this is \"%s\", not shennong: the GPU steps are for the Xiaomi 14 Pro only\n", redwood_phone());
		}
		return;
	}
	PE_parse_boot_argn("sgpu_dcvs", &dcvs_on, sizeof(dcvs_on));
	if (PE_parse_boot_argn("sgpu_speed", &speed_index, sizeof(speed_index)) && (speed_index == 0 || speed_index >= GPU_SPEEDS)) {
		speed_index = 1;
	}
	if (by_itself) {
		gpu_start();
		return;
	}
	if (last > sizeof(steps) / sizeof(steps[0])) {
		last = sizeof(steps) / sizeof(steps[0]);
	}
	newest_step = last;
	if (!PE_parse_boot_argn("sgpu_new", &first_new_step, sizeof(first_new_step)) || first_new_step == 0 ||
	    first_new_step > last) {
		first_new_step = last;
	}
	PE_parse_boot_argn("sgpu_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("sgpu_keep", &keep, sizeof(keep));
	if (kernel_thread_start(sgpu_thread, (void *)(uintptr_t)last, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
