/*
 * libshennong-als.dylib: the Xiaomi 14 Pro's ambient light sensor for iOS's auto-brightness, inside backboardd.
 *
 * The kernel reads the light (the TCS3720 under the screen, through the audio DSP's sensors' framework:
 * boot-args snsrpc=1 gyro=1, bsd/dev/shennong_sensors.c) and keeps the latest in the sysctl kern.redwood.als_mlux
 * (thousandths of a lux, -1 until the first). CoreBrightness, in backboardd, takes the light from the HID service
 * that is Apple's ambient light sensor (vendor page 0xff00, usage 4) and reads its events
 * (kIOHIDEventTypeAmbientLightSensor, 12) as -[CBALSEvent initWithHIDEvent:...] does in iOS 16.5 (20F66): the
 * colour space (field 0xc0006, which must be 1), three colour components (0xc0007..9, X Y Z: it takes the colour
 * temperature from X / (X + Y + Z) and Y / (X + Y + Z)) and the illuminance in lux (0xc000a, a float); the level
 * (0xc0000) too. Here such a service is made, a virtual one in the HID event server (backboardd itself), as
 * libredwood.dylib makes the touch screen, and the light is sent to it five times a second when it changes and
 * once a second when it does not; the colour is taken to be daylight's (D65), the sensor's own being unread.
 *
 * CoreBrightness asks, of every light sensor that turns up, whether True Tone's "harmony" should hold it back
 * (-shouldMitigateHarmony:), and that reads the harmony's state, which a display without True Tone's data does
 * not have: backboardd died there (a nil object at +0x1c8, 20F66). Its answer is NO here, for every class that
 * has the method.
 *
 * Said on the console: "shennong-als: ...". Nothing happens outside backboardd, or while the sysctl is not there.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <libkern/OSCacheControl.h>
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
extern const void *_dyld_get_shared_cache_range(size_t *length);

typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 (as libredwood.dylib has it, checked against 20F66) */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching,
	    uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client,
    CFDictionaryRef attributes, const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateAmbientLightSensorEvent(CFAllocatorRef allocator, uint64_t time, int32_t level,
    int32_t channel0, int32_t channel1, int32_t channel2, int32_t channel3, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);
extern void IOHIDEventSetFloatValue(IOHIDEventRef event, uint32_t field, double value);

#define HID_CLIENT_SIMPLE               4
#define HID_CLIENT_RATE_CONTROLLED      3
#define HID_PAGE_APPLE_VENDOR           0xff00
#define HID_USAGE_ALS                   4
#define EVENT_ALS                       12
#define FIELD_ALS_LEVEL                 0xc0000
#define FIELD_ALS_COLOR_SPACE           0xc0006
#define FIELD_ALS_COMPONENT0            0xc0007
#define FIELD_ALS_ILLUMINANCE           0xc000a
#define FIELD_IS_BUILT_IN               0x4

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

static CFMutableDictionaryRef properties;
static volatile bool opened;
static IOHIDEventRef last;
static pthread_mutex_t last_lock = PTHREAD_MUTEX_INITIALIZER;

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

static CFTypeRef
copy_property(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	static int asked;
	CFTypeRef value = CFDictionaryGetValue(properties, key);

	if (value == NULL && asked++ < 64) {
		char name[96] = "?";

		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("shennong-als: asked for property \"%s\", which it does not have\n", name);
	}
	return value ? CFRetain(value) : NULL;
}

static Boolean
set_property(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value)
{
	static int told;

	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		opened = true;
	}
	if (key != NULL && told++ < 24) {
		char name[96] = "?", text[128] = "?";
		CFStringRef description = value ? CFCopyDescription(value) : NULL;

		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("shennong-als: set \"%s\" = %s\n", name, text);
	}
	return true;
}

static void
notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service, __unused uint32_t type,
    __unused CFDictionaryRef property)
{
}

/* CoreBrightness asks the sensor for its latest event (IOHIDServiceClientCopyEvent) */
static IOHIDEventRef
copy_event(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service, uint32_t type,
    __unused IOHIDEventRef matching, __unused uint32_t options)
{
	IOHIDEventRef event = NULL;

	if (type != EVENT_ALS) {
		return NULL;
	}
	pthread_mutex_lock(&last_lock);
	if (last != NULL) {
		event = (IOHIDEventRef)CFRetain(last);
	}
	pthread_mutex_unlock(&last_lock);
	return event;
}

static const struct virtual_service_callbacks callbacks = {
	.version = 2, .notification = notification, .setProperty = set_property, .copyProperty = copy_property,
	.copyEvent = copy_event,
};

/*
 * CoreBrightness's auto-brightness takes a light sensor for the built-in display's only if the kernel has a service
 * with the HID service's registry id: -[CBABModuleiOS ...] calls IOServiceGetMatchingService(IORegistryEntryIDMatching
 * (id)) at 0x1af039740 (20F66) and drops the sensor, and then all its events, when there is none -- and a virtual
 * service has none. That call goes through a branch island of the shared cache (adrp x16; add x16; br x16, straight
 * to IOKit's function), which is pointed at ours here: a lookup of our sensor's id is answered with the kernel's
 * ShennongALS (patch 0462), every other one goes on to IOKit as it was.
 */
#define CB_LOOKUP_CALL          0x1af039740UL

extern CFTypeRef IOHIDServiceClientGetRegistryID(IOHIDServiceClientRef service);
static io_service_t (*real_lookup)(mach_port_t port, CFDictionaryRef matching);
static CFTypeRef our_id;
static unsigned redirected;

static io_service_t
lookup(mach_port_t port, CFDictionaryRef matching)
{
	CFTypeRef id = matching != NULL ? CFDictionaryGetValue(matching, CFSTR("IORegistryEntryID")) : NULL;

	if (id != NULL && our_id != NULL && CFEqual(id, our_id)) {
		io_service_t als = real_lookup(port, IOServiceMatching("ShennongALS"));

		CFRelease(matching);
		say("shennong-als: CoreBrightness looks our sensor up in the kernel: given ShennongALS (%u)\n", als);
		redirected++;
		return als;
	}
	return real_lookup(port, matching);
}

static bool
lookup_fix(void)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);
	uintptr_t slide = cache != NULL ? (uintptr_t)cache - 0x180000000UL : 0, call = CB_LOOKUP_CALL + slide, island, target;
	uint32_t bl = *(const uint32_t *)call, *code;
	int64_t offset;
	uint32_t patch[3];
	uintptr_t page;

	real_lookup = (io_service_t (*)(mach_port_t, CFDictionaryRef))dlsym(RTLD_DEFAULT, "IOServiceGetMatchingService");
	if ((bl & 0xfc000000u) != 0x94000000u || real_lookup == NULL) {
		say("shennong-als: no call to look the sensor up at %#lx (%#x): CoreBrightness left as it is\n", CB_LOOKUP_CALL, bl);
		return false;
	}
	offset = (int64_t)((int32_t)(bl << 6) >> 6) * 4;
	island = call + offset;
	code = (uint32_t *)island;
	/* the island as it should be: adrp x16 / add x16 / br x16, to IOKit's IOServiceGetMatchingService */
	if ((code[0] & 0x9f00001fu) != 0x90000010u || (code[1] & 0xffc003ffu) != 0x91000210u || code[2] != 0xd61f0200u) {
		say("shennong-als: the island at %#lx is not adrp/add/br x16: left as it is\n", island - slide);
		return false;
	}
	target = (island & ~0xfffUL) + (uintptr_t)((int64_t)(((code[0] >> 29) & 3) | ((code[0] >> 3) & 0x1ffffc)) << 43 >> 31) +
	    ((code[1] >> 10) & 0xfff);
	if (target != (uintptr_t)real_lookup) {
		say("shennong-als: the island at %#lx goes to %#lx, not IOServiceGetMatchingService (%#lx): left as it is\n",
		    island - slide, target - slide, (uintptr_t)real_lookup - slide);
		return false;
	}
	{
		int64_t pages = (int64_t)(((uintptr_t)lookup & ~0xfffUL) - (island & ~0xfffUL)) >> 12;

		if (pages >= (1 << 20) || pages < -(1 << 20)) {
			say("shennong-als: our function is too far from the island for adrp\n");
			return false;
		}
		patch[0] = 0x90000010u | ((uint32_t)(pages & 3) << 29) | ((uint32_t)((pages >> 2) & 0x7ffff) << 5);
		patch[1] = 0x91000210u | ((uint32_t)((uintptr_t)lookup & 0xfff) << 10);
		patch[2] = 0xd61f0200u;
	}
	page = island & ~0x3fffUL;
	if (vm_protect(mach_task_self(), page, 0x8000, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("shennong-als: the island's page cannot be made writable\n");
		return false;
	}
	memcpy(code, patch, sizeof(patch));
	vm_protect(mach_task_self(), page, 0x8000, false, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(code, sizeof(patch));
	say("shennong-als: CoreBrightness's lookup of the sensor in the kernel goes through us (island %#lx)\n", island - slide);
	return true;
}

static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client)
{
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFNumberRef page = number(HID_PAGE_APPLE_VENDOR), usage = number(HID_USAGE_ALS), zero = number(0);
	CFArrayRef pairs;

	properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPU"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("ams"));
	CFDictionarySetValue(properties, CFSTR("Product"), CFSTR("TCS3720 ambient light sensor"));
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	/*
	 * What CoreBrightness asks of a light sensor before its auto-brightness takes the sensor's events: without
	 * "ColorSupport" (and "crgb", both read as booleans, -[CBALSNodeiOS initWithALSServiceClient:]) no node is made
	 * for it, and CBABModuleiOS drops every event (20F66; the trace showed them reaching it and going no further).
	 */
	CFDictionarySetValue(properties, CFSTR("ColorSupport"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("crgb"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Calibrated"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("DeviceIsReady"), kCFBooleanTrue);
	for (int attempt = 1; attempt <= 20; attempt++) {
		opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &callbacks, NULL, NULL);
		if (service == NULL) {
			break;
		}
		/* (before the event server opens it: CoreBrightness looks it up as soon as it is there) */
		{
			CFTypeRef id = IOHIDServiceClientGetRegistryID(service);
			CFTypeRef old = our_id;

			our_id = id != NULL ? CFRetain(id) : NULL;
			if (old != NULL) {
				CFRelease(old);
			}
		}
		for (int tenths = 0; tenths < 20 && !opened; tenths++) {
			usleep(100000);
		}
		if (opened) {
			break;
		}
		say("shennong-als: the event server has not opened the sensor (attempt %d): making it again\n", attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

static BOOL
no_mitigation(__unused id self, __unused SEL selector, __unused id service)
{
	return NO;
}

/*
 * A trace of CoreBrightness's event handlers (-handleHIDEvent:from:, -handleALSEvent:), to learn whether the light's
 * events reach it: each class's own method is wrapped, the first calls of each said, then one in 200. Up to 16.
 */
typedef void *(*handler_t)(id self, SEL selector, void *a, void *b);
#define TRACED 16
static struct {
	Class cls;
	SEL selector;
	handler_t original;
	unsigned calls;
} traced[TRACED];
static unsigned traced_count;
extern uint32_t IOHIDEventGetType(void *event);

static void *
trace(id self, SEL selector, void *a, void *b)
{
	for (unsigned i = 0; i < traced_count; i++) {
		if (traced[i].selector == selector && object_getClass(self) != NULL &&
		    (object_getClass(self) == traced[i].cls || class_getInstanceMethod(object_getClass(self), selector) ==
		    class_getInstanceMethod(traced[i].cls, selector))) {
			unsigned n = traced[i].calls++;

			if (n < 12 || n % 200 == 0) {
				say("shennong-als: -[%s %s] call %u (event type %d)\n", class_getName(traced[i].cls), sel_getName(selector),
				    n + 1, a != NULL && sel_getName(selector)[6] == 'H' ? (int)IOHIDEventGetType(a) : -1);
			}
			return traced[i].original(self, selector, a, b);
		}
	}
	return NULL;
}

static uintptr_t
cache_slide(void)
{
	size_t length = 0;
	const void *start = _dyld_get_shared_cache_range(&length);

	return start != NULL ? (uintptr_t)start - 0x180000000UL : 0;
}

static void
trace_handlers(void)
{
	static const char *const names[] = { "handleHIDEvent:from:", "handleALSEvent:", "handleHIDEventInternal:from:" };
	unsigned count = 0;
	Class *classes = objc_copyClassList(&count);

	for (unsigned i = 0; classes != NULL && i < count && traced_count < TRACED; i++) {
		const char *image = class_getImageName(classes[i]);

		if (image == NULL || strstr(image, "CoreBrightness") == NULL) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(names) / sizeof(names[0]) && traced_count < TRACED; k++) {
			SEL selector = sel_registerName(names[k]);
			unsigned methods = 0;
			Method *list = class_copyMethodList(classes[i], &methods);

			for (unsigned m = 0; m < methods; m++) {
				if (method_getName(list[m]) == selector) {
					traced[traced_count].cls = classes[i];
					traced[traced_count].selector = selector;
					traced[traced_count].original = (handler_t)method_getImplementation(list[m]);
					traced_count++;
					method_setImplementation(list[m], (IMP)trace);
					say("shennong-als: tracing -[%s %s] at %#lx (unslid)\n", class_getName(classes[i]), names[k],
					    (unsigned long)((uintptr_t)traced[traced_count - 1].original - cache_slide()));
				}
			}
			free(list);
		}
	}
	free(classes);
}

/* -shouldMitigateHarmony: answered NO in every class that has it (see above) */
static unsigned
harmony_fix(void)
{
	SEL selector = sel_registerName("shouldMitigateHarmony:");
	unsigned count = 0, fixed = 0;
	Class *classes = objc_copyClassList(&count);

	for (unsigned i = 0; classes != NULL && i < count; i++) {
		Method method = class_getInstanceMethod(classes[i], selector);

		/* (only where the class itself has it, not where it inherits it) */
		if (method != NULL && class_getSuperclass(classes[i]) != NULL &&
		    class_getInstanceMethod(class_getSuperclass(classes[i]), selector) == method) {
			continue;
		}
		if (method != NULL) {
			method_setImplementation(method, (IMP)no_mitigation);
			say("shennong-als: -[%s shouldMitigateHarmony:] answers NO\n", class_getName(classes[i]));
			fixed++;
		}
	}
	free(classes);
	return fixed;
}

static void
send_light(IOHIDServiceClientRef service, int mlux)
{
	double lux = mlux / 1000.0;
	IOHIDEventRef event = IOHIDEventCreateAmbientLightSensorEvent(NULL, mach_absolute_time(), (int32_t)(lux + 0.5), 0, 0, 0,
	    0, 0);

	if (event == NULL) {
		return;
	}
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDEventSetIntegerValue(event, FIELD_ALS_COLOR_SPACE, 1);
	IOHIDEventSetFloatValue(event, FIELD_ALS_COMPONENT0, 0.9505 * lux);    /* daylight (D65): X, Y, Z */
	IOHIDEventSetFloatValue(event, FIELD_ALS_COMPONENT0 + 1, lux);
	IOHIDEventSetFloatValue(event, FIELD_ALS_COMPONENT0 + 2, 1.0891 * lux);
	IOHIDEventSetFloatValue(event, FIELD_ALS_ILLUMINANCE, lux);
	IOHIDVirtualServiceClientDispatchEvent(service, event);
	pthread_mutex_lock(&last_lock);
	if (last != NULL) {
		CFRelease(last);
	}
	last = event;
	pthread_mutex_unlock(&last_lock);
}

/*
 * iOS's brightness slider follows the kernel's auto-brightness (bright_auto=1, patch 0464): what it has made of the
 * level, as iOS's brightness, is in kern.redwood.bright_auto, and is handed to iOS as the slider does it
 * (BKSDisplayBrightnessSet, in a brightness transaction) when it has changed by 1 % or more, at most three times a
 * second... (every 150 ms at most). iOS then sets that brightness on the panel's service, which the kernel knows for its own echo.
 */
static void
slider_follow(void)
{
	static void *(*transaction_create)(CFAllocatorRef);
	static void (*brightness_set)(float, int);
	static bool looked;
	static int offered = -1;
	static uint64_t offered_at;
	int value = -1;
	size_t size = sizeof(value);
	uint64_t now = mach_absolute_time();

	if (!looked) {
		looked = true;
		transaction_create = (void *(*)(CFAllocatorRef))dlsym(RTLD_DEFAULT, "BKSDisplayBrightnessTransactionCreate");
		brightness_set = (void (*)(float, int))dlsym(RTLD_DEFAULT, "BKSDisplayBrightnessSet");
		say("shennong-als: iOS's slider follows auto-brightness: %s\n", transaction_create && brightness_set ? "yes" :
		    "NO (BackBoardServices' functions not found)");
	}
	if (brightness_set == NULL || sysctlbyname("kern.redwood.bright_auto", &value, &size, NULL, 0) != 0 || value < 0) {
		return;
	}
	if (offered >= 0 && (value > offered ? value - offered : offered - value) < 100) {
		return;
	}
	/*
	 * (never a big jump: the slider is moved at most 0.5 % of its range at a time, 60 times a second, toward
	 * the level -- gliding the whole range takes 3 s; a jump of the slider's is drawn as a teleport, the user said)
	 */
	if (offered >= 0 && value > offered + 328) {
		value = offered + 328;
	} else if (offered >= 0 && value + 328 < offered) {
		value = offered - 328;
	}
	static mach_timebase_info_data_t timebase;

	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
	}
	if (offered_at != 0 && (now - offered_at) * timebase.numer / timebase.denom < 14000000ULL) {      /* 14 ms */
		return;
	}
	{
		void *transaction = transaction_create != NULL ? transaction_create(kCFAllocatorDefault) : NULL;

		brightness_set((float)value / 65536.0f, 1);
		if (transaction != NULL) {
			CFRelease(transaction);
		}
	}
	offered = value;
	offered_at = now;
}

/*
 * CoreBrightness tells backboardd now and then that the brightness is 0 ("brightness change:0
 * reason:BrightnessSystemDidChange"), its own auto-brightness having no light sensor it can use, and the slider drops
 * to nothing for a moment. -[BKDisplayBrightnessController setBrightnessLevel:reason:options:] passes on no such 0
 * from CoreBrightness; the screen going off is not this (it is the backlight factor).
 */
typedef void (*set_level_fn)(id self, SEL selector, float level, id reason, unsigned long long options);
static set_level_fn set_level_original;
static unsigned zeros_dropped;

static void
set_level_hook(id self, SEL selector, float level, id reason, unsigned long long options)
{
	if (level < 0.002f && reason != NULL && CFGetTypeID((CFTypeRef)reason) == CFStringGetTypeID() &&
	    CFEqual((CFStringRef)reason, CFSTR("BrightnessSystemDidChange"))) {
		if (zeros_dropped++ < 5) {
			say("shennong-als: CoreBrightness's brightness 0 not passed on\n");
		}
		return;
	}
	set_level_original(self, selector, level, reason, options);
}

static void
zero_fix(void)
{
	Class controller = objc_getClass("BKDisplayBrightnessController");
	Method method = controller != NULL ? class_getInstanceMethod(controller,
	    sel_registerName("setBrightnessLevel:reason:options:")) : NULL;
	const char *types = method != NULL ? method_getTypeEncoding(method) : NULL;

	if (types == NULL || strncmp(types, "v", 1) != 0 || strchr(types, 'f') == NULL) {
		say("shennong-als: -[BKDisplayBrightnessController setBrightnessLevel:reason:options:] is %s: left alone\n",
		    types ? types : "not there");
		return;
	}
	set_level_original = (set_level_fn)method_setImplementation(method, (IMP)set_level_hook);
	say("shennong-als: CoreBrightness's zeros are dropped (%s)\n", types);
}

static void *
slider_thread(__unused void *argument)
{
	pthread_setname_np("shennong slider");
	zero_fix();
	for (;;) {
		slider_follow();
		usleep(16000);          /* (60 a second: the slider moves with the screen's glide) */
	}
	return NULL;
}

static void *
als_thread(__unused void *argument)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef service = NULL;
	int mlux = -1, sent = -2, events = 0;
	size_t size = sizeof(mlux);
	unsigned quiet = 0;

	pthread_setname_np("shennong als");
	if (sysctlbyname("kern.redwood.als_mlux", &mlux, &size, NULL, 0) != 0) {
		say("shennong-als: no kern.redwood.als_mlux in this kernel: no light sensor\n");
		return NULL;
	}
	/* CoreBrightness waits only 5 s for a light sensor after it starts (CBU_ShouldWaitForALS, "timeout waiting for
	 * ALS arrival"), and never takes one that comes later for its auto-brightness: 3 s here and the making were too
	 * long. A second for backboardd's event server to be there; one that is made too early is made again. */
	sleep(1);
	/* (trace_handlers() and lookup_fix() are kept for the next look at CoreBrightness's auto-brightness, not used:
	 * with the lookup answered it went into its AABC mode, made for a display coprocessor this phone has not got,
	 * "CBAPEndpoint init failed", and left the panel alone; the kernel's bright_auto=1 does the job instead) */
	if (getenv("SHENNONG_ALS_TRACE") != NULL) {
		trace_handlers();
		lookup_fix();
	}
	for (int tries = 0; harmony_fix() == 0; tries++) {
		if (tries == 20) {
			say("shennong-als: no class has -shouldMitigateHarmony: after 10 s: the sensor is not made\n");
			return NULL;
		}
		usleep(500000);
	}
	for (unsigned i = 0; service == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
		if (client == NULL) {
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("shennong.als", NULL));
		service = make_service(client);
	}
	say("shennong-als: the light sensor's HID service: %p\n", (void *)service);

	if (service == NULL) {
		return NULL;
	}
	for (;;) {
		size = sizeof(mlux);
		if (sysctlbyname("kern.redwood.als_mlux", &mlux, &size, NULL, 0) != 0) {
			mlux = -1;
		}
		if (mlux >= 0 && (mlux != sent || ++quiet >= 5)) {
			send_light(service, mlux);
			if (events++ < 10 || (mlux != sent && events % 50 == 0)) {
				say("shennong-als: %d.%03d lux to iOS\n", mlux / 1000, mlux % 1000);
			}
			sent = mlux;
			quiet = 0;
		}
		slider_follow();
		usleep(200000);
	}
	return NULL;
}

__attribute__((constructor))
static void
start(void)
{
	pthread_t thread;

	if (strcmp(getprogname(), "backboardd") != 0) {
		return;
	}
	/*
	 * (the light sensor's HID service, which CoreBrightness takes but does nothing with on this phone, only with
	 * SHENNONG_ALS_HID set: its own auto-brightness then moved iOS's brightness to 0 at times, under the kernel's
	 * bright_auto=1, which makes the level itself; the slider's following needs nothing of it)
	 */
	if (getenv("SHENNONG_ALS_HID") == NULL) {
		if (pthread_create(&thread, NULL, slider_thread, NULL) == 0) {
			pthread_detach(thread);
		}
		return;
	}
	if (pthread_create(&thread, NULL, als_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}
