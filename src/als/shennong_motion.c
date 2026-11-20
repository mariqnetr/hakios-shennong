/*
 * The Xiaomi 14 Pro's accelerometer and gyroscope for iOS, inside backboardd (part of libshennong-als.dylib).
 *
 * The kernel streams them from the audio DSP's sensors' framework (boot-args snsrpc=1 gyro=1,
 * bsd/dev/shennong_sensors.c) into /dev/shennong-motion: records of 32 bytes, the sensor (1 accelerometer in
 * micro-m/s^2, 2 gyroscope in micro-rad/s), how reliable, x y z in Android's axes, the DSP's time; reading never
 * waits. backboardd takes the accelerometer for the device's orientation (the screen's rotation: its
 * "[OrientationDevice] missing accel HID service plugin" without one) from a built-in HID service of Apple's vendor
 * page 0xff00, usage 3, and the gyroscope from usage 9 (its BKIOHIDServiceMatchers, 20F66); CoreMotion takes them from
 * the same services. Those two are made here as virtual services in the HID event server, as the light sensor's
 * is (shennong_als.c), and fed every record:
 *   accelerometer events (kIOHIDEventTypeAccelerometer) in g, as Apple's report them: what the device feels is
 *     against gravity, -1 g along z lying face up where Android says +9.8 m/s^2 -- so -value / 9.80665
 *   gyroscope events (kIOHIDEventTypeGyro) in rad/s, the same axes and sense as Android's
 * Said on the console: "shennong-motion: ...".
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const char *getprogname(void);

typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

struct callbacks {
	uint64_t version;
	void (*notification)(void *, void *, IOHIDServiceClientRef, uint32_t, CFDictionaryRef);
	Boolean (*setProperty)(void *, void *, IOHIDServiceClientRef, CFStringRef, CFTypeRef);
	CFTypeRef (*copyProperty)(void *, void *, IOHIDServiceClientRef, CFStringRef);
	IOHIDEventRef (*copyEvent)(void *, void *, IOHIDServiceClientRef, uint32_t, IOHIDEventRef, uint32_t);
	int (*setOutputEvent)(void *, void *, IOHIDServiceClientRef, IOHIDEventRef);
	CFTypeRef (*copyMatchingEvent)(void *, void *, IOHIDServiceClientRef, CFDictionaryRef);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client,
    CFDictionaryRef attributes, const struct callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateAccelerometerEvent(CFAllocatorRef allocator, uint64_t time, double x, double y, double z,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateGyroEvent(CFAllocatorRef allocator, uint64_t time, double x, double y, double z,
    uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);
extern IOHIDEventRef IOHIDEventCreateProximtyEvent(CFAllocatorRef allocator, uint64_t time, uint32_t mask, uint32_t options);

#define HID_PAGE_APPLE_VENDOR   0xff00
#define USAGE_ACCELEROMETER     3
#define USAGE_GYRO              9
#define EVENT_ACCELEROMETER     13
#define EVENT_GYRO              20
#define FIELD_IS_BUILT_IN       0x4
#define MOTION_DEVICE           "/dev/shennong-motion"

struct motion_record {
	uint32_t sensor, status;
	int32_t value[3];
	uint32_t reserved;
	uint64_t time;
};

/* one of our services: its properties, whether the server has opened it, its latest event */
struct service {
	const char *name;
	uint32_t usage, event_type;
	CFMutableDictionaryRef properties;
	volatile bool opened;
	IOHIDEventRef last;
	pthread_mutex_t lock;
	unsigned sent;
};

static struct service accel = { "accelerometer", USAGE_ACCELEROMETER, EVENT_ACCELEROMETER, NULL, false, NULL,
	PTHREAD_MUTEX_INITIALIZER, 0 };
static struct service gyro = { "gyroscope", USAGE_GYRO, EVENT_GYRO, NULL, false, NULL, PTHREAD_MUTEX_INITIALIZER, 0 };
/*
 * The proximity sensor: backboardd's +[BKHIDUISensorConfiguratorFactory _proximityServiceWrapper] matches page
 * 0xff00 usage 8, and BKHIDProximityEventProcessor reads kIOHIDEventTypeProximity's detection mask (field 0xe0000):
 * near is sent as a large body in contact and the receiver's crude and fine detections (0x1 | 0x40 | 0x80), far as 0.
 * From the kernel's kern.redwood.prox (patch 0472), polled with the motion records; sent when it changes, and once
 * a second all the same.
 */
#define USAGE_PROXIMITY         8
#define EVENT_PROXIMITY         14
#define PROX_NEAR_MASK          (0x1 | 0x40 | 0x80)
static struct service prox = { "proximity sensor", USAGE_PROXIMITY, EVENT_PROXIMITY, NULL, false, NULL,
	PTHREAD_MUTEX_INITIALIZER, 0 };

static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[256];
	va_list list;
	int n;

	va_start(list, format);
	n = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

static CFTypeRef
copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef client, CFStringRef key)
{
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	return value ? CFRetain(value) : NULL;
}

static Boolean
set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef client, CFStringRef key, CFTypeRef value)
{
	struct service *s = refcon;
	static int told;

	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		s->opened = true;
	}
	if (key != NULL && told++ < 32) {
		char name[96] = "?", text[96] = "?";
		CFStringRef description = value ? CFCopyDescription(value) : NULL;

		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("shennong-motion: %s: set \"%s\" = %s\n", s->name, name, text);
	}
	return true;
}

static void
notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef client, __unused uint32_t type,
    __unused CFDictionaryRef property)
{
}

static IOHIDEventRef
copy_event(__unused void *target, void *refcon, __unused IOHIDServiceClientRef client, uint32_t type,
    __unused IOHIDEventRef matching, __unused uint32_t options)
{
	struct service *s = refcon;
	IOHIDEventRef event = NULL;

	if (type != s->event_type) {
		return NULL;
	}
	pthread_mutex_lock(&s->lock);
	if (s->last != NULL) {
		event = (IOHIDEventRef)CFRetain(s->last);
	}
	pthread_mutex_unlock(&s->lock);
	return event;
}

static const struct callbacks callbacks = {
	.version = 2, .notification = notification, .setProperty = set_property, .copyProperty = copy_property,
	.copyEvent = copy_event,
};

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

static IOHIDServiceClientRef
make(IOHIDEventSystemClientRef client, struct service *s, const char *product)
{
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFNumberRef page = number(HID_PAGE_APPLE_VENDOR), usage = number((int)s->usage), zero = number(0);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	IOHIDServiceClientRef service = NULL;
	CFArrayRef pairs;

	s->properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(s->properties, CFSTR("PrimaryUsagePage"), page);
	CFDictionarySetValue(s->properties, CFSTR("PrimaryUsage"), usage);
	CFDictionarySetValue(s->properties, CFSTR("DeviceUsagePage"), page);
	CFDictionarySetValue(s->properties, CFSTR("DeviceUsage"), usage);
	CFDictionarySetValue(s->properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(s->properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(s->properties, CFSTR("Transport"), CFSTR("SPU"));
	CFDictionarySetValue(s->properties, CFSTR("Manufacturer"), CFSTR("STMicro"));
	CFDictionarySetValue(s->properties, CFSTR("Product"), name);
	CFDictionarySetValue(s->properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(s->properties, CFSTR("ProductID"), zero);
	if (s->usage == USAGE_PROXIMITY) {
		/* (backboardd takes a discrete proximity sensor only with Placement 1, the top; any other it calls not
		 * top placement, leaves out -- and then trips over the gap, -[NSNull applyUIMode:], and dies) */
		CFNumberRef top = number(1);

		CFDictionarySetValue(s->properties, CFSTR("Placement"), top);
	}
	for (int attempt = 1; attempt <= 20; attempt++) {
		s->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, s->properties, &callbacks, NULL, s);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !s->opened; tenths++) {
			usleep(100000);
		}
		if (s->opened) {
			break;
		}
		say("shennong-motion: the event server has not opened the %s (attempt %d): making it again\n", s->name, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

static void
dispatch_to(IOHIDServiceClientRef service, struct service *s, IOHIDEventRef event)
{
	if (event == NULL) {
		return;
	}
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(service, event);
	pthread_mutex_lock(&s->lock);
	if (s->last != NULL) {
		CFRelease(s->last);
	}
	s->last = event;
	pthread_mutex_unlock(&s->lock);
	s->sent++;
}

/*
 * The device's orientation. backboardd takes it from CoreMotion (-[... _queue_updateDeviceOrientationFromCoreMotion:],
 * "Updating device orientation from CoreMotion to: ..."), which lives in locationd -- a daemon this system does not
 * run. So it is made here from the accelerometer, as UIDeviceOrientation (1 portrait, 2 upside down, 3 landscape
 * left, 4 landscape right, 5 face up, 6 face down): the axis gravity is along when it is along one by 0.75 g or more,
 * held for a third of a second. backboardd's own calls of that method (it calls it, with "unknown", when orientation
 * events are turned on) tell which object it is and on which queue (its _queue) it is to be called; from then on
 * each new orientation is handed to it there.
 */
typedef void (*observe_fn)(id self, SEL selector, id motion);
typedef void (*update_fn)(id self, SEL selector, long long orientation, long long source);
static observe_fn observe_original;
static id orientation_owner;
static dispatch_queue_t orientation_queue;
static volatile long long orientation_now;

/* -[BKOrientationManager _queue_updateDeviceOrientationFromCoreMotion:] (its argument an object): who and where */
static void
observe_hook(id self, SEL selector, id motion)
{
	if (orientation_owner == NULL) {
		Ivar ivar = class_getInstanceVariable(object_getClass(self), "_queue");

		orientation_owner = self;
		orientation_queue = ivar != NULL ? (dispatch_queue_t)object_getIvar(self, ivar) : NULL;
		say("shennong-motion: orientations go to %s %p on its queue %p\n", class_getName(object_getClass(self)),
		    (void *)self, (void *)orientation_queue);
	}
	observe_original(self, selector, motion);
	/* (CoreMotion's "unknown" is not the last word when the accelerometer knows better: on this queue already) */
	if (orientation_now != 0) {
		((update_fn)objc_msgSend)(self, sel_registerName("_queue_updateDeviceOrientation:changeSource:"), orientation_now, 1);
	}
}

static void
orientation_fix(void)
{
	Class manager = objc_getClass("BKOrientationManager");
	SEL observe = sel_registerName("_queue_updateDeviceOrientationFromCoreMotion:");
	Method method = manager != NULL ? class_getInstanceMethod(manager, observe) : NULL;
	Method update = manager != NULL ? class_getInstanceMethod(manager,
	    sel_registerName("_queue_updateDeviceOrientation:changeSource:")) : NULL;
	const char *types = update != NULL ? method_getTypeEncoding(update) : NULL;

	if (method == NULL || types == NULL || strncmp(types, "v32@0:8q16q24", 13) != 0) {
		say("shennong-motion: BKOrientationManager is not as expected (%s): no orientation\n", types ? types : "?");
		return;
	}
	observe_original = (observe_fn)method_setImplementation(method, (IMP)observe_hook);
	say("shennong-motion: BKOrientationManager hooked: orientations from the accelerometer, as from CoreMotion\n");
}

static void
orientation_feed(double x, double y, double z)
{
	static long long candidate;
	static uint64_t since;
	static mach_timebase_info_data_t timebase;
	long long o = 0;
	uint64_t now = mach_absolute_time();

	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
	}
	if (fabs(y) >= 0.75 && fabs(y) >= fabs(x) && fabs(y) >= fabs(z)) {
		o = y < 0 ? 1 : 2;
	} else if (fabs(x) >= 0.75 && fabs(x) >= fabs(z)) {
		o = x < 0 ? 3 : 4;
	} else if (fabs(z) >= 0.75) {
		o = z < 0 ? 5 : 6;
	}
	if (o == 0) {
		return;
	}
	if (o != candidate) {
		candidate = o;
		since = now;
		return;
	}
	if (o == orientation_now || (now - since) * timebase.numer / timebase.denom < 330000000ULL) {
		return;
	}
	orientation_now = o;
	say("shennong-motion: orientation %lld (%.2f %.2f %.2f g)\n", o, x, y, z);
	if (orientation_owner != NULL && orientation_queue != NULL) {
		id owner = orientation_owner;

		/* (as -_queue_updateDeviceOrientationFromCoreMotion: does with CoreMotion's: change source 1) */
		dispatch_async(orientation_queue, ^{
			((update_fn)objc_msgSend)(owner, sel_registerName("_queue_updateDeviceOrientation:changeSource:"), o, 1);
		});
	}
}

static void *
motion_thread(__unused void *argument)
{
	static const int types[] = { 4, 3 };     /* (a client that only sends; one rate controlled) */
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef accel_service = NULL, gyro_service = NULL, prox_service = NULL;
	int prox_sent = -2;
	unsigned prox_ticks = 0;
	struct motion_record r[32];
	unsigned said = 0;
	int device;

	pthread_setname_np("shennong motion");
	orientation_fix();
	/* (the device is made when the DSP's sensors come up, a minute or so into the boot: waited for, up to 5 minutes) */
	for (int seconds = 0; (device = open(MOTION_DEVICE, O_RDONLY)) < 0; seconds++) {
		if (seconds == 300) {
			say("shennong-motion: no %s after 5 minutes (boot-args snsrpc=1 gyro=1): no accelerometer or gyroscope\n",
			    MOTION_DEVICE);
			return NULL;
		}
		sleep(1);
	}
	say("shennong-motion: %s is there\n", MOTION_DEVICE);
	for (unsigned i = 0; accel_service == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
		if (client == NULL) {
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("shennong.motion", NULL));
		accel_service = make(client, &accel, "LSM6DSO accelerometer");
	}
	if (accel_service != NULL) {
		gyro_service = make(client, &gyro, "LSM6DSO gyroscope");
		prox_service = make(client, &prox, "TCS3720 proximity");
	}
	say("shennong-motion: accelerometer service %p, gyroscope %p\n", (void *)accel_service, (void *)gyro_service);
	if (accel_service == NULL) {
		return NULL;
	}
	for (;;) {
		ssize_t got = read(device, r, sizeof(r));
		uint64_t now = mach_absolute_time();

		for (ssize_t i = 0; got > 0 && i < got / (ssize_t)sizeof(r[0]); i++) {
			double x = r[i].value[0] / 1e6, y = r[i].value[1] / 1e6, z = r[i].value[2] / 1e6;

			if (r[i].sensor == 1) {
				/* m/s^2 of Android's, what holds the phone up; g of Apple's, what the phone feels */
				dispatch_to(accel_service, &accel, IOHIDEventCreateAccelerometerEvent(NULL, now, -x / 9.80665,
				    -y / 9.80665, -z / 9.80665, 0));
				orientation_feed(-x / 9.80665, -y / 9.80665, -z / 9.80665);
			} else if (r[i].sensor == 2 && gyro_service != NULL) {
				dispatch_to(gyro_service, &gyro, IOHIDEventCreateGyroEvent(NULL, now, x, y, z, 0));
			}
			if (said < 3 && r[i].sensor == 1) {
				said++;
				say("shennong-motion: accelerometer %.3f %.3f %.3f g to iOS\n", -x / 9.80665, -y / 9.80665, -z / 9.80665);
			}
		}
		/* the proximity sensor: its state from the kernel, every 10 ms or so */
		if (prox_service != NULL) {
			int near = -1;
			size_t size = sizeof(near);

			if (sysctlbyname("kern.redwood.prox", &near, &size, NULL, 0) == 0 && near >= 0 &&
			    (near != prox_sent || ++prox_ticks >= 100)) {
				dispatch_to(prox_service, &prox, IOHIDEventCreateProximtyEvent(NULL, now, near ? PROX_NEAR_MASK : 0, 0));
				if (near != prox_sent) {
					say("shennong-motion: proximity: %s\n", near ? "NEAR" : "far");
				}
				prox_sent = near;
				prox_ticks = 0;
			}
		}
		if (got <= 0) {
			usleep(10000);
		}
	}
	return NULL;
}

__attribute__((constructor))
static void
motion_start(void)
{
	pthread_t thread;

	if (strcmp(getprogname(), "backboardd") != 0) {
		return;
	}
	if (pthread_create(&thread, NULL, motion_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}
