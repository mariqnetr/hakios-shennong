/*
 * The touch screen of the Xiaomi 14 Pro (shennong, Snapdragon 8 Gen 3): a Synaptics S3910P speaking
 * TouchComm version 1 on SPI, as a source of touches for /dev/redwood-touch (sys/redwood_input.h).
 * Only with boot-arg syna_touch=1: every address below belongs to this phone (and touchscreen=1,
 * the POCO X5 Pro's driver, must stay off here: its addresses are another phone's).
 *
 * Like redwood_goodix.c, the driver does everything itself, in the order the vendor's Linux driver
 * does (Qualcomm's synaptics_tcm, "synaptics,tcm-spi", which the phone's device tree names):
 *   power    the chip's I/O supply is LDO12 of the PM8550 ("ldob12", 1.8 V), switched through RPMh;
 *            the analog supply is a switch behind GPIO164
 *   pins     reset GPIO161 (low = in reset), attention GPIO162 (low = the chip has a message)
 *   SPI      the four pins of serial engine 4 of QUP 1 (GPIO48 MISO, 49 MOSI, 50 CLK, 51 CS), driven by
 *            hand in mode 0 -- the serial engine itself is not used, so no clocks are needed
 *   chip     TouchComm v1: messages "A5 code length16 payload 5A"; commands "code length16 payload".
 *            After a reset the chip sends an identify report; if it is in its boot loader it is told
 *            to run its application. The application's information says the coordinate range, and
 *            the touch report configuration says how a touch report is packed (bit fields, read the
 *            way the vendor driver reads them: synaptics_tcm_touch.c touch_parse_report())
 * No interrupt is used: a thread looks at the attention line 250 times a second
 * (boot-arg syna_poll_us=N: every N microseconds instead of 4000), and reads anyway every 80 ms.
 * Messages are read the way both vendor drivers read them ("predictive reading"): the first transfer
 * is as long as the last message was, so most come in one; the rest by continued reads. Between two
 * transfers the chip needs a moment to get its next bytes ready (boot-arg syna_gap_us, 200).
 * The chip may send reports of its own besides touches (frames of sensor data for Xiaomi's host
 * processing, say); one that comes more than 30 times a second is turned off (DISABLE_REPORT), since
 * reading kilobytes by hand 100 times a second would take the processor (syna_keep=1: left on).
 *
 * Other boot-args: syna_iovdd=0 leaves the I/O supply alone; syna_flip=1 mirrors left-right,
 * 2 top-bottom, 3 both; syna_swap=1 swaps the axes; syna_dump=N prints the first N messages in hex.
 *
 * Where things come from: the phone's own device tree (pins, supplies, delays: power 50 ms, reset held
 * 10 ms, 200 ms after it), Qualcomm's synaptics_tcm driver (touch-drivers/synaptics_tcm in the
 * vendor kernel modules) for the protocol, Linux drivers/soc/qcom/{rpmh-rsc,cmd-db}.c and
 * drivers/pinctrl/qcom/pinctrl-sm8650.c for the registers.
 */
#ifndef SYNA_HOST_TEST
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

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
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static uint64_t
micros_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}
#endif /* SYNA_HOST_TEST: the test brings its own rd/wr/sleep_us/micros_now and kernel stand-ins */

static void
sleep_ms(uint32_t ms)
{
	sleep_us(ms * 1000);
}

static vm_offset_t tlmm, rsc, cmd_db;

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL    /* pinctrl@f000000 + 0x100000 (pinctrl-sm8650.c) */
#define TLMM_SIZE               0x100000UL      /* 211 pins of 0x1000 */
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3

#define PIN_MISO                48      /* qup1_se4_l0 */
#define PIN_MOSI                49      /* qup1_se4_l1 */
#define PIN_CLK                 50      /* qup1_se4_l2 */
#define PIN_CS                  51      /* qup1_se4_l3 */
#define PIN_RESET               161     /* low = in reset */
#define PIN_ATTN                162     /* low = the chip has a message */
#define PIN_AVDD                164     /* high = analog supply on (touch_avdd_vreg) */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the I/O supply (RPMh) ----------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x81c60000UL    /* reserved-memory aop_cmd_db_region */
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x17a20000UL    /* apps_rsc "drv-2", the application processor's */
#define RSC_SIZE                0x10000UL
#define RSC_TCS_BASE            0xd00           /* qcom,tcs-offset; TCS 0 is an "active" one (tcs-config) */

/* the command slot registers move between RSC versions (rpmh-rsc.c rpmh_rsc_reg_offset_ver_2_7/3_0) */
static struct {
	uint32_t wait_for_completion, control, status, cmd_enable, msgid, addr, data, cmd_stride;
} tcs_reg;
#define TCS(reg)                (rsc + RSC_TCS_BASE + (reg))
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
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

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("syna: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512 ||
		    sizeof(*db) + slave->header_offset + slave->count * sizeof(*entry) > CMD_DB_SIZE) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("syna: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh through command slot set 0 and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(TCS(tcs_reg.status)) == 0; i++) {
		if (i > 2000) {
			printf("syna: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(tcs_reg.msgid + tcs_reg.cmd_stride * i), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(tcs_reg.addr + tcs_reg.cmd_stride * i), address[i]);
		wr(TCS(tcs_reg.data + tcs_reg.cmd_stride * i), data[i]);
	}
	wr(TCS(tcs_reg.wait_for_completion), (1u << count) - 1);
	wr(TCS(tcs_reg.cmd_enable), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);        /* the status bit follows the enable; the interrupt itself stays masked */
	wr(TCS(tcs_reg.control), TCS_CONTROL_ENABLE);
	wr(TCS(tcs_reg.control), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(tcs_reg.cmd_enable), 0);
	wr(TCS(tcs_reg.wait_for_completion), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

/* The chip's I/O supply: 1.8 V from LDO12 of the PM8550 ("ldob12", pm_humu_l12 in the device tree). */
static bool
io_supply_on(void)
{
	uint32_t id = rd(rsc), major = id >> 16 & 0xff, minor = id >> 8 & 0xff;
	uint32_t base = rpmh_address("ldob12");
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { 1800, 1, VRM_MODE_LDO_HPM };

	if (major >= 3) {
		tcs_reg.wait_for_completion = 0x20; tcs_reg.control = 0x24; tcs_reg.status = 0x28; tcs_reg.cmd_enable = 0x2c;
		tcs_reg.msgid = 0x34; tcs_reg.addr = 0x38; tcs_reg.data = 0x3c; tcs_reg.cmd_stride = 24;
	} else {
		tcs_reg.wait_for_completion = 0x10; tcs_reg.control = 0x14; tcs_reg.status = 0x18; tcs_reg.cmd_enable = 0x1c;
		tcs_reg.msgid = 0x30; tcs_reg.addr = 0x34; tcs_reg.data = 0x38; tcs_reg.cmd_stride = 20;
	}
	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("syna: RPMh: no regulator \"ldob12\" (address %#x)\n", base);
		return false;
	}
	printf("syna: RPMh %u.%u: ldob12 is at %#x; slot status %#x, control %#x, irq enable %#x status %#x\n", major, minor,
	    base, rd(TCS(tcs_reg.status)), rd(TCS(tcs_reg.control)), rd(TCS(TCS_IRQ_ENABLE)), rd(TCS(TCS_IRQ_STATUS)));
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	printf("syna: I/O supply on (1.8 V)\n");
	return true;
}

/* ---- SPI by hand: mode 0, MSB first, chip select low for the whole of one transfer ---------------- */


static void
spi_init(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(PIN_MOSI, false, PULL_NONE, 6);
	gpio_configure(PIN_MISO, 0, PULL_DOWN, 2, false);       /* the device tree's bias-pull-down */
}

/*
 * tx NULL: 0xFF goes out (what the vendor driver sends while it reads); rx NULL: what comes in is dropped.
 * Writes to the pins are posted: two in a row may reach them a few nanoseconds apart, a clock pulse too
 * short for a chip rated at 15 MHz. A read of the same block cannot overtake the writes before it, so
 * the read after the data bit holds the clock low until the bit and the falling edge are out (and gives
 * the chip time to put out its next bit), and the read of MISO holds it high.
 */
static uint32_t gap_us = 200;           /* boot-arg syna_gap_us */

static void
spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	/*
	 * The chip gets its next bytes ready only after chip select has gone high: a transfer that follows
	 * the one before within microseconds gets whatever was left in its shift register (seen on the
	 * phone: a continued read that began "01 ff" instead of "a5 03", and a correct one when the same
	 * read came 200 ms later). Linux's SPI framework leaves tens of microseconds between transfers
	 * anyway; here the gap is explicit, and chip select goes low a few microseconds before the clock.
	 */
	sleep_us(gap_us);
	gpio_set(PIN_CS, false);
	for (int i = 0; i < 8; i++) {
		(void)gpio_get(PIN_CS);
	}
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0xff, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(PIN_MOSI, out >> bit & 1);
			(void)gpio_get(PIN_MOSI);
			gpio_set(PIN_CLK, true);
			in = (uint8_t)(in << 1 | gpio_get(PIN_MISO));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
}

/* ---- TouchComm v1 -------------------------------------------------------------------------------- */

#define MESSAGE_MARKER          0xa5
#define MESSAGE_PADDING         0x5a

#define CMD_IDENTIFY            0x02
#define CMD_RESET               0x04
#define CMD_ENABLE_REPORT       0x05
#define CMD_DISABLE_REPORT      0x06
#define CMD_GET_BOOT_INFO       0x10
#define CMD_RUN_APPLICATION     0x14
#define CMD_GET_APP_INFO        0x20
#define CMD_GET_TOUCH_REPORT_CONFIG 0x25

#define STATUS_IDLE             0x00
#define STATUS_OK               0x01
#define STATUS_BUSY             0x02
#define STATUS_CONTINUED_READ   0x03
#define STATUS_ERROR            0x0f    /* 0x0b..0x0f: errors */
#define STATUS_INVALID          0xff

#define REPORT_IDENTIFY         0x10
#define REPORT_TOUCH            0x11
#define REPORT_STATUS           0x1b
#define REPORT_PRINTF           0x82

#define MODE_APPLICATION        0x01
#define MODE_HOST_DOWNLOAD      0x02
#define MODE_BOOTLOADER         0x0b
#define MODE_TDDI_BOOTLOADER    0x0c

#define MAX_PAYLOAD             2048    /* more than any message this driver asks for */
#define CHUNK                   256     /* payload bytes per continued read */

static uint8_t message[4 + MAX_PAYLOAD + 1];
static uint8_t chunk[2 + CHUNK];
static uint32_t dump;                   /* boot-arg syna_dump: messages still to print in hex */

static struct {
	uint32_t looks, messages, touch_reports, touches, bad, other, resets;
	uint32_t last_x, last_y;
} seen;

static void
hex_line(const char *what, const uint8_t *data, uint32_t length)
{
	char line[3 * 48 + 8];
	uint32_t n = length < 48 ? length : 48, at = 0;

	for (uint32_t i = 0; i < n; i++) {
		at += (uint32_t)snprintf(line + at, sizeof(line) - at, "%02x ", data[i]);
	}
	printf("syna: %s (%u bytes): %s%s\n", what, length, line, length > n ? "..." : "");
}

#define MIN_READ_LENGTH         9       /* the vendor drivers' PREDICTIVE_READING */
#define MAX_PREDICTED           160     /* a first read longer than a touch report costs more than it saves */

static uint32_t read_length = MIN_READ_LENGTH;
static bool lost;                       /* a message that would not end: the chip wants a reset */

/* The rest of a message left half-read: read on until the chip pads with 5A. */
static void
drain(void)
{
	for (int i = 0; i < 16; i++) {
		bool ended = true;

		spi_transfer(NULL, chunk, sizeof(chunk));
		if (chunk[0] != MESSAGE_MARKER || chunk[1] != STATUS_CONTINUED_READ) {
			return;
		}
		for (uint32_t j = sizeof(chunk) - 8; j < sizeof(chunk); j++) {
			ended &= chunk[j] == MESSAGE_PADDING;
		}
		if (ended) {
			return;
		}
	}
	printf("syna: a message that does not end\n");
	lost = true;
}

/*
 * Read one message: a first transfer as long as the last message was (at least 9 bytes), then what
 * it did not bring by continued reads, each of which the chip begins with A5 03. The message lands in
 * message[], its payload at message[4...]. Returns false when there was nothing that looked like a
 * message (no chip, a lost frame, the rest of an earlier one).
 */
static bool
read_message(uint8_t *code, uint32_t *length)
{
	uint32_t first = read_length, total, at;

	spi_transfer(NULL, message, first);
	if (message[0] != MESSAGE_MARKER) {
		return false;
	}
	*code = message[1];
	*length = message[2] | (uint32_t)message[3] << 8;
	if (*code == STATUS_CONTINUED_READ) {
		if (seen.bad++ < 8) {
			printf("syna: out of step: the rest of an earlier message where a new one should begin; reading it away\n");
		}
		drain();
		*length = 0;
		return false;
	}
	if (*code == STATUS_IDLE || *code == STATUS_BUSY) {
		*length = 0;            /* nothing waiting */
		return true;
	}
	if (*code == STATUS_INVALID) {
		return false;
	}
	/*
	 * What the first transfer did not bring: payload and the padding byte, in chunks; what does not fit
	 * is read and dropped. The padding byte alone is not read at all (neither do the vendor drivers:
	 * the chip would take the read for a new message).
	 */
	total = 4 + *length + 1;
	at = first;
	while (at < total) {
		uint32_t n = total - at > CHUNK ? CHUNK : total - at;

		if (n == 1) {
			if (at < sizeof(message)) {
				message[at] = MESSAGE_PADDING;
			}
			at++;
			continue;
		}
		spi_transfer(NULL, chunk, 2 + n);
		if (chunk[0] != MESSAGE_MARKER || chunk[1] != STATUS_CONTINUED_READ) {
			if (seen.bad++ < 8) {
				printf("syna: continued read of %#x (%u bytes, at %u) began %02x %02x, not a5 03\n", *code, *length, at,
				    chunk[0], chunk[1]);
			}
			return false;
		}
		for (uint32_t i = 0; i < n; i++) {
			if (at + i < sizeof(message)) {
				message[at + i] = chunk[2 + i];
			}
		}
		at += n;
	}
	if (*length > MAX_PAYLOAD) {
		printf("syna: message %#x of %u bytes, only the first %u kept\n", *code, *length, MAX_PAYLOAD);
		*length = MAX_PAYLOAD;
	} else if (message[4 + *length] != MESSAGE_PADDING) {
		if (seen.bad++ < 8) {
			printf("syna: message %#x (%u bytes) ends in %02x, not 5a\n", *code, *length, message[4 + *length]);
		}
		return false;
	}
	read_length = total < MIN_READ_LENGTH ? MIN_READ_LENGTH : total > MAX_PREDICTED ? MAX_PREDICTED : total;
	if (dump) {
		dump--;
		char what[24];
		snprintf(what, sizeof(what), "message %#x", *code);
		hex_line(what, message + 4, *length);
	}
	return true;
}

static void
write_command(uint8_t command, const uint8_t *payload, uint16_t length)
{
	uint8_t out[3 + 64];

	if (length > sizeof(out) - 3) {
		return;
	}
	out[0] = command;
	out[1] = (uint8_t)length;
	out[2] = (uint8_t)(length >> 8);
	for (uint16_t i = 0; i < length; i++) {
		out[3 + i] = payload[i];
	}
	spi_transfer(out, NULL, 3 + length);
}

/* ---- what the chip says about itself -------------------------------------------------------------- */

static struct {
	bool known;
	uint8_t version, mode;
	char part[17];
	uint32_t build, max_write;
} id;
static uint32_t max_x = 1439, max_y = 3199, max_objects = 10;  /* the device tree's until the chip says */
static uint8_t report_config[256];
static uint32_t report_config_length;

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static const char *
mode_name(uint8_t mode)
{
	switch (mode) {
	case MODE_APPLICATION: return "application";
	case MODE_HOST_DOWNLOAD: return "waiting for its firmware from the host";
	case MODE_BOOTLOADER: return "boot loader";
	case MODE_TDDI_BOOTLOADER: return "TDDI boot loader";
	case 0x0e: return "production test";
	default: return "unknown";
	}
}

static void
take_identify(const uint8_t *p, uint32_t length)
{
	uint8_t raw[24] = { 0 };

	for (uint32_t i = 0; i < length && i < sizeof(raw); i++) {
		raw[i] = p[i];
	}
	id.known = true;
	id.version = raw[0];
	id.mode = raw[1];
	for (int i = 0; i < 16; i++) {
		id.part[i] = raw[2 + i] >= 0x20 && raw[2 + i] < 0x7f ? (char)raw[2 + i] : (raw[2 + i] ? '?' : 0);
	}
	id.part[16] = 0;
	id.build = le32(raw + 18);
	id.max_write = le16(raw + 22);
	printf("syna: identify: TouchComm %u, mode %#x (%s), part \"%s\", build %u, writes up to %u bytes\n", id.version,
	    id.mode, mode_name(id.mode), id.part, id.build, id.max_write);
}

static void parse_touch_report(const uint8_t *report, uint32_t length);
static bool reconfigure;                /* the chip has reset: ask it again how it reports */
static bool turned_off[256];            /* reports turned off for coming too often */
static uint32_t per_second[256];        /* reports of each kind in the current second */

/* A report that came while doing something else, or in the loop. */
static void
take_report(uint8_t code, const uint8_t *payload, uint32_t length)
{
	static uint8_t shown_other[256];

	switch (code) {
	case REPORT_IDENTIFY:
		take_identify(payload, length);
		seen.resets++;
		reconfigure = true;
		break;
	case REPORT_TOUCH:
		seen.touch_reports++;
		parse_touch_report(payload, length);
		break;
	case REPORT_PRINTF: {
		char text[120];
		uint32_t n = length < sizeof(text) - 1 ? length : sizeof(text) - 1;

		for (uint32_t i = 0; i < n; i++) {
			text[i] = payload[i] >= 0x20 && payload[i] < 0x7f ? (char)payload[i] : ' ';
		}
		text[n] = 0;
		printf("syna: the chip prints: %s\n", text);
		break;
	}
	default:
		seen.other++;
		if (shown_other[code] < 3) {    /* the first three of each kind */
			shown_other[code]++;
			char what[32];
			snprintf(what, sizeof(what), "report %#x", code);
			hex_line(what, payload, length);
		}
		break;
	}
}

/* Wait for the attention line, then read; at most `ms` milliseconds. Reads anyway every 20 ms. */
static bool
wait_message(uint32_t ms, uint8_t *code, uint32_t *length)
{
	for (uint32_t t = 0; t < ms * 2; t++) {
		if (!gpio_get(PIN_ATTN) || t % 40 == 39) {
			if (read_message(code, length) && *code != STATUS_IDLE && *code != STATUS_BUSY &&
			    *code != STATUS_CONTINUED_READ) {
				return true;
			}
		}
		sleep_us(500);
	}
	return false;
}

/*
 * Send a command and wait for its answer (STATUS_OK and a payload, in message[4...]). Reports that
 * come meanwhile are taken as they come; an identify report is the answer to a reset or to running
 * the application.
 */
static bool
command_with(uint8_t cmd, const uint8_t *payload, uint16_t payload_length, uint32_t *length, uint32_t ms)
{
	uint8_t code;

	write_command(cmd, payload, payload_length);
	for (uint64_t start = micros_now(); micros_now() - start < (uint64_t)ms * 1000;) {
		if (!wait_message(ms, &code, length)) {
			break;
		}
		if (code == STATUS_OK) {
			return true;
		}
		if (code >= 0x0b && code <= STATUS_ERROR) {
			printf("syna: command %#x: error %#x%s\n", cmd, code, *length ? "," : "");
			if (*length) {
				hex_line("error payload", message + 4, *length);
			}
			return false;
		}
		if (code >= REPORT_IDENTIFY) {
			take_report(code, message + 4, *length);
			if (code == REPORT_IDENTIFY && (cmd == CMD_RESET || cmd == CMD_RUN_APPLICATION)) {
				return true;
			}
		}
	}
	printf("syna: command %#x: no answer in %u ms\n", cmd, ms);
	return false;
}

static bool
command(uint8_t cmd, uint32_t *length, uint32_t ms)
{
	return command_with(cmd, NULL, 0, length, ms);
}

/* ---- the touch report: bit fields as the configuration lists them -------------------------------- */

#define TOUCH_END                       0x00
#define TOUCH_FOREACH_ACTIVE_OBJECT     0x01
#define TOUCH_FOREACH_OBJECT            0x02
#define TOUCH_FOREACH_END               0x03
#define TOUCH_PAD_TO_NEXT_BYTE          0x04
#define TOUCH_OBJECT_N_INDEX            0x06
#define TOUCH_OBJECT_N_CLASSIFICATION   0x07
#define TOUCH_OBJECT_N_X_POSITION       0x08
#define TOUCH_OBJECT_N_Y_POSITION       0x09
#define TOUCH_NUM_OF_ACTIVE_OBJECTS     0x18

#define CLASS_FINGER                    1
#define CLASS_GLOVED_FINGER             2

#define MAX_FINGERS                     10

static uint32_t
bits_at(const uint8_t *report, uint32_t length, uint32_t offset, uint32_t bits)
{
	uint32_t value = 0;

	if (bits == 0 || bits > 32 || offset + bits > length * 8) {
		return 0;
	}
	for (uint32_t i = 0; i < bits; i++) {
		uint32_t at = offset + i;

		value |= (uint32_t)(report[at / 8] >> (at % 8) & 1) << i;
	}
	return value;
}

/* bits one pass of a FOREACH takes, from just after it to its FOREACH_END (pads counted as nothing) */
static uint32_t
loop_bits(uint32_t from, uint32_t *end)
{
	uint32_t bits = 0, i = from;

	while (i < report_config_length) {
		uint8_t code = report_config[i++];

		if (code == TOUCH_FOREACH_END || code == TOUCH_END) {
			break;
		}
		if (code == TOUCH_PAD_TO_NEXT_BYTE || code == TOUCH_FOREACH_ACTIVE_OBJECT || code == TOUCH_FOREACH_OBJECT) {
			continue;
		}
		if (i < report_config_length) {
			bits += report_config[i++];
		}
	}
	*end = i;
	return bits;
}
extern void shennong_touch_boost(void);

static struct {
	bool down, seen;
	uint32_t x, y;
} contact[MAX_FINGERS];
static uint32_t flip, swap_axes;
static uint32_t super_x = 1, super_y = 1;     /* positions in hundredths of a pixel ("super resolution") */

static void
post_finger(uint32_t finger, uint32_t raw_x, uint32_t raw_y)
{
	static unsigned shown;
	uint32_t x, y, span_x, span_y;

	shennong_touch_boost();         /* the speeds up while a finger is down (shennong_thermal.c) */

	/* the device tree's super-resolution-factor is 100: positions beyond the range are in its units */
	if (raw_x > max_x + 1 && super_x == 1 && raw_x <= (max_x + 1) * 100) {
		super_x = 100;
		printf("syna: x positions are in 1/100 pixel\n");
	}
	if (raw_y > max_y + 1 && super_y == 1 && raw_y <= (max_y + 1) * 100) {
		super_y = 100;
		printf("syna: y positions are in 1/100 pixel\n");
	}
	span_x = (max_x + 1) * super_x - 1;
	span_y = (max_y + 1) * super_y - 1;
	if (shown < 40) {
		shown++;
		printf("syna: finger %u at %u, %u (of %u x %u)\n", finger, raw_x, raw_y, span_x, span_y);
	}
	seen.touches++;
	seen.last_x = raw_x;
	seen.last_y = raw_y;
	x = raw_x >= span_x ? 65535 : (uint32_t)((uint64_t)raw_x * 65535 / span_x);
	y = raw_y >= span_y ? 65535 : (uint32_t)((uint64_t)raw_y * 65535 / span_y);
	if (swap_axes) {
		uint32_t t = x;

		x = y;
		y = t;
	}
	if (flip & 1) {
		x = 65535 - x;
	}
	if (flip & 2) {
		y = 65535 - y;
	}
	contact[finger].seen = true;
	if (!contact[finger].down) {
		redwood_touch_post(REDWOOD_TOUCH_DOWN, finger, x, y);
	} else if (contact[finger].x != x || contact[finger].y != y) {
		redwood_touch_post(REDWOOD_TOUCH_MOVE, finger, x, y);
	}
	contact[finger].down = true;
	contact[finger].x = x;
	contact[finger].y = y;
}

static void
parse_touch_report(const uint8_t *report, uint32_t length)
{
	static unsigned unknown_shown, class_shown;
	uint32_t idx = 0, offset = 0, active = (uint32_t)-1;
	uint32_t loop_start = 0, loop_end = 0, per_object = 0, pass = 0;
	bool in_loop = false, active_only = false;
	struct { uint32_t index, cls, x, y; } object = { 0, 0, 0, 0 };

	if (report_config_length == 0) {
		return;
	}
	for (int i = 0; i < MAX_FINGERS; i++) {
		contact[i].seen = false;
	}
	while (idx < report_config_length) {
		uint8_t code = report_config[idx++];
		uint32_t bits, value;

		switch (code) {
		case TOUCH_END:
			idx = report_config_length;
			break;
		case TOUCH_FOREACH_ACTIVE_OBJECT:
		case TOUCH_FOREACH_OBJECT:
			active_only = code == TOUCH_FOREACH_ACTIVE_OBJECT;
			per_object = loop_bits(idx, &loop_end);
			loop_start = idx;
			pass = 0;
			/* no objects at all: past the loop */
			if (active_only && (active == 0 || (active == (uint32_t)-1 && offset + per_object > length * 8))) {
				idx = loop_end;
				break;
			}
			in_loop = true;
			object.index = 0;
			object.cls = object.x = object.y = 0;
			break;
		case TOUCH_FOREACH_END:
			if (!in_loop) {
				break;
			}
			if (object.cls == CLASS_FINGER || object.cls == CLASS_GLOVED_FINGER) {
				if (object.index < MAX_FINGERS) {
					post_finger(object.index, object.x, object.y);
				}
			} else if (object.cls != 0 && class_shown++ < 8) {
				printf("syna: object %u of class %u at %u, %u (not a finger: left out)\n", object.index, object.cls,
				    object.x, object.y);
			}
			pass++;
			if (active_only ? (active != (uint32_t)-1 ? pass < active : offset + per_object <= length * 8)
			    : pass < max_objects) {
				if (pass < 32) {
					idx = loop_start;
					object.index = pass;
					object.cls = object.x = object.y = 0;
					break;
				}
			}
			in_loop = false;
			break;
		case TOUCH_PAD_TO_NEXT_BYTE:
			offset = (offset + 7) & ~7u;
			break;
		default:
			if (idx >= report_config_length) {
				break;
			}
			bits = report_config[idx++];
			value = bits_at(report, length, offset, bits);
			offset += bits;
			switch (code) {
			case TOUCH_OBJECT_N_INDEX: object.index = value; break;
			case TOUCH_OBJECT_N_CLASSIFICATION: object.cls = value; break;
			case TOUCH_OBJECT_N_X_POSITION: object.x = value; break;
			case TOUCH_OBJECT_N_Y_POSITION: object.y = value; break;
			case TOUCH_NUM_OF_ACTIVE_OBJECTS: active = value; break;
			default:
				if (code > TOUCH_NUM_OF_ACTIVE_OBJECTS + 1 && unknown_shown++ < 4) {
					printf("syna: report field %#x (%u bits) skipped\n", code, bits);
				}
				break;
			}
			break;
		}
	}
	/* a finger that is no longer in the report has lifted */
	for (uint32_t i = 0; i < MAX_FINGERS; i++) {
		if (contact[i].down && !contact[i].seen) {
			contact[i].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, i, contact[i].x, contact[i].y);
		}
	}
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static void
show_state(const char *when)
{
	printf("syna: %s: pins miso/mosi/clk/cs cfg %#x %#x %#x %#x, reset %#x/%u, attention %#x/%u, avdd %#x/%u\n", when,
	    rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_CFG(PIN_CLK)), rd(GPIO_CFG(PIN_CS)),
	    rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)) & 3, rd(GPIO_CFG(PIN_ATTN)), rd(GPIO_IO(PIN_ATTN)) & 3,
	    rd(GPIO_CFG(PIN_AVDD)), rd(GPIO_IO(PIN_AVDD)) & 3);
}

/* Power the chip up from nothing, in the vendor driver's order, and let it out of reset. */
static void
power_on(bool iovdd)
{
	gpio_output(PIN_RESET, false, PULL_NONE, 2);            /* held in reset while the supplies come */
	gpio_configure(PIN_ATTN, 0, PULL_NONE, 2, false);       /* syna_tcm_eint_init: bias-disable, input */
	spi_init();
	if (iovdd) {
		io_supply_on();
	}
	sleep_ms(3);
	gpio_output(PIN_AVDD, true, PULL_NONE, 2);
	sleep_ms(50);                                           /* power-delay-ms */
	gpio_set(PIN_RESET, true);
	sleep_ms(200);                                          /* reset-delay-ms */
}

static void
hard_reset(void)
{
	gpio_set(PIN_RESET, false);
	sleep_ms(10);                                           /* reset-active-ms */
	gpio_set(PIN_RESET, true);
	sleep_ms(200);
}

/* After a reset: the identify report, or ask for it. */
static bool
find_chip(void)
{
	uint8_t code;
	uint32_t length;

	id.known = false;
	for (int i = 0; i < 3 && !id.known; i++) {
		if (wait_message(i == 0 ? 300 : 100, &code, &length)) {
			if (code == REPORT_IDENTIFY) {
				take_identify(message + 4, length);
			} else if (code >= REPORT_IDENTIFY) {
				take_report(code, message + 4, length);
			}
		}
	}
	if (!id.known && command(CMD_IDENTIFY, &length, 200)) {
		take_identify(message + 4, length);
	}
	if (!id.known) {
		uint8_t raw[8];

		spi_transfer(NULL, raw, sizeof(raw));
		printf("syna: no answer; a read gives %02x %02x %02x %02x %02x %02x %02x %02x (attention line %s)\n",
		    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], gpio_get(PIN_ATTN) ? "high" : "low");
	}
	return id.known;
}

/* The application running, its numbers, and how it packs a touch report. */
static bool
configure(void)
{
	uint32_t length;

	reconfigure = false;
	if (id.mode == MODE_BOOTLOADER || id.mode == MODE_TDDI_BOOTLOADER) {
		if (command(CMD_GET_BOOT_INFO, &length, 200) && length >= 2) {
			printf("syna: boot loader: status %#x%s\n", message[5],
			    message[5] == 0xfe ? " (bad application firmware)" : "");
		}
		printf("syna: telling the chip to run its application\n");
		if (!command(CMD_RUN_APPLICATION, &length, 1000)) {
			return false;
		}
	}
	if (id.mode == MODE_HOST_DOWNLOAD) {
		printf("syna: the chip has no firmware of its own and waits for it from the host -- not done yet\n");
		return false;
	}
	if (id.mode != MODE_APPLICATION) {
		printf("syna: the chip is in mode %#x (%s), not running its application\n", id.mode, mode_name(id.mode));
		return false;
	}
	for (int i = 0; i < 20; i++) {
		if (!command(CMD_GET_APP_INFO, &length, 200) || length < 4) {
			return false;
		}
		if (le16(message + 4 + 2) != 0x01) {    /* status 1: still booting */
			break;
		}
		sleep_ms(50);
	}
	{
		const uint8_t *a = message + 4;
		char config_id[17];

		for (int i = 0; i < 16; i++) {
			config_id[i] = length >= 32 && a[16 + i] >= 0x20 && a[16 + i] < 0x7f ? (char)a[16 + i] : '.';
		}
		config_id[16] = 0;
		printf("syna: application %u, status %u; config %s; touch report config up to %u bytes, report up to %u\n",
		    le16(a), le16(a + 2), config_id, length >= 14 ? le16(a + 12) : 0, length >= 16 ? le16(a + 14) : 0);
		/* struct syna_tcm_app_info: ... customer_config_id[16] at 16, max_x 32, max_y 34, max_objects 36,
		 * buttons 38, image rows 40, columns 42 */
		if (length >= 38) {
			max_x = le16(a + 32);
			max_y = le16(a + 34);
			max_objects = le16(a + 36);
			printf("syna: %u x %u, up to %u objects; sensor image %u x %u\n", max_x, max_y, max_objects,
			    length >= 44 ? le16(a + 42) : 0, length >= 42 ? le16(a + 40) : 0);
		}
		if (max_x == 0 || max_y == 0) {
			max_x = 1439;
			max_y = 3199;
		}
		if (max_objects == 0 || max_objects > 32) {
			max_objects = MAX_FINGERS;
		}
		if (le16(a + 2) != 0) {
			printf("syna: the application's status is %#x, not 0 (ok); going on anyway\n", le16(a + 2));
		}
	}
	if (!command(CMD_GET_TOUCH_REPORT_CONFIG, &length, 200) || length == 0) {
		printf("syna: no touch report configuration\n");
		return false;
	}
	report_config_length = length < sizeof(report_config) ? length : sizeof(report_config);
	for (uint32_t i = 0; i < report_config_length; i++) {
		report_config[i] = message[4 + i];
	}
	hex_line("touch report config", report_config, report_config_length);
	/*
	 * Positions in 1/100 pixel (the device tree's super-resolution-factor) need wider fields than the
	 * range: 1440 x 100 takes 18 bits, 1440 itself 11. A field more than wide enough for 50 times the
	 * range says so from the first touch on; post_finger() also notices a position beyond the range.
	 */
	super_x = super_y = 1;
	for (uint32_t i = 0; i + 1 < report_config_length; i++) {
		uint8_t code = report_config[i];

		if (code == TOUCH_END || code == TOUCH_FOREACH_ACTIVE_OBJECT || code == TOUCH_FOREACH_OBJECT ||
		    code == TOUCH_FOREACH_END || code == TOUCH_PAD_TO_NEXT_BYTE) {
			continue;
		}
		if (code == TOUCH_OBJECT_N_X_POSITION && report_config[i + 1] < 32 &&
		    (1u << report_config[i + 1]) > (max_x + 1) * 50) {
			super_x = 100;
		}
		if (code == TOUCH_OBJECT_N_Y_POSITION && report_config[i + 1] < 32 &&
		    (1u << report_config[i + 1]) > (max_y + 1) * 50) {
			super_y = 100;
		}
		i++;    /* its width */
	}
	if (super_x > 1 || super_y > 1) {
		printf("syna: positions in 1/100 pixel (x %s, y %s), by the widths of their fields\n", super_x > 1 ? "yes" : "no",
		    super_y > 1 ? "yes" : "no");
	}
	/* touch reports on, in case the firmware starts with them off (its answer is all that is wanted) */
	{
		uint8_t report = REPORT_TOUCH;
		uint32_t n;

		printf("syna: ENABLE_REPORT %#x: %s\n", report, command_with(CMD_ENABLE_REPORT, &report, 1, &n, 200) ? "ok" : "refused");
	}
	for (int i = 0; i < 256; i++) {
		turned_off[i] = false;  /* a reset turns the chip's reports back to what they were */
	}
	reconfigure = false;    /* the identify report that running the application brought is this one */
	return true;
}

static bool
bring_up(void)
{
	uint32_t iovdd = 1;

	PE_parse_boot_argn("syna_iovdd", &iovdd, sizeof(iovdd));
	show_state("as found");
	power_on(iovdd != 0);
	show_state("powered");
	/*
	 * Which pin is which is fixed by the serial engine (its line 0 is MISO, 1 MOSI, 2 the clock, 3 the
	 * chip select), so unlike redwood_goodix.c the data pins are not tried the other way round: that
	 * would drive the chip's own output. A second reset, though, if the first brought nothing.
	 */
	for (int attempt = 0; attempt < 2; attempt++) {
		if (attempt) {
			printf("syna: resetting the chip once more\n");
			hard_reset();
		}
		if (find_chip()) {
			return configure();
		}
	}
	show_state("no answer");
	return false;
}

static void
syna_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 4000, idle = 0, idle_looks, keep = 0;

	PE_parse_boot_argn("syna_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 4000;
	}
	idle_looks = 80000 / poll_us;

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */

	if (!bring_up()) {
		printf("syna: no Synaptics touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("syna: touch screen ready; the attention line is looked at every %u us\n", poll_us);
	PE_parse_boot_argn("syna_keep", &keep, sizeof(keep));
	for (uint64_t second = micros_now(), status = second;;) {
		uint8_t code;
		uint32_t length;
		uint64_t now;

		seen.looks++;
		if (!gpio_get(PIN_ATTN) || ++idle >= idle_looks) {
			idle = 0;
			if (read_message(&code, &length)) {
				if (code >= REPORT_IDENTIFY) {
					seen.messages++;
					per_second[code]++;
					take_report(code, message + 4, length);
				}
			} else {
				seen.bad++;
			}
		}
		if (lost) {
			printf("syna: resetting the chip to get back in step\n");
			lost = false;
			hard_reset();
			if (!find_chip()) {
				printf("syna: giving up\n");
				thread_terminate(current_thread());
				return;
			}
			reconfigure = true;
		}
		if (reconfigure) {
			printf("syna: the chip has reset; asking it again how it reports\n");
			for (int i = 0; i < MAX_FINGERS; i++) {
				if (contact[i].down) {
					contact[i].down = false;
					redwood_touch_post(REDWOOD_TOUCH_UP, (uint32_t)i, contact[i].x, contact[i].y);
				}
			}
			if (!configure()) {
				printf("syna: giving up\n");
				thread_terminate(current_thread());
				return;
			}
		}
		now = micros_now();
		if (now - second >= 1000000) {
			second = now;
			/* a report that floods: off, unless it is one this driver reads */
			for (uint32_t c = REPORT_IDENTIFY; c < 256; c++) {
				if (per_second[c] > 30 && c != REPORT_TOUCH && c != REPORT_IDENTIFY && !turned_off[c] && !keep) {
					uint8_t report = (uint8_t)c;
					uint32_t n;

					turned_off[c] = true;
					printf("syna: report %#x comes %u times a second; DISABLE_REPORT: %s\n", c, per_second[c],
					    command_with(CMD_DISABLE_REPORT, &report, 1, &n, 200) ? "ok" : "refused");
				}
				per_second[c] = 0;
			}
		}
		if (now - status >= 30000000) {
			status = now;
			printf("syna: so far %u messages (%u touch reports, %u other) in %u looks, %u bad, %u resets; "
			    "%u finger positions, the last at %u, %u; attention line %s\n", seen.messages, seen.touch_reports,
			    seen.other, seen.looks, seen.bad, seen.resets, seen.touches, seen.last_x, seen.last_y,
			    gpio_get(PIN_ATTN) ? "high" : "low");
		}
		sleep_us(poll_us);
	}
}

/*
 * A touch script (boot-arg touch_script=S: S seconds after the driver starts, then over and over): the same gestures
 * every run, so that a run measures the desktop with no one at the phone and two runs compare. Coordinates are the
 * screen's, 0..65535 (what redwood_touch_post takes), for the 1290 x 2796 desktop: the dock's four icons, the home
 * gesture from the bottom edge, the pages left and right (past the last, the App Library) and the App Library scrolled.
 * Each step is said on the console ("touch script: ...") so a log can be cut by it. A real finger meanwhile is left
 * alone, but its contact 0 is the script's too: hands off the glass during a scripted run.
 */
extern void IOSleep(unsigned int milliseconds);
#define TS(v)   ((uint32_t)((v) * 65535u / 10000u))   /* v: ten-thousandths of the screen */

static void
script_swipe(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t ms)
{
	uint32_t steps = ms / 8 ? ms / 8 : 1;

	shennong_touch_boost();
	redwood_touch_post(REDWOOD_TOUCH_DOWN, 0, TS(x0), TS(y0));
	for (uint32_t i = 1; i <= steps; i++) {
		IOSleep(8);
		shennong_touch_boost();
		redwood_touch_post(REDWOOD_TOUCH_MOVE, 0, TS(x0 + (x1 - x0) * (int32_t)i / (int32_t)steps), TS(y0 + (y1 - y0) * (int32_t)i / (int32_t)steps));
	}
	redwood_touch_post(REDWOOD_TOUCH_UP, 0, TS(x1), TS(y1));
}

static void
script_tap(int32_t x, int32_t y)
{
	shennong_touch_boost();
	redwood_touch_post(REDWOOD_TOUCH_DOWN, 0, TS(x), TS(y));
	IOSleep(70);
	redwood_touch_post(REDWOOD_TOUCH_UP, 0, TS(x), TS(y));
}

static void
script_thread(void *arg, wait_result_t wr)
{
	uint32_t start = (uint32_t)(uintptr_t)arg, round = 0;
	static const int32_t dock_x[4] = { 1550, 3850, 6150, 8450 };

	(void)wr;
	IOSleep(start * 1000);
	for (;;) {
		round++;
		for (int i = 0; i < 4; i++) {
			printf("touch script: round %u: open the dock's app %d\n", round, i + 1);
			script_tap(dock_x[i], 9050);
			IOSleep(2500);
			printf("touch script: round %u: home\n", round);
			script_swipe(5000, 9950, 5000, 5500, 160);
			IOSleep(1500);
		}
		printf("touch script: round %u: pages left, to the App Library\n", round);
		for (int i = 0; i < 3; i++) {
			script_swipe(8500, 5000, 1500, 5000, 200);
			IOSleep(900);
		}
		printf("touch script: round %u: the App Library scrolled\n", round);
		for (int i = 0; i < 3; i++) {
			script_swipe(5000, 8000, 5000, 3000, 180);
			IOSleep(700);
		}
		for (int i = 0; i < 3; i++) {
			script_swipe(5000, 3000, 5000, 8000, 180);
			IOSleep(700);
		}
		printf("touch script: round %u: pages right, back to the first\n", round);
		for (int i = 0; i < 3; i++) {
			script_swipe(1500, 5000, 8500, 5000, 200);
			IOSleep(900);
		}
		printf("touch script: round %u: done\n", round);
		IOSleep(1000);
	}
}

void
shennong_synaptics_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("syna_touch", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("syna_flip", &flip, sizeof(flip));
	PE_parse_boot_argn("syna_swap", &swap_axes, sizeof(swap_axes));
	PE_parse_boot_argn("syna_dump", &dump, sizeof(dump));
	PE_parse_boot_argn("syna_gap_us", &gap_us, sizeof(gap_us));
	if (gap_us > 20000) {
		gap_us = 200;
	}
	if (kernel_thread_start(syna_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	{
		uint32_t script = 0;

		if (PE_parse_boot_argn("touch_script", &script, sizeof(script)) && script != 0 &&
		    kernel_thread_start(script_thread, (void *)(uintptr_t)script, &thread) == KERN_SUCCESS) {
			printf("touch script: the gestures start in %u s (touch_script=%u)\n", script, script);
			thread_deallocate(thread);
		}
	}
}
