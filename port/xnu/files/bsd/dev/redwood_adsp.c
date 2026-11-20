/*
 * The audio DSP of the POCO X5 Pro (redwood), started, and spoken to: step 1 of sound. Every
 * address below belongs to this phone, and nothing here is touched unless a boot-arg asks for it.
 *
 * The SoC's audio (LPASS: its clocks, its I2S and SoundWire, the codec's macros) does not belong
 * to the application processor. It is the audio DSP's -- a Hexagon of its own inside LPASS,
 * Android's "adsp" -- which stays in reset until somebody loads it, exactly as the Wi-Fi's does
 * (redwood_wifi.c): the phone's own signed image, adsp.mdt and adsp.b02 to adsp.b36 from
 * /vendor/firmware_mnt/image of its Android (20 MB: in the system image under /fw, or with ADSP=1 in
 * the small one, see image_file()), goes into the 64 MB reserved for it, TrustZone is
 * asked to authenticate and start it, and from there everything is conversation through the
 * shared memory: SMP2P for "up", "given up", "stopped"; GLINK for channels; QRTR on the channel
 * "IPCRTR" for the services the DSP offers. (Reading an LPASS clock register from here ends the
 * kernel with an external abort: the DSP owns that block, and talks about it only this way.)
 * What audio needs of it later -- the audio services, the codec, the speaker's path -- is for the
 * steps after this one; this file only brings the DSP up and the link with it.
 *
 * The way is the Wi-Fi DSP's, taken over from redwood_wifi.c with the audio DSP's numbers: its
 * comments say why each thing is done so. What is different is said here:
 *   - processor 2 (not 13) in the shared memory; its SMP2P items 443 (its) and 429 (ours), its
 *     last words in item 423; GLINK's three items in our partition with it
 *   - its doorbells are IPCC client 3 (LPASS): signal 0 GLINK, signal 2 SMP2P
 *   - TrustZone knows it as image 1; its memory is 0x86700000, 64 MB (dt: qcom,lpass@3700000,
 *     reserved-memory adsp@86700000), every byte of which the image takes
 *   - the always-on processor is told of it as "adsp"
 *   - the Wi-Fi DSP may be running at the same time (wifi=1): the mailbox to the always-on
 *     processor and the lock for allocating in the shared memory are the two things both use,
 *     each behind a lock both take (redwood_wifi.c); the doorbells are only counted here, and
 *     only the audio DSP's are cleared from here
 * The code is a copy, not a shared library, for now: step 1 is not to change what the Wi-Fi does.
 *
 * Boot-arg adsp_step=N performs steps 1 to N and stops; as with wifi_step, a step says what it is
 * about to touch before it touches it, and a step that does not find what it needs ends the
 * sequence. adsp=1 is all of them, on a thread beside the system's start.
 *
 *   1  reads   the shared memory: our partition with the DSP, its items, whether it has run
 *              since the phone was started (its SMP2P item is there if so), its last words
 *   2  reads   the firmware in the small image: is it whole, does it fit the reserved memory
 *   3  writes  the always-on processor's mailbox open, unless it is (the Wi-Fi may have opened it)
 *   4  asks    TrustZone to take the image's header (image 1) and where the image will be; a
 *              DSP that a kernel before this one left running is asked to stop and shut down first
 *   5  writes  the image into the DSP's memory, and reads it back
 *   6  writes  our SMP2P item (429) into the shared memory, and our two marks of GLINK to zero
 *   7  asks    RPMh for the DSP's rails, writes "adsp: on" to the always-on processor, asks
 *              TrustZone to authenticate and start the DSP, and watches for its first words
 *              (answering in our item what SMP2P has us answer)
 *   8  reads   GLINK's queues
 *   9  writes  our side of GLINK: the versions, the channel "IPCRTR" opened both ways, our intents;
 *              from here a thread of ours listens to the DSP
 *  10  talks   QRTR on that channel: hello both ways, and the services the DSP tells of, written
 *              down for adsp_listen seconds (default 3)
 *  11  talks   APR, on the channel apr_audio_svc that the DSP opens and we answer as well: whether
 *              the audio services are up, and which there are (see step_apr())
 *
 * Once everything is up with adsp=1, the sound's path takes over (redwood_sound.c, boot-arg sound=N):
 * redwood_adsp_apr() is how it speaks to the audio services (any service, port and token; what
 * nobody waits for goes to its handler of events), and it is stopped before the DSP is.
 *
 * What is started is stopped: when the steps are over (unless adsp_keep=1, and then when the run
 * ends), and on a panic's way back to the loader. The DSP is asked first (bit 0 of our entry
 * "master-kernel"), then TrustZone shuts it down, then the always-on processor hears "adsp: off".
 * A DSP that a kernel before this one left running is found by its item and treated the same way
 * before this one is started; one that was shut down without having said it stopped is not
 * started again unless adsp_again=1 (the Wi-Fi DSP reset the phone, twice, when that was tried).
 *
 * More boot-args: adsp_new=K (steps K on have not been seen to work, and wait adsp_pause=MS after
 * each announcement, default 2200: what the loader needs to note the end of the log), adsp_wait=S
 * (how long step 7 watches for "up", default 10), adsp_keep=1, adsp_again=1, adsp_return=1 (back
 * to the loader when the steps are over).
 *
 * Nothing here writes a clock or changes a pin. One request goes to RPMh, as Linux makes it: the
 * DSP's two rails, LCX and LMX, at level 384 while it boots, given back when it says it has asked
 * for its own (see rails()).
 *
 * Addresses are from the phone's own device tree (qcom,lpass@3700000, qcom,smp2p-adsp,
 * qcom,glink/adsp, qcom,ipcc@408000, power-controller@c300000, reserved-memory), layouts from
 * Linux (see redwood_wifi.c for the files).
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

/* What the Wi-Fi's DSP and this one both use (redwood_wifi.c). */
extern lck_mtx_t redwood_aop_mailbox_lock;
extern lck_mtx_t redwood_smem_alloc_lock;

static LCK_GRP_DECLARE(adsp_lock_group, "redwood-adsp");

#define STEPS                   11
#define REDWOOD_SOC_BASE        0x00400000ul    /* where the loader says the phone's peripherals begin */

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * Every line goes to the log the loader reads from another core as well (as in redwood_wifi.c);
 * with adsp=1 only the few that say how it went (tell()), the rest to the kernel's own log.
 */
extern void loader_log_putc(char c);

static LCK_MTX_DECLARE(say_lock, &adsp_lock_group);
static bool quiet;

static void
say_line(const char *line, bool aloud)
{
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (!quiet || aloud) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

static int say(const char *format, ...) __printflike(1, 2);
static int tell(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return length;
}

static int
tell(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return length;
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

/* ---- register access (as in redwood_wifi.c) ------------------------------------------------------- */

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

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(void)
{
	if (step >= new_from && pause_ms && !leaving) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("adsp: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	value = rd(b->base + offset);
	printf("adsp: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("adsp: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the shared memory (redwood_wifi.c, step 1) --------------------------------------------------- */

#define SMEM_PHYS               smem_phys
static unsigned long smem_phys = 0x80900000UL;  /* shennong: see board_shennong() */
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_ADSP               2               /* dt: qcom,remote-pid of qcom,smp2p-adsp and qcom,glink/adsp */
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};

/*
 * The DSP's items (dt: qcom,smem of qcom,smp2p-adsp, <443 429>: the item the DSP writes first, then
 * ours -- the Wi-Fi's <617 616> are in the same order; taken the other way round, the DSP made
 * our item its own and never said "up" --, and qcom,smem-id of qcom,lpass@3700000).
 */
#define ITEM_SMP2P_OURS         429
#define ITEM_SMP2P_THEIRS       443
#define ITEM_CRASH_REASON       423

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

static void
smem_poke(uint32_t offset, uint32_t value)
{
	wr((vm_offset_t)smem + offset, value);
}

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the DSP and we share: in the partition of the two, or else in the global one. Where it is; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_ADSP }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

static void
show_partition(void)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp: %u: there is no partition for us and the audio DSP (0 and 2)\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("adsp: %u: the partition for us and the audio DSP (0 and 2): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
	    SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("adsp: %u:   items (bytes):%s\n", step, line);
	}
}

/* ---- SMP2P ---------------------------------------------------------------------------------------- */

struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* The bits of the DSP's entry "slave-kernel" (dt: the interrupts of qcom,lpass@3700000). */
#define DSP_FATAL               (1u << 0)
#define DSP_READY               (1u << 1)
#define DSP_HANDOVER            (1u << 2)
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* Our entry (dt: the child "master-kernel" of qcom,smp2p-adsp; "qcom,force-stop" of qcom,lpass@3700000). */
#define OUR_ENTRY_STOP          "master-kernel"
#define DSP_STOP                (1u << 0)

static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t
dsp_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("adsp: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("adsp: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("adsp: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("adsp: %u: THE AUDIO DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* ---- the doorbells, and the mailbox to the always-on processor (redwood_wifi.c, steps 2 and 7) ------ */

#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64

static struct block aoss = { "mailbox", 0x0c300000, 0x400 };

#define IPCC_SEND               0x0c
#define IPCC_RECEIVED           0x10
#define IPCC_ENABLE             0x14
#define IPCC_DISABLE            0x18
#define IPCC_CLEAR              0x1c
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_LPASS              3u              /* dt: mboxes of qcom,smp2p-adsp and qcom,glink/adsp */
#define IPCC_GLINK              0u
#define IPCC_SMP2P              2u

static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };

/*
 * shennong: the IPCC's interrupt (SPI 229, interrupt 261 = 0x105) is left enabled at the GIC's distributor by the
 * boot loader, and nothing here takes it -- we listen by polling. Raised by a doorbell, it made TrustZone answer
 * "interrupted" to every call while it stood, and the boot loader say "Spurious GIC interrupt: 0x105" without end
 * after us. So it is turned off at the distributor (GICD_ICENABLER) before the doorbells are listened for.
 */
static struct block gicd = { "GIC distributor", 0x17100000, 0x10000 };
static uint32_t ipcc_interrupt;         /* 0: left as it is (redwood) */
#define GICD_ICENABLER(n)       (0x180 + 4 * ((n) / 32))
#define GICD_ISENABLER(n)       (0x100 + 4 * ((n) / 32))

static void
ipcc_interrupt_off(void)
{
	if (ipcc_interrupt == 0) {
		return;
	}
	need(&gicd);
	if (rd(gicd.base + GICD_ISENABLER(ipcc_interrupt)) & (1u << (ipcc_interrupt % 32))) {
		put(&gicd, "the IPCC's interrupt off", GICD_ICENABLER(ipcc_interrupt), 1u << (ipcc_interrupt % 32));
	}
}

static struct {
	uint32_t glink, smp2p, said;
	uint64_t since;
} doorbell;

/*
 * The audio DSP's rings, counted and cleared. Another's ring (the Wi-Fi DSP's, the always-on
 * processor's) is left for whoever listens for it: IPCC names the same one until it is cleared,
 * so the look ends there.
 */
static void
doorbells(void)
{
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = rd(ipcc.base + IPCC_RECEIVED);

		if (who == IPCC_NOBODY || who >> 16 != IPCC_LPASS) {
			return;
		}
		wr(ipcc.base + IPCC_CLEAR, who);
		doorbell.glink += (who & 0xffff) == IPCC_GLINK;
		doorbell.smp2p += (who & 0xffff) == IPCC_SMP2P;
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("adsp: doorbell: +%llu.%03llu ms: signal %u of the audio DSP; heard and cleared\n", now / 1000, now % 1000,
			    who & 0xffff);
		}
	}
}

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: not open */

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring_aop(void)
{
	wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
}

/* Linux's qmp_open(), as redwood_wifi.c does it; what is done already is found done. Under the mailbox's lock. */
static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	need(&ipcc);
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("adsp: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
		printf("adsp: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("adsp: %u: the mailbox is open already (the Wi-Fi's start, or a kernel before this one, opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	printf("adsp: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()), under the lock the Wi-Fi's start takes too. */
static bool
mailbox_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool sent = false;

	if (!leaving) {
		lck_mtx_lock(&redwood_aop_mailbox_lock);
	}
	if (our_box == 0 && (leaving || !mailbox_open())) {
		goto out;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("adsp: %u: about to tell the always-on processor \"%s\"\n", step, text);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	if (rd(aoss.base + our_box) != 0) {
		printf("adsp: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	/*
	 * shennong: the first message after the mailbox was opened by this kernel is not taken (twice so after a
	 * reboot), and with the mailbox opened by a kernel before, it is at once. So a message not taken is taken back
	 * and written and rung for again, three times in all.
	 */
	for (unsigned attempt = 1;; attempt++) {
		wr(aoss.base + our_box, QMP_MESSAGE);
		(void)rd(aoss.base + our_box);
		ring_aop();
		if (mailbox_wait(our_box, 0, 1000)) {
			break;
		}
		wr(aoss.base + our_box, 0);
		if (attempt == 3 || leaving) {
			printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second, %u times; it is taken back\n",
			    step, attempt);
			goto out;
		}
		printf("adsp: %u: the always-on processor has not taken it within a second (attempt %u): written again\n", step,
		    attempt);
		sleep_us(100000);
	}
	printf("adsp: %u: the always-on processor has taken it\n", step);
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&redwood_aop_mailbox_lock);
	}
	return sent;
}

/* ---- RPMh: the DSP's two rails, asked for while it boots ------------------------------------------------ */

/*
 * The audio DSP's logic and memories run from LPASS's own rails, LCX and LMX (dt: vdd_cx and
 * vdd_mx of qcom,lpass@3700000 are PM7325 L5 and L4 "level" regulators, RPMh resources lcx.lvl and
 * lmx.lvl), which the boot loader leaves at retention (16). Linux and Android ask for level 384
 * while the DSP boots, until it says it has asked for its own (bit 2, "proxy-unvote"); without it
 * the DSP rang once and left "SFR Init: wdog or kernel error suspected." in its last words.
 *
 * A request goes to RPMh through the application processor's sequencer (dt: rsc@18200000, DRV 2,
 * qcom,tcs-offset 0xd00, qcom,tcs-config: TCSs 0 and 1 are for requests made at once), as Linux's
 * rpmh-rsc.c writes one: the command's message id, address and value into a TCS that is idle, the
 * command enabled and marked to be waited for, the TCS put in "active" mode and triggered, and its
 * bit in the status of completions looked for. A rail asked for by level is asked by position in
 * the list of its levels that the boot loader's command database keeps (cmd-db.c), the first
 * position at or above the level wanted.
 */
#define CMD_DB_PHYS             cmd_db_phys
static unsigned long cmd_db_phys = 0x80860000UL;
#define CMD_DB_SIZE             0x20000UL

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

static const struct cmd_db_header *cmd_db;

static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_TCS_BASE            0xd00
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)   /* a bit per TCS: done */
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
/* (SM8650's sequencer is version 3: its TCS's registers lie further on, Linux's rsc_reg_offset_ver_3_0) */
static bool rsc_v3;
#define RSC_TCS(n)              (RSC_TCS_BASE + (rsc_v3 ? 336u : 672u) * (n))
#define TCS_WAIT_FOR_DONE       (rsc_v3 ? 0x20u : 0x10u)
#define TCS_CONTROL             (rsc_v3 ? 0x24u : 0x14u)
#define TCS_STATUS              (rsc_v3 ? 0x28u : 0x18u)        /* not zero: idle */
#define TCS_ENABLED             (rsc_v3 ? 0x2cu : 0x1cu)
#define TCS_COMMAND(c)          ((rsc_v3 ? 0x34u : 0x30u) + (rsc_v3 ? 24u : 20u) * (c))        /* message id, address, value, status */
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)       /* four bytes, an answer wanted, a write */
#define RPMH_TCS                0                       /* the first of the two for requests made at once */

/* The level position RPMh takes for that rail and level; -1: not found. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	char id[8] = { 0 };

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		printf("adsp: %u: the boot loader's command database is not there\n", step);
		return -1;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;
			char levels[160];
			int at;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return -1;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			at = snprintf(levels, sizeof(levels), "%s at %#x, levels:", name, entry[i].address);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				unsigned value = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && value == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", value);
			}
			printf("adsp: %u: RPMh has %s\n", step, levels);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				if ((uint32_t)(data[2 * l] | data[2 * l + 1] << 8) >= level) {
					*address = entry[i].address;
					return (int)l;
				}
			}
			return -1;
		}
	}
	printf("adsp: %u: RPMh has no \"%s\"\n", step, name);
	return -1;
}

/* The two rails at that level (Linux's proxy request), or given back (level 0). True: RPMh has said done. */
static bool
rails(uint32_t level)
{
	static const char *const names[2] = { "lcx.lvl", "lmx.lvl" };
	uint32_t addresses[2] = { 0, 0 }, values[2] = { 0, 0 }, done = 0;
	uint64_t began;

	for (unsigned r = 0; r < 2; r++) {
		int position = level == 0 ? 0 : rail_position(names[r], level, &addresses[r]);

		if (level == 0) {
			rail_position(names[r], 0, &addresses[r]);
		}
		if (position < 0 || addresses[r] == 0) {
			printf("adsp: %u: no position for %s at level %u: the rails are not asked\n", step, names[r], level);
			return false;
		}
		values[r] = (uint32_t)position;
	}
	need(&rsc);
	/* (the boot loader leaves the trigger set after its last request, done: idle is what the status says) */
	if (rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS) == 0) {
		printf("adsp: %u: TCS %u of the sequencer is busy (status %#x, control %#x): the rails are not asked\n", step, RPMH_TCS,
		    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL));
		return false;
	}
	printf("adsp: %u: about to ask RPMh, through TCS %u, for lcx.lvl (%#x) = %u and lmx.lvl (%#x) = %u (level %u%s)\n", step,
	    RPMH_TCS, addresses[0], values[0], addresses[1], values[1], level, level == 0 ? ": given back" : "");
	snprintf(last_write, sizeof(last_write), "step %u asked RPMh for the audio DSP's rails at level %u", step, level);
	hold();
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	for (unsigned r = 0; r < 2; r++) {
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 4, addresses[r]);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_WAIT_FOR_DONE, 3);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 3);
	settle();
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_TRIGGER);
	began = now_us();
	while (now_us() - began < 100000) {
		done = rd(rsc.base + RSC_IRQ_STATUS) & (1u << RPMH_TCS);
		if (done) {
			break;
		}
		sleep_us(50);
	}
	printf("adsp: %u: RPMh %s after %llu us; the commands' status %#x %#x, the TCS's %#x\n", step, done ? "HAS DONE IT" :
	    "HAS NOT SAID DONE", now_us() - began, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 12),
	    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(1) + 12), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS));
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 0);
	return done != 0;
}

/* ---- step 1: the shared memory --------------------------------------------------------------------- */

static bool
step_survey(void)
{
	struct smp2p words;
	uint32_t size = 0;

	announce(smem_phys == 0x80900000UL ? "read our partition with the audio DSP in the shared memory (0x80900000), and its items" :
	    "read our partition with the audio DSP in the shared memory (0x81d00000), and its items");
	show_partition();
	if (dsp_words(&words)) {
		show_words("the DSP's item (443) is there: it has run since the phone was started", &words);
	} else {
		printf("adsp: %u: the DSP's SMP2P item (443) is not there: it has not run since the phone was started\n", step);
	}
	printf("adsp: %u: our SMP2P item (429): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
	    smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "NOT THERE",
	    smem_item(479, &size) ? "there" : "NOT THERE", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
	show_crash_reason();
	need(&ipcc);
	printf("adsp: %u: the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	return smem_partition(SMEM_US, SMEM_ADSP, &size, &size) != 0;
}

/* ---- step 2: the firmware -------------------------------------------------------------------------- */

#define ADSP_MEMORY             adsp_memory     /* dt: reserved-memory adsp@86700000, memory-region of qcom,lpass@3700000 */
#define ADSP_MEMORY_SIZE        adsp_memory_size
static uint32_t adsp_memory = 0x86700000u, adsp_memory_size = 0x04000000u;
#define PAS_ADSP                1               /* dt: qcom,pas-id */


/*
 * A file of /fw on the root file system, read whole into memory of ours: the system image, where
 * userland/build.sh puts fw-audio/ when it makes that image. It is compressed there, which the file
 * system undoes and the reading of the small image's memory below does not; and it is there only
 * once the root is mounted, so only adsp=1, which waits for that on its own thread, finds it so.
 */
static const uint8_t *
root_file(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > ADSP_MEMORY_SIZE) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		printf("adsp: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool from_root;                  /* the image is in memory of ours (root_file()), to be given back once written */

/* For the sound (redwood_sound.c): a file of /fw on the root, read whole into memory of ours, and given back. */
const uint8_t *
redwood_adsp_fw_file(const char *name, uint64_t *size)
{
	return root_file(name, size);
}

void
redwood_adsp_fw_free(const uint8_t *data, uint64_t size)
{
	uint8_t *ours = __DECONST(uint8_t *, data);

	kfree_data(ours, size);
}

/* A file of fw/ in the small image (ADSP=1 userland/build.sh puts fw-audio/ there), else of /fw on the root (above). */
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	{
		const uint8_t *data = redwood_image_file("fw", name, size, NULL);  /* (redwood_image.c) */

		if (data != NULL) {
			return data;
		}
	}
	{
		const uint8_t *data = root_file(name, size);

		if (data != NULL) {
			from_root = true;
			return data;
		}
	}
	printf("adsp: %u: no fw/%s in the small image nor on the root: put the phone's /vendor/firmware_mnt/image/%s into "
	    "fw-audio/ and run SYSTEM=1 userland/build.sh (or ADSP=1, for the small image)\n", step, name, name);
	return NULL;
}


#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)
#define MAX_SEGMENTS            64              /* (shennong's adsp.mdt has 55) */

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

struct dsp_image {
	const char *name;                       /* its files: NAME.mdt, NAME.bNN */
	uint32_t memory, memory_size;           /* where it goes */
	bool from_root;
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;
};

/*
 * The DSP's own image, and on SM8650 (shennong) a second one before it: adsp_dtb, the DSP's device
 * tree, with its own PAS id and its own reserved memory (Linux's qcom_q6v5_pas.c: dtb_firmware_name,
 * dtb_pas_id 0x24 for sm8650_adsp_resource), taken by TrustZone and started before the DSP's.
 */
static struct dsp_image image = { .name = "adsp" }, dtb = { .name = "adsp_dtb" };
static uint32_t dtb_pas;                /* 0: no such image on this phone */

/* An image's files given back, if they were read into memory of ours. */
static void
image_forget_one(struct dsp_image *im)
{
	const char *name = im->name;
	uint32_t memory = im->memory, memory_size = im->memory_size;

	if (im->from_root) {
		if (im->header != NULL) {
			void *data = (void *)(uintptr_t)im->header;

			kfree_data(data, im->header_size);
		}
		for (unsigned s = 0; s < im->segments; s++) {
			if (im->segment[s].file != NULL) {
				void *data = (void *)(uintptr_t)im->segment[s].file;

				kfree_data(data, im->segment[s].file_size);
			}
		}
	}
	bzero(im, sizeof(*im));
	im->name = name;
	im->memory = memory;
	im->memory_size = memory_size;
}

static void
image_forget(void)
{
	image_forget_one(&image);
	image_forget_one(&dtb);
	from_root = false;
}

static bool
image_read(struct dsp_image *im)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0;
	char header_name[24];

	image_forget_one(im);
	from_root = false;
	snprintf(header_name, sizeof(header_name), "%s.mdt", im->name);
	header = image_file(header_name, &size);
	if (header == NULL) {
		return false;
	}
	im->header = header;
	im->header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		printf("adsp: %u: fw/%s is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header_name, header[0],
		    header[1], header[2], header[3], count, size);
		goto fail;
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[24];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.paddr < im->memory || segment.file_size > segment.memory_size ||
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)im->memory + im->memory_size) {
			printf("adsp: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			goto fail;
		}
		im->segment[im->segments].at = segment.paddr - im->memory;
		im->segment[im->segments].file_size = segment.file_size;
		im->segment[im->segments].memory_size = segment.memory_size;
		im->segment[im->segments].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "%s.b%02u", im->name, i);
			im->segment[im->segments].file = image_file(name, &file_size);
			if (im->segment[im->segments].file == NULL) {
				goto fail;
			}
			if (file_size != segment.file_size) {
				printf("adsp: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				im->segments++;               /* (for its file to be given back) */
				goto fail;
			}
			files += file_size;
		}
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		im->segments++;
	}
	if (im->segments == 0 || lowest != im->memory || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("adsp: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, im->segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	im->header = header;
	im->header_size = size;
	im->size = ((highest + 0xfff) & ~0xfffu) - im->memory;
	im->from_root = from_root;
	printf("adsp: %u: the image %s is whole (%s): a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x "
	    "bytes of the DSP's %#x from %#x\n", step, im->name, from_root ? "from /fw on the root" : "from the small image", size, hashes,
	    im->segments, files, im->size, im->memory_size, im->memory);
	return true;
fail:
	im->from_root = from_root;
	image_forget_one(im);
	return false;
}

static bool
step_firmware(void)
{
	announce(dtb_pas != 0 ? "find the audio DSP's two images (adsp_dtb, then adsp) in the small image or on the root, and check "
	    "each against its own header" : "find the audio DSP's image in the small image or on the root, and check it against its "
	    "own header");
	image_forget();
	if (dtb_pas != 0 && !image_read(&dtb)) {
		return false;
	}
	if (!image_read(&image)) {
		image_forget();
		return false;
	}
	return true;
}

/* ---- step 3: the mailbox --------------------------------------------------------------------------- */

static bool
step_mailbox(void)
{
	bool open;

	announce("open the mailbox to the always-on processor, unless it is open: words of its message memory, and its doorbell");
	lck_mtx_lock(&redwood_aop_mailbox_lock);
	open = mailbox_open();
	lck_mtx_unlock(&redwood_aop_mailbox_lock);
	return open;
}

/* ---- TrustZone, and stopping the DSP --------------------------------------------------------------- */

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1

static LCK_MTX_DECLARE(dsp_lock, &adsp_lock_group);     /* the DSP being started or stopped */
static bool rails_asked;                /* our request for its rails stands (until it asks for its own, or is shut down) */
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("adsp: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	/*
	 * TrustZone gives the call back "interrupted" whenever an interrupt of ours is waiting, and is called again to go
	 * on (Linux goes on for as long as it takes). SM8650's shutting down of the audio DSP came back so a hundred times
	 * in a row with no pause between: now as many as 20000, with 50 us between them -- a second at most.
	 */
	for (int i = 0; i < 20000; i++) {
		if (i > 0) {
			uint64_t until = now_us() + 50;

			while (now_us() < until) {
			}
		}
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("adsp: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("adsp: %u: TrustZone keeps saying the call was interrupted (20000 times, a second)\n", step);
	return -1;
}

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}

/* What Linux does when the DSP rings for SMP2P: agree on the features once, and answer a restart. */
static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("adsp: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("adsp: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
		    step, theirs->flags, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	}
}

/* Linux's qcom_q6v5_request_stop(): our bit "stop", the DSP's "stop ack" waited for, our bit taken back. */
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
	    (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
		printf("adsp: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("adsp: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("adsp: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("adsp: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	need(&ipcc);
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("adsp: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx, and its SMP2P doorbell\n", step,
	    OUR_ENTRY_STOP, SMEM_PHYS + value_at);
	snprintf(last_write, sizeof(last_write), "step %u asked the audio DSP to stop", step);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("adsp: %u: %s after %llu.%03llu ms; its entry says %#x\n", step, state & DSP_STOP_ACK ?
	    "THE AUDIO DSP ACKNOWLEDGES THE STOP" : "THE AUDIO DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000,
	    (now - asked) % 1000, state);
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

static void link_stop(void);

/* Asked to stop, then shut down by TrustZone, then the always-on processor told that the image is gone. */
static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;

	printf("adsp: %u: %s: the audio DSP is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	/* Its doorbells no longer listened for, GLINK's as well as SMP2P's, whatever TrustZone says next: a signal left
	 * listened for keeps the line to the GIC up for the boot loader after us, whose own GIC driver then says
	 * "Spurious GIC interrupt: 0x105" -- SPI 229, the IPCC's -- without end (shennong). */
	need(&ipcc);
	wr(ipcc.base + IPCC_DISABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
	wr(ipcc.base + IPCC_DISABLE, IPCC_LPASS << 16 | IPCC_GLINK);
	doorbells();
	if (!leaving) {
		ipcc_interrupt_off();
	}
	result = scm("to shut the audio DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT SHUT THE AUDIO DSP DOWN: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		return false;
	}
	printf("adsp: %u: THE AUDIO DSP IS SHUT DOWN\n", step);
	if (dtb_pas != 0) {
		answer = ~0ull;
		result = scm("to shut the audio DSP's device tree down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, dtb_pas, 0, 0, &answer);
		printf("adsp: %u: its device tree (image %#x): TrustZone returns %d, answer %#llx\n", step, dtb_pas, result, answer);
	}
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: adsp, val: off}");
	if (rails_asked) {
		rails(0);
		rails_asked = false;
	}
	return true;
}

/* An image's header handed to TrustZone (image `pas'), and the place it will be in. */
static bool
pas_take(struct dsp_image *im, uint32_t pas)
{
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	int32_t result;
	char what[160];

	if (im->segments == 0) {
		return false;
	}
	snprintf(what, sizeof(what), "hand the header of %s to TrustZone to check its signature (image %#x), in a page of ours",
	    im->name, pas);
	announce(what);
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adsp: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, im->header, im->header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm("to take the image's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, pas, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE HEADER of %s: the call returns %d, its answer is %#llx\n", step, im->name,
		    result, answer);
		return false;
	}
	printf("adsp: %u: TRUSTZONE ACCEPTS THE SIGNATURE OF %s FROM THIS KERNEL\n", step, im->name);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, pas, im->memory, im->size, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE PLACE of %s: the call returns %d, its answer is %#llx\n", step, im->name,
		    result, answer);
		return false;
	}
	printf("adsp: %u: TrustZone takes the place of %s: %#x bytes at %#x\n", step, im->name, im->size, im->memory);
	return true;
}

/* ---- step 4: TrustZone takes the header ------------------------------------------------------------ */

static bool
step_header(void)
{
	struct smp2p words;

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down an audio DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !option("adsp_again")) {
			printf("adsp: %u: THE AUDIO DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and "
			    "with the Wi-Fi's DSP a start after that reset the phone (adsp_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("adsp: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started\n", step);
	}

	if (dtb_pas != 0 && !pas_take(&dtb, dtb_pas)) {
		return false;
	}
	return pas_take(&image, PAS_ADSP);
}

/* ---- step 5: the image, in the DSP's memory --------------------------------------------------------- */

static bool image_written;

static uint64_t
segment_word(const struct dsp_image *im, unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8 && at + b < im->segment[s].file_size; b++) {
		word |= (uint64_t)im->segment[s].file[at + b] << (8 * b);
	}
	return word;
}

static bool
image_write(struct dsp_image *im)
{
	volatile uint64_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;
	char what[256];

	if (im->segments == 0) {
		return false;
	}
	snprintf(what, sizeof(what), "put the segments of %s into its memory (%#x, %#x bytes: memory of nobody's until TrustZone "
	    "starts the DSP), with zeroes where a segment is longer than its file, and read them back", im->name, im->memory,
	    im->memory_size);
	announce(what);
	dsp_memory = (volatile uint64_t *)ml_io_map_unmappable(im->memory, im->memory_size, VM_WIMG_WCOMB);
	snprintf(last_write, sizeof(last_write), "step %u wrote %s at %#x", step, im->name, im->memory);
	for (unsigned s = 0; s < im->segments; s++) {
		volatile uint64_t *to = dsp_memory + im->segment[s].at / 8;

		/* (a segment may end anywhere -- shennong's has two of 0x52 and 0x13e07 bytes -- and is written up to its next
		 * multiple of eight with zeroes, which lands in its memory still; it must begin at one) */
		if ((im->segment[s].at & 7) || ((im->segment[s].memory_size + 7) & ~7u) > im->memory_size - im->segment[s].at) {
			printf("adsp: %u: a segment that does not begin at a multiple of eight bytes, or ends beyond the memory: not "
			    "written\n", step);
			ml_io_unmap((vm_offset_t)dsp_memory, im->memory_size);
			return false;
		}
		for (uint32_t i = 0; i < im->segment[s].memory_size; i += 8) {
			to[i / 8] = segment_word(im, s, i);
		}
		written += im->segment[s].file_size;
		cleared += im->segment[s].memory_size - im->segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < im->segments; s++) {
		volatile uint64_t *from = dsp_memory + im->segment[s].at / 8;

		for (uint32_t i = 0; i < im->segment[s].memory_size; i += 8) {
			wrong += from[i / 8] != segment_word(im, s, i);
		}
	}
	printf("adsp: %u: %s: %u segments, %llu bytes written and %llu cleared; read back, %llu words of eight bytes differ\n", step,
	    im->name, im->segments, written, cleared, wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, im->memory_size);
	return wrong == 0;
}

static bool image_written;

static bool
step_image(void)
{
	image_written = (dtb_pas == 0 || image_write(&dtb)) && image_write(&image);
	image_forget();                         /* (it is in the DSP's memory now: what was read of the root, given back) */
	return image_written;
}

/* ---- step 6: our SMP2P item ------------------------------------------------------------------------ */

static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u

/* Our item made in the partition, as Linux's qcom_smem_alloc_private() makes one (redwood_wifi.c: our_item_make()). */
static bool
our_item_make(uint32_t partition)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (sizeof(struct smp2p) + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	lck_mtx_lock(&redwood_smem_alloc_lock);         /* (the Wi-Fi's start allocates under the same hardware lock) */
	need(&smem_lock);
	printf("adsp: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
	    "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, ITEM_SMP2P_OURS, item_size, need_bytes);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("adsp: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		lck_mtx_unlock(&redwood_smem_alloc_lock);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("adsp: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == ITEM_SMP2P_OURS ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("adsp: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need_bytes > header.free_cached) {
		printf("adsp: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | ITEM_SMP2P_OURS << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - (uint32_t)sizeof(struct smp2p));
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("adsp: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
	lck_mtx_unlock(&redwood_smem_alloc_lock);
	return made;
}

static void glink_reset(void);

static bool
step_our_item(void)
{
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition, name[4] = { 0 };

	announce("look for an SMP2P item of ours (429) in our partition with the audio DSP, make it if there is none, and fill it in");
	partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp: %u: there is no such partition\n", step);
		return false;
	}
	our_item = our_item_find();
	if (our_item != 0) {
		printf("adsp: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		if (!our_item_make(partition)) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("adsp: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("adsp: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	}
	/* zeroes; from processor 0 to 2, room for 16 entries, "master-kernel" at zero, feature bit 0; last the version */
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_ADSP << 16);
	strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 1u << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	need(&ipcc);
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("adsp: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);
	glink_reset();
	return true;
}

/* ---- step 7: the DSP, started ---------------------------------------------------------------------- */

/*
 * Android's kernel (this phone, dmesg): the image loaded at 8.92 s, "brought out of reset" at
 * 9.10, "error ready" 40 ms later; the audio services' protection domain up at 9.39.
 * Here (runs 085341 and later): the DSP's SMP2P item appears 17 ms after TrustZone returns, with
 * a ring of its SMP2P and GLINK doorbells; at 41 ms "slave-kernel" says 6, up and asking for its
 * own power, and our request for the rails is given back; at 2.2 s it adds "sleepstate_see".
 * Asked to stop, it acknowledges within 25 ms (0x10e), and it starts again in the same session.
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	int32_t result;
	bool have, spoke = false, handed_over = false;

	if (!image_written) {
		printf("adsp: %u: the image is not in place\n", step);
		return false;
	}
	PE_parse_boot_argn("adsp_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	announce("listen for the audio DSP's doorbells, ask RPMh for its rails, tell the always-on processor that its image is "
	    "loaded, and ask TrustZone to authenticate the image and let the DSP out of reset");
	need(&ipcc);
	for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY && rang >> 16 == IPCC_LPASS; i++) {
		put(&ipcc, "heard", IPCC_CLEAR, rang);
	}
	ipcc_interrupt_off();
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_GLINK);
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	if (!rails(384)) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	rails_asked = true;
	if (!mailbox_send("{class: image, res: load_state, name: adsp, val: on}")) {
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = 0;
	answer = 0;
	if (dtb_pas != 0) {
		result = scm("to authenticate the audio DSP's device tree (adsp_dtb) and take it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE,
		    dtb_pas, 0, 0, &answer);
		if (result != 0 || answer != 0) {
			printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE DSP'S DEVICE TREE: the call returns %d, its answer is %#llx\n", step,
			    result, answer);
		}
	}
	if (result == 0 && answer == 0) {
		result = scm("to authenticate the audio DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_ADSP, 0,
		    0, &answer);
	}
	doorbell.since = started = now_us();
	image_written = false;
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT START THE AUDIO DSP: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		mailbox_send("{class: image, res: load_state, name: adsp, val: off}");
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("adsp: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE AUDIO DSP OUT OF RESET; watching for %u s\n", step,
	    wait_seconds);
	limit = started + wait_seconds * 1000000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !dsp_started) {
			break;
		}
		doorbells();
		spoke = spoke || doorbell.glink + doorbell.smp2p != 0;
		if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(when, &words);
			}
			last = words;
			have = spoke = true;
			dsp_listen(&words);
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + 3000000ull;
				printf("adsp: %u: THE AUDIO DSP SAYS IT IS UP: \"slave-kernel\" says %#x, %llu.%03llu ms after TrustZone started "
				    "it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("adsp: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start: ours is "
				    "given back\n", step, (now - started) / 1000, (now - started) % 1000);
				lck_mtx_lock(&dsp_lock);
				if (rails_asked) {
					rails(0);
					rails_asked = false;
				}
				lck_mtx_unlock(&dsp_lock);
			}
			if (state & DSP_FATAL) {
				printf("adsp: %u: THE AUDIO DSP SAYS IT HAS GIVEN UP\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	printf("adsp: %u: the watch is over after %llu ms; its doorbells heard: GLINK %u, SMP2P %u\n", step,
	    (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p);
	show_partition();
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("our item (429) now", &words);
	}
	{
		/* (what is at the bottom of our partition, entry by entry, as it is now) */
		uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
		struct smem_partition header;
		struct smem_entry entry;

		if (partition != 0 && smem_peek(partition, &header, sizeof(header))) {
			printf("adsp: %u: the partition's header: hosts %u and %u, size %#x, free from the bottom at +%#x, from the top at "
			    "+%#x; the table's line size %u\n", step, header.host0, header.host1, header.size, header.free_uncached,
			    header.free_cached, cacheline);
			for (uint32_t at = sizeof(header), n = 0; at + sizeof(entry) <= header.size && n < 8; n++) {
				smem_peek(partition + at, &entry, sizeof(entry));
				printf("adsp: %u:   +%#x: canary %#x item %u size %u padding %u/%u\n", step, at, entry.canary, entry.item,
				    entry.size, entry.padding_data, entry.padding_header);
				if (entry.canary != SMEM_CANARY) {
					break;
				}
				at += sizeof(entry) + entry.padding_header + entry.size;
			}
		}
	}
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at == 0) {
		printf("adsp: %u: %s\n", step, spoke ? "THE AUDIO DSP RUNS BUT HAS NOT SAID \"UP\"" : "NOT A WORD FROM THE AUDIO DSP");
		show_crash_reason();
	}
	return ready_at != 0;
}

/* ---- GLINK (redwood_wifi.c; items 478 to 480 of our partition with the audio DSP) ------------------- */

#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
#define GLINK_OUR_TAIL          0
#define GLINK_OUR_HEAD          4
#define GLINK_THEIR_TAIL        8
#define GLINK_THEIR_HEAD        12
#define GLINK_RESERVE           16

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

/*
 * The channels we answer when the DSP opens them, each with our number for it and the intents
 * we give it when it is open both ways (how many buffers of which size the DSP may send into):
 * IPCRTR, which carries QRTR (dt: qcom,intents of qcom,adsp_qrtr), and apr_audio_svc, which
 * carries APR, the audio services' own protocol (below; Android's apr_tal_rpmsg.c asks for no
 * particular intents: what the DSP sends there is a few hundred bytes, up to a few kilobytes).
 */
enum { CHANNEL_QRTR, CHANNEL_APR, CHANNEL_PMIC, CHANNEL_FASTRPC, CHANNELS };

struct glink_channel {
	const char *name;
	uint32_t ours;                          /* our number for it */
	struct {
		uint32_t size, count;
	} intents[3];
	uint32_t theirs;                        /* the DSP's number for it; 0: it has not opened it */
	bool asked, acknowledged, open;         /* our open sent; acknowledged; both ways done and our intents told */
	bool request_answered, request_granted; /* our last request for an intent */
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[256];                   /* what we may send into */
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[24];                      /* what the DSP may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
};

static LCK_MTX_DECLARE(glink_lock, &adsp_lock_group);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken;
	uint32_t features;
	bool agreed;
	struct glink_channel channel[CHANNELS];
	uint32_t said, heard, trace;
	char others[4][33];                     /* the channels the DSP opens that nobody here speaks (for the log) */
} glink = {
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
		[CHANNEL_APR] = { .name = "apr_audio_svc", .ours = 2, .intents = { { 0x200, 8 }, { 0x1000, 4 }, { 0x2000, 2 } } },
		/* the battery manager's (battmgr below): answers of a few dozen bytes, a name of 128 */
		[CHANNEL_PMIC] = { .name = "PMIC_RTR_ADSP_APPS", .ours = 3, .intents = { { 0x200, 6 }, { 0x1000, 2 }, { 0, 0 } } },
		/* FastRPC's (below): the DSP answers an invocation in 16 bytes (dt: qcom,msm_fastrpc_rpmsg, qcom,intents <0x64 0x40>) */
		[CHANNEL_FASTRPC] = { .name = "fastrpcglink-apps-dsp", .ours = 4, .intents = { { 0x100, 16 }, { 0, 0 }, { 0, 0 } } },
	},
};

static struct glink_channel *
glink_theirs(uint32_t theirs)
{
	for (unsigned c = 0; c < CHANNELS; c++) {
		if (glink.channel[c].theirs != 0 && glink.channel[c].theirs == theirs) {
			return &glink.channel[c];
		}
	}
	return NULL;
}

static bool
glink_find(void)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	if (glink.descriptor == 0 || size != 32) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7);
}

static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	if (descriptor == 0 || size != 32) {
		printf("adsp: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("adsp: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("adsp: %u: about to write our two marks there (+4, +8): 0\n", step);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_GLINK);
	glink.said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf("adsp: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

static unsigned intents_said;           /* the DSP's intents said in the log (glink_pump()) */

static bool
glink_offer(struct glink_channel *channel, uint32_t size)
{
	uint32_t words[2] = { size, channel->our_intent_count + 1 }, i = channel->our_intent_count;

	if (i >= sizeof(channel->our_intents) / sizeof(channel->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (channel->our_intents[i].data == NULL || channel->our_intents[i].size != size) {
		channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (channel->our_intents[i].data == NULL) {
		return false;
	}
	channel->our_intents[i].size = size;
	channel->our_intents[i].fill = 0;
	channel->our_intent_count++;
	glink_say(GLINK_INTENT, channel->ours, 1, words, sizeof(words));
	return true;
}

/* Everything the DSP has put into its queue, taken out and acted on (Linux's qcom_glink_native_rx()). With glink_lock held. */
static void
glink_pump(void)
{
	struct glink_message message;
	struct glink_channel *channel;
	uint32_t words[4];
	char name[33];

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > glink.theirs_size - 8) {
			printf("adsp: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (waiting < length) {
			return;
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE &&
		    message.command != GLINK_INTENT && message.command != GLINK_DONE_REUSE) {
			glink.trace--;
			printf("adsp: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				glink.features &= message.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
				glink.agreed = true;
			} else if (message.param1 != 0) {
				glink.features &= message.param2;
				glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(length);
			channel = NULL;
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (strcmp(name, glink.channel[c].name) == 0) {
					channel = &glink.channel[c];
				}
			}
			if (channel == NULL) {
				printf("adsp: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (glink.others[i][0] == 0) {
						strlcpy(glink.others[i], name, sizeof(glink.others[i]));
						break;
					}
				}
				break;
			}
			printf("adsp: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, channel->ours);
			channel->theirs = message.param1;
			glink_say(GLINK_OPEN_ACK, channel->theirs, 0, NULL, 0);
			if (!channel->asked) {
				uint32_t size = (uint32_t)strlen(channel->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, channel->name, sizeof(name));
				glink_say(GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
				channel->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].ours) {
					glink.channel[c].acknowledged = true;
				}
			}
			break;
		case GLINK_INTENT:
			channel = glink_theirs(message.param1);
			if (glink.trace > 0) {
				glink_peek(words, 8, 8);
				glink.trace--;
				printf("adsp: glink: < intent %u %u: the first of %u bytes (id %u)\n", message.param1, message.param2, words[0],
				    words[1]);
			}
			for (uint32_t i = 0; channel != NULL && i < message.param2; i++) {
				unsigned n, slots = sizeof(channel->their_intents) / sizeof(channel->their_intents[0]);

				glink_peek(words, 8 + 8 * i, 8);
				for (n = 0; n < slots; n++) {
					if (!channel->their_intents[n].there || channel->their_intents[n].id == words[1]) {
						channel->their_intents[n].there = true;
						channel->their_intents[n].in_use = false;
						channel->their_intents[n].size = words[0];
						channel->their_intents[n].id = words[1];
						break;
					}
				}
				/* (said always when it is not one of the small ones, or there is no place left for it) */
				if (n == slots || (words[0] > 128 && intents_said < 40)) {
					intents_said++;
					printf("adsp: glink: < intent for \"%s\": %u bytes, id %u%s\n", channel->name, words[0], words[1],
					    n == slots ? ": NO PLACE LEFT FOR IT, forgotten" : "");
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			channel = glink_theirs(message.param1);
			for (unsigned n = 0; channel != NULL && n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
				if (channel->their_intents[n].there && channel->their_intents[n].id == message.param2) {
					channel->their_intents[n].in_use = false;
					channel->their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				bool granted = glink_offer(channel, message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, channel->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].theirs || message.param1 == glink.channel[c].ours) {
					glink.channel[c].request_answered = true;
					glink.channel[c].request_granted = message.param2 != 0;
				}
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			channel = glink_theirs(message.param1);
			if (channel == NULL || intent >= channel->our_intent_count ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				printf("adsp: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			glink_peek(channel->our_intents[intent].data + channel->our_intents[intent].fill, 16, words[0]);
			channel->our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + channel->our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = channel->our_intents[intent].fill;
					memcpy(packet->data, channel->our_intents[intent].data, packet->length);
					if (channel->last != NULL) {
						channel->last->next = packet;
					} else {
						channel->first = packet;
					}
					channel->last = packet;
				}
				channel->our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				printf("adsp: glink: THE DSP CLOSES \"%s\"\n", channel->name);
				channel->open = false;
				channel->theirs = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("adsp: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		for (unsigned c = 0; c < CHANNELS; c++) {
			channel = &glink.channel[c];
			if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
				channel->open = true;
				printf("adsp: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
				    channel->name, channel->ours, channel->theirs);
				for (unsigned i = 0; i < 3; i++) {
					for (unsigned n = 0; n < channel->intents[i].count; n++) {
						glink_offer(channel, channel->intents[i].size);
					}
				}
			}
		}
	}
}

/* One whole message to the DSP on a channel; waits up to five seconds for an intent of the DSP's that it fits. */
static int
glink_send(struct glink_channel *channel, const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, (uint16_t)channel->ours, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !channel->open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
			if (!channel->their_intents[n].there || channel->their_intents[n].size < length) {
				continue;
			}
			fits = true;
			if (!channel->their_intents[n].in_use &&
			    (found < 0 || channel->their_intents[n].size < channel->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			channel->request_answered = false;
			glink.trace = 12;               /* (what the DSP answers is said) */
			glink_say(GLINK_INTENT_REQUEST, channel->ours, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && channel->request_answered && !channel->request_granted)) {
			unsigned listed = 0;

			printf("adsp: glink: %u bytes for \"%s\" not sent after %u ms: %s; its intents:", length, channel->name, waited,
			    !asked ? "none asked for" : !channel->request_answered ? "the DSP DID NOT ANSWER our request for one" :
			    channel->request_granted ? "the DSP granted one, which did not come" : "the DSP REFUSED one");
			for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
				if (channel->their_intents[n].there && listed++ < 24) {
					printf(" %u:%u%s", channel->their_intents[n].id, channel->their_intents[n].size,
					    channel->their_intents[n].in_use ? "(in use)" : "");
				}
			}
			printf("%s (%u in all)\n", listed ? "" : " none", listed);
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		sleep_ms(1);
		lck_mtx_lock(&glink_lock);
	}
	channel->their_intents[found].in_use = true;
	header.message.param2 = channel->their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR: hello, and the services the DSP tells of (redwood_wifi.c, without ports of our own yet) ---- */

#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

struct qrtr_header {
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];
};

static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer, qrtr_in, qrtr_out, qrtr_hellos, qrtr_lookups;

/*
 * Ports of our own on QRTR, for a client of one of the DSP's QMI services (shennong_sensors.c: the sensors' service
 * 400). A datagram to one of them goes to its handler, on the link's thread: the handler copies what it needs and
 * does not wait.
 */
static struct {
	uint32_t port;
	redwood_qrtr_input_t input;
} qrtr_ports[12];

static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = glink_send(&glink.channel[CHANNEL_QRTR], packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;
		}
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("adsp: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		break;
	case QRTR_NEW_SERVER:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!qrtr_servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qrtr_servers[found].there = true;
			qrtr_servers[found].service = control->words[0];
			qrtr_servers[found].instance = control->words[1];
			qrtr_servers[found].node = control->words[2];
			qrtr_servers[found].port = control->words[3];
		}
		printf("adsp: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		break;
	case QRTR_DEL_SERVER:
		printf("adsp: qrtr: service %u at node %u port %u has gone\n", control->words[0], control->words[2], control->words[3]);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				qrtr_servers[i].there = false;
			}
		}
		break;
	case QRTR_NEW_LOOKUP:
		/* the DSP looks for a service of ours (a protection domain's locator, say): there are none yet */
		if (qrtr_lookups++ < 16) {
			printf("adsp: qrtr: the DSP looks for service %u (%#x), instance %u: nobody here offers it\n", control->words[0],
			    control->words[0], control->words[1]);
		}
		break;
	case QRTR_BYE:
		printf("adsp: qrtr: BYE from node %u\n", header->from_node);
		bzero(qrtr_servers, sizeof(qrtr_servers));
		break;
	default:
		break;
	}
}

static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		printf("adsp: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	if (header.size == 0 || at + header.size > length) {
		printf("adsp: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;
	}
	if (header.confirm) {
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
	    header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_NEW_LOOKUP) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	for (unsigned i = 0; i < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); i++) {
		redwood_qrtr_input_t input = qrtr_ports[i].input;

		if (input != NULL && qrtr_ports[i].port == header.to_port) {
			input(header.to_port, header.from_node, header.from_port, data + at, header.size);
			return;
		}
	}
	printf("adsp: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", header.size,
	    header.from_node, header.from_port, header.to_port);
}

/* A port of ours: its datagrams to `input' (NULL: to nobody again). False if there is no room for another. */
bool
redwood_adsp_qrtr_port(uint32_t port, redwood_qrtr_input_t input)
{
	int slot = -1;

	for (unsigned i = 0; i < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); i++) {
		if (qrtr_ports[i].input != NULL && qrtr_ports[i].port == port) {
			slot = (int)i;
			break;
		}
		if (qrtr_ports[i].input == NULL && slot < 0) {
			slot = (int)i;
		}
	}
	if (slot < 0) {
		return false;
	}
	qrtr_ports[slot].input = NULL;
	qrtr_ports[slot].port = port;
	qrtr_ports[slot].input = input;
	return true;
}

/* Where the DSP has said a service is (any instance); false if it has not told of it (or it has gone). */
bool
redwood_adsp_qrtr_service(uint32_t service, uint32_t *node, uint32_t *port)
{
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		if (qrtr_servers[i].there && qrtr_servers[i].service == service) {
			*node = qrtr_servers[i].node;
			*port = qrtr_servers[i].port;
			return true;
		}
	}
	return false;
}

/* One datagram from our port `from_port' to a port of the DSP's; 0 or why not. */
int
redwood_adsp_qrtr_send(uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	if (!glink.up || !glink.channel[CHANNEL_QRTR].open) {
		return ENOTCONN;
	}
	return qrtr_out_packet(QRTR_DATA, from_port, to_node, to_port, data, length);
}

/* ---- APR: the audio services' protocol, on the channel apr_audio_svc ------------------------------------- */

/*
 * What Android's audio drivers say to the DSP's audio services (Linux: drivers/soc/qcom/apr.c,
 * include/linux/soc/qcom/apr.h; Android's q6core.c): a packet of a header of eight words -- a
 * word of kind, header length and version and the packet's length, who it is from and to (a
 * service, a domain, a port each: we are domain 5, the DSP 4), a token the answer carries back,
 * an opcode -- and what the opcode takes. The DSP's core service (3) answers whether the audio
 * services are up (AVCS_CMD_ADSP_EVENT_GET_STATE, which Android asks again and again after the DSP
 * has started, until the answer is 1) and which services it has and in which versions.
 */
#define APR_DOMAIN_ADSP                 4
#define APR_DOMAIN_APPS                 5
#define APR_SVC_ADSP_CORE               3
#define APR_MSG_TYPE_SEQ_CMD            2
#define APR_HDR_FIELD(type, bytes)      ((uint16_t)((type) << 8 | ((bytes) / 4) << 4))      /* version 0 */
#define APR_BASIC_RSP_RESULT            0x000110e8u
#define AVCS_GET_VERSIONS               0x00012905u
#define AVCS_GET_VERSIONS_RSP           0x00012906u
#define AVCS_CMD_ADSP_EVENT_GET_STATE   0x0001290cu
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE 0x0001290du

struct apr_header {
	uint16_t field, size;
	uint8_t from_service, from_domain;
	uint16_t from_port;
	uint8_t to_service, to_domain;
	uint16_t to_port;
	uint32_t token, opcode;
};

static struct {
	uint32_t opcode, token, length;
	uint32_t words[64];
	volatile bool there;
} apr_answer;
static uint32_t apr_in, apr_out, apr_next_token = 1;
static bool audio_up;                   /* the core service has said that the audio services are up (step 11) */
static uint32_t apr_waited_for;         /* the token a question waits for the answer to; 0: none */
static redwood_apr_events_t apr_events; /* the sound's (redwood_sound.c): what nobody waits for */
static redwood_apr_events_t apr_events_second;  /* the microphones' (redwood_mic.c) */
static LCK_MTX_DECLARE(apr_lock, &adsp_lock_group);     /* one question at a time */

/*
 * SM8650 (shennong): the DSP speaks GPR, AudioReach's packet router, instead of APR, on the channel "adsp_apps"
 * (dt: qcom,gpr in glink-edge of remoteproc-adsp; Linux: drivers/soc/qcom/apr.c in its GPR half,
 * include/linux/soc/qcom/apr.h, sound/soc/qcom/qdsp6/q6apm.c). A packet's header is six words: a version (0), the
 * header's length in words (6) and the packet's length in bytes; the domains it is to and from (the DSP 2, we 3);
 * the ports it is from and to; a token; an opcode. The DSP's APM (the audio processing manager, port 1) answers
 * APM_CMD_GET_SPF_STATE with 1 once its audio framework (SPF) is up -- what q6apm_is_adsp_ready() asks.
 */
static bool gpr;
#define GPR_DOMAIN_ADSP                 2
#define GPR_DOMAIN_APPS                 3
#define GPR_APM_PORT                    1
#define GPR_OUR_PORT                    3       /* dt: spf_core, reg = <3> */
#define GPR_BASIC_RSP_RESULT            0x02001005u
#define APM_CMD_GET_SPF_STATE           0x01001021u
#define APM_CMD_RSP_GET_SPF_STATE       0x02001007u
#define GPR_MAX_PAYLOAD                 1024    /* the largest command sent: a graph's open (shennong_sound.c) is some 500 bytes */
static redwood_gpr_events_t gpr_events; /* the sound's (shennong_sound.c): what nobody waits for */
static uint32_t gpr_from_port = GPR_OUR_PORT;   /* the port our packets are from (redwood_adsp_gpr_port()) */

struct gpr_header {
	uint32_t field;                         /* version [3:0], header words [7:4], packet bytes [31:8] */
	uint8_t to_domain, from_domain;
	uint16_t reserved;
	uint32_t from_port, to_port;
	uint32_t token, opcode;
};

static void
gpr_input(const uint8_t *data, uint32_t length)
{
	struct gpr_header header;
	uint32_t header_length, size, payload;

	if (length < sizeof(header)) {
		printf("adsp: gpr: %u bytes, no GPR packet\n", length);
		return;
	}
	memcpy(&header, data, sizeof(header));
	header_length = ((header.field >> 4) & 0xf) * 4;
	size = header.field >> 8;
	if (header_length < sizeof(header) || header_length > length) {
		printf("adsp: gpr: a header of %u bytes in a packet of %u: dropped\n", header_length, length);
		return;
	}
	payload = (size < length ? size : length) - header_length;
	apr_in++;
	if (header.token != apr_waited_for || apr_waited_for == 0) {
		redwood_gpr_events_t events = gpr_events;
		uint32_t words[64];

		bzero(words, sizeof(words));
		memcpy(words, data + header_length, payload < sizeof(words) ? payload : sizeof(words));
		if (events != NULL && events(header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
	}
	if (apr_in <= 40) {
		uint32_t w[4] = { 0, 0, 0, 0 };

		memcpy(w, data + header_length, payload < sizeof(w) ? payload : sizeof(w));
		printf("adsp: gpr: < from port %#x of domain %u to port %#x of %u: opcode %#x, token %u, %u bytes: %#x %#x %#x %#x\n",
		    header.from_port, header.from_domain, header.to_port, header.to_domain, header.opcode, header.token, payload, w[0],
		    w[1], w[2], w[3]);
	}
	bzero(apr_answer.words, sizeof(apr_answer.words));
	memcpy(apr_answer.words, data + header_length, payload < sizeof(apr_answer.words) ? payload : sizeof(apr_answer.words));
	apr_answer.length = payload;
	apr_answer.opcode = header.opcode;
	apr_answer.token = header.token;
	apr_answer.there = true;
}

/*
 * A GPR command to a port of the DSP's (an instance of a module, or the APM, the PRM), with the given
 * token (0: one of its own) and, with wait_ms, the packet that carries that token back waited for: true
 * when it was sent and, if waited for, answered; the answer in apr_answer and, if asked for, in *answer.
 * What is not waited for goes to the GPR events' handler (shennong_sound.c), if there is one.
 */
bool
redwood_adsp_gpr(uint32_t to_port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length, uint32_t ms,
    struct redwood_apr_answer *answer)
{
	uint8_t packet[sizeof(struct gpr_header) + GPR_MAX_PAYLOAD];
	struct gpr_header *header = (struct gpr_header *)packet;
	bool answered = false;
	int error;

	if (!gpr || length > sizeof(packet) - sizeof(*header) || !glink.channel[CHANNEL_APR].open) {
		return false;
	}
	bzero(packet, sizeof(*header));
	header->field = 0 | (uint32_t)(sizeof(*header) / 4) << 4 | (uint32_t)(sizeof(*header) + length) << 8;
	header->to_domain = GPR_DOMAIN_ADSP;
	header->from_domain = GPR_DOMAIN_APPS;
	header->from_port = gpr_from_port;
	header->to_port = to_port;
	header->opcode = opcode;
	if (length != 0) {
		memcpy(packet + sizeof(*header), payload, length);
	}
	if (ms == 0) {
		/* (sent and not waited for: what comes back is the events' handler's) */
		header->token = token;
		error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
		apr_out += error == 0;
		return error == 0;
	}
	lck_mtx_lock(&apr_lock);
	if (token == 0) {
		token = apr_next_token++;
		if (apr_next_token >= 0x01000000u) {
			apr_next_token = 1;
		}
	}
	header->token = token;
	apr_answer.there = false;
	apr_waited_for = token;
	error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
	if (error != 0) {
		printf("adsp: gpr: > opcode %#x to port %#x: NOT SENT (%d)\n", opcode, to_port, error);
		if (answer != NULL) {
			answer->opcode = REDWOOD_GPR_NOT_SENT;
		}
	} else {
		apr_out++;
		for (uint32_t waited = 0; waited < ms; waited++) {
			if (apr_answer.there && apr_answer.token == token) {
				answered = true;
				if (answer != NULL) {
					answer->opcode = apr_answer.opcode;
					answer->token = apr_answer.token;
					answer->length = apr_answer.length;
					memcpy(answer->words, apr_answer.words, sizeof(answer->words));
				}
				break;
			}
			sleep_ms(1);
		}
	}
	apr_waited_for = 0;
	lck_mtx_unlock(&apr_lock);
	return answered;
}

void
redwood_adsp_gpr_events(redwood_gpr_events_t handler)
{
	gpr_events = handler;
}

void
redwood_adsp_gpr_port(uint32_t port)
{
	gpr_from_port = port != 0 ? port : GPR_OUR_PORT;
}

/* A GPR command to a port of the DSP's, and its answer waited for: true, and the answer in apr_answer, if it came. */
static bool
gpr_ask(uint32_t to_port, uint32_t opcode, const void *payload, uint32_t length, uint32_t ms)
{
	return redwood_adsp_gpr(to_port, 0, opcode, payload, length, ms, NULL);
}

/*
 * FastRPC (Linux: drivers/misc/fastrpc.c), only as far as the Xiaomi 14 Pro's audio needs it: the DSP's audio
 * protection domain ("audiopd") gets the heap it makes its containers in from us, as Android's audioadsprpcd has
 * the kernel give it (dt: qcom,msm_fastrpc, qcom,fastrpc-adsp-audio-pdr; adsp_heap_region). An invocation is one
 * message of 40 bytes on "fastrpcglink-apps-dsp" -- whose process (0: the kernel's), the caller's context (an
 * index << 4 | the domain), the handle and method invoked, and where its arguments are -- answered with the
 * context and a return value.
 */
struct fastrpc_message {
	int32_t pid, tid;
	uint64_t context;
	uint32_t handle, scalars;
	uint64_t address, size;
} __attribute__((packed));

static struct {
	uint64_t waited_for;
	int32_t retval;
	volatile bool there;
	uint32_t next;
} fastrpc = { .next = 1 };

static redwood_fastrpc_answer_t fastrpc_answers[16];       /* by the low bits of the context: the DSP's domain */

void
redwood_adsp_fastrpc_answers(uint32_t domain, redwood_fastrpc_answer_t handler)
{
	fastrpc_answers[domain & 0xf] = handler;
}

int
redwood_adsp_fastrpc_send(int32_t pid, int32_t tid, uint64_t context, uint32_t handle, uint32_t scalars, uint64_t address,
    uint64_t size)
{
	struct fastrpc_message message = { .pid = pid, .tid = tid, .context = context, .handle = handle, .scalars = scalars,
		                           .address = address, .size = size };

	if (!glink.up || !glink.channel[CHANNEL_FASTRPC].open) {
		return ENXIO;
	}
	return glink_send(&glink.channel[CHANNEL_FASTRPC], &message, sizeof(message));
}

static void
fastrpc_input(const uint8_t *data, uint32_t length)
{
	uint64_t context;
	int32_t retval;

	if (length < 12) {
		printf("adsp: fastrpc: < %u bytes, not an answer\n", length);
		return;
	}
	memcpy(&context, data, 8);
	memcpy(&retval, data + 8, 4);
	if (fastrpc_answers[context & 0xf] != NULL) {
		fastrpc_answers[context & 0xf](context, retval);
		return;
	}
	printf("adsp: fastrpc: < the answer to context %#llx: %d\n", context, retval);
	if ((context & 0xff0) == (fastrpc.waited_for & 0xff0)) {
		fastrpc.retval = retval;
		fastrpc.there = true;
	}
}

bool
redwood_adsp_fastrpc(uint32_t domain, uint32_t handle, uint32_t scalars, uint64_t address, uint64_t size, uint32_t ms,
    int32_t *retval)
{
	struct fastrpc_message message = { .pid = 0, .tid = 1, .handle = handle, .scalars = scalars, .address = address,
		                           .size = size };
	int error;

	if (!glink.up || !glink.channel[CHANNEL_FASTRPC].open) {
		printf("adsp: fastrpc: the channel is not open\n");
		return false;
	}
	message.context = (uint64_t)(fastrpc.next << 4) | (domain & 0xf);
	fastrpc.next = fastrpc.next % 255 + 1;
	fastrpc.there = false;
	fastrpc.waited_for = message.context;
	printf("adsp: fastrpc: > context %#llx, handle %u, scalars %#x, arguments at %#llx (%llu bytes)\n", message.context, handle,
	    scalars, address, size);
	error = glink_send(&glink.channel[CHANNEL_FASTRPC], &message, sizeof(message));
	if (error != 0) {
		printf("adsp: fastrpc: NOT SENT (%d)\n", error);
		return false;
	}
	for (uint32_t waited = 0; waited < ms; waited++) {
		if (fastrpc.there) {
			*retval = fastrpc.retval;
			return true;
		}
		sleep_ms(1);
	}
	printf("adsp: fastrpc: NO ANSWER in %u ms\n", ms);
	return false;
}

/*
 * Memory handed from one set of virtual machines to another by the hypervisor, through TrustZone (Linux:
 * qcom_scm_assign_mem(), SCM_SVC_MP 0x0c, command 0x16, seven arguments): the region, the machines it is taken
 * from, and those it is given to with their rights. The three lists lie in one page of ours, each at 64 bytes'
 * alignment; the arguments beyond the third in a buffer of their own (x5), as Linux's qcom_scm_call() has it.
 */
#define SCM_MP_ASSIGN           SCM_CALL(0x0c, 0x16)
#define SCM_ASSIGN_ARGUMENTS    (7 | 1 << 4 | 0 << 6 | 1 << 8 | 0 << 10 | 1 << 12 | 0 << 14 | 0 << 16)

/*
 * SHM bridges (Linux: qcom_tzmem.c, qcom_scm_shm_bridge_enable()/_create()/_delete(), SCM_SVC_MP 0x1c, 0x1e, 0x1d):
 * on SM8650 Linux hands TrustZone pointers only into memory it has first registered as a bridge, HLOS reading and
 * writing it. The twenty-sixth run: every assign_mem, of any page to any machine, answered -2 (invalid argument) with
 * our lists in a page of ours that is no bridge. redwood_adsp_assign_bridge(true): the lists' page made one first.
 */
#define SCM_MP_SHM_BRIDGE_ENABLE        SCM_CALL(0x0c, 0x1c)
#define SCM_MP_SHM_BRIDGE_DELETE        SCM_CALL(0x0c, 0x1d)
#define SCM_MP_SHM_BRIDGE_CREATE        SCM_CALL(0x0c, 0x1e)
#define SCM_PERM_RW                     (0x4 | 0x2)
#define SCM_BRIDGE_ONE_VM               (1ull << 9)

static bool assign_bridge, bridges_enabled;
int redwood_adsp_bridge_result, redwood_adsp_bridge_enable_result = 1;

void
redwood_adsp_assign_bridge(bool on)
{
	assign_bridge = on;
}

static int32_t
scm4(uint64_t function, uint64_t arguments, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t *r1, uint64_t *r2)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	for (int i = 0; i < 20000; i++) {
		if (i > 0) {
			uint64_t until = now_us() + 50;

			while (now_us() < until) {
			}
		}
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = a0;
		register uint64_t x3 __asm__("x3") = a1;
		register uint64_t x4 __asm__("x4") = a2;
		register uint64_t x5 __asm__("x5") = a3;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			*r1 = x1;
			*r2 = x2;
			return (int32_t)x0;
		}
	}
	return -1;
}

/* The page at `base' made a bridge (HLOS reads and writes it): 0 and its handle, or what TrustZone says. */
static int
bridge_create(uint64_t base, uint64_t *handle)
{
	uint64_t r1 = 0, r2 = 0;
	int32_t result = scm4(SCM_MP_SHM_BRIDGE_CREATE, 4, base | SCM_PERM_RW, base | SCM_PERM_RW, 4096 | SCM_BRIDGE_ONE_VM,
	    3 /* HLOS */, &r1, &r2);

	printf("adsp: shm bridge: create at %#llx: TrustZone returns %d, answer %#llx, handle %#llx\n", base, result, r1, r2);
	if (result == 0 && (int32_t)r1 != 0) {
		result = (int32_t)r1;
	}
	*handle = r2;
	return result;
}

static int
bridge_make(uint64_t base, uint64_t *handle)
{
	int result = bridge_create(base, handle);

	if (result != 0 && !bridges_enabled) {
		uint64_t r1 = 0, r2 = 0;
		int32_t enable = scm4(SCM_MP_SHM_BRIDGE_ENABLE, 0, 0, 0, 0, 0, &r1, &r2);

		redwood_adsp_bridge_enable_result = enable != 0 ? enable : (int32_t)r1;
		printf("adsp: shm bridge: enable: TrustZone returns %d, answer %#llx\n", enable, r1);
		bridges_enabled = true;
		result = bridge_create(base, handle);
	}
	return result;
}

int
redwood_adsp_assign(uint64_t physical, uint64_t size, const uint32_t *from, unsigned from_count, const uint32_t *to,
    unsigned to_count)
{
	uint64_t bridge = 0;
	bool bridged = false;
	vm_offset_t page = 0;
	uint8_t *p;
	uint64_t base, *extra;
	uint32_t from_bytes = 4 * from_count, to_bytes = 24 * to_count;
	int32_t result = -1;

	if (from_count == 0 || to_count == 0 || from_count > 8 || to_count > 8 ||
	    kmem_alloc(kernel_map, &page, 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return -1;
	}
	p = (uint8_t *)page;
	base = kvtophys(page);
	/* at 0: the machines it is taken from; at 64: the region; at 128: the machines it is given to; at 512: x5's */
	memcpy(p, from, from_bytes);
	((uint64_t *)(p + 64))[0] = physical;
	((uint64_t *)(p + 64))[1] = size;
	for (unsigned i = 0; i < to_count; i++) {
		uint32_t *perm = (uint32_t *)(p + 128 + 24 * i);

		perm[0] = to[i];
		perm[1] = 7;                    /* read, write, execute */
	}
	extra = (uint64_t *)(p + 512);
	extra[0] = from_bytes;                  /* the size of the list it is taken from */
	extra[1] = base + 128;                  /* those it is given to */
	extra[2] = to_bytes;
	extra[3] = 0;
	for (vm_offset_t line = page; line < page + 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	if (assign_bridge) {
		redwood_adsp_bridge_result = bridge_make(base, &bridge);
		bridged = redwood_adsp_bridge_result == 0;
	}
	printf("adsp: about to ask TrustZone to hand %#llx (%llu KB) from machine %u to %u machines (the first %#x)\n", physical,
	    size / 1024, from[0], to_count, to[0]);
	{
		uint64_t session = 0;

		for (int i = 0; i < 20000; i++) {
			register uint64_t x0 __asm__("x0") = i == 0 ? (SCM_MP_ASSIGN | SCM_64 | SCM_SIP) : SCM_INTERRUPTED;
			register uint64_t x1 __asm__("x1") = SCM_ASSIGN_ARGUMENTS;
			register uint64_t x2 __asm__("x2") = base + 64;         /* the region's list */
			register uint64_t x3 __asm__("x3") = 16;
			register uint64_t x4 __asm__("x4") = base;              /* the machines it is taken from */
			register uint64_t x5 __asm__("x5") = base + 512;        /* the rest: from's size, to, to's size, 0 */
			register uint64_t x6 __asm__("x6") = session;
			register uint64_t x7 __asm__("x7") = 0;

			if (i > 0) {
				uint64_t until = now_us() + 50;

				while (now_us() < until) {
				}
			}
			__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
			    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
			session = x6;
			if (x0 != SCM_INTERRUPTED) {
				result = (int32_t)x0;
				printf("adsp: TrustZone returns %d (answer %#llx) for the hand-over\n", result, x1);
				break;
			}
		}
	}
	if (bridged) {
		uint64_t r1 = 0, r2 = 0;
		int32_t gone = scm4(SCM_MP_SHM_BRIDGE_DELETE, 1, bridge, 0, 0, 0, &r1, &r2);

		printf("adsp: shm bridge: delete %#llx: TrustZone returns %d, answer %#llx\n", bridge, gone, r1);
		if (gone != 0 || (int32_t)r1 != 0) {
			return result;          /* (the page stays a bridge: never freed, never used for anything else) */
		}
	}
	kmem_free(kernel_map, page, 4096);
	return result;
}

/* A packet from the DSP's audio services. From the link's thread. */
static void
apr_input(const uint8_t *data, uint32_t length)
{
	struct apr_header header;
	uint32_t header_length, payload;

	if (gpr) {
		gpr_input(data, length);
		return;
	}

	if (length < sizeof(header)) {
		printf("adsp: apr: %u bytes, no APR packet\n", length);
		return;
	}
	memcpy(&header, data, sizeof(header));
	header_length = ((header.field >> 4) & 0xf) * 4;
	if (header_length < sizeof(header) || header_length > length) {
		printf("adsp: apr: a header of %u bytes in a packet of %u: dropped\n", header_length, length);
		return;
	}
	payload = (header.size < length ? header.size : length) - header_length;
	apr_in++;
	if (header.token != apr_waited_for || apr_waited_for == 0) {
		redwood_apr_events_t events = apr_events;
		uint32_t words[64];

		bzero(words, sizeof(words));
		memcpy(words, data + header_length, payload < sizeof(words) ? payload : sizeof(words));
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
		events = apr_events_second;
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
	}
	if (apr_in <= 40) {
		uint32_t w[4] = { 0, 0, 0, 0 };

		memcpy(w, data + header_length, payload < sizeof(w) ? payload : sizeof(w));
		printf("adsp: apr: < from service %u of domain %u (port %u) to %u of %u (port %u): opcode %#x, token %u, %u bytes: %#x %#x "
		    "%#x %#x\n", header.from_service, header.from_domain, header.from_port, header.to_service, header.to_domain,
		    header.to_port, header.opcode, header.token, payload, w[0], w[1], w[2], w[3]);
	}
	bzero(apr_answer.words, sizeof(apr_answer.words));
	memcpy(apr_answer.words, data + header_length, payload < sizeof(apr_answer.words) ? payload : sizeof(apr_answer.words));
	apr_answer.length = payload;
	apr_answer.opcode = header.opcode;
	apr_answer.token = header.token;
	apr_answer.there = true;
}

/*
 * A command to one of the DSP's services (and its answer waited for, with ms): true when it was
 * sent and, if waited for, answered; the answer is in apr_answer and, if asked for, in *answer.
 * The port is the service's own numbering of what the command is for (ASM: session << 8 | stream).
 */
bool
redwood_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t ms, struct redwood_apr_answer *answer)
{
	uint8_t packet[sizeof(struct apr_header) + 256];
	struct apr_header *header = (struct apr_header *)packet;
	bool answered = false;
	int error;

	if (length > sizeof(packet) - sizeof(*header) || !glink.channel[CHANNEL_APR].open) {
		return false;
	}
	bzero(packet, sizeof(packet));
	header->field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, sizeof(*header));
	header->size = (uint16_t)(sizeof(*header) + length);
	header->from_service = service;
	header->from_domain = APR_DOMAIN_APPS;
	header->from_port = port;
	header->to_service = service;
	header->to_domain = APR_DOMAIN_ADSP;
	header->to_port = port;
	header->opcode = opcode;
	if (length != 0) {
		memcpy(packet + sizeof(*header), payload, length);
	}
	if (ms == 0) {
		/* (sent and not waited for: what comes back is the events' handler's) */
		header->token = token;
		error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
		apr_out += error == 0;
		return error == 0;
	}
	lck_mtx_lock(&apr_lock);
	if (token == 0) {
		token = apr_next_token++;
		if (apr_next_token >= 0x01000000u) {
			apr_next_token = 1;
		}
	}
	header->token = token;
	apr_answer.there = false;
	apr_waited_for = token;
	error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
	if (error != 0) {
		printf("adsp: apr: > opcode %#x to service %u: NOT SENT (%d)\n", opcode, service, error);
	} else {
		apr_out++;
		for (uint32_t waited = 0; waited < ms && !answered; waited++) {
			if (apr_answer.there && apr_answer.token == token) {
				answered = true;
				if (answer != NULL) {
					answer->opcode = apr_answer.opcode;
					answer->token = apr_answer.token;
					answer->length = apr_answer.length;
					memcpy(answer->words, apr_answer.words, sizeof(answer->words));
				}
				break;
			}
			sleep_ms(1);
		}
	}
	apr_waited_for = 0;
	lck_mtx_unlock(&apr_lock);
	return answered;
}

void
redwood_adsp_apr_events(redwood_apr_events_t handler)
{
	apr_events = handler;
}

void
redwood_adsp_apr_events_second(redwood_apr_events_t handler)
{
	apr_events_second = handler;
}

/* A command to one of the DSP's services, and its answer waited for: true, and the answer in apr_answer, if it came. */
static bool
apr_ask(uint8_t service, uint32_t opcode, const void *payload, uint32_t length, uint32_t ms)
{
	return redwood_adsp_apr(service, 0, 0, opcode, payload, length, ms, NULL);
}

/* ---- the battery manager, on the channel PMIC_RTR_ADSP_APPS ------------------------------------------ */

/*
 * On this SoC the audio DSP also runs the battery's and the charger's management -- the fuel gauge,
 * the charging -- and the operating system only asks it how things are: Android's
 * qti_battery_charger.c, Linux's qcom_battmgr.c over pmic_glink.c, on the channel PMIC_RTR_ADSP_APPS
 * that the DSP opens. A message is a header (owner 32778, the battery manager; type 1, request and
 * answer, or 2, a notice; an opcode) and its words; a property is asked for with opcode 0x30 (the
 * battery's) or 0x32 (the USB port's), the battery's number (0), the property's number and a word
 * that a get does not use, and is answered with the property's number, its value and a result (0:
 * good). Only gets are sent from here: nothing is set, no limit, no current, no notice asked for.
 * With adsp=1 (battmgr=0: not) it is asked every BATTMGR_SECONDS while the DSP runs, and what it says
 * goes to bsd/dev/redwood_battery.c (redwood_battery_report()), and from there to the power source
 * that iOS reads (iokit/Kernel/RedwoodBattery.cpp). battmgr=2: every property once, for the log.
 * Units: capacity in hundredths of a percent, voltage in uV, current in uA (more than 0 while the
 * battery takes charge), the temperature in hundredths of a degree, charges in uAh.
 * The properties' numbers are this phone's firmware's, read with battmgr=2 and checked against what
 * Android shows at the same moment (dumpsys battery, /sys/class/power_supply/battery): up to 9 they
 * are those of Linux's qcom_battmgr.c (sm8350), from there on one more property comes before the
 * temperature, so the temperature is 13, not 12, and the charge counter, the cycles, the designed and
 * the learnt full charge are 15 to 18.
 * The Xiaomi 14 Pro (shennong) has the same numbers and units: its Android's qti_battery_charger.c
 * (MiCode/Xiaomi_Kernel_OpenSource, shennong-u-oss) has BATT_CONSTANT_CURRENT as 12, BATT_TEMP 13,
 * BATT_CHG_COUNTER 15 .. BATT_CHG_FULL 18, owner 32778, gets 0x30 and 0x32, the capacity divided by
 * 100 and the temperature by 10 for Android. Its fuel gauge is Xiaomi's, behind the DSP (dmesg:
 * "BATTERY_CHG: [ADSP] fg_update_status"); the notices 0x50 and 0x80 seen on its channel are Xiaomi's.
 * There it is asked only with boot-arg battmgr=1 (2: and the dump), see shennong_battmgr below.
 */
#define BATTMGR_OWNER           32778
#define BATTMGR_REQUEST         1
#define BATTMGR_NOTICE          2
#define BATTMGR_BATTERY_GET     0x30
#define BATTMGR_USB_GET         0x32
#define BATTMGR_XM_GET          0x50            /* Xiaomi's own properties (shennong) */
#define BATTMGR_XM_SET          0x51
/*
 * The numbers of Xiaomi's properties are those of this phone's firmware (HyperOS 3), not of the
 * shennong-u-oss source (there they differ from 3 on): taken from the phone's own
 * qti_battery_charger.ko, whose sysfs show/store functions each ask for one number, and checked
 * against battmgr=4 on the screen (2 and 4 the connector's temperatures, 42 the pumps' 8.9 V on PD).
 */
#define XM_CONNECTOR_TEMP       2               /* tenths of a degree: 400 when Android says 40.0 */
#define XM_REAL_TYPE            9               /* Linux's POWER_SUPPLY_USB_TYPE_*: 6 PD, 8 PD_PPS (after the check) */
#define XM_VERIFY_PROCESS       20
#define XM_ADAPTER_SVID         31              /* 0x2717: Xiaomi's own adapter */
#define XM_PD_VERIFED           32              /* 1 once the charger counts as Xiaomi's own: the DSP then goes to PPS and the pumps */
#define XM_CP_MODE              36              /* the charge pumps' mode (5 on 120 W in Android) */
#define XM_BUS_CURRENT          39              /* the master pump's input, mA */
#define XM_SLAVE_BUS_CURRENT    40
#define XM_BUS_VOLTAGE          42              /* mV */
#define XM_FASTCHGMODE          55
#define XM_APDO_MAX             61              /* the charger's largest PPS offer, W */
#define XM_POWER_MAX            62              /* what the phone will take from it, W */
#define BATTMGR_USB_SET         0x33
#define USB_TYPE_PD             6               /* USB_REAL_TYPE's values: Linux's POWER_SUPPLY_USB_TYPE_* */
#define USB_TYPE_PD_PPS         8
#define CHG_MAX_ICL             3000000         /* uA: what a 9 V PD charger offers (Xiaomi's 120 W one: 9 V x 3 A) */
#define CHG_SAFE_ICL            1500000         /* uA: the DSP's own, if it was never read */
#define CHG_HOT                 4200            /* battery hundredths of a degree: from here on back to the DSP's limit */
#define CHG_COOL                3800            /* and 3 A again only below this */
#define CHG_CONNECTOR_HOT       600             /* the connector, tenths of a degree */
#define USB_INPUT_CURR_LIMIT    5               /* uA */
#define USB_REAL_TYPE           11
#define BATT_STATUS             0               /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
#define BATT_HEALTH             1
#define BATT_PRESENT            2
#define BATT_CAPACITY           4
#define BATT_VOLT_NOW           7
#define BATT_CURR_NOW           9
#define BATT_CHG_CTRL_LIM       10              /* the charge's thermal level, which Android's thermal daemon sets: 0 none */
#define BATT_CHG_CTRL_LIM_MAX   11              /* its highest level (15 on shennong) */
#define BATT_CONSTANT_CURRENT   12              /* Xiaomi's: the battery's constant charge current, uA (2000000 at rest) */
#define BATT_TEMP               13              /* hundredths of a degree: 3900 when Android says 39.0 */
#define BATT_CHG_COUNTER        15              /* the charge in the battery now, uAh */
#define BATT_CYCLE_COUNT        16
#define BATT_CHG_FULL_DESIGN    17              /* 5000000 uAh */
#define BATT_CHG_FULL           18              /* the full charge as the fuel gauge has learnt it, uAh */
#define USB_VOLT_NOW            1               /* uV */
#define USB_CURR_NOW            3               /* uA, what the port gives */
#define USB_TYPE                6               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 here, a computer's port */
#define USB_ONLINE              0
#define BATTMGR_SECONDS         10

/*
 * Xiaomi's firmware (shennong) gives the battery's current the other way round: more than 0 while the
 * battery gives charge. Seen on the phone: +2.9 A with 5.18 V and 0.36 A at the USB port (1.8 W in,
 * the battery could not take 10 W), and the charge counter falling by 33 mAh in 41 s, 2.9 A out of it.
 * There the sign is turned, and "charging" (status 1) with charge going out is said as 3, not charging:
 * the charger is on, but what the port gives is less than the phone takes.
 */
static bool battmgr_current_out;
void redwood_overlay_text(unsigned line, const char *text);     /* iokit/Kernel/MiniIOMobileFramebuffer.cpp */
void redwood_overlay_redraw(void);
void redwood_overlay_scale(unsigned scale);
static uint32_t battmgr_mode;            /* the boot-arg battmgr: 1, 2 (and every property once), 3 (and the charger every round) */

struct battmgr_message {
	uint32_t owner, type, opcode;
	uint32_t words[3];                      /* asked: battery, property, value; answered: property, value, result */
};

static struct {
	volatile uint32_t answers, notices;
	uint32_t opcode, property, value, result, length;
	bool wanted;
	volatile bool runs;
} battmgr;

static void
battmgr_input(const uint8_t *data, uint32_t length)
{
	struct battmgr_message m;

	bzero(&m, sizeof(m));
	memcpy(&m, data, length < sizeof(m) ? length : sizeof(m));
	if (m.owner != BATTMGR_OWNER || length < 12) {
		printf("adsp: battmgr: < %u bytes of owner %u, type %u, opcode %#x: not the battery manager's\n", length, m.owner,
		    m.type, m.opcode);
		return;
	}
	if (m.type == BATTMGR_NOTICE) {
		if (battmgr.notices++ < 8) {
			tell("adsp: battmgr: < a notice, opcode %#x, %u bytes (%#x %#x %#x)\n", m.opcode, length, m.words[0], m.words[1],
			    m.words[2]);
		}
		return;
	}
	battmgr.opcode = m.opcode;
	battmgr.property = m.words[0];
	battmgr.value = m.words[1];
	battmgr.result = m.words[2];
	battmgr.length = length;
	os_atomic_inc(&battmgr.answers, release);
}

/* One property asked for (or set) and its answer waited for (a second at most); one thread asks at a time (battmgr_thread). */
static bool
battmgr_ask(uint32_t opcode, uint32_t property, uint32_t set, uint32_t *value)
{
	struct battmgr_message q = { BATTMGR_OWNER, BATTMGR_REQUEST, opcode, { 0, property, set } };
	uint32_t before = os_atomic_load(&battmgr.answers, acquire);

	if (glink_send(&glink.channel[CHANNEL_PMIC], &q, sizeof(q)) != 0) {
		return false;
	}
	for (unsigned ms = 0; ms < 1000; ms++) {
		if (os_atomic_load(&battmgr.answers, acquire) != before) {
			if (battmgr.opcode != opcode || battmgr.property != property || battmgr.length < sizeof(struct battmgr_message)) {
				before = battmgr.answers;       /* (somebody else's answer: wait on) */
				continue;
			}
			if (battmgr.result != 0) {
				return false;
			}
			*value = battmgr.value;
			return true;
		}
		sleep_ms(1);
	}
	return false;
}

static bool
battmgr_get(uint32_t opcode, uint32_t property, uint32_t *value)
{
	return battmgr_ask(opcode, property, 0, value);
}

static bool
battmgr_set(uint32_t opcode, uint32_t property, uint32_t value)
{
	uint32_t answer = 0;

	return battmgr_ask(opcode, property, value, &answer);
}

/*
 * Boot-arg chg_max=1 (shennong): the fastest charge that a charger gives without Xiaomi's check.
 * Xiaomi's 120 W needs its adapter to check the phone first (only then does the adapter offer
 * 20 V x 6 A, pdo2 0x64258 under Android; here 9 V x 3 A, 0x2D12C), and that check is done by
 * Xiaomi's closed daemon: setting pd_verifed here was tried and the adapter went on
 * offering 9 V x 3 A. What is left is the input current: without Android nothing raises it from
 * the DSP's 1.5 A, so a 9 V PD charger gave 12 W. Here it is set to 3 A, as Android's
 * usb_psy_set_icl does (USB set 0x33, property 5, uA; the DSP takes it on SDP, CDP and PD) and
 * the DSP keeps to what the charger offers; only on PD (a computer's port is left as it is). Our guard: with the battery at 42 degrees or the
 * connector at 60, back to the DSP's own limit, and 3 A again only below 38. Every round (10 s);
 * what it did on the screen's line 8 (battmgr=3).
 */
static bool chg_max;

static void
chg_max_round(uint32_t usb, uint32_t temp, char *line, size_t size)
{
	static bool hot;
	static uint32_t own;            /* the DSP's own limit, as first read */
	uint32_t limit = 0, connector = 0, real = 0, want;

	if (!chg_max) {
		return;
	}
	if (!usb) {
		own = 0;                /* (the next charger: its own limit read again) */
		snprintf(line, size, "MAX: no charger");
		return;
	}
	if (!battmgr_get(BATTMGR_USB_GET, USB_INPUT_CURR_LIMIT, &limit)) {
		snprintf(line, size, "MAX: input limit NOT READ");
		return;
	}
	if (own == 0 && limit != CHG_MAX_ICL) {
		own = limit;
	}
	(void)battmgr_get(BATTMGR_USB_GET, USB_REAL_TYPE, &real);
	if (real != USB_TYPE_PD && real != USB_TYPE_PD_PPS) {
		/* (a computer's port or a plain charger: left to the DSP, as USB says) */
		snprintf(line, size, "MAX: not PD (type %u), %u mA left as it is", real, limit / 1000);
		return;
	}
	(void)battmgr_get(BATTMGR_XM_GET, XM_CONNECTOR_TEMP, &connector);
	if ((int)temp >= CHG_HOT || (int)connector >= CHG_CONNECTOR_HOT) {
		hot = true;
	} else if ((int)temp < CHG_COOL && (int)connector < CHG_CONNECTOR_HOT - 50) {
		hot = false;
	}
	want = hot ? (own ? own : CHG_SAFE_ICL) : CHG_MAX_ICL;
	if (limit != want) {
		bool ok = battmgr_set(BATTMGR_USB_SET, USB_INPUT_CURR_LIMIT, want);

		tell("adsp: battmgr: charge: battery %d.%d C, connector %u.%u C: input limit %u -> %u uA (%s)\n", (int)temp / 100,
		    ((int)temp / 10) % 10, connector / 10, connector % 10, limit, want, ok ? "set" : "NOT SET");
		snprintf(line, size, "MAX: %s%u -> %u mA %s", hot ? "HOT " : "", limit / 1000, want / 1000, ok ? "set" : "NOT SET");
		return;
	}
	snprintf(line, size, "MAX: %s%u mA  %d.%d C / %u.%u C", hot ? "HOT, " : "", limit / 1000, (int)temp / 100,
	    ((int)temp / 10) % 10, connector / 10, connector % 10);
}

static void
battmgr_thread(__unused void *parameter, __unused wait_result_t result)
{
	battmgr.runs = true;
	for (unsigned ms = 0; ms < 15000 && battmgr.wanted && !glink.channel[CHANNEL_PMIC].open; ms += 10) {
		sleep_ms(10);
	}
	if (!glink.channel[CHANNEL_PMIC].open) {
		tell("adsp: battmgr: the DSP has not opened PMIC_RTR_ADSP_APPS: nothing is asked\n");
		battmgr.runs = false;
		thread_terminate(current_thread());
	}
	{
		/* boot-arg battmgr=2: every property once, for the log (which number is which on this phone's firmware) */
		uint32_t mode = 1;

		PE_parse_boot_argn("battmgr", &mode, sizeof(mode));
		battmgr_mode = mode;
		{
			uint32_t max = 0;

			chg_max = battmgr_current_out && PE_parse_boot_argn("chg_max", &max, sizeof(max)) && max != 0;
			if (chg_max) {
				tell("adsp: battmgr: chg_max=1: input limit 3 A (our own 42 C guard)\n");
			}
		}
		for (uint32_t p = 0; mode == 2 && p < 32; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_BATTERY_GET, p, &v);

			tell("adsp: battmgr: battery property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
		for (uint32_t p = 0; mode == 2 && p < 13; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_USB_GET, p, &v);

			tell("adsp: battmgr: USB property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
		for (uint32_t p = 0; mode == 2 && battmgr_current_out && p < 50; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_XM_GET, p, &v);

			tell("adsp: battmgr: Xiaomi property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
	}
	for (unsigned round = 0; battmgr.wanted && glink.up && glink.channel[CHANNEL_PMIC].open; round++) {
		struct redwood_battery_state b = { .size = sizeof(b) };
		uint32_t capacity = 0, status = 0, present = 0, volt = 0, curr = 0, temp = 0, usb = 0;
		uint32_t usb_type = 0, usb_volt = 0, usb_curr = 0, charge = 0;
		static uint32_t full, design, cycles;
		bool good = battmgr_get(BATTMGR_BATTERY_GET, BATT_CAPACITY, &capacity) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_STATUS, &status) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_PRESENT, &present) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_VOLT_NOW, &volt) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_CURR_NOW, &curr) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_TEMP, &temp) &&
		    battmgr_get(BATTMGR_USB_GET, USB_ONLINE, &usb);

		if (good) {
			/* (what may be missing without harm, and the capacities and the cycles only now and then: they hardly change) */
			(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_COUNTER, &charge);
			if (usb) {
				(void)battmgr_get(BATTMGR_USB_GET, USB_TYPE, &usb_type);
				(void)battmgr_get(BATTMGR_USB_GET, USB_VOLT_NOW, &usb_volt);
				(void)battmgr_get(BATTMGR_USB_GET, USB_CURR_NOW, &usb_curr);
			}
			if (round % 30 == 0 || full == 0) {
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL, &full);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL_DESIGN, &design);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CYCLE_COUNT, &cycles);
			}
			b.percent_hundredths = (int32_t)capacity;
			b.voltage_uv = (int32_t)volt;
			b.current_ua = battmgr_current_out ? -(int32_t)curr : (int32_t)curr;
			b.temperature_hundredths = (int32_t)temp;
			b.present = present != 0;
			b.external = usb != 0;
			b.status = (int32_t)status;
			if (b.status == 1 && b.current_ua < -50000) {
				b.status = 3;   /* POWER_SUPPLY_STATUS_NOT_CHARGING: on a charger, and still losing charge */
			}
			if (battmgr_current_out && b.status == 3 && b.current_ua < -50000) {
				/* (iOS draws the bolt for any charger that is there: one that gives less than the phone takes is said as none) */
				b.external = 0;
			}
			b.charge_uah = (int32_t)charge;
			b.full_uah = (int32_t)full;
			b.design_uah = (int32_t)design;
			b.cycles = (int32_t)cycles;
			b.usb_type = (int32_t)usb_type;
			b.usb_voltage_uv = (int32_t)usb_volt;
			b.usb_current_ua = (int32_t)usb_curr;
			redwood_battery_report(&b);
		}
		if (round < 3 || round % 6 == 0 || !good || battmgr_mode == 3) {
			tell("adsp: battmgr: %s capacity %u (hundredths of a percent), status %u, present %u, %d uV, %d uA, temperature %d "
			    "(hundredths of a degree), charge %u uAh, USB online %u (type %u, %u uV, %u uA); to iOS: %d mA, status %d\n",
			    good ? "the DSP says:" : "NO ANSWER to all of", capacity, status, present, (int)volt, (int)curr, (int)temp,
			    charge, usb, usb_type, usb_volt, usb_curr, b.current_ua / 1000, b.status);
			if (round % 30 == 0 && good) {
				tell("adsp: battmgr: full charge %u uAh, as designed %u uAh, %u cycles\n", full, design, cycles);
			}
		}
		if (battmgr_current_out && good && battmgr_mode == 4) {
			/*
			 * battmgr=4: Xiaomi's properties 0..99 on the screen, every 10 s, small (only gets). The phone's firmware is
			 * newer than the source their numbers were taken from (shennong-u-oss), and some of them do not fit what they
			 * say (a "quick charge type" of 4026): which number is which is read off against Android's sysfs
			 * (/sys/class/qcom-battery) on the same charger.
			 */
			char line[96];
			int n = 0;

			redwood_overlay_scale(2);
			snprintf(line, sizeof(line), "t %llu  BAT %d%% %dmV %dmA  USB %u t%u %umV %umA", now_us() / 1000000,
			    (int)(capacity + 50) / 100, (int)volt / 1000, b.current_ua / 1000, usb, usb_type, usb_volt / 1000, usb_curr / 1000);
			redwood_overlay_text(0, line);
			line[0] = 0;
			for (uint32_t p = 0; p < 100; p++) {
				uint32_t v = 0;
				bool ok = battmgr_get(BATTMGR_XM_GET, p, &v);

				if (ok) {
					n += snprintf(line + n, sizeof(line) - n, "%2u:%-10u ", p, v);
				} else {
					n += snprintf(line + n, sizeof(line) - n, "%2u:-          ", p);
				}
				if (p % 4 == 3) {
					redwood_overlay_text(1 + p / 4, line);
					line[0] = 0;
					n = 0;
				}
			}
		}
		if (battmgr_current_out && good && (usb || battmgr_mode == 3 || chg_max) && (round < 3 || round % 6 == 0 || battmgr_mode == 3 || chg_max)) {
			/* (shennong: how the charger is taken -- PD, PPS, the charge pumps, Xiaomi's check; battmgr=3: every round) */
			uint32_t real = 0, limit = 0, xm_real = 0, verified = 0, cp = 0, bus_ma = 0, bus_mv = 0, fast = 0, qc = 0, apdo = 0, pmax = 0;
			char fast_line[64] = "MAX: off (chg_max=1 for 3 A)";

			chg_max_round(usb, temp, fast_line, sizeof(fast_line));

			(void)battmgr_get(BATTMGR_USB_GET, USB_REAL_TYPE, &real);
			(void)battmgr_get(BATTMGR_USB_GET, USB_INPUT_CURR_LIMIT, &limit);
			(void)battmgr_get(BATTMGR_XM_GET, XM_REAL_TYPE, &xm_real);
			(void)battmgr_get(BATTMGR_XM_GET, XM_PD_VERIFED, &verified);
			(void)battmgr_get(BATTMGR_XM_GET, XM_CP_MODE, &cp);
			(void)battmgr_get(BATTMGR_XM_GET, XM_BUS_CURRENT, &bus_ma);
			(void)battmgr_get(BATTMGR_XM_GET, XM_BUS_VOLTAGE, &bus_mv);
			(void)battmgr_get(BATTMGR_XM_GET, XM_FASTCHGMODE, &fast);
			(void)battmgr_get(BATTMGR_XM_GET, XM_SLAVE_BUS_CURRENT, &qc);
			(void)battmgr_get(BATTMGR_XM_GET, XM_APDO_MAX, &apdo);
			(void)battmgr_get(BATTMGR_XM_GET, XM_POWER_MAX, &pmax);
			tell("adsp: battmgr: charger: USB real type %u, input limit %u uA; Xiaomi: real type %u, PD verified %u, slave pump %u mA, "
			    "PPS offer %u W, power max %u W, charge pump mode %u (%u mV, %u mA), fast mode %u; port %u mV x %u mA = %u mW\n",
			    real, limit, xm_real, verified, qc, apdo, pmax, cp, bus_mv, bus_ma, fast, usb_volt / 1000, usb_curr / 1000,
			    (uint32_t)((uint64_t)usb_volt * usb_curr / 1000000000ULL));
			if (battmgr_mode == 3) {
				/* (and on the screen, over iOS: away from the Mac nothing of the log comes back -- the loader's USB does not survive the cable) */
				char line[64];

				snprintf(line, sizeof(line), "BATTERY %d%%  %d mV  %d mA  %d.%d C", (int)(capacity + 50) / 100, (int)volt / 1000,
				    b.current_ua / 1000, (int)temp / 100, ((int)temp / 10) % 10);
				redwood_overlay_text(0, line);
				snprintf(line, sizeof(line), "USB %s type %u real %u  %u mV %u mA", usb ? "ON" : "off", usb_type, real, usb_volt / 1000,
				    usb_curr / 1000);
				redwood_overlay_text(1, line);
				snprintf(line, sizeof(line), "port %u mW  input limit %u mA", (uint32_t)((uint64_t)usb_volt * usb_curr / 1000000000ULL),
				    limit / 1000);
				redwood_overlay_text(2, line);
				snprintf(line, sizeof(line), "XM real %u  verified %u  slave pump %u mA", xm_real, verified, qc);
				redwood_overlay_text(3, line);
				snprintf(line, sizeof(line), "PPS offer %u W  power max %u W", apdo, pmax);
				redwood_overlay_text(4, line);
				snprintf(line, sizeof(line), "pump mode %u  %u mV  %u mA  fast %u", cp, bus_mv, bus_ma, fast);
				redwood_overlay_text(5, line);
				snprintf(line, sizeof(line), "t %llu s  round %u  status %u", now_us() / 1000000, round, status);
				redwood_overlay_text(6, line);
				redwood_overlay_text(7, fast_line);
				{
					/* what else may hold the input at 1.5 A: the charge's thermal level and the battery's own current */
					uint32_t lim = 0, lim_max = 0, cc = 0, type = 0;
					bool ok = battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_CTRL_LIM, &lim);

					(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_CTRL_LIM_MAX, &lim_max);
					(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CONSTANT_CURRENT, &cc);
					(void)battmgr_get(BATTMGR_BATTERY_GET, 3 /* BATT_CHG_TYPE */, &type);
					if (ok) {
						snprintf(line, sizeof(line), "THERM level %u/%u  CC %u mA  type %u", lim, lim_max, cc / 1000, type);
					} else {
						snprintf(line, sizeof(line), "THERM level NOT READ");
					}
					redwood_overlay_text(8, line);
					tell("adsp: battmgr: thermal level %u of %u, constant current %u uA, charge type %u\n", lim, lim_max, cc, type);
				}
			}
		}
		for (unsigned ms = 0; ms < BATTMGR_SECONDS * 1000 && battmgr.wanted && glink.up; ms += 50) {
			sleep_ms(50);
			if (battmgr_mode >= 3 && ms % 200 == 0) {
				redwood_overlay_redraw();       /* (the firmware's messages may have scrolled over it) */
			}
		}
	}
	battmgr.runs = false;
	thread_terminate(current_thread());
}

static void
battmgr_start(uint32_t wanted)  /* wanted: what it is without the boot-arg battmgr (redwood: 1, shennong: 0) */
{
	thread_t thread;

	PE_parse_boot_argn("battmgr", &wanted, sizeof(wanted));
	if (wanted == 0 || battmgr.runs) {
		return;
	}
	if (option("battery")) {
		/* (the loader's way, through Qualcomm's UEFI charger drivers: one way to the battery at a time) */
		tell("adsp: battmgr: not with battery=1: the battery is asked about the loader's way\n");
		return;
	}
	battmgr.wanted = true;
	if (kernel_thread_start(battmgr_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
battmgr_stop(void)
{
	battmgr.wanted = false;
	for (unsigned ms = 0; ms < 2000 && battmgr.runs; ms++) {
		sleep_ms(1);
	}
}

/* ---- the link's thread ----------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;

static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	dsp_words(&last);
	while (link_wanted) {
		sleep_us(1000);
		doorbells();
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				printf("adsp: link: THE AUDIO DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
			}
		}
		for (;;) {
			unsigned from = CHANNELS;

			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = NULL;
			for (unsigned c = 0; c < CHANNELS && packet == NULL; c++) {
				struct glink_channel *channel = &glink.channel[c];

				if ((packet = channel->first) != NULL) {
					channel->first = packet->next;
					channel->last = channel->first == NULL ? NULL : channel->last;
					from = c;
				}
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			if (from == CHANNEL_QRTR) {
				qrtr_input(packet->data, packet->length);
			} else if (from == CHANNEL_PMIC) {
				battmgr_input(packet->data, packet->length);
			} else if (from == CHANNEL_FASTRPC) {
				fastrpc_input(packet->data, packet->length);
			} else {
				apr_input(packet->data, packet->length);
			}
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
	}
	link_runs = false;
	thread_terminate(current_thread());
}

static void
link_stop(void)
{
	struct glink_packet *packet;

	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("adsp: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams, APR %u and %u "
		    "packets; its doorbells heard: GLINK %u, SMP2P %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
		    glink.heard, qrtr_out, qrtr_in, apr_out, apr_in, doorbell.glink, doorbell.smp2p);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = false;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->open = false;
		while ((packet = channel->first) != NULL) {
			channel->first = packet->next;
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		channel->last = NULL;
	}
	lck_mtx_unlock(&glink_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
}

/* ---- step 8: GLINK's queues, looked at -------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues, and read what the audio DSP has put into its own");
	if (!glink_find()) {
		printf("adsp: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("adsp: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("adsp: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 9: GLINK, spoken -------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!dsp_started || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the audio DSP: our version, then whatever its messages call for; a thread of ours listens "
	    "from here on, every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.broken = false;
	glink.said = glink.heard = 0;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->asked = channel->acknowledged = channel->open = false;
		channel->theirs = channel->our_intent_count = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	apr_in = apr_out = 0;
	audio_up = false;
	bzero(glink.others, sizeof(glink.others));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("adsp: %u: no thread\n", step);
		return false;
	}
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.channel[CHANNEL_QRTR].open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);
	printf("adsp: %u: %llu ms after our version: versions %s (features %#x); the channel \"IPCRTR\" is %s, \"%s\" %s; "
	    "the DSP also opened \"%s\" \"%s\" \"%s\" \"%s\"\n", step, (now_us() - began) / 1000, glink.agreed ? "agreed" :
	    "NOT AGREED", glink.features, glink.channel[CHANNEL_QRTR].open ? "OPEN" : "NOT OPEN", glink.channel[CHANNEL_APR].name,
	    glink.channel[CHANNEL_APR].open ? "OPEN" : "NOT OPEN", glink.others[0], glink.others[1], glink.others[2], glink.others[3]);
	return glink.channel[CHANNEL_QRTR].open;
}

/* ---- step 10: QRTR ---------------------------------------------------------------------------------- */

static bool
step_qrtr(void)
{
	unsigned services = 0;

	if (!glink.channel[CHANNEL_QRTR].open) {
		return false;
	}
	PE_parse_boot_argn("adsp_listen", &listen_seconds, sizeof(listen_seconds));
	announce("listen to QRTR on the channel: the DSP's hello (answered from the link's thread), and the services it tells of");
	for (unsigned ms = 0; ms < listen_seconds * 1000; ms++) {
		sleep_ms(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("adsp: %u: after %u s: %s; the DSP is node %u and has told of %u services; it looked for %u of ours; QRTR received %u "
	    "and sent %u datagrams\n", step, listen_seconds, qrtr_hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr_peer,
	    services, qrtr_lookups, qrtr_in, qrtr_out);
	return qrtr_hellos != 0;
}

/* ---- step 11: APR ---------------------------------------------------------------------------------- */

static bool
step_gpr(void)
{
	uint32_t wait_seconds = 10, asked = 0, state = 0;
	uint64_t began = now_us();

	PE_parse_boot_argn("adsp_apr_wait", &wait_seconds, sizeof(wait_seconds));
	announce("ask the audio DSP's APM over GPR (the channel adsp_apps) whether its audio framework is up, again and again "
	    "as q6apm.c does");
	for (unsigned ms = 0; ms < 3000 && !glink.channel[CHANNEL_APR].open; ms++) {
		sleep_ms(1);
	}
	if (!glink.channel[CHANNEL_APR].open) {
		printf("adsp: %u: the channel \"adsp_apps\" is not open: the DSP has not opened it\n", step);
		return false;
	}
	while (now_us() - began < wait_seconds * 1000000ull) {
		asked++;
		if (gpr_ask(GPR_APM_PORT, APM_CMD_GET_SPF_STATE, NULL, 0, 500) && apr_answer.opcode == APM_CMD_RSP_GET_SPF_STATE) {
			state = apr_answer.words[0];
			if (state == 1) {
				break;
			}
		}
		sleep_ms(200);
	}
	audio_up = state == 1;
	printf("adsp: %u: %s after %llu ms and %u questions (the last answer: opcode %#x, %u bytes, %#x %#x)\n", step, state == 1 ?
	    "THE AUDIO FRAMEWORK (SPF) IS UP" : "THE AUDIO FRAMEWORK DOES NOT SAY IT IS UP", (now_us() - began) / 1000, asked,
	    apr_answer.opcode, apr_answer.length, apr_answer.words[0], apr_answer.words[1]);
	return state == 1;
}

static bool
step_apr(void)
{
	uint32_t wait_seconds = 10, asked = 0, state = 0;
	uint64_t began = now_us();

	if (gpr) {
		return step_gpr();
	}
	PE_parse_boot_argn("adsp_apr_wait", &wait_seconds, sizeof(wait_seconds));
	announce("ask the audio DSP's core service over APR whether the audio services are up (again and again, as Android "
	    "does), then which services it has");
	for (unsigned ms = 0; ms < 3000 && !glink.channel[CHANNEL_APR].open; ms++) {
		sleep_ms(1);
	}
	if (!glink.channel[CHANNEL_APR].open) {
		printf("adsp: %u: the channel \"apr_audio_svc\" is not open: the DSP has not opened it\n", step);
		return false;
	}
	while (now_us() - began < wait_seconds * 1000000ull) {
		asked++;
		if (apr_ask(APR_SVC_ADSP_CORE, AVCS_CMD_ADSP_EVENT_GET_STATE, NULL, 0, 500) &&
		    apr_answer.opcode == AVCS_CMDRSP_ADSP_EVENT_GET_STATE) {
			state = apr_answer.words[0];
			if (state == 1) {
				break;
			}
		}
		sleep_ms(200);
	}
	audio_up = state == 1;
	printf("adsp: %u: %s after %llu ms and %u questions (the last answer: opcode %#x, %u bytes, %#x)\n", step, state == 1 ?
	    "THE AUDIO SERVICES ARE UP" : "THE AUDIO SERVICES DO NOT SAY THEY ARE UP", (now_us() - began) / 1000, asked,
	    apr_answer.opcode, apr_answer.length, apr_answer.words[0]);
	if (apr_ask(APR_SVC_ADSP_CORE, AVCS_GET_VERSIONS, NULL, 0, 1000)) {
		char line[400];
		int at = 0;

		for (uint32_t i = 0; i < apr_answer.length / 4 && i < 64 && at < (int)sizeof(line) - 12; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %#x", apr_answer.words[i]);
		}
		printf("adsp: %u: the services and their versions (opcode %#x, %u bytes):%s\n", step, apr_answer.opcode,
		    apr_answer.length, line);
	} else {
		printf("adsp: %u: no answer to the question of the services' versions\n", step);
	}
	return state == 1;
}

/* ---- the steps, and their end ----------------------------------------------------------------------- */

static bool (*const steps[STEPS])(void) = {
	step_survey, step_firmware, step_mailbox, step_header, step_image, step_our_item, step_start, step_queues, step_link,
	step_qrtr, step_apr,
};

static bool keep;                       /* the DSP and the link stay up while the system runs (adsp_keep, adsp=1) */

static void
steps_over(uint32_t done)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && done == last_step && keep) {
		printf("adsp: THE AUDIO DSP STAYS ON while the system runs%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* When the run ends in order: a DSP that runs is shut down, for the next kernel to start again. */
void
redwood_adsp_stop(void)
{
	battmgr_stop();
	shennong_sensors_dsp_down();            /* (the sensors' streams stopped while the DSP still listens, shennong_sensors.c) */
	shennong_snsrpc_dsp_down();             /* (the sensors' domain's FastRPC: its SMMU stream put back, shennong_snsrpc.c) */
	shennong_audiorpc_dsp_down();           /* (the audio domain's FastRPC: its SMMU stream put back, shennong_audiorpc.c) */
	redwood_sound_dsp_down();               /* the amplifiers, the stream and the port first, while the DSP still listens */
	shennong_mic_dsp_down();                /* (the Xiaomi 14 Pro's microphones: the PRM's clock and votes) */
	shennong_sound_dsp_down();              /* (the Xiaomi 14 Pro's: its graph, its clock and its pins) */
	redwood_mic_dsp_down();                 /* and what the microphones' path has asked for */
	lck_mtx_lock(&dsp_lock);
	if (dsp_started) {
		unsigned services = 0;
		uint32_t node = qrtr_peer;
		bool down;

		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			services += qrtr_servers[i].there;
		}
		down = dsp_shutdown("the run is about to end");
		if (down) {
			shennong_sound_dsp_gone();      /* (the audio domain's heap, handed to the DSP, taken back) */
		}
		/* (said again at the end: a long run's log keeps its beginning and its end, and the middle may be gone) */
		tell("adsp: the audio DSP is off: it had been up%s (node %u, %u services; audio services %s), it %s, and TrustZone %s\n",
		    node ? " and linked" : ", NOT LINKED", node, services, audio_up ? "up" : "NOT UP", stopped_by_its_word ?
		    "stopped when asked" : "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* The same on a panic's way back to the loader: one processor, interrupts off, no lock, no sleep. */
void
redwood_adsp_quiesce(void)
{
	redwood_sound_quiesce();                /* the amplifiers off, whatever the DSP's state */
	if (!dsp_started) {
		return;
	}
	leaving = true;
	new_from = ~0u;
	dsp_shutdown("the kernel is on its way back to the loader");
}

static void
run_steps(void)
{
	uint32_t done = 0;

	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("adsp: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("adsp: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;
	steps_over(done);
	printf("adsp: over after step %u; %s\n", done, last_write);
}

static void
whole_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t began = now_us();
	unsigned services = 0;

	/* (the firmware may be on the root file system, which is not mounted yet when this begins: a minute at most) */
	for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
	run_steps();
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	if (step == STEPS && dsp_started) {
		tell("adsp: THE AUDIO DSP IS UP AND LINKED, its audio services up (APR), %llu ms after the kernel began on it (the "
		    "root's waited for included): node %u, %u services; it stays on\n", (now_us() - began) / 1000, qrtr_peer, services);
		redwood_sound_dsp_up();         /* (boot-arg sound=N: the speakers' path, redwood_sound.c) */
		redwood_mic_dsp_up();           /* (boot-arg mic=N: the microphones' path, redwood_mic.c) */
		battmgr_start(1);               /* (how the battery is, from the DSP; battmgr=0: not asked) */
	} else {
		tell("adsp: THE AUDIO DSP DID NOT COME UP: step %u failed (%s); see the kernel's own log for the steps\n", step + 1,
		    last_write);
	}
	thread_terminate(current_thread());
}

/*
 * The Xiaomi 14 Pro (shennong, SM8650): the same DSP's way, at its own addresses (its device tree:
 * reserved-memory smem_region@81d00000, aop_cmd_db_region@81c60000, adspslpi_region@9df00000;
 * qcom,ipcc@406000, power-controller@c300000, rsc@17a00000 drv-2, syscon@1f40000 with hwlock 3;
 * qcom,smp2p-adsp with the same items 443/429 and remote pid 2, IPCC client 3 with signals 0 and 2).
 * What is different: SM8650's RPMh is version 3 (the TCS's registers are elsewhere, Linux's
 * rsc_reg_offset_ver_3_0), and its DSP takes a second image before its own, adsp_dtb.mdt (PAS id
 * 0x24, into q6_adsp_dtb_region@9de80000). Step 11 speaks GPR (AudioReach) on "adsp_apps" instead of
 * APR on "apr_audio_svc" (see gpr above). The steps wait for the root to be mounted
 * (the firmware is in the system image's /fw), on a thread of their own.
 */
#define SHENNONG_STEPS          11
#define SHENNONG_LINK_STEP      9       /* step_link: GLINK up; the DSP then opens PMIC_RTR_ADSP_APPS by itself */
#define SHENNONG_QRTR_STEP      10      /* step_qrtr: hello both ways and the DSP's services (the sensors' 400 among them) */

/*
 * The battery on shennong (boot-arg battmgr=1, or 2 for every property once in the log): the DSP's
 * battery manager, as on redwood (battmgr above). It needs the DSP up and linked and kept up, so
 * battmgr=N alone brings it up to step 9 (GLINK; no audio, no GPR) and keeps it; with adsp=1 or
 * adsp_step=N as well, those steps are taken, and the DSP is kept all the same.
 */
static bool shennong_battmgr;

/*
 * The motion sensors on shennong (boot-arg gyro=N, shennong_sensors.c): the DSP's sensors' service on QRTR. Like the
 * battery it needs the DSP up, linked and kept, so gyro=N alone brings it up to step 10 (QRTR) and keeps it.
 */
static bool shennong_sensors;

static void
shennong_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
	printf("adsp: shennong: the root is %s; steps up to %u, those from %u on wait %u ms after each announcement\n",
	    rootvnode == NULLVP ? "NOT mounted after a minute" : "mounted", last_step, new_from, pause_ms);
	run_steps();
	if (step == SHENNONG_STEPS && dsp_started && audio_up) {
		shennong_sound_dsp_up();        /* (boot-arg sound=N, with adsp_keep=1: the speakers' path, shennong_sound.c) */
		shennong_mic_dsp_up();          /* (boot-arg mic=N: the microphones, shennong_mic.c) */
	}
	if (step >= SHENNONG_LINK_STEP && dsp_started && keep && glink.up) {
		shennong_snsrpc_dsp_up();       /* (boot-arg snsrpc=N: the sensors' domain served its files, shennong_snsrpc.c) */
	}
	if (shennong_sensors && step >= SHENNONG_QRTR_STEP && dsp_started && keep && glink.up) {
		shennong_snsrpc_wait();
		shennong_sensors_dsp_up();      /* (boot-arg gyro=N: the accelerometer and the gyroscope, shennong_sensors.c) */
	} else if (shennong_sensors) {
		tell("adsp: sensors: shennong: NOT STARTED: the DSP is %s after step %u (QRTR is step %u)%s\n",
		    dsp_started ? "up" : "not up", step, SHENNONG_QRTR_STEP, dsp_started && !keep ? ", and not kept" : "");
	}
	if (step >= SHENNONG_LINK_STEP && dsp_started && keep && glink.up) {
		battmgr_start(0);               /* (boot-arg battmgr=1: the battery's charge from the DSP, every 10 s) */
	} else if (shennong_battmgr) {
		tell("adsp: battmgr: shennong: NOT ASKED: the DSP is %s after step %u (the link is step %u)%s\n",
		    dsp_started ? "up" : "not up", step, SHENNONG_LINK_STEP, dsp_started && !keep ? ", and not kept" : "");
	}
	thread_terminate(current_thread());
}

static void
board_shennong(void)
{
	smem_phys = 0x81d00000UL;
	cmd_db_phys = 0x81c60000UL;
	adsp_memory = 0x9df00000u;
	adsp_memory_size = 0x04680000u;
	ipcc.phys = 0x00406000;
	rsc.phys = 0x17a20000;
	rsc_v3 = true;
	dtb.memory = 0x9de80000u;               /* q6_adsp_dtb_region@9de80000 */
	dtb.memory_size = 0x00080000u;
	dtb_pas = 0x24;
	ipcc_interrupt = 32 + 229;              /* qcom,ipcc@406000: interrupts = <0 0xe5 4> */
	gpr = true;
	battmgr_current_out = true;             /* (Xiaomi's sign of the battery's current, see battmgr) */
	glink.channel[CHANNEL_APR].name = "adsp_apps";   /* GPR's channel (dt: qcom,gpr, qcom,intents = <0x200 0x14>) */
}

/* Called while the system starts (bsd/dev/redwood_input.c), after the Wi-Fi's start has been set going. */
void
redwood_adsp_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	PE_parse_boot_argn("adsp_step", &last_step, sizeof(last_step));
	if (strcmp(redwood_phone(), "shennong") == 0 && option("battmgr")) {
		shennong_battmgr = true;
		if (last_step == 0 && (!PE_parse_boot_argn("adsp", &on, sizeof(on)) || on != 1)) {
			/* (battmgr=N alone: only what the battery needs, the DSP up and linked, steps 1 to 9) */
			last_step = SHENNONG_LINK_STEP;
			new_from = SHENNONG_STEPS + 1;
			printf("adsp: shennong: battmgr without adsp=1: steps 1..%u (up to GLINK) for the battery\n", last_step);
		}
	}
	if (strcmp(redwood_phone(), "shennong") == 0 && option("gyro")) {
		shennong_sensors = true;
		if (new_from == 0 && last_step == 0 && (!PE_parse_boot_argn("adsp", &on, sizeof(on)) || on != 1)) {
			new_from = SHENNONG_STEPS + 1;
		}
		if (last_step == SHENNONG_LINK_STEP && shennong_battmgr && new_from == SHENNONG_STEPS + 1) {
			last_step = 0;          /* (battmgr's steps 1..9 alone: one more, QRTR, for the sensors) */
		}
		if (last_step == 0 && (!PE_parse_boot_argn("adsp", &on, sizeof(on)) || on != 1)) {
			/* (gyro=N alone: the DSP up, linked and QRTR's hello, steps 1 to 10) */
			last_step = SHENNONG_QRTR_STEP;
			printf("adsp: shennong: gyro without adsp=1: steps 1..%u (up to QRTR) for the sensors\n", last_step);
		} else if (last_step != 0 && last_step < SHENNONG_QRTR_STEP) {
			printf("adsp: shennong: gyro: adsp_step=%u stops before QRTR (step %u): the sensors are not started\n",
			    last_step, SHENNONG_QRTR_STEP);
		}
	}
	if (last_step == 0 && (!PE_parse_boot_argn("adsp", &on, sizeof(on)) || on != 1)) {
		return;
	}
	if (strcmp(redwood_phone(), "shennong") == 0) {
		board_shennong();
		image.memory = ADSP_MEMORY;
		image.memory_size = ADSP_MEMORY_SIZE;
		if (last_step == 0 || last_step > SHENNONG_STEPS) {
			printf("adsp: shennong: steps 1..%u (step 11 in GPR); taken as adsp_step=%u\n", SHENNONG_STEPS, SHENNONG_STEPS);
			last_step = SHENNONG_STEPS;
		}
		keep = option("adsp_keep") || shennong_battmgr || shennong_sensors;
		if (shennong_battmgr && last_step < SHENNONG_LINK_STEP) {
			printf("adsp: shennong: battmgr: adsp_step=%u stops before GLINK (step %u): the battery is not asked\n",
			    last_step, SHENNONG_LINK_STEP);
		}
		if (new_from == 0) {
			new_from = last_step;
		}
		PE_parse_boot_argn("adsp_new", &new_from, sizeof(new_from));
		PE_parse_boot_argn("adsp_pause", &pause_ms, sizeof(pause_ms));
		if (kernel_thread_start(shennong_thread, NULL, &thread) != KERN_SUCCESS) {
			printf("adsp: no thread: the audio DSP is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		printf("adsp: shennong: steps up to %u, on a thread that waits for the root (the firmware is in its /fw)\n", last_step);
		return;
	} else if (pe_arm_get_soc_base_phys() != REDWOOD_SOC_BASE) {
		printf("adsp: adsp=1 and adsp_step=N are for the phone's audio DSP, and this is not the phone\n");
		return;
	}
	image.memory = ADSP_MEMORY;
	image.memory_size = ADSP_MEMORY_SIZE;
	if (last_step == 0) {
		/* all of it, on a thread of its own, and left up */
		last_step = STEPS;
		keep = true;
		quiet = true;
		pause_ms = 0;
		listen_seconds = 1;
		new_from = STEPS + 1;
		if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
			printf("adsp: no thread: the audio DSP is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		tell("adsp: the audio DSP is being brought up beside the system's start (adsp=1)\n");
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	keep = option("adsp_keep");
	new_from = last_step;
	PE_parse_boot_argn("adsp_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("adsp_pause", &pause_ms, sizeof(pause_ms));
	printf("adsp: steps up to %u; those from %u on are new and wait %u ms after each announcement\n", last_step, new_from,
	    pause_ms);
	run_steps();
	if (option("adsp_return")) {
		printf("adsp: back to the loader (adsp_return)\n");
		loader_return();
	}
	printf("adsp: the system goes on starting\n");
}
