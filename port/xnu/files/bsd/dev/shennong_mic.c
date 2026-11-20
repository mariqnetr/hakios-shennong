/*
 * The microphones of the Xiaomi 14 Pro (shennong, SM8650), through the audio DSP. Every address and number
 * below belongs to this phone: its device tree (lpass-cdc with tx-macro@6AE0000 and va-macro@6D44000, the
 * VA macro's SoundWire master va_swr_master at 0x6d30000 with wcd939x-tx-slave, lpi_pinctrl@6E80000 and
 * tx_swr_clk_data_pinctrl, wcd939x_reset_active), Xiaomi's mixer paths (odm/etc/audio/sku_pineapple/
 * mixer_paths_pineapple_mtp.xml), what Android sent the DSP and what its registers were while it recorded
 * (build/android-capture/: gpr-mic-session.txt, mic-open-890-at-0x1000.bin, mic-regs/), and Linux 7.0's
 * drivers for the same blocks (sound/soc/codecs/lpass-tx-macro.c, lpass-va-macro.c, wcd939x.c,
 * drivers/soundwire/qcom.c, its SoundWire master of version 2.0).
 *
 * The four microphones are analog, on the WCD9395 codec's ADCs (Xiaomi's paths "amic1".."amic5": the main
 * one at the top is amic1, ADC1 -> SWR_MIC4 -> the TX macro's decimator 2; the bottom ones amic3 and amic4,
 * ADC3/ADC4 -> SWR_MIC8/9 -> decimators 5 and 3; the back one amic5, ADC2 -> SWR_MIC5). The codec's TX side
 * speaks SoundWire to the master of the VA macro, the samples go through the TX macro's decimators and out of
 * the codec DMA interface TX_3 (LPAIF type 1 "RXTX", index 4) into the DSP's recording graph. The DSP gives the
 * LPASS's clocks out (its PRM): Android asks for the two hardware cores and the TX macro's core clock
 * (0x30c) at 19.2 MHz before it records.
 *
 * Nothing here happens unless boot-arg mic=N asks for it, with the DSP up to its audio framework
 * (adsp_step=11, as for sound=N; redwood_adsp.c calls shennong_mic_dsp_up() then):
 *
 *   mic=1    a look: the two cores voted for and the TX macro's core clock asked of the PRM, then -- only if
 *            all three were given -- the registers of the TX macro, of the VA macro, of the SoundWire master,
 *            of its four LPASS pins and of the codec's reset pin read (nothing written to any of them); then
 *            the clock and the votes given back. Each block is named before it is read and the run waits
 *            mic_pause ms (default 500) so that the loader's log has it if the read hangs the bus.
 *
 *   mic=2    ... and the codec brought up far enough to answer on SoundWire, nothing of it set: its supplies as
 *            the boot chain leaves them (L15B 1.8 V, BOB1, L3C 1.2 V: nothing of the PMICs is touched), its reset
 *            released (gpio107 low 20 us, then high, 16 mA), the master's pins to their active state (LPASS gpio0,
 *            1, 2, 14: function 1, 4 mA, slew rate 1; the clock without a pull, the data with a keeper), the VA
 *            macro's clocks (its fs-gen sequence and its SoundWire clock with a reset), the master started as
 *            qcom_swrm_init() starts a v2 master (50 rows x 16 columns, auto-enumeration; its interrupts left off:
 *            polled), the codec's TX device waited for (ID 0x000e01170223) and its chip ID read over SoundWire.
 *            Then all of it undone, in reverse: the codec back in reset, pins and registers as they were found.
 *            Every write is said and read back; one that does not read back stops it there.
 *
 * What is asked for is given back at the end, and before the DSP is shut down (shennong_mic_dsp_down()).
 */
#include <kern/clock.h>
#include <kern/debug.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_audio.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static LCK_GRP_DECLARE(mic_lock_group, "shennong-mic");
static LCK_MTX_DECLARE(mic_lock, &mic_lock_group);      /* what is asked for, and given back */

static uint32_t level, pause_ms = 500;
static bool running;

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

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

/*
 * The blocks. Each mapped once, by the 4 KB page it needs (the kernel's I/O maps are never given back):
 * only the pages read here.
 */
struct block {
	const char *name;
	uint32_t phys;
	vm_offset_t page[4];            /* the first four 4 KB pages from phys, mapped when first read */
};

static struct block tx_macro = { "the TX macro", 0x06ae0000, { 0 } };
static struct block va_macro = { "the VA macro", 0x06d44000, { 0 } };
static struct block swr = { "the SoundWire master (VA)", 0x06d30000, { 0 } };
static struct block swr_v2 = { "the SoundWire master (VA), its v2 registers", 0x06d35000, { 0 } };
static struct block lpi = { "the LPASS pins", 0x06e80000, { 0 } };      /* (only page 0 used: one pin per 4 KB) */
static struct block lpi_14 = { "the LPASS pin 14", 0x06e8e000, { 0 } };
static struct block tlmm_107 = { "the pin controller's gpio107", 0x0f16b000, { 0 } };

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	unsigned p = offset / 0x1000;
	uint32_t value;

	if (p >= 4) {
		say("mic: %s %s (%#x): beyond what is mapped\n", b->name, name, b->phys + offset);
		return 0;
	}
	if (b->page[p] == 0) {
		b->page[p] = ml_io_map(b->phys + 0x1000 * p, 0x1000);
	}
	value = rd(b->page[p] + offset % 0x1000);
	say("mic: %s %s (%#x) = %#x\n", b->name, name, b->phys + offset, value);
	return value;
}

static void
announce(const char *what)
{
	say("mic: about to read %s\n", what);
	if (pause_ms != 0) {
		sleep_ms(pause_ms);
	}
}

/* ---- the DSP's PRM: the cores voted for, the TX macro's core clock ---------------------------------- */

#define PRM_PORT                        2
#define GPR_BASIC_RSP_RESULT            0x02001005u
#define GPR_WAIT_MS                     1000
#define PRM_CMD_REQUEST_HW_RSC          0x0100100fu
#define PRM_CMD_RELEASE_HW_RSC          0x01001010u
#define PRM_CMD_RSP_REQUEST_HW_RSC      0x02001002u
#define PRM_CMD_RSP_RELEASE_HW_RSC      0x02001003u
#define PARAM_ID_RSC_AUDIO_HW_CLK       0x0800102cu
#define PARAM_ID_RSC_HW_CORE            0x08001032u
#define HW_CORE_LPASS                   1
#define HW_CORE_DCODEC                  2
#define CLOCK_TX_CORE                   0x30c   /* LPASS_CLK_ID_TX_CORE_MCLK: what Android asked for (gpr-mic-session.txt) */
#define CLOCK_HZ                        19200000
#define CLOCK_ATTRIBUTE                 1       /* (as Android: "couple no") */

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "no error", "failed", "bad parameter", "unsupported", "bad version", "unexpected",
		                             "panic", "no resource", "bad handle", "already", "not ready", "pending", "busy",
		                             "aborted", "continue", "immediate", "not implemented", "needs more", "no memory",
		                             "does not exist" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}

/* A PRM command with one parameter inline (apm_cmd_header: address 0, map 0, the parameters' size). */
static bool
prm(const char *what, bool request, uint32_t param, const uint32_t *data, uint32_t words)
{
	uint32_t packet[4 + 4 + 8];
	struct redwood_apr_answer answer;
	uint32_t opcode = request ? PRM_CMD_REQUEST_HW_RSC : PRM_CMD_RELEASE_HW_RSC;
	uint32_t wanted = request ? PRM_CMD_RSP_REQUEST_HW_RSC : PRM_CMD_RSP_RELEASE_HW_RSC;
	uint32_t size = 4 * words;              /* (not padded: as Android sends it) */

	bzero(packet, sizeof(packet));
	packet[3] = 16 + size;
	packet[4] = PRM_PORT;                   /* the module instance: the PRM itself */
	packet[5] = param;
	packet[6] = size;
	memcpy(&packet[8], data, 4 * words);
	say("mic: > the PRM: %s\n", what);
	answer.opcode = 0;
	if (!redwood_adsp_gpr(PRM_PORT, 0, opcode, packet, 16 + 16 + size, GPR_WAIT_MS, &answer)) {
		say("mic: %s: NO ANSWER%s\n", what, answer.opcode == REDWOOD_GPR_NOT_SENT ? " (NOT SENT)" : "");
		return false;
	}
	if (answer.opcode == GPR_BASIC_RSP_RESULT && answer.words[1] != 0) {
		say("mic: %s: REFUSED: status %u (%s)\n", what, answer.words[1], dsp_error(answer.words[1]));
		return false;
	}
	if (answer.opcode == GPR_BASIC_RSP_RESULT || answer.opcode == wanted) {
		say("mic: %s: done (opcode %#x: %#x %#x)\n", what, answer.opcode, answer.words[0], answer.words[1]);
		return true;
	}
	say("mic: %s: AN ANSWER NOT LOOKED FOR: opcode %#x: %#x %#x\n", what, answer.opcode, answer.words[0], answer.words[1]);
	return false;
}

static bool voted_lpass, voted_dcodec, clock_on;

static bool
hw_core(uint32_t core, bool on)
{
	char what[64];

	snprintf(what, sizeof(what), "a vote for hardware core %u (%s), %s", core, core == HW_CORE_LPASS ? "LPASS" : "DCODEC",
	    on ? "asked for" : "given back");
	return prm(what, on, PARAM_ID_RSC_HW_CORE, &core, 1);
}

static bool
tx_core_clock(bool on)
{
	uint32_t data[5] = { 1, CLOCK_TX_CORE, CLOCK_HZ, CLOCK_ATTRIBUTE, 0 };

	return prm(on ? "the TX macro's core clock (0x30c) at 19.2 MHz" : "the TX macro's core clock, given back", on,
	           PARAM_ID_RSC_AUDIO_HW_CLK, data, on ? 5 : 2);
}

static bool
clocks_on(void)
{
	voted_lpass = hw_core(HW_CORE_LPASS, true);
	voted_dcodec = voted_lpass && hw_core(HW_CORE_DCODEC, true);
	clock_on = voted_dcodec && tx_core_clock(true);
	return clock_on;
}

static void
clocks_off(void)
{
	if (clock_on) {
		(void)tx_core_clock(false);
		clock_on = false;
	}
	if (voted_dcodec) {
		(void)hw_core(HW_CORE_DCODEC, false);
		voted_dcodec = false;
	}
	if (voted_lpass) {
		(void)hw_core(HW_CORE_LPASS, false);
		voted_lpass = false;
	}
}

/* ---- mic=1: a look ------------------------------------------------------------------------------- */

static void
look(void)
{
	static const struct { const char *name; uint32_t offset; } tx[] = {
		/* lpass-tx-macro.c; the values Android had while recording in mic-regs/rec1 (the lpass-cdc map's 0x0000..) */
		{ "CLK_RST_CTRL_MCLK_CONTROL", 0x000 }, { "CLK_RST_CTRL_FS_CNT_CONTROL", 0x004 },
		{ "CLK_RST_CTRL_SWR_CONTROL", 0x008 }, { "TOP_CSR_TOP_CFG0", 0x080 }, { "TOP_CSR_SWR_CTRL", 0x088 },
		{ "TOP_CSR_FREQ_MCLK", 0x090 }, { "TOP_CSR_SWR_MIC0_CTL", 0x0c0 }, { "TOP_CSR_SWR_MIC1_CTL", 0x0c4 },
		{ "INP_MUX_ADC_MUX0_CFG0", 0x100 }, { "INP_MUX_ADC_MUX2_CFG0", 0x108 }, { "TX0_TX_PATH_CTL", 0x400 },
		{ "TX2_TX_PATH_CTL", 0x500 },
	}, va[] = {
		{ "CLK_RST_CTRL_MCLK_CONTROL", 0x000 }, { "CLK_RST_CTRL_FS_CNT_CONTROL", 0x004 },
		{ "CLK_RST_CTRL_SWR_CONTROL", 0x008 }, { "TOP_CSR_TOP_CFG0", 0x080 },
	}, master[] = {
		/* drivers/soundwire/qcom.c */
		{ "COMP_HW_VERSION", 0x000 }, { "COMP_CFG", 0x004 }, { "COMP_STATUS", 0x014 }, { "COMP_PARAMS", 0x100 },
		{ "COMP_MASTER_ID", 0x104 }, { "ENUMERATOR_CFG", 0x500 }, { "MCP_FRAME_CTRL_BANK0", 0x101c },
		{ "MCP_BUS_CTRL", 0x1044 }, { "MCP_CFG", 0x1048 }, { "MCP_STATUS", 0x104c }, { "MCP_SLV_STATUS", 0x1090 },
	}, master_v2[] = {
		{ "INTERRUPT_STATUS (v2)", 0x000 }, { "INTERRUPT_CPU_EN (v2)", 0x004 }, { "CMD_FIFO_STATUS (v2)", 0x050 },
		{ "CLK_CTRL (v2)", 0x060 }, { "LINK_STATUS (v2)", 0x064 },
	};
	static const unsigned pins[] = { 0, 1, 2 };

	announce("the TX macro (0x6ae0000)");
	for (size_t i = 0; i < sizeof(tx) / sizeof(tx[0]); i++) {
		(void)get(&tx_macro, tx[i].name, tx[i].offset);
	}
	announce("the SoundWire master (0x6d30000)");
	for (size_t i = 0; i < sizeof(master) / sizeof(master[0]); i++) {
		(void)get(&swr, master[i].name, master[i].offset);
	}
	for (size_t i = 0; i < sizeof(master_v2) / sizeof(master_v2[0]); i++) {
		(void)get(&swr_v2, master_v2[i].name, master_v2[i].offset);
	}
	announce("the LPASS pins 0, 1, 2, 14 (the SoundWire clock and data) and the codec's reset, gpio107");
	for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		char name[48];

		snprintf(name, sizeof(name), "gpio%u config", pins[i]);
		(void)get(&lpi, name, 0x1000 * pins[i]);
		snprintf(name, sizeof(name), "gpio%u in/out", pins[i]);
		(void)get(&lpi, name, 0x1000 * pins[i] + 4);
	}
	(void)get(&lpi_14, "gpio14 config", 0);
	(void)get(&lpi_14, "gpio14 in/out", 4);
	(void)get(&tlmm_107, "config", 0);
	(void)get(&tlmm_107, "in/out", 4);
	/* (last: Xiaomi's VA macro runs from the TX macro's clock, va-clk-mux-select; if its clock is off, this may hang) */
	announce("the VA macro (0x6d44000)");
	for (size_t i = 0; i < sizeof(va) / sizeof(va[0]); i++) {
		(void)get(&va_macro, va[i].name, va[i].offset);
	}
}

/* ---- writing: every write said and read back; one that does not read back stops the bring-up ---------- */

static bool broken;

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
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

static vm_offset_t
at(struct block *b, uint32_t offset)
{
	unsigned p = offset / 0x1000;

	if (p >= 4) {
		panic("shennong_mic: %s offset %#x beyond what is mapped", b->name, offset);
	}
	if (b->page[p] == 0) {
		b->page[p] = ml_io_map(b->phys + 0x1000 * p, 0x1000);
	}
	return b->page[p] + offset % 0x1000;
}

static uint32_t
peek(struct block *b, uint32_t offset)
{
	return rd(at(b, offset));
}

/* A register written and, in the bits of `check', read back (0: not read back: bits that clear themselves). */
static bool
put(struct block *b, const char *name, uint32_t offset, uint32_t value, uint32_t check)
{
	uint32_t back;

	if (broken) {
		return false;
	}
	wr(at(b, offset), value);
	back = rd(at(b, offset));
	if ((back & check) != (value & check)) {
		say("mic: %s %s (%#x) <- %#x, but it reads %#x: STOPPING HERE\n", b->name, name, b->phys + offset, value, back);
		broken = true;
		return false;
	}
	say("mic: %s %s (%#x) <- %#x%s\n", b->name, name, b->phys + offset, value, check ? ", reads back" : "");
	return true;
}

static bool
put_bits(struct block *b, const char *name, uint32_t offset, uint32_t mask, uint32_t value)
{
	return put(b, name, offset, (peek(b, offset) & ~mask) | (value & mask), mask);
}

/* ---- mic=2: the codec's reset (gpio107, wcd939x_reset_active/sleep) ----------------------------------- */

#define TLMM_CFG                0x0
#define TLMM_IO                 0x4
#define TLMM_OUT                (1u << 1)
static uint32_t found_reset_cfg, found_reset_io;
static bool reset_taken;

static bool
reset_release(void)
{
	found_reset_cfg = peek(&tlmm_107, TLMM_CFG);
	found_reset_io = peek(&tlmm_107, TLMM_IO);
	reset_taken = true;
	/* wcd939x_reset(): low, 20 us, high, 20 us; the pin: gpio, 16 mA ((16 / 2 - 1) << 6), no pull, an output */
	if (!put(&tlmm_107, "out: low (the codec in reset)", TLMM_IO, 0, TLMM_OUT) ||
	    !put(&tlmm_107, "config: gpio, 16 mA, no pull, output", TLMM_CFG, 0x3c0, 0x3ff)) {
		return false;
	}
	spin_us(30);
	if (!put(&tlmm_107, "out: high (the codec out of reset)", TLMM_IO, TLMM_OUT, TLMM_OUT)) {
		return false;
	}
	spin_us(30);
	return true;
}

static void
reset_restore(void)
{
	if (!reset_taken) {
		return;
	}
	broken = false;         /* (what is put back is put back whatever went before) */
	(void)put(&tlmm_107, "out: low (the codec in reset)", TLMM_IO, 0, TLMM_OUT);
	(void)put(&tlmm_107, "config, as it was found", TLMM_CFG, found_reset_cfg, 0x3ff);
	(void)put(&tlmm_107, "out, as it was found", TLMM_IO, found_reset_io & TLMM_OUT, TLMM_OUT);
	reset_taken = false;
}

/* ---- the SoundWire pins: LPASS gpio0 (clock), 1, 2, 14 (data): tx_swr_*_active --------------------------- */

/*
 * [1:0] pull (0 none, 2 keeper), [5:2] function, [8:6] drive (mA / 2 - 1), [9] output, [12:11] slew rate
 * (SM8650: in the same register, LPI_FLAG_SLEW_RATE_SAME_REG, slew offset 11): function 1, 4 mA, slew rate 1;
 * the clock without a pull, the data with a keeper (bias-bus-hold).
 */
static const struct { unsigned pin; uint32_t pull; const char *name; } swr_pins[] = {
	{ 0, 0, "gpio0 (SoundWire clock)" }, { 1, 2, "gpio1 (data 0)" }, { 2, 2, "gpio2 (data 1)" }, { 14, 2, "gpio14 (data 2)" },
};
#define SWR_PINS                (sizeof(swr_pins) / sizeof(swr_pins[0]))
#define LPI_CFG_BITS            0x1bffu         /* pull, function, drive, output, slew rate */
static uint32_t found_pin_cfg[SWR_PINS];
static bool pins_taken;

static struct block *
pin_block(unsigned pin, uint32_t *offset)
{
	if (pin == 14) {
		*offset = 0;
		return &lpi_14;
	}
	*offset = 0x1000 * pin;
	return &lpi;
}

static bool
pins_on(void)
{
	for (unsigned i = 0; i < SWR_PINS; i++) {
		uint32_t offset;
		struct block *b = pin_block(swr_pins[i].pin, &offset);

		found_pin_cfg[i] = peek(b, offset);
	}
	pins_taken = true;
	for (unsigned i = 0; i < SWR_PINS; i++) {
		uint32_t offset;
		struct block *b = pin_block(swr_pins[i].pin, &offset);
		uint32_t cfg = (found_pin_cfg[i] & ~LPI_CFG_BITS) | swr_pins[i].pull | 1u << 2 | 1u << 6 | 1u << 11;

		if (!put(b, swr_pins[i].name, offset, cfg, LPI_CFG_BITS)) {
			return false;
		}
	}
	return true;
}

static void
pins_restore(void)
{
	if (!pins_taken) {
		return;
	}
	broken = false;
	for (unsigned i = 0; i < SWR_PINS; i++) {
		uint32_t offset;
		struct block *b = pin_block(swr_pins[i].pin, &offset);

		(void)put(b, "a SoundWire pin, as it was found", offset, found_pin_cfg[i], LPI_CFG_BITS);
	}
	pins_taken = false;
}

/* ---- the VA macro's clocks and its SoundWire clock (lpass-va-macro.c: fsgen_gate_enable()) ---------------- */

#define VA_MCLK_CONTROL         0x000
#define VA_FS_CNT_CONTROL       0x004
#define VA_SWR_CONTROL          0x008
#define VA_TOP_CFG0             0x080
static bool va_clocks_taken;

static bool
va_clocks_on(void)
{
	va_clocks_taken = true;
	/* va_clk_rsc_fs_gen_request(): the MCLK, the frame counter cleared and on, its broadcast */
	return put_bits(&va_macro, "MCLK_CONTROL: on", VA_MCLK_CONTROL, 0x01, 0x01) &&
	       put_bits(&va_macro, "FS_CNT_CONTROL: on, cleared", VA_FS_CNT_CONTROL, 0x03, 0x03) &&
	       put_bits(&va_macro, "FS_CNT_CONTROL: on", VA_FS_CNT_CONTROL, 0x03, 0x01) &&
	       put_bits(&va_macro, "TOP_CFG0: FS broadcast", VA_TOP_CFG0, 0x02, 0x02) &&
	       /* (probe: the SoundWire block reset, its clock on, the reset over) */
	       put_bits(&va_macro, "SWR_CONTROL: reset", VA_SWR_CONTROL, 0x02, 0x02) &&
	       put_bits(&va_macro, "SWR_CONTROL: clock on", VA_SWR_CONTROL, 0x01, 0x01) &&
	       put_bits(&va_macro, "SWR_CONTROL: reset over", VA_SWR_CONTROL, 0x02, 0x00);
}

static void
va_clocks_off(void)
{
	if (!va_clocks_taken) {
		return;
	}
	broken = false;
	(void)put_bits(&va_macro, "SWR_CONTROL: clock off", VA_SWR_CONTROL, 0x01, 0x00);
	(void)put_bits(&va_macro, "MCLK_CONTROL: off", VA_MCLK_CONTROL, 0x01, 0x00);
	(void)put_bits(&va_macro, "FS_CNT_CONTROL: off", VA_FS_CNT_CONTROL, 0x01, 0x00);
	(void)put_bits(&va_macro, "TOP_CFG0: no FS broadcast", VA_TOP_CFG0, 0x02, 0x00);
	va_clocks_taken = false;
}

/* ---- the SoundWire master, version 2.1 (drivers/soundwire/qcom.c: qcom_swrm_init(), its v2 layout) ------- */

#define SWRM_COMP_CFG                   0x0004
#define SWRM_LINK_MANAGER_EE            0x0018
#define SWRM_CMD_FIFO_CMD               0x0308
#define SWRM_CMD_FIFO_CFG               0x0314
#define SWRM_ENUMERATOR_CFG             0x0500
#define SWRM_ENUMERATOR_SLAVE_DEV_ID_1(m) (0x0530 + 0x8 * (m))
#define SWRM_ENUMERATOR_SLAVE_DEV_ID_2(m) (0x0534 + 0x8 * (m))
#define SWRM_MCP_FRAME_CTRL_BANK(m)     (0x101c + 0x40 * (m))
#define SWRM_MCP_CFG                    0x1048
#define SWRM_MCP_STATUS                 0x104c
#define SWRM_MCP_SLV_STATUS             0x1090
/* (the v2 registers, in the swr_v2 block: 0x5000 on) */
#define SWRM_V2_INTERRUPT_STATUS        0x000
#define SWRM_V2_INTERRUPT_CPU_EN        0x004
#define SWRM_V2_INTERRUPT_CLEAR         0x008
#define SWRM_V2_CMD_FIFO_WR_CMD         0x020
#define SWRM_V2_CMD_FIFO_RD_CMD         0x024
#define SWRM_V2_CMD_FIFO_RD_FIFO        0x040
#define SWRM_V2_CMD_FIFO_STATUS         0x050
#define SWRM_V2_CLK_CTRL                0x060
#define SWRM_V2_LINK_STATUS             0x064
#define SWR_ROWS_50                     1       /* sdw_rows[]: 48, 50, 60, ... */
#define SWR_COLS_16                     7       /* sdw_cols[]: 2, 4, ... 16 (swrm_v2_0_data: 50 x 16) */
#define CODEC_TX_ID                     0x000e01170223ull       /* the tree's wcd939x-tx-slave, reg <0x0e 0x1170223> */

static bool master_taken;
static uint8_t read_id;

static bool
master_on(void)
{
	bool linked = false;
	uint32_t cfg;

	master_taken = true;
	cfg = (peek(&swr, SWRM_MCP_CFG) & ~0x3e0000u) | 0x1fu << 17;
	if (!put(&swr, "MCP_FRAME_CTRL_BANK0: 50 rows, 16 columns", SWRM_MCP_FRAME_CTRL_BANK(0), SWR_ROWS_50 << 3 | SWR_COLS_16,
	    0xff) ||
	    !put(&swr, "ENUMERATOR_CFG: auto", SWRM_ENUMERATOR_CFG, 1, 1) ||
	    !put(&swr, "MCP_CFG: 31 commands without pings", SWRM_MCP_CFG, cfg, 0x3e0000) ||
	    !put(&swr, "LINK_MANAGER_EE: the CPU", SWRM_LINK_MANAGER_EE, 1, 0) ||
	    !put(&swr_v2, "CLK_CTRL: the clock started", SWRM_V2_CLK_CTRL, 1, 0) ||
	    !put(&swr, "CMD_FIFO_CFG: 7 retries, go on past an ignored command", SWRM_CMD_FIFO_CFG, 7u | 1u << 31, 0) ||
	    !put(&swr, "COMP_CFG: on", SWRM_COMP_CFG, 1, 0) ||
	    !put(&swr, "COMP_CFG: interrupts as pulses", SWRM_COMP_CFG, 2, 0) ||
	    !put(&swr_v2, "INTERRUPT_CLEAR", SWRM_V2_INTERRUPT_CLEAR, 0xffffffff, 0) ||
	    /* (the CPU's interrupts left off: nobody takes them, we poll) */
	    !put(&swr, "COMP_CFG: on, pulses", SWRM_COMP_CFG, 3, 0)) {
		return false;
	}
	for (int i = 0; i < 100 && !linked; i++) {
		linked = peek(&swr_v2, SWRM_V2_LINK_STATUS) & 1;
		if (!linked) {
			spin_us(510);
		}
	}
	say("mic: the SoundWire link %s (LINK_STATUS %#x, MCP_STATUS %#x, interrupts %#x)\n", linked ? "IS UP" : "DID NOT COME UP",
	    peek(&swr_v2, SWRM_V2_LINK_STATUS), peek(&swr, SWRM_MCP_STATUS), peek(&swr_v2, SWRM_V2_INTERRUPT_STATUS));
	return linked;
}

static void
master_off(void)
{
	if (!master_taken) {
		return;
	}
	broken = false;
	(void)put(&swr, "COMP_CFG: off", SWRM_COMP_CFG, 0, 0);
	(void)put(&swr_v2, "CLK_CTRL: stopped", SWRM_V2_CLK_CTRL, 0, 0);
	(void)put(&swr, "ENUMERATOR_CFG: off", SWRM_ENUMERATOR_CFG, 0, 0);
	(void)put(&swr_v2, "INTERRUPT_CLEAR", SWRM_V2_INTERRUPT_CLEAR, 0xffffffff, 0);
	master_taken = false;
}

/* A register of a device on the bus read (SWRM_REG_VAL_PACK, one byte): false if no answer of this command's. */
static bool
swr_read(uint8_t device, uint16_t reg, uint8_t *value)
{
	uint32_t answer = 0;

	read_id = read_id < 14 ? read_id + 1 : 0;
	wr(at(&swr_v2, SWRM_V2_CMD_FIFO_RD_CMD), reg | (uint32_t)read_id << 16 | (uint32_t)device << 20 | 1u << 24);
	spin_us(255);
	for (int tries = 0; tries < 4; tries++) {
		for (int i = 0; i < 20 && ((peek(&swr_v2, SWRM_V2_CMD_FIFO_STATUS) >> 16) & 0x1f) == 0; i++) {
			spin_us(500);
		}
		answer = peek(&swr_v2, SWRM_V2_CMD_FIFO_RD_FIFO);
		if (((answer >> 8) & 0xf) == read_id) {
			*value = (uint8_t)answer;
			return true;
		}
		spin_us(505);
		wr(at(&swr, SWRM_CMD_FIFO_CMD), 1);     /* (flushed, and asked again, as qcom.c does) */
		wr(at(&swr_v2, SWRM_V2_CMD_FIFO_RD_CMD), reg | (uint32_t)read_id << 16 | (uint32_t)device << 20 | 1u << 24);
		spin_us(255);
	}
	say("mic: SoundWire: device %u register %#x: no answer (fifo status %#x, last %#x, interrupts %#x)\n", device, reg,
	    peek(&swr_v2, SWRM_V2_CMD_FIFO_STATUS), answer, peek(&swr_v2, SWRM_V2_INTERRUPT_STATUS));
	return false;
}

static uint8_t codec_device;

/* The codec's TX device, enumerated: its device number, 0 if it did not show up within a second. */
static uint8_t
codec_wait(void)
{
	for (int waited = 0; waited < 1000; waited++) {
		for (uint8_t d = 1; d <= 11; d++) {
			uint64_t id = (uint64_t)peek(&swr, SWRM_ENUMERATOR_SLAVE_DEV_ID_2(d)) << 32 |
			    peek(&swr, SWRM_ENUMERATOR_SLAVE_DEV_ID_1(d));
			uint32_t status = (peek(&swr, SWRM_MCP_SLV_STATUS) >> (2 * d)) & 3;

			if ((id & 0xffffffffffffull) == CODEC_TX_ID && (status == 1 || status == 2)) {
				say("mic: THE CODEC'S TX DEVICE IS ON THE BUS: device %u (ID %#llx, status %u) after %d ms\n", d,
				    id & 0xffffffffffffull, status, waited);
				return d;
			}
		}
		sleep_ms(1);
	}
	say("mic: the codec's TX device did not show up within a second; the master has seen:\n");
	for (uint8_t d = 0; d <= 11; d++) {
		uint64_t id = (uint64_t)peek(&swr, SWRM_ENUMERATOR_SLAVE_DEV_ID_2(d)) << 32 | peek(&swr, SWRM_ENUMERATOR_SLAVE_DEV_ID_1(d));

		if (id != 0) {
			say("mic:   device %u: ID %#llx\n", d, id);
		}
	}
	say("mic:   slave status %#x, interrupts %#x\n", peek(&swr, SWRM_MCP_SLV_STATUS), peek(&swr_v2, SWRM_V2_INTERRUPT_STATUS));
	return 0;
}

static void
codec_look(void)
{
	static const struct { const char *name; uint16_t reg; } regs[] = {
		{ "SCP DEVICE_ID_0", 0x0050 }, { "SCP DEVICE_ID_1", 0x0051 }, { "SCP DEVICE_ID_2", 0x0052 },
		{ "DIGITAL_CHIP_ID0", 0x3401 }, { "DIGITAL_CHIP_ID1", 0x3402 }, { "DIGITAL_CHIP_ID2", 0x3403 },
		{ "DIGITAL_CHIP_ID3", 0x3404 },
	};

	for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t value;

		if (swr_read(codec_device, regs[i].reg, &value)) {
			say("mic: the codec's %s (%#x) = %#x\n", regs[i].name, regs[i].reg, value);
		}
	}
}

/* mic=2: the codec up far enough to answer on SoundWire, and its chip ID read; then all of it undone. */
static void
bring_up(void)
{
	say("mic: mic=2: the codec out of reset, the SoundWire pins, the VA macro's clocks, the master, the codec waited for\n");
	broken = false;
	if (reset_release() && pins_on() && va_clocks_on() && master_on()) {
		codec_device = codec_wait();
		if (codec_device != 0) {
			codec_look();
		}
	}
}

static void
take_down(void)
{
	master_off();
	va_clocks_off();
	pins_restore();
	reset_restore();
}

/* ---- the thread ------------------------------------------------------------------------------------ */

static void
mic_thread(__unused void *parameter, __unused wait_result_t result)
{
	lck_mtx_lock(&mic_lock);
	say("mic: mic=%u: the cores and the TX macro's core clock asked of the DSP's PRM\n", level);
	if (clocks_on()) {
		say("mic: THE PRM HAS GIVEN THE CORES AND THE TX CORE CLOCK\n");
		if (level == 1) {
			look();
			say("mic: the look is over\n");
		} else {
			bring_up();
			take_down();
			say("mic: mic=2: all of it undone (codec in reset, pins, clocks and the master as they were found)\n");
		}
	} else {
		say("mic: the PRM did not give all three: nothing of the LPASS is read\n");
	}
	clocks_off();
	running = false;
	say("mic: mic=%u done; all given back\n", level);
	lck_mtx_unlock(&mic_lock);
	thread_terminate(current_thread());
}

/* The DSP's audio framework is up (redwood_adsp.c, step 11): boot-arg mic=N begins. */
void
shennong_mic_dsp_up(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("mic", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 2) {
		say("mic: mic=%u: only mic=1 and mic=2 are there yet; taken as 2\n", level);
		level = 2;
	}
	PE_parse_boot_argn("mic_pause", &pause_ms, sizeof(pause_ms));
	running = true;
	if (kernel_thread_start(mic_thread, NULL, &thread) != KERN_SUCCESS) {
		running = false;
		say("mic: no thread\n");
		return;
	}
	thread_deallocate(thread);
}

/* Before the DSP is shut down at the run's end: what was asked of it given back (after the look, if it runs still). */
void
shennong_mic_dsp_down(void)
{
	for (int waited = 0; waited < 100 && running; waited++) {
		sleep_ms(50);
	}
	lck_mtx_lock(&mic_lock);
	take_down();
	clocks_off();
	lck_mtx_unlock(&mic_lock);
}
