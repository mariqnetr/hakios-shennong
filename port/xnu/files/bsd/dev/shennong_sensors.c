/*
 * The motion sensors of the Xiaomi 14 Pro (shennong, SM8650): the accelerometer and the gyroscope, from the
 * audio DSP's sensors' framework. Nothing here happens unless boot-arg gyro=N asks for it.
 *
 * On SM8650 the motion sensors are not the application processor's: they hang on the sensors' own I2C/I3C
 * buses (the device tree's qupv3_se*_i3c pins of the "island"), which the audio DSP owns, and a domain of
 * the DSP's, the sensors' (Android's "sensorspd", fastrpc's qcom,fastrpc-adsp-sensors-pdr), runs their
 * drivers and Qualcomm's sensors' framework (SEE). Android reaches it through one QMI service on QRTR:
 * the sensors' client service, 400 (SNS_CLIENT_SVC), which the DSP tells of at its node 5 (port 20 in the
 * logs of shennong's runs) as soon as GLINK and QRTR are up -- before anybody on Android's side has done
 * anything for the sensors' domain. redwood_adsp.c brings the DSP up to QRTR (step 10) for gyro=N and keeps
 * it; this file is a QMI client of that service from ports of our own.
 *
 * The service carries Protocol Buffers both ways (Qualcomm's sns_client.proto, sns_std.proto, sns_suid.proto,
 * sns_std_sensor.proto, published with the sensors' HAL; postmarketOS's libssc speaks the same):
 *   - QMI request 0x20: TLV 1 the request's bytes (a 16-bit count, then up to 1000 bytes); the response
 *     0x20: TLV 2 QMI's result, TLV 0x10 our client's id, TLV 0x11 the framework's own result
 *   - indications 0x21 (and 0x22, "jumbo"): TLV 1 the client's id, TLV 2 the event's bytes
 *   - a request (sns_client_request_msg): the sensor's SUID (two fixed64), the message's id (fixed32), where
 *     its events go (the application processor, 1; waking it, 0), and sns_std_request with the payload
 *   - an event (sns_client_event_msg): the SUID it is from, and events: id, time (QTimer ticks, 19.2 MHz),
 *     payload
 * What is asked:
 *   1  the SUID lookup sensor (SUID 0xabab...ab both halves), message 512 with the data type ("accel",
 *      "gyro"): its event 768 has the SUIDs of the sensors of that type (the default one alone)
 *   2  each sensor's attributes (message 1, event 128): name, vendor, rates, ranges...
 *   3  its stream (message 513, sns_std_sensor_config: the rate as a float): events 1025 with three floats
 *      (accelerometer: m/s^2; gyroscope: rad/s; in Android's axes, the framework having turned the chip's
 *      by its placement) and how reliable they are (0..3); event 768 when the chip is set up
 * and at the end, before the DSP is shut down, each stream is stopped (message 10).
 *
 * The samples go to /dev/shennong-motion, a queue that a program reads records of 32 bytes from (struct
 * motion_record below; reading never waits: nothing to read is 0 bytes), and every gyro_log seconds a line
 * of the latest of each goes to the log.
 *
 *   gyro=1  accel and gyro: looked up, their attributes, their streams at gyro_hz (default 50)
 *   gyro=2  ... and first a look at what else the framework has (a list of data types looked up, with the
 *           attributes of each that is there), for the log
 *   gyro=3  as 1, and the first 16 events of each stream in hex as well
 * gyro_log=S: the line of the latest samples every S seconds (default 1; 0: none).
 *
 * What the DSP's sensors' domain needs that it may not have under this kernel: Android serves it files
 * (the registry in /mnt/vendor/persist/sensors, the chips' configuration in /vendor/etc/sensors) through
 * FastRPC (sscrpcd), which nobody here does. A lookup that finds no SUID for "accel" or "gyro" is said in
 * those words, and is the first thing to find out with gyro=2.
 *
 * The kernel does not use the floating point unit: floats are made and read from their bits here.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static LCK_GRP_DECLARE(sensors_lock_group, "shennong-sensors");

/* ---- what is said: to the loader's log (read on another core) and the kernel's ------------------------------- */

static LCK_MTX_DECLARE(say_lock, &sensors_lock_group);

static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[320];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	lck_mtx_lock(&say_lock);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_ms(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / NSEC_PER_MSEC;
}

/* ---- floats from and to their bits ---------------------------------------------------------------------------- */

/* A whole number as a 32-bit float's bits. */
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

/* A float's value times `scale' (1000: thousandths), toward zero; what does not fit is the largest that does. */
static int64_t
float_scaled(uint32_t bits, uint32_t scale)
{
	int exponent = (int)((bits >> 23) & 0xff);
	uint64_t mantissa = bits & 0x7fffff, value;
	int shift;

	if (exponent == 0xff) {
		return (bits >> 31) ? -INT64_MAX : INT64_MAX;
	}
	if (exponent == 0) {
		return 0;                       /* (subnormal: below any scale used here) */
	}
	mantissa |= 1u << 23;
	value = mantissa * scale;               /* < 2^24 * 2^20 */
	shift = exponent - 150;
	if (shift >= 0) {
		value = shift >= 19 ? (uint64_t)INT64_MAX : value << shift;
	} else {
		value = -shift >= 64 ? 0 : value >> -shift;
	}
	if (value > (uint64_t)INT64_MAX) {
		value = (uint64_t)INT64_MAX;
	}
	return (bits >> 31) ? -(int64_t)value : (int64_t)value;
}

/* "-1.234" from thousandths. */
static const char *
milli(char *buffer, size_t size, int64_t thousandths)
{
	uint64_t magnitude = thousandths < 0 ? (uint64_t)(-thousandths) : (uint64_t)thousandths;

	snprintf(buffer, size, "%s%llu.%03llu", thousandths < 0 ? "-" : "", magnitude / 1000, magnitude % 1000);
	return buffer;
}

/* ---- Protocol Buffers: written into a buffer, read from one --------------------------------------------------- */

enum { PB_VARINT = 0, PB_FIXED64 = 1, PB_BYTES = 2, PB_FIXED32 = 5 };

struct pb_out {
	uint8_t *data;
	uint32_t length, size;
	bool full;
};

static void
pb_byte(struct pb_out *o, uint8_t b)
{
	if (o->length >= o->size) {
		o->full = true;
		return;
	}
	o->data[o->length++] = b;
}

static void
pb_varint(struct pb_out *o, uint64_t v)
{
	do {
		pb_byte(o, (uint8_t)((v & 0x7f) | (v > 0x7f ? 0x80 : 0)));
		v >>= 7;
	} while (v != 0);
}

static void
pb_key(struct pb_out *o, uint32_t field, uint32_t wire)
{
	pb_varint(o, (uint64_t)field << 3 | wire);
}

static void
pb_uint(struct pb_out *o, uint32_t field, uint64_t v)
{
	pb_key(o, field, PB_VARINT);
	pb_varint(o, v);
}

static void
pb_fixed32(struct pb_out *o, uint32_t field, uint32_t v)
{
	pb_key(o, field, PB_FIXED32);
	for (int i = 0; i < 4; i++) {
		pb_byte(o, (uint8_t)(v >> (8 * i)));
	}
}

static void
pb_fixed64(struct pb_out *o, uint32_t field, uint64_t v)
{
	pb_key(o, field, PB_FIXED64);
	for (int i = 0; i < 8; i++) {
		pb_byte(o, (uint8_t)(v >> (8 * i)));
	}
}

static void
pb_bytes(struct pb_out *o, uint32_t field, const void *data, uint32_t length)
{
	pb_key(o, field, PB_BYTES);
	pb_varint(o, length);
	for (uint32_t i = 0; i < length; i++) {
		pb_byte(o, ((const uint8_t *)data)[i]);
	}
}

struct pb_in {
	const uint8_t *data;
	uint32_t length, at;
};

struct pb_field {
	uint32_t number, wire;
	uint64_t value;                         /* a varint's, a fixed32's or a fixed64's */
	const uint8_t *bytes;                   /* PB_BYTES: what it holds */
	uint32_t length;
};

static bool
pb_read_varint(struct pb_in *in, uint64_t *v)
{
	*v = 0;
	for (unsigned shift = 0; shift < 64; shift += 7) {
		uint8_t b;

		if (in->at >= in->length) {
			return false;
		}
		b = in->data[in->at++];
		*v |= (uint64_t)(b & 0x7f) << shift;
		if (!(b & 0x80)) {
			return true;
		}
	}
	return false;
}

/* The next field; false at the end or at something that is not well formed. */
static bool
pb_next(struct pb_in *in, struct pb_field *f)
{
	uint64_t key, length;

	if (in->at >= in->length || !pb_read_varint(in, &key)) {
		return false;
	}
	f->number = (uint32_t)(key >> 3);
	f->wire = (uint32_t)(key & 7);
	f->value = 0;
	f->bytes = NULL;
	f->length = 0;
	switch (f->wire) {
	case PB_VARINT:
		return pb_read_varint(in, &f->value);
	case PB_FIXED64:
	case PB_FIXED32: {
		uint32_t n = f->wire == PB_FIXED64 ? 8 : 4;

		if (in->length - in->at < n) {
			return false;
		}
		for (uint32_t i = 0; i < n; i++) {
			f->value |= (uint64_t)in->data[in->at + i] << (8 * i);
		}
		in->at += n;
		return true;
	}
	case PB_BYTES:
		if (!pb_read_varint(in, &length) || length > in->length - in->at) {
			return false;
		}
		f->bytes = in->data + in->at;
		f->length = (uint32_t)length;
		in->at += (uint32_t)length;
		return true;
	default:
		return false;
	}
}

/* ---- the sensors' service: QMI on QRTR ------------------------------------------------------------------------ */

#define SNS_CLIENT_SERVICE      400
#define SNS_CLIENT_REQ          0x20
#define SNS_CLIENT_REPORT_IND   0x21
#define SNS_CLIENT_JUMBO_IND    0x22
#define SNS_PAYLOAD_MAX         1000
#define QMI_REQUEST             0
#define QMI_RESPONSE            2
#define QMI_INDICATION          4

/* message ids (sns_client.proto, sns_std.proto, sns_suid.proto, sns_std_sensor.proto) */
#define MSG_ATTR_REQ            1
#define MSG_DISABLE_REQ         10
#define MSG_ATTR_EVENT          128
#define MSG_FLUSH_EVENT         129
#define MSG_ERROR_EVENT         130
#define MSG_SUID_REQ            512
#define MSG_SENSOR_CONFIG       513
#define MSG_SUID_EVENT          768     /* (from the lookup sensor) */
#define MSG_PHYSICAL_CONFIG     768     /* (from a physical sensor) */
#define MSG_SENSOR_EVENT        1025
#define MSG_PROXIMITY_EVENT     1028    /* (sns_proximity.proto: near or far, the raw reading, status) */
#define SUID_LOOKUP             0xababababababababull
#define PROCESSOR_APSS          1
#define DELIVERY_WAKEUP         0

/* attribute ids (sns_std.proto, sns_std_sensor_attr_id) */
static const char *const attribute_names[] = {
	"name", "vendor", "type", "available", "version", "api", "rates", "resolutions", "fifo_size", "active_current",
	"sleep_current", "ranges", "op_modes", "dri", "stream_sync", "event_size", "stream_type", "dynamic", "hw_id",
	"rigid_body", "placement", "physical_sensor", "physical_sensor_tests", "selected_resolution", "selected_range",
	"additional_low_latency_rates", "passive_request",
};

#define OUR_PORT_LOOKUP         0x4001  /* the lookups and the attributes */
#define OUR_PORT_STREAM         0x4002  /* + the sensor's number: each stream a client of its own */

enum { SENSOR_ACCEL, SENSOR_GYRO, SENSOR_ALS, SENSOR_ALS_RAW, SENSOR_ALS_CAL, SENSOR_CCT_FRONT, SENSOR_ALS_BACK, SENSOR_PROX, SENSOR_PROX_CAL,
	SENSORS };
#define LIGHT(which)            ((which) >= SENSOR_ALS)        /* (the proximity sensor's two are read as these) */

/*
 * The ambient light sensor (the TCS3720 under the screen, "ambient_light": an on-change sensor, asked for with
 * message 514 and no configuration): its first value is the light in lux. The latest, in thousandths of a lux, is
 * the sysctl kern.redwood.als_mlux (-1 until the first), which iOS's brightness is fed from (backboardd).
 */
#define MSG_ON_CHANGE_CONFIG    514
/*
 * Xiaomi's "ambient_light" gives its calibration only (the channels' scales); the light itself comes from data
 * types of their own (the phone's persist/sensors/sensors_list.txt): each of these is asked for too, and the first
 * twelve events of each said, to learn which gives what. boot-arg als_from=N: the one kern.redwood.als_mlux takes its
 * first value from (0 ambient_light, 1 ambient_light_raw, 2 ambient_light_cal_strm, 3 cct_front_strm,
 * 4 ambient_light_back_strm; default 3: the front sensor, its lux made below).
 */
static const char *const sensor_types[SENSORS] = { "accel", "gyro", "ambient_light", "ambient_light_raw",
	"ambient_light_cal_strm", "cct_front_strm", "ambient_light_back_strm", "proximity", "proximity_cal_strm" };
static const char *const sensor_units[SENSORS] = { "m/s2", "rad/s", "lux", "", "", "", "lux", "", "" };
static const unsigned sensor_values[SENSORS] = { 3, 3, 1, 1, 1, 1, 1, 1, 1 };    /* the floats an event must have */
static uint32_t als_from = 3;           /* (cct_front_strm: the front sensor's counts, made into lux below) */

/*
 * The light in front of the phone, from the TCS3720 under the screen. Its stream (cct_front_strm) gives counts of four
 * channels, C R G B (its values 4 to 7), and those are mostly the screen's own light: Xiaomi's sensors' HAL takes it
 * off before it makes lux, from what the panel at each brightness was measured to give off in red, green and blue
 * (persist's lightSensorCali.json, below) and from the colours on the screen right above the sensor (lightSensorConfig's
 * cwbInfo: around 700, 334 of the panel). The same here: the brightness from ShennongBacklight.cpp, the colours from
 * the frame the display shows, and then lux = channel_coef . (counts - the screen's), its coefficients x 10^6.
 */
extern uint32_t shennong_backlight_dbv(void);
extern bool shennong_screen_light(uint32_t x, uint32_t y, uint32_t radius, uint32_t light[3]);
static const int64_t lux_coef[4] = { 26474, -17179, 42490, -58922 };    /* lightSensorConfig.json, Cct.channel_coef */
/* persist/sensors/lightSensorCali.json, panel_Info_cali: the TCS3720's C, R, G, B counts (x10) for a full red,
 * green, blue screen at each brightness level (DBV, the first column) */
#define LEAK_ROWS 26
static const uint32_t leak_r[LEAK_ROWS][5] = {
	{ 0, 0, 0, 0, 0 },
	{ 10, 1059, 1024, 109, 106 },
	{ 50, 2075, 2015, 191, 202 },
	{ 100, 4041, 3970, 370, 387 },
	{ 150, 5986, 5859, 540, 563 },
	{ 200, 7879, 7699, 724, 752 },
	{ 250, 9789, 9610, 897, 924 },
	{ 300, 11794, 11535, 1075, 1115 },
	{ 350, 13875, 13554, 1270, 1311 },
	{ 400, 15298, 15018, 1403, 1438 },
	{ 450, 16843, 16462, 1538, 1585 },
	{ 500, 18484, 18069, 1700, 1749 },
	{ 550, 20328, 19929, 1861, 1914 },
	{ 600, 22256, 21823, 2042, 2104 },
	{ 650, 24370, 23900, 2250, 2286 },
	{ 800, 31417, 30739, 2888, 2937 },
	{ 950, 36299, 35456, 3317, 3396 },
	{ 1100, 41014, 39991, 3756, 3819 },
	{ 1250, 45900, 44942, 4226, 4291 },
	{ 1400, 51275, 50229, 4706, 4811 },
	{ 1550, 57138, 55934, 5250, 5367 },
	{ 1700, 63876, 62577, 5922, 6025 },
	{ 1850, 71105, 69565, 6608, 6683 },
	{ 2200, 86525, 84353, 8039, 8149 },
	{ 2550, 100156, 97539, 9307, 9407 },
	{ 2866, 113299, 110458, 10591, 10691 },
};
static const uint32_t leak_g[LEAK_ROWS][5] = {
	{ 0, 0, 0, 0, 0 },
	{ 10, 1170, 344, 754, 309 },
	{ 50, 2340, 703, 1491, 593 },
	{ 100, 4658, 1367, 2996, 1162 },
	{ 150, 6887, 2023, 4464, 1747 },
	{ 200, 9084, 2657, 5820, 2278 },
	{ 250, 11131, 3257, 7154, 2787 },
	{ 300, 13451, 3913, 8633, 3375 },
	{ 350, 16019, 4678, 10308, 4014 },
	{ 400, 17667, 5142, 11361, 4442 },
	{ 450, 19427, 5675, 12481, 4845 },
	{ 500, 21405, 6241, 13781, 5373 },
	{ 550, 23568, 6860, 15151, 5895 },
	{ 600, 25658, 7493, 16518, 6413 },
	{ 650, 28121, 8165, 18105, 6989 },
	{ 800, 36418, 10588, 23428, 9116 },
	{ 950, 41746, 12150, 26909, 10454 },
	{ 1100, 47175, 13672, 30453, 11820 },
	{ 1250, 53204, 15388, 34235, 13298 },
	{ 1400, 59306, 17144, 38146, 14853 },
	{ 1550, 66409, 19160, 42588, 16624 },
	{ 1700, 73946, 21295, 47472, 18497 },
	{ 1850, 81630, 23533, 52309, 20408 },
	{ 2200, 99233, 28624, 63978, 24919 },
	{ 2550, 115694, 33405, 74807, 29105 },
	{ 2866, 131253, 37810, 84708, 33047 },
};
static const uint32_t leak_b[LEAK_ROWS][5] = {
	{ 0, 0, 0, 0, 0 },
	{ 10, 208, 35, 75, 158 },
	{ 50, 403, 61, 138, 301 },
	{ 100, 780, 106, 276, 576 },
	{ 150, 1158, 153, 407, 848 },
	{ 200, 1547, 204, 541, 1108 },
	{ 250, 1929, 251, 660, 1383 },
	{ 300, 2301, 298, 797, 1664 },
	{ 350, 2684, 347, 932, 1949 },
	{ 400, 2986, 382, 1035, 2157 },
	{ 450, 3279, 421, 1148, 2364 },
	{ 500, 3613, 464, 1252, 2601 },
	{ 550, 3949, 525, 1380, 2862 },
	{ 600, 4357, 559, 1520, 3146 },
	{ 650, 4740, 618, 1654, 3423 },
	{ 800, 6129, 785, 2134, 4427 },
	{ 950, 7087, 916, 2468, 5119 },
	{ 1100, 8056, 1049, 2792, 5809 },
	{ 1250, 9083, 1157, 3148, 6529 },
	{ 1400, 10118, 1300, 3530, 7309 },
	{ 1550, 11341, 1457, 3957, 8187 },
	{ 1700, 12693, 1641, 4419, 9150 },
	{ 1850, 14106, 1826, 4895, 10200 },
	{ 2200, 17338, 2237, 6013, 12538 },
	{ 2550, 20285, 2601, 7020, 14600 },
	{ 2866, 22961, 2953, 7944, 16565 },
};

/* a table's row for the brightness `dbv', its counts x 10, between the two rows around it */
static void
leak_at(const uint32_t table[LEAK_ROWS][5], uint32_t dbv, int64_t out[4])
{
	unsigned i = 1;

	while (i < LEAK_ROWS - 1 && table[i][0] < dbv) {
		i++;
	}
	for (unsigned c = 0; c < 4; c++) {
		int64_t lo = table[i - 1][c + 1], hi = table[i][c + 1], span = table[i][0] - table[i - 1][0];
		int64_t at = (int64_t)(dbv > table[i][0] ? table[i][0] : dbv) - table[i - 1][0];

		out[c] = span > 0 ? lo + (hi - lo) * (at < 0 ? 0 : at) / span : hi;
	}
}

/* thousandths of a lux from the front sensor's four counts (thousandths) */
static int64_t
front_lux(const int64_t counts[4], bool say_it)
{
	uint32_t dbv = shennong_backlight_dbv(), light[3] = { 65535, 65535, 65535 };
	bool seen = shennong_screen_light(700, 334, 30, light);
	int64_t leak[4] = { 0, 0, 0, 0 }, row[4], lux = 0;
	const uint32_t (*const tables[3])[5] = { leak_r, leak_g, leak_b };

	for (unsigned color = 0; color < 3; color++) {
		leak_at(tables[color], dbv, row);
		for (unsigned c = 0; c < 4; c++) {
			leak[c] += row[c] * 100 * light[color] / 65535;      /* (x 10 -> thousandths) */
		}
	}
	for (unsigned c = 0; c < 4; c++) {
		lux += lux_coef[c] * (counts[c] - leak[c]);
	}
	lux /= 1000000;
	if (say_it) {
		say("sensors: front light: level %u, screen above it r %u g %u b %u%s; counts %lld %lld %lld %lld, the screen's "
		    "%lld %lld %lld %lld: %lld.%03lld lux\n", dbv, light[0] >> 8, light[1] >> 8, light[2] >> 8, seen ? "" :
		    " (NOT READ: taken as white)", counts[0] / 1000, counts[1] / 1000, counts[2] / 1000, counts[3] / 1000,
		    leak[0] / 1000, leak[1] / 1000, leak[2] / 1000, leak[3] / 1000, lux / 1000, (lux < 0 ? -lux : lux) % 1000);
	}
	return lux < 0 ? 0 : lux;
}


SYSCTL_DECL(_kern_redwood);
static int als_mlux = -1;
static uint32_t als_hz = 10;                /* boot-arg als_hz=N: the light asked for N times a second; 0: its changes */
static unsigned als_events_said[SENSORS];
SYSCTL_INT(_kern_redwood, OID_AUTO, als_mlux, CTLFLAG_RD | CTLFLAG_LOCKED, &als_mlux, 0,
    "the ambient light, thousandths of a lux (-1: none yet)");

/* the level auto-brightness has made, as iOS's brightness (0..65536; -1: none), for the slider (libshennong-als) */
static int bright_auto = -1;
SYSCTL_INT(_kern_redwood, OID_AUTO, bright_auto, CTLFLAG_RD | CTLFLAG_LOCKED, &bright_auto, 0,
    "auto-brightness's level as iOS's brightness, 0..65536 (-1: none)");
void shennong_bright_auto_value(int value);
void
shennong_bright_auto_value(int value)
{
	bright_auto = value;
}

/*
 * The proximity sensor (the TCS3720's): sns_proximity_event, its field 1 near (1) or far (0), 2 the raw reading,
 * 3 the status. kern.redwood.prox: 1 something near, 0 nothing, -1 not known yet; for iOS (libshennong-als: the
 * proximity HID service backboardd takes, screen off at the ear). The first twelve events said.
 */
static int prox_near = -1;
SYSCTL_INT(_kern_redwood, OID_AUTO, prox, CTLFLAG_RD | CTLFLAG_LOCKED, &prox_near, 0,
    "the proximity sensor: 1 near, 0 far, -1 not known");

static void
proximity_event(unsigned which, const uint8_t *data, uint32_t length)
{
	static unsigned said;
	struct pb_in in = { data, length, 0 };
	struct pb_field f;
	uint64_t near = 0, raw = 0, status = 0;

	while (pb_next(&in, &f)) {
		if (f.wire == PB_VARINT && f.number == 1) {
			near = f.value;
		} else if (f.wire == PB_VARINT && f.number == 2) {
			raw = f.value;
		} else if (f.wire == PB_VARINT && f.number == 3) {
			status = f.value;
		}
	}
	if (which == SENSOR_PROX) {
		prox_near = near != 0;
	}
	if (said++ < 12) {
		say("sensors: %s: %s, raw %llu, status %llu\n", sensor_types[which], near ? "NEAR" : "far", raw, status);
	}
}

/* the latest light for the backlight's auto-brightness (iokit/Kernel/ShennongBacklight.cpp) */
int shennong_als_mlux(void);
int
shennong_als_mlux(void)
{
	return als_mlux;
}

struct suid {
	uint64_t low, high;
};

static struct {
	struct suid suid;
	bool found, streaming;
	uint32_t events, events_shown, status, last_bits[3];
	uint64_t last_time;
	uint64_t client;                        /* the service's id for our stream's client */
} sensor[SENSORS];

static uint32_t service_node, service_port, mode, rate_hz = 50, log_seconds = 1;
static uint16_t next_txn = 1;
static volatile bool wanted, runs;

/* What the DSP sends our ports, copied on the link's thread and taken by ours. */
#define RX_SLOTS                64
#define RX_MAX                  2048

struct rx {
	uint32_t port, length;
	uint8_t data[RX_MAX];
};
static struct rx *rx_ring;
static uint32_t rx_head, rx_tail, rx_dropped, rx_too_long;
static LCK_MTX_DECLARE(rx_lock, &sensors_lock_group);

static void
rx_input(uint32_t to_port, __unused uint32_t from_node, __unused uint32_t from_port, const uint8_t *data, uint32_t length)
{
	lck_mtx_lock(&rx_lock);
	if (rx_ring == NULL) {
		;
	} else if (length > RX_MAX) {
		rx_too_long++;
	} else if (rx_tail - rx_head >= RX_SLOTS) {
		rx_dropped++;
	} else {
		struct rx *r = &rx_ring[rx_tail % RX_SLOTS];

		r->port = to_port;
		r->length = length;
		memcpy(r->data, data, length);
		rx_tail++;
	}
	lck_mtx_unlock(&rx_lock);
}

static bool
rx_take(struct rx *out)
{
	bool got = false;

	lck_mtx_lock(&rx_lock);
	if (rx_ring != NULL && rx_head != rx_tail) {
		*out = rx_ring[rx_head % RX_SLOTS];
		rx_head++;
		got = true;
	}
	lck_mtx_unlock(&rx_lock);
	return got;
}

/* A request to a sensor (sns_client_request_msg) from our port `port'; the QMI transaction's id, 0 if not sent. */
static uint16_t
request(uint32_t port, struct suid suid, uint32_t msg_id, const uint8_t *payload, uint32_t payload_length)
{
	uint8_t inner[64], susp[8], std[SNS_PAYLOAD_MAX], message[7 + 3 + 2 + SNS_PAYLOAD_MAX];
	struct pb_out o_suid = { inner, 0, sizeof(inner), false }, o_susp = { susp, 0, sizeof(susp), false };
	struct pb_out o_std = { std, 0, sizeof(std), false };
	struct pb_out o = { message + 7 + 3 + 2, 0, SNS_PAYLOAD_MAX, false };
	uint16_t txn = next_txn++;
	uint32_t qmi_length;
	int error;

	if (next_txn == 0) {
		next_txn = 1;
	}
	pb_fixed64(&o_suid, 1, suid.low);
	pb_fixed64(&o_suid, 2, suid.high);
	pb_uint(&o_susp, 1, PROCESSOR_APSS);
	pb_uint(&o_susp, 2, DELIVERY_WAKEUP);
	if (payload_length != 0) {
		pb_bytes(&o_std, 2, payload, payload_length);
	}
	pb_bytes(&o, 1, inner, o_suid.length);
	pb_fixed32(&o, 2, msg_id);
	pb_bytes(&o, 3, susp, o_susp.length);
	pb_bytes(&o, 4, std, o_std.length);
	if (o.full || o_std.full) {
		say("sensors: a request of message %u does not fit in %u bytes: not sent\n", msg_id, SNS_PAYLOAD_MAX);
		return 0;
	}
	/* QMI: type, transaction, message, length; then TLV 1: a count of 16 bits and the bytes */
	qmi_length = 3 + 2 + o.length;
	message[0] = QMI_REQUEST;
	message[1] = (uint8_t)txn;
	message[2] = (uint8_t)(txn >> 8);
	message[3] = SNS_CLIENT_REQ;
	message[4] = 0;
	message[5] = (uint8_t)qmi_length;
	message[6] = (uint8_t)(qmi_length >> 8);
	message[7] = 0x01;
	message[8] = (uint8_t)(2 + o.length);
	message[9] = (uint8_t)((2 + o.length) >> 8);
	message[10] = (uint8_t)o.length;
	message[11] = (uint8_t)(o.length >> 8);
	error = redwood_adsp_qrtr_send(port, service_node, service_port, message, 7 + qmi_length);
	if (error != 0) {
		say("sensors: a request of message %u from our port %#x NOT SENT (%d)\n", msg_id, port, error);
		return 0;
	}
	return txn;
}

/* ---- events -------------------------------------------------------------------------------------------------- */

static void
hex_line(const char *what, const uint8_t *data, uint32_t length)
{
	char line[200];
	uint32_t at = 0;

	for (uint32_t i = 0; i < length && at + 4 < sizeof(line); i++) {
		at += (uint32_t)snprintf(line + at, sizeof(line) - at, "%02x", data[i]);
	}
	say("sensors: %s (%u bytes): %s%s\n", what, length, line, at + 4 >= sizeof(line) ? "..." : "");
}

/* One attribute's values, written after what `line' has. */
static void
attribute_values(char *line, size_t size, const uint8_t *data, uint32_t length, unsigned depth)
{
	struct pb_in in = { data, length, 0 };
	struct pb_field f;
	size_t at = strlen(line);
	char number[32];

	while (pb_next(&in, &f) && at + 8 < size) {
		struct pb_in value = { f.bytes, f.length, 0 };
		struct pb_field v;

		if (f.number != 1 || f.wire != PB_BYTES) {
			continue;
		}
		while (pb_next(&value, &v) && at + 8 < size) {
			switch (v.number) {
			case 1:         /* a list of its own */
				if (depth < 2) {
					strlcat(line, " [", size);
					attribute_values(line, size, v.bytes, v.length, depth + 1);
					strlcat(line, " ]", size);
				}
				break;
			case 2:         /* a string */
				at = strlen(line);
				snprintf(line + at, size - at, " \"%.*s\"", (int)(v.length < 48 ? v.length : 48), v.bytes);
				break;
			case 3:         /* a float */
				at = strlen(line);
				snprintf(line + at, size - at, " %s", milli(number, sizeof(number),
				    float_scaled((uint32_t)v.value, 1000)));
				break;
			case 4:         /* a signed integer (zigzag) */
				at = strlen(line);
				snprintf(line + at, size - at, " %lld", (long long)((v.value >> 1) ^ -(v.value & 1)));
				break;
			case 5:         /* a truth */
				strlcat(line, v.value ? " true" : " false", size);
				break;
			default:
				break;
			}
			at = strlen(line);
		}
	}
}

static void
attribute_event(const char *type, const uint8_t *data, uint32_t length)
{
	struct pb_in in = { data, length, 0 };
	struct pb_field f;
	unsigned count = 0;

	while (pb_next(&in, &f)) {
		struct pb_in attribute;
		struct pb_field a;
		uint32_t id = ~0u;
		const uint8_t *value = NULL;
		uint32_t value_length = 0;
		char line[256];

		if (f.number != 1 || f.wire != PB_BYTES) {
			continue;
		}
		attribute = (struct pb_in){ f.bytes, f.length, 0 };
		while (pb_next(&attribute, &a)) {
			if (a.number == 1 && a.wire == PB_VARINT) {
				id = (uint32_t)a.value;
			} else if (a.number == 2 && a.wire == PB_BYTES) {
				value = a.bytes;
				value_length = a.length;
			}
		}
		snprintf(line, sizeof(line), "%s",
		    id < sizeof(attribute_names) / sizeof(attribute_names[0]) ? attribute_names[id] : "?");
		if (value != NULL) {
			attribute_values(line, sizeof(line), value, value_length, 0);
		}
		say("sensors: %s: attribute %u %s\n", type, id, line);
		count++;
	}
	say("sensors: %s: %u attributes\n", type, count);
}

static struct {
	const char *type;
	struct suid suids[4];
	unsigned count;
	bool answered;
} lookup;

static void
suid_event(const uint8_t *data, uint32_t length)
{
	struct pb_in in = { data, length, 0 };
	struct pb_field f;
	char type[33] = "";

	while (pb_next(&in, &f)) {
		if (f.number == 1 && f.wire == PB_BYTES) {
			memcpy(type, f.bytes, f.length < 32 ? f.length : 32);
		} else if (f.number == 2 && f.wire == PB_BYTES && lookup.type != NULL && strcmp(type, lookup.type) == 0 &&
		    lookup.count < 4) {
			struct pb_in s = { f.bytes, f.length, 0 };
			struct pb_field half;
			struct suid suid = { 0, 0 };

			while (pb_next(&s, &half)) {
				if (half.number == 1) {
					suid.low = half.value;
				} else if (half.number == 2) {
					suid.high = half.value;
				}
			}
			lookup.suids[lookup.count++] = suid;
		}
	}
	if (lookup.type != NULL && strcmp(type, lookup.type) == 0) {
		lookup.answered = true;
	}
}

/* /dev/shennong-motion: the samples, as records */
struct motion_record {
	uint32_t sensor;                        /* 1 accelerometer (micro-m/s^2), 2 gyroscope (micro-rad/s) */
	uint32_t status;                        /* 0 unreliable .. 3 accurate */
	int32_t value[3];                       /* x, y, z in Android's axes */
	uint32_t reserved;
	uint64_t time;                          /* the DSP's QTimer, 19.2 MHz */
};
#define MOTION_QUEUE            1024    /* records; a power of two */
static struct motion_record motion_queue[MOTION_QUEUE];
static uint32_t motion_head, motion_tail, motion_lost;
static LCK_MTX_DECLARE(motion_lock, &sensors_lock_group);

static void
motion_post(unsigned which, uint32_t status, const uint32_t *bits, uint64_t time)
{
	struct motion_record r = { which + 1, status, { 0, 0, 0 }, 0, time };

	for (int i = 0; i < 3; i++) {
		int64_t v = float_scaled(bits[i], 1000000);

		r.value[i] = v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
	}
	lck_mtx_lock(&motion_lock);
	if (motion_tail - motion_head >= MOTION_QUEUE) {
		motion_head++;                  /* (the oldest gives way) */
		motion_lost++;
	}
	motion_queue[motion_tail++ % MOTION_QUEUE] = r;
	lck_mtx_unlock(&motion_lock);
}

static void
sensor_event(unsigned which, uint64_t time, const uint8_t *data, uint32_t length)
{
	struct pb_in in = { data, length, 0 };
	struct pb_field f;
	uint32_t bits[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }, status = 0;
	unsigned n = 0, most = LIGHT(which) ? 8 : 3;

	while (pb_next(&in, &f)) {
		if (f.number == 1 && f.wire == PB_FIXED32 && n < most) {
			bits[n++] = (uint32_t)f.value;
		} else if (f.number == 1 && f.wire == PB_BYTES) {          /* (packed) */
			for (uint32_t at = 0; at + 4 <= f.length && n < most; at += 4) {
				bits[n++] = (uint32_t)f.bytes[at] | (uint32_t)f.bytes[at + 1] << 8 |
				    (uint32_t)f.bytes[at + 2] << 16 | (uint32_t)f.bytes[at + 3] << 24;
			}
		} else if (f.number == 2 && f.wire == PB_VARINT) {
			status = (uint32_t)f.value;
		}
	}
	if (n < sensor_values[which]) {
		return;
	}
	sensor[which].events++;
	sensor[which].status = status;
	memcpy(sensor[which].last_bits, bits, sizeof(sensor[which].last_bits));
	sensor[which].last_time = time;
	if (LIGHT(which)) {
		int64_t mlux = float_scaled(bits[0], 1000);
		char a[24], b[24];

		if (which == SENSOR_CCT_FRONT && n >= 8) {
			static uint64_t said_at;
			int64_t counts[4];
			bool say_it = now_ms() - said_at >= 2000;

			for (unsigned i = 0; i < 4; i++) {
				counts[i] = float_scaled(bits[4 + i], 1000);
			}
			mlux = front_lux(counts, say_it);
			if (say_it) {
				said_at = now_ms();
			}
		}
		if (which == SENSOR_ALS + als_from) {
			als_mlux = mlux < 0 ? 0 : mlux > INT32_MAX ? INT32_MAX : (int)mlux;
		}
		/*
		 * the proximity sensor's raw reflection (proximity_cal_strm's first value: some 45 with nothing in front):
		 * near above 123, far again below 77 (persist's tcs3720_fb_platform.prox.fac_cal). Xiaomi's "proximity" itself
		 * answers its stream with an error (2).
		 */
		if (which == SENSOR_PROX_CAL) {
			int64_t count = float_scaled(bits[0], 1);
			int was = prox_near;

			if (count > 123) {
				prox_near = 1;
			} else if (count < 77) {
				prox_near = 0;
			} else if (prox_near < 0) {
				prox_near = 0;
			}
			if (prox_near != was) {
				say("sensors: proximity: %s (reflection %lld)\n", prox_near ? "NEAR" : "far", count);
			}
		}
		if (als_events_said[which]++ < 12) {
			char line[200] = "";

			for (unsigned i = 1; i < n; i++) {
				size_t l = strlen(line);

				snprintf(line + l, sizeof(line) - l, " %s", milli(b, sizeof(b), float_scaled(bits[i], 1000)));
			}
			say("sensors: %s: status %u; all %u values: %s%s\n", sensor_types[which], status, n, milli(a, sizeof(a), mlux),
			    line);
		}
		return;
	}
	motion_post(which, status, bits, time);
}

/* What one of our ports has been sent: a QMI response or indication of the sensors' service. */
static void
received(const struct rx *r)
{
	const uint8_t *d = r->data;
	uint32_t qmi_length, at = 7;
	uint16_t txn, msg;
	uint8_t type;
	uint64_t client = 0;
	const uint8_t *payload = NULL;
	uint32_t payload_length = 0, result = 0, qmi_result = 0, qmi_error = 0;
	bool have_result = false, have_client = false;
	int from = -1;
	struct pb_in in;
	struct pb_field f;

	if (r->length < 7) {
		return;
	}
	type = d[0];
	txn = (uint16_t)(d[1] | d[2] << 8);
	msg = (uint16_t)(d[3] | d[4] << 8);
	qmi_length = (uint32_t)(d[5] | d[6] << 8);
	if (7 + qmi_length > r->length) {
		say("sensors: a QMI message of %u bytes in a datagram of %u: dropped\n", qmi_length, r->length);
		return;
	}
	while (at + 3 <= 7 + qmi_length) {
		uint8_t t = d[at];
		uint32_t l = (uint32_t)(d[at + 1] | d[at + 2] << 8);
		const uint8_t *v = d + at + 3;

		if (at + 3 + l > 7 + qmi_length) {
			break;
		}
		if (type == QMI_RESPONSE && t == 0x02 && l >= 4) {
			qmi_result = (uint32_t)(v[0] | v[1] << 8);
			qmi_error = (uint32_t)(v[2] | v[3] << 8);
		} else if (type == QMI_RESPONSE && t == 0x10 && l >= 8) {
			memcpy(&client, v, 8);
			have_client = true;
		} else if (type == QMI_RESPONSE && t == 0x11 && l >= 4) {
			memcpy(&result, v, 4);
			have_result = true;
		} else if (type == QMI_INDICATION && t == 0x01 && l >= 8) {
			memcpy(&client, v, 8);
		} else if (type == QMI_INDICATION && t == 0x02 && l >= 2) {
			payload_length = (uint32_t)(v[0] | v[1] << 8);
			payload = v + 2;
			if (payload_length > l - 2) {
				payload_length = l - 2;
			}
		}
		at += 3 + l;
	}
	if (r->port >= OUR_PORT_STREAM && r->port < OUR_PORT_STREAM + SENSORS) {
		from = (int)(r->port - OUR_PORT_STREAM);
	}
	if (type == QMI_RESPONSE) {
		if (qmi_result != 0 || (have_result && result != 0) || mode >= 2 || from >= 0) {
			say("sensors: < response to %u (message %#x) on our port %#x: QMI %s (error %u)%s%llx, the framework's result %u%s\n",
			    txn, msg, r->port, qmi_result == 0 ? "success" : "FAILURE", qmi_error, have_client ? ", client " : "",
			    have_client ? client : 0ull, result, have_result && result != 0 ? " (NOT 0: refused)" : "");
		}
		if (from >= 0 && have_client) {
			sensor[from].client = client;
		}
		return;
	}
	if (type != QMI_INDICATION || (msg != SNS_CLIENT_REPORT_IND && msg != SNS_CLIENT_JUMBO_IND) || payload == NULL) {
		say("sensors: < QMI type %u message %#x (%u bytes) on our port %#x: not ours to read\n", type, msg, qmi_length,
		    r->port);
		return;
	}
	/* sns_client_event_msg: the SUID it is from, then events */
	in = (struct pb_in){ payload, payload_length, 0 };
	while (pb_next(&in, &f)) {
		struct pb_in event;
		struct pb_field e;
		uint32_t id = 0;
		uint64_t time = 0;
		const uint8_t *body = NULL;
		uint32_t body_length = 0;

		if (f.number != 2 || f.wire != PB_BYTES) {
			continue;
		}
		event = (struct pb_in){ f.bytes, f.length, 0 };
		while (pb_next(&event, &e)) {
			if (e.number == 1) {
				id = (uint32_t)e.value;
			} else if (e.number == 2) {
				time = e.value;
			} else if (e.number == 3 && e.wire == PB_BYTES) {
				body = e.bytes;
				body_length = e.length;
			}
		}
		if (from >= 0 && mode == 3 && sensor[from].events_shown < 16) {
			char what[48];

			sensor[from].events_shown++;
			snprintf(what, sizeof(what), "%s event %u", sensor_types[from], id);
			hex_line(what, body != NULL ? body : (const uint8_t *)"", body_length);
		}
		if (from >= 0 && id == MSG_PROXIMITY_EVENT && body != NULL) {
			proximity_event((unsigned)from, body, body_length);
		} else if (from >= 0 && id == MSG_SENSOR_EVENT && body != NULL) {
			sensor_event((unsigned)from, time, body, body_length);
		} else if (from < 0 && id == MSG_SUID_EVENT && body != NULL) {
			suid_event(body, body_length);
		} else if (id == MSG_ATTR_EVENT && body != NULL) {
			attribute_event(from >= 0 ? sensor_types[from] : lookup.type != NULL ? lookup.type : "?", body, body_length);
			lookup.answered = true;
		} else if (from >= 0 && id == MSG_PHYSICAL_CONFIG && body != NULL) {
			struct pb_in config = { body, body_length, 0 };
			struct pb_field c;
			char line[200] = "", number[32];

			while (pb_next(&config, &c)) {
				size_t l = strlen(line);

				if (c.wire == PB_FIXED32) {
					snprintf(line + l, sizeof(line) - l, " %u:%s", c.number, milli(number, sizeof(number),
					    float_scaled((uint32_t)c.value, 1000)));
				} else if (c.wire == PB_VARINT) {
					snprintf(line + l, sizeof(line) - l, " %u:%llu", c.number, c.value);
				}
			}
			say("sensors: %s: THE CHIP IS SET UP (physical config:%s)\n", sensor_types[from], line);
		} else if (id == MSG_ERROR_EVENT) {
			uint64_t code = 0;
			struct pb_in err = { body, body_length, 0 };
			struct pb_field c;

			if (pb_next(&err, &c)) {
				code = c.value;
			}
			say("sensors: %s: AN ERROR EVENT from the framework, error %llu\n", from >= 0 ? sensor_types[from] : "lookup", code);
		} else if (id != MSG_FLUSH_EVENT && mode >= 2) {
			say("sensors: %s: event %u (%u bytes) not read\n", from >= 0 ? sensor_types[from] : "lookup", id, body_length);
		}
	}
}

/* Everything the DSP has sent until `ms' have passed or `done' says enough. */
static void
pump(uint32_t ms, volatile bool *done)
{
	uint64_t until = now_ms() + ms;
	struct rx *r = kalloc_data(sizeof(*r), Z_WAITOK);

	if (r == NULL) {
		return;
	}
	while (wanted && now_ms() < until && (done == NULL || !*done)) {
		if (rx_take(r)) {
			received(r);
		} else {
			sleep_ms(2);
		}
	}
	kfree_data(r, sizeof(*r));
}

/* ---- the steps ---------------------------------------------------------------------------------------------- */

/* The SUIDs of a data type (the default one only); how many. */
static unsigned
look_up(const char *type, bool quiet)
{
	uint8_t req[64];
	struct pb_out o = { req, 0, sizeof(req), false };
	struct suid lookup_suid = { SUID_LOOKUP, SUID_LOOKUP };

	pb_bytes(&o, 1, type, (uint32_t)strlen(type));
	pb_uint(&o, 2, 1);                      /* register_updates: a sensor that comes later is told too */
	pb_uint(&o, 3, 1);                      /* default_only */
	bzero(&lookup, sizeof(lookup));
	lookup.type = type;
	if (request(OUR_PORT_LOOKUP, lookup_suid, MSG_SUID_REQ, req, o.length) == 0) {
		return 0;
	}
	pump(3000, &lookup.answered);
	if (!lookup.answered) {
		say("sensors: \"%s\": NO ANSWER from the lookup in 3 s\n", type);
	} else if (lookup.count == 0) {
		if (!quiet) {
			say("sensors: \"%s\": THE FRAMEWORK HAS NONE (no SUID)\n", type);
		}
	} else {
		for (unsigned i = 0; i < lookup.count; i++) {
			say("sensors: \"%s\": SUID %016llx%016llx\n", type, lookup.suids[i].high, lookup.suids[i].low);
		}
	}
	return lookup.count;
}

static void
attributes(const char *type, struct suid suid)
{
	bzero(&lookup, sizeof(lookup));
	lookup.type = type;
	if (request(OUR_PORT_LOOKUP, suid, MSG_ATTR_REQ, NULL, 0) == 0) {
		return;
	}
	pump(2000, &lookup.answered);
	if (!lookup.answered) {
		say("sensors: %s: NO ATTRIBUTES in 2 s\n", type);
	}
	lookup.type = NULL;
}

static void
stream(unsigned which, bool on)
{
	uint8_t config[8];
	struct pb_out o = { config, 0, sizeof(config), false };

	if (!on) {
		if (sensor[which].streaming) {
			(void)request(OUR_PORT_STREAM + which, sensor[which].suid, MSG_DISABLE_REQ, NULL, 0);
			sensor[which].streaming = false;
		}
		return;
	}
	if (LIGHT(which) && als_hz != 0) {
		/* (asked for as a stream: with only its changes asked for it gave one reading, 0 lux, and no more) */
		pb_fixed32(&o, 1, float_bits(als_hz));
		if (request(OUR_PORT_STREAM + which, sensor[which].suid, MSG_SENSOR_CONFIG, config, o.length) != 0) {
			sensor[which].streaming = true;
			say("sensors: %s: its stream asked for at %u Hz\n", sensor_types[which], als_hz);
		}
		return;
	}
	if (LIGHT(which)) {
		if (request(OUR_PORT_STREAM + which, sensor[which].suid, MSG_ON_CHANGE_CONFIG, NULL, 0) != 0) {
			sensor[which].streaming = true;
			say("sensors: %s: its changes asked for\n", sensor_types[which]);
		}
		return;
	}
	pb_fixed32(&o, 1, float_bits(rate_hz));
	if (request(OUR_PORT_STREAM + which, sensor[which].suid, MSG_SENSOR_CONFIG, config, o.length) != 0) {
		sensor[which].streaming = true;
		say("sensors: %s: its stream asked for at %u Hz\n", sensor_types[which], rate_hz);
	}
}

static const char *const extra_types[] = {
	"mag", "pressure", "proximity", "ambient_light", "sensor_temperature", "gravity", "rotv", "game_rv",
	"device_orient", "sig_motion", "accel_cal", "gyro_cal", "registry", "hinge_angle",
};

static void
sensors_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t last_line = now_ms();
	uint32_t last_events[SENSORS] = { 0 };
	unsigned found = 0;

	for (unsigned ms = 0; ms < 10000 && wanted && !redwood_adsp_qrtr_service(SNS_CLIENT_SERVICE, &service_node,
	    &service_port); ms += 100) {
		sleep_ms(100);
	}
	if (!redwood_adsp_qrtr_service(SNS_CLIENT_SERVICE, &service_node, &service_port)) {
		say("sensors: THE DSP HAS NOT TOLD OF THE SENSORS' SERVICE (400) in 10 s: nothing is asked\n");
		goto out;
	}
	say("sensors: the sensors' service (400) is at node %u port %u; our ports %#x (lookups) and %#x.. (streams)\n",
	    service_node, service_port, OUR_PORT_LOOKUP, OUR_PORT_STREAM);
	bool ports = redwood_adsp_qrtr_port(OUR_PORT_LOOKUP, rx_input);

	for (unsigned s = 0; s < SENSORS; s++) {
		ports = ports && redwood_adsp_qrtr_port(OUR_PORT_STREAM + s, rx_input);
	}
	if (!ports) {
		say("sensors: no room for our QRTR ports\n");
		goto out;
	}
	if (mode == 2) {
		for (unsigned i = 0; i < sizeof(extra_types) / sizeof(extra_types[0]) && wanted; i++) {
			if (look_up(extra_types[i], false) != 0) {
				attributes(extra_types[i], lookup.suids[0]);
			}
		}
	}
	for (unsigned s = 0; s < SENSORS && wanted; s++) {
		if (look_up(sensor_types[s], false) == 0) {
			continue;
		}
		sensor[s].suid = lookup.suids[0];
		sensor[s].found = true;
		found++;
		attributes(sensor_types[s], sensor[s].suid);
	}
	if (found == 0) {
		say("sensors: NEITHER ACCELEROMETER NOR GYROSCOPE in the framework: its registry and the chips' configuration "
		    "come from Android's files through FastRPC (sscrpcd), which this kernel does not serve\n");
		goto out;
	}
	for (unsigned s = 0; s < SENSORS && wanted; s++) {
		if (sensor[s].found) {
			stream(s, true);
		}
	}
	while (wanted) {
		pump(100, NULL);
		if (log_seconds != 0 && now_ms() - last_line >= log_seconds * 1000ull) {
			char line[256] = "sensors:", a[24], b[24], c[24];

			for (unsigned s = 0; s <= SENSOR_ALS; s++) {
				size_t l = strlen(line);
				uint32_t *bits = sensor[s].last_bits;

				if (!sensor[s].found) {
					continue;
				}
				snprintf(line + l, sizeof(line) - l, " %s %s %s %s %s (%u/s, status %u);", sensor_types[s],
				    milli(a, sizeof(a), float_scaled(bits[0], 1000)), milli(b, sizeof(b), float_scaled(bits[1], 1000)),
				    milli(c, sizeof(c), float_scaled(bits[2], 1000)), sensor_units[s],
				    (uint32_t)((sensor[s].events - last_events[s]) * 1000ull / (now_ms() - last_line)), sensor[s].status);
				last_events[s] = sensor[s].events;
			}
			if (rx_dropped || rx_too_long || motion_lost) {
				size_t l = strlen(line);

				snprintf(line + l, sizeof(line) - l, " dropped %u, too long %u, records lost %u", rx_dropped, rx_too_long,
				    motion_lost);
			}
			say("%s\n", line);
			last_line = now_ms();
		}
	}
out:
	for (unsigned s = 0; s < SENSORS; s++) {
		stream(s, false);
	}
	if (found != 0) {
		pump(200, NULL);                /* (the disables' responses) */
	}
	redwood_adsp_qrtr_port(OUR_PORT_LOOKUP, NULL);
	for (unsigned s = 0; s < SENSORS; s++) {
		redwood_adsp_qrtr_port(OUR_PORT_STREAM + s, NULL);
	}
	say("sensors: over: ambient light %u events, ", sensor[SENSOR_ALS].events);
	say("sensors: over: accel %u events, gyro %u\n", sensor[SENSOR_ACCEL].events, sensor[SENSOR_GYRO].events);
	runs = false;
	thread_terminate(current_thread());
}

/* ---- /dev/shennong-motion ------------------------------------------------------------------------------------ */

static int
motion_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
motion_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
motion_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int error = 0;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(struct motion_record) && error == 0) {
		struct motion_record r;

		lck_mtx_lock(&motion_lock);
		if (motion_head == motion_tail) {
			lck_mtx_unlock(&motion_lock);
			break;
		}
		r = motion_queue[motion_head++ % MOTION_QUEUE];
		lck_mtx_unlock(&motion_lock);
		error = uiomove((caddr_t)&r, sizeof(r), uio);
	}
	return error;
}

static const struct cdevsw motion_cdevsw = {
	.d_open = motion_open, .d_close = motion_close, .d_read = motion_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* ---- from redwood_adsp.c ------------------------------------------------------------------------------------- */

void
shennong_sensors_dsp_up(void)
{
	static bool device_made;
	thread_t thread;

	if (!PE_parse_boot_argn("gyro", &mode, sizeof(mode)) || mode == 0 || runs) {
		return;
	}
	PE_parse_boot_argn("gyro_hz", &rate_hz, sizeof(rate_hz));
	PE_parse_boot_argn("gyro_log", &log_seconds, sizeof(log_seconds));
	PE_parse_boot_argn("als_hz", &als_hz, sizeof(als_hz));
	PE_parse_boot_argn("als_from", &als_from, sizeof(als_from));
	if (SENSOR_ALS + als_from >= SENSORS) {
		als_from = 0;
	}
	if (rate_hz == 0 || rate_hz > 500) {
		rate_hz = 50;
	}
	if (!device_made) {
		int major = cdevsw_add(-1, &motion_cdevsw);

		device_made = true;
		if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0444, "shennong-motion") ==
		    NULL) {
			say("sensors: cannot make /dev/shennong-motion\n");
		}
	}
	if (rx_ring == NULL) {
		struct rx *ring = kalloc_data(sizeof(struct rx) * RX_SLOTS, Z_WAITOK | Z_ZERO);

		lck_mtx_lock(&rx_lock);
		rx_ring = ring;
		rx_head = rx_tail = 0;
		lck_mtx_unlock(&rx_lock);
		if (ring == NULL) {
			say("sensors: no memory for what the DSP sends\n");
			return;
		}
	}
	bzero(sensor, sizeof(sensor));
	wanted = runs = true;
	if (kernel_thread_start(sensors_thread, NULL, &thread) != KERN_SUCCESS) {
		wanted = runs = false;
		say("sensors: no thread\n");
		return;
	}
	thread_deallocate(thread);
	say("sensors: gyro=%u: the accelerometer and the gyroscope asked of the DSP's sensors' framework, %u Hz\n", mode, rate_hz);
}

void
shennong_sensors_dsp_down(void)
{
	if (!runs) {
		return;
	}
	wanted = false;
	for (unsigned ms = 0; ms < 1500 && runs; ms += 10) {
		sleep_ms(10);
	}
	if (runs) {
		say("sensors: the sensors' thread has not ended in 1.5 s\n");
	}
}
