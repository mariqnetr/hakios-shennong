/*
 * RedwoodGPU: the phone's Adreno for programs to draw with.
 *
 * bsd/dev/redwood_adreno.c wakes the GPU and, with boot-arg adreno_keep=1, keeps it on and
 * starts this service. A program that opens it speaks the interface of Linux's msm DRM driver,
 * the one Mesa's freedreno knows: every request of that interface is a call of the user client
 * with the request's number as the selector and the request's structure passed in and back
 * (userland: tools/mesa/redwood-drm, which stands in for libdrm inside Mesa). What Linux does
 * with mmap() on the device is the client memory of the type that is the buffer's handle.
 *
 * What there is of the interface is what freedreno uses to draw: parameters, buffers (made,
 * told their address in the GPU's address space, mapped, given back), submission of command
 * buffers, and waiting for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context; it is zero when made, and write-combined: what the program writes goes to
 * memory, where the GPU reads it, and not into this processor's cache (as Linux maps them for
 * freedreno; boot-arg gpu_cached=1 makes them ordinary memory, pushed out of the cache whenever
 * the GPU is given something to do: see submit()).
 *
 * One request is not Linux's: a buffer that is the memory of an IOSurface (surfaceBuffer()), for
 * drawing into what the display shows and for reading pictures where other programs have drawn
 * them. Linux hands such memory around as file descriptors; here it has a name already. That
 * memory is ordinary, cached memory, which everybody else reads and writes as such.
 *
 * Not here yet: interrupts (waiting is asking again five times a millisecond), more than one
 * program's worth of care (every client's buffers are in the one address space; a client that
 * goes takes its own away), fences and buffers as file descriptors. A GPU that has stopped
 * answering is reported (redwood_gpu_report()) and, with boot-arg gpu_recover=1, reset and
 * started again, so that the submission that hung is all that is lost: what that does and
 * which source says so is at redwood_gpu_recover() in bsd/dev/redwood_adreno.c. Without the
 * boot-arg it stays stopped and nothing is waited for again.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

#include "MiniIOSurface.h"

extern "C" void redwood_tl(const char * format, ...) __printflike(1, 2);

extern "C" {
/* bsd/dev/redwood_adreno.c ("the GPU kept on"; declared in bsd/sys/redwood_input.h, which is not seen from here) */
bool redwood_gpu_ready(void);
void shennong_gpu_dcvs(void);
bool redwood_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t redwood_gpu_address(uint64_t size);
void redwood_gpu_address_free(uint64_t address, uint64_t size);
void redwood_gpu_space(uint64_t * used, uint32_t * live, uint64_t * largest, unsigned * holes, unsigned * most,
    uint64_t * lost, uint32_t * refused);
extern uint32_t gpu_va;                 /* boot-arg gpu_va: 0 as before, 1 describe the addresses, 2 also guard them */
bool redwood_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void redwood_gpu_mapped(void);
void redwood_gpu_unmap(uint64_t address, uint64_t size);
void redwood_gpu_to_memory(void * start, uint64_t size);
uint32_t redwood_gpu_submit(const uint64_t * addresses, const uint32_t * words, uint32_t count);
extern bool shennong_gpu_plain;
extern uint32_t gpu_log;
bool redwood_gpu_done(uint32_t fence);
uint32_t redwood_gpu_last(void);
void redwood_gpu_worked(uint64_t * microseconds, uint32_t * submissions, uint32_t by[7]);
void redwood_gpu_report(const char * why);
bool redwood_gpu_recover(void);
void redwood_gpu_stop(void);

/* osfmk/vm (vm_page.h, vm_map.h and zalloc_internal.h are not seen from here): for a buffer there is no memory for */
extern unsigned int vm_page_free_count, vm_page_free_target, vm_page_free_reserved, vm_page_wire_count,
    vm_page_wire_count_initial, vm_page_active_count, vm_page_inactive_count, vm_page_speculative_count,
    vm_page_throttled_count, vm_page_purgeable_count;
extern uint32_t c_segment_pages_compressed;
extern vm_allocation_site_t * vm_allocation_sites[VM_MAX_TAG_VALUE];
uint64_t vm_tag_get_size(vm_tag_t tag);
const char * kern_allocation_get_name(kern_allocation_name_t allocation);
void vm_map_sizes(vm_map_t map, vm_map_size_t * psize, vm_map_size_t * pfree, vm_map_size_t * plargest_free);
void zone_map_sizes(vm_map_size_t * psize, vm_map_size_t * pfree, vm_map_size_t * plargest_free);
kern_return_t vm_map_region(vm_map_t map, vm_map_offset_t * address, vm_map_size_t * size, vm_region_flavor_t flavor,
    vm_region_info_t info, mach_msg_type_number_t * count, mach_port_t * object_name);

void RedwoodGPUStart(void);
void RedwoodGPUStop(void);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kRedwoodGPUWorked       = 0x7d,         /* ours: how long the 3D core itself has worked, and on how many */
	kRedwoodSurface         = 0x7e,         /* ours: an IOSurface's memory as a buffer */
	kRedwoodShow            = 0x7f,         /* ours: pixels of the caller's onto the panel, to see a test's result */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct redwood_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kRedwoodDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
struct redwood_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};
/*
 * kRedwoodGPUWorked: the GPU's own side of a measurement a program makes with its own clock.
 *
 * A program timing this driver (userland/src/metal_drv_test.m --time) can measure only wall
 * time: the whole of a draw, which is its own work, this ioctl, the submission, the core's
 * execution and the wait. Nothing it can reach says how much of that was the core -- and the
 * kernel has known all along, because bsd/dev/redwood_adreno.c times every submission off
 * CP_ALWAYS_ON_COUNTER (19.2 ticks a microsecond) and until now only ever printed it. This
 * request hands the same three numbers to the program, so the subtraction happens where the
 * two halves are both in hand instead of in whoever reads the log afterwards.
 *
 * It RESETS what it reports, which is redwood_gpu_worked()'s own contract ("since this was
 * last asked") and is what makes it useful: a caller reads once to zero the counters, does the
 * work it wants to price, and reads again. Two consequences, and they are the whole of the
 * sharp edge: two programs asking at once each get a part of the truth, and the kernel's own
 * five-second report in submit() below shares the same counters -- so a boot that reads this
 * will see gaps in those lines, and that is expected rather than a fault.
 */
struct redwood_worked {
	uint64_t microseconds;                  /* the core's own time on the submissions counted here */
	uint32_t submissions;                   /* how many of them it timed; 0 means nothing finished since */
	uint32_t by[7];                         /* ... of which under 0.1, 0.25, 0.5, 1, 2, 4 ms, and longer */
};

#define kRedwoodDescribe        0xffffffffu

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_SET_IOVA       4
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             8192            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_SUBMIT_BOS          8192
#define GPU_PATIENCE_MS         5000            /* for a buffer or a submission that somebody waits for without a limit */
#define GPU_ASK_AGAIN_US        200             /* while waiting for the core: how long between two looks at how far it is */
#define GPU_PUSH_AGAIN_MS       4               /* a surface is pushed out of the cache again for a submission this long after the last */

static IOLock * gGPULock;                       /* around everything that is the GPU's (redwood_gpu_...) */

/*
 * Who holds gGPULock and for how long (said with the 5 s lines): each lock site by its line, the time it held the lock
 * and the time it waited for it, as a whole and at most. A 16 KB buffer's mapping -- a look-up under this lock -- took
 * 2 to 7 ms in an application's opening frames, run 235615: waiting for some other holder.
 */
#define GPU_LOCK_SITES 32
static struct { uint32_t line; uint32_t times; uint64_t held, heldMost, waited, waitedMost; } gLockSite[GPU_LOCK_SITES];
static uint32_t gLockHolder;            /* the site that holds it now */
static uint64_t gLockSince;
static void
gpuLockAt(uint32_t line)
{
	const uint64_t asked = mach_absolute_time();
	IOLockLock(gGPULock);
	const uint64_t now = mach_absolute_time();
	uint32_t i = 0;
	for (; i < GPU_LOCK_SITES - 1 && gLockSite[i].line != 0 && gLockSite[i].line != line; i++) {
	}
	gLockSite[i].line = line;
	gLockSite[i].times++;
	gLockSite[i].waited += now - asked;
	if (now - asked > gLockSite[i].waitedMost) gLockSite[i].waitedMost = now - asked;
	gLockHolder = i;
	gLockSince = now;
}
static void
gpuUnlockAt(void)
{
	const uint64_t held = mach_absolute_time() - gLockSince;
	gLockSite[gLockHolder].held += held;
	if (held > gLockSite[gLockHolder].heldMost) gLockSite[gLockHolder].heldMost = held;
	IOLockUnlock(gGPULock);
}
#define GPU_LOCK()   gpuLockAt(__LINE__)
#define GPU_UNLOCK() gpuUnlockAt()

/*
 * Memory given back under gGPULock, released once it is let go: a buffer's IOBufferMemoryDescriptor freed under the
 * lock (closing a buffer held it 50 to 75 ms in 5 s, 3.6 ms at most, run 000257) kept every other user of the GPU --
 * a buffer's mapping, a submission, a wait -- waiting. The GPU's own page tables are still undone under the lock;
 * the memory's own freeing, which needs nothing of the GPU's, is not.
 */
#define GPU_RELEASE_LATER 512
static OSObject * gReleaseLater[GPU_RELEASE_LATER];
static uint32_t gReleaseLaterCount;
static IOSimpleLock * gReleaseLaterLock;
static void
releaseLater(OSObject * o)
{
	if (gReleaseLaterLock != NULL) {
		IOSimpleLockLock(gReleaseLaterLock);
		if (gReleaseLaterCount < GPU_RELEASE_LATER) {
			gReleaseLater[gReleaseLaterCount++] = o;
			o = NULL;
		}
		IOSimpleLockUnlock(gReleaseLaterLock);
	}
	if (o != NULL) {
		o->release();
	}
}
static void
releasePending(void)
{
	OSObject * batch[64];
	uint32_t n;

	do {
		n = 0;
		if (gReleaseLaterLock == NULL) {
			return;
		}
		IOSimpleLockLock(gReleaseLaterLock);
		while (gReleaseLaterCount > 0 && n < 64) {
			batch[n++] = gReleaseLater[--gReleaseLaterCount];
		}
		IOSimpleLockUnlock(gReleaseLaterLock);
		for (uint32_t i = 0; i < n; i++) {
			batch[i]->release();
		}
	} while (n == 64);
}
static bool gGPUReported;
static bool gGPUCached;                         /* boot-arg gpu_cached=1 */
static bool gGPUSaidSetIova;                    /* MSM_INFO_SET_IOVA has been refused out loud once */

/* What the last seconds' submissions were and cost here, for the log. */
static struct {
	uint64_t since, flushing, waiting, taking;      /* times of the processor's clock */
	uint64_t inside, locking, ringing, reaping;     /* a submission's whole call; for the lock; in the ring; reap() */
	uint64_t flushed;                       /* bytes sent out of the cache */
	uint32_t submissions, commands, buffers, surfaces, waits, over;
	uint32_t displayWaits;                  /* surfaces a submission waited for, the panel's copy still reading them */
	uint32_t made, given_again;             /* buffers of their own made for programs, and of those kept ones given again */
	uint64_t making;                        /* ... and the time it took to make the others */
} gGPUStat;
/* ... and every request's: how many, and the time in them (by request number; the lock is not taken for this) */
static uint32_t gGPUCalls[0x80];
static uint64_t gGPUCallTime[0x80];

/* Wait for the 3D core to be done with submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (bool waited = false;; waited = true) {
		if (redwood_gpu_done(fence)) {
			if (waited) {
				gGPUStat.waits++;
				gGPUStat.waiting += mach_absolute_time() - began;
			}
			return true;
		}
		if (mach_absolute_time() - began >= patience || gGPUReported) {
			break;                  /* (a GPU that has stopped answering is not waited for again) */
		}
		GPU_UNLOCK();
		delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
		GPU_LOCK();
	}
	/*
	 * A wait of a second or more that came to nothing: the core has stopped answering. What it
	 * and its SMMU context say is put down, and then, with boot-arg gpu_recover=1, the core is
	 * reset and started again, which gives up the submission that hung and leaves later ones
	 * the core's own. Without that -- or if it does not work -- nothing is waited for again in
	 * this run: a GPU that may yet write memory of ours is given nothing more. Either way this
	 * wait has failed, and says so: the submission did not finish.
	 */
	if (milliseconds >= 1000 && !gGPUReported) {
		redwood_gpu_report("no answer");
		gGPUReported = !redwood_gpu_recover();
	}
	return false;
}

static TUNABLE(uint32_t, gDrawnLeavesCache, "fb_gpu", 0);    /* (fb_gpu=1: the processor's reads ask for themselves) */

bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	uint32_t fence = surface->drawn;
	bool done = true;

	if (fence != 0 && gGPULock != NULL) {
		GPU_LOCK();
		done = !redwood_gpu_ready() || gpuWait(fence, milliseconds);
		GPU_UNLOCK();
		/*
		 * The pixels the GPU has written are in memory; what this processor's cache holds of them
		 * may be older (the surface is ordinary cached memory: it was pushed out before the GPU
		 * was given it, but a line read back in meanwhile, by the display's copy of an earlier
		 * frame or ahead of time by the processor itself, is the old picture). The display reads
		 * it through the cache next, so the cache forgets the surface first, once for each
		 * submission that drew into it. OpenGL ES never needed this: eagl.c copies its frame
		 * into the surface with the processor. A Metal driver draws into it with the GPU
		 * (hakios-metal on the Adreno 750): without this, rows of an older frame -- black
		 * and torn ones while something moved -- reached the panel (shennong, 9 November).
		 */
		if (done && !gDrawnLeavesCache) {
			MiniIOSurfaceForProcessor(surface);
		}
	}
	return done;
}

/*
 * What the GPU drew of a surface out of this processor's cache, once a submission, before the processor reads it. With
 * fb_gpu=1 the display reads a frame's surface by the GPU, not the processor: then MiniIOSurfaceDrawn leaves the cache
 * (14 MB of it a frame) and the display asks here only where the processor does read it.
 */
void
MiniIOSurfaceForProcessor(MiniIOSurface * surface)
{
	uint32_t fence = surface->drawn;

	if (fence != 0 && fence != surface->drawnSeen && surface->kpixels != NULL && surface->info.allocSize != 0) {
		redwood_gpu_to_memory(surface->kpixels, surface->info.allocSize);
		surface->drawnSeen = fence;
	}
}

/*
 * Boot-arg fb_gpu=1: the display's copy of a frame onto the panel scaled by the GPU (MiniIOMobileFramebuffer.cpp,
 * drawLayer). The swap's surface, 1290 x 2796 (or the other phones' screens), is drawn by the 2D engine, filtered,
 * into a copy of the panel's size, 1440 x 3200, in ordinary memory; the display then copies that onto the panel row
 * for row behind the refresh, as it does a frame of the panel's own size (the refresh-synchronised copy stays what it
 * was; the processor's bilinear scaling, 6 to 7 ms of a frame of 8.3, goes). The commands are hakios-metal's own
 * scaled copy (panel_blit_a750.h, made by its tools/panel-blit.cpp), the surface's, the copy's and a table's GPU
 * addresses put into them. Any surface it was not made for -- another size, rows of another length, memory of a
 * program's own -- or a GPU that did not finish it in a second: the processor scales, as before (and after a failure
 * for the rest of the run).
 */
#include "panel_blit_a750.h"

static TUNABLE(uint32_t, gPanelGpu, "fb_gpu", 0);
#define PANEL_SURFACES  4
static struct {
	MiniIOSurface * s;      /* held (MiniIOSurfaceCopy) while it is mapped here */
	uint64_t        gpu, size, used;
} gPanelSurfaces[PANEL_SURFACES];
static IOBufferMemoryDescriptor * gPanelCopy, * gPanelAux;     /* the panel-sized copy; the table, then the commands */
static uint64_t gPanelCopyGpu, gPanelAuxGpu, gPanelUses;
static bool gPanelBroken;
static uint32_t gPanelRefusals;
static uint64_t gPanelTime, gPanelFrames;

/* `size` bytes of M from `from` on into the GPU's address space; 0: no room. With the lock held. */
static uint64_t
panelMap(IOMemoryDescriptor * m, uint64_t from, uint64_t size, bool readOnly)
{
	uint64_t at = redwood_gpu_address(size);
	bool ok = at != 0;

	/* (in the GPU's pages of 4 KB: the kernel's are 16, and stepping by those left three in four unmapped -- the SMMU
	 * refused the commands' page, run 114956) */
	for (uint64_t o = 0; ok && o < size; o += 4096) {
		IOByteCount length = 0;
		addr64_t phys = m->getPhysicalSegment(from + o, &length, kIOMemoryMapperNone);

		ok = phys != 0 && redwood_gpu_map(at + o, phys, readOnly);
	}
	redwood_gpu_mapped();
	if (!ok && at != 0) {
		redwood_gpu_unmap(at, size);
		redwood_gpu_address_free(at, size);
		at = 0;
	}
	return at;
}

static char gPanelState[200] = "not asked yet";

/* what the 5-second line of MiniIOMobileFramebuffer says of it (the first lines are lost from a long run's log) */
const char *
RedwoodGPUPanelState(void)
{
	return gPanelGpu ? gPanelState : "off (fb_gpu=0)";
}

static void
panelRefuse(const MiniIOSurface * s, const char * why)
{
	snprintf(gPanelState, sizeof(gPanelState), "%llu frames by the GPU; the last refusal: a %ux%u surface, rows %u, '%c%c%c%c': %.60s",
	    gPanelFrames, s->info.width, s->info.height, s->info.bytesPerRow, (int) (s->info.pixelFormat >> 24 & 0xff),
	    (int) (s->info.pixelFormat >> 16 & 0xff), (int) (s->info.pixelFormat >> 8 & 0xff), (int) (s->info.pixelFormat & 0xff), why);
	if (gPanelRefusals++ < 8) {
		IOLog("RedwoodGPU: fb_gpu: a %ux%u surface (rows of %u bytes, '%c%c%c%c') scaled by the processor: %s\n", s->info.width,
		    s->info.height, s->info.bytesPerRow, (int) (s->info.pixelFormat >> 24 & 0xff), (int) (s->info.pixelFormat >> 16 & 0xff),
		    (int) (s->info.pixelFormat >> 8 & 0xff), (int) (s->info.pixelFormat & 0xff), why);
	}
}

/* SIZE bytes of physical memory from PHYS on (the display's own, contiguous) in the GPU's address space, written by it; 0: no room */
uint64_t
RedwoodGPUMapPhysical(uint64_t phys, uint64_t size)
{
	uint64_t at;
	bool ok;

	if (gGPULock == NULL || !redwood_gpu_ready()) {
		return 0;
	}
	GPU_LOCK();
	at = redwood_gpu_address(size);
	ok = at != 0;
	for (uint64_t o = 0; ok && o < size; o += 4096) {
		ok = redwood_gpu_map(at + o, phys + o, false);
	}
	redwood_gpu_mapped();
	if (!ok && at != 0) {
		redwood_gpu_unmap(at, size);
		redwood_gpu_address_free(at, size);
		at = 0;
	}
	GPU_UNLOCK();
	return at;
}

bool
RedwoodGPUPanelScale(MiniIOSurface * s, uint32_t panelWidth, uint32_t panelHeight, const uint8_t ** pixels, uint32_t * rowBytes,
    uint64_t intoGpu)
{
	const struct panel_blit_variant * v = NULL;
	MiniIOSurface * drop = NULL, * held = NULL;
	uint64_t surfaceGpu = 0, began = mach_absolute_time(), size = round_page_64(s->info.allocSize);
	uint32_t * words, fence, count;
	uint64_t address;
	unsigned slot = 0;
	bool done;

	/* (not before the GPU has run a few hundred of the window server's submissions: the first one the kernel gave it
	 * by itself, the second of the run, hung -- run 213911 -- while the same commands from a program ran, probe 9p) */
	if (gPanelGpu && !gPanelBroken && gGPULock != NULL && redwood_gpu_ready() && redwood_gpu_last() < 300) {
		snprintf(gPanelState, sizeof(gPanelState), "waiting for the GPU to have done 300 submissions (%u)", redwood_gpu_last());
		return false;
	}
	if (!gPanelGpu || gPanelBroken || gGPULock == NULL || !redwood_gpu_ready()) {
		if (gPanelGpu && !gPanelBroken) {
			strlcpy(gPanelState, "no GPU to scale with", sizeof(gPanelState));
		}
		return false;
	}
	for (unsigned i = 0; i < sizeof(panel_blit_variants) / sizeof(panel_blit_variants[0]); i++) {
		const struct panel_blit_variant * c = &panel_blit_variants[i];

		if (c->sw == s->info.width && c->sh == s->info.height && c->dw == panelWidth && c->dh == panelHeight) {
			v = c;
		}
	}
	if (v == NULL) {
		panelRefuse(s, "no commands for that size onto this panel");
		return false;
	}
	/* (any format of four bytes a pixel, filtered as four 8-bit channels: what the processor's scaling does too) */
	if ((s->info.bytesPerRow & 63) || s->info.bytesPerRow < 4 * s->info.width || s->info.bytesPerRow >> 6 > PANEL_BLIT_PITCH_MASK >> PANEL_BLIT_PITCH_SHIFT ||
	    (s->info.offset & 63) || s->info.bytesPerElement != 4 || s->client != NULL || s->info.planeCount != 0 || size == 0) {
		panelRefuse(s, "its rows (not 64-byte aligned), format (not 4 bytes a pixel) or memory (a program's own) are not what the commands take");
		return false;
	}
	if (gPanelCopy == NULL) {
		gPanelCopy = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, (vm_size_t) v->dpitch * v->dh, PAGE_SIZE);
		gPanelAux = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, PANEL_BLIT_TABLE_BYTES + 4 * PAGE_SIZE, PAGE_SIZE);
		if (gPanelCopy == NULL || gPanelAux == NULL) {
			IOLog("RedwoodGPU: fb_gpu: no memory for the panel's copy: the processor scales\n");
			strlcpy(gPanelState, "BROKEN: no memory for the panel's copy", sizeof(gPanelState));
			gPanelBroken = true;
			return false;
		}
		bzero(gPanelCopy->getBytesNoCopy(), gPanelCopy->getLength());
		redwood_gpu_to_memory(gPanelCopy->getBytesNoCopy(), gPanelCopy->getLength());
		GPU_LOCK();
		gPanelCopyGpu = panelMap(gPanelCopy, 0, gPanelCopy->getLength(), false);
		gPanelAuxGpu = panelMap(gPanelAux, 0, gPanelAux->getLength(), false);
		GPU_UNLOCK();
		if (gPanelCopyGpu == 0 || gPanelAuxGpu == 0) {
			IOLog("RedwoodGPU: fb_gpu: no room in the GPU's address space for the panel's copy: the processor scales\n");
			strlcpy(gPanelState, "BROKEN: no room in the GPU's address space", sizeof(gPanelState));
			gPanelBroken = true;
			return false;
		}
		IOLog("RedwoodGPU: fb_gpu: frames of %ux%u scaled by the GPU onto %ux%u (copy at %#llx, commands at %#llx, %u words)\n",
		    v->sw, v->sh, v->dw, v->dh, gPanelCopyGpu, gPanelAuxGpu + PANEL_BLIT_TABLE_BYTES, v->count);
	}
	/* the surface in the GPU's address space: mapped once, kept (and held) while it is one of the last few shown */
	GPU_LOCK();
	for (unsigned i = 0; i < PANEL_SURFACES; i++) {
		if (gPanelSurfaces[i].s == s && gPanelSurfaces[i].size == size) {
			surfaceGpu = gPanelSurfaces[i].gpu;
			gPanelSurfaces[i].used = ++gPanelUses;
			break;
		}
		if (gPanelSurfaces[i].used < gPanelSurfaces[slot].used) {
			slot = i;
		}
	}
	GPU_UNLOCK();
	if (surfaceGpu == 0) {
		held = MiniIOSurfaceCopy(s->sid);
		if (held != s) {
			if (held != NULL) {
				MiniIOSurfaceDrop(held);
			}
			panelRefuse(s, "it could not be held");
			return false;
		}
		GPU_LOCK();
		if (gPanelSurfaces[slot].s != NULL) {
			redwood_gpu_unmap(gPanelSurfaces[slot].gpu, gPanelSurfaces[slot].size);
			redwood_gpu_address_free(gPanelSurfaces[slot].gpu, gPanelSurfaces[slot].size);
			drop = gPanelSurfaces[slot].s;
			gPanelSurfaces[slot].s = NULL;
		}
		surfaceGpu = panelMap(s->memory, PAGE_SIZE, size, true);
		if (surfaceGpu != 0) {
			gPanelSurfaces[slot].s = s;
			gPanelSurfaces[slot].gpu = surfaceGpu;
			gPanelSurfaces[slot].size = size;
			gPanelSurfaces[slot].used = ++gPanelUses;
			held = NULL;
		}
		GPU_UNLOCK();
		if (drop != NULL) {
			MiniIOSurfaceDrop(drop);
		}
		if (held != NULL) {
			MiniIOSurfaceDrop(held);
			panelRefuse(s, "no room in the GPU's address space for it");
			return false;
		}
	}
	/* the table as hakios-metal starts it, then the commands with the three addresses put in */
	{
		uint8_t * aux = (uint8_t *) gPanelAux->getBytesNoCopy();
		/* (fb_flip=1: straight into a buffer the display controller shows next, INTO_GPU) */
		const uint64_t bases[3] = { surfaceGpu + s->info.offset, intoGpu != 0 ? intoGpu : gPanelCopyGpu, gPanelAuxGpu };

		bzero(aux, PANEL_BLIT_TABLE_BYTES);
		for (uint32_t i = 0; i < v->ntable; i++) {
			((uint32_t *) aux)[v->table[i].at] = v->table[i].value;
		}
		words = (uint32_t *) (aux + PANEL_BLIT_TABLE_BYTES);
		memcpy(words, v->words, 4 * v->count);
		words[v->pitchAt] = (words[v->pitchAt] & ~PANEL_BLIT_PITCH_MASK) | ((s->info.bytesPerRow >> 6) << PANEL_BLIT_PITCH_SHIFT & PANEL_BLIT_PITCH_MASK);
		for (uint32_t i = 0; i < v->nrelocs; i++) {
			const uint64_t at = bases[v->relocs[i].kind] + v->relocs[i].offset;

			words[v->relocs[i].at] = v->relocs[i].high ? (uint32_t) (at >> 32) : (uint32_t) at;
		}
		redwood_gpu_to_memory(aux, PANEL_BLIT_TABLE_BYTES + 4 * v->count);
	}
	/* a surface the processor drew (OpenGL ES's eagl.c copies its frame in): its pixels out of the cache for the GPU */
	if (s->drawn == 0) {
		redwood_gpu_to_memory(s->kbase + PAGE_SIZE, size);
	}
	address = gPanelAuxGpu + PANEL_BLIT_TABLE_BYTES;
	count = v->count;
	GPU_LOCK();
	shennong_gpu_plain = true;      /* (hakios-metal's commands: nothing to rewrite) */
	fence = redwood_gpu_submit(&address, &count, 1);
	done = fence != 0 && gpuWait(fence, 1000);
	GPU_UNLOCK();
	if (!done) {
		static uint32_t failures;

		failures++;
		IOLog("RedwoodGPU: fb_gpu: the GPU did not scale a frame (submission %u, failure %u)\n", fence, failures);
		snprintf(gPanelState, sizeof(gPanelState), "%s after %llu frames: failure %u, submission %u not done in 1 s, the GPU's last done %u, "
		    "copy at %#llx, commands at %#llx, surface at %#llx", failures >= 3 ? "BROKEN" : "FAILED", gPanelFrames, failures, fence,
		    redwood_gpu_last(), gPanelCopyGpu, gPanelAuxGpu + PANEL_BLIT_TABLE_BYTES, surfaceGpu);
		gPanelBroken = failures >= 3;
		return false;
	}
	/* what the GPU wrote, and nothing of it older in this processor's cache */
	if (intoGpu == 0) {
		redwood_gpu_to_memory(gPanelCopy->getBytesNoCopy(), gPanelCopy->getLength());
	}
	*pixels = intoGpu != 0 ? NULL : (const uint8_t *) gPanelCopy->getBytesNoCopy();
	*rowBytes = v->dpitch;
	gPanelTime += mach_absolute_time() - began;
	gPanelFrames++;
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		snprintf(gPanelState, sizeof(gPanelState), "%llu frames by the GPU (the last %llu us, submitted to scaled)%s", gPanelFrames, ns / 1000,
		    gPanelRefusals ? "; some refused" : "");
	}
	if (gPanelFrames % 600 == 0) {
		uint64_t ns;

		absolutetime_to_nanoseconds(gPanelTime / 600, &ns);
		IOLog("RedwoodGPU: fb_gpu: %llu frames scaled by the GPU, the last 600 in %llu us each (submitted to scaled)\n",
		    gPanelFrames, ns / 1000);
		gPanelTime = 0;
	}
	return true;
}

/*
 * Boot-arg touch_lat=1: a line for every wait of a program's for the GPU that took a millisecond or
 * more ("tl: f KIND US at US"), for telling apart what holds a frame up (tools/frames.py).
 */
static TUNABLE(uint32_t, gWaitLines, "touch_lat", 0);

static void
slowWait(const char * kind, uint64_t began)
{
	uint64_t now = mach_absolute_time(), ns, at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(now - began, &ns);
	if (ns >= NSEC_PER_MSEC) {
		absolutetime_to_nanoseconds(now, &at);
		redwood_tl("tl: f %s %llu at %llu\n", kind, ns / 1000, at / 1000);
	}
}

/* How long from now a time of msm's is, in milliseconds; 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
	uint64_t now;
	int64_t left;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when->seconds * (int64_t) NSEC_PER_SEC + when->nanoseconds - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}

/*
 * A wait that was to end at a time that had passed when it was asked for, and the GPU was not
 * done: whoever asked goes on as if it were (a picture is shown half drawn). That is what comes
 * of a caller whose clock is not this one, so the first few are told.
 */
static void
waitOver(const struct msm_timespec * when, uint32_t fence)
{
	static uint32_t told;
	uint64_t now;

	gGPUStat.over++;
	if (told < 4) {
		told++;
		absolutetime_to_nanoseconds(mach_continuous_time(), &now);
		IOLog("RedwoodGPU: a wait for submission %u, which is not done, until %lld.%09lld s, asked for at %llu.%09llu s: "
		    "over before it began\n", fence, when->seconds, when->nanoseconds, now / NSEC_PER_SEC, now % NSEC_PER_SEC);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class RedwoodGPU : public IOService
{
	OSDeclareDefaultStructors(RedwoodGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodGPU, IOService);

bool
RedwoodGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "RedwoodGPUUserClient");
	setName("RedwoodGPU");
	registerService();
	return true;
}

/* The 3D core's speed by its load (shennong_adreno.c, boot-arg sgpu_dcvs=1): looked at every 50 ms, under the lock */
static thread_call_t gDcvsCall;

static void
dcvsTick(thread_call_param_t, thread_call_param_t)
{
	uint64_t deadline;

	if (gGPULock != NULL) {
		GPU_LOCK();
		shennong_gpu_dcvs();
		GPU_UNLOCK();
	}
	clock_interval_to_deadline(50, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(gDcvsCall, deadline);
}

void
RedwoodGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	RedwoodGPU * gpu;
	uint32_t cached = 0;

	gGPUCached = PE_parse_boot_argn("gpu_cached", &cached, sizeof(cached)) && cached != 0;
	gGPULock = IOLockAlloc();
	gReleaseLaterLock = IOSimpleLockAlloc();
	gpu = OSTypeAlloc(RedwoodGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("RedwoodGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("RedwoodGPU: the service did not start\n");
	}
	gpu->release();
	{
		uint32_t dcvs = 0;

		if (PE_parse_boot_argn("sgpu_dcvs", &dcvs, sizeof(dcvs)) && dcvs != 0 && (gDcvsCall = thread_call_allocate(dcvsTick, NULL)) != NULL) {
			uint64_t deadline;

			clock_interval_to_deadline(50, NSEC_PER_MSEC, &deadline);
			thread_call_enter_delayed(gDcvsCall, deadline);
		}
	}
}

/* The run is ending (redwood_adreno.c): nobody is in the middle of anything when the GPU goes. */
void
RedwoodGPUStop(void)
{
	GPU_LOCK();
	redwood_gpu_stop();
	GPU_UNLOCK();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOMemoryDescriptor *            memory;         /* NULL: this handle is free */
	uint8_t *                       kernel;         /* where it is in the kernel */
	MiniIOSurface *                 surface;        /* whose memory it is, when not its own */
	uint64_t                        address;        /* the GPU's for it */
	uint64_t                        size;
	uint32_t                        flags;
	uint32_t                        seed;           /* the surface's when it was last pushed out of the cache, */
	uint64_t                        pushed;         /* ... and when that was */
	bool                            seen;           /* by the program: mapped, and from then on written by it */
	uint32_t                        fence;          /* the last submission that names it (0: none has) */
};

/*
 * A buffer given back while the GPU is still at it: its memory and its place in the GPU's
 * address space are kept until the submission is done, and nobody waits for that. (A window
 * server that makes a surface for every frame gives the last frame's back while the GPU draws
 * this one: waiting there, for all the GPU had been given, cost it some five milliseconds a
 * frame.)
 */
struct gpu_left {
	IOMemoryDescriptor *            memory;
	MiniIOSurface *                 surface;
	uint64_t                        address, size;
	uint32_t                        fence, flags;
};
#define GPU_LEFT                64

/*
 * A buffer of the program's own given back, and done with: kept, memory and place in the GPU's
 * address space, for the next one of the same size and kind. CoreAnimation makes a new texture to
 * draw a blur into for every frame of an animation with one (Search: 1082 x 1226, 6 MB with Mesa's
 * rounding), and gives them back only later, a score at a time; Mesa keeps what is given back for
 * a second and then lets it go. Each made afresh here took 2 to 3.5 ms of the window server's frame
 * (memory got, cleared, made write-combined, and entered page by page into the GPU's address
 * space). A buffer kept is given again as it is, not cleared: it was the same program's (Mesa's own
 * cache gives buffers back the same way). At most GPU_KEPT_BYTES are kept (the one kept longest
 * makes way), none for longer than GPU_KEPT_SECONDS. rc.jobs.fps, Search: 8 such textures made
 * slowly instead of 77, the window server's frames drawn in 6.8 ms instead of 9.3-10 (median).
 */
struct gpu_kept {
	IOMemoryDescriptor *            memory;         /* NULL: this place is free */
	uint8_t *                       kernel;
	uint64_t                        address, size, since;
	uint32_t                        flags;
};
#define GPU_KEPT                48
#define GPU_KEPT_BYTES          (160ull << 20)
#define GPU_KEPT_SECONDS        60              /* a buffer not asked for again in so long is let go */
#define GPU_KEEP_FROM           (256u << 10)    /* bytes: smaller ones are not worth it */
static TUNABLE(uint32_t, gGPUKeep, "gpu_keep", 1);      /* boot-arg gpu_keep=0 (a test): none kept; gpu_keep=2: kept ones cleared when given again */

class RedwoodGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(RedwoodGPUUserClient);

	task_t                  fTask;
	struct gpu_buffer *     fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextQueue;
	uint32_t                fNextBuffer;            /* where the search for a free handle starts: one given back is not the next given */
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;
	uint32_t                fStrangers;             /* requests that named a buffer the client does not have */
	struct gpu_left         fLeft[GPU_LEFT];
	uint32_t                fLeftCount;
	struct gpu_kept         fKept[GPU_KEPT];
	uint64_t                fKeptBytes;

	struct gpu_buffer * buffer(uint32_t handle);
	struct gpu_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle);
	void freeBuffer(struct gpu_buffer * b);
	bool keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags);
	void dropKept(struct gpu_kept * k);
	void reap(void);
	IOReturn gemNew(struct msm_gem_new * request);
	IOReturn surfaceBuffer(struct redwood_surface * request);
	IOReturn gemInfo(struct msm_gem_info * request);
	IOReturn submit(struct msm_gem_submit * request);
	IOReturn show(const struct redwood_show * request);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodGPUUserClient, IOUserClient);

bool
RedwoodGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
RedwoodGPUUserClient::start(IOService * provider)
{
	if (!redwood_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	return fBuffers != NULL;
}

/*
 * With the lock held, and the GPU done with everything it has been given (`idle`). If it is
 * not -- it has stopped answering -- the buffer's memory stays the GPU's for good: it may yet be
 * written, and must not be somebody else's by then.
 */
void
RedwoodGPUUserClient::freeBuffer(struct gpu_buffer * b)
{
	bool busy = redwood_gpu_ready() && b->fence != 0 && !redwood_gpu_done(b->fence);

	if (busy && fLeftCount == GPU_LEFT) {
		busy = !gpuWait(b->fence, 1000);        /* (nowhere to keep it: this one is waited for) */
	}
	if (busy && fLeftCount < GPU_LEFT) {
		struct gpu_left * left = &fLeft[fLeftCount++];

		left->memory = b->memory;
		left->surface = b->surface;
		left->address = b->address;
		left->size = b->size;
		left->fence = b->fence;
		left->flags = b->flags;
	} else {
		if (busy) {
			static uint32_t said;

			if (b->surface == NULL) {
				b->memory->retain();
			} else {
				b->surface->memory->retain();
			}
			if (said++ < 8) {
				IOLog("RedwoodGPU: the GPU is not done with what it was given: a buffer of %llu bytes stays its own\n", b->size);
			}
		} else if (b->surface == NULL && redwood_gpu_ready() && keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
			b->memory = NULL;               /* (now the kept one's) */
		} else if (redwood_gpu_ready()) {
			redwood_gpu_unmap(b->address, b->size);
			redwood_gpu_address_free(b->address, b->size);
		}
		if (b->memory != NULL) {
			releaseLater(b->memory);
		}
		if (b->surface != NULL) {
			MiniIOSurfaceDrop(b->surface);
		}
	}
	b->memory = NULL;
	b->surface = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;
}

/* The buffers that were given back early and that the GPU is done with now: freed. With the lock held. */
void
RedwoodGPUUserClient::reap(void)
{
	for (uint32_t i = 0; i < fLeftCount;) {
		struct gpu_left * left = &fLeft[i];

		if (redwood_gpu_ready() && !redwood_gpu_done(left->fence)) {
			i++;
			continue;
		}
		if (left->surface == NULL && redwood_gpu_ready() &&
		    keep(left->memory, NULL, left->address, left->size, left->flags)) {
			left->memory = NULL;
		} else if (redwood_gpu_ready()) {
			redwood_gpu_unmap(left->address, left->size);
			redwood_gpu_address_free(left->address, left->size);
		}
		if (left->memory != NULL) {
			releaseLater(left->memory);
		}
		if (left->surface != NULL) {
			MiniIOSurfaceDrop(left->surface);
		}
		*left = fLeft[--fLeftCount];
	}
}

IOReturn
RedwoodGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		GPU_LOCK();
		/* what the client had the GPU do last may still be going on, in these buffers */
		if (redwood_gpu_ready()) {
			gpuWait(redwood_gpu_last(), 1000);
		}
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		reap();
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			dropKept(&fKept[i]);
		}
		if (fLeftCount != 0) {
			IOLog("RedwoodGPU: the GPU is not done with %u buffers of a program that is gone: they stay its own\n", fLeftCount);
		}
		GPU_UNLOCK();
		releasePending();
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
RedwoodGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
RedwoodGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		/* (a program that names a buffer it has given back draws into somebody else's next: worth knowing) */
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("RedwoodGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * A handle and addresses of the GPU's for `size` bytes of a memory descriptor, from byte `from`
 * of it on (wired memory; both in whole pages). With the lock held; NULL: there is no room.
 */
struct gpu_buffer *
RedwoodGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle)
{
	struct gpu_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = redwood_gpu_address(size)) == 0) {
		IOLog("RedwoodGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && redwood_gpu_map(b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
	}
	redwood_gpu_mapped();
	if (!mapped) {
		redwood_gpu_unmap(b->address, size);
		redwood_gpu_address_free(b->address, size);
		IOLog("RedwoodGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	b->size = size;
	b->flags = flags;
	b->seed = 0;
	b->pushed = 0;
	b->seen = false;
	b->fence = 0;
	b->surface = NULL;
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

/* With the lock held. */
void
RedwoodGPUUserClient::dropKept(struct gpu_kept * k)
{
	if (k->memory == NULL) {
		return;
	}
	if (redwood_gpu_ready()) {
		redwood_gpu_unmap(k->address, k->size);
		redwood_gpu_address_free(k->address, k->size);
	}
	releaseLater(k->memory);
	k->memory = NULL;
	fKeptBytes -= k->size;
}

/* With the lock held, the GPU done with the buffer: kept if it is worth it (those kept longest make way). */
bool
RedwoodGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
	struct gpu_kept * place = NULL;

	if (gGPUKeep == 0 || size < GPU_KEEP_FROM || size > GPU_KEPT_BYTES / 4 || memory == NULL ||
	    (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
		return false;
	}
	for (;;) {
		struct gpu_kept * oldest = NULL;

		place = NULL;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			if (fKept[i].memory == NULL) {
				place = place != NULL ? place : &fKept[i];
			} else if (oldest == NULL || fKept[i].since < oldest->since) {
				oldest = &fKept[i];
			}
		}
		if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
			break;
		}
		dropKept(oldest);
	}
	place->memory = memory;
	place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
	place->address = address;
	place->size = size;
	place->flags = flags;
	place->since = mach_absolute_time();
	fKeptBytes += size;
	return true;
}

/*
 * The kernel map, entry by entry, in each of kmem's ranges (vm_statistics.h's KMEM_RANGE_ID_*): how much of
 * it is taken, in how many entries, and its largest hole; then the biggest entries. Only inside those ranges:
 * the rest of the map holds the zones, and looking an address up among the zones' guarded pages (PGZ) is a
 * panic. (vm_map_region()'s basic information: it does not look at the pages.)
 */
static void
redwoodGPUMap(void)
{
	static const char * const names[KMEM_RANGE_COUNT] = { "outside", "ptr 0", "ptr 1", "ptr 2", "sprayqtn", "data" };
	uint64_t big[6] = { }, big_at[6] = { };
	uint32_t big_range[6] = { };

	for (uint32_t r = 1; r < KMEM_RANGE_COUNT; r++) {
		vm_map_offset_t address = kmem_ranges[r].min_address, last = kmem_ranges[r].min_address;
		vm_map_size_t size = 0;
		uint64_t used = 0, hole = 0;
		uint32_t entries = 0;

		while (address < kmem_ranges[r].max_address) {
			vm_region_basic_info_data_64_t info;
			mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
			mach_port_t name = MACH_PORT_NULL;

			if (vm_map_region(kernel_map, &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t) &info, &count,
			    &name) != KERN_SUCCESS || address >= kmem_ranges[r].max_address) {
				break;
			}
			if (address > last && address - last > hole) {
				hole = address - last;
			}
			used += size;
			entries++;
			for (uint32_t i = 0; i < 6; i++) {
				if (size > big[i]) {
					memmove(&big[i + 1], &big[i], (5 - i) * sizeof(big[0]));
					memmove(&big_at[i + 1], &big_at[i], (5 - i) * sizeof(big_at[0]));
					memmove(&big_range[i + 1], &big_range[i], (5 - i) * sizeof(big_range[0]));
					big[i] = size;
					big_at[i] = address;
					big_range[i] = r;
					break;
				}
			}
			address += size;
			last = address;
		}
		if (kmem_ranges[r].max_address > last && kmem_ranges[r].max_address - last > hole) {
			hole = kmem_ranges[r].max_address - last;
		}
		IOLog("RedwoodGPU: kernel map: %s %#llx..%#llx (%llu MB): %llu MB taken in %u entries, the largest hole %llu MB\n",
		    names[r], (uint64_t) kmem_ranges[r].min_address, (uint64_t) kmem_ranges[r].max_address,
		    (uint64_t) (kmem_ranges[r].max_address - kmem_ranges[r].min_address) >> 20, used >> 20, entries, hole >> 20);
	}
	for (uint32_t i = 0; i < 6 && big[i] != 0; i++) {
		IOLog("RedwoodGPU: kernel map: entry %#llx, %llu MB, in %s\n", big_at[i], big[i] >> 20, names[big_range[i]]);
	}
}

/*
 * What the machine's memory is doing: the pages by state, the kernel and zone maps, and the biggest holders of
 * wired memory by kind (vm_statistics.h's VM_KERN_MEMORY_*). With the five-second statistics, and when there is
 * no memory for a buffer (size: then at most every two seconds). A buffer's memory comes from
 * kernel_memory_allocate() in the kernel map's data range, so that is tried again by hand first:
 * KERN_RESOURCE_SHORTAGE (6) is the pages, KERN_NO_SPACE (3) the addresses.
 */
static void
redwoodGPUMemory(uint64_t size)
{
	static const char * const kinds[VM_KERN_MEMORY_FIRST_DYNAMIC] = {
		"none", "osfmk", "bsd", "iokit", "libkern", "oskext", "kext", "ipc", "stack", "cpu", "pmap", "pte", "zone",
		"kalloc", "compressor", "compressed data", "phantom cache", "waitq", "diag", "log", "file", "mbuf", "ubc",
		"security", "mlock", "reason", "skywalk", "ltable", "hv", "kalloc data", "retired", "kalloc type", "triage",
		"recount",
	};
	static uint64_t last;
	uint64_t now = mach_absolute_time(), apart;
	vm_offset_t address = 0;
	kern_return_t kr = KERN_SUCCESS;
	vm_map_size_t map_size, map_free, map_largest, zone_size, zone_free, zone_largest;
	uint64_t top[8] = { };
	vm_tag_t which[8] = { };
	char line[512];
	int at = 0;

	nanoseconds_to_absolutetime(2 * NSEC_PER_SEC, &apart);
	if (size != 0) {
		if (last != 0 && now - last < apart) {
			return;
		}
		last = now;
		kr = kernel_memory_allocate(kernel_map, &address, (vm_size_t) size, PAGE_MASK,
		    (kma_flags_t) (KMA_DATA | KMA_ZERO), VM_KERN_MEMORY_IOKIT);
		if (kr == KERN_SUCCESS) {
			kmem_free(kernel_map, address, (vm_size_t) size);
		}
	}
	vm_map_sizes(kernel_map, &map_size, &map_free, &map_largest);
	zone_map_sizes(&zone_size, &zone_free, &zone_largest);
	IOLog("RedwoodGPU: memory: %llu bytes by hand: %d; MB of pages: %llu free (target %llu, reserved %llu), %llu wired "
	    "(%llu of them from the start), %llu active, %llu inactive, %llu speculative, %llu throttled, %llu purgeable, %llu "
	    "compressed; max_mem %llu MB, sane_size %llu; kernel map %llu GB, %llu free, largest %llu MB; zones %llu MB, %llu "
	    "free, largest %llu\n", size, kr, ptoa_64(vm_page_free_count) >> 20, ptoa_64(vm_page_free_target) >> 20,
	    ptoa_64(vm_page_free_reserved) >> 20, ptoa_64(vm_page_wire_count) >> 20, ptoa_64(vm_page_wire_count_initial) >> 20,
	    ptoa_64(vm_page_active_count) >> 20, ptoa_64(vm_page_inactive_count) >> 20, ptoa_64(vm_page_speculative_count) >> 20,
	    ptoa_64(vm_page_throttled_count) >> 20, ptoa_64(vm_page_purgeable_count) >> 20,
	    ptoa_64(c_segment_pages_compressed) >> 20, max_mem >> 20, sane_size >> 20, map_size >> 30, map_free >> 30,
	    map_largest >> 20, zone_size >> 20, zone_free >> 20, zone_largest >> 20);

	for (vm_tag_t tag = 1; tag < VM_MAX_TAG_VALUE; tag++) {
		uint64_t bytes = vm_tag_get_size(tag);

		for (uint32_t i = 0; i < 8; i++) {
			if (bytes > top[i]) {
				memmove(&top[i + 1], &top[i], (7 - i) * sizeof(top[0]));
				memmove(&which[i + 1], &which[i], (7 - i) * sizeof(which[0]));
				top[i] = bytes;
				which[i] = tag;
				break;
			}
		}
	}
	for (uint32_t i = 0; i < 8 && top[i] != 0 && at < (int) sizeof(line) - 64; i++) {
		const char * name = which[i] < VM_KERN_MEMORY_FIRST_DYNAMIC ? kinds[which[i]] :
		    vm_allocation_sites[which[i]] != NULL ? kern_allocation_get_name(vm_allocation_sites[which[i]]) : NULL;

		at += snprintf(line + at, sizeof(line) - at, "%s%s (%u) %llu MB", i ? ", " : "", name ? name : "?", which[i],
		    top[i] >> 20);
	}
	IOLog("RedwoodGPU: memory: wired by kind: %s\n", at ? line : "nothing counted");

	static uint64_t mapped;
	uint64_t every;

	nanoseconds_to_absolutetime(30 * NSEC_PER_SEC, &every);
	if (size != 0 || mapped == 0 || now - mapped >= every) {
		mapped = now;
		redwoodGPUMap();
	}
}

IOReturn
RedwoodGPUUserClient::gemNew(struct msm_gem_new * request)
{
	uint64_t size = round_page_64(request->size), began = mach_absolute_time();
	struct gpu_buffer * b;
	IOBufferMemoryDescriptor * memory;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* one kept of this size and kind, if there is one (the one kept last: likeliest still in the caches' way) */
	GPU_LOCK();
	struct gpu_kept * found = NULL;
	uint64_t old;

	nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		struct gpu_kept * k = &fKept[i];

		if (k->memory != NULL && began - k->since > old) {
			dropKept(k);
		}

		if (k->memory != NULL && k->size == size && k->flags == request->flags && (found == NULL || k->since > found->since)) {
			found = k;
		}
	}
	for (uint32_t n = 0; found != NULL && n < GPU_BUFFERS; n++) {
		uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[h].memory == NULL) {
			b = &fBuffers[h];
			bzero(b, sizeof(*b));
			b->memory = found->memory;
			b->kernel = found->kernel;
			b->address = found->address;
			b->size = found->size;
			b->flags = found->flags;
			fNextBuffer = h + 1;
			fBufferCount++;
			fBufferBytes += size;
			request->handle = h + 1;
			found->memory = NULL;
			fKeptBytes -= size;
			gGPUStat.given_again++;
			GPU_UNLOCK();
			if (gGPUKeep == 2) {
				bzero(b->kernel, (size_t) b->size);
			}
			return kIOReturnSuccess;
		}
	}
	GPU_UNLOCK();
	/* (the memory comes zeroed, and making it write-combined pushes the zeroes out of the cache) */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("RedwoodGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		redwoodGPUMemory(size);
		return kIOReturnNoMemory;
	}
	if (gGPUCached) {
		redwood_gpu_to_memory(memory->getBytesNoCopy(), size);
	}

	GPU_LOCK();
	b = newBuffer(memory, 0, size, request->flags, &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->kernel = (uint8_t *) memory->getBytesNoCopy();
		gGPUStat.made++;
		gGPUStat.making += mach_absolute_time() - began;
	}
	GPU_UNLOCK();
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * The memory of an IOSurface as a buffer: its pixels, from the page after the surface's shared
 * state on (MiniIOSurface.h). The surface is held for as long as the buffer is there. A surface
 * that is a buffer of this client's already is that buffer again, as with Linux's handles: so
 * the client must not be giving that buffer back at the same moment (freedreno asks under the
 * lock it gives buffers back under). Asked only to describe (kRedwoodDescribe), no buffer is made.
 */
IOReturn
RedwoodGPUUserClient::surfaceBuffer(struct redwood_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct gpu_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size, began = mach_absolute_time();

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		/* (memory of a program's own with a surface made around it: not in whole pages, and seldom in rows the GPU takes) */
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > GPU_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kRedwoodDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}

	GPU_LOCK();
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
	} else if ((b = newBuffer(s->memory, PAGE_SIZE, size, 0, &request->handle)) != NULL) {
		b->memory = memory;
		b->kernel = s->kbase + PAGE_SIZE;
		b->surface = s;
		gGPUStat.surfaces++;
	}
	gGPUStat.taking += mach_absolute_time() - began;
	GPU_UNLOCK();
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
RedwoodGPUUserClient::gemInfo(struct msm_gem_info * request)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	GPU_LOCK();
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t) request->handle << 32;      /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		/*
		 * The address the buffer was given when it was made, and the buffer's until it is
		 * given back. Nothing here ever moves a live buffer, so a caller may ask once and
		 * keep the answer -- which is what freedreno does (fd_bo_init_common).
		 */
		request->value = b->address;
	} else if (request->info == MSM_INFO_SET_IOVA) {
		/*
		 * Where a buffer goes is not the program's to choose here: every client is in the
		 * one SMMU context 0, so an address one program picked would be a place in every
		 * other program's space as well. Linux draws the line in the same place for the
		 * two parameters that describe a space -- VA_START and VA_SIZE are refused unless
		 * the client has page tables of its own (adreno_gpu.c, adreno_get_param) -- and
		 * placing a buffer is the stronger request of the two. Said once, by name, rather
		 * than returned as a bare code nobody can look up.
		 */
		if (!gGPUSaidSetIova) {
			gGPUSaidSetIova = true;
			IOLog("RedwoodGPU: a program asked to place buffer %u at %#llx itself: refused. There is one "
			    "page table here for every client, so addresses are the kernel's to give and "
			    "MSM_INFO_GET_IOVA is how to learn them\n", request->handle, request->value);
		}
		ret = kIOReturnUnsupported;
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {            /* a name for somebody's debugger: not kept */
		ret = kIOReturnUnsupported;
	}
	GPU_UNLOCK();
	return ret;
}

IOReturn
RedwoodGPUUserClient::submit(struct msm_gem_submit * request)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	uint64_t began, nanos, again, entered = mach_absolute_time(), asked;
	IOReturn ret = kIOReturnBadArgument;

	if (request->nr_cmds == 0 || request->nr_cmds > GPU_SUBMIT_CMDS || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		IOLog("RedwoodGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	/*
	 * A surface the GPU is to write that is still on its way to the panel -- a swap holds it, waiting
	 * or being copied there (MiniIOMobileFramebuffer.cpp) -- is waited for first: the copy reads it
	 * row by row for some milliseconds, and the GPU drawing the next frame into it meanwhile put rows
	 * of a half-drawn frame on the panel, black ones while something moved (a Metal driver draws into
	 * the window server's surfaces with the GPU; OpenGL ES copied its frame in with the processor at
	 * the swap). Out of gGPULock, which the copy takes to wait for the GPU; 100 ms at most.
	 */
	{
		MiniIOSurface * shown[8];
		uint32_t held = 0;

		GPU_LOCK();
		for (uint32_t i = 0; i < request->nr_bos && held < 8; i++) {
			/* (written, whether or not also read: CoreAnimation's passes read what they draw into through
			 * framebuffer fetch, so hakios-metal names their target both ways. freedreno names every
			 * buffer both ways too; under OpenGL ES the surfaces on their way to the panel are written
			 * by the processor, so a submission of its meets one only to read it, and waits a copy's
			 * length at most) */
			struct gpu_buffer * b = (bos[i].flags & 0x2) ? buffer(bos[i].handle) : NULL;   /* MSM_SUBMIT_BO_WRITE */

			if (b != NULL && b->surface != NULL && b->surface->displayed > 0) {
				b->surface->retain();
				shown[held++] = b->surface;
			}
		}
		GPU_UNLOCK();
		for (uint32_t i = 0; i < held; i++) {
			for (uint32_t ms = 0; shown[i]->displayed > 0 && ms < 100; ms++) {
				IOSleep(1);
			}
			gGPUStat.displayWaits++;
			shown[i]->release();
		}
	}
	asked = mach_absolute_time();
	GPU_LOCK();
	gGPUStat.locking += mach_absolute_time() - asked;
	/*
	 * The GPU reads memory and writes memory. What this processor's cache holds of an IOSurface
	 * (somebody has written it, or has read it, as the display's driver does, and the GPU is to
	 * draw into it again) goes out of the cache now: when the surface is said to have changed
	 * (its seed: who writes one is to lock it, and so does a program that has had the GPU draw
	 * into one: userland/src/eagl.c), and in any case once in a few milliseconds, which is once
	 * a frame and not once a submission: not everybody who draws into a surface says so (an
	 * application drawing a picture that is on the screen meanwhile). A surface this program
	 * has mapped, every time. The buffers made here need none of that, unless they are
	 * ordinary memory (gpu_cached=1).
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
	{
		/*
		 * What is to be pushed out of the cache is picked under the lock and pushed out of it (each one's memory
		 * held meanwhile, so that a buffer closed by another thread cannot go away under the push): a screen-sized
		 * surface's push took the lock a millisecond and more, while every other user of the GPU waited, run 000257.
		 * The lock is then taken again for the ring: the push is done before the GPU is given the work.
		 */
		struct { uint8_t * kernel; uint64_t size; OSObject * held; } push[128];
		uint32_t pushes = 0;

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			if (b == NULL) {
				continue;
			}
			if (b->surface != NULL) {
				uint32_t seed = b->surface->shared()->seed;

				if (seed == b->seed && !b->seen && began - b->pushed < again) {
					continue;
				}
				b->seed = seed;
				b->pushed = began;
			} else if (!b->seen || !gGPUCached) {
				continue;
			}
			OSObject * held = b->surface != NULL ? (OSObject *) b->surface : (OSObject *) b->memory;
			if (pushes < 128 && held != NULL && b->kernel != NULL) {
				held->retain();
				push[pushes].kernel = b->kernel;
				push[pushes].size = b->size;
				push[pushes].held = held;
				pushes++;
			} else {
				redwood_gpu_to_memory(b->kernel, b->size);
			}
			gGPUStat.flushed += b->size;
		}
		if (pushes != 0) {
			GPU_UNLOCK();
			for (uint32_t i = 0; i < pushes; i++) {
				redwood_gpu_to_memory(push[i].kernel, push[i].size);
				push[i].held->release();
			}
			redwood_gpu_to_memory(NULL, 0);
			GPU_LOCK();
		}
	}
	redwood_gpu_to_memory(NULL, 0);                 /* (and what was written last is in memory: a barrier) */
	gGPUStat.flushing += mach_absolute_time() - began;
	gGPUStat.buffers += request->nr_bos;
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size) {
			IOLog("RedwoodGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			GPU_UNLOCK();
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	asked = mach_absolute_time();
	/* (0x00800000, hakios-metal's: a stream that is not Mesa's, given as it is -- see shennong_gpu_submit) */
	shennong_gpu_plain = (request->flags & 0x00800000) != 0;
	request->fence = redwood_gpu_submit(addresses, words, count);
	gGPUStat.ringing += mach_absolute_time() - asked;
	/*
	 * The ring is full and the core is not reading it: it has stopped. With gpu_recover=1 it is
	 * reset and given this submission again -- after a reset the ring is empty, so there is
	 * room for it. (A wait of a second or more notices a stopped core first, as a rule; this is
	 * for a program that submits without waiting.)
	 */
	if (request->fence == 0 && !gGPUReported) {
		redwood_gpu_report("the ring is full");
		gGPUReported = !redwood_gpu_recover();
		if (!gGPUReported) {
			request->fence = redwood_gpu_submit(addresses, words, count);
		}
	}
	if (gpu_log && request->fence != 0) {
		/* "tl: Q FENCE at US: N buffers, B of 4 MB or more, W words of commands": what was given (tl: q: what it took) */
		uint32_t big = 0, all_words = 0;
		uint64_t ns;

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			big += b != NULL && b->size >= (4u << 20);
		}
		for (uint32_t i = 0; i < count; i++) {
			all_words += words[i];
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		redwood_tl("tl: Q %u at %llu: %u buffers, %u big, %u words\n", request->fence, ns / 1000, request->nr_bos, big, all_words);
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	/*
	 * The surfaces named are being drawn into, or read, until this submission is done: the
	 * display's driver waits for that before it puts one on the panel (MiniIOSurfaceDrawn()).
	 * (freedreno names every buffer for reading and writing alike, so which it is cannot be
	 * told; the last submission of a frame names the surface the frame is drawn into.)
	 */
	for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
			if (b->surface != NULL) {
				b->surface->drawn = request->fence;
			}
		}
	}
	asked = mach_absolute_time();
	reap();
	gGPUStat.reaping += mach_absolute_time() - asked;
	if (fSubmits++ < 4) {
		IOLog("RedwoodGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    addresses[0], words[0]);
	}
	/* every five seconds in which something was submitted: how much, and what it cost here */
	gGPUStat.submissions++;
	gGPUStat.commands += count;
	gGPUStat.inside += mach_absolute_time() - entered;
	if (gGPUStat.since == 0) {
		gGPUStat.since = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t flushing, waiting, taking, worked;
		uint32_t timed, by[7];

		absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
		absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
		absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
		redwood_gpu_worked(&worked, &timed, by);
		uint64_t making;

		absolutetime_to_nanoseconds(gGPUStat.making, &making);
		IOLog("RedwoodGPU: buffers: %u made in %llu us each, %u kept ones given again (%llu MB kept)\n", gGPUStat.made,
		    gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.given_again, fKeptBytes >> 20);
		redwoodGPUMemory(0);
		IOLog("RedwoodGPU: %u submissions in %llu ms: %u command buffers and %u buffers named, %llu KB out of the cache "
		    "in %llu us a submission; the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
		    "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all); %u waits of %llu us, %u over before they "
		    "began; %u IOSurfaces taken as buffers in %llu us each; %u buffers, %llu MB\n",
		    gGPUStat.submissions, nanos / NSEC_PER_MSEC, gGPUStat.commands, gGPUStat.buffers,
		    (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions,
		    timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000,
		    gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
		    gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
		    fBufferBytes >> 20);
		{
			/* (what a submission costs its program here, in the kernel, as a whole and by part) */
			uint64_t inside, locking, ringing, reaping;

			absolutetime_to_nanoseconds(gGPUStat.inside, &inside);
			absolutetime_to_nanoseconds(gGPUStat.locking, &locking);
			absolutetime_to_nanoseconds(gGPUStat.ringing, &ringing);
			absolutetime_to_nanoseconds(gGPUStat.reaping, &reaping);
			IOLog("RedwoodGPU: a submission takes %llu us here (%llu waiting for the lock, %llu out of the cache, "
			    "%llu into the ring, %llu giving back what is done), %llu ms of the %llu\n",
			    inside / NSEC_PER_USEC / gGPUStat.submissions, locking / NSEC_PER_USEC / gGPUStat.submissions,
			    flushing / NSEC_PER_USEC / gGPUStat.submissions, ringing / NSEC_PER_USEC / gGPUStat.submissions,
			    reaping / NSEC_PER_USEC / gGPUStat.submissions, inside / NSEC_PER_MSEC, nanos / NSEC_PER_MSEC);
		}
		{
			/* (the lock's holders: line, times, held us all/most, waited us all/most -- the top by time held) */
			char line[600];
			int at = 0;
			for (int round = 0; round < 6 && at < (int) sizeof(line) - 90; round++) {
				int best = -1;
				for (int k = 0; k < GPU_LOCK_SITES; k++) {
					if (gLockSite[k].line != 0 && gLockSite[k].times != 0 && (best < 0 || gLockSite[k].held > gLockSite[best].held)) best = k;
				}
				if (best < 0) break;
				uint64_t h, hm, w, wm;
				absolutetime_to_nanoseconds(gLockSite[best].held, &h);
				absolutetime_to_nanoseconds(gLockSite[best].heldMost, &hm);
				absolutetime_to_nanoseconds(gLockSite[best].waited, &w);
				absolutetime_to_nanoseconds(gLockSite[best].waitedMost, &wm);
				at += snprintf(line + at, sizeof(line) - at, " %u: %u x, held %llu us (most %llu), waited %llu us (most %llu);", gLockSite[best].line,
				    gLockSite[best].times, h / NSEC_PER_USEC, hm / NSEC_PER_USEC, w / NSEC_PER_USEC, wm / NSEC_PER_USEC);
				gLockSite[best].times = 0;      /* (taken: the next round finds the next) */
			}
			for (int k = 0; k < GPU_LOCK_SITES; k++) {
				gLockSite[k].times = 0;
				gLockSite[k].held = gLockSite[k].heldMost = gLockSite[k].waited = gLockSite[k].waitedMost = 0;
			}
			IOLog("RedwoodGPU: the lock's holders by line, the last 5 s:%s\n", line);
		}
		{
			/* (and every request: "number: how many x us each"; 0x46 submits, 0x44 and 0x47 wait for the GPU, 0x42 makes a buffer) */
			char line[512];
			int at = 0;
			uint64_t all = 0;

			for (unsigned i = 0; i < 0x80 && at < (int) sizeof(line) - 40; i++) {
				uint64_t ns;

				if (gGPUCalls[i] == 0) {
					continue;
				}
				absolutetime_to_nanoseconds(gGPUCallTime[i], &ns);
				all += ns;
				at += snprintf(line + at, sizeof(line) - at, " %#x: %u x %llu us;", i, gGPUCalls[i],
				    ns / NSEC_PER_USEC / gGPUCalls[i]);
				gGPUCalls[i] = 0;
				gGPUCallTime[i] = 0;
			}
			IOLog("RedwoodGPU: requests:%s %llu ms in all\n", line, all / NSEC_PER_MSEC);
		}
		if (gpu_va != 0) {
			/*
			 * The address space itself, because nothing else said anything about it: what
			 * argument buffers and heaps are built on is that this allocator holds, and
			 * "lost" and "refused" are the two ways it stops holding (bsd/dev/
			 * redwood_adreno.c, redwood_gpu_address_free).
			 */
			uint64_t used, largest, lost;
			uint32_t live, refused;
			unsigned holes, most;

			redwood_gpu_space(&used, &live, &largest, &holes, &most, &lost, &refused);
			IOLog("RedwoodGPU: addresses: %llu MB in %u buffers, longest run free %llu MB, %u holes "
			    "(%u at most, of 1024), %llu KB lost, %u allocations refused\n", used >> 20, live,
			    largest >> 20, holes, most, lost >> 10, refused);
		}
		bzero(&gGPUStat, sizeof(gGPUStat));
	}
	GPU_UNLOCK();
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}

/* Pixels of the caller's (32 bits each, blue first) copied to the panel: for a test to show what it has drawn. */
IOReturn
RedwoodGPUUserClient::show(const struct redwood_show * request)
{
	uint64_t size = (uint64_t) request->rowBytes * request->height;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
RedwoodGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	GPU_LOCK();
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		b->seen = true;
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	GPU_UNLOCK();
	return ret;
}

IOReturn
RedwoodGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct redwood_surface surface;
		struct redwood_show show;
		struct redwood_worked worked;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0, patience, waitBegan, called = mach_absolute_time();

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!redwood_gpu_ready() && selector != kRedwoodShow) {
		return kIOReturnNotReady;                       /* given back: the run is ending */
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		ret = redwood_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kMsmSetParam:                              /* the program's name, for Linux's lists of who uses the GPU */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep:                            /* is the GPU done with a buffer: with all it has been given, here */
		waitBegan = mach_absolute_time();
		GPU_LOCK();
		patience = (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout);
		if (!gpuWait(redwood_gpu_last(), patience)) {
			ret = kIOReturnBusy;
			if (patience == 0 && !(r.cpuPrep.op & MSM_PREP_NOSYNC)) {
				waitOver(&r.cpuPrep.timeout, redwood_gpu_last());
			}
		}
		GPU_UNLOCK();
		slowWait("cpu-prep", waitBegan);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		waitBegan = mach_absolute_time();
		GPU_LOCK();
		patience = millisecondsUntil(&r.waitFence.timeout);
		if (!gpuWait(r.waitFence.fence, patience)) {
			ret = kIOReturnTimeout;
			if (patience == 0) {
				waitOver(&r.waitFence.timeout, r.waitFence.fence);
			}
		}
		GPU_UNLOCK();
		slowWait("fence", waitBegan);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:                      /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		GPU_LOCK();
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
			reap();
		}
		GPU_UNLOCK();
		break;
	case kRedwoodGPUWorked:
		/*
		 * Under gGPULock because redwood_gpu_worked() calls gpu_times(), which reads the
		 * scratch page and advances the same counters the submit path advances -- and
		 * submit() holds this lock while it does exactly that. The output goes back
		 * through the common copy at the end of this method, so a caller that passes a
		 * struct of this size in gets it filled on the way out; one that passes fewer
		 * bytes gets as many as it asked for, and the rest of the numbers are lost with
		 * the reset, which is the one way to misuse this request.
		 */
		GPU_LOCK();
		redwood_gpu_worked(&r.worked.microseconds, &r.worked.submissions, r.worked.by);
		GPU_UNLOCK();
		break;
	case kRedwoodSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kRedwoodShow:
		ret = show(&r.show);
		break;
	default:
		IOLog("RedwoodGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	releasePending();       /* (what this request gave back under the lock, freed out of it) */
	gGPUCalls[selector & 0x7f]++;
	gGPUCallTime[selector & 0x7f] += mach_absolute_time() - called;
	return ret;
}
