/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <mach/thread_policy.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void redwood_tl(const char * format, ...) __printflike(1, 2);

/*
 * A few lines of text that stay on the panel over whatever iOS shows (redwood_overlay_text()): for
 * readings that must be seen while the phone is away from the Mac and its log cannot come back -- the
 * battery on the 120 W charger (bsd/dev/redwood_adsp.c, battmgr=3). White on black, the text console's
 * own font (osfmk/console/iso_font.c) three times its size, near the top; drawn again after every frame
 * and whenever its owner asks (the firmware's USB messages scroll over the whole screen otherwise).
 */
extern "C" unsigned char iso_font[256 * 16];
#define OVERLAY_LINES   28
#define OVERLAY_COLS    88
#define OVERLAY_TOP     150
static char gOverlay[OVERLAY_LINES][OVERLAY_COLS + 1];
static volatile uint32_t gOverlayLines;         /* lines in use; none: no overlay */
static volatile uint32_t gOverlayScale = 3;     /* 3: 58 columns of large letters; 2: 88 smaller ones, for a dump */

extern "C" void
redwood_overlay_scale(unsigned scale)
{
	gOverlayScale = scale == 2 ? 2 : 3;
}

extern "C" void
redwood_overlay_redraw(void)
{
	uint8_t * base = (uint8_t *) vinfo.v_baseaddr;
	uint32_t lines = gOverlayLines, OVERLAY_SCALE = gOverlayScale;

	if (lines == 0 || base == NULL || vinfo.v_depth != 32) {
		return;
	}
	for (unsigned l = 0; l < lines && l < OVERLAY_LINES; l++) {
		for (unsigned r = 0; r < 16 * OVERLAY_SCALE + 6; r++) {
			uint32_t y = OVERLAY_TOP + l * (16 * OVERLAY_SCALE + 6) + r;
			uint32_t * row;

			if (y >= vinfo.v_height) {
				return;
			}
			row = (uint32_t *) (base + (size_t) y * vinfo.v_rowbytes);
			for (unsigned c = 0; c < OVERLAY_COLS; c++) {
				unsigned char ch = (unsigned char) gOverlay[l][c];
				unsigned char bits = (ch != 0 && r < 16 * OVERLAY_SCALE) ? iso_font[ch * 16 + r / OVERLAY_SCALE] : 0;

				for (unsigned b = 0; b < 8 * OVERLAY_SCALE; b++) {
					uint32_t x = 12 + c * 8 * OVERLAY_SCALE + b;

					if (x < vinfo.v_width) {
						row[x] = (bits & (1u << (b / OVERLAY_SCALE))) ? 0xffffffffu : 0xff000000u;
					}
				}
			}
		}
	}
}

extern "C" void
redwood_overlay_text(unsigned line, const char * text)
{
	if (line >= OVERLAY_LINES) {
		return;
	}
	bzero(gOverlay[line], sizeof(gOverlay[line]));
	strlcpy(gOverlay[line], text, sizeof(gOverlay[line]));
	if (line + 1 > gOverlayLines) {
		gOverlayLines = line + 1;
	}
	redwood_overlay_redraw();
}

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 *
 * Boot-arg fb_phone=2796: the screen of the iPhone 14 Pro Max instead, 1290 x 2796 pixels, 430 x 932
 * points at three pixels a point (460 dots an inch). iOS 16 knows that phone, so MobileGestalt has a
 * screen class for it, and its artwork device subtype (2796, what the /product node says) is set to
 * match: with it SpringBoard lays out for that phone, Dynamic Island included (on this panel the
 * black island sits over the front camera's hole). fb_island=0: the subtype stays the iPhone X's
 * (2436), and with it the notch's status bar. Everything else the system says about itself stays
 * the iPhone X's (Settings still names it so).
 *
 * But only SpringBoard believes in the island: with a 932-point screen and the iPhone X's identity
 * MobileGestalt makes HomeButtonType 0 once its cache is made again, and the applications get a home
 * button's status bar and no gestures. Boot-arg fb_phone=2778: the screen of the iPhone 14 Plus (and
 * 13 Pro Max) instead: 1284 x 2778 pixels, 428 x 926 points, two fewer each way; a notch, no
 * island, and a phone iOS knows without a home button. Its artwork subtype is 2778 (fb_island=0: 2436).
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458
#define kFBProMaxWidth          1290
#define kFBProMaxHeight         2796
#define kFBProMaxDotsPerInch    460
static const struct fb_phone { uint32_t subtype, width, height, dpi; const char *name; } gFBPhones[] = {
	{ 2796, 1290, 2796, 460, "iPhone 14 Pro Max" },
	{ 2778, 1284, 2778, 458, "iPhone 14 Plus" },
};
static TUNABLE(uint32_t, gFBPhone, "fb_phone", 2436);
static TUNABLE(uint32_t, gFBIsland, "fb_island", 1);
/*
 * The /product node's home-button-type, MobileGestalt's HomeButtonType: 2, no button -- the gestures,
 * the home indicator and the status bar of a phone without one. The iPhone X's device tree has no such
 * property and MobileGestalt works its answer out otherwise; with the 14 Pro Max's screen that came to
 * 0, a home button (the iPhone 8's status bar, no gestures), as soon as its cache was made again.
 * It did not change that answer (so it is not where MobileGestalt has it from on this phone):
 * boot-arg fb_home=N puts it there, by default it is left out, as it was.
 */
static TUNABLE(uint32_t, gFBHome, "fb_home", 0);
static uint32_t gPhoneWidth = kFBDefaultWidth;          /* the phone's screen in pixels at 3x, and its */
static uint32_t gPointsWide = 375, gPointsHigh = 812;  /* size in points, and dots an inch */
static uint32_t gDotsPerInch = kFBDotsPerInch;

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points (430 x 932 with fb_phone=2796) drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
/*
 * Boot-arg fb_sync=0: a frame goes into the panel's memory as soon as its turn comes, as before.
 * The display controller sends that memory to the panel over and over, from the top line down
 * (on shennong 120 times a second), and a frame copied in while a refresh is half way down is
 * shown, for that refresh, half old and half new: a tear across the picture where the refresh
 * was, and with the rows shared out between processors (fb_threads) one at every share. By
 * default, on shennong, whose controller's line counter can be read (bsd/dev/redwood_adreno.c,
 * display_probe(), found it counting; see scanStart()), each share of the rows is copied only
 * once the refresh has gone past it, and all of them behind one and the same refresh: the
 * refresh after it shows the whole frame and nothing of the one before. The panel is then the
 * beat (fb_beat is not waited for).
 */
static TUNABLE(uint32_t, gSync, "fb_sync", 1);
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/redwood_input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static inline uint32_t
refreshHz(void)
{
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

/* ---- where the display controller is in sending the panel its memory (fb_sync) ---- */

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" const char * redwood_phone(void);
bool RedwoodGPUPanelScale(MiniIOSurface * s, uint32_t panelWidth, uint32_t panelHeight, const uint8_t ** pixels, uint32_t * rowBytes,
    uint64_t intoGpu = 0);
uint64_t RedwoodGPUMapPhysical(uint64_t phys, uint64_t size);

/* ---- fb_flip=1: two buffers, the display controller told which (needs fb_gpu=1) ---- */
/*
 * The firmware leaves control 0 staging pipes VIG0 and VIG1 (CTL_LAYER0 0x0100002d), each half of the 1440 x 3200
 * panel, both reading the framebuffer at 0xfc400000, rows of 5760 bytes (fb_probe=33, 13 November). The display's
 * reserved memory is 0x2b00000 bytes from there: a second framebuffer fits behind the first. A frame is drawn by the
 * GPU (RedwoodGPUPanelScale) into the one not shown, both pipes are given its address and flushed (CTL_FLUSH bits 0
 * and 1, Linux's dpu_hw_ctl), and the flush is taken at the start of the panel's next frame -- the bits read 0 again
 * then -- after which the other one is free: no copy by the processor, no tear.
 */
static TUNABLE(uint32_t, gFlip, "fb_flip", 0);
static TUNABLE(uint32_t, gVSyncPanel, "fb_vsync_panel", 1);   /* (0: the vsync a free timer, as before) */
extern "C" uint64_t kvtophys(vm_offset_t va);
extern "C" void redwood_cpufreq_now(char * out, size_t size);
#define FLIP_LIMIT              (0xfc400000u + 0x2b00000u - 0x1000u)   /* the display's reserved memory, less the command page */
#define FLIP_MDP                0x0ae00000u
#define FLIP_VIG0_ADDR          (0x5000u + 0x14u)
#define FLIP_VIG1_ADDR          (0x7000u + 0x14u)
#define FLIP_VIG0_STRIDE        (0x5000u + 0x24u)
#define FLIP_CTL0_FLUSH         (0x16000u + 0x18u)
#define FLIP_PIPES              0x3u            /* VIG0, VIG1 */
static volatile uint32_t * gFlipMdp;
static uint64_t gFlipPhys[2], gFlipGpu[2];
static unsigned gFlipFront;
static int gFlipState;                          /* 0 not tried, 1 on, -1 off for good */
static uint32_t gFlipFlips, gFlipSlow;
static uint64_t gFlipWait;

static bool
flipSetUp(const uint8_t * base, uint32_t rowBytes, uint32_t height)
{
	uint64_t size, a, b;

	if (gFlipState != 0) {
		return gFlipState > 0;
	}
	gFlipState = -1;
	size = (uint64_t) rowBytes * height;
	a = kvtophys((vm_offset_t) base);
	b = (a + size + 0xffffu) & ~(uint64_t) 0xffffu;
	if (b + size > FLIP_LIMIT) {
		IOLog("MiniIOMobileFramebuffer: fb_flip: no room for a second framebuffer (%#llx + %#llx past %#x): off\n", b, size, FLIP_LIMIT);
		return false;
	}
	gFlipMdp = (volatile uint32_t *) ml_io_map(FLIP_MDP, 0x20000);
	if (gFlipMdp == NULL || gFlipMdp[FLIP_VIG0_ADDR / 4] != (uint32_t) a || gFlipMdp[FLIP_VIG1_ADDR / 4] != (uint32_t) a ||
	    gFlipMdp[FLIP_VIG0_STRIDE / 4] != rowBytes) {
		IOLog("MiniIOMobileFramebuffer: fb_flip: the pipes are not as the firmware was seen to leave them (VIG0 %#x, VIG1 %#x, rows %u): off\n",
		    gFlipMdp ? gFlipMdp[FLIP_VIG0_ADDR / 4] : 0, gFlipMdp ? gFlipMdp[FLIP_VIG1_ADDR / 4] : 0, gFlipMdp ? gFlipMdp[FLIP_VIG0_STRIDE / 4] : 0);
		return false;
	}
	gFlipPhys[0] = a;
	gFlipPhys[1] = b;
	gFlipGpu[0] = RedwoodGPUMapPhysical(a, size);
	gFlipGpu[1] = RedwoodGPUMapPhysical(b, size);
	if (gFlipGpu[0] == 0 || gFlipGpu[1] == 0) {
		IOLog("MiniIOMobileFramebuffer: fb_flip: the framebuffers are not in the GPU's address space: off\n");
		return false;
	}
	gFlipFront = 0;
	gFlipState = 1;
	IOLog("MiniIOMobileFramebuffer: fb_flip: on: framebuffers at %#llx and %#llx (GPU %#llx, %#llx)\n", a, b, gFlipGpu[0], gFlipGpu[1]);
	return true;
}

/*
 * fb_dpu_scale=1 (with fb_flip=1): the frame copied into the framebuffer as it is (1290 x 2796, the GPU's copy without
 * its scaling), and the display controller's own scalers -- QSEED 3.3 in each VIG pipe, at 0xa00 in it (Linux's
 * dpu_hw_catalog.c, sm8650: dpu_vig_sblk_qseed3_3_3) -- take each half of it onto its half of the 1440 x 3200 panel,
 * bilinear, as Linux's dpu_plane _dpu_plane_setup_scaler3 and dpu_hw_setup_scaler3 set them up: the source rectangle,
 * the pixel extension's request (the source size), the phase steps in 21 fractional bits, preload 4 and 2, the op mode
 * (on, bilinear for colour and alpha, blended, not DX). The GPU's scaling of each frame -- 1.2 to 2 ms -- is gone.
 */
static TUNABLE(uint32_t, gDpuScale, "fb_dpu_scale", 0);
static uint32_t gDpuScaled[3];                  /* the source it was set up for: width, height, rows */

static void
dpuScaleSetUp(uint32_t srcW, uint32_t srcH, uint32_t rowBytes, uint32_t panelW, uint32_t panelH)
{
	static const uint32_t pipes[2] = { 0x5000u, 0x7000u };
	const uint32_t half = srcW / 2, outHalf = panelW / 2;

	if (gDpuScaled[0] == srcW && gDpuScaled[1] == srcH && gDpuScaled[2] == rowBytes) {
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		volatile uint32_t * p = gFlipMdp + pipes[i] / 4;
		volatile uint32_t * q = gFlipMdp + (pipes[i] + 0xa00u) / 4;

		p[0x00 / 4] = (srcH << 16) | half;              /* SRC_SIZE */
		p[0x04 / 4] = (srcH << 16) | srcW;              /* SRC_IMG_SIZE */
		p[0x08 / 4] = i == 0 ? 0 : half;                /* SRC_XY */
		p[0x0c / 4] = (panelH << 16) | outHalf;         /* OUT_SIZE (OUT_XY as the firmware left it) */
		p[0x24 / 4] = rowBytes;                         /* SRC_YSTRIDE0 */
		p[0x100 / 4] = 0;                               /* SW_PIX_EXT_C0_LR, _TB, _REQ_PIXELS; C1C2; C3 */
		p[0x104 / 4] = 0;
		p[0x108 / 4] = (srcH << 16) | half;
		p[0x110 / 4] = 0;
		p[0x114 / 4] = 0;
		p[0x118 / 4] = (srcH << 16) | half;
		p[0x120 / 4] = 0;
		p[0x124 / 4] = 0;
		p[0x128 / 4] = (srcH << 16) | half;
		q[0x90 / 4] = 0;                                /* PHASE_INIT_Y_H, _Y_V, _UV_H, _UV_V */
		q[0x94 / 4] = 0;
		q[0x98 / 4] = 0;
		q[0x9c / 4] = 0;
		q[0x10 / 4] = (uint32_t) (((uint64_t) half << 21) / outHalf);   /* PHASE_STEP_Y_H, _Y_V, _UV_H, _UV_V */
		q[0x14 / 4] = (uint32_t) (((uint64_t) srcH << 21) / panelH);
		q[0x18 / 4] = (uint32_t) (((uint64_t) half << 21) / outHalf);
		q[0x1c / 4] = (uint32_t) (((uint64_t) srcH << 21) / panelH);
		q[0x20 / 4] = 4u | (2u << 8) | (4u << 16) | (2u << 24);        /* PRELOAD */
		q[0x40 / 4] = half | (srcH << 16);              /* SRC_SIZE_Y_RGB_A, _UV */
		q[0x44 / 4] = half | (srcH << 16);
		q[0x48 / 4] = outHalf | (panelH << 16);         /* DST_SIZE */
		q[0x04 / 4] = 1u | (3u << 16) | (1u << 31) | (1u << 14) | (1u << 10) | (1u << 29);   /* OP_MODE */
	}
	gDpuScaled[0] = srcW;
	gDpuScaled[1] = srcH;
	gDpuScaled[2] = rowBytes;
	IOLog("MiniIOMobileFramebuffer: fb_dpu_scale: frames of %ux%u (rows of %u bytes) taken onto the %ux%u panel by the display's scalers "
	    "(VIG0 scaler op mode %#x, step %#x x %#x)\n", srcW, srcH, rowBytes, panelW, panelH, gFlipMdp[(0x5000u + 0xa04u) / 4],
	    gFlipMdp[(0x5000u + 0xa10u) / 4], gFlipMdp[(0x5000u + 0xa14u) / 4]);
}

/* (back to the pipes as the firmware left them: each half of the panel-sized framebuffer as it is, no scaler) */
static void
dpuScaleOff(uint32_t rowBytes, uint32_t panelW, uint32_t panelH)
{
	static const uint32_t pipes[2] = { 0x5000u, 0x7000u };

	if (gDpuScaled[0] == 0) {
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		volatile uint32_t * p = gFlipMdp + pipes[i] / 4;

		p[0x00 / 4] = (panelH << 16) | (panelW / 2);
		p[0x04 / 4] = (panelH << 16) | panelW;
		p[0x08 / 4] = i == 0 ? 0 : panelW / 2;
		p[0x24 / 4] = rowBytes;
		p[0x108 / 4] = (panelH << 16) | (panelW / 2);
		p[0x118 / 4] = (panelH << 16) | (panelW / 2);
		p[0x128 / 4] = (panelH << 16) | (panelW / 2);
		gFlipMdp[(pipes[i] + 0xa04u) / 4] = 0;
	}
	gDpuScaled[0] = gDpuScaled[1] = gDpuScaled[2] = 0;
}

/* Both pipes onto framebuffer WHICH, from the panel's next frame; false: the controller did not take it in 50 ms */
static bool
flipTo(unsigned which)
{
	uint64_t began = mach_absolute_time(), ns;
	uint32_t ms;

	gFlipMdp[FLIP_VIG0_ADDR / 4] = (uint32_t) gFlipPhys[which];
	gFlipMdp[FLIP_VIG1_ADDR / 4] = (uint32_t) gFlipPhys[which];
	__asm__ volatile ("dsb sy" ::: "memory");
	gFlipMdp[FLIP_CTL0_FLUSH / 4] = FLIP_PIPES;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (ms = 0; (gFlipMdp[FLIP_CTL0_FLUSH / 4] & FLIP_PIPES) && ms < 50; ms++) {
		IODelay(100);
		if (!(gFlipMdp[FLIP_CTL0_FLUSH / 4] & FLIP_PIPES)) {
			break;
		}
		IOSleep(1);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	gFlipWait += ns;
	if (gFlipMdp[FLIP_CTL0_FLUSH / 4] & FLIP_PIPES) {
		gFlipSlow++;
		IOLog("MiniIOMobileFramebuffer: fb_flip: the flush was not taken in 50 ms (CTL_FLUSH %#x, VIG0 %#x): back to copying\n",
		    gFlipMdp[FLIP_CTL0_FLUSH / 4], gFlipMdp[FLIP_VIG0_ADDR / 4]);
		return false;
	}
	gFlipFront = which;
	gFlipFlips++;
	return true;
}

const char * RedwoodGPUPanelState(void);
static uint32_t gPartLayers;            /* fb_gpu: layers drawn that were not a whole frame onto the whole panel */
static int32_t gPartLast[8];

#define SCAN_GCC_DISP_AHB       0x00127004      /* GCC's bus clock of the display's clock controller */
#define SCAN_DISPCC             0x0af00000      /* the display's clock controller (dt: clock-controller@af00000, */
#define SCAN_DISPCC_SIZE        0x10000         /* "qcom,pineapple-dispcc": Linux's dispcc-sm8550.c has its registers) */
#define SCAN_MDSS_GDSCR         0x9000          /* ... the display block's power switch (mdss_gdsc) */
#define SCAN_MDSS_AHB_CBCR      0x80a4          /* ... the bus clock of its registers (disp_cc_mdss_ahb_clk) */
#define SCAN_INTF               0x0ae36000      /* the interface of the first DSI (dt: qcom,sde-intf-off 0x36000 of 0xae00000) */
#define SCAN_INTF2              0x0ae37000      /* ... of the second */
#define SCAN_TIMING_ENGINE_EN   0x000           /* bit 0: the interface sends frames by itself (a video mode panel) */
#define SCAN_HSYNC_CTL          0x008           /* clocks a line << 16 */
#define SCAN_VSYNC_PERIOD       0x00c           /* clocks a frame */
#define SCAN_DISPLAY_V_START    0x01c           /* clocks before the first line shown */
#define SCAN_FRAME_COUNT        0x0ac
#define SCAN_LINE_COUNT         0x0b0           /* 0 to the lines of a frame less one, porches with them */
#define SCAN_TEAR_CHECK_EN      0x280           /* a command mode panel's tear check (Linux's dpu_hw_intf.c, INTF_TEAR_*) */
#define SCAN_TEAR_INT_COUNT_VAL 0x294           /* the panel's read pointer: frame << 16 | line */
#define SCAN_TEAR_LINE_COUNT    0x2a8           /* the line the controller has written to the panel */
#define SCAN_TEAR_AUTOREFRESH   0x2b4           /* bit 31: a frame is sent to the panel's memory at each of its refreshes */

static volatile uint32_t * gScan;               /* the interface's registers, when they can be read */
static uint32_t gScanFirst;                     /* lines of a frame before its first row shown */
static uint32_t gScanLines;                     /* lines of a frame, porches with them */
static uint64_t gScanLine;                      /* one line's time, in mach_absolute_time */
static const char * gScanWhy = "not tried";     /* why not, said every five seconds with the frames */
static uint32_t gScanSeen[3];                   /* ... and the registers that said so */
static volatile SInt32 gScanTorn;               /* shares the next refresh came to before they were in */
static volatile SInt32 gScanShares;
/* the first torn pieces of every five seconds, for telling what held them up */
#define SCAN_TORN_KEPT          12
typedef struct {
	int32_t         row, at, chosenAt;      /* the piece; where the refresh was when it was in; when the frame was given its refresh */
	uint32_t        copier, ahead;          /* which copier; refreshes past the one it went behind */
	uint64_t        since, waited, took;    /* from the frame's refresh chosen to the piece in; waiting for the refresh; copying */
} ScanTorn;
static ScanTorn gScanTornKept[SCAN_TORN_KEPT];
static int32_t gScanChosenAt;
static uint64_t gScanChosenTime;
/* what the interfaces were found doing: their registers 0x000 to 0x2bc, and the counters read a millisecond apart */
#define SCAN_SAMPLES            12
static uint32_t gScanRegs[2][0x2c0 / 4];
static uint32_t gScanSamples[SCAN_SAMPLES][5];
static bool gScanRead;
/*
 * A command mode panel (shennong's, as the firmware leaves it: 28 October) keeps its picture in
 * a memory of its own, and the interface sends ours to it, all of it, at every one of the panel's
 * refreshes (autorefresh), 60 a second: seen, line 1 for some 9.5 ms, then 1 to 3200 in 7.1 ms
 * (450 lines a millisecond), the frame counter up by one at the end. A row is safe to write once
 * it has been sent and until it is sent again, more than 9 ms later.
 */
static bool gScanCommand;
/*
 * ... and at 120 a second (fb_panel_hz=120) the idle part is gone: a frame every 8.3 ms, 7.1 of
 * them sending, and a row is sent again 8.3 ms after it was. A share copied in behind a refresh
 * that is far down already is not in before the next one comes round to its first row (the
 * phone, 28 October: 30 shares in 100). gScanPeriod is the panel's frame (measured once its rate
 * is set; 1/60 s before), gShareTime how long a piece of rows takes to copy (drawShare()), the longest lately.
 */
static uint64_t gScanPeriod;
static uint64_t gShareTime;
/*
 * Boot-arg fb_rt=0: the threads that copy frames to the panel run at a kernel thread's priority,
 * as before. By default they are real-time threads (a share of a frame every 8.3 ms, some 2 ms of
 * work): at 120 a second a share took 1.5 ms one moment and 7.5 the next (the phone, 28 October),
 * the copiers put off by backboardd's render thread, which is real-time itself, and a share that
 * long is not in before the next refresh comes round to it whatever refresh it goes behind.
 */
static TUNABLE(uint32_t, gRealtime, "fb_rt", 1);

extern "C" kern_return_t thread_policy_set(thread_t thread, thread_policy_flavor_t flavor, thread_policy_t policy_info,
    mach_msg_type_number_t count);

static void
copierRealtime(thread_t thread)
{
	thread_time_constraint_policy_data_t policy;
	uint64_t period, computation, constraint;
	kern_return_t kr;

	if (!gRealtime) {
		return;
	}
	nanoseconds_to_absolutetime(NSEC_PER_SEC / 120, &period);
	nanoseconds_to_absolutetime(3 * NSEC_PER_MSEC, &computation);
	nanoseconds_to_absolutetime(8 * NSEC_PER_MSEC, &constraint);
	policy.period = (uint32_t) period;
	policy.computation = (uint32_t) computation;
	policy.constraint = (uint32_t) constraint;
	policy.preemptible = TRUE;
	kr = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t) &policy,
	    THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	if (kr != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: fb_rt: not real-time (%#x)\n", kr);
	}
}

static void
shareTook(uint64_t began)
{
	uint64_t took = mach_absolute_time() - began, longest = gShareTime;

	/* (racy between the copiers: one estimate a little off for a frame) */
	gShareTime = took > longest - longest / 32 ? took : longest - longest / 32;
}

/*
 * Boot-arg fb_probe=1 (for 120 a second, next): what the DSI controller that talks to the panel
 * and the display controller's pipes and controls are found doing, read once and said with the
 * first report. Only read: the registers that are not 0.
 */
extern "C" uint64_t kvtophys(vm_offset_t va);
static TUNABLE(uint32_t, gProbe, "fb_probe", 0);
#define PROBE_MDP               0x0ae00000
#define PROBE_DSI0              0x0ae94000
static const uint32_t gProbeSSPP[] = { 0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000, 0x2d000, 0x2f000 };
static const uint32_t gProbeCTL[] = { 0x16000, 0x17000, 0x18000, 0x19000, 0x1a000, 0x1b000 };
static char gProbeText[6144];
static size_t gProbeUsed;

static void
probeBlock(const char * name, volatile uint32_t * regs, uint32_t phys, uint32_t size)
{
	size_t line = gProbeUsed;

	if (gProbeUsed + 64 >= sizeof(gProbeText)) {
		return;
	}
	gProbeUsed += snprintf(gProbeText + gProbeUsed, sizeof(gProbeText) - gProbeUsed, "%s %#x:", name, phys);
	for (uint32_t i = 0; i < size / 4 && gProbeUsed + 40 < sizeof(gProbeText); i++) {
		uint32_t value = regs[i];

		if (value == 0) {
			continue;
		}
		if (gProbeUsed - line > 400) {
			gProbeUsed += snprintf(gProbeText + gProbeUsed, sizeof(gProbeText) - gProbeUsed, "\n%s %#x (more):", name, phys);
			line = gProbeUsed;
		}
		gProbeUsed += snprintf(gProbeText + gProbeUsed, sizeof(gProbeText) - gProbeUsed, " %x:%x", i * 4, value);
	}
	gProbeUsed += snprintf(gProbeText + gProbeUsed, sizeof(gProbeText) - gProbeUsed, "\n");
}

/*
 * The phone restarted with all of it at once (28 October): one of these reads is not answered.
 * Now fb_probe is a mask of what is read, each announced in the kernel's log and a few seconds
 * given to the loader to take that down before: 1 the framebuffer's address, 2 the controls,
 * 4 the pipes, 8 the top of the display controller, 16 the DSI controller (its first 0x100 bytes).
 */
static void
probeAnnounce(const char * what, bool wait)
{
	printf("MiniIOMobileFramebuffer: fb_probe: about to read %s\n", what);
	if (wait) {
		IOSleep(3000);
	}
}

/* What was read, said at once (28 October: with the probe in start(), SpringBoard never got to a first frame). */
static void
probePrint(void)
{
	/* (kept, and said again with the first frame lines: what is said in start() is gone from the log by its end) */
	for (char * text = gProbeText, * end; text < gProbeText + gProbeUsed && *text != 0; text = end + 1) {
		end = strchr(text, '\n');
		if (end == NULL) {
			break;
		}
		*end = 0;
		IOLog("MiniIOMobileFramebuffer: fb_probe: %s\n", text);
		*end = '\n';
	}
}

static void
probeDisplay(const uint8_t * framebuffer)
{
	volatile uint32_t * mdp = (volatile uint32_t *) ml_io_map(PROBE_MDP, 0x84000);
	volatile uint32_t * dsi = (volatile uint32_t *) ml_io_map(PROBE_DSI0, 0x1000);

	if (mdp == NULL || dsi == NULL) {
		return;
	}
	if (gProbe & 1) {
		probeAnnounce("where the framebuffer is", false);
		gProbeUsed += snprintf(gProbeText + gProbeUsed, sizeof(gProbeText) - gProbeUsed,
		    "the framebuffer is at %#llx\n", (unsigned long long) kvtophys((vm_offset_t) framebuffer));
	}
	if (gProbe & 2) {
		probeAnnounce("the controls (ctl 0 to 5, 0xae16000 on)", true);
		for (unsigned i = 0; i < sizeof(gProbeCTL) / sizeof(gProbeCTL[0]); i++) {
			probeBlock("ctl", mdp + gProbeCTL[i] / 4, PROBE_MDP + gProbeCTL[i], 0x100);
		}
	}
	if (gProbe & 4) {
		probeAnnounce("the pipes (sspp, 0xae05000 on)", true);
		for (unsigned i = 0; i < sizeof(gProbeSSPP) / sizeof(gProbeSSPP[0]); i++) {
			volatile uint32_t * pipe = mdp + gProbeSSPP[i] / 4;

			if (pipe[0] != 0) {             /* (SSPP_SRC_SIZE: a pipe in use) */
				probeBlock("sspp", pipe, PROBE_MDP + gProbeSSPP[i], 0x100);
			}
		}
	}
	/* 32: only the two pipes control 0 stages (CTL_LAYER0 0x0100002d: VIG0 and VIG1 at stage 4, the border colour;
	 * fb_probe=3, 13 November), their first 0x40 bytes -- SRC_SIZE, SRC_IMG_SIZE, SRC_XY, OUT_SIZE, OUT_XY, SRC0..3_ADDR,
	 * the strides, SRC_FORMAT, SRC_UNPACK_PATTERN, SRC_OP_MODE -- and without the pauses: fb_probe=3's three seconds
	 * in start() left the window server without a first frame */
	if (gProbe & 32) {
		probeAnnounce("pipes VIG0 and VIG1 (0xae05000, 0xae07000), 0x40 bytes each", false);
		probeBlock("vig0", mdp + 0x5000 / 4, PROBE_MDP + 0x5000, 0x40);
		probeBlock("vig1", mdp + 0x7000 / 4, PROBE_MDP + 0x7000, 0x40);
	}
	if (gProbe & 8) {
		probeAnnounce("the top of the display controller (0xae01000)", true);
		probeBlock("mdp top", mdp + 0x1000 / 4, PROBE_MDP + 0x1000, 0x400);
	}
	if (gProbe & 16) {
		/* (read without harm, 28 October; now all of it) */
		probeAnnounce("the DSI controller (0xae94000)", false);
		probeBlock("dsi0", dsi, PROBE_DSI0, 0x400);
	}
	printf("MiniIOMobileFramebuffer: fb_probe: all read\n");
	probePrint();
}

/*
 * Boot-arg fb_panel_hz=120 (or 60): the panel is told to refresh so many times a second, with the
 * commands the phone's own device tree has for it (qcom,mdss_dsi_n2_42_02_0a_dsc_cmd, and its p11
 * and p2 the same: "qcom,mdss-dsi-timing-switch-command" of timing@wqhd_normal_120hz_index_01 and
 * _60hz_index_00; the line rate, the porches and the compression are the same in both). They go
 * as the firmware left the DSI controller sending them (28 October, fb_probe=16: commands from
 * memory, CMD_DMA_CTRL 0x14000000, DMA_BASE 0xd069b000 a physical address under 4 GB, DMA_LEN 4,
 * triggered by software, TRIG_CTRL 0x80000004), one packet at a time, in the 9 ms after a
 * frame has gone to the panel and before the next goes (the line counter at 1, the frame counter
 * just up by one). The page they are in is the last of the firmware's splash region (dt:
 * reserved-memory/splash_region, 0xfc400000, 0x2b00000 bytes; the framebuffer is its first
 * 18 MB), memory the display controller is known to read: from a page of the kernel's own under
 * 4 GB, held back by BLOCK_DMA_WITHIN_FRAME, the first command was never sent and the panel was
 * sent no frame after it (28 October). The panel keeps none of it: a restart is 60 a second again.
 */
static TUNABLE(uint32_t, gPanelHz, "fb_panel_hz", 0);
#define DSI_CTRL                0x004           /* (the controller's registers, Linux's dsi.xml plus 4: a 6G one) */
#define DSI_CMD_DMA_CTRL        0x03c
#define DSI_DMA_BASE            0x048
#define DSI_DMA_LEN             0x04c
#define DSI_ACK_ERR_STATUS      0x068
#define DSI_TRIG_CTRL           0x084
#define DSI_TRIG_DMA            0x090
#define DSI_INTR_CTRL           0x110           /* bit 0: a command sent (written 1 to clear) */
#define PANEL_COMMAND_PAGE      (0xfc400000u + 0x2b00000u - 0x1000u)

/* each: the packet's data type, then its bytes (DCS: 0x15 one parameter, 0x39 long) */
static const uint8_t gPanel120[] = {
	0x15, 2, 0x26, 0x00,
	0x15, 2, 0x2f, 0x00,
	0x39, 6, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00,
	0x15, 2, 0xbc, 0x00,
	0x15, 2, 0x2c, 0x00,
	0
};
static const uint8_t gPanel60[] = {
	0x15, 2, 0x26, 0x01,
	0x15, 2, 0x2f, 0x00,
	0x15, 2, 0x2f, 0x30,
	0x39, 6, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00,
	0x15, 2, 0x6f, 0x1c,
	0x39, 10, 0xba, 0x91, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00,
	0x15, 2, 0xbc, 0x20,
	0x15, 2, 0x2c, 0x00,
	0
};

/*
 * The panel's other timings, from the same device tree node (timing@wqhd_*: 90, 40 and 30 a second, and the
 * ones where the panel itself lowers its rate while it is sent nothing new: "idle 60 to 1 Hz", "test 120 to
 * 0.5 Hz", "auto 120 to 60"). Register 0xba (after 0x6f 0x1c, in the page f0 55 aa 52 08 00) is how many of
 * the base rate's refreshes are skipped, 0xbc the base (0x00 120, 0x20 60, 0x40 40, 0x60 30).
 */
static const uint8_t gPanel90[] = {
	0x15, 2, 0x2f, 0x01,
	0x39, 6, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00,
	0x15, 2, 0xbc, 0x00,
	0
};
static const uint8_t gPanel40[] = {
	0x15, 2, 0x26, 0x01, 0x15, 2, 0x2f, 0x00, 0x15, 2, 0x2f, 0x30,
	0x39, 6, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00, 0x15, 2, 0x6f, 0x1c,
	0x39, 10, 0xba, 0x91, 0x01, 0x01, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00,
	0x15, 2, 0xbc, 0x40, 0x15, 2, 0x2c, 0x00,
	0
};
static const uint8_t gPanel30[] = {
	0x15, 2, 0x26, 0x01, 0x15, 2, 0x2f, 0x00, 0x15, 2, 0x2f, 0x30,
	0x39, 6, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00, 0x15, 2, 0x6f, 0x1c,
	0x39, 10, 0xba, 0x91, 0x01, 0x01, 0x00, 0x01, 0x03, 0x03, 0x00, 0x00,
	0x15, 2, 0xbc, 0x60, 0x15, 2, 0x2c, 0x00,
	0
};
static const uint8_t gPanelIdle1[] = {         /* idle 60 to 1 Hz */
	0x15, 2, 0x26, 0x01, 0x15, 2, 0x2f, 0x00, 0x15, 2, 0x2f, 0x30,
	0x39, 6, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00, 0x15, 2, 0x6f, 0x1c,
	0x39, 10, 0xba, 0x91, 0x0b, 0x03, 0x00, 0x11, 0x77, 0x77, 0x00, 0x05,
	0x15, 2, 0xbc, 0x20, 0x15, 2, 0x2c, 0x00,
	0
};
static const uint8_t gPanelHalf[] = {          /* test 120 to 0.5 Hz */
	0x15, 2, 0x26, 0x01, 0x15, 2, 0x2f, 0x00, 0x15, 2, 0x2f, 0x30,
	0x39, 6, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00, 0x15, 2, 0x6f, 0x1c,
	0x39, 10, 0xba, 0x91, 0xef, 0xef, 0x00, 0x01, 0xef, 0xef, 0x00, 0x00,
	0x15, 2, 0xbc, 0x00, 0x15, 2, 0x2c, 0x00,
	0
};
static const uint8_t gPanelAuto120[] = {       /* auto 120 to 60 */
	0x15, 2, 0x26, 0x00, 0x15, 2, 0x2f, 0x30,
	0x39, 6, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00, 0x15, 2, 0x6f, 0x1c,
	0x39, 10, 0xba, 0x91, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00,
	0x15, 2, 0x5a, 0x00, 0x15, 2, 0xbc, 0x00, 0x15, 2, 0x2c, 0x00,
	0
};

/* The frames a second the interface sends the panel now (its frame counter over `ms`), in tenths. */
static uint32_t
panelMeasure(volatile uint32_t * intf, uint32_t ms)
{
	uint64_t began, nanos;
	uint32_t frames;

	frames = intf[SCAN_FRAME_COUNT / 4];
	began = mach_absolute_time();
	IOSleep(ms);
	frames = intf[SCAN_FRAME_COUNT / 4] - frames;
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &nanos);
	return (uint32_t) ((uint64_t) frames * 10 * NSEC_PER_SEC / (nanos ? nanos : 1));
}

extern "C" void shennong_dsi_lock(void);       /* (ShennongBacklight.cpp) */
extern "C" void shennong_dsi_unlock(void);

/* `commands` to the panel between two frames; how many were sent (-1: none could be) */
static int
panelSend(volatile uint32_t * intf, const uint8_t * commands)
{
	static volatile uint32_t * dsi;         /* (mapped once: this is called again and again) */
	static volatile uint8_t * packet;
	uint64_t phys = PANEL_COMMAND_PAGE;
	uint32_t ctrl, trig, frame;
	int sent = 0;

	if (dsi == NULL) {
		dsi = (volatile uint32_t *) ml_io_map(PROBE_DSI0, 0x1000);
	}
	if (packet == NULL) {
		packet = (volatile uint8_t *) ml_io_map(PANEL_COMMAND_PAGE, 0x1000);
	}

	if (commands == NULL || dsi == NULL || packet == NULL) {
		return -1;
	}
	ctrl = dsi[DSI_CTRL / 4];
	trig = dsi[DSI_TRIG_CTRL / 4];
	/* (as found: enabled, in command mode, commands from memory, triggered by software) */
	if ((ctrl & 0x5) != 0x5 || !(dsi[DSI_CMD_DMA_CTRL / 4] & 1u << 28) || (trig & 7) != 4) {
		IOLog("MiniIOMobileFramebuffer: fb_panel_hz: the DSI controller is not as the firmware was seen to leave it "
		    "(ctrl %#x, cmd_dma_ctrl %#x, trig_ctrl %#x): nothing sent\n", ctrl, dsi[DSI_CMD_DMA_CTRL / 4], trig);
		return -1;
	}
	/* the moment a frame has gone: the frame counter up by one (it is not more than 17 ms away) */
	frame = intf[SCAN_FRAME_COUNT / 4];
	for (uint32_t waited = 0; intf[SCAN_FRAME_COUNT / 4] == frame && waited < 40000; waited += 5) {
		IODelay(5);
	}
	if (intf[SCAN_FRAME_COUNT / 4] == frame) {
		IOLog("MiniIOMobileFramebuffer: fb_panel_hz: no frame went to the panel in 40 ms: nothing sent\n");
		return -1;
	}
	shennong_dsi_lock();    /* (the DSI controller's command registers are the backlight's too: ShennongBacklight.cpp) */
	for (const uint8_t * c = commands; *c != 0; c += 2 + c[1]) {
		uint8_t type = c[0], length = c[1];
		uint32_t size, status = 0;

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
		dsi[DSI_INTR_CTRL / 4] = dsi[DSI_INTR_CTRL / 4] | 1;    /* (a command sent before: forgotten) */
		dsi[DSI_DMA_BASE / 4] = (uint32_t) phys;
		dsi[DSI_DMA_LEN / 4] = size;
		__asm__ volatile ("dsb sy" ::: "memory");
		dsi[DSI_TRIG_DMA / 4] = 1;
		for (uint32_t waited = 0; waited < 1500; waited += 5) {     /* (a short packet takes some us) */
			status = dsi[DSI_INTR_CTRL / 4];
			if (status & 1) {
				break;
			}
			IODelay(5);
		}
		if (!(status & 1)) {
			IOLog("MiniIOMobileFramebuffer: fb_panel_hz: the command %#x %#x was not sent in 1.5 ms (intr_ctrl %#x, "
			    "ack_err %#x): the rest is not sent\n", type, c[2], status, dsi[DSI_ACK_ERR_STATUS / 4]);
			break;
		}
		dsi[DSI_INTR_CTRL / 4] = status | 1;
		sent++;
	}
	shennong_dsi_unlock();
	if (sent > 0 && dsi[DSI_ACK_ERR_STATUS / 4] != 0) {
		IOLog("MiniIOMobileFramebuffer: panel commands: ack_err %#x\n", dsi[DSI_ACK_ERR_STATUS / 4]);
	}
	return sent;
}

static void
panelRate(volatile uint32_t * intf)
{
	const uint8_t * commands = gPanelHz == 120 ? gPanel120 : gPanelHz == 60 ? gPanel60 : NULL;
	uint32_t tenths;
	int sent;

	if (commands == NULL) {
		IOLog("MiniIOMobileFramebuffer: fb_panel_hz=%u: only 120 and 60 are known\n", gPanelHz);
		return;
	}
	sent = panelSend(intf, commands);
	if (sent < 0) {
		return;
	}
	/* how many frames a second the panel asks for now: the frame counter over 200 ms */
	IOSleep(50);
	tenths = panelMeasure(intf, 200);
	IOLog("MiniIOMobileFramebuffer: fb_panel_hz=%u: %d commands sent to the panel; it is now sent %u frames a second\n",
	    gPanelHz, sent, tenths / 10);
	if (tenths >= 250) {
		nanoseconds_to_absolutetime(NSEC_PER_SEC * 10 / tenths, &gScanPeriod);
	}
}

/*
 * Boot-arg fb_panel_test=S: S seconds after the display came up, every timing the panel has is tried in turn
 * for 3 s each and the frames a second the interface then sends it are measured twice (the first second, and
 * the last two -- the panel's own lowering of its rate takes a while to start); then back to fb_panel_hz (or
 * 120). Lines "fb_panel_test: <timing>: ...". While it runs the screen should be left alone and looked at:
 * a timing that flickers or tears is to be seen, not measured.
 */
static TUNABLE(uint32_t, gPanelTest, "fb_panel_test", 0);

/*
 * Boot-arg fb_dyn=1: the panel's rate follows what is on the screen. A frame drawn (drawLayer) or a finger on the
 * glass (shennong_touch_boosted, the thermal governor's touch boost) puts it at 120 at once; nothing new for
 * fb_dyn_mid ms (150) lowers it to 60, for fb_dyn_low ms (1000) to 30. While it is low the thread looks every
 * 10 ms (a frame or a finger comes to wait at most that and one refresh of 30); at 120 every 50 ms. The commands
 * go between two frames (panelSend), and gScanPeriod follows, for the copiers' timing. Every 5 s a line:
 * "fb_dyn: the last 5 s: 120 a%, 60 b%, 30 c%; n changes".
 */
static TUNABLE(uint32_t, gDyn, "fb_dyn", 2);         /* (on by default since 18 November: fb_dyn=0 off, 1 down to 30 only) */
static TUNABLE(uint32_t, gDynMidMs, "fb_dyn_mid", 150);
static TUNABLE(uint32_t, gDynLowMs, "fb_dyn_low", 1000);
static volatile uint64_t gDynLastFrame;         /* mach_absolute_time of the last layer drawn */
extern "C" bool shennong_touch_boosted(void);
/*
 * fb_dyn=2: and after fb_dyn_deep ms (3000) of nothing new, deeper: the panel is put in its "idle 60 to 1 Hz"
 * timing and the interface's autorefresh is stopped, so that nothing is sent to it and it shows what its own
 * memory has, lowering its own rate as far as 1 a second. The first frame drawn (drawLayer, at once) or a
 * finger (the thread, within 10 ms) starts the autorefresh again as it was, then 120. The panel's own
 * refreshes go on being counted by the tear check (its read pointer's frame count): "panel refreshes" in the
 * 5 s line, so that how low it went is measured, not guessed.
 */
static TUNABLE(uint32_t, gDynDeepMs, "fb_dyn_deep", 3000);
static volatile uint32_t * gDynIntf;
static volatile uint32_t gDynAutorefresh;       /* the autorefresh register as it was, while stopped; 0 running */
static uint32_t gDynFramesAt;

static void
dynWake(void)
{
	uint32_t was = gDynAutorefresh;

	if (was != 0 && OSCompareAndSwap(was, 0, &gDynAutorefresh)) {
		gDynIntf[SCAN_TEAR_AUTOREFRESH / 4] = was;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
}

static void
dynNoteFrame(void)
{
	gDynLastFrame = mach_absolute_time();
	dynWake();
}

static void
dynMain(void * arg, __unused wait_result_t wr)
{
	volatile uint32_t * intf = (volatile uint32_t *) arg;
	uint32_t hz = gPanelHz == 60 ? 60 : 120, changes = 0, teAt = intf[SCAN_TEAR_INT_COUNT_VAL / 4] >> 16;
	uint64_t mid, low, deep, said = mach_absolute_time(), at = said, period, timeAt[4] = { 0, 0, 0, 0 };

	gDynIntf = intf;
	nanoseconds_to_absolutetime((uint64_t) gDynDeepMs * NSEC_PER_MSEC, &deep);

	if (gPanelHz == 0 && panelSend(intf, gPanel120) >= 0) {
		hz = 120;
	}
	nanoseconds_to_absolutetime((uint64_t) gDynMidMs * NSEC_PER_MSEC, &mid);
	nanoseconds_to_absolutetime((uint64_t) gDynLowMs * NSEC_PER_MSEC, &low);
	nanoseconds_to_absolutetime(5 * NSEC_PER_SEC, &period);
	IOLog("MiniIOMobileFramebuffer: fb_dyn: on: 120 on a frame or a touch, 60 after %u ms of nothing new, 30 after %u ms\n",
	    gDynMidMs, gDynLowMs);
	for (;;) {
		uint64_t now = mach_absolute_time(), quiet = now - gDynLastFrame;
		bool touched = shennong_touch_boosted();
		uint32_t want = touched || quiet < mid ? 120 : quiet < low ? 60 : gDyn >= 2 && quiet >= deep ? 1 : 30;

		timeAt[hz == 120 ? 0 : hz == 60 ? 1 : hz == 30 ? 2 : 3] += now - at;
		at = now;
		if (touched) {
			dynWake();
		}
		if (hz == 1 && gDynAutorefresh == 0) {
			hz = 30;        /* (woken by a frame: the autorefresh runs again, at the idle timing's rate) */
		}
		if (want == 1 && hz != 1) {
			/* the idle timing first, while frames still go (panelSend waits for one), then the autorefresh stopped
			 * right after a frame has gone */
			if (panelSend(intf, gPanelIdle1) > 0) {
				uint32_t frame = intf[SCAN_FRAME_COUNT / 4], value = intf[SCAN_TEAR_AUTOREFRESH / 4];

				for (uint32_t waited = 0; intf[SCAN_FRAME_COUNT / 4] == frame && waited < 40000; waited += 5) {
					IODelay(5);
				}
				if ((value & 1u << 31) && gDynLastFrame == now - quiet && !shennong_touch_boosted()) {
					gDynAutorefresh = value;
					intf[SCAN_TEAR_AUTOREFRESH / 4] = 0;
					__asm__ volatile ("dsb sy" ::: "memory");
					hz = 1;
					changes++;
				} else {
					hz = 60;        /* (the idle timing's base: put right below) */
				}
			}
		} else if (want != hz && want != 1) {
			const uint8_t * commands = want == 120 ? gPanel120 : want == 60 ? gPanel60 : gPanel30;

			if (panelSend(intf, commands) > 0) {
				hz = want;
				changes++;
				nanoseconds_to_absolutetime(NSEC_PER_SEC / hz, &gScanPeriod);
			}
		}
		if (now - said >= period) {
			uint64_t all = timeAt[0] + timeAt[1] + timeAt[2] + timeAt[3];
			uint32_t te = intf[SCAN_TEAR_INT_COUNT_VAL / 4] >> 16;

			if (all == 0) {
				all = 1;
			}
			IOLog("MiniIOMobileFramebuffer: fb_dyn: the last 5 s: 120 %llu%%, 60 %llu%%, 30 %llu%%, stopped %llu%%; %u changes; "
			    "now %u; panel refreshes %u, frames sent %u\n",
			    timeAt[0] * 100 / all, timeAt[1] * 100 / all, timeAt[2] * 100 / all, timeAt[3] * 100 / all, changes, hz,
			    (te - teAt) & 0xffff, intf[SCAN_FRAME_COUNT / 4] - gDynFramesAt);
			teAt = te;
			gDynFramesAt = intf[SCAN_FRAME_COUNT / 4];
			timeAt[0] = timeAt[1] = timeAt[2] = timeAt[3] = 0;
			changes = 0;
			said = now;
		}
		IOSleep(hz == 120 ? 50 : 10);
	}
}

static void
panelTestMain(void * arg, __unused wait_result_t wr)
{
	volatile uint32_t * intf = (volatile uint32_t *) arg;
	static const struct {
		const char *    name;
		const uint8_t * commands;
	} timings[] = {
		{ "120", gPanel120 }, { "90", gPanel90 }, { "60", gPanel60 }, { "40", gPanel40 }, { "30", gPanel30 },
		{ "idle 60 to 1", gPanelIdle1 }, { "test 120 to 0.5", gPanelHalf }, { "auto 120 to 60", gPanelAuto120 },
	};

	IOSleep(gPanelTest * 1000);
	IOLog("MiniIOMobileFramebuffer: fb_panel_test: %u timings, 3 s each\n", (unsigned) (sizeof(timings) / sizeof(timings[0])));
	for (unsigned i = 0; i < sizeof(timings) / sizeof(timings[0]); i++) {
		int sent = panelSend(intf, timings[i].commands);
		uint32_t first, last;

		IOSleep(50);
		first = panelMeasure(intf, 950);
		last = panelMeasure(intf, 2000);
		IOLog("MiniIOMobileFramebuffer: fb_panel_test: %s: %d commands; %u.%u frames a second the first second, %u.%u the last two\n",
		    timings[i].name, sent, first / 10, first % 10, last / 10, last % 10);
	}
	panelSend(intf, gPanelHz == 60 ? gPanel60 : gPanel120);
	IOLog("MiniIOMobileFramebuffer: fb_panel_test: done, back to %u\n", gPanelHz == 60 ? 60 : 120);
}

/*
 * The two clocks it takes to read the interface are turned on (the firmware leaves the panel
 * running and them off; nothing else is written), and what one line and one frame are is read.
 * False where that cannot be: not shennong, not asked for, the block without power or clock, or
 * a panel that the interface does not send frames to by itself (a command mode one).
 */
static bool
scanStart(uint32_t panelHeight, const uint8_t * framebuffer)
{
	const char * phone = redwood_phone(), * want = "shennong";
	volatile uint32_t * gcc, * dispcc, * intf, * intf2;
	uint32_t line, frame;

	while (*phone != 0 && *phone == *want) {
		phone++;
		want++;
	}
	if (gSync == 0) {
		gScanWhy = "fb_sync=0";
		return false;
	}
	if (*phone != 0 || *want != 0) {
		gScanWhy = "not shennong";
		return false;
	}
	gcc = (volatile uint32_t *) ml_io_map(SCAN_GCC_DISP_AHB & ~0xfffu, 0x1000);
	dispcc = (volatile uint32_t *) ml_io_map(SCAN_DISPCC, SCAN_DISPCC_SIZE);
	intf = (volatile uint32_t *) ml_io_map(SCAN_INTF, 0x1000);
	intf2 = (volatile uint32_t *) ml_io_map(SCAN_INTF2, 0x1000);
	if (gcc == NULL || dispcc == NULL || intf == NULL || intf2 == NULL) {
		gScanWhy = "the registers could not be mapped";
		return false;
	}
	gcc += (SCAN_GCC_DISP_AHB & 0xfffu) / 4;
	if (!(*gcc & 1)) {
		*gcc |= 1;
		IODelay(10000);
	}
	gScanSeen[0] = *gcc;
	if (!(*gcc & 1)) {
		gScanWhy = "the display's clock controller has no bus clock (gcc 0x127004)";
		return false;
	}
	gScanSeen[1] = dispcc[SCAN_MDSS_GDSCR / 4];
	if (!(gScanSeen[1] & 1u << 31)) {
		gScanWhy = "the display block has no power (dispcc 0x9000)";
		return false;
	}
	gScanSeen[2] = dispcc[SCAN_MDSS_AHB_CBCR / 4];
	if (gScanSeen[2] & 1u << 31) {
		dispcc[SCAN_MDSS_AHB_CBCR / 4] = gScanSeen[2] | 1;
		IODelay(10000);
		gScanSeen[2] = dispcc[SCAN_MDSS_AHB_CBCR / 4];
	}
	if (gScanSeen[2] & 1u << 31) {
		gScanWhy = "the bus clock of the display block's registers does not run (dispcc 0x80a4)";
		return false;
	}
	for (unsigned i = 0; i < 0x2c0 / 4; i++) {
		gScanRegs[0][i] = intf[i];
		gScanRegs[1][i] = intf2[i];
	}
	for (unsigned i = 0; i < SCAN_SAMPLES; i++) {
		gScanSamples[i][0] = (uint32_t) mach_absolute_time();
		gScanSamples[i][1] = intf[SCAN_FRAME_COUNT / 4];
		gScanSamples[i][2] = intf[SCAN_LINE_COUNT / 4];
		gScanSamples[i][3] = intf[SCAN_TEAR_INT_COUNT_VAL / 4];
		gScanSamples[i][4] = intf[SCAN_TEAR_LINE_COUNT / 4];
		IODelay(1000);
	}
	gScanRead = true;
	if (gProbe != 0) {
		probeDisplay(framebuffer);
	}
	if (!(intf[SCAN_TIMING_ENGINE_EN / 4] & 1) && (intf[SCAN_TEAR_AUTOREFRESH / 4] & 1u << 31)) {
		uint64_t nanos = 2200;                  /* (seen: 2.2 us a line) */

		/* how fast the lines went, where the samples saw them going */
		for (unsigned i = 1; i < SCAN_SAMPLES; i++) {
			uint32_t lines = gScanSamples[i][2] - gScanSamples[i - 1][2];
			uint64_t took;

			if (gScanSamples[i][1] == gScanSamples[i - 1][1] && gScanSamples[i - 1][2] > 1 && lines > 100) {
				absolutetime_to_nanoseconds((uint64_t) (gScanSamples[i][0] - gScanSamples[i - 1][0]), &took);
				nanos = took / lines;
				break;
			}
		}
		if (nanos < 500 || nanos > 6000) {
			nanos = 2200;
		}
		gScanCommand = true;
		gScanLines = panelHeight;
		gScanFirst = 1;
		nanoseconds_to_absolutetime(nanos, &gScanLine);
		gScanSeen[0] = intf[SCAN_LINE_COUNT / 4];
		gScanSeen[1] = (uint32_t) nanos;
		gScanSeen[2] = intf[SCAN_FRAME_COUNT / 4];
		nanoseconds_to_absolutetime(NSEC_PER_SEC / 60, &gScanPeriod);
		if (gPanelHz != 0) {
			panelRate(intf);
		}
		if (gDyn != 0 && gPanelTest == 0) {
			thread_t dyn;

			if (kernel_thread_start(dynMain, (void *) intf, &dyn) == KERN_SUCCESS) {
				thread_deallocate(dyn);
			}
		}
		if (gPanelTest != 0) {
			thread_t tester;

			if (kernel_thread_start(panelTestMain, (void *) intf, &tester) == KERN_SUCCESS) {
				thread_deallocate(tester);
			}
		}
		gScanWhy = "on";
		gScan = intf;
		return true;
	}
	if (!(intf[SCAN_TIMING_ENGINE_EN / 4] & 1)) {
		gScanWhy = "the interface sends the panel nothing by itself (no timing engine, no autorefresh)";
		return false;
	}
	line = intf[SCAN_HSYNC_CTL / 4] >> 16;
	frame = intf[SCAN_VSYNC_PERIOD / 4];
	gScanSeen[0] = line;
	gScanSeen[1] = frame;
	gScanSeen[2] = panelHeight;
	if (line == 0 || frame / line < panelHeight || frame / line > 2 * panelHeight + 1000) {
		gScanWhy = "the interface's timing is not the panel's (clocks a line, a frame; rows)";
		return false;
	}
	gScanLines = frame / line;
	gScanFirst = intf[SCAN_DISPLAY_V_START / 4] / line;
	/* (the refresh rate is not in these registers: 120 a second assumed) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / 120 / gScanLines, &gScanLine);
	gScanSeen[0] = intf[SCAN_LINE_COUNT / 4];
	IODelay(1000);
	gScanSeen[1] = intf[SCAN_LINE_COUNT / 4];
	gScanSeen[2] = intf[SCAN_FRAME_COUNT / 4];
	gScanWhy = "on";
	gScan = intf;
	return true;
}

/* Once: what the two interfaces were found doing (the registers that are not 0, and the counters). */
static void
scanReport(void)
{
	if (!gScanRead) {
		return;
	}
	gScanRead = false;
	for (unsigned n = 0; n < 2; n++) {
		char text[600];
		size_t used = 0;

		text[0] = 0;
		for (unsigned i = 0; i < 0x2c0 / 4 && used + 24 < sizeof(text); i++) {
			if (gScanRegs[n][i] != 0) {
				used += snprintf(text + used, sizeof(text) - used, " %x:%x", i * 4, gScanRegs[n][i]);
			}
		}
		IOLog("MiniIOMobileFramebuffer: fb_sync: intf%u's registers:%s\n", n + 1, text);
	}
	for (unsigned i = 0; i < SCAN_SAMPLES; i++) {
		uint64_t nanos;

		absolutetime_to_nanoseconds((uint64_t) (gScanSamples[i][0] - gScanSamples[0][0]), &nanos);
		IOLog("MiniIOMobileFramebuffer: fb_sync: at %llu us: frame %u line %u; the panel's read pointer %#x, written "
		    "line %#x\n", nanos / 1000, gScanSamples[i][1], gScanSamples[i][2], gScanSamples[i][3], gScanSamples[i][4]);
	}
}

/* The row of the panel the refresh is at (less than 0 or past the last: between two), and which refresh. */
static inline int32_t
scanAt(uint32_t * refresh)
{
	uint32_t frame, line;

	do {
		frame = gScan[SCAN_FRAME_COUNT / 4];
		line = gScan[SCAN_LINE_COUNT / 4];
	} while (gScan[SCAN_FRAME_COUNT / 4] != frame);
	*refresh = frame;
	return (int32_t) line - (int32_t) gScanFirst;
}

/* Until the refresh `refresh` has gone past row `row` (or is over). */
static void
scanBehind(uint32_t refresh, int32_t row)
{
	for (;;) {
		uint32_t now;
		int32_t at = scanAt(&now);

		if (now != refresh || at >= row) {
			return;
		}
		clock_delay_until(mach_absolute_time() + gScanLine * (uint64_t) (row - at));
	}
}

/* A share from row `row` down is in: had the refresh after `refresh` come to it already? (counted) */
static void
scanCheck(uint32_t refresh, int32_t row, unsigned copier, uint64_t waited, uint64_t took)
{
	uint32_t now;
	int32_t at = scanAt(&now);

	OSIncrementAtomic(&gScanShares);
	/* (at the next refresh's line 0 is between the two, its row 0 not sent yet) */
	if (now != refresh && !(now == refresh + 1 && (at < row || at == 0))) {
		SInt32 n = OSIncrementAtomic(&gScanTorn);

		if (n < SCAN_TORN_KEPT) {
			ScanTorn * t = &gScanTornKept[n];

			t->row = row;
			t->copier = copier;
			t->ahead = now - refresh;
			t->at = at;
			t->chosenAt = gScanChosenAt;
			t->since = mach_absolute_time() - gScanChosenTime;
			t->waited = waited;
			t->took = took;
		}
	}
}

/*
 * The refresh a frame is to go in behind: the one under way, unless it is so far down that the
 * rows above would not be in before the next comes round to them: then the next.
 */
static uint32_t
scanChoose(int32_t rows)
{
	for (;;) {
		uint32_t refresh;
		int32_t at = scanAt(&refresh);

		if (gScanCommand) {
			uint64_t margin;

			/*
			 * The rows it has sent already are copied at once, the top ones first, and the first
			 * piece has to be in before the next refresh starts: a refresh period after this one
			 * did. Otherwise the frame goes in behind the next one (the frame counter goes up
			 * when this one is sent).
			 */
			nanoseconds_to_absolutetime(500 * NSEC_PER_USEC, &margin);
			if (at <= 0 || gScanLine * (uint64_t) at + gShareTime + margin <= gScanPeriod) {
				gScanChosenAt = at;
				gScanChosenTime = mach_absolute_time();
				return refresh;
			}
			for (uint32_t now = refresh; now == refresh;) {
				clock_delay_until(mach_absolute_time() + gScanLine * 64);
				scanAt(&now);
			}
			continue;
		}
		if (at <= rows - rows / 4) {
			return refresh;
		}
		clock_delay_until(mach_absolute_time() + gScanLine * (uint64_t) (rows + 64 - at));
	}
}

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	bool            fScanTried;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	MiniIOSurface * fEarly[SWAP_LAYERS];    /* fb_gpu: surfaces of the frame being shown let go of before its copy */
	unsigned        fEarlyCount;
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	void drawShare(const struct RowJob * job, unsigned index, unsigned of, uint32_t * blendRow);
	volatile SInt32 fNextPiece;             /* drawShare(): the piece of rows the next copier free takes */
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	/*
	 * CoreAnimation's beat: it draws a frame at most every displayMinRefreshInterval, in
	 * seconds times 2^32 (QuartzCore of 20F66, the display's heartbeat), and without this
	 * property every 1/60 s, whatever the timing above says (the internal panel's mode carries
	 * no rate for it). At fb_hz=120 so 120 frames a second instead of 60.
	 */
	OSDictionary * refresh = OSDictionary::withCapacity(1);
	if (refresh != NULL) {
		setNumber(refresh, "displayMinRefreshInterval", (1ULL << 32) / refreshHz());
		setProperty("IOMFBDisplayRefresh", refresh);
		refresh->release();
	}
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	copierRealtime(thread);
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		copierRealtime(thread);
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBHome != 0) {
		IORegistryEntry * product = IORegistryEntry::fromPath("/product", gIODTPlane);
		uint32_t home = gFBHome;

		if (product != NULL) {
			product->setProperty("home-button-type", &home, sizeof(home));
			product->release();
		}
	}
	for (unsigned i = 0; i < sizeof(gFBPhones) / sizeof(gFBPhones[0]); i++) {
		const struct fb_phone * phone = &gFBPhones[i];

		if (gFBPhone != phone->subtype) {
			continue;
		}
		gPhoneWidth = phone->width;
		gPointsWide = phone->width / kFBDefaultScale;
		gPointsHigh = phone->height / kFBDefaultScale;
		gDotsPerInch = phone->dpi;
		/* unless fbwidth= or fbheight= asked for a size of their own */
		if (fWidth == kFBDefaultWidth && fHeight == kFBDefaultHeight) {
			fWidth = phone->width;
			fHeight = phone->height;
		}
		if (gFBIsland != 0) {
			IORegistryEntry * product = IORegistryEntry::fromPath("/product", gIODTPlane);
			uint32_t subtype = phone->subtype;

			if (product != NULL) {
				product->setProperty("artwork-device-subtype", &subtype, sizeof(subtype));
				product->release();
			}
		}
		IOLog("MiniIOMobileFramebuffer: the %s's screen (fb_phone=%u%s)\n", phone->name, phone->subtype,
		    gFBIsland != 0 ? ", its artwork subtype" : ", the iPhone X's artwork subtype");
	}
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = gPointsWide * scale;
		fHeight = gPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	if ((gProbe & ~17u) != 0) {
		/*
		 * (here, while the kernel's log is quiet: the loader's look at its last line shows the read
		 * that stopped it; but what is said here is gone from the log by its end, so the reads known
		 * to be harmless, 1 and 16, are left for the first frame)
		 */
		fScanTried = true;
		scanStart(fPanelHeight, fBase);
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("RedwoodWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	MiniIOSurfaceForProcessor(s);
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	bool            filter, unscaled;
	bool            sync;                   /* fb_sync: each share behind refresh `refresh` */
	uint32_t        refresh;
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
}

/*
 * Copier `index` of `of`'s rows. Without fb_sync a band of its own. With it, pieces of SCAN_PIECE
 * rows, the next one down whenever a copier is free (fNextPiece), each once the refresh has sent
 * it: the copiers go down the panel right behind the refresh, all of them, and a row is in some
 * 8 ms before the next refresh comes to it. In bands of a quarter each (until 28 October) the
 * last band could only start once the refresh was done with it, and had the 1.2 ms before the
 * next one starts and the 5 ms it takes that to come to the band: a band that took longer, put
 * off at 120 a second, was torn. And with every fourth piece each, one copier of the four was
 * now and then ten times slower than the others for a whole frame (1 to 2 ms a piece, not 0.15:
 * on a little core, or one shared), fell behind the refresh and its pieces were torn: a piece is
 * now taken by whoever is free, and a slow copier holds up one piece, not a quarter of them.
 */
#define SCAN_PIECE      64

void
IOMobileFramebuffer::drawShare(const RowJob * job, unsigned index, unsigned of, uint32_t * blendRow)
{
	int32_t rows = job->py1 - job->py0;

	if (!job->sync) {
		drawRows(job, job->py0 + (int32_t) ((int64_t) rows * index / of),
		    job->py0 + (int32_t) ((int64_t) rows * (index + 1) / of), blendRow);
		return;
	}
	for (;;) {
		int32_t y0 = job->py0 + SCAN_PIECE * (int32_t) OSIncrementAtomic(&fNextPiece), y1;
		uint64_t asked = mach_absolute_time(), began;

		if (y0 >= job->py1) {
			break;
		}
		y1 = y0 + SCAN_PIECE < job->py1 ? y0 + SCAN_PIECE : job->py1;
		scanBehind(job->refresh, y1);
		began = mach_absolute_time();
		drawRows(job, y0, y1, blendRow);
		shareTook(began);
		scanCheck(job->refresh, y0, index, began - asked, mach_absolute_time() - began);
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		fb->drawShare(job, index + 1, fb->fThreads, fb->fHelperBlend[index]);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	dynNoteFrame();         /* (fb_dyn: something new on the screen) */
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	/* (fb_gpu=1: a whole frame onto the whole panel scaled by the GPU into a copy of the panel's size, RedwoodGPU.cpp;
	 * that copy then goes in row for row as a frame of the panel's own size does) */
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	{
		const uint8_t * scaled = NULL;
		uint32_t scaledRow = 0;

		const bool whole = sx == 0 && sy == 0 && (uint32_t) sw == s->info.width && (uint32_t) sh == s->info.height && px0 == 0 && py0 == 0 &&
		    (uint32_t) px1 == fPanelWidth && (uint32_t) py1 == fPanelHeight && (px1 != sw || py1 != sh);

		if (!whole) {
			const int32_t last[8] = { sx, sy, sw, sh, px0, py0, px1, py1 };

			gPartLayers++;
			memcpy(gPartLast, last, sizeof(gPartLast));
		}
		const bool dpu = gDpuScale != 0 && ((s->info.width == 1290 && s->info.height == 2796) || (s->info.width == 1125 && s->info.height == 2436) ||
		    (s->info.width == 860 && s->info.height == 1864));
		if (whole && gFlip && flipSetUp(fBase, fRowBytes, fPanelHeight) && gFlipState > 0 &&
		    RedwoodGPUPanelScale(s, dpu ? s->info.width : fPanelWidth, dpu ? s->info.height : fPanelHeight, &scaled, &scaledRow,
		    gFlipGpu[gFlipFront ^ 1])) {
			if (dpu) {
				dpuScaleSetUp(s->info.width, s->info.height, scaledRow, fPanelWidth, fPanelHeight);
			}
			if (fEarlyCount < SWAP_LAYERS) {
				fEarly[fEarlyCount++] = s;
				OSDecrementAtomic(&s->displayed);
			}
			if (flipTo(gFlipFront ^ 1)) {
				return;         /* (shown: nothing for the processor to copy) */
			}
			/* (not taken: back to the copy into the first framebuffer, which the pipes are told of again) */
			gFlipState = -1;
			if (gFlipFront != 0) {
				(void) flipTo(0);
			}
			dpuScaleOff(fRowBytes, fPanelWidth, fPanelHeight);
			gFlipFront = 0;
			gFlipMdp[FLIP_VIG0_ADDR / 4] = (uint32_t) gFlipPhys[0];
			gFlipMdp[FLIP_VIG1_ADDR / 4] = (uint32_t) gFlipPhys[0];
			gFlipMdp[FLIP_CTL0_FLUSH / 4] = FLIP_PIPES;
		}
		if (whole && (!gFlip || gFlipState < 0) && RedwoodGPUPanelScale(s, fPanelWidth, fPanelHeight, &scaled, &scaledRow)) {
			/* (the surface is read: its frame is in the panel-sized copy now, so whoever draws into it next need not
			 * wait for the copy onto the panel, a whole refresh -- RedwoodGPU's submit waits while it is displayed) */
			if (fEarlyCount < SWAP_LAYERS) {
				fEarly[fEarlyCount++] = s;
				OSDecrementAtomic(&s->displayed);
			}
			job.pixels = scaled;
			job.stride = scaledRow;
			sw = (int32_t) fPanelWidth;
			sh = (int32_t) fPanelHeight;
			cx0 = 0;
			cx1 = (int32_t) fPanelWidth;
		} else {
			MiniIOSurfaceForProcessor(s);   /* (read by the processor after all: what the GPU drew, not the cache's) */
		}
	}
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	job.sync = gScan != NULL;
	job.refresh = job.sync ? scanChoose((int32_t) fPanelHeight) : 0;
	fNextPiece = 0;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawShare(&job, 0, 1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawShare(&job, 0, fThreads, fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		redwood_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
		if (fSwapSurfaces[slot][layer] != NULL) {
			OSIncrementAtomic(&fSwapSurfaces[slot][layer]->displayed);   /* (until it is on the panel: RedwoodGPU's submit waits) */
		}
	}
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		fEarlyCount = 0;
		show(swap, surfaces);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				bool early = false;

				for (unsigned e = 0; e < fEarlyCount; e++) {
					if (fEarly[e] == surfaces[layer]) {
						fEarly[e] = NULL;       /* (once: the same surface twice in a swap is let go twice) */
						early = true;
						break;
					}
				}
				if (!early) {
					OSDecrementAtomic(&surfaces[layer]->displayed);
				}
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	if (!fScanTried) {
		/* (here, not in start(): what it logs there is gone from the kernel's log before anyone reads it) */
		fScanTried = true;
		scanStart(fPanelHeight, fBase);
	}
	began = ready = mach_absolute_time();
	if (gOnTheBeat && gScan == NULL) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	/* its turn has come: the swap after it may be handed over */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			drawLayer(s, src, dst);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	redwood_overlay_redraw();               /* (a few lines kept over the picture, if any: see above) */
	drawn = mach_absolute_time();
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		redwood_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		{
			/* fb_probe's reads made in start() (2, 4, 8) are gone from the kernel's log by its end: said again with
			 * the first three of these lines, when somebody is reading */
			static unsigned probeSaid;

			if (gProbeUsed != 0 && probeSaid < 3) {
				probeSaid++;
				probePrint();
			}
		}
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		fStatSwapsLate = 0;
		{
			char cpus[128];

			redwood_cpufreq_now(cpus, sizeof(cpus));
			if (cpus[0] != 0) {
				IOLog("MiniIOMobileFramebuffer: cpufreq now (MHz, as held by the limits management / as asked): %s\n", cpus);
			}
		}
		if (gFlip) {
			IOLog("MiniIOMobileFramebuffer: fb_flip: %s, %u flips, %u not taken, %llu us a flip waiting for the panel's next frame; "
			    "the display's scalers %s (%ux%u, VIG0 scaler op mode %#x)\n",
			    gFlipState > 0 ? "on" : gFlipState < 0 ? "OFF" : "not tried", gFlipFlips, gFlipSlow,
			    gFlipFlips ? gFlipWait / 1000 / gFlipFlips : 0, gDpuScaled[0] != 0 ? "SCALE" : gDpuScale ? "asked for but not in use" : "off",
			    gDpuScaled[0], gDpuScaled[1], gFlipMdp != NULL ? gFlipMdp[(0x5000u + 0xa04u) / 4] : 0);
			gFlipFlips = 0;
			gFlipWait = 0;
		}
		IOLog("MiniIOMobileFramebuffer: fb_gpu: %s; layers not a whole frame onto the whole panel: %u (the last %d,%d %dx%d onto %d,%d-%d,%d)\n",
		    RedwoodGPUPanelState(), gPartLayers, gPartLast[0], gPartLast[1], gPartLast[2], gPartLast[3], gPartLast[4], gPartLast[5],
		    gPartLast[6], gPartLast[7]);
		scanReport();
		if (gScan != NULL) {
			uint64_t share, period;

			absolutetime_to_nanoseconds(gShareTime, &share);
			absolutetime_to_nanoseconds(gScanPeriod, &period);
			IOLog("MiniIOMobileFramebuffer: fb_sync: frames behind the %s (%u lines a frame, the first row at line %u; "
			    "at the start: %u, %u, frame %u); %d of %d shares were still going in when the next one came to them; "
			    "a piece of %u rows takes %llu us at most lately, the panel's frame %llu us\n",
			    gScanCommand ? "sending to the command mode panel (ns a line)" : "refresh (line, a ms later)", gScanLines,
			    gScanFirst, gScanSeen[0], gScanSeen[1], gScanSeen[2], (int) gScanTorn, (int) gScanShares,
			    SCAN_PIECE, share / NSEC_PER_USEC, period / NSEC_PER_USEC);
			for (int i = 0; i < gScanTorn && i < SCAN_TORN_KEPT; i++) {
				const ScanTorn * t = &gScanTornKept[i];
				uint64_t since, waited, took;

				absolutetime_to_nanoseconds(t->since, &since);
				absolutetime_to_nanoseconds(t->waited, &waited);
				absolutetime_to_nanoseconds(t->took, &took);
				IOLog("MiniIOMobileFramebuffer: fb_sync: torn: row %d by copier %u, the frame behind the refresh at row %d, "
				    "in %llu us later (waited %llu, copied in %llu), by then %u refreshes on at row %d\n", t->row, t->copier,
				    t->chosenAt, since / NSEC_PER_USEC, waited / NSEC_PER_USEC, took / NSEC_PER_USEC, t->ahead, t->at);
			}
			gScanTorn = gScanShares = 0;
		} else {
			IOLog("MiniIOMobileFramebuffer: fb_sync off: %s (read %#x %#x %#x)\n", gScanWhy, gScanSeen[0], gScanSeen[1],
			    gScanSeen[2]);
		}
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	/*
	 * ...and on a command mode panel whose lines can be read (fb_sync), at the panel's own frames: its period as
	 * measured (119.9 a second, not the timer's 120), and in phase with the start of its sending, where a flip
	 * (fb_flip) is taken. A timer of its own drifted against the panel, and every so many frames CoreAnimation's
	 * frame came just after a flip could be taken and waited a refresh: the steps in a moving picture (13 November).
	 * Re-phased only while the panel is sending (its line counter past 50): between frames the counter rests at 1.
	 */
	if (gScan != NULL && gScanCommand && gScanPeriod != 0 && gVSyncPanel) {
		uint32_t frame;
		int32_t line = scanAt(&frame);

		period = gScanPeriod;
		if (line > 50) {
			uint64_t start = now - (uint64_t) line * gScanLine, next = start + period;

			while (next <= now + period / 4) {
				next += period;
			}
			fVSyncTime = next;
			thread_call_enter_delayed(fVSyncCall, fVSyncTime);
			return;
		}
	}
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = gDotsPerInch * fOwner->fWidth / gPhoneWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* a swap is on the screen as soon as it is made */
		if (gTouchLat) {
			redwood_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 *
 * With boot-arg metal=1 a user process may put those two properties here, which is the whole
 * of what keeps Metal from finding our driver; see setProperties below, and docs/metal.md 10.31
 * (10.14 for the measurement that made it a boot-arg).
 */

/* Metal's step 2 reads exactly these two, in this order, off this service (docs/metal.md 1). */
#define kAcceleratorPluginNameKey      "MetalPluginName"
#define kAcceleratorPluginClassNameKey "MetalPluginClassName"

static bool gAcceleratorTakesProperties;        /* boot-arg metal=1 */

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * Switch Metal's own device discovery on, from user space: IORegistryEntrySetCFProperties() with
 * MetalPluginName and MetalPluginClassName. Metal reads the first (getMetalPluginClassForService,
 * 0x181a39ba4), builds "/System/Library/Extensions/" + it + ".bundle", opens that with NSBundle
 * and takes the class the second names; with the first missing it returns nil before it even
 * reads the second, and that single `cbz` is where the chain breaks on this port today.
 *
 * This method is the only thing that was in the way. IORegistryEntry::setProperties is
 * "return kIOReturnUnsupported" (IORegistryEntry.cpp) and IOService does not override it; the
 * MAC check in front of the call has no policy to ask, because this kernel registers no iokit
 * hook (bsd/kern/kern_minimac.c); and this service carries no property whitelist.
 *
 * Why it is gated on a boot-arg, which is a measured risk and not caution in general
 * (docs/metal.md 10.14): the conditions Metal puts on a device object are asserts, not soft
 * refusals (10.2-10.3). A process that asks Metal and is handed a half-made device does not
 * degrade to another renderer -- it aborts ("Assertion failed: ([self
 * conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion _limits.maxColorAttachments
 * > 0"), and an aborting daemon is restarted by launchd, which is a loop. Published instead at
 * registerService() time, the properties would exist before launchd, backboardd and SpringBoard
 * are born, so each of the three would take the Metal road on its first call, with nobody left
 * to read the log. Behind a boot-arg, a machine that has them is a machine booted on purpose to
 * have them, and the first such boot is meant to be RC=rc.ios, where launchd is never born.
 *
 * An empty string removes a property rather than setting it. That is the way out of the loop
 * above without a reboot: the properties live only in the registry, so processes started after
 * they are gone go back to the software road.
 *
 * Everything else is refused, and refused whole, before anything is set: every process on the
 * machine can reach this method and the only thing it exists for is this one switch.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (!gAcceleratorTakesProperties) {
		static unsigned said;

		if (said++ < 4) {
			proc_selfname(who, sizeof(who));
			IOLog("IOAcceleratorES: %s asks to set properties on it; boot-arg metal=1 was not given\n", who);
		}
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);

		if (key == NULL || (!key->isEqualTo(kAcceleratorPluginNameKey) &&
		    !key->isEqualTo(kAcceleratorPluginClassNameKey))) {
			IOLog("IOAcceleratorES: refusing what %s sent: only " kAcceleratorPluginNameKey " and "
			    kAcceleratorPluginClassNameKey " are taken here\n", who);
			keys->release();
			return kIOReturnUnsupported;
		}
		if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			IOLog("IOAcceleratorES: refusing \"%s\" from %s: Metal reads it as a CFString\n",
			    key->getCStringNoCopy(), who);
			keys->release();
			return kIOReturnBadArgument;
		}
	}
	keys->reset();
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSString * value = OSDynamicCast(OSString, dict->getObject(key));

		if (value->getLength() == 0) {
			removeProperty(key);
			IOLog("IOAcceleratorES: %s removed by %s\n", key->getCStringNoCopy(), who);
		} else if (setProperty(key, value)) {
			IOLog("IOAcceleratorES: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
		} else {
			IOLog("IOAcceleratorES: %s could not be set\n", key->getCStringNoCopy());
			keys->release();
			return kIOReturnNoMemory;
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
	uint32_t metal = 0;
	uint32_t plugin = 0;

	gAcceleratorTakesProperties = PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0;
	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		/*
		 * Boot-arg metal_plugin=1: the service names the Metal plug-in itself, before it is registered, as
		 * hakios's MiniIOMobileFramebuffer does under its metal=1. A user process (metal_props) can only put
		 * the two properties here once launchd runs, and the window server may have asked Metal for its
		 * device by then and taken OpenGL ES for good. The plug-in is hakios-metal's (HakiosMetalCA.m, the
		 * Adreno 750's driver), installed by the userland build as /redwood/lib/RedwoodMetal.bundle, which
		 * /System/Library/Extensions/RedwoodMetal.bundle already points at; its class HakiosMetalDevice.
		 */
		if (PE_parse_boot_argn("metal_plugin", &plugin, sizeof(plugin)) && plugin != 0) {
			accelerator->setProperty(kAcceleratorPluginNameKey, "RedwoodMetal");
			accelerator->setProperty(kAcceleratorPluginClassNameKey, "HakiosMetalDevice");
			IOLog("IOAcceleratorES: metal_plugin=1: the accelerator names RedwoodMetal / HakiosMetalDevice as its Metal plug-in\n");
		}
		accelerator->registerService();
		/* the first line to look for in the log of such a boot: the gate is open, nothing is set yet */
		if (gAcceleratorTakesProperties) {
			IOLog("IOAcceleratorES: metal=%u: this service takes " kAcceleratorPluginNameKey
			    " and " kAcceleratorPluginClassNameKey " from user space\n", metal);
		}
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
