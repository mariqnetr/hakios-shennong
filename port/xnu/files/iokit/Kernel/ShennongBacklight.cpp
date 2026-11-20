/*
 * The Xiaomi 14 Pro's (shennong) screen brightness, as iOS sets it: an AppleARMBacklight service.
 * Nothing here happens unless boot-arg bright=N asks for it.
 *
 * iOS 16's CoreBrightness (in backboardd) looks for the display's backlight among the services with
 * the property backlight-control = true (IOPropertyMatch) and takes one of the class AppleARMBacklight
 * as the built-in display's. From its IODisplayParameters it reads brightness = { min, max, value },
 * and from then on every change of brightness -- the slider in Control Center and in Settings, the
 * fades on locking and waking, auto-brightness -- is IORegistryEntrySetCFProperties(service,
 * { brightness = N, ... }) with N between min and max: ceil(factor * (max - min)) + min (Display.m,
 * the brightness thread). The other keys of that dictionary are said once in the log and left alone.
 *
 * The panel (dt: qcom,mdss_dsi_n2_42_02_0a_dsc_cmd, a command mode OLED) takes its brightness as the
 * DCS command 0x51 with two bytes, the high one first (qcom,mdss-dsi-bl-inverted-dbv), from 25
 * (qcom,mdss-dsi-bl-min-level) to 2866 (mi,mdss-fac-brightness-max-level: the most outside the
 * panel's high brightness mode; up to 4095 is that mode, for sunlight). The commands go as
 * MiniIOMobileFramebuffer's fb_panel_hz ones do: the DSI controller as the firmware left it, a packet
 * in memory it reads (a page of the firmware's splash region; this one the page before fb_panel_hz's),
 * triggered by software, between two frames. A thread of its own sends them, the latest level only,
 * so that a fade of iOS's (a new level every frame) costs the caller nothing and the panel at most
 * one command a frame. Brightness 0 (what iOS asks for with the screen off) is the panel dark (0).
 *
 *   bright=1  the service, and the panel's brightness follows iOS's
 *   bright=2  ... and a line for every level sent
 * bright_max=N: the panel's level at iOS's full brightness (default 2866; at most 4095).
 * bright_way=N, bright_test=S: how the level is sent, and a test of every way (see sendLevelWay()).
 *
 * shennong_dsi_lock()/unlock(): around a command to the panel, for whoever else sends any (the DSI
 * controller's command registers and its one DMA are shared).
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

extern "C" const char * redwood_phone(void);
extern "C" void shennong_brightness_init(void);
extern "C" void shennong_dsi_lock(void);
extern "C" void shennong_dsi_unlock(void);
extern "C" int shennong_als_mlux(void);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" unsigned int ml_phys_read_word_64(addr64_t paddr);

#define BL_RANGE                65536           /* iOS's brightness: 0 to this */
#define BL_PANEL_MIN            25              /* the panel's dimmest level that is not dark */
#define BL_PANEL_MAX            2866            /* its brightest outside the high brightness mode */

#define DSI0                    0x0ae94000      /* the first DSI controller (Linux's dsi.xml, 6G) */
#define DSI_CTRL                0x004
#define DSI_CMD_DMA_CTRL        0x03c
#define DSI_DMA_BASE            0x048
#define DSI_DMA_LEN             0x04c
#define DSI_ACK_ERR_STATUS      0x068
#define DSI_TRIG_CTRL           0x084
#define DSI_TRIG_DMA            0x090
#define DSI_INTR_CTRL           0x110           /* bit 0: a command sent (written 1 to clear) */
#define BL_COMMAND_PAGE         (0xfc400000u + 0x2b00000u - 0x2000u)    /* the splash region's last page but one */

#define GCC_DISP_AHB            0x00127004      /* the display's clock controller's bus clock: bit 0 */
#define DISPCC                  0x0af00000
#define DISPCC_MDSS_AHB_CBCR    0x80a4          /* bit 31: the display block's register clock is off */
#define INTF1                   0x0ae36000      /* the first DSI's interface */
#define INTF_FRAME_COUNT        0x0ac

static uint32_t gMode;
static uint32_t gPanelMax = BL_PANEL_MAX;
static IOLock * gDSILock;
static IOLock * gLock;
static uint32_t gWanted = ~0u;                  /* the panel's level iOS asks for (none yet) */
static uint32_t gSent = ~0u;                    /* ... and the last one sent */
static uint32_t gSends, gFailures;

/* ---- the panel's commands ------------------------------------------------------------------------------------ */

extern "C" void
shennong_dsi_lock(void)
{
	if (gDSILock != NULL) {
		IOLockLock(gDSILock);
	}
}

extern "C" void
shennong_dsi_unlock(void)
{
	if (gDSILock != NULL) {
		IOLockUnlock(gDSILock);
	}
}

/* the first DSI's frame counter, where it can be read without harm (its clocks running); NULL where not */
static volatile uint32_t *
frameCounter(void)
{
	static volatile uint32_t * intf;
	static bool looked;

	if (!looked) {
		volatile uint32_t * gcc = (volatile uint32_t *) ml_io_map(GCC_DISP_AHB & ~0xfffu, 0x1000);
		volatile uint32_t * dispcc = (volatile uint32_t *) ml_io_map(DISPCC, 0x10000);

		looked = true;
		if (gcc != NULL && dispcc != NULL && (gcc[(GCC_DISP_AHB & 0xfffu) / 4] & 1) &&
		    !(dispcc[DISPCC_MDSS_AHB_CBCR / 4] & 1u << 31)) {
			intf = (volatile uint32_t *) ml_io_map(INTF1, 0x1000);
		}
		IOLog("ShennongBacklight: %s\n", intf != NULL ? "commands go between frames" :
		    "the display's interface cannot be read (its clocks are off): commands go when they come");
	}
	return intf;
}

/*
 * `commands' to the panel (each: the packet's data type, its length, its bytes; a 0 type ends them), between two
 * frames; in high speed (`hs') instead of the low power mode the firmware leaves for commands. False if one could
 * not be sent.
 */
static bool
sendCommands(const uint8_t * commands, bool hs)
{
	static volatile uint32_t * dsi;
	static volatile uint8_t * packet;
	volatile uint32_t * intf = frameCounter();
	uint32_t status = 0, frame, dmaCtrl = 0;
	bool sent = false;

	if (dsi == NULL) {
		dsi = (volatile uint32_t *) ml_io_map(DSI0, 0x1000);
	}
	if (packet == NULL) {
		packet = (volatile uint8_t *) ml_io_map(BL_COMMAND_PAGE, 0x1000);
	}
	if (dsi == NULL || packet == NULL) {
		return false;
	}
	shennong_dsi_lock();
	/* (as the firmware leaves it: enabled, in command mode, commands from memory, triggered by software) */
	if ((dsi[DSI_CTRL / 4] & 0x5) != 0x5 || !(dsi[DSI_CMD_DMA_CTRL / 4] & 1u << 28) || (dsi[DSI_TRIG_CTRL / 4] & 7) != 4) {
		static bool said;

		if (!said) {
			said = true;
			IOLog("ShennongBacklight: the DSI controller is not as the firmware leaves it (ctrl %#x, cmd_dma_ctrl %#x, "
			    "trig_ctrl %#x): no brightness is sent\n", dsi[DSI_CTRL / 4], dsi[DSI_CMD_DMA_CTRL / 4], dsi[DSI_TRIG_CTRL / 4]);
		}
		goto out;
	}
	/*
	 * the moment a frame has gone (not more than 34 ms away even at 30 a second) -- unless no frame has gone since the
	 * last command: the display idles (fb_dyn's deep idle sends the panel nothing), and waiting would only hold the
	 * DSI lock for nothing, 40 ms a step of a fade
	 */
	if (intf != NULL) {
		static uint32_t lastFrame = ~0u;

		frame = intf[INTF_FRAME_COUNT / 4];
		if (frame != lastFrame) {
			for (uint32_t waited = 0; intf[INTF_FRAME_COUNT / 4] == frame && waited < 40000; waited += 5) {
				IODelay(5);
			}
		}
		lastFrame = intf[INTF_FRAME_COUNT / 4];
	}
	dmaCtrl = dsi[DSI_CMD_DMA_CTRL / 4];
	if (hs) {
		dsi[DSI_CMD_DMA_CTRL / 4] = dmaCtrl & ~(1u << 26);      /* (bit 26: low power) */
	}
	sent = true;
	for (const uint8_t * c = commands; *c != 0 && sent; c += 2 + c[1]) {
		uint8_t type = c[0], length = c[1];
		uint32_t size;

		/* the controller's own form of a packet: its header's three bytes in this order, then a last-and-long byte */
		if (type == 0x39) {
			packet[0] = length;
			packet[1] = 0;
			packet[2] = type;
			packet[3] = 0x80 | 0x40;
			for (uint32_t i = 0; i < length; i++) {
				packet[4 + i] = c[2 + i];
			}
			size = 4 + length;
			while (size & 3) {
				packet[size++] = 0xff;
			}
		} else {
			packet[0] = c[2];
			packet[1] = length > 1 ? c[3] : 0;
			packet[2] = type;
			packet[3] = 0x80;
			size = 4;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		dsi[DSI_INTR_CTRL / 4] = dsi[DSI_INTR_CTRL / 4] | 1;
		dsi[DSI_DMA_BASE / 4] = BL_COMMAND_PAGE;
		dsi[DSI_DMA_LEN / 4] = size;
		__asm__ volatile ("dsb sy" ::: "memory");
		dsi[DSI_TRIG_DMA / 4] = 1;
		status = 0;
		for (uint32_t waited = 0; waited < 20000; waited += 5) {
			status = dsi[DSI_INTR_CTRL / 4];
			if (status & 1) {
				break;
			}
			IODelay(5);
		}
		if (status & 1) {
			dsi[DSI_INTR_CTRL / 4] = status | 1;
		} else {
			sent = false;
			if (gFailures++ < 5) {
				IOLog("ShennongBacklight: command %#x was not sent in 20 ms (intr_ctrl %#x, ack_err %#x)\n", c[2], status,
				    dsi[DSI_ACK_ERR_STATUS / 4]);
			}
		}
	}
	if (hs) {
		dsi[DSI_CMD_DMA_CTRL / 4] = dmaCtrl;
	}
	if (dsi[DSI_ACK_ERR_STATUS / 4] != 0 && gFailures++ < 5) {
		IOLog("ShennongBacklight: the panel reports an error: ack_err %#x\n", dsi[DSI_ACK_ERR_STATUS / 4]);
	}
out:
	shennong_dsi_unlock();
	return sent;
}

/*
 * How the level goes (boot-arg bright_way=N, default 0): 0 DCS 0x51, high byte first, low power mode; 1 the same
 * in high speed; 2 0x53 0x20 (brightness control on) before it; 3 the low byte first; 4 0x51 with six bytes, as
 * the panel's on-commands have it (the level, then zeros); 5 the panel's command page 0 chosen before it.
 */
static uint32_t gWay;
#define BL_WAYS                 6

static bool
sendLevelWay(uint32_t level, uint32_t way)
{
	uint8_t hi = (uint8_t) (level >> 8), lo = (uint8_t) level;
	uint8_t c[32];
	unsigned n = 0;

	if (way == 2) {
		c[n++] = 0x15; c[n++] = 2; c[n++] = 0x53; c[n++] = 0x20;
	}
	if (way == 5) {
		static const uint8_t page0[] = { 0x39, 6, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00 };

		memcpy(c + n, page0, sizeof(page0));
		n += sizeof(page0);
	}
	if (way == 4) {
		c[n++] = 0x39; c[n++] = 7; c[n++] = 0x51; c[n++] = hi; c[n++] = lo;
		for (int i = 0; i < 4; i++) {
			c[n++] = 0;
		}
	} else {
		c[n++] = 0x39; c[n++] = 3; c[n++] = 0x51;
		c[n++] = way == 3 ? lo : hi;
		c[n++] = way == 3 ? hi : lo;
	}
	c[n] = 0;
	return sendCommands(c, way == 1);
}

static bool
sendLevel(uint32_t level)
{
	return sendLevelWay(level, gWay);
}

/*
 * Boot-arg bright_test=S: S seconds after the start, each way of sending the level in turn, the panel brightest
 * for 2 s and dimmest for 2 s, twice; a line for each ("bright_test: way N ..."), for whoever watches the screen.
 */
static uint32_t gTest;
static volatile bool gTesting;              /* (iOS's levels meanwhile kept, not sent) */

static void
testThread(__unused void * parameter, __unused wait_result_t result)
{
	IOSleep(gTest * 1000);
	gTesting = true;
	IOLog("ShennongBacklight: bright_test: %u ways, 8 s each: watch the screen\n", BL_WAYS);
	for (uint32_t way = 0; way < BL_WAYS; way++) {
		for (int i = 0; i < 2; i++) {
			bool a = sendLevelWay(gPanelMax, way);
			IOLog("ShennongBacklight: bright_test: way %u: BRIGHT (%s)\n", way, a ? "sent" : "NOT SENT");
			IOSleep(2000);
			a = sendLevelWay(BL_PANEL_MIN, way);
			IOLog("ShennongBacklight: bright_test: way %u: DIM (%s)\n", way, a ? "sent" : "NOT SENT");
			IOSleep(2000);
		}
	}
	IOLog("ShennongBacklight: bright_test: done\n");
	sendLevelWay(0x133, 0);
	gTesting = false;
}

static void
senderThread(__unused void * parameter, __unused wait_result_t result)
{
	IOLockLock(gLock);
	for (;;) {
		uint32_t level;
		bool sent;

		while (gWanted == gSent) {
			IOLockSleep(gLock, &gWanted, THREAD_UNINT);
		}
		level = gWanted;
		IOLockUnlock(gLock);
		sent = sendLevel(level);
		IOLockLock(gLock);
		if (sent) {
			gSends++;
			if (gMode >= 2) {
				IOLog("ShennongBacklight: panel level %u\n", level);
			}
		}
		gSent = level;          /* (one that failed is not tried again until another comes) */
	}
}

/*
 * Auto-brightness of our own (boot-arg bright_auto=1, the default; 0: the level is iOS's alone). CoreBrightness's
 * own needs a light sensor of the kernel's that it can tie to a display coprocessor this phone has not got, so the
 * light (kern.redwood.als_mlux, bsd/dev/shennong_sensors.c, from the TCS3720 under the screen) is made into the level
 * here: a fraction of the panel's range for the light (by the table below, between its points on a log scale of
 * lux), times iOS's slider as the user's taste (its middle the table as it is, its ends a quarter and twice that),
 * the light smoothed, and the level moved a tenth of the way every 100 ms (about 2 s for a change), only when the
 * new level is more than 3 % away. Without a light reading the level is iOS's, as without bright_auto.
 */
static uint32_t gAuto = 1;
static volatile uint32_t gIosValue = BL_RANGE / 2;
static uint32_t gAutoLevel;
static const struct {
	uint32_t lux, permille;                 /* a light, and the share of the range for it */
} gCurve[] = {
	{ 0, 50 }, { 1, 90 }, { 3, 150 }, { 10, 250 }, { 30, 360 }, { 100, 480 }, { 300, 620 }, { 1000, 780 }, { 3000, 900 },
	{ 10000, 1000 },
};

/* the share (per mille) for `mlux' thousandths of a lux: the curve, between its points by the log of the light */
static uint32_t
curvePermille(uint32_t mlux)
{
	unsigned n = sizeof(gCurve) / sizeof(gCurve[0]);
	uint32_t lux = mlux / 1000;

	if (lux <= gCurve[0].lux) {
		return gCurve[0].permille;
	}
	for (unsigned i = 1; i < n; i++) {
		if (lux < gCurve[i].lux) {
			/* (log-ish: by the light's place between the two points, its square root the measure) */
			uint64_t lo = gCurve[i - 1].lux, hi = gCurve[i].lux, at = lux;
			uint64_t a = 0, b = 0;

			while ((a + 1) * (a + 1) <= (at - lo) * 1000000 / (hi - lo)) {
				a++;
			}
			b = 1000;
			return gCurve[i - 1].permille + (uint32_t) ((gCurve[i].permille - gCurve[i - 1].permille) * a / b);
		}
	}
	return gCurve[n - 1].permille;
}

/*
 * The level glides: every 16 ms it moves at most 1.5 % of itself (and at least one step) toward the target, so a
 * change of light is a smooth fade of a second or two whatever its size, even at the dim end where one step of the
 * panel's is a lot. The target is made again every 100 ms from the smoothed light; a new one is taken only when it is
 * more than 4 % from the one being glided to, so the level does not wander with the light's noise.
 *
 * iOS's slider: what auto-brightness has made of the level is offered back to iOS as its brightness (the sysctl
 * kern.redwood.bright_auto, which libshennong-als.dylib in backboardd hands to BKSDisplayBrightnessSet), so the
 * slider shows it; iOS then sets that value here, which is its echo and changes nothing. A value far from it is the
 * user's hand on the slider: the taste (gTaste, per mille) becomes what was asked for over what the light alone
 * gives, and auto-brightness goes on from there, brighter or dimmer.
 */
extern "C" void shennong_bright_auto_value(int value);
static volatile uint32_t gTaste = 1000;
static bool gFading;
static volatile uint32_t gOfferedBefore, gOffered;        /* iOS values offered: the last two */
static volatile uint64_t gOfferedAt;
static volatile uint32_t gAutoTarget;           /* the level being glided to */
static volatile uint32_t gLightPermille;        /* the share of the range for the light alone */

static uint32_t
iosValue(uint32_t level)
{
	return level <= BL_PANEL_MIN ? 0 : (uint32_t) ((uint64_t) (level - BL_PANEL_MIN) * BL_RANGE / (gPanelMax - BL_PANEL_MIN));
}

static void
autoThread(__unused void * parameter, __unused wait_result_t result)
{
	uint64_t smooth = 0;
	bool started = false;
	unsigned ticks = 0;

	for (;;) {
		IOSleep(16);
		ticks++;
		if (gIosValue == 0 || gTesting) {
			continue;
		}
		if (ticks % 6 == 0) {           /* (about every 100 ms: a new target) */
			int mlux = shennong_als_mlux();
			uint32_t permille, target;

			if (mlux < 0) {
				shennong_bright_auto_value(-1);
				continue;
			}
			smooth = started ? (smooth * 4 + (uint64_t) mlux) / 5 : (uint64_t) mlux;
			started = true;
			gLightPermille = curvePermille((uint32_t) smooth);
			permille = gLightPermille * gTaste / 1000;
			permille = permille > 1000 ? 1000 : permille < 10 ? 10 : permille;
			target = BL_PANEL_MIN + (gPanelMax - BL_PANEL_MIN) * permille / 1000;
			if (gAutoTarget == 0 || (target > gAutoTarget ? target - gAutoTarget : gAutoTarget - target) * 100 > gAutoTarget * 6) {
				gAutoTarget = target;
			}
		}
		IOLockLock(gLock);
		if (gAutoTarget != 0 && gAutoLevel != gAutoTarget) {
			uint32_t level = gAutoLevel == 0 || gAutoLevel == ~0u ? gAutoTarget : gAutoLevel;
			uint32_t step = level * 15 / 1000;

			step = step == 0 ? 1 : step;
			if (level < gAutoTarget) {
				level = level + step > gAutoTarget ? gAutoTarget : level + step;
			} else {
				level = level < gAutoTarget + step ? gAutoTarget : level - step;
			}
			gAutoLevel = level;
			gWanted = level;
			IOLockWakeup(gLock, &gWanted, true);
			/*
			 * (the slider is offered the level as it glides, libshennong-als handing it on 20 times a second: offered
			 * only at the end of a glide it jumped, offered five times a second it went in jerks)
			 */
			{
				gOfferedBefore = gOffered;
				gOffered = iosValue(level);
				gOfferedAt = mach_absolute_time();
				shennong_bright_auto_value((int) gOffered);
			}
		}
		IOLockUnlock(gLock);
	}
}

/* ---- the service iOS finds ----------------------------------------------------------------------------------- */

class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	void publish(uint32_t value);
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

static uint32_t
panelLevel(uint32_t value)
{
	if (value == 0) {
		return 0;
	}
	if (value > BL_RANGE) {
		value = BL_RANGE;
	}
	return BL_PANEL_MIN + (uint32_t) (((uint64_t) value * (gPanelMax - BL_PANEL_MIN) + BL_RANGE - 1) / BL_RANGE);
}

/* IODisplayParameters = { brightness = { min = 0, max = BL_RANGE, value } }, as CoreBrightness reads it */
void
AppleARMBacklight::publish(uint32_t value)
{
	OSDictionary * parameters = OSDictionary::withCapacity(1);
	OSDictionary * brightness = OSDictionary::withCapacity(3);
	OSNumber * min = OSNumber::withNumber(0ULL, 32);
	OSNumber * max = OSNumber::withNumber((unsigned long long) BL_RANGE, 32);
	OSNumber * now = OSNumber::withNumber((unsigned long long) value, 32);

	if (parameters != NULL && brightness != NULL && min != NULL && max != NULL && now != NULL) {
		brightness->setObject("min", min);
		brightness->setObject("max", max);
		brightness->setObject("value", now);
		parameters->setObject("brightness", brightness);
		setProperty("IODisplayParameters", parameters);
	}
	OSSafeReleaseNULL(now);
	OSSafeReleaseNULL(max);
	OSSafeReleaseNULL(min);
	OSSafeReleaseNULL(brightness);
	OSSafeReleaseNULL(parameters);
}

IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	static uint32_t calls;
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSNumber * brightness;
	char who[32];

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	calls++;
	if (calls <= 3) {
		OSCollectionIterator * keys = OSCollectionIterator::withCollection(dict);
		OSObject * next;

		proc_selfname(who, sizeof(who));
		while (keys != NULL && (next = keys->getNextObject()) != NULL) {
			const OSSymbol * key = OSDynamicCast(OSSymbol, next);
			OSNumber * n = key != NULL ? OSDynamicCast(OSNumber, dict->getObject(key)) : NULL;

			IOLog("ShennongBacklight: %s sets %s = %lld%s\n", who, key != NULL ? key->getCStringNoCopy() : "?",
			    n != NULL ? (long long) n->unsigned64BitValue() : 0LL, n != NULL ? "" : " (not a number)");
		}
		OSSafeReleaseNULL(keys);
	}
	brightness = OSDynamicCast(OSNumber, dict->getObject("brightness"));
	if (brightness == NULL) {
		return kIOReturnSuccess;        /* (the rest is iOS's own bookkeeping) */
	}
	publish(brightness->unsigned32BitValue());
	IOLockLock(gLock);
	{
		static uint64_t lastZero;
		uint32_t value = brightness->unsigned32BitValue();

		/*
		 * (iOS's fade back from the screen off is no hand on the slider: 2 s after a 0, nothing is taken as one; nor in
		 * the 4 s after a level was offered to iOS: what iOS then sets here is its own curve of the slider's value,
		 * not that value -- taken for a hand, it walked the level down to nothing, the run of 18 November)
		 */
		if (value == 0 || gIosValue == 0) {
			lastZero = mach_absolute_time();
		}
		uint64_t two;

		nanoseconds_to_absolutetime(2ull * NSEC_PER_SEC, &two);
		gFading = (lastZero != 0 && mach_absolute_time() - lastZero < two) ||
		    (gOfferedAt != 0 && mach_absolute_time() - gOfferedAt < two * 2);
	}
	gIosValue = brightness->unsigned32BitValue();
	if (!gAuto || gIosValue == 0 || shennong_als_mlux() < 0) {      /* (the screen off at once; no light: iOS's level) */
		gWanted = panelLevel(gIosValue);
		gAutoLevel = gIosValue == 0 ? 0 : gWanted;
	} else if (gAutoTarget != 0 && gLightPermille != 0 && !gFading) {
		uint32_t value = gIosValue, low = gOfferedBefore < gOffered ? gOfferedBefore : gOffered;
		uint32_t high = gOfferedBefore > gOffered ? gOfferedBefore : gOffered, band = BL_RANGE * 3 / 100;

		/* not the echo of what was offered, nor a step of iOS's fade to it from the one before: the user's slider */
		if (value + band < low || value > high + band) {
			uint32_t asked = (uint32_t) ((uint64_t) value * 1000 / BL_RANGE);       /* per mille of the range */
			uint32_t taste = asked * 1000 / gLightPermille;

			gTaste = taste < 200 ? 200 : taste > 5000 ? 5000 : taste;
			gAutoTarget = panelLevel(value);
			gOfferedBefore = gOffered = value;
			shennong_bright_auto_value((int) value);
			if (gMode >= 2) {
				IOLog("ShennongBacklight: auto: the slider at %u: taste %u per mille\n", value, gTaste);
			}
		}
	}
	if (gWanted != gSent && !gTesting) {
		IOLockWakeup(gLock, &gWanted, true);
	}
	IOLockUnlock(gLock);
	return kIOReturnSuccess;
}

/*
 * The light sensor as an IOService of the kernel's (ShennongALS), with bright=N: CoreBrightness takes a light sensor
 * for the built-in display's only if the kernel has a service with the HID service's registry id
 * (IOServiceGetMatchingService(IORegistryEntryIDMatching(...)), CBABModuleiOS, 20F66), and reads its overrides (none
 * here) from it. The HID service itself is a virtual one in backboardd (libshennong-als.dylib), which has no such
 * id: that library answers CoreBrightness's question with this service instead.
 */
class ShennongALS : public IOService
{
	OSDeclareDefaultStructors(ShennongALS);
};

OSDefineMetaClassAndStructors(ShennongALS, IOService);

static void
publishALS(IOService * platform)
{
	ShennongALS * als = OSTypeAlloc(ShennongALS);

	if (als == NULL || !als->init()) {
		OSSafeReleaseNULL(als);
		return;
	}
	als->setProperty("Built-In", kOSBooleanTrue);
	if (!als->attach(platform)) {
		als->release();
		return;
	}
	als->setName("ShennongALS");
	if (!als->start(platform)) {
		als->detach(platform);
		als->release();
		return;
	}
	als->registerService();
}

/* at boot (bsd/dev/redwood_input.c), with bright=N on shennong */
extern "C" void
shennong_brightness_init(void)
{
	AppleARMBacklight * service;
	IOService * platform = IOService::getPlatform();
	thread_t thread;
	uint32_t max;

	if (!PE_parse_boot_argn("bright", &gMode, sizeof(gMode)) || gMode == 0) {
		return;
	}
	if (strcmp(redwood_phone(), "shennong") != 0) {
		IOLog("ShennongBacklight: bright=%u: only for shennong\n", gMode);
		return;
	}
	if (PE_parse_boot_argn("bright_max", &max, sizeof(max)) && max > BL_PANEL_MIN && max <= 4095) {
		gPanelMax = max;
	}
	PE_parse_boot_argn("bright_way", &gWay, sizeof(gWay));
	gWay %= BL_WAYS;
	PE_parse_boot_argn("bright_test", &gTest, sizeof(gTest));
	PE_parse_boot_argn("bright_auto", &gAuto, sizeof(gAuto));
	gDSILock = IOLockAlloc();
	gLock = IOLockAlloc();
	service = OSTypeAlloc(AppleARMBacklight);
	if (gDSILock == NULL || gLock == NULL || platform == NULL || service == NULL || !service->init()) {
		IOLog("ShennongBacklight: cannot be set up\n");
		OSSafeReleaseNULL(service);
		return;
	}
	if (kernel_thread_start(senderThread, NULL, &thread) != KERN_SUCCESS) {
		IOLog("ShennongBacklight: no thread for the panel's commands\n");
		service->release();
		return;
	}
	thread_deallocate(thread);
	if (gAuto != 0 && kernel_thread_start(autoThread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	if (gTest != 0 && kernel_thread_start(testThread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	/* (iOS's value until it sets one: the level the panel is started with, qcom,mdss-brightness-init-level 0x133) */
	service->publish((uint32_t) (((uint64_t) (0x133 - BL_PANEL_MIN) * BL_RANGE) / (gPanelMax - BL_PANEL_MIN)));
	service->setProperty("backlight-control", kOSBooleanTrue);
	if (!service->attach(platform)) {
		service->release();
		return;
	}
	service->setName("AppleARMBacklight");
	if (!service->start(platform)) {
		service->detach(platform);
		service->release();
		return;
	}
	service->registerService();
	publishALS(platform);
	IOLog("ShennongBacklight: bright=%u: AppleARMBacklight is up; iOS's brightness 0..%u is the panel's %u..%u (0: dark)\n",
	    gMode, BL_RANGE, BL_PANEL_MIN, gPanelMax);
}

/* ---- for the light sensor under the screen (bsd/dev/shennong_sensors.c) ---------------------------------------- */

/* the panel's brightness level now (DBV): what was last sent, or the level the panel is started with */
extern "C" uint32_t
shennong_backlight_dbv(void)
{
	return gMode == 0 || gSent == ~0u ? 0x133 : gSent;
}

/* (i / 255)^2.2 x 65535: a pixel's value as light */
static const uint16_t gamma_lut[256] = {
	0, 0, 2, 4, 7, 11, 17, 24, 32, 42, 53, 65, 79, 94, 111, 129,
	148, 169, 192, 216, 242, 270, 299, 330, 362, 396, 432, 469, 508, 549, 591, 635,
	681, 729, 779, 830, 883, 938, 995, 1053, 1113, 1175, 1239, 1305, 1373, 1443, 1514, 1587,
	1663, 1740, 1819, 1900, 1983, 2068, 2155, 2243, 2334, 2427, 2521, 2618, 2717, 2817, 2920, 3024,
	3131, 3240, 3350, 3463, 3578, 3694, 3813, 3934, 4057, 4182, 4309, 4438, 4570, 4703, 4838, 4976,
	5115, 5257, 5401, 5547, 5695, 5845, 5998, 6152, 6309, 6468, 6629, 6792, 6957, 7124, 7294, 7466,
	7640, 7816, 7994, 8175, 8358, 8543, 8730, 8919, 9111, 9305, 9501, 9699, 9900, 10102, 10307, 10515,
	10724, 10936, 11150, 11366, 11585, 11806, 12029, 12254, 12482, 12712, 12944, 13179, 13416, 13655, 13896, 14140,
	14386, 14635, 14885, 15138, 15394, 15652, 15912, 16174, 16439, 16706, 16975, 17247, 17521, 17798, 18077, 18358,
	18642, 18928, 19216, 19507, 19800, 20095, 20393, 20694, 20996, 21301, 21609, 21919, 22231, 22546, 22863, 23182,
	23504, 23829, 24156, 24485, 24817, 25151, 25487, 25826, 26168, 26512, 26858, 27207, 27558, 27912, 28268, 28627,
	28988, 29351, 29717, 30086, 30457, 30830, 31206, 31585, 31966, 32349, 32735, 33124, 33514, 33908, 34304, 34702,
	35103, 35507, 35913, 36321, 36732, 37146, 37562, 37981, 38402, 38825, 39252, 39680, 40112, 40546, 40982, 41421,
	41862, 42306, 42753, 43202, 43654, 44108, 44565, 45025, 45487, 45951, 46418, 46888, 47360, 47835, 48313, 48793,
	49275, 49761, 50249, 50739, 51232, 51728, 52226, 52727, 53230, 53736, 54245, 54756, 55270, 55787, 56306, 56828,
	57352, 57879, 58409, 58941, 59476, 60014, 60554, 61097, 61642, 62190, 62741, 63295, 63851, 64410, 64971, 65535,
};

/*
 * How much light the screen gives off right above the light sensor: the average of red, green and blue as light
 * (0..65535 each), over a square of +-`radius' panel pixels around (`x', `y') on the 1440 x 3200 panel. Read from the
 * frame the display controller shows: pipes VIG0 and VIG1 both read it (SRC0_ADDR, SRC_YSTRIDE0; each half of it,
 * SRC_SIZE), 32 bits a pixel, blue first. False where it cannot be read.
 */
#define PANEL_W                 1440
#define PANEL_H                 3200
#define MDP_BASE                0x0ae00000
#define VIG0                    0x5000
#define VIG1                    0x7000

extern "C" bool
shennong_screen_light(uint32_t x, uint32_t y, uint32_t radius, uint32_t light[3])
{
	static volatile uint32_t * mdp;
	uint32_t phys, stride, size, width, height, sx, sy, sr, first, rows, n = 0;
	uint64_t sum[3] = { 0, 0, 0 };

	if (mdp == NULL) {
		mdp = (volatile uint32_t *) ml_io_map(MDP_BASE, 0x10000);
	}
	if (mdp == NULL) {
		return false;
	}
	phys = mdp[(VIG0 + 0x14) / 4];
	stride = mdp[(VIG0 + 0x24) / 4] & 0xffff;
	size = mdp[(VIG0 + 0x00) / 4];
	width = (size & 0xffff) * (mdp[(VIG1 + 0x14) / 4] == phys ? 2 : 1);
	height = size >> 16;
	if (phys == 0 || width == 0 || height == 0 || stride < width * 4) {
		return false;
	}
	sx = x * width / PANEL_W;
	sy = y * height / PANEL_H;
	sr = radius * width / PANEL_W;
	if (sr == 0) {
		sr = 1;
	}
	first = sy > sr ? sy - sr : 0;
	rows = 2 * sr + 1;
	if (first + rows > height || sx + sr >= width || sx < sr) {
		return false;
	}
	/*
	 * (each pixel read where it is, through the kernel's copy windows, ml_phys_read_word_64(): a mapping of the rows
	 * cost 288 KB of the kernel's map for every new frame buffer -- the display's pipes show the apps' surfaces
	 * themselves -- and maps are never given back here, so a long run ran out of it, 19 November)
	 */
	for (uint32_t dy = 0; dy < rows; dy += rows / 8 ? rows / 8 : 1) {
		for (uint32_t px = sx - sr; px <= sx + sr; px += sr / 4 ? sr / 4 : 1) {
			uint32_t pixel = ml_phys_read_word_64((addr64_t) phys + (uint64_t) (first + dy) * stride + px * 4);

			sum[0] += gamma_lut[(pixel >> 16) & 0xff];      /* red */
			sum[1] += gamma_lut[(pixel >> 8) & 0xff];
			sum[2] += gamma_lut[pixel & 0xff];              /* blue */
			n++;
		}
	}
	for (int c = 0; c < 3; c++) {
		light[c] = (uint32_t) (sum[c] / (n ? n : 1));
	}
	return true;
}
