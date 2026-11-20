/*
 * Host test for xnu/files/bsd/dev/shennong_synaptics.c: the driver runs against a simulated S3910P
 * that answers on simulated pins -- every SPI bit goes through the driver's own GPIO writes and reads,
 * so this checks the bit-banging, TouchComm framing (header, continued reads in chunks, padding), the
 * boot loader -> application path, the report configuration parser, the RPMh vote for LDO12 and the
 * coordinates posted to /dev/redwood-touch.
 *
 *   A  the chip starts in its boot loader; report: timestamp, per finger index/class/x/y/z/widths and
 *      a vendor field (0xC5), padded to a byte
 *   B  the same, but the report counts its objects first and has x/y in 1/100 pixel (18 and 19 bits)
 *   C  the chip starts in its application
 *   D  no chip at all: the driver must give up cleanly
 *   E  what the phone showed: a half-read frame left over, frames of sensor data (report 0xC0, 1.5 KB)
 *      100 times a second until DISABLE_REPORT, touch reports only after ENABLE_REPORT 0x11
 *
 * In every case a transfer that begins less than 50 us after the one before gets stale bytes (01 ff),
 * as the phone's chip did, and moves nothing on.
 *
 *   tools/syna-sim/run.sh
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef void *thread_t;
#define KERN_SUCCESS 0
#define __unused __attribute__((unused))
#define REDWOOD_TOUCH_DOWN 1
#define REDWOOD_TOUCH_MOVE 2
#define REDWOOD_TOUCH_UP 3

#define strlcpy test_strlcpy
static size_t
strlcpy(char *d, const char *s, size_t n)
{
	size_t l = strlen(s);
	if (n) {
		size_t c = l < n - 1 ? l : n - 1;
		memcpy(d, s, c);
		d[c] = 0;
	}
	return l;
}

/* ---- simulated time, boot-args, thread ---------------------------------------------------------- */
static uint64_t now_us, end_us;
static jmp_buf jump_out;
static const char *bootargs[16][2];

static void
sleep_us(uint32_t us)
{
	now_us += us;
	if (now_us > end_us) {
		longjmp(jump_out, 1);
	}
}
static uint64_t
micros_now(void)
{
	return now_us++;
}
static int
PE_parse_boot_argn(const char *name, void *value, int size)
{
	for (int i = 0; bootargs[i][0]; i++) {
		if (strcmp(bootargs[i][0], name) == 0) {
			uint32_t v = (uint32_t)strtoul(bootargs[i][1], NULL, 0);
			memcpy(value, &v, (size_t)size < 4 ? (size_t)size : 4);
			return 1;
		}
	}
	return 0;
}
static void *current_thread(void) { return NULL; }
static kern_return_t
thread_terminate(thread_t t)
{
	(void)t;
	longjmp(jump_out, 2);
}
static void thread_deallocate(thread_t t) { (void)t; }
static void (*thread_fn)(void *, wait_result_t);
static kern_return_t
kernel_thread_start(void (*fn)(void *, wait_result_t), void *arg, thread_t *t)
{
	(void)arg;
	thread_fn = fn;
	*t = NULL;
	return KERN_SUCCESS;
}

/* ---- events posted to /dev/redwood-touch -------------------------------------------------------- */
static struct { uint32_t type, finger, x, y; } posted[64];
static int nposted;
static void
redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	printf("  POST %s finger %u at %u, %u\n", type == 1 ? "DOWN" : type == 2 ? "MOVE" : "UP  ", finger, x, y);
	if (nposted < 64) {
		posted[nposted].type = type; posted[nposted].finger = finger; posted[nposted].x = x; posted[nposted].y = y;
		nposted++;
	}
}

/* ---- the simulated hardware ---------------------------------------------------------------------- */
#define FAKE_TLMM 0x10000000UL
#define FAKE_RSC  0x20000000UL
static uint8_t fake_cmd_db[0x20000] __attribute__((aligned(16)));
static uint32_t gpio_cfg[256], gpio_out[256];
static uint32_t rsc_reg[0x4000];
static uint32_t rpmh_addr_seen[3], rpmh_data_seen[3];
static int rpmh_requests;

static vm_offset_t
ml_io_map(vm_offset_t phys, vm_size_t size)
{
	(void)size;
	if (phys == 0x0f100000UL) return FAKE_TLMM;
	if (phys == 0x17a20000UL) return FAKE_RSC;
	fprintf(stderr, "unexpected map %#lx\n", (unsigned long)phys);
	exit(1);
}
static vm_offset_t
ml_io_map_wcomb(vm_offset_t phys, vm_size_t size)
{
	(void)size;
	if (phys != 0x81c60000UL) { fprintf(stderr, "unexpected wcomb map\n"); exit(1); }
	return (vm_offset_t)fake_cmd_db;
}

/* chip model */
enum { MSG_MAX = 64 };
static struct { uint8_t bytes[1600]; uint32_t len; } queue[MSG_MAX];
static int qhead, qtail;
static uint32_t cur_pos;                /* bytes of the head message already read */
static bool powered, out_of_reset, configured_seen;
static uint8_t chip_mode = 0x0b;        /* starts in its boot loader */
static int variant;                     /* 'A' or 'B' */
static uint8_t txn_out[1700], txn_in[1700];
static uint64_t last_rise;
static bool txn_junk;
static bool touch_reports_on = true, frames_on;
static uint64_t next_frame;
static int frames_sent;
static uint32_t txn_bit, txn_len_out;
static bool cs_low, clk_high;
static bool txn_is_continued;
static uint64_t touch_start;
static int touch_step;

static void
queue_msg(uint8_t code, const uint8_t *payload, uint32_t len)
{
	uint8_t *b = queue[qtail].bytes;
	b[0] = 0xa5; b[1] = code; b[2] = (uint8_t)len; b[3] = (uint8_t)(len >> 8);
	memcpy(b + 4, payload, len);
	b[4 + len] = 0x5a;
	queue[qtail].len = 5 + len;
	qtail = (qtail + 1) % MSG_MAX;
}
static bool pending(void) { return qhead != qtail; }

static void
identify_payload(uint8_t *p)
{
	memset(p, 0, 24);
	p[0] = 1; p[1] = chip_mode;
	memcpy(p + 2, "S3910P-shennong", 15);
	p[18] = 0x39; p[19] = 0x30; p[20] = 0x05; p[21] = 0;        /* build */
	p[22] = 0x00; p[23] = 0x01;                                  /* max write 256 */
}

/* report configurations */
static const uint8_t config_A[] = {
	0x05, 32,                       /* TIMESTAMP 32 */
	0x01,                           /* FOREACH_ACTIVE_OBJECT */
	0x06, 4, 0x07, 4,               /* INDEX 4, CLASSIFICATION 4 */
	0x08, 16, 0x09, 16,             /* X 16, Y 16 */
	0x0a, 8, 0x0b, 8, 0x0c, 8,      /* Z, X width, Y width */
	0xc5, 6,                        /* something of Xiaomi's, 6 bits */
	0x04,                           /* PAD_TO_NEXT_BYTE */
	0x03, 0x00                      /* FOREACH_END, END */
};
static const uint8_t config_B[] = {
	0x18, 8,                        /* NUM_OF_ACTIVE_OBJECTS 8 */
	0x1e, 8,                        /* SENSING_MODE 8 (unknown to the vendor driver) */
	0x01,
	0x06, 4, 0x07, 4,
	0x08, 18, 0x09, 19,             /* super resolution: x up to 143999, y up to 319999 */
	0x03,
	0x05, 16,                       /* TIMESTAMP after the loop */
	0x00
};

static void
put_bits(uint8_t *r, uint32_t *off, uint32_t bits, uint32_t v)
{
	for (uint32_t i = 0; i < bits; i++, (*off)++) {
		if (v >> i & 1) r[*off / 8] |= (uint8_t)(1u << (*off % 8));
	}
}

struct finger { uint32_t idx, cls, x, y; };
static void
queue_touch(const struct finger *f, int n)
{
	uint8_t r[128] = { 0 };
	uint32_t off = 0;

	if (!touch_reports_on) {
		printf("  (chip: touch reports are off, finger data dropped)\n");
		return;
	}

	if (variant != 'B') {
		put_bits(r, &off, 32, (uint32_t)now_us);
		for (int i = 0; i < n; i++) {
			put_bits(r, &off, 4, f[i].idx); put_bits(r, &off, 4, f[i].cls);
			put_bits(r, &off, 16, f[i].x); put_bits(r, &off, 16, f[i].y);
			put_bits(r, &off, 8, 40); put_bits(r, &off, 8, 5); put_bits(r, &off, 8, 6);
			put_bits(r, &off, 6, 0x2a);
			off = (off + 7) & ~7u;
		}
	} else {
		put_bits(r, &off, 8, (uint32_t)n);
		put_bits(r, &off, 8, 3);
		for (int i = 0; i < n; i++) {
			put_bits(r, &off, 4, f[i].idx); put_bits(r, &off, 4, f[i].cls);
			put_bits(r, &off, 18, f[i].x * 100 + 37); put_bits(r, &off, 19, f[i].y * 100 + 63);
		}
		put_bits(r, &off, 16, 0xbeef);
	}
	queue_msg(0x11, r, (off + 7) / 8);
}

static void
chip_command(const uint8_t *in, uint32_t n)
{
	uint8_t p[64];

	if (n < 3) return;
	switch (in[0]) {
	case 0x02: identify_payload(p); queue_msg(0x01, p, 24); break;
	case 0x05:
	case 0x06:
		if (n >= 4 && in[3] == 0x11) touch_reports_on = in[0] == 0x05;
		if (n >= 4 && in[3] == 0xc0) frames_on = in[0] == 0x05;
		queue_msg(0x01, NULL, 0);
		break;
	case 0x10: memset(p, 0, 27); p[0] = 1; p[1] = 0; queue_msg(0x01, p, 27); break;
	case 0x14:
		chip_mode = 0x01; identify_payload(p); queue_msg(0x10, p, 24);
		if (variant == 'E') { frames_on = true; next_frame = now_us + 5000; }
		break;
	case 0x20:
		memset(p, 0, 46);
		p[0] = 3;                                       /* version */
		memcpy(p + 16, "XIAOMI-14PRO-CFG", 16);
		p[12] = 128; p[14] = 0x40;                      /* config size, payload size */
		p[32] = 1439 & 0xff; p[33] = 1439 >> 8;         /* max x */
		p[34] = 3199 & 0xff; p[35] = 3199 >> 8;         /* max y */
		p[36] = 10;                                     /* objects */
		p[40] = 40; p[42] = 18;                         /* rows, columns */
		queue_msg(0x01, p, 46);
		break;
	case 0x25:
		if (variant != 'B') queue_msg(0x01, config_A, sizeof(config_A));
		else queue_msg(0x01, config_B, sizeof(config_B));
		configured_seen = true;
		touch_start = now_us + (variant == 'E' ? 1500000 : 20000);
		break;
	default: queue_msg(0x0e, NULL, 0); break;
	}
}

static void
cs_fall(void)
{
	uint32_t l = 0;

	txn_bit = 0;
	memset(txn_in, 0, sizeof(txn_in));
	txn_is_continued = false;
	txn_junk = now_us - last_rise < 50;
	if (txn_junk) {                                 /* too soon: what was left in the shift register */
		memset(txn_out, 0xff, sizeof(txn_out));
		txn_out[0] = 0x01;
		txn_len_out = sizeof(txn_out);
		return;
	}
	if (!powered || !out_of_reset) {
		memset(txn_out, 0, sizeof(txn_out));    /* the pull-down */
		txn_len_out = sizeof(txn_out);
		return;
	}
	if (pending()) {
		if (cur_pos > 0) {
			txn_out[l++] = 0xa5; txn_out[l++] = 0x03;
			txn_is_continued = true;
		}
		for (uint32_t i = cur_pos; i < queue[qhead].len; i++) txn_out[l++] = queue[qhead].bytes[i];
	} else {
		txn_out[l++] = 0xa5; txn_out[l++] = 0; txn_out[l++] = 0; txn_out[l++] = 0;
	}
	while (l < sizeof(txn_out)) txn_out[l++] = 0x5a;
	txn_len_out = l;
}

static void
cs_rise(void)
{
	uint32_t nbytes = txn_bit / 8;

	last_rise = now_us;
	if (txn_junk || !powered || !out_of_reset || nbytes == 0) return;
	if (txn_in[0] != 0xff) {                        /* a command */
		chip_command(txn_in, nbytes);
		return;
	}
	if (pending()) {                                /* a read: the head message advances */
		uint32_t got = txn_is_continued ? nbytes - 2 : nbytes;
		uint32_t plen = queue[qhead].bytes[2] | queue[qhead].bytes[3] << 8;

		cur_pos += got;
		if (cur_pos >= 4 + plen) {                  /* header and payload read: done (the padding is optional) */
			qhead = (qhead + 1) % MSG_MAX;
			cur_pos = 0;
		}
	}
}

static bool
miso_level(void)
{
	if (!cs_low) return false;
	uint32_t byte = txn_bit / 8, bit = 7 - txn_bit % 8;
	return byte < txn_len_out ? (txn_out[byte] >> bit & 1) : 0;
}

/* scripted touches, by simulated time once the driver has the report configuration */
static void
script(void)
{
	static const struct finger s1[] = { { 0, 1, 720, 1600 } };
	static const struct finger s2[] = { { 0, 1, 730, 1610 }, { 3, 1, 100, 3000 } };
	static const struct finger s3[] = { { 3, 1, 101, 3001 }, { 5, 6, 500, 500 } };   /* 0 lifted; 5 is a palm */
	if (frames_on && now_us >= next_frame) {
		static uint8_t frame[1500];
		int pending_n = (qtail - qhead + MSG_MAX) % MSG_MAX;
		for (int i = 0; i < 1500; i++) frame[i] = (uint8_t)(i & 1 ? 0x27 : 0x70 + i % 7);
		if (pending_n < 3) { queue_msg(0xc0, frame, sizeof(frame)); frames_sent++; }
		next_frame = now_us + 10000;
	}
	if (!configured_seen || now_us < touch_start) return;
	uint64_t t = now_us - touch_start;
	if (touch_step == 0 && t > 0)      { queue_touch(s1, 1); touch_step++; }
	if (touch_step == 1 && t > 10000)  { queue_touch(s2, 2); touch_step++; }
	if (touch_step == 2 && t > 20000)  { queue_touch(s3, 2); touch_step++; }
	if (touch_step == 3 && t > 30000)  { queue_touch(NULL, 0); touch_step++; }
	if (touch_step == 4 && t > 40000)  {
		static const uint8_t text[] = "hello from firmware";
		queue_msg(0x82, text, sizeof(text) - 1);
		uint8_t longtext[300];
		for (int i = 0; i < 300; i++) longtext[i] = (uint8_t)('a' + i % 26);
		longtext[0] = 'L'; longtext[299] = 'Z';
		queue_msg(0x82, longtext, 300);
		uint8_t st[3] = { 1, 2, 3 };
		queue_msg(0x1b, st, 3);
		touch_step++;
	}
}

static uint32_t
rd(vm_offset_t a)
{
	if (a >= FAKE_TLMM && a < FAKE_TLMM + 0x100000) {
		uint32_t pin = (uint32_t)((a - FAKE_TLMM) / 0x1000), reg = (uint32_t)((a - FAKE_TLMM) % 0x1000);
		script();
		if (reg == 0) return gpio_cfg[pin];
		if (pin == 162) return (powered && out_of_reset && pending()) ? 0 : 1;   /* attention: low = message */
		if (pin == 48) return miso_level() | (gpio_out[pin] & 2);
		return (gpio_out[pin] & 2) | ((gpio_out[pin] >> 1) & 1);
	}
	if (a >= FAKE_RSC && a < FAKE_RSC + 0x10000) {
		uint32_t off = (uint32_t)(a - FAKE_RSC);
		if (off == 0) return 0x00030000;            /* DRV_ID: version 3.0 */
		if (off == 0xd00 + 0x28) return 1;          /* TCS 0 status: idle */
		return rsc_reg[off / 4];
	}
	fprintf(stderr, "rd of unexpected %#lx\n", (unsigned long)a);
	exit(1);
}

static void
wr(vm_offset_t a, uint32_t v)
{
	if (a >= FAKE_TLMM && a < FAKE_TLMM + 0x100000) {
		uint32_t pin = (uint32_t)((a - FAKE_TLMM) / 0x1000), reg = (uint32_t)((a - FAKE_TLMM) % 0x1000);
		if (reg == 0) { gpio_cfg[pin] = v; return; }
		bool high = v & 2;
		gpio_out[pin] = v;
		switch (pin) {
		case 164:
			powered = high && variant != 'D'; break;
		case 161:
			if (high && !out_of_reset && powered) {
				out_of_reset = true;
				qhead = qtail = 0; cur_pos = 0;
				chip_mode = variant == 'C' ? 0x01 : 0x0b;
				if (variant == 'E') {
					static uint8_t frame[1500];
					for (int i = 0; i < 1500; i++) frame[i] = (uint8_t)(i & 1 ? 0x27 : 0x70 + i % 7);
					queue_msg(0xc0, frame, sizeof(frame));
					cur_pos = 300;                      /* read half-way by someone before */
					touch_reports_on = false;
				}
				uint8_t p[24]; identify_payload(p); queue_msg(0x10, p, 24);
			} else if (!high) {
				out_of_reset = false;
			}
			break;
		case 51:
			if (!high && !cs_low) { cs_low = true; cs_fall(); }
			else if (high && cs_low) { cs_low = false; cs_rise(); }
			break;
		case 50:
			if (cs_low && high && !clk_high) {                 /* rising: sample MOSI */
				uint32_t byte = txn_bit / 8;
				if (byte < sizeof(txn_in))
					txn_in[byte] = (uint8_t)(txn_in[byte] << 1 | ((gpio_out[49] >> 1) & 1));
			} else if (cs_low && !high && clk_high) {          /* falling: next bit */
				txn_bit++;
			}
			clk_high = high;
			break;
		}
		return;
	}
	if (a >= FAKE_RSC && a < FAKE_RSC + 0x10000) {
		uint32_t off = (uint32_t)(a - FAKE_RSC);
		rsc_reg[off / 4] = v;
		for (int i = 0; i < 3; i++) {
			if (off == 0xd00 + 0x38u + 24u * i) rpmh_addr_seen[i] = v;
			if (off == 0xd00 + 0x3cu + 24u * i) rpmh_data_seen[i] = v;
		}
		if (off == 0xd00 + 0x24 && v == ((1u << 16) | (1u << 24))) {
			rsc_reg[(0xd00 + 0x04) / 4] |= 1;          /* done */
			rpmh_requests++;
		}
		if (off == 0xd00 + 0x08) rsc_reg[(0xd00 + 0x04) / 4] &= ~v;
		return;
	}
	fprintf(stderr, "wr of unexpected %#lx\n", (unsigned long)a);
	exit(1);
}

static void
make_cmd_db(void)
{
	/* header: version, magic, 8 slaves of 16 bytes, checksum, reserved; data after */
	uint8_t *d = fake_cmd_db;
	d[4] = 0xdb; d[5] = 0x30; d[6] = 0x03; d[7] = 0x0c;
	/* slave 0: VRM (id 4), entries at data + 0x40, 3 entries */
	uint16_t *s = (uint16_t *)(d + 8);
	s[0] = 4; s[1] = 0x40; s[2] = 0; s[3] = 3;
	uint8_t *data = d + 8 + 8 * 16 + 8;
	const char *names[3] = { "ldob11", "ldob12", "smpa4" };
	uint32_t addrs[3] = { 0x40500, 0x40600, 0x40000 };
	for (int i = 0; i < 3; i++) {
		uint8_t *e = data + 0x40 + 24 * i;
		strncpy((char *)e, names[i], 8);
		memcpy(e + 16, &addrs[i], 4);
	}
}

#define SYNA_HOST_TEST 1
#include "shennong_synaptics.c"

int
main(int argc, char **argv)
{
	volatile int fails = 0;

	variant = argc > 1 ? argv[1][0] : 'A';
	bootargs[0][0] = "syna_touch"; bootargs[0][1] = "1";
	bootargs[1][0] = "syna_dump"; bootargs[1][1] = "3";
	for (int i = 2, b = 2; i < argc && b < 15; i++, b++) {     /* more boot-args: name=value */
		char *eq = strchr(argv[i], '=');
		if (eq) { *eq = 0; bootargs[b][0] = argv[i]; bootargs[b][1] = eq + 1; }
	}
	make_cmd_db();
	end_us = 3000000;
	shennong_synaptics_init();
	if (!thread_fn) { printf("no thread started\n"); return 1; }
	int how = setjmp(jump_out);
	if (how == 0) thread_fn(NULL, 0);
	printf("== variant %c ended (%s) at %llu us\n", variant, how == 1 ? "time" : "driver gave up",
	    (unsigned long long)now_us);
	if (rpmh_requests != 1 || rpmh_addr_seen[0] != 0x40600 || rpmh_addr_seen[1] != 0x40604 ||
	    rpmh_addr_seen[2] != 0x40608 || rpmh_data_seen[0] != 1800 || rpmh_data_seen[1] != 1 || rpmh_data_seen[2] != 7) {
		printf("FAIL rpmh: %d requests, %#x %#x %#x = %u %u %u\n", rpmh_requests, rpmh_addr_seen[0], rpmh_addr_seen[1],
		    rpmh_addr_seen[2], rpmh_data_seen[0], rpmh_data_seen[1], rpmh_data_seen[2]);
		fails++;
	}
	/* expected: DOWN 0 (720,1600), MOVE 0 (730,1610), DOWN 3 (100,3000), MOVE 3, UP 0, UP 3 */
	struct { uint32_t type, finger, x, y; } want[] = {
		{ 1, 0, 720, 1600 }, { 2, 0, 730, 1610 }, { 1, 3, 100, 3000 }, { 2, 3, 101, 3001 }, { 3, 0, 730, 1610 },
		{ 3, 3, 101, 3001 },
	};
	int nwant = variant == 'D' ? 0 : (int)(sizeof(want) / sizeof(want[0]));
	if (variant == 'D') {
		if (how != 2) { printf("FAIL: the driver should have given up\n"); fails++; }
	}
	if (nposted != nwant) { printf("FAIL: %d events, want %d\n", nposted, nwant); fails++; }
	for (int i = 0; i < nwant && i < nposted; i++) {
		bool plain = variant != 'B';
		double sx = plain ? 1439.0 : 143999.0, sy = plain ? 3199.0 : 319999.0;
		double rx = plain ? want[i].x : want[i].x * 100 + 37, ry = plain ? want[i].y : want[i].y * 100 + 63;
		uint32_t ex = (uint32_t)(rx * 65535 / sx), ey = (uint32_t)(ry * 65535 / sy);
		if (posted[i].type != want[i].type || posted[i].finger != want[i].finger ||
		    (posted[i].x + 1 < ex || posted[i].x > ex + 1) || (posted[i].y + 1 < ey || posted[i].y > ey + 1)) {
			printf("FAIL event %d: %u %u %u,%u want %u %u %u,%u\n", i, posted[i].type, posted[i].finger, posted[i].x,
			    posted[i].y, want[i].type, want[i].finger, ex, ey);
			fails++;
		}
	}
	if (variant == 'E' && (frames_on || !touch_reports_on)) {
		printf("FAIL: frames still on (%d sent) or touch reports off\n", frames_sent);
		fails++;
	}
	printf("== variant %c: %s\n", variant, fails ? "FAILED" : "all as expected");
	return fails != 0;
}
