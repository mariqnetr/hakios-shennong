/*
 * libredwood-bt.dylib, inside bluetoothd on the Xiaomi 14 Pro. bluetoothd gives a HID device's reports to the kernel
 * through an IOHIDUserDevice (IOHIDFamily's resource driver, which this kernel has not got). So its calls are answered
 * here (dyld's interposing): the report descriptor says where a keyboard's keys are, and what changes between reports
 * goes to the input device as REDWOOD_KEY_DOWN/UP, which src/inject.c in backboardd gives to the event system.
 * Keyboards only, so far: other reports are noted in the log and dropped.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include "redwood_input.h"

typedef void *IOHIDUserDeviceRef;
extern IOHIDUserDeviceRef IOHIDUserDeviceCreateWithOptions(CFAllocatorRef, CFDictionaryRef, uint32_t);
extern void IOHIDUserDeviceScheduleWithDispatchQueue(IOHIDUserDeviceRef, dispatch_queue_t);
extern void IOHIDUserDeviceUnscheduleFromDispatchQueue(IOHIDUserDeviceRef, dispatch_queue_t);
extern void IOHIDUserDeviceRegisterSetReportCallback(IOHIDUserDeviceRef, void *, void *);
extern void IOHIDUserDeviceRegisterGetReportWithReturnLengthCallback(IOHIDUserDeviceRef, void *, void *);
extern int IOHIDUserDeviceHandleReportWithTimeStamp(IOHIDUserDeviceRef, uint64_t, const uint8_t *, CFIndex);

struct keyboard {
	int found;
	int has_ids;
	unsigned id;
	unsigned modifiers_bit, keys_bit, keys;         /* where they are in the report, after its id */
	uint8_t last_modifiers, last_keys[16];
	unsigned said;
};

static void
say(const char *format, ...)
{
	static int console = -2;
	char line[300];
	va_list list;
	int n;

	if (console == -2) {
		console = open("/dev/console", O_WRONLY);
	}
	va_start(list, format);
	n = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (console >= 0 && n > 0) {
		write(console, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
	}
}

static void
key(unsigned usage, int down)
{
	static int device = -2;
	struct redwood_touch t = { .type = down ? REDWOOD_KEY_DOWN : REDWOOD_KEY_UP, .x = usage, .y = 7 };

	if (device == -2) {
		device = open(REDWOOD_TOUCH_DEVICE, O_WRONLY);
		say("bt-hid: %s: %d\n", REDWOOD_TOUCH_DEVICE, device);
	}
	if (device >= 0) {
		write(device, &t, sizeof(t));
	}
}

/* Where a keyboard's modifier bits and key array are: the HID descriptor, as far as that needs. */
static void
descriptor(struct keyboard *k, const uint8_t *d, CFIndex length)
{
	unsigned page = 0, size = 0, count = 0, id = 0;
	unsigned bits[256] = { 0 };

	for (CFIndex i = 0; i < length;) {
		uint8_t item = d[i];
		unsigned n = item & 3, value = 0;

		n = n == 3 ? 4 : n;
		if (item == 0xfe || i + 1 + (CFIndex)n > length) {
			break;
		}
		for (unsigned b = 0; b < n; b++) {
			value |= (unsigned)d[i + 1 + b] << (8 * b);
		}
		switch (item & 0xfc) {
		case 0x04: page = value; break;
		case 0x74: size = value; break;
		case 0x94: count = value; break;
		case 0x84: id = value & 0xff; k->has_ids = 1; break;
		case 0x80:
			if (page == 7 && (value & 1) == 0 && size == 1 && count == 8) {
				k->found = 1;
				k->id = id;
				k->modifiers_bit = bits[id];
			} else if (page == 7 && (value & 1) == 0 && size == 8 && count >= 1) {
				k->found = 1;
				k->id = id;
				k->keys_bit = bits[id];
				k->keys = count > 16 ? 16 : count;
			}
			bits[id] += size * count;
			break;
		default:
			break;
		}
		i += 1 + n;
	}
	say("bt-hid: descriptor %ld bytes: keyboard %d, report id %u (ids %d), modifiers at bit %u, %u keys at bit %u\n",
	    (long)length, k->found, k->id, k->has_ids, k->modifiers_bit, k->keys, k->keys_bit);
}

static IOHIDUserDeviceRef
my_create(CFAllocatorRef allocator, CFDictionaryRef properties, uint32_t options)
{
	CFMutableDataRef device = CFDataCreateMutable(kCFAllocatorDefault, sizeof(struct keyboard));
	CFDataRef d = properties != NULL ? CFDictionaryGetValue(properties, CFSTR("ReportDescriptor")) : NULL;
	struct keyboard *k;

	CFDataSetLength(device, sizeof(struct keyboard));
	k = (struct keyboard *)CFDataGetMutableBytePtr(device);
	say("bt-hid: a HID device is made, options %#x\n", options);
	if (d != NULL) {
		descriptor(k, CFDataGetBytePtr(d), CFDataGetLength(d));
	}
	return (IOHIDUserDeviceRef)device;
}

static void my_schedule(IOHIDUserDeviceRef d, dispatch_queue_t q) { }
static void my_unschedule(IOHIDUserDeviceRef d, dispatch_queue_t q) { }
static void my_set_callback(IOHIDUserDeviceRef d, void *c, void *r) { }
static void my_get_callback(IOHIDUserDeviceRef d, void *c, void *r) { }

static int
my_report(IOHIDUserDeviceRef device, uint64_t timestamp, const uint8_t *report, CFIndex length)
{
	struct keyboard *k = (struct keyboard *)CFDataGetMutableBytePtr((CFMutableDataRef)device);
	const uint8_t *body = report;
	uint8_t modifiers, keys[16] = { 0 };

	if (k->said < 24) {
		k->said++;
		say("bt-hid: report %ld bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", (long)length,
		    length > 0 ? report[0] : 0, length > 1 ? report[1] : 0, length > 2 ? report[2] : 0, length > 3 ? report[3] : 0,
		    length > 4 ? report[4] : 0, length > 5 ? report[5] : 0, length > 6 ? report[6] : 0, length > 7 ? report[7] : 0,
		    length > 8 ? report[8] : 0);
	}
	if (!k->found || k->keys == 0) {
		return 0;
	}
	if (k->has_ids) {
		if (length < 1 || report[0] != k->id) {
			return 0;
		}
		body++;
		length--;
	}
	if ((CFIndex)(k->keys_bit / 8 + k->keys) > length || (CFIndex)(k->modifiers_bit / 8) >= length) {
		return 0;
	}
	modifiers = body[k->modifiers_bit / 8];
	memcpy(keys, body + k->keys_bit / 8, k->keys);
	for (unsigned b = 0; b < 8; b++) {
		if ((modifiers ^ k->last_modifiers) & (1u << b)) {
			key(0xe0 + b, (modifiers >> b) & 1);
		}
	}
	for (unsigned i = 0; i < k->keys; i++) {
		if (k->last_keys[i] >= 4 && memchr(keys, k->last_keys[i], k->keys) == NULL) {
			key(k->last_keys[i], 0);
		}
	}
	for (unsigned i = 0; i < k->keys; i++) {
		if (keys[i] >= 4 && memchr(k->last_keys, keys[i], k->keys) == NULL) {
			key(keys[i], 1);
		}
	}
	k->last_modifiers = modifiers;
	memcpy(k->last_keys, keys, sizeof(keys));
	return 0;
}

__attribute__((used)) static const struct {
	const void *replacement, *original;
} interposed[] __attribute__((section("__DATA,__interpose"))) = {
	{ my_create, IOHIDUserDeviceCreateWithOptions },
	{ my_schedule, IOHIDUserDeviceScheduleWithDispatchQueue },
	{ my_unschedule, IOHIDUserDeviceUnscheduleFromDispatchQueue },
	{ my_set_callback, IOHIDUserDeviceRegisterSetReportCallback },
	{ my_get_callback, IOHIDUserDeviceRegisterGetReportWithReturnLengthCallback },
	{ my_report, IOHIDUserDeviceHandleReportWithTimeStamp },
};
