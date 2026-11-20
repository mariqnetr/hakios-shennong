/*
 * xnuloader/slot.c on the computer, against a copy of a phone's partition table (head and tail of the unit with boot_a and
 * boot_b, as ufs-layout.sh saves them):   cc -I xnuloader tools/slot-test.c -o slot-test && ./slot-test HEAD TAIL BLOCKS
 *
 * Every state the menu can meet. After each one: only the boot entry's attributes and the CRCs that cover them may differ,
 * both copies check out and agree.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "efi.h"

static int verbose;
static void print(const char *f, ...) { if (verbose) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); } }
static uint32_t ufs_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;
	for (uint64_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
	return ~c;
}
#include "slot.c"

#define BS 4096u
static uint8_t *disk, *orig; static uint64_t blocks, writes; static long fail_write_at = -1; static int lie_on_readback;
static efi_status rd(efi_block_io *b, uint32_t m, uint64_t lba, uint64_t n, void *buf)
{ if (m != b->media->media_id || lba * BS + n > blocks * BS || n % BS) return 0x8000000000000002ULL; memcpy(buf, disk + lba * BS, n); if (lie_on_readback && writes) ((uint8_t *)buf)[0] ^= 1; return 0; }
static efi_status wr(efi_block_io *b, uint32_t m, uint64_t lba, uint64_t n, void *buf)
{ if ((long)writes == fail_write_at) return 0x8000000000000007ULL; writes++; memcpy(disk + lba * BS, buf, n); return 0; }
static efi_status fl(efi_block_io *b) { return 0; }
static efi_block_media media = { .media_id = 7, .block_size = BS };
static efi_block_io bio = { .media = &media, .read_blocks = rd, .write_blocks = wr, .flush_blocks = fl };
static uint64_t ALT, ELBA[2]; static uint32_t COUNT, A = ~0u, B = ~0u;

static uint64_t attr(int copy, uint32_t i) { uint64_t v; memcpy(&v, disk + ELBA[copy] * BS + (uint64_t)i * 128 + 48, 8); return v; }
static void reseal(int copy) {
	uint64_t h = copy ? ALT : 1; uint32_t c, hs; memcpy(&hs, disk + h * BS + 12, 4);
	c = ufs_crc32(disk + ELBA[copy] * BS, (uint64_t)COUNT * 128); memcpy(disk + h * BS + 88, &c, 4);
	memset(disk + h * BS + 16, 0, 4); c = ufs_crc32(disk + h * BS, hs); memcpy(disk + h * BS + 16, &c, 4);
}
static void set_attrs(uint8_t a, uint8_t b) {
	for (int c = 0; c < 2; c++) {
		uint64_t v = attr(c, A); v = (v & 0x00ffffffffffffffULL & ~(0xffULL << 48)) | ((uint64_t)a << 48) | (v & (0xffULL << 56) & 0); memcpy(disk + ELBA[c] * BS + (uint64_t)A * 128 + 48, &v, 8);
		v = attr(c, B); v = (v & ~(0xffULL << 48)) | ((uint64_t)b << 48); memcpy(disk + ELBA[c] * BS + (uint64_t)B * 128 + 48, &v, 8);
		reseal(c);
	}
	memcpy(orig, disk, blocks * BS); writes = 0;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAILED: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
/* byte by byte: outside entry B's attribute byte and the two copies' CRC fields nothing moved */
static void only_attr_moved(void)
{
	uint64_t diffs = 0;
	for (uint64_t off = 0; off < blocks * BS; off++) {
		if (disk[off] == orig[off]) continue;
		int ok = 0;
		for (int c = 0; c < 2; c++) {
			uint64_t h = c ? ALT : 1, e = ELBA[c] * BS + (uint64_t)B * 128 + 48;
			if (off >= e && off < e + 8) ok = 1;
			if (off >= h * BS + 16 && off < h * BS + 20) ok = 1;
			if (off >= h * BS + 88 && off < h * BS + 92) ok = 1;
		}
		if (!ok) { CHECK(0, "byte at %llu moved", (unsigned long long)off); if (++diffs > 5) return; }
	}
}
static void both_check_out(void)
{
	enum slot_result r = slot_table_load(&bio);
	CHECK(r == SLOT_OK, "the table written does not load: %d", r);
}

int main(int argc, char **argv)
{
	FILE *f; long hs, ts;
	if (argc != 4) return 2;
	blocks = strtoull(argv[3], 0, 0); media.last_block = blocks - 1;
	disk = calloc(blocks, BS); orig = malloc(blocks * BS);
	f = fopen(argv[1], "rb"); fseek(f, 0, 2); hs = ftell(f); rewind(f); fread(disk, 1, hs, f); fclose(f);
	f = fopen(argv[2], "rb"); fseek(f, 0, 2); ts = ftell(f); rewind(f); fread(disk + blocks * BS - ts, 1, ts, f); fclose(f);
	memcpy(&ALT, disk + BS + 32, 8); memcpy(&ELBA[0], disk + BS + 72, 8); memcpy(&COUNT, disk + BS + 80, 4); memcpy(&ELBA[1], disk + ALT * BS + 72, 8);
	for (uint32_t i = 0; i < COUNT; i++) {
		uint8_t *e = disk + ELBA[0] * BS + (uint64_t)i * 128;
		if (slot_name_is(e, "boot_a")) A = i;
		if (slot_name_is(e, "boot_b")) B = i;
	}
	printf("%llu blocks, backup header at %llu, tables at %llu and %llu, %u entries, boot_a #%u %llx, boot_b #%u %llx\n", (unsigned long long)blocks,
	    (unsigned long long)ALT, (unsigned long long)ELBA[0], (unsigned long long)ELBA[1], COUNT, A + 1, (unsigned long long)(attr(0, A) >> 48), B + 1, (unsigned long long)(attr(0, B) >> 48));
	/* slot a: retries 6, successful, active, priority 3 (0x77); slot b as set_active leaves it: priority 3, active, retries 7 */
	enum { A_GOOD = 0x73, B_FRESH = 0x3f };

	printf("the phone as dumped (a active), menu by fastboot boot: nothing written\n");
	memcpy(orig, disk, blocks * BS); writes = 0;
	CHECK(slot_table_load(&bio) == SLOT_OK, "no table");
	CHECK(slot_table_change(1, 0) == SLOT_NOT_ACTIVE_HERE && writes == 0, "good");
	CHECK(slot_table_load(&bio) == SLOT_OK && slot_table_change(1, 1) == SLOT_NOT_ACTIVE_HERE && writes == 0, "android");
	CHECK(!memcmp(disk, orig, blocks * BS), "moved");

	printf("b active, first start: marked successful\n");
	set_attrs(A_GOOD, B_FRESH);
	CHECK(slot_table_load(&bio) == SLOT_OK && slot_table_change(1, 0) == SLOT_OK, "failed");
	only_attr_moved(); both_check_out();
	CHECK(attr(0, B) >> 48 == (B_FRESH | 0x40) && attr(1, B) >> 48 == (B_FRESH | 0x40) && attr(0, A) >> 48 == A_GOOD, "attrs %llx", (unsigned long long)(attr(0, B) >> 48));
	printf("  %llu writes\n", (unsigned long long)writes);

	printf("b active and successful already: nothing written\n");
	memcpy(orig, disk, blocks * BS); writes = 0;
	CHECK(slot_table_load(&bio) == SLOT_OK && slot_table_change(1, 0) == SLOT_OK && writes == 0, "wrote");

	printf("Android chosen: b unbootable, not successful, a untouched\n");
	CHECK(slot_table_load(&bio) == SLOT_OK && slot_table_change(1, 1) == SLOT_OK, "failed");
	only_attr_moved(); both_check_out();
	CHECK((attr(0, B) >> 48 & 0xc0) == 0x80 && (attr(1, B) >> 48 & 0xc0) == 0x80 && attr(0, A) >> 48 == A_GOOD, "attrs %llx", (unsigned long long)(attr(0, B) >> 48));

	printf("Android chosen, slot a not successful or unbootable: refused\n");
	for (int s = 0; s < 2; s++) {
		set_attrs(s ? (A_GOOD | 0x80) : (A_GOOD & ~0x40), B_FRESH);
		CHECK(slot_table_load(&bio) == SLOT_OK && slot_table_change(1, 1) == SLOT_BAD_STATE && writes == 0, "not refused");
	}

	printf("both or neither active: refused\n");
	set_attrs(A_GOOD | 4, B_FRESH); CHECK(slot_table_load(&bio) == SLOT_OK && slot_table_change(1, 0) == SLOT_BAD_STATE && writes == 0, "both");
	set_attrs(A_GOOD & ~4, B_FRESH & ~4); CHECK(slot_table_load(&bio) == SLOT_OK && slot_table_change(1, 0) == SLOT_BAD_STATE && writes == 0, "neither");

	printf("damaged tables: refused, nothing written\n");
	set_attrs(A_GOOD, B_FRESH); disk[ELBA[1] * BS + 200] ^= 1;                      /* backup entries' CRC broken */
	CHECK(slot_table_load(&bio) == SLOT_BAD_TABLE && writes == 0, "backup entries"); disk[ELBA[1] * BS + 200] ^= 1;
	disk[ALT * BS + 40] ^= 1; CHECK(slot_table_load(&bio) == SLOT_BAD_TABLE, "backup header"); disk[ALT * BS + 40] ^= 1;
	disk[ELBA[0] * BS + 200] ^= 1; CHECK(slot_table_load(&bio) == SLOT_NO_TABLE, "primary"); disk[ELBA[0] * BS + 200] ^= 1;
	{ uint64_t off = ELBA[1] * BS + (uint64_t)A * 128 + 50; disk[off] ^= 4; reseal(1);        /* both good, different */
	  CHECK(slot_table_load(&bio) == SLOT_BAD_TABLE && writes == 0, "copies differ"); disk[off] ^= 4; reseal(1); }

	printf("a backup that says it is at LBA 1 (as ABL writes it): taken, and kept so\n");
	set_attrs(A_GOOD, B_FRESH); { uint64_t one = 1; memcpy(disk + ALT * BS + 24, &one, 8); reseal(1); memcpy(orig, disk, blocks * BS); }
	CHECK(slot_table_load(&bio) == SLOT_OK && slot_table_change(1, 0) == SLOT_OK, "failed"); only_attr_moved(); both_check_out();
	{ uint64_t my; memcpy(&my, disk + ALT * BS + 24, 8); CHECK(my == 1, "changed"); memcpy(&my, (uint8_t[8]){0}, 8); my = ALT; memcpy(disk + ALT * BS + 24, &my, 8); reseal(1); }

	printf("a write that fails, at each of the 4 writes: reported; the primary is whole until its turn\n");
	for (long w = 0; w < 4; w++) {
		set_attrs(A_GOOD, B_FRESH); fail_write_at = w;
		CHECK(slot_table_load(&bio) == SLOT_OK && slot_table_change(1, 0) == SLOT_WRITE_FAILED, "not reported at %ld", w);
		fail_write_at = -1;
		if (w < 2) { uint64_t e = ELBA[0] * BS; CHECK(!memcmp(disk + BS, orig + BS, BS) && !memcmp(disk + e, orig + e, (uint64_t)COUNT * 128), "primary moved at %ld", w); }
	}
	printf("a read-back that differs: reported\n");
	set_attrs(A_GOOD, B_FRESH); CHECK(slot_table_load(&bio) == SLOT_OK, "load"); lie_on_readback = 1;
	CHECK(slot_table_change(1, 0) == SLOT_WRITE_FAILED, "not reported"); lie_on_readback = 0;

	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails != 0;
}
