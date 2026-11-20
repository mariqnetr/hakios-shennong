/*
 * Writing APFS: the space manager.
 *
 * Which blocks of the container are in use is told by a bitmap per chunk of blocks (32768 of
 * them at 4096 bytes a block), listed by chunk-info blocks (CIBs) whose addresses the space
 * manager -- an ephemeral object, written with every checkpoint -- keeps.  The bitmaps and the
 * CIBs are themselves blocks of a small region of their own, the internal pool, whose use is
 * told by one more bitmap, kept in a ring of copies (apfs_txn.c writes the space manager; this
 * file writes everything it points at).
 *
 * Nothing the live checkpoint uses is ever written over: a bitmap or a CIB that changes is
 * written into a free block of the internal pool and the space manager made to point at it,
 * the internal pool's bitmap into the next free copy of its ring.
 *
 * Blocks given up
 * ---------------
 * Apple's driver puts a block that is given up into a free queue (a B-tree per device, keyed
 * by transaction) and marks it free only once no checkpoint it keeps can still want it.  Here
 * a block given up in a transaction is free in the bitmaps that transaction writes, and taken
 * by nothing before that transaction has committed: the transaction after it may take it.  The
 * one checkpoint that can still want it then is the one before -- and a reader only ever goes
 * back to that one when the commit of the transaction that gave it up did not complete, in
 * which case nothing after it ran either.  So the free queues stay empty; what an image comes
 * with in them (newfs_apfs leaves both with an entry) is taken out at load and freed with the
 * first commit.
 *
 * What is not done: tier-2 devices (Fusion), chunk-info address blocks (CABs: containers of
 * more than chunks_per_cib * cibs_per_cab chunks -- 507 * 126 * 32768 blocks, ~8 TB, is the
 * first that needs one... CABs come before that, when cib_count would not fit the space
 * manager; refused at load), the free queue of the second device.
 */

#include "apfs_space.h"

static inline uint16_t
s_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t
s_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t
s_le64(const uint8_t *p)
{
	return (uint64_t)s_le32(p) | (uint64_t)s_le32(p + 4) << 32;
}

static inline void
s_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void
s_put32(uint8_t *p, uint32_t v)
{
	s_put16(p, (uint16_t)v);
	s_put16(p + 2, (uint16_t)(v >> 16));
}

static inline void
s_put64(uint8_t *p, uint64_t v)
{
	s_put32(p, (uint32_t)v);
	s_put32(p + 4, (uint32_t)(v >> 32));
}

/* spaceman_phys_t */
#define SM_BLOCKS_PER_CHUNK  36
#define SM_CHUNKS_PER_CIB    40
#define SM_DEV_MAIN          48         /* spaceman_device_t, 48 bytes */
#define SM_DEV_TIER2         96
#define SMD_BLOCK_COUNT      0
#define SMD_CHUNK_COUNT      8
#define SMD_CIB_COUNT        16
#define SMD_CAB_COUNT        20
#define SMD_FREE_COUNT       24
#define SMD_ADDR_OFFSET      32
#define SM_IP_BLOCK_COUNT    152
#define SM_IP_BM_SIZE        160
#define SM_IP_BM_BLOCKS      164
#define SM_IP_BM_BASE        168
#define SM_IP_BASE           176
#define SM_FQ                200        /* spaceman_free_queue_t[3], 40 bytes each */
#define SFQ_COUNT            0
#define SFQ_TREE_OID         8
#define SFQ_OLDEST_XID       16
#define SFQ_IP               0
#define SFQ_MAIN             1
#define SFQ_TIER2            2
#define SM_IP_BM_FREE_HEAD   320
#define SM_IP_BM_FREE_TAIL   322
#define SM_IP_BM_XID_OFF     324
#define SM_IP_BITMAP_OFF     328
#define SM_IP_BM_FREE_NEXT   332
#define IP_BM_END            0xffff

/* chunk_info_block_t and chunk_info_t */
#define CIB_INDEX            32
#define CIB_COUNT            36
#define CIB_INFO             40
#define CI_SIZE              32
#define CI_XID               0
#define CI_ADDR              8
#define CI_BLOCK_COUNT       16
#define CI_FREE_COUNT        20
#define CI_BITMAP_ADDR       24

/* btree_node_phys_t, as far as a free queue's root leaf needs it */
#define BTN_FLAGS            32
#define BTN_LEVEL            34
#define BTN_NKEYS            36
#define BTN_TOC_OFF          40
#define BTN_TOC_LEN          42
#define BTN_FREE_OFF         44
#define BTN_FREE_LEN         46
#define BTN_DATA             56
#define BTI_SIZE             40         /* btree_info_t at the end of a root */
#define BTI_KEY_COUNT        24
#define BTI_NODE_COUNT       32
#define BTNODE_LEAF          0x2
#define BTNODE_FIXED_KV      0x4

static inline bool
bit_get(const uint8_t *bm, uint64_t i)
{
	return (bm[i >> 3] >> (i & 7)) & 1;
}

static inline void
bit_set(uint8_t *bm, uint64_t i, bool v)
{
	if (v) {
		bm[i >> 3] |= (uint8_t)(1u << (i & 7));
	} else {
		bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
	}
}

void
apfs_space_fini(struct apfs_space *sp)
{
	uint32_t bs = sp->txn != NULL ? sp->txn->nx->block_size : 0;

	for (uint64_t c = 0; sp->chunk != NULL && c < sp->chunk_count; c++) {
		if (sp->chunk[c].bitmap != NULL) {
			apfs_free(sp->chunk[c].bitmap, bs);
		}
	}
	for (uint32_t i = 0; sp->cib != NULL && i < sp->cib_count; i++) {
		if (sp->cib[i] != NULL) {
			apfs_free(sp->cib[i], bs);
		}
	}
	if (sp->chunk != NULL) {
		apfs_free(sp->chunk, sp->chunk_count * sizeof(*sp->chunk));
	}
	if (sp->cib != NULL) {
		apfs_free(sp->cib, sp->cib_count * sizeof(*sp->cib));
	}
	if (sp->cib_addr != NULL) {
		apfs_free(sp->cib_addr, sp->cib_count * sizeof(*sp->cib_addr));
	}
	if (sp->cib_dirty != NULL) {
		apfs_free(sp->cib_dirty, sp->cib_count * sizeof(*sp->cib_dirty));
	}
	if (sp->ip_bitmap != NULL) {
		apfs_free(sp->ip_bitmap, bs);
	}
	if (sp->frees != NULL) {
		apfs_free(sp->frees, sp->maxfrees * sizeof(*sp->frees));
	}
	memset(sp, 0, sizeof(*sp));
}

static int
note_free(struct apfs_space *sp, uint64_t addr, uint64_t count, bool ip)
{
	if (sp->nfrees == sp->maxfrees) {
		uint32_t more = sp->maxfrees ? sp->maxfrees * 2 : 64;
		struct apfs_space_free *n = apfs_alloc(more * sizeof(*n));

		if (n == NULL) {
			return ENOMEM;
		}
		if (sp->frees != NULL) {
			memcpy(n, sp->frees, sp->nfrees * sizeof(*n));
			apfs_free(sp->frees, sp->maxfrees * sizeof(*sp->frees));
		}
		sp->frees = n;
		sp->maxfrees = more;
	}
	sp->frees[sp->nfrees++] = (struct apfs_space_free){ .addr = addr, .count = count, .ip = ip };
	return 0;
}

/* the entries of a free queue whose tree is one root leaf of fixed-size keys, into frees; then empty it */
static int
drain_queue(struct apfs_space *sp, int q)
{
	uint8_t *fq = sp->sm->data + SM_FQ + 40 * q;
	uint64_t oid = s_le64(fq + SFQ_TREE_OID), count = s_le64(fq + SFQ_COUNT);
	uint32_t bs = sp->txn->nx->block_size;
	struct apfs_txn_ephemeral *t;
	uint8_t *n;
	uint32_t nkeys, toc_len, kbase, vend;
	int error;

	if (count == 0 && oid == 0) {
		return 0;
	}
	t = apfs_txn_find(sp->txn, APFS_OBJECT_TYPE_BTREE, oid);
	if (t == NULL || t->size != bs) {
		apfs_log("space: free queue %d: its tree %llu is not among the checkpoint's objects\n", q,
		    (unsigned long long)oid);
		return EIO;
	}
	n = t->data;
	nkeys = s_le32(n + BTN_NKEYS);
	toc_len = s_le16(n + BTN_TOC_LEN);
	if (s_le16(n + BTN_LEVEL) != 0 || (s_le16(n + BTN_FLAGS) & (BTNODE_LEAF | BTNODE_FIXED_KV)) !=
	    (BTNODE_LEAF | BTNODE_FIXED_KV) || s_le16(n + BTN_TOC_OFF) != 0 || nkeys * 4 > toc_len) {
		apfs_log("space: free queue %d is more than one leaf: not handled\n", q);
		return ENOTSUP;
	}
	kbase = BTN_DATA + toc_len;
	vend = bs - BTI_SIZE;
	for (uint32_t i = 0; i < nkeys; i++) {
		uint16_t koff = s_le16(n + BTN_DATA + 4 * i), voff = s_le16(n + BTN_DATA + 4 * i + 2);
		uint64_t paddr, len = 1;

		if (kbase + koff + 16 > vend) {
			return EIO;
		}
		paddr = s_le64(n + kbase + koff + 8);
		if (voff != 0xffff) {           /* a ghost (no value) is a single block */
			if (voff < 8 || voff > vend - kbase) {
				return EIO;
			}
			len = s_le64(n + vend - voff);
		}
		if ((error = note_free(sp, paddr, len, q == SFQ_IP)) != 0) {
			return error;
		}
	}
	/* the queue, empty: no keys, all of the node's room free */
	memset(n + BTN_DATA, 0, vend - BTN_DATA);
	s_put32(n + BTN_NKEYS, 0);
	s_put16(n + BTN_FREE_OFF, 0);
	s_put16(n + BTN_FREE_LEN, (uint16_t)(vend - kbase));
	s_put64(n + bs - BTI_SIZE + BTI_KEY_COUNT, 0);
	s_put64(fq + SFQ_COUNT, 0);
	s_put64(fq + SFQ_OLDEST_XID, 0);
	return 0;
}

int
apfs_space_load(struct apfs_txn *txn, struct apfs_space *sp)
{
	uint32_t bs = txn->nx->block_size, addr_off, bmoff;
	uint8_t *sm, *dev;
	int error = 0;

	memset(sp, 0, sizeof(*sp));
	sp->txn = txn;
	sp->sm = apfs_txn_find(txn, APFS_OBJECT_TYPE_SPACEMAN, 0);
	if (sp->sm == NULL) {
		return EIO;
	}
	sm = sp->sm->data;
	dev = sm + SM_DEV_MAIN;
	sp->blocks_per_chunk = s_le32(sm + SM_BLOCKS_PER_CHUNK);
	sp->chunks_per_cib = s_le32(sm + SM_CHUNKS_PER_CIB);
	sp->block_count = s_le64(dev + SMD_BLOCK_COUNT);
	sp->chunk_count = s_le64(dev + SMD_CHUNK_COUNT);
	sp->cib_count = s_le32(dev + SMD_CIB_COUNT);
	sp->free_count = s_le64(dev + SMD_FREE_COUNT);
	addr_off = s_le32(dev + SMD_ADDR_OFFSET);
	sp->ip_base = s_le64(sm + SM_IP_BASE);
	sp->ip_blocks = s_le64(sm + SM_IP_BLOCK_COUNT);
	sp->ip_bm_base = s_le64(sm + SM_IP_BM_BASE);
	sp->ip_bm_blocks = s_le32(sm + SM_IP_BM_BLOCKS);
	if (sp->blocks_per_chunk != bs * 8 || s_le32(dev + SMD_CAB_COUNT) != 0 || s_le64(sm + SM_DEV_TIER2) != 0 ||
	    s_le32(sm + SM_IP_BM_SIZE) != 1 || sp->ip_blocks > (uint64_t)bs * 8 || sp->cib_count == 0 ||
	    sp->block_count != txn->nx->block_count ||
	    sp->chunk_count != (sp->block_count + sp->blocks_per_chunk - 1) / sp->blocks_per_chunk ||
	    sp->cib_count != (sp->chunk_count + sp->chunks_per_cib - 1) / sp->chunks_per_cib ||
	    addr_off + 8ull * sp->cib_count > sp->sm->size) {
		apfs_log("space: a space manager this does not handle (CABs, a second device, a big pool)\n");
		return ENOTSUP;
	}
	sp->cib_addr = apfs_alloc(sp->cib_count * sizeof(*sp->cib_addr));
	sp->cib_dirty = apfs_alloc(sp->cib_count * sizeof(*sp->cib_dirty));
	sp->cib = apfs_alloc(sp->cib_count * sizeof(*sp->cib));
	sp->chunk = apfs_alloc(sp->chunk_count * sizeof(*sp->chunk));
	sp->ip_bitmap = apfs_alloc(bs);
	if (sp->cib_addr == NULL || sp->cib_dirty == NULL || sp->cib == NULL || sp->chunk == NULL || sp->ip_bitmap == NULL) {
		error = ENOMEM;
		goto out;
	}
	/* the internal pool's bitmap: the copy the space manager names */
	bmoff = s_le16(sm + s_le32(sm + SM_IP_BITMAP_OFF));
	if (bmoff >= sp->ip_bm_blocks || (error = apfs_txn_read(txn, sp->ip_bm_base + bmoff, sp->ip_bitmap, 1)) != 0) {
		error = error ? error : EIO;
		goto out;
	}
	/* the CIBs and the chunk bitmaps */
	for (uint32_t i = 0; i < sp->cib_count; i++) {
		uint8_t *cib;
		uint32_t n;

		sp->cib_addr[i] = s_le64(sm + addr_off + 8 * i);
		cib = sp->cib[i] = apfs_alloc(bs);
		if (cib == NULL) {
			error = ENOMEM;
			goto out;
		}
		if ((error = apfs_txn_read(txn, sp->cib_addr[i], cib, 1)) != 0) {
			goto out;
		}
		n = s_le32(cib + CIB_COUNT);
		if (!apfs_checksum_ok(cib, bs) || s_le32(cib + CIB_INDEX) != i ||
		    n > sp->chunks_per_cib || (uint64_t)i * sp->chunks_per_cib + n > sp->chunk_count) {
			apfs_log("space: chunk-info block %u at %llu does not check out\n", i, (unsigned long long)sp->cib_addr[i]);
			error = EIO;
			goto out;
		}
		for (uint32_t j = 0; j < n; j++) {
			const uint8_t *ci = cib + CIB_INFO + CI_SIZE * j;
			uint64_t c = (uint64_t)i * sp->chunks_per_cib + j;
			struct apfs_space_chunk *ch = &sp->chunk[c];

			if (s_le64(ci + CI_ADDR) != c * sp->blocks_per_chunk) {
				error = EIO;
				goto out;
			}
			ch->bitmap_addr = s_le64(ci + CI_BITMAP_ADDR);
			ch->free_count = s_le32(ci + CI_FREE_COUNT);
			if (ch->bitmap_addr != 0) {
				ch->bitmap = apfs_alloc(bs);
				if (ch->bitmap == NULL) {
					error = ENOMEM;
					goto out;
				}
				if ((error = apfs_txn_read(txn, ch->bitmap_addr, ch->bitmap, 1)) != 0) {
					goto out;
				}
			}
		}
	}
	/* what is waiting in the free queues is freed with the first commit (see above) */
	if ((error = drain_queue(sp, SFQ_IP)) != 0 || (error = drain_queue(sp, SFQ_MAIN)) != 0) {
		goto out;
	}
out:
	if (error) {
		apfs_space_fini(sp);
	}
	return error;
}

static bool
main_used(struct apfs_space *sp, uint64_t b)
{
	struct apfs_space_chunk *ch = &sp->chunk[b / sp->blocks_per_chunk];

	return ch->bitmap != NULL && bit_get(ch->bitmap, b % sp->blocks_per_chunk);
}

static int
main_mark(struct apfs_space *sp, uint64_t b, bool used)
{
	struct apfs_space_chunk *ch = &sp->chunk[b / sp->blocks_per_chunk];
	uint32_t bs = sp->txn->nx->block_size;

	if (ch->bitmap == NULL) {
		ch->bitmap = apfs_alloc(bs);    /* zeroed: a chunk without a bitmap is all free */
		if (ch->bitmap == NULL) {
			return ENOMEM;
		}
	}
	if (bit_get(ch->bitmap, b % sp->blocks_per_chunk) == used) {
		apfs_log("space: block %llu is already %s\n", (unsigned long long)b, used ? "in use" : "free");
		return EIO;
	}
	bit_set(ch->bitmap, b % sp->blocks_per_chunk, used);
	if (used) {
		ch->free_count--;
		sp->free_count--;
	} else {
		ch->free_count++;
		sp->free_count++;
	}
	ch->dirty = true;
	sp->cib_dirty[b / sp->blocks_per_chunk / sp->chunks_per_cib] = true;
	return 0;
}

int
apfs_space_alloc(struct apfs_space *sp, uint64_t count, uint64_t *addr)
{
	uint64_t run = 0;

	if (count == 0) {
		return EINVAL;
	}
	/* first fit; a chunk with no block free is passed over whole */
	for (uint64_t b = 0; b < sp->block_count;) {
		struct apfs_space_chunk *ch = &sp->chunk[b / sp->blocks_per_chunk];

		if (b % sp->blocks_per_chunk == 0 && ch->free_count == 0) {
			b += sp->blocks_per_chunk;
			run = 0;
			continue;
		}
		run = main_used(sp, b) ? 0 : run + 1;
		b++;
		if (run == count) {
			int error;

			*addr = b - count;
			for (uint64_t k = 0; k < count; k++) {
				if ((error = main_mark(sp, *addr + k, true)) != 0) {
					return error;
				}
			}
			return 0;
		}
	}
	return ENOSPC;
}

int
apfs_space_free(struct apfs_space *sp, uint64_t addr, uint64_t count)
{
	if (addr == 0 || addr + count > sp->block_count || addr + count < addr) {
		return EINVAL;
	}
	return note_free(sp, addr, count, false);
}

/* a block of the internal pool, free in the live checkpoint and not taken since */
static int
ip_alloc(struct apfs_space *sp, uint64_t *addr)
{
	for (uint64_t i = 0; i < sp->ip_blocks; i++) {
		if (!bit_get(sp->ip_bitmap, i)) {
			bit_set(sp->ip_bitmap, i, true);
			*addr = sp->ip_base + i;
			return 0;
		}
	}
	apfs_log("space: the internal pool is full\n");
	return ENOSPC;
}

static int
ip_mark_free(struct apfs_space *sp, uint64_t addr)
{
	if (addr < sp->ip_base || addr >= sp->ip_base + sp->ip_blocks || !bit_get(sp->ip_bitmap, addr - sp->ip_base)) {
		apfs_log("space: internal pool block %llu is not one in use\n", (unsigned long long)addr);
		return EIO;
	}
	bit_set(sp->ip_bitmap, addr - sp->ip_base, false);
	return 0;
}

int
apfs_space_flush(struct apfs_space *sp)
{
	struct apfs_txn *txn = sp->txn;
	uint32_t bs = txn->nx->block_size;
	uint64_t xid = apfs_txn_xid(txn);
	uint8_t *sm = sp->sm->data, *dev = sm + SM_DEV_MAIN;
	uint32_t addr_off = s_le32(dev + SMD_ADDR_OFFSET);
	uint32_t ip_frees_from;
	uint16_t head, tail, old, slot;
	uint8_t *xids, *offs, *next;
	int error;

	/* 1. the main device's blocks given up: free in what this commit writes */
	for (uint32_t i = 0; i < sp->nfrees; i++) {
		if (sp->frees[i].ip) {
			continue;
		}
		for (uint64_t k = 0; k < sp->frees[i].count; k++) {
			if ((error = main_mark(sp, sp->frees[i].addr + k, false)) != 0) {
				return error;
			}
		}
	}
	ip_frees_from = sp->nfrees;
	/* 2. every changed bitmap into a new block of the internal pool; its CIB changes with it */
	for (uint64_t c = 0; c < sp->chunk_count; c++) {
		struct apfs_space_chunk *ch = &sp->chunk[c];
		uint8_t *ci = sp->cib[c / sp->chunks_per_cib] + CIB_INFO + CI_SIZE * (c % sp->chunks_per_cib);
		uint64_t at;

		if (!ch->dirty) {
			continue;
		}
		if (ch->free_count == sp->blocks_per_chunk) {
			at = 0;                 /* all free again: no bitmap at all, as newfs leaves it */
		} else {
			if ((error = ip_alloc(sp, &at)) != 0 || (error = apfs_txn_write(txn, at, ch->bitmap, 1)) != 0) {
				return error;
			}
		}
		if (ch->bitmap_addr != 0 && (error = note_free(sp, ch->bitmap_addr, 1, true)) != 0) {
			return error;
		}
		ch->bitmap_addr = at;
		s_put64(ci + CI_XID, xid);
		s_put32(ci + CI_FREE_COUNT, ch->free_count);
		s_put64(ci + CI_BITMAP_ADDR, at);
		ch->dirty = false;
	}
	for (uint32_t i = 0; i < sp->cib_count; i++) {
		uint64_t at;

		if (!sp->cib_dirty[i]) {
			continue;
		}
		if ((error = ip_alloc(sp, &at)) != 0) {
			return error;
		}
		s_put64(sp->cib[i] + 8, at);    /* a CIB is a physical object: its id is its address */
		apfs_obj_seal(sp->cib[i], bs, xid);
		if ((error = apfs_txn_write(txn, at, sp->cib[i], 1)) != 0 ||
		    (error = note_free(sp, sp->cib_addr[i], 1, true)) != 0) {
			return error;
		}
		sp->cib_addr[i] = at;
		s_put64(sm + addr_off + 8 * i, at);
		sp->cib_dirty[i] = false;
	}
	/* 3. the internal pool's blocks given up -- after every allocation above, never taken by one */
	for (uint32_t i = 0; i < sp->nfrees; i++) {
		if (!sp->frees[i].ip) {
			continue;
		}
		for (uint64_t k = 0; k < sp->frees[i].count; k++) {
			if ((error = ip_mark_free(sp, sp->frees[i].addr + k)) != 0) {
				return error;
			}
		}
	}
	(void)ip_frees_from;
	sp->nfrees = 0;
	/* 4. the internal pool's bitmap into the head of its ring's free list; the old copy to the tail */
	xids = sm + s_le32(sm + SM_IP_BM_XID_OFF);
	offs = sm + s_le32(sm + SM_IP_BITMAP_OFF);
	next = sm + s_le32(sm + SM_IP_BM_FREE_NEXT);
	head = s_le16(sm + SM_IP_BM_FREE_HEAD);
	tail = s_le16(sm + SM_IP_BM_FREE_TAIL);
	old = s_le16(offs);
	slot = head;
	if (slot >= sp->ip_bm_blocks || slot == old || tail >= sp->ip_bm_blocks) {
		apfs_log("space: the internal pool's bitmap ring has no free copy\n");
		return EIO;
	}
	if ((error = apfs_txn_write(txn, sp->ip_bm_base + slot, sp->ip_bitmap, 1)) != 0) {
		return error;
	}
	head = s_le16(next + 2 * slot);
	s_put16(next + 2 * slot, IP_BM_END);
	s_put16(next + 2 * tail, old);
	s_put16(next + 2 * old, IP_BM_END);
	tail = old;
	if (head == IP_BM_END) {
		head = tail;
	}
	s_put16(sm + SM_IP_BM_FREE_HEAD, head);
	s_put16(sm + SM_IP_BM_FREE_TAIL, tail);
	s_put16(offs, slot);
	s_put64(xids, xid);
	/* 5. the counts */
	s_put64(dev + SMD_FREE_COUNT, sp->free_count);
	return 0;
}
