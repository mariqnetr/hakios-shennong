/*
 * The audio DSP's sensors' domain on the Xiaomi 14 Pro (shennong, SM8650), served its files over FastRPC as
 * Android's sscrpcd does. Nothing here happens unless boot-arg snsrpc=N asks for it.
 *
 * The DSP's sensors' framework (SEE, in its static process "sensorspd") runs as soon as the DSP does, but with
 * no sensor in it: it reads its registry (Android's /mnt/vendor/persist/sensors/registry/registry/...) and the
 * chips' configuration (/vendor/etc/sensors/...) from the application processor, through FastRPC's reverse
 * calls, which on Android sscrpcd answers (libadsprpc's listener: github.com/quic/fastrpc). Without them every
 * SUID lookup of shennong_sensors.c is answered with none. What is done here, as libadsprpc does it:
 *
 *   1  the DSP's FastRPC stream for that domain, 0x1004 (dt: qcom,msm_fastrpc_compute_cb11, pd-type 3 the
 *      sensors' static process; iommus <&apps_smmu 0x1004 0x80>), through a context of the SMMU's of ours
 *      with page tables of ours: a megabyte of ours behind the DSP's addresses 0x10000000 on
 *   2  "attach sensors" (Linux's FASTRPC_IOCTL_INIT_ATTACH_SNS: handle 1, method 0, our process's id), our
 *      process's invocations from then on in the domain 2
 *   3  remotectl_open("adsp_listener") and its init2; remotectl_open("adsp_default_listener") and its register
 *   4  over and over adsp_listener_next2: it returns when the DSP has a call for us (a handle, a method, its
 *      buffers), and takes our answer to the previous one. Handle 0 is apps_remotectl (the DSP opens our
 *      modules by name with it); apps_std is the files (fopen, fread, opendir, readdir, stat...), served out of
 *      fw/sns.tar of the small image (Android's files, pulled from the phone into build/fw-gpu/sns.tar); what is
 *      written is said and dropped. Every call is said in the log, what is not served with its arguments.
 *
 *   snsrpc=1  all that, a line per call
 *   snsrpc=2  ... and the first bytes of every buffer
 * snsrpc_wait=S: shennong_sensors.c's lookups (gyro=N) wait up to S seconds (default 20) for the files to be read.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern lck_mtx_t redwood_smmu_lock;

static LCK_GRP_DECLARE(snsrpc_lock_group, "shennong-snsrpc");
static LCK_MTX_DECLARE(say_lock, &snsrpc_lock_group);

static uint32_t mode;
static volatile bool settled, gone;
static volatile uint64_t last_call;       /* (mach_absolute_time) */
static volatile bool called;            /* the DSP has called us at least once */

static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[400];
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

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

static void
clean(const void *start, uint64_t bytes)
{
	vm_offset_t line = (vm_offset_t)start & ~63ul, end = (vm_offset_t)start + bytes;

	for (; line < end; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* ---- 1: the SMMU ----------------------------------------------------------------------------------------------- */

#define SMMU                    0x15000000      /* apps_smmu */
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define PTE                     (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)  /* as shennong_sound.c's */

#define STREAM                  0x1004
#define STREAM_MASK             0x80
#define STREAM_BITS             (4ull << 32)    /* (0x1004 & 0xf: where the DSP takes the stream from) */
#define IOVA                    0x10000000ull
#define REGION_PAGES            512             /* two megabytes: the listener's 1.5, the other calls' 0.5 */
#define REGION_BYTES            (REGION_PAGES * 4096u)

static struct {
	vm_offset_t smmu, context_base;
	uint32_t page, pages, match, context, old_smr, old_s2cr, old_sctlr;
	bool on, passed;
	vm_offset_t tables;
} space;
static uint8_t *region;                 /* REGION_BYTES of ours, physically one piece */
static uint64_t region_phys, region_dsp_all; /* ... and as the DSP sees it */

static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts;

	space.smmu = ml_io_map(SMMU, 0x100000);
	if (space.smmu == 0) {
		return false;
	}
	scr0 = rd(space.smmu + SMMU_SCR0);
	idr0 = rd(space.smmu + SMMU_IDR0);
	idr1 = rd(space.smmu + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	if (scr0 & 1u << 3) {
		say("snsrpc: the SMMU numbers its streams with 16 bits, which nothing here is written for\n");
		return false;
	}
	space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	space.pages = 2u << (idr1 >> 28 & 7);
	space.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(space.smmu + SMMU_SMR(n)), s2cr = rd(space.smmu + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if (((stream ^ STREAM) & ~mask & ~STREAM_MASK) == 0) {
				say("snsrpc: match register %u covers stream %#x already: stream %#x mask %#x, s2cr %#x\n", n, STREAM,
				    stream, mask, s2cr);
				if ((s2cr >> 16 & 3) == 1) {
					space.passed = true;
					return true;
				}
				say("snsrpc: ... and translates it: somebody's, left alone\n");
				return false;
			}
		} else if (space.match == ~0u) {
			space.match = n;
			space.old_smr = smr;
			space.old_s2cr = s2cr;
		}
	}
	/* (from the top down: shennong_sound.c takes the first free one from the bottom) */
	for (space.context = contexts; space.context > 0; space.context--) {
		if (!used[space.context - 1]) {
			vm_offset_t base = space.smmu + (space.pages + space.context - 1) * space.page;

			if (!(rd(base + SMMU_CB_SCTLR) & 1)) {
				break;
			}
		}
	}
	if (space.match == ~0u || space.context == 0) {
		say("snsrpc: the SMMU has no match register or no context left\n");
		return false;
	}
	space.context--;
	space.context_base = space.smmu + (space.pages + space.context) * space.page;
	space.old_sctlr = rd(space.context_base + SMMU_CB_SCTLR);
	say("snsrpc: stream %#x is nobody's: match register %u and context %u of the SMMU are taken for it\n", STREAM,
	    space.match, space.context);
	return true;
}

static bool
smmu_on(void)
{
	uint64_t *root, *second, *third, table_root;
	uint32_t sees = 1u << 31 | (uint32_t)STREAM_MASK << 16 | STREAM;

	if (kmem_alloc(kernel_map, &space.tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		return false;
	}
	root = (uint64_t *)space.tables;
	second = (uint64_t *)(space.tables + 4096);
	third = (uint64_t *)(space.tables + 8192);
	root[IOVA >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	second[IOVA >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	for (unsigned page = 0; page < REGION_PAGES; page++) {
		third[(IOVA >> 12 & 0x1ff) + page] = (region_phys + 4096 * page) | PTE;
	}
	clean((void *)space.tables, 3 * 4096);
	table_root = kvtophys(space.tables) | (uint64_t)space.context << 48;
	wr(space.context_base + SMMU_CB_SCTLR, 0);
	wr(space.smmu + space.page + SMMU_CBA2R(space.context), 1);
	wr(space.smmu + space.page + SMMU_CBAR(space.context), SMMU_CBAR_STAGE_1);
	wr(space.context_base + SMMU_CB_TCR2, SMMU_TCR2);
	wr(space.context_base + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	wr(space.context_base + SMMU_CB_TTBR0, (uint32_t)table_root);
	wr(space.context_base + SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32));
	wr(space.context_base + SMMU_CB_TTBR1, 0);
	wr(space.context_base + SMMU_CB_TTBR1 + 4, 0);
	wr(space.context_base + SMMU_CB_MAIR0, SMMU_MAIR0);
	wr(space.context_base + SMMU_CB_MAIR1, 0);
	wr(space.context_base + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(space.context_base + SMMU_CB_TLBIASID, space.context);
	wr(space.context_base + SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(space.context_base + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			say("snsrpc: the SMMU does not finish forgetting old translations\n");
			return false;
		}
		sleep_ms(1);
	}
	wr(space.context_base + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	space.on = true;
	wr(space.smmu + SMMU_S2CR(space.match), space.context);
	wr(space.smmu + SMMU_SMR(space.match), sees);
	if (rd(space.smmu + SMMU_SMR(space.match)) != sees || !(rd(space.context_base + SMMU_CB_SCTLR) & 1)) {
		say("snsrpc: THE SMMU DID NOT TAKE WHAT WAS WRITTEN\n");
		return false;
	}
	say("snsrpc: stream %#x goes through context %u with our tables: %u KB of ours at the DSP's %#llx\n", STREAM,
	    space.context, REGION_BYTES / 1024, IOVA);
	return true;
}

static void
smmu_faults(const char *when)
{
	uint32_t status;

	if (!space.on) {
		return;
	}
	status = rd(space.context_base + SMMU_CB_FSR);
	if ((status & SMMU_FSR_ALL) == 0) {
		return;
	}
	wr(space.context_base + SMMU_CB_FSR, status);
	say("snsrpc: %s: THE SMMU REFUSED THE DSP: status %#x, address %#llx, syndrome %#x\n", when, status,
	    (uint64_t)rd(space.context_base + SMMU_CB_FAR + 4) << 32 | rd(space.context_base + SMMU_CB_FAR),
	    rd(space.context_base + SMMU_CB_FSYNR0));
}

static void
smmu_off(void)
{
	if (!space.on) {
		return;
	}
	smmu_faults("at the end");
	wr(space.smmu + SMMU_SMR(space.match), space.old_smr);
	wr(space.smmu + SMMU_S2CR(space.match), space.old_s2cr);
	wr(space.context_base + SMMU_CB_SCTLR, space.old_sctlr);
	space.on = false;
	say("snsrpc: stream %#x and context %u of the SMMU put back as they were\n", STREAM, space.context);
}

static bool
region_up(void)
{
	vm_offset_t va = 0;

	if (kmem_alloc_contig(kernel_map, &va, REGION_BYTES, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("snsrpc: no megabyte of contiguous memory for the DSP's buffers\n");
		return false;
	}
	region = (uint8_t *)va;
	region_phys = kvtophys(va);
	clean(region, REGION_BYTES);
	lck_mtx_lock(&redwood_smmu_lock);
	bool ok = smmu_find() && (space.passed || smmu_on());
	lck_mtx_unlock(&redwood_smmu_lock);
	if (!ok) {
		return false;
	}
	region_dsp_all = space.passed ? region_phys : IOVA;
	return true;
}

/* ---- 2: invocations ------------------------------------------------------------------------------------------- */

#define SNS_DOMAIN              2               /* Linux's SENSORS_PD: the low bits of an invocation's context */
#define OUR_PID                 0x534e          /* our "process" */
#define META_BYTES              4096            /* the first page: the invocation's description */
#define ALIGN                   128

struct arg {
	const void *in;                 /* an input's bytes (NULL: none) */
	void *out;                      /* where an output's go (NULL: dropped) */
	uint32_t length;
	uint32_t offset;                /* (where it lies in the region) */
};

/* two invocations at a time: the listener's (it waits for the DSP's calls) and another, each its part of the region */
#define SLOTS                   2
static struct slot {
	uint32_t offset, bytes;
	volatile uint64_t context;
	volatile int32_t retval;
	volatile bool there;
} slots[SLOTS] = { { 0, 1536 * 1024 }, { 1536 * 1024, 512 * 1024 } };
static uint32_t next_context;
static LCK_MTX_DECLARE(context_lock, &snsrpc_lock_group);

static void
on_answer(uint64_t context, int32_t retval)
{
	for (unsigned i = 0; i < SLOTS; i++) {
		if (slots[i].context == context && !slots[i].there) {
			slots[i].retval = retval;
			slots[i].there = true;
			return;
		}
	}
	say("snsrpc: < an answer to %#llx (%d), not one waited for\n", context, retval);
}

static void *
space_of(struct slot *slot)
{
	return region + slot->offset;
}

/* handle's method with `in' inputs then `out' outputs; the DSP's return value, or INT32_MIN if it did not answer */
static int32_t
invoke_in(unsigned which, uint32_t handle, uint32_t method, uint32_t in, uint32_t out, struct arg *args, uint32_t ms)
{
	struct slot *slot = &slots[which];
	uint8_t *region = (uint8_t *)space_of(slot);
	uint64_t region_dsp = region_dsp_all + slot->offset;
	uint32_t n = in + out, at = META_BYTES, scalars, metadata = n * (16 + 8 + 16) + 16 * 8 + 64 * 4;
	uint64_t *rpra = (uint64_t *)region, *pages;
	uint32_t *list;
	int error;

	if (n > 16 || metadata > META_BYTES) {
		return INT32_MIN;
	}
	bzero(region, META_BYTES);
	list = (uint32_t *)(region + n * 16);
	pages = (uint64_t *)(region + n * 24);
	for (uint32_t i = 0; i < n; i++) {
		at = (at + ALIGN - 1) & ~(ALIGN - 1);
		args[i].offset = at;
		if (at + args[i].length > slot->bytes) {
			say("snsrpc: %u bytes of arguments do not fit\n", at + args[i].length);
			return INT32_MIN;
		}
		if (i < in && args[i].in != NULL) {
			memcpy(region + at, args[i].in, args[i].length);
		} else {
			bzero(region + at, args[i].length);
		}
		rpra[2 * i] = region_dsp + at;
		rpra[2 * i + 1] = args[i].length;
		list[2 * i] = args[i].length != 0;
		list[2 * i + 1] = i;
		pages[2 * i] = ((region_dsp + at) & ~0xfffull) | (space.passed ? 0 : STREAM_BITS);
		pages[2 * i + 1] = (((at & 0xfff) + args[i].length + 0xfff) & ~0xfffu) ?: 4096;
		at += args[i].length;
	}
	clean(region, at);
	scalars = (method & 0x1f) << 24 | in << 16 | out << 8;
	lck_mtx_lock(&context_lock);
	next_context = next_context % 4095 + 1;
	slot->context = (uint64_t)next_context << 4 | SNS_DOMAIN;
	slot->there = false;
	lck_mtx_unlock(&context_lock);
	error = redwood_adsp_fastrpc_send(OUR_PID, OUR_PID + 1 + (int32_t)which, slot->context, handle, scalars,
	    region_dsp | (space.passed ? 0 : STREAM_BITS), (at + 0xfff) & ~0xfffu);
	if (error != 0) {
		say("snsrpc: invocation of %u.%u NOT SENT (%d)\n", handle, method, error);
		return INT32_MIN;
	}
	for (uint32_t waited = 0; !slot->there && !gone && (ms == 0 || waited < ms); waited += 2) {
		sleep_ms(2);
	}
	smmu_faults("an invocation");
	if (!slot->there) {
		if (!gone) {
			say("snsrpc: invocation of %u.%u NOT ANSWERED in %u ms\n", handle, method, ms);
		}
		return INT32_MIN;
	}
	clean(region, at);
	for (uint32_t i = in; i < n; i++) {
		if (args[i].out != NULL) {
			memcpy(args[i].out, region + args[i].offset, args[i].length);
		}
	}
	return slot->retval;
}

static int32_t
invoke(uint32_t handle, uint32_t method, uint32_t in, uint32_t out, struct arg *args, uint32_t ms)
{
	return invoke_in(1, handle, method, in, out, args, ms);
}

/* remotectl_open(name) (handle 0, method 0): true and the handle (any 32 bits) */
static bool
remote_open(const char *name, uint32_t *handle)
{
	uint32_t prim_in[2] = { (uint32_t)strlen(name) + 1, 128 }, prim_out[2] = { 0, 0 };
	char dlerror[128] = "";
	struct arg args[4] = {
		{ .in = prim_in, .length = sizeof(prim_in) }, { .in = name, .length = prim_in[0] },
		{ .out = prim_out, .length = sizeof(prim_out) }, { .out = dlerror, .length = sizeof(dlerror) },
	};
	int32_t retval = invoke(0, 0, 2, 2, args, 30000);

	dlerror[sizeof(dlerror) - 1] = 0;
	say("snsrpc: remotectl_open(\"%s\"): %d, handle %#x, error %d%s%s\n", name, retval, prim_out[0], (int32_t)prim_out[1],
	    dlerror[0] ? ": " : "", dlerror);
	*handle = prim_out[0];
	return retval == 0 && prim_out[1] == 0;
}

static const char *
first_char(const char *s, char c)
{
	for (; *s != 0; s++) {
		if (*s == c) {
			return s;
		}
	}
	return NULL;
}

static bool
contains(const char *s, const char *what)
{
	size_t n = strlen(what);

	for (; *s != 0; s++) {
		if (strncmp(s, what, n) == 0) {
			return true;
		}
	}
	return false;
}

static const char *
last_char(const char *s, char c)
{
	const char *found = NULL;

	for (; *s != 0; s++) {
		if (*s == c) {
			found = s;
		}
	}
	return found;
}

/* ---- 4: the files ------------------------------------------------------------------------------------------- */

struct file {
	const char *path;               /* within the tar: "mnt/vendor/persist/sensors/..." */
	const uint8_t *data;
	uint32_t size;
};
static struct file *files;
static uint32_t file_count;

static uint64_t
octal(const char *s, unsigned length)
{
	uint64_t value = 0;

	for (unsigned i = 0; i < length && s[i] >= '0' && s[i] <= '7'; i++) {
		value = value * 8 + (uint64_t)(s[i] - '0');
	}
	return value;
}

static void
files_load(void)
{
	uint64_t size = 0, at = 0;
	const uint8_t *tar = redwood_wifi_file("sns.tar", &size);
	uint32_t capacity = 512;

	if (tar == NULL) {
		say("snsrpc: NO fw/sns.tar in the small image: no file can be served\n");
		return;
	}
	files = kalloc_data(capacity * sizeof(struct file), Z_WAITOK | Z_ZERO);
	if (files == NULL) {
		return;
	}
	while (at + 512 <= size && tar[at] != 0 && file_count < capacity) {
		const char *h = (const char *)(tar + at);
		uint64_t length = octal(h + 124, 12);
		char type = h[156];

		if (type == '0' || type == 0) {
			char *path = kalloc_data(256, Z_WAITOK | Z_ZERO);

			if (path == NULL) {
				break;
			}
			if (h[345] != 0) {      /* (ustar: a prefix) */
				strlcpy(path, h + 345, 156);
				strlcat(path, "/", 256);
			}
			{
				size_t at = strlen(path);

				for (unsigned i = 0; i < 100 && h[i] != 0 && at < 255; i++) {
					path[at++] = h[i];
				}
				path[at] = 0;
			}
			while (path[0] == '.' && path[1] == '/') {
				memmove(path, path + 2, strlen(path + 2) + 1);
			}
			files[file_count].path = path;
			files[file_count].data = tar + at + 512;
			files[file_count].size = (uint32_t)length;
			file_count++;
		}
		at += 512 + ((length + 511) & ~511ull);
	}
	say("snsrpc: fw/sns.tar: %u files to serve\n", file_count);
}

/* "/a/b" or "a/b" */
static const char *
unrooted(const char *path)
{
	while (*path == '/') {
		path++;
	}
	return path;
}

static int
file_find(const char *path)
{
	path = unrooted(path);
	for (uint32_t i = 0; i < file_count; i++) {
		if (strcmp(files[i].path, path) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* is `path' a directory: some file below it */
static bool
directory(const char *path)
{
	size_t length;

	path = unrooted(path);
	length = strlen(path);
	while (length > 0 && path[length - 1] == '/') {
		length--;
	}
	for (uint32_t i = 0; i < file_count; i++) {
		if (strncmp(files[i].path, path, length) == 0 && files[i].path[length] == '/') {
			return true;
		}
	}
	return length == 0;
}

#define OPEN_MAX_FILES          64
static struct {
	int file;                       /* -1: a file written: dropped */
	uint32_t at;
	bool used;
} open_files[OPEN_MAX_FILES];

#define DIRS_MAX                8
static struct {
	char path[200];
	uint32_t next;                  /* the next of `files' to look at */
	char given[16][100];            /* subdirectories already said */
	uint32_t given_count;
	bool used;
} dirs[DIRS_MAX];

/* ---- the DSP's calls ------------------------------------------------------------------------------------- */

#define H_REMOTECTL             0
#define H_APPS_STD              0x100
#define H_APPS_MEM              0x101
#define ENOSUCHMOD_             ((int32_t)(0x80000400u + 0x3e))
#define EUNSUPPORTED_           ((int32_t)(0x80000400u + 0x14))
#define EBADPARM_               ((int32_t)(0x80000400u + 0x0e))

struct call {
	uint32_t handle, sc, method, ins, outs;
	const uint8_t *in[16];
	uint32_t in_length[16];
	uint8_t *out[16];
	uint32_t out_length[16];
};

static uint32_t
u32(const uint8_t *p, uint32_t offset)
{
	uint32_t value;

	memcpy(&value, p + offset, 4);
	return value;
}

static void
put32(uint8_t *p, uint32_t offset, uint32_t value)
{
	memcpy(p + offset, &value, 4);
}

static void
put64(uint8_t *p, uint32_t offset, uint64_t value)
{
	memcpy(p + offset, &value, 8);
}

/* an input that is a string: at most its length, ended */
static const char *
string(struct call *c, uint32_t i, char *into, uint32_t size)
{
	uint32_t n = i < c->ins ? c->in_length[i] : 0;

	if (n >= size) {
		n = size - 1;
	}
	if (n > 0) {
		memcpy(into, c->in[i], n);
	}
	into[n] = 0;
	return into;
}

static int32_t
std_open(const char *path, const char *how, uint8_t *handle_out)
{
	int file = file_find(path);
	bool writing = how[0] == 'w' || how[0] == 'a' || first_char(how, '+') != NULL;

	if (file < 0 && !writing) {
		return ENOENT;
	}
	for (uint32_t i = 0; i < OPEN_MAX_FILES; i++) {
		if (!open_files[i].used) {
			open_files[i].used = true;
			open_files[i].file = writing ? -1 : file;
			open_files[i].at = 0;
			put32(handle_out, 0, i + 1);
			return 0;
		}
	}
	return EMFILE;
}

static int
std_handle(const uint8_t *p)
{
	uint32_t h = u32(p, 0);

	return h >= 1 && h <= OPEN_MAX_FILES && open_files[h - 1].used ? (int)h - 1 : -1;
}

static int32_t
apps_std(struct call *c, char *note, size_t note_size)
{
	char a[200], b[200], d[200];
	const uint8_t *prim = c->ins > 0 ? c->in[0] : NULL;
	uint8_t *rout = c->outs > 0 ? c->out[0] : NULL;
	uint32_t method = c->method;
	int h;

	if (method == 31 && prim != NULL && c->in_length[0] >= 4) {
		method = u32(prim, 0);  /* (methods from 31 on: the real one in the first word) */
	}
	switch (method) {
	case 0:                 /* fopen(name, mode) -> FILE */
		string(c, 1, a, sizeof(a));
		string(c, 2, b, sizeof(b));
		snprintf(note, note_size, "fopen(\"%s\", \"%s\")", a, b);
		return rout != NULL ? std_open(a, b, rout) : EBADPARM_;
	case 19: {              /* fopen_with_env(env, delim, name, mode) -> FILE */
		int32_t result = ENOENT;

		string(c, 1, a, sizeof(a));
		string(c, 3, b, sizeof(b));
		string(c, 4, d, sizeof(d));
		snprintf(note, note_size, "fopen_with_env(%s, \"%s\", \"%s\")", a, b, d);
		if (rout == NULL) {
			return EBADPARM_;
		}
		if (b[0] == '/') {
			return std_open(b, d, rout);
		}
		/* a relative name: any file of ours with it as its last part */
		for (uint32_t i = 0; i < file_count; i++) {
			const char *last = last_char(files[i].path, '/');

			if (strcmp(last != NULL ? last + 1 : files[i].path, b) == 0) {
				result = std_open(files[i].path, d, rout);
				break;
			}
		}
		return result;
	}
	case 3:                 /* fclose(FILE) */
	case 2:                 /* fflush */
	case 22:                /* fsync */
		h = prim != NULL ? std_handle(prim) : -1;
		snprintf(note, note_size, "%s(%d)", method == 3 ? "fclose" : "fflush", h + 1);
		if (h < 0) {
			return EBADF;
		}
		if (method == 3) {
			open_files[h].used = false;
		}
		return 0;
	case 4: {               /* fread(FILE, buf[len]) -> read, eof */
		uint32_t want, have;

		h = prim != NULL ? std_handle(prim) : -1;
		want = prim != NULL ? u32(prim, 4) : 0;
		snprintf(note, note_size, "fread(%d, %u)", h + 1, want);
		if (h < 0 || c->outs < 2 || open_files[h].file < 0) {
			return EBADF;
		}
		have = files[open_files[h].file].size - open_files[h].at;
		if (want > have) {
			want = have;
		}
		if (want > c->out_length[1]) {
			want = c->out_length[1];
		}
		memcpy(c->out[1], files[open_files[h].file].data + open_files[h].at, want);
		open_files[h].at += want;
		put32(rout, 0, want);
		put32(rout, 4, open_files[h].at >= files[open_files[h].file].size);
		return 0;
	}
	case 5:                 /* fwrite(FILE, buf) -> written, eof: said and dropped */
		h = prim != NULL ? std_handle(prim) : -1;
		snprintf(note, note_size, "fwrite(%d, %u bytes): dropped", h + 1, c->ins > 1 ? c->in_length[1] : 0);
		if (h < 0 || rout == NULL) {
			return EBADF;
		}
		put32(rout, 0, c->ins > 1 ? c->in_length[1] : 0);
		put32(rout, 4, 0);
		return 0;
	case 8:                 /* ftell(FILE) -> pos */
	case 12:                /* feof(FILE) -> eof */
	case 13:                /* ferror(FILE) -> 0 */
		h = prim != NULL ? std_handle(prim) : -1;
		snprintf(note, note_size, "%s(%d)", method == 8 ? "ftell" : method == 12 ? "feof" : "ferror", h + 1);
		if (h < 0 || rout == NULL) {
			return EBADF;
		}
		put32(rout, 0, method == 8 ? open_files[h].at : method == 12 ?
		    (open_files[h].file >= 0 && open_files[h].at >= files[open_files[h].file].size) : 0);
		return 0;
	case 9: {               /* fseek(FILE, offset, whence) */
		int32_t offset = prim != NULL ? (int32_t)u32(prim, 4) : 0;
		uint32_t whence = prim != NULL ? u32(prim, 8) : 0, size;
		int64_t to;

		h = prim != NULL ? std_handle(prim) : -1;
		snprintf(note, note_size, "fseek(%d, %d, %u)", h + 1, offset, whence);
		if (h < 0) {
			return EBADF;
		}
		size = open_files[h].file >= 0 ? files[open_files[h].file].size : 0;
		to = (whence == 0 ? 0 : whence == 1 ? (int64_t)open_files[h].at : (int64_t)size) + offset;
		if (to < 0) {
			return EINVAL;
		}
		open_files[h].at = (uint32_t)to;
		return 0;
	}
	case 10:                /* flen(FILE) -> uint64 */
		h = prim != NULL ? std_handle(prim) : -1;
		snprintf(note, note_size, "flen(%d)", h + 1);
		if (h < 0 || rout == NULL) {
			return EBADF;
		}
		put64(rout, 0, open_files[h].file >= 0 ? files[open_files[h].file].size : 0);
		return 0;
	case 11:                /* rewind */
		h = prim != NULL ? std_handle(prim) : -1;
		snprintf(note, note_size, "rewind(%d)", h + 1);
		if (h < 0) {
			return EBADF;
		}
		open_files[h].at = 0;
		return 0;
	case 15:                /* print_string(s) */
		snprintf(note, note_size, "print_string: %s", string(c, 1, a, sizeof(a)));
		return 0;
	case 26: {              /* opendir(name) -> DIR */
		string(c, 1, a, sizeof(a));
		snprintf(note, note_size, "opendir(\"%s\")", a);
		if (rout == NULL) {
			return EBADPARM_;
		}
		if (!directory(a)) {
			return ENOENT;
		}
		for (uint32_t i = 0; i < DIRS_MAX; i++) {
			if (!dirs[i].used) {
				bzero(&dirs[i], sizeof(dirs[i]));
				dirs[i].used = true;
				strlcpy(dirs[i].path, unrooted(a), sizeof(dirs[i].path));
				while (strlen(dirs[i].path) > 0 && dirs[i].path[strlen(dirs[i].path) - 1] == '/') {
					dirs[i].path[strlen(dirs[i].path) - 1] = 0;
				}
				put64(rout, 0, i + 1);
				return 0;
			}
		}
		return EMFILE;
	}
	case 27:                /* closedir(DIR) */
	case 28: {              /* readdir(DIR) -> dirent { ino, name[255] }, eof */
		uint64_t which = 0;
		uint32_t i;

		if (prim != NULL && c->in_length[0] >= 8) {
			memcpy(&which, prim, 8);
		}
		i = (uint32_t)which - 1;
		if (which == 0 || which > DIRS_MAX || !dirs[i].used) {
			snprintf(note, note_size, "%s(%llu): no such", method == 27 ? "closedir" : "readdir", which);
			return EBADF;
		}
		if (method == 27) {
			snprintf(note, note_size, "closedir(%llu)", which);
			dirs[i].used = false;
			return 0;
		}
		if (rout == NULL || c->out_length[0] < 264) {
			return EBADPARM_;
		}
		bzero(rout, 264);
		for (; dirs[i].next < file_count; dirs[i].next++) {
			const char *p = files[dirs[i].next].path;
			size_t length = strlen(dirs[i].path);
			char name[100];
			const char *slash;
			bool seen = false;

			if (length > 0 && (strncmp(p, dirs[i].path, length) != 0 || p[length] != '/')) {
				continue;
			}
			p += length > 0 ? length + 1 : 0;
			slash = first_char(p, '/');
			strlcpy(name, p, slash != NULL && (size_t)(slash - p) + 1 < sizeof(name) ? (size_t)(slash - p) + 1 :
			    sizeof(name));
			if (slash != NULL) {
				for (uint32_t g = 0; g < dirs[i].given_count; g++) {
					seen |= strcmp(dirs[i].given[g], name) == 0;
				}
				if (seen) {
					continue;
				}
				if (dirs[i].given_count < 16) {
					strlcpy(dirs[i].given[dirs[i].given_count++], name, 100);
				}
			}
			put32(rout, 0, dirs[i].next + 1);
			strlcpy((char *)rout + 4, name, 255);
			dirs[i].next++;
			snprintf(note, note_size, "readdir(%llu): %s", which, name);
			return 0;
		}
		put32(rout, 260, 1);
		snprintf(note, note_size, "readdir(%llu): the end", which);
		return 0;
	}
	case 29:                /* mkdir */
		snprintf(note, note_size, "mkdir(\"%s\"): said done", string(c, 1, a, sizeof(a)));
		return 0;
	case 24:                /* fremove */
		snprintf(note, note_size, "fremove(\"%s\"): said done", string(c, 1, a, sizeof(a)));
		return 0;
	case 33:                /* frename(old, new) (method 31's) */
		snprintf(note, note_size, "frename: said done");
		return 0;
	case 31 + 0:            /* (not reached: 31 is taken apart above) */
	default:
		break;
	}
	/* stat (31): the first word 31, then the name's length; the name; out: a STAT of 96 bytes */
	if (c->method == 31 && method == 31) {
		int file;

		string(c, 1, a, sizeof(a));
		snprintf(note, note_size, "stat(\"%s\")", a);
		if (rout == NULL || c->out_length[0] < 96) {
			return EBADPARM_;
		}
		bzero(rout, 96);
		file = file_find(a);
		if (file >= 0) {
			put32(rout, 24, 0100644);
			put32(rout, 28, 1);
			put64(rout, 40, files[file].size);
			return 0;
		}
		if (directory(a)) {
			put32(rout, 24, 040755);
			put32(rout, 28, 2);
			return 0;
		}
		return ENOENT;
	}
	snprintf(note, note_size, "apps_std method %u: NOT SERVED", method);
	return EUNSUPPORTED_;
}

static int32_t
dispatch(struct call *c, char *note, size_t note_size)
{
	char a[100];

	if (c->handle == H_REMOTECTL) {
		uint8_t *rout = c->outs > 0 ? c->out[0] : NULL;

		if (c->method == 0) {   /* open(name) -> handle, error; dlerror */
			string(c, 1, a, sizeof(a));
			if (rout == NULL) {
				return EBADPARM_;
			}
			put32(rout, 0, 0);
			if (strcmp(a, "apps_std") == 0) {
				put32(rout, 0, H_APPS_STD);
				put32(rout, 4, 0);
			} else {
				put32(rout, 4, (uint32_t)ENOSUCHMOD_);
				if (c->outs > 1 && c->out_length[1] > 0) {
					strlcpy((char *)c->out[1], "not served by the kernel", c->out_length[1]);
				}
			}
			snprintf(note, note_size, "apps_remotectl_open(\"%s\")%s", a, strcmp(a, "apps_std") == 0 ? "" :
			    ": NOT SERVED");
			return 0;
		}
		if (c->method == 1) {   /* close(handle) -> error */
			snprintf(note, note_size, "apps_remotectl_close(%#x)", c->ins > 0 ? u32(c->in[0], 0) : 0);
			if (rout != NULL) {
				put32(rout, 0, 0);
			}
			return 0;
		}
	} else if (c->handle == H_APPS_STD) {
		return apps_std(c, note, note_size);
	}
	snprintf(note, note_size, "handle %#x method %u: NOT SERVED", c->handle, c->method);
	return ENOSUCHMOD_;
}

/* ---- 3, 4: the listener --------------------------------------------------------------------------------------- */

#define BUFS_BYTES              (256u * 1024)

static uint8_t *in_bufs, *out_bufs;

static void
hexline(const char *what, const uint8_t *p, uint32_t length)
{
	char line[3 * 32 + 1];
	uint32_t n = length < 32 ? length : 32;

	for (uint32_t i = 0; i < n; i++) {
		snprintf(line + 3 * i, 4, " %02x", p[i]);
	}
	line[3 * n] = 0;
	say("snsrpc:     %s (%u):%s\n", what, length, line);
}

static void
default_listener_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t handle = 0;
	int32_t retval;

	if (remote_open("adsp_default_listener", &handle)) {
		struct arg none[1];

		retval = invoke(handle, 0, 0, 0, none, 30000);     /* register */
		say("snsrpc: adsp_default_listener_register: %d (%#x)\n", retval, (uint32_t)retval);
	}
	thread_terminate(current_thread());
}

static void
listener(void)
{
	int32_t listener_handle, default_handle, retval;
	uint32_t prev_context = 0, prev_length = 0, calls = 0, served = 0;
	int32_t prev_result = -1;

	listener_handle = 3;    /* (adsp_listener's handle is a constant one: _const_adsp_listener_handle) */
	{
		struct arg none[1];
		thread_t thread;

		retval = invoke_in(0, (uint32_t)listener_handle, 3, 0, 0, none, 5000);    /* init2 */
		say("snsrpc: adsp_listener_init2: %d (%#x)\n", retval, (uint32_t)retval);
		if (retval != 0) {
			return;
		}
		/* the default listener registered meanwhile: opening it may need files from us, through the listener */
		if (kernel_thread_start(default_listener_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	(void)default_handle;
	for (;;) {
		uint32_t prim_in[4] = { prev_context, (uint32_t)prev_result, prev_length, BUFS_BYTES };
		uint32_t prim_out[4] = { 0, 0, 0, 0 };
		struct arg args[4] = {
			{ .in = prim_in, .length = sizeof(prim_in) }, { .in = out_bufs, .length = prev_length },
			{ .out = prim_out, .length = sizeof(prim_out) }, { .out = in_bufs, .length = BUFS_BYTES },
		};
		struct call c;
		char note[300] = "";
		uint32_t at = 0, need, sc;

		retval = invoke_in(0, (uint32_t)listener_handle, 4, 2, 2, args, 0);     /* next2: as long as it takes */
		if (gone) {
			return;
		}
		if (retval != 0) {
			say("snsrpc: adsp_listener_next2: %d (%#x): the listener stops\n", retval, (uint32_t)retval);
			return;
		}
		bzero(&c, sizeof(c));
		c.handle = prim_out[1];
		sc = c.sc = prim_out[2];
		c.method = sc >> 24 & 0x1f;
		c.ins = sc >> 16 & 0xff;
		c.outs = sc >> 8 & 0xff;
		need = prim_out[3];
		prev_context = prim_out[0];
		prev_length = 0;
		calls++;
		last_call = mach_absolute_time();
		called = true;
		if (need > BUFS_BYTES || c.ins > 16 || c.outs > 16 || (sc & 0xff) != 0) {
			say("snsrpc: call %u: handle %#x sc %#x, %u bytes: NOT TAKEN\n", calls, c.handle, sc, need);
			prev_result = EBADPARM_;
			continue;
		}
		/* the inputs: each its length, then (at 8 bytes' alignment) its bytes; then the outputs' lengths */
		for (uint32_t i = 0; i < c.ins; i++) {
			c.in_length[i] = u32(in_bufs, at);
			at += 4;
			if (c.in_length[i] != 0) {
				at = (at + 7) & ~7u;
				c.in[i] = in_bufs + at;
				at += c.in_length[i];
			}
		}
		for (uint32_t i = 0; i < c.outs; i++) {
			c.out_length[i] = u32(in_bufs, at);
			at += 4;
		}
		if (at > need) {
			say("snsrpc: call %u: its buffers run past what it said (%u > %u)\n", calls, at, need);
			prev_result = EBADPARM_;
			continue;
		}
		/* the outputs: laid out as they go back (each its length, then its bytes at 8 bytes' alignment) */
		at = 0;
		for (uint32_t i = 0; i < c.outs; i++) {
			put32(out_bufs, at, c.out_length[i]);
			at += 4;
			if (c.out_length[i] != 0) {
				at = (at + 7) & ~7u;
				if (at + c.out_length[i] > BUFS_BYTES) {
					break;
				}
				c.out[i] = out_bufs + at;
				bzero(c.out[i], c.out_length[i]);
				at += c.out_length[i];
			}
		}
		if (at > BUFS_BYTES) {
			say("snsrpc: call %u: %u bytes of outputs do not fit\n", calls, at);
			prev_result = EBADPARM_;
			continue;
		}
		prev_result = dispatch(&c, note, sizeof(note));
		prev_length = prev_result == 0 ? at : 0;
		served += prev_result == 0;
		say("snsrpc: call %u: %s: %d\n", calls, note, prev_result);
		if (mode >= 2 || (prev_result != 0 && contains(note, "NOT SERVED"))) {
			for (uint32_t i = 0; i < c.ins; i++) {
				hexline("in", c.in[i] != NULL ? c.in[i] : (const uint8_t *)"", c.in_length[i]);
			}
		}
	}
}

static void
snsrpc_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct arg args[1];
	int32_t pid = OUR_PID, retval;

	in_bufs = kalloc_data(BUFS_BYTES, Z_WAITOK | Z_ZERO);
	out_bufs = kalloc_data(BUFS_BYTES, Z_WAITOK | Z_ZERO);
	files_load();
	if (in_bufs == NULL || out_bufs == NULL || !region_up()) {
		say("snsrpc: NOT STARTED\n");
		settled = true;
		thread_terminate(current_thread());
		return;
	}
	redwood_adsp_fastrpc_answers(SNS_DOMAIN, on_answer);
	args[0] = (struct arg){ .in = &pid, .length = 4 };
	retval = invoke(1, 0, 1, 0, args, 5000);         /* attach to the sensors' process */
	say("snsrpc: attach sensors (process %#x): %d (%#x)\n", OUR_PID, retval, (uint32_t)retval);
	if (retval == 0) {
		last_call = mach_absolute_time();
		listener();
	}
	settled = true;
	thread_terminate(current_thread());
}

/* the DSP is up and linked (bsd/dev/redwood_adsp.c) */
void
shennong_snsrpc_dsp_up(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("snsrpc", &mode, sizeof(mode)) || mode == 0) {
		settled = true;
		return;
	}
	say("snsrpc: snsrpc=%u: the sensors' domain is to be served its files over FastRPC\n", mode);
	if (kernel_thread_start(snsrpc_thread, NULL, &thread) != KERN_SUCCESS) {
		settled = true;
		return;
	}
	thread_deallocate(thread);
}

/* before shennong_sensors.c's lookups: up to snsrpc_wait seconds for the files to have been read (nothing asked
 * for in two seconds) */
void
shennong_snsrpc_wait(void)
{
	uint32_t seconds = 20;

	if (mode == 0) {
		return;
	}
	PE_parse_boot_argn("snsrpc_wait", &seconds, sizeof(seconds));
	for (uint32_t waited = 0; waited < seconds * 10 && !settled && shennong_snsrpc_quiet_ms() < 2000; waited++) {
		sleep_ms(100);
	}
	say("snsrpc: the lookups go on (%s)\n", settled ? "the listener has stopped" : "nothing asked for 2 s, or time is up");
}

uint32_t
shennong_snsrpc_quiet_ms(void)
{
	uint64_t nanoseconds;

	if (last_call == 0 || !called) {
		return 0;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - last_call, &nanoseconds);
	return (uint32_t)(nanoseconds / NSEC_PER_MSEC);
}

/* the DSP is about to be shut down */
void
shennong_snsrpc_dsp_down(void)
{
	if (mode == 0) {
		return;
	}
	gone = true;
	lck_mtx_lock(&redwood_smmu_lock);
	smmu_off();
	lck_mtx_unlock(&redwood_smmu_lock);
}
