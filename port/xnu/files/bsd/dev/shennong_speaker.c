/*
 * The speakers of the Xiaomi 14 Pro (shennong): their two amplifiers, a first look. Every address
 * below belongs to this phone, from its device tree, and nothing here is touched unless a boot-arg
 * asks for it.
 *
 * Each speaker has an amplifier of its own, a Cirrus Logic CS35L43 ("cirrus,cs35l43"), set up over
 * I2C -- the bus of the serial engine 8 (QUP2 SE0, i2c@880000: GPIO 0 data, 1 clock, pull-ups, on
 * which the device tree has these two and nothing else) -- and fed by the SoC's second I2S
 * interface (sec_tdm_pinctrl: GPIO 121 bit clock, 123 frame sync, 124 data out, 122 data in),
 * which the audio DSP drives:
 *
 *   0x40   "B" (the bottom speaker), reset GPIO 85 (high = running), interrupt GPIO 98
 *   0x42   "T" (the top one), reset GPIO 111, interrupt GPIO 99
 *
 * The chip's registers are 32 bits wide at 32-bit addresses, both sent most significant byte
 * first (as Linux's regmap for the CS35L41 family: reg_bits 32, val_bits 32).
 *
 *   spk_probe=1    the pins, reading only
 *   spk_probe=2    ... and the resets released (low 2 ms, then high, as Cirrus's driver does; they
 *                  stay released until the run ends, then low again), and the first registers of
 *                  each amplifier read: the device ID (0x00), revision (0x04), and 0x08..0x10.
 *                  Nothing is written to the chips.
 *   spk_probe=3    ... and the whole bus asked: every address 0x08..0x77 called (to read, a byte
 *                  taken and not acknowledged), and who answers said -- with the resets released
 *                  and, after, held low. The device tree has 0x40 and 0x42 in "reg" but 0x28 and 0x2a
 *                  in "cirrus,i2c-addr".
 *   spk_probe=4    ... and first the two lines tested: each pulled low and let go with both read,
 *                  then both left with the pins' own pull-downs (the bus's pull-ups still win if they
 *                  are powered); the interrupt pins get pull-ups so that what they read means something.
 *
 * I2C is done by hand, as in redwood_speaker.c: a pin is pulled low or let go (the internal
 * pull-ups raise it), 100 kHz at most, the two pins taken from the serial engine for each transfer
 * and given back as they were found.
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

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static vm_offset_t tlmm = 1;           /* (no longer a mapping: the pins' pages are, pin_page()) */
static vm_offset_t pin_pages[256];

static vm_offset_t
pin_page(unsigned pin)
{
	if (pin >= 256) {
		return 0;
	}
	if (pin_pages[pin] == 0) {
		pin_pages[pin] = ml_io_map(0x0f100000UL + 0x1000UL * pin, 0x1000);     /* (TLMM_PHYS) */
	}
	return pin_pages[pin];
}
static uint32_t probe_level;
static bool released, leaving;

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

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               0x0f100000UL    /* pinctrl@f000000 + 0x100000 (pinctrl-sm8650.c) */
#define TLMM_SIZE               0x100000UL
/*
 * (the thirty-fifth run panicked mapping the whole pin controller, a megabyte: the kernel's room for mappings of
 * devices is short, and they are permanent here. Each pin's page is mapped by itself, once, when first used.)
 */
static vm_offset_t pin_page(unsigned pin);
#define GPIO_CFG(n)             (pin_page(n))                   /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (pin_page(n) + 4)               /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)
/*
 * An "eGPIO": a pin that another subsystem may own (pinctrl-msm.c, egpio_present/egpio_enable,
 * bits 11 and 12 on SM8650). Bit 11 reads 1 on such a pin, and the pin is APPS's only while bit 12
 * is set: written without it, the pin went to its other owner, and with spk_probe=4 a line "pulled
 * low" read 1 (cfg 0xa03) -- which is why nobody ever answered on the bus.
 */
#define CFG_EGPIO_PRESENT       (1u << 11)
#define CFG_EGPIO_ENABLE        (1u << 12)

/* a pin's configuration, the pin claimed for APPS when it is an eGPIO */
static void
cfg_wr(vm_offset_t address, uint32_t value)
{
	if (*(volatile uint32_t *)address & CFG_EGPIO_PRESENT) {
		value |= CFG_EGPIO_ENABLE;
	}
	*(volatile uint32_t *)address = value;
}

#define PIN_SDA                 0
#define PIN_SCL                 1

static const struct amplifier {
	const char *    name;
	uint8_t         address;
	unsigned        reset, interrupt;
} amplifiers[] = {
	{ "B (bottom)", 0x40, 85, 98 },
	{ "T (top)", 0x42, 111, 99 },
};

static void
show_pins(void)
{
	static const struct {
		unsigned pin;
		const char *what;
	} pins[] = { { PIN_SDA, "I2C data" }, { PIN_SCL, "I2C clock" }, { 85, "B reset" }, { 98, "B interrupt" }, { 111, "T reset" },
		     { 99, "T interrupt" }, { 121, "I2S bit clock" }, { 123, "I2S frame sync" }, { 124, "I2S data out" },
		     { 122, "I2S data in" } };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("speaker: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u (cfg %#x%s)\n", pins[i].pin, pins[i].what,
		    (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") :
		    "input", io & 1, cfg, (cfg & CFG_EGPIO_PRESENT) ? ((cfg & CFG_EGPIO_ENABLE) ? ", an eGPIO, APPS's" :
		    ", an eGPIO, NOT APPS's") : "");
	}
}

/* a reset pin as an output of the GPIO function, driving `high' (no pull) */
static void
reset_pin(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
	cfg_wr(GPIO_CFG(pin), CFG_OUTPUT | (3u << 6));  /* 8 mA */
}

/* ---- I2C by hand (as in redwood_speaker.c) ---------------------------------------------------------- */

static void
half_bit(void)
{
	spin_us(5);
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	cfg_wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !level_of(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

static uint32_t taken_cfg[2], taken_io[2];

static void
bus_give_back(void)
{
	wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
	wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
	wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
	wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
}

static bool
bus_take(void)
{
	taken_cfg[0] = rd(GPIO_CFG(PIN_SDA));
	taken_cfg[1] = rd(GPIO_CFG(PIN_SCL));
	taken_io[0] = rd(GPIO_IO(PIN_SDA));
	taken_io[1] = rd(GPIO_IO(PIN_SCL));
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	if (!level_of(PIN_SDA) || !level_of(PIN_SCL)) {
		bus_give_back();
		say("speaker: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA, level_of(PIN_SDA),
		    PIN_SCL, level_of(PIN_SCL));
		return false;
	}
	return true;
}

/* one 32-bit register: its address and then its value, most significant byte first */
static bool
register_read(uint8_t address, uint32_t reg, uint32_t *value)
{
	bool answered;
	uint32_t v = 0;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1));
	for (int shift = 24; answered && shift >= 0; shift -= 8) {
		answered = i2c_write((uint8_t)(reg >> shift));
	}
	if (answered) {
		i2c_start();
		answered = i2c_write((uint8_t)(address << 1 | 1));
	}
	for (int i = 0; answered && i < 4; i++) {
		v = v << 8 | i2c_read(i < 3);
	}
	i2c_stop();
	bus_give_back();
	*value = v;
	return answered;
}

/* who answers to `address' (a read: one byte taken, not acknowledged) */
static bool
answers(uint8_t address)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1 | 1));
	if (answered) {
		(void)i2c_read(false);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static void
scan(const char *when)
{
	char line[200];
	size_t at = (size_t)snprintf(line, sizeof(line), "speaker: the bus %s: answering", when);
	unsigned found = 0;

	for (uint8_t address = 0x08; address <= 0x77; address++) {
		if (answers(address)) {
			found++;
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %#04x", address);
		}
	}
	say("%s%s\n", line, found ? "" : " nobody");
}

/*
 * spk_probe=4: are the two lines ours at all? Each is pulled low by its own pin and then let go, and
 * both are read each time; then each is left as an input with the pin's own pull-DOWN, which the bus's
 * pull-ups (if they are powered) still hold high. Nobody answered at any address, and this tells a pin
 * that does not obey (or a bus held by something else) from chips that are not there or not powered.
 */
static void
line_test(void)
{
	static const unsigned pins[] = { PIN_SDA, PIN_SCL };

	taken_cfg[0] = rd(GPIO_CFG(PIN_SDA));
	taken_cfg[1] = rd(GPIO_CFG(PIN_SCL));
	taken_io[0] = rd(GPIO_IO(PIN_SDA));
	taken_io[1] = rd(GPIO_IO(PIN_SCL));
	for (unsigned i = 0; i < 2; i++) {
		line(PIN_SDA, true);
		line(PIN_SCL, true);
		spin_us(20);
		line(pins[i], false);
		spin_us(20);
		say("speaker: line test: gpio%u pulled low: gpio%u reads %u, gpio%u reads %u (cfg %#x)\n", pins[i], PIN_SDA,
		    level_of(PIN_SDA), PIN_SCL, level_of(PIN_SCL), rd(GPIO_CFG(pins[i])));
		line(pins[i], true);
		spin_us(20);
		say("speaker: line test: gpio%u let go: gpio%u reads %u, gpio%u reads %u\n", pins[i], PIN_SDA, level_of(PIN_SDA),
		    PIN_SCL, level_of(PIN_SCL));
	}
	for (unsigned i = 0; i < 2; i++) {
		cfg_wr(GPIO_CFG(pins[i]), 1);   /* input, pull-down */
	}
	spin_us(50);
	say("speaker: line test: both with the pins' pull-downs: gpio%u reads %u, gpio%u reads %u (1: the bus has powered pull-ups)\n",
	    PIN_SDA, level_of(PIN_SDA), PIN_SCL, level_of(PIN_SCL));
	bus_give_back();
	for (unsigned i = 0; i < sizeof(amplifiers) / sizeof(amplifiers[0]); i++) {
		cfg_wr(GPIO_CFG(amplifiers[i].interrupt), CFG_PULL_UP);
	}
}

/* ---- spk_probe=N ----------------------------------------------------------------------------------- */

static void
show_amplifier(const struct amplifier *a)
{
	static const uint32_t regs[] = { 0x00, 0x04, 0x08, 0x0c, 0x10 };
	uint32_t v;

	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		if (!register_read(a->address, regs[i], &v)) {
			say("speaker: %s at %#x: nobody answers (register %#x)\n", a->name, a->address, regs[i]);
			return;
		}
		say("speaker: %s at %#x: register %#010x = %#010x%s\n", a->name, a->address, regs[i], v,
		    regs[i] == 0 ? ((v & 0xffff0) == 0x35a40 ? " (a CS35L4x device ID)" : " (not the CS35L4x ID 0x35a4x expected)") : "");
	}
}

static void
probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("speaker: shennong, probe level %u: %s\n", probe_level, probe_level == 1 ? "the pins, reading only" : probe_level == 2 ?
	    "the resets released and the amplifiers asked who they are" : probe_level == 3 ? "... and every address of the bus asked" :
	    "... and the two lines of the bus tested first");
	show_pins();
	if (probe_level >= 4) {
		line_test();
	}
	if (probe_level >= 2) {
		for (unsigned i = 0; i < sizeof(amplifiers) / sizeof(amplifiers[0]); i++) {
			say("speaker: %s: reset gpio%u low for 2 ms, then high\n", amplifiers[i].name, amplifiers[i].reset);
			reset_pin(amplifiers[i].reset, false);
		}
		released = true;
		wait_ms(2);
		for (unsigned i = 0; i < sizeof(amplifiers) / sizeof(amplifiers[0]); i++) {
			reset_pin(amplifiers[i].reset, true);
		}
		wait_ms(5);
		for (unsigned i = 0; i < sizeof(amplifiers) / sizeof(amplifiers[0]); i++) {
			show_amplifier(&amplifiers[i]);
			say("speaker: %s: interrupt gpio%u reads %u (low: the chip has something to say)\n", amplifiers[i].name,
			    amplifiers[i].interrupt, rd(GPIO_IO(amplifiers[i].interrupt)) & 1);
		}
	}
	if (probe_level >= 3) {
		scan("with the resets released");
		for (unsigned i = 0; i < sizeof(amplifiers) / sizeof(amplifiers[0]); i++) {
			reset_pin(amplifiers[i].reset, false);
		}
		wait_ms(2);
		scan("with the resets held low");
		for (unsigned i = 0; i < sizeof(amplifiers) / sizeof(amplifiers[0]); i++) {
			reset_pin(amplifiers[i].reset, true);
		}
	}
	say("speaker: done\n");
	thread_terminate(current_thread());
}

/* ---- the amplifiers on, as Android leaves and starts them (sound=3) ----------------------------------- */

/* one 32-bit register written: its address and then its value, most significant byte first */
static bool
register_write(uint8_t address, uint32_t reg, uint32_t value)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1));
	for (int shift = 24; answered && shift >= 0; shift -= 8) {
		answered = i2c_write((uint8_t)(reg >> shift));
	}
	for (int shift = 24; answered && shift >= 0; shift -= 8) {
		answered = i2c_write((uint8_t)(value >> shift));
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

/*
 * The configuration Android's driver leaves the two CS35L43 in, read out of their regmaps (debugfs) after it had
 * played through them: every register of 0x2000..0x7fff but those the start below writes itself, the bottom's
 * value then the top's (they differ in which TDM slot each takes: 0x4810, 0x4820).
 */
static const struct {
	uint32_t reg, bottom, top;
} android_config[] = {
	{ 0x0201c, 0x01001010, 0x01001010 },
	{ 0x02020, 0x00000002, 0x00000002 },
	{ 0x02024, 0x00000000, 0x00000000 },
	{ 0x02028, 0x00000001, 0x00000001 },
	{ 0x0202c, 0x00000000, 0x00000000 },
	{ 0x02030, 0x00000000, 0x00000000 },
	{ 0x02034, 0x00000000, 0x00000000 },
	{ 0x02038, 0x00000000, 0x00000000 },
	{ 0x02040, 0x007ffe01, 0x007ffe01 },
	{ 0x02044, 0x42800020, 0x42800020 },
	{ 0x02418, 0x00000007, 0x00000007 },
	{ 0x0241c, 0x00000007, 0x00000007 },
	{ 0x02420, 0x00000007, 0x00000007 },
	{ 0x02424, 0x00000007, 0x00000007 },
	{ 0x0242c, 0x64000000, 0x64000000 },
	{ 0x02440, 0x00000000, 0x00000000 },
	{ 0x02900, 0x00000002, 0x00000002 },
	{ 0x02904, 0x00000010, 0x00000010 },
	{ 0x02908, 0x00000000, 0x00000000 },
	{ 0x0290c, 0x00000001, 0x00000001 },
	{ 0x02910, 0x00000000, 0x00000000 },
	{ 0x02914, 0x00000000, 0x00000000 },
	{ 0x02c08, 0x00000009, 0x00000009 },
	{ 0x02c0c, 0x00000003, 0x00000003 },
	{ 0x02c14, 0x00000000, 0x00000000 },
	{ 0x02d3c, 0x01010101, 0x01010101 },
	{ 0x02d40, 0x00000101, 0x00000101 },
	{ 0x02d60, 0x01010101, 0x01010101 },
	{ 0x02d64, 0x00000101, 0x00000101 },
	{ 0x0300c, 0x000040a4, 0x000040a4 },
	{ 0x03400, 0x00000200, 0x00000200 },
	{ 0x03800, 0x000000aa, 0x000000aa },
	{ 0x03804, 0x00000001, 0x00000001 },
	{ 0x03808, 0x00000040, 0x00000040 },
	{ 0x0380c, 0x00000003, 0x00000003 },
	{ 0x03810, 0x00002424, 0x00002424 },
	{ 0x03814, 0x00007500, 0x00007500 },
	{ 0x03818, 0x01008000, 0x01008000 },
	{ 0x0381c, 0x00000051, 0x00000051 },
	{ 0x03820, 0x00000000, 0x00000000 },
	{ 0x03830, 0x00000130, 0x00000130 },
	{ 0x03850, 0x50000802, 0x50000802 },
	{ 0x04008, 0x00000000, 0x00000000 },
	{ 0x04220, 0x00000002, 0x00000002 },
	{ 0x04224, 0x00000000, 0x00000000 },
	{ 0x04300, 0x00000000, 0x00000000 },
	{ 0x04304, 0x00000200, 0x00000200 },
	{ 0x04308, 0x00000000, 0x00000000 },
	{ 0x04804, 0x00000028, 0x00000028 },
	{ 0x04808, 0x20200000, 0x20200000 },
	{ 0x0480c, 0x00000003, 0x00000003 },
	{ 0x04810, 0x00000001, 0x00000000 },
	{ 0x04820, 0x00020401, 0x00020200 },
	{ 0x04830, 0x00000020, 0x00000020 },
	{ 0x04840, 0x00000020, 0x00000020 },
	{ 0x04c08, 0x00000037, 0x00000037 },
	{ 0x04c20, 0x00000036, 0x00000036 },
	{ 0x04c24, 0x00000000, 0x00000000 },
	{ 0x04c28, 0x00000020, 0x00000020 },
	{ 0x04c2c, 0x00000021, 0x00000021 },
	{ 0x04c40, 0x00000008, 0x00000008 },
	{ 0x04c44, 0x00000008, 0x00000008 },
	{ 0x04c48, 0x00000029, 0x00000029 },
	{ 0x04c4c, 0x00000019, 0x00000019 },
	{ 0x04c50, 0x00000018, 0x00000018 },
	{ 0x04c54, 0x00000028, 0x00000028 },
	{ 0x04c60, 0x00000008, 0x00000008 },
	{ 0x04c64, 0x00000009, 0x00000009 },
	{ 0x06000, 0x00008004, 0x00008004 },
	{ 0x06004, 0x00000000, 0x00000000 },
	{ 0x06008, 0x00000000, 0x00000000 },
	{ 0x0600c, 0x00000000, 0x00000000 },
	{ 0x06010, 0x00000680, 0x00000680 },
	{ 0x06200, 0x0000000d, 0x0000000d },
	{ 0x06204, 0x00000000, 0x00000000 },
	{ 0x06208, 0x00000000, 0x00000000 },
	{ 0x0620c, 0x00000000, 0x00000000 },
	{ 0x06404, 0x02005308, 0x02005308 },
	{ 0x06408, 0x02aa1905, 0x02aa1905 },
	{ 0x0640c, 0x00000000, 0x00000000 },
	{ 0x06410, 0x00000000, 0x00000000 },
	{ 0x06414, 0x00000001, 0x00000001 },
	{ 0x06418, 0x00000000, 0x00000000 },
	{ 0x06450, 0x00000000, 0x00000000 },
	{ 0x06800, 0x000b0405, 0x000b0405 },
	{ 0x06804, 0x00000111, 0x00000111 },
	{ 0x06c04, 0x00000240, 0x00000240 },
	{ 0x07400, 0xd0580010, 0xd0580010 },
	{ 0x07404, 0x11330000, 0x11330000 },
	{ 0x07424, 0x00000003, 0x00000003 },
};

static bool amplifiers_on;
static unsigned amp_start(const struct amplifier *a);
static unsigned amp_write(const struct amplifier *a, uint32_t reg, uint32_t value);
static bool amp_dsp_running;            /* the firmware went in: the mailbox is told play and pause */
bool shennong_speaker_protected;        /* both run Cirrus's speaker protection in their DSP (amp_fw) */

/* registers from `reg' on, the bytes as they are (most significant first): the chip's address counts on itself */
static bool
register_write_bytes(uint8_t address, uint32_t reg, const uint8_t *bytes, uint32_t count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1));
	for (int shift = 24; answered && shift >= 0; shift -= 8) {
		answered = i2c_write((uint8_t)(reg >> shift));
	}
	for (uint32_t i = 0; answered && i < count; i++) {
		answered = i2c_write(bytes[i]);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

/*
 * amp_fw=1 (the default when fw/amp-B.wr and amp-T.wr are there): Cirrus's speaker protection into the amplifier's
 * HALO DSP, as Android's driver has it running (tools/amp-fw.py turns the phone's wmfw and B-/T- tuning into these
 * lists of writes, at the places Linux's cs_dsp puts them): the core held, the firmware and its tuning written, the
 * memory protection opened (HALO_MPU_*: every region 0xffffffff, as Android's dump has them), the core started
 * (HALO_CCM_CORE_CONTROL 0x281, as there). True if every write was taken.
 */
static bool
amp_firmware(const struct amplifier *a, const char *file)
{
	uint64_t size = 0, at = 0;
	const uint8_t *list = redwood_wifi_file(file, &size);
	unsigned writes = 0, failed = 0;
	uint64_t began = mach_absolute_time(), ns;

	if (list == NULL) {
		say("speaker: %s: no fw/%s: no speaker protection\n", a->name, file);
		return false;
	}
	failed += amp_write(a, 0x2bc1000, 0);                    /* HALO_CCM_CORE_CONTROL: the core held */
	while (at + 8 <= size) {
		uint32_t reg, count;

		memcpy(&reg, list + at, 4);
		memcpy(&count, list + at + 4, 4);
		if (at + 8 + count > size) {
			break;
		}
		/* (in pieces of 256 bytes: one transfer each) */
		for (uint32_t done = 0; done < count; done += 256) {
			uint32_t n = count - done < 256 ? count - done : 256;

			if (!register_write_bytes(a->address, reg + done, list + at + 8 + done, n)) {
				failed++;
			}
			writes++;
		}
		at += 8 + ((count + 3) & ~3u);
	}
	{
		static const uint32_t mpu[] = {
			0x2bc3000, 0x2bc3004, 0x2bc3008, 0x2bc300c, 0x2bc3014, 0x2bc3018, 0x2bc301c, 0x2bc3020, 0x2bc3024,
			0x2bc302c, 0x2bc3030, 0x2bc3034, 0x2bc3038, 0x2bc303c, 0x2bc3044, 0x2bc3048, 0x2bc304c, 0x2bc3050,
			0x2bc3054, 0x2bc305c,
		};

		failed += amp_write(a, 0x2bc3140, 0x5555);       /* HALO_MPU_LOCK_CONFIG: unlocked */
		failed += amp_write(a, 0x2bc3140, 0xaaaa);
		for (unsigned i = 0; i < sizeof(mpu) / sizeof(mpu[0]); i++) {
			failed += amp_write(a, mpu[i], 0xffffffff);
		}
		failed += amp_write(a, 0x2bc3140, 0);
	}
	failed += amp_write(a, 0x2bc1000, 0x281);                /* the core: out of reset, running */
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	say("speaker: %s: fw/%s: %u transfers of the firmware and its tuning, %u not taken, in %llu ms; the DSP started\n",
	    a->name, file, writes, failed, ns / 1000000);
	return failed == 0;
}

static unsigned
amp_write(const struct amplifier *a, uint32_t reg, uint32_t value)
{
	if (!register_write(a->address, reg, value)) {
		say("speaker: %s: register %#x NOT WRITTEN (%#x)\n", a->name, reg, value);
		return 1;
	}
	return 0;
}

/*
 * With the TDM clock running (shennong_sound.c, the graph started): both amplifiers let out of reset, given Android's
 * configuration, and started as Android's driver starts them for playback -- traced on the phone (regmap_reg_write):
 * 0x4c00 the DAC's source, 0x6808, the PLL (0x2c04: d10, d00, 500, 510), 0x2d10, the ASP on (0x4800), power (0x2018),
 * 0x2084 behind the test key (0x40: 55 aa ... cc 33), global enable (0x2014), power again. Android's DAC source is
 * 0x36, its own DSP (with Cirrus's speaker protection firmware); here the DSP is not loaded, so the DAC takes the TDM
 * slot straight (ASPRX1, 0x08), as Linux's cs35l41 can. True if both answered every write.
 */
bool
shennong_speaker_on(void)
{
	unsigned failed = 0, protected = 0;
	uint32_t id, fw = 1;

	PE_parse_boot_argn("amp_fw", &fw, sizeof(fw));

	for (unsigned i = 0; i < 2; i++) {
		reset_pin(amplifiers[i].reset, false);
	}
	released = true;
	wait_ms(2);
	for (unsigned i = 0; i < 2; i++) {
		reset_pin(amplifiers[i].reset, true);
	}
	wait_ms(10);
	for (unsigned i = 0; i < 2; i++) {
		const struct amplifier *a = &amplifiers[i];

		if (!register_read(a->address, 0, &id)) {
			say("speaker: %s: NOBODY ANSWERS after its reset\n", a->name);
			failed++;
			continue;
		}
		for (unsigned r = 0; r < sizeof(android_config) / sizeof(android_config[0]); r++) {
			failed += amp_write(a, android_config[r].reg, i == 0 ? android_config[r].bottom : android_config[r].top);
		}
		if (fw && amp_firmware(a, i == 0 ? "amp-B.wr" : "amp-T.wr")) {
			protected++;
			amp_dsp_running = true;
			failed += amp_write(a, 0x4c00, 0x36);   /* the DAC's source: the DSP's output, as Android's */
		} else {
			failed += amp_write(a, 0x4c00, 0x08);   /* the DAC's source: ASPRX1 (Android: 0x36, its DSP) */
		}
		failed += amp_start(a);
	}
	amplifiers_on = failed == 0;
	shennong_speaker_protected = protected == 2;
	say("speaker: the amplifiers %s (%u writes not taken)\n", failed != 0 ? "ARE NOT ALL ON" : protected == 2 ?
	    "ARE ON, WITH CIRRUS'S SPEAKER PROTECTION RUNNING IN THEIR DSP" : "ARE ON, the DAC fed by TDM slot straight", failed);
	return failed == 0;
}

/* Android's playback start of one amplifier (regmap trace): the PLL, the ASP, power, the key, global enable. */
static unsigned
amp_start(const struct amplifier *a)
{
	unsigned failed = 0;

	{
		failed += amp_write(a, 0x6808, 0x3f75);
		failed += amp_write(a, 0x2c04, 0xd10);
		failed += amp_write(a, 0x2c04, 0xd00);
		failed += amp_write(a, 0x2c04, 0x500);
		failed += amp_write(a, 0x2c04, 0x510);
		failed += amp_write(a, 0x2d10, 0x21012);
		failed += amp_write(a, 0x4800, 0x10001);
		failed += amp_write(a, 0x2018, 0x3720);
		failed += amp_write(a, 0x40, 0x55);
		failed += amp_write(a, 0x40, 0xaa);
		failed += amp_write(a, 0x2084, 0xf1aa0);
		failed += amp_write(a, 0x40, 0xcc);
		failed += amp_write(a, 0x40, 0x33);
		failed += amp_write(a, 0x2014, 0x1);
		failed += amp_write(a, 0x2018, 0x3721);
		if (shennong_speaker_protected || a->address != 0) {
			/* (the DSP's mailbox, as Android: "play", then its interrupt status cleared) */
			if (amp_dsp_running) {
				failed += amp_write(a, 0x13020, 0xb000001);
				failed += amp_write(a, 0x10014, 0x1);
			}
		}
	}
	return failed;
}

/* ... and its stop (power down, the key, global enable off, the ASP off) */
static void
amp_stop(const struct amplifier *a)
{
	amp_write(a, 0x2018, 0x21);
	if (amp_dsp_running) {
		amp_write(a, 0x13020, 0xb000002);       /* the DSP's mailbox: "pause", as Android */
	}
	amp_write(a, 0x2018, 0x20);
	amp_write(a, 0x40, 0x55);
	amp_write(a, 0x40, 0xaa);
	amp_write(a, 0x2084, 0xf1aa3);
	amp_write(a, 0x40, 0xcc);
	amp_write(a, 0x40, 0x33);
	amp_write(a, 0x2014, 0);
	amp_write(a, 0x4800, 0);
}

/*
 * sound=4: started while there is sound, stopped after a few seconds of silence (shennong_sound.c), the
 * configuration kept meanwhile (the chips stay out of reset). Each said once a line.
 */
void
shennong_speaker_start(void)
{
	unsigned failed = 0;

	if (!released || amplifiers_on) {
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		failed += amp_start(&amplifiers[i]);
	}
	amplifiers_on = true;
	say("speaker: started (sound)%s\n", failed ? ": SOME WRITES NOT TAKEN" : "");
}

/*
 * The user hears gaps of about a tenth of a second, at random, about every ten seconds; what the kernel feeds and the
 * DSP takes has none. What the amplifiers say meanwhile: their interrupt status (0x10010..0x1001c, as Linux's cs35l41
 * IRQ1_STATUS1..4), said when not zero and then cleared (written back), with the DSP's mailbox state.
 */
void
shennong_speaker_poll(void)
{
	if (!released) {
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		const struct amplifier *a = &amplifiers[i];
		uint32_t st[4] = { 0 };
		bool any = false;

		for (unsigned r = 0; r < 4; r++) {
			register_read(a->address, 0x10010 + 4 * r, &st[r]);
			any |= st[r] != 0;
		}
		if (any) {
			say("speaker: %s: interrupt status %#x %#x %#x %#x\n", a->name, st[0], st[1], st[2], st[3]);
			for (unsigned r = 0; r < 4; r++) {
				if (st[r] != 0) {
					register_write(a->address, 0x10010 + 4 * r, st[r]);
				}
			}
		}
	}
}

void
shennong_speaker_stop(void)
{
	if (!released || !amplifiers_on) {
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		amp_stop(&amplifiers[i]);
	}
	amplifiers_on = false;
	say("speaker: stopped (silence)\n");
}

/* As Android's driver stops them (power down, the key, global enable off, the ASP off), then into reset. */
void
shennong_speaker_off(void)
{
	if (!released || tlmm == 0) {
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		const struct amplifier *a = &amplifiers[i];

		if (amplifiers_on) {
			amp_stop(a);
		}
		reset_pin(a->reset, false);
	}
	amplifiers_on = false;
	say("speaker: the amplifiers off and in reset\n");
}

void
shennong_speaker_init(void)
{
	thread_t thread;

	if (strcmp(redwood_phone(), "shennong") != 0 || !PE_parse_boot_argn("spk_probe", &probe_level, sizeof(probe_level)) ||
	    probe_level == 0) {
		return;
	}
	probe_level = probe_level > 4 ? 4 : probe_level;
	if (kernel_thread_start(probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the amplifiers back into reset, if they were let out. */
void
shennong_speaker_quiesce(void)
{
	if (released && tlmm != 0) {
		leaving = true;
		for (unsigned i = 0; i < sizeof(amplifiers) / sizeof(amplifiers[0]); i++) {
			reset_pin(amplifiers[i].reset, false);
		}
	}
}
