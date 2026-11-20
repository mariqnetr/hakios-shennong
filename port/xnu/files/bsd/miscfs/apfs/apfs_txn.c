/*
 * Writing APFS: transactions and checkpoints.
 *
 * What makes a change live is a checkpoint (apfs_write.c, steps 7 and 8): the ephemeral
 * objects -- the space manager, the reaper and the trees they own -- copied into the next
 * blocks of the checkpoint data ring, a checkpoint map in the descriptor ring that says where
 * each of them landed, and right after it a container superblock with the next transaction
 * identifier.  A reader (apfs_nx_open, and Apple's) takes the superblock of the highest
 * transaction whose checksum and map check out, so up to the last block of a commit the
 * previous checkpoint is the live one.
 *
 * This file keeps the ephemeral objects in memory from the live checkpoint on (apfs_txn_load)
 * and writes them out again at every commit, after the blocks the live checkpoint uses in both
 * rings, never over them.  The order on the device: the data ring's blocks and the map, then
 * the superblock in the descriptor ring, then the copy at block 0 that a reader may start from.
 *
 * Built into the kernel and, as apfs_lib.c is, into a program on the build machine that runs
 * it against an image Apple's newfs_apfs made, which Apple's fsck_apfs then checks.
 */

#include "apfs_txn.h"

static inline uint32_t
t_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t
t_le64(const uint8_t *p)
{
	return (uint64_t)t_le32(p) | (uint64_t)t_le32(p + 4) << 32;
}

static inline void
t_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void
t_put64(uint8_t *p, uint64_t v)
{
	t_put32(p, (uint32_t)v);
	t_put32(p + 4, (uint32_t)(v >> 32));
}

/* offsets in the container superblock (struct apfs_nx_superblock) */
#define NX_OFF(f)        ((uint32_t)__builtin_offsetof(struct apfs_nx_superblock, f))
#define NX_OID           NX_OFF(nx_o.o_oid)
#define NX_XID           NX_OFF(nx_o.o_xid)
#define NX_TYPE          NX_OFF(nx_o.o_type)
#define NX_MAGIC         NX_OFF(nx_magic)
#define NX_NEXT_XID      NX_OFF(nx_next_xid)
#define NX_DESC_BLOCKS   NX_OFF(nx_xp_desc_blocks)
#define NX_DATA_BLOCKS   NX_OFF(nx_xp_data_blocks)
#define NX_DESC_BASE     NX_OFF(nx_xp_desc_base)
#define NX_DATA_BASE     NX_OFF(nx_xp_data_base)
#define NX_DESC_NEXT     NX_OFF(nx_xp_desc_next)
#define NX_DATA_NEXT     NX_OFF(nx_xp_data_next)
#define NX_DESC_INDEX    NX_OFF(nx_xp_desc_index)
#define NX_DESC_LEN      NX_OFF(nx_xp_desc_len)
#define NX_DATA_INDEX    NX_OFF(nx_xp_data_index)
#define NX_DATA_LEN      NX_OFF(nx_xp_data_len)

/* a checkpoint map: the object header, flags, count, then mappings of 40 bytes */
#define CPM_FLAGS        32
#define CPM_COUNT        36
#define CPM_MAP          40
#define CPM_ENTRY        40
#define CPM_LAST         0x1

#define NX_MAGIC_VALUE   0x4253584eu    /* "NXSB" */

void
apfs_obj_seal(uint8_t *obj, uint32_t size, uint64_t xid)
{
	t_put64(obj + NX_XID, xid);
	t_put64(obj, apfs_fletcher64(obj + 8, size - 8));
}

/*
 * Writes that follow each other on the device go down as one request: a commit is some thirty blocks
 * mostly next to each other, and each request costs the phone a third of a millisecond whatever
 * its size. What is kept here is no less safe than what the device's own cache holds: nothing counts
 * as written before t_sync, which sends it first. A read of any of it sends it too, and so does a
 * write that does not follow on, so that the device sees writes to one block in their order.
 */
#define T_GATHER_BYTES (1024 * 1024)

static int
t_gather_out(struct apfs_txn *txn)
{
	uint32_t bs = txn->nx->block_size, n = txn->gather_blocks;

	if (n == 0) {
		return 0;
	}
	txn->gather_blocks = 0;
	return txn->nx->dev.write(txn->nx->dev.ctx, txn->gather_block * bs, txn->gather, (size_t)n * bs);
}

static int
t_read(struct apfs_txn *txn, uint64_t block, void *buf, uint32_t blocks)
{
	uint32_t bs = txn->nx->block_size;

	if (txn->gather_blocks != 0 && block < txn->gather_block + txn->gather_blocks && txn->gather_block < block + blocks) {
		int error = t_gather_out(txn);

		if (error != 0) {
			return error;
		}
	}
	return txn->nx->dev.read(txn->nx->dev.ctx, block * bs, buf, (size_t)blocks * bs);
}

static int
t_write(struct apfs_txn *txn, uint64_t block, const void *buf, uint32_t blocks)
{
	uint32_t bs = txn->nx->block_size;

	size_t bytes = (size_t)blocks * bs;
	int error;

	if (txn->nx->dev.write == NULL) {
		return EROFS;
	}
	if (!txn->no_gather && bytes < T_GATHER_BYTES && txn->gather == NULL) {
		txn->gather = apfs_alloc(T_GATHER_BYTES);
	}
	if (txn->no_gather || txn->gather == NULL || bytes >= T_GATHER_BYTES) {
		if ((error = t_gather_out(txn)) != 0) {
			return error;
		}
		return txn->nx->dev.write(txn->nx->dev.ctx, block * bs, buf, bytes);
	}
	if (txn->gather_blocks != 0 && (block != txn->gather_block + txn->gather_blocks ||
	    ((size_t)txn->gather_blocks * bs) + bytes > T_GATHER_BYTES)) {
		if ((error = t_gather_out(txn)) != 0) {
			return error;
		}
	}
	if (txn->gather_blocks == 0) {
		txn->gather_block = block;
	}
	memcpy(txn->gather + (size_t)txn->gather_blocks * bs, buf, bytes);
	txn->gather_blocks += blocks;
	return 0;
}

uint64_t
apfs_txn_xid(const struct apfs_txn *txn)
{
	return t_le64(txn->sb + NX_NEXT_XID);
}

int
apfs_txn_read(struct apfs_txn *txn, uint64_t block, void *buf, uint32_t blocks)
{
	return t_read(txn, block, buf, blocks);
}

int
apfs_txn_write(struct apfs_txn *txn, uint64_t block, const void *buf, uint32_t blocks)
{
	if (block == 0 || block >= txn->nx->block_count || blocks > txn->nx->block_count - block) {
		return EINVAL;
	}
	return t_write(txn, block, buf, blocks);
}

struct apfs_txn_ephemeral *
apfs_txn_find(struct apfs_txn *txn, uint32_t type, uint64_t oid)
{
	for (uint32_t i = 0; i < txn->count; i++) {
		if ((txn->eph[i].type & APFS_OBJECT_TYPE_MASK) == type && (oid == 0 || txn->eph[i].oid == oid)) {
			return &txn->eph[i];
		}
	}
	return NULL;
}

static int
t_sync(struct apfs_txn *txn)
{
	int error = t_gather_out(txn);

	if (error != 0) {
		return error;
	}
	return txn->nx->dev.sync != NULL ? txn->nx->dev.sync(txn->nx->dev.ctx) : 0;
}

void
apfs_txn_fini(struct apfs_txn *txn)
{
	uint32_t bs = txn->nx != NULL ? txn->nx->block_size : 0;

	if (txn->gather != NULL) {
		apfs_free(txn->gather, T_GATHER_BYTES);
	}
	for (uint32_t i = 0; i < txn->count; i++) {
		apfs_free(txn->eph[i].data, txn->eph[i].size);
	}
	if (txn->sb != NULL) {
		apfs_free(txn->sb, bs);
	}
	memset(txn, 0, sizeof(*txn));
}

/*
 * The live checkpoint: the superblock with the highest transaction in the descriptor ring
 * that is the one apfs_nx_open read (nx->xid), and the maps before it.
 */
int
apfs_txn_load(struct apfs_nx *nx, struct apfs_txn *txn)
{
	uint32_t bs = nx->block_size, desc_blocks, data_blocks, index, len;
	uint64_t desc_base, data_base;
	uint8_t *blk = NULL;
	int error = 0;
	bool found = false;

	memset(txn, 0, sizeof(*txn));
	txn->nx = nx;
	txn->sb = apfs_alloc(bs);
	blk = apfs_alloc(bs);
	if (txn->sb == NULL || blk == NULL) {
		error = ENOMEM;
		goto out;
	}
	desc_base = nx->xp_desc_base;
	desc_blocks = nx->xp_desc_blocks;
	for (uint32_t i = 0; i < desc_blocks; i++) {
		if ((error = t_read(txn, desc_base + i, blk, 1)) != 0) {
			goto out;
		}
		if (t_le32(blk + NX_MAGIC) == NX_MAGIC_VALUE && (t_le32(blk + NX_TYPE) & APFS_OBJECT_TYPE_MASK) ==
		    APFS_OBJECT_TYPE_NX_SUPERBLOCK && apfs_checksum_ok(blk, bs) && t_le64(blk + NX_XID) == nx->xid) {
			memcpy(txn->sb, blk, bs);
			found = true;
			break;
		}
	}
	if (!found) {
		apfs_log("txn: the superblock of transaction %llu is not in the descriptor ring\n",
		    (unsigned long long)nx->xid);
		error = EIO;
		goto out;
	}
	data_base = t_le64(txn->sb + NX_DATA_BASE);
	data_blocks = t_le32(txn->sb + NX_DATA_BLOCKS);
	index = t_le32(txn->sb + NX_DESC_INDEX);
	len = t_le32(txn->sb + NX_DESC_LEN);
	if (desc_base != t_le64(txn->sb + NX_DESC_BASE) || desc_blocks != t_le32(txn->sb + NX_DESC_BLOCKS) ||
	    len < 2 || len > desc_blocks || index >= desc_blocks) {
		error = EIO;
		goto out;
	}
	/* the maps: every block of the checkpoint but its last, which is the superblock */
	for (uint32_t k = 0; k + 1 < len; k++) {
		uint32_t n;

		if ((error = t_read(txn, desc_base + (index + k) % desc_blocks, blk, 1)) != 0) {
			goto out;
		}
		if (!apfs_checksum_ok(blk, bs) || (t_le32(blk + NX_TYPE) & APFS_OBJECT_TYPE_MASK) !=
		    APFS_OBJECT_TYPE_CHECKPOINT_MAP || t_le64(blk + NX_XID) != nx->xid) {
			error = EIO;
			goto out;
		}
		n = t_le32(blk + CPM_COUNT);
		if (n > (bs - CPM_MAP) / CPM_ENTRY) {
			error = EIO;
			goto out;
		}
		for (uint32_t j = 0; j < n; j++) {
			const uint8_t *m = blk + CPM_MAP + j * CPM_ENTRY;
			struct apfs_txn_ephemeral *e;
			uint64_t paddr = t_le64(m + 32);
			uint32_t size = t_le32(m + 8), nb;

			if (txn->count == APFS_TXN_MAX_EPHEMERAL || size == 0 || size % bs != 0 ||
			    paddr < data_base || paddr >= data_base + data_blocks) {
				error = EIO;
				goto out;
			}
			e = &txn->eph[txn->count];
			e->type = t_le32(m);
			e->subtype = t_le32(m + 4);
			e->size = size;
			e->fs_oid = t_le64(m + 16);
			e->oid = t_le64(m + 24);
			e->data = apfs_alloc(size);
			if (e->data == NULL) {
				error = ENOMEM;
				goto out;
			}
			txn->count++;
			/* an object may wrap round the end of the ring a block at a time */
			nb = size / bs;
			for (uint32_t b = 0; b < nb; b++) {
				uint64_t at = data_base + (paddr - data_base + b) % data_blocks;

				if ((error = t_read(txn, at, e->data + (size_t)b * bs, 1)) != 0) {
					goto out;
				}
			}
			if (!apfs_checksum_ok(e->data, size) || t_le64(e->data + NX_OID) != e->oid) {
				apfs_log("txn: ephemeral object %llu does not check out\n", (unsigned long long)e->oid);
				error = EIO;
				goto out;
			}
		}
		if ((t_le32(blk + CPM_FLAGS) & CPM_LAST) != (k + 2 == len ? CPM_LAST : 0)) {
			error = EIO;
			goto out;
		}
	}
out:
	if (blk != NULL) {
		apfs_free(blk, bs);
	}
	if (error) {
		apfs_txn_fini(txn);
	}
	return error;
}

/* is ring block b (an index) within [index, index + len) of a ring of n blocks? */
static bool
in_ring(uint32_t b, uint32_t index, uint32_t len, uint32_t n)
{
	return (b + n - index) % n < len;
}

int
apfs_txn_commit(struct apfs_txn *txn)
{
	struct apfs_nx *nx = txn->nx;
	uint8_t *sb = txn->sb, *map = NULL;
	uint32_t bs = nx->block_size;
	uint32_t desc_blocks = t_le32(sb + NX_DESC_BLOCKS), data_blocks = t_le32(sb + NX_DATA_BLOCKS);
	uint64_t desc_base = t_le64(sb + NX_DESC_BASE), data_base = t_le64(sb + NX_DATA_BASE);
	uint32_t old_desc_index = t_le32(sb + NX_DESC_INDEX), old_desc_len = t_le32(sb + NX_DESC_LEN);
	uint32_t old_data_index = t_le32(sb + NX_DATA_INDEX), old_data_len = t_le32(sb + NX_DATA_LEN);
	uint32_t desc_at = t_le32(sb + NX_DESC_NEXT), data_at = t_le32(sb + NX_DATA_NEXT);
	uint32_t per_map = (bs - CPM_MAP) / CPM_ENTRY, maps, data_len = 0, d;
	uint64_t xid = t_le64(sb + NX_NEXT_XID);
	int error;

	maps = txn->count == 0 ? 1 : (txn->count + per_map - 1) / per_map;
	for (uint32_t i = 0; i < txn->count; i++) {
		data_len += txn->eph[i].size / bs;
	}
	/* nothing of the live checkpoint is written over */
	for (uint32_t k = 0; k < maps + 1; k++) {
		if (in_ring((desc_at + k) % desc_blocks, old_desc_index, old_desc_len, desc_blocks)) {
			return ENOSPC;
		}
	}
	for (uint32_t k = 0; k < data_len; k++) {
		if (in_ring((data_at + k) % data_blocks, old_data_index, old_data_len, data_blocks)) {
			return ENOSPC;
		}
	}
	map = apfs_alloc((size_t)maps * bs);
	if (map == NULL) {
		return ENOMEM;
	}

	/* 1. the ephemeral objects, into the data ring, and their mappings */
	d = data_at;
	for (uint32_t i = 0; i < txn->count; i++) {
		struct apfs_txn_ephemeral *e = &txn->eph[i];
		uint8_t *m = map + (size_t)(i / per_map) * bs + CPM_MAP + (i % per_map) * CPM_ENTRY;
		uint32_t nb = e->size / bs;

		apfs_obj_seal(e->data, e->size, xid);
		t_put32(m, e->type);
		t_put32(m + 4, e->subtype);
		t_put32(m + 8, e->size);
		t_put32(m + 12, 0);
		t_put64(m + 16, e->fs_oid);
		t_put64(m + 24, e->oid);
		t_put64(m + 32, data_base + d);
		for (uint32_t b = 0; b < nb; b++) {
			if ((error = t_write(txn, data_base + (d + b) % data_blocks, e->data + (size_t)b * bs, 1)) != 0) {
				goto out;
			}
		}
		d = (d + nb) % data_blocks;
	}
	/* 2. the maps, into the descriptor ring */
	for (uint32_t k = 0; k < maps; k++) {
		uint8_t *m = map + (size_t)k * bs;
		uint32_t n = txn->count - k * per_map;

		if (n > per_map) {
			n = per_map;
		}
		t_put64(m + NX_OID, desc_base + (desc_at + k) % desc_blocks);     /* a map is physical */
		t_put32(m + NX_TYPE, APFS_OBJ_PHYSICAL | APFS_OBJECT_TYPE_CHECKPOINT_MAP);
		t_put32(m + 28, 0);
		t_put32(m + CPM_FLAGS, k + 1 == maps ? CPM_LAST : 0);
		t_put32(m + CPM_COUNT, n);
		apfs_obj_seal(m, bs, xid);
		if ((error = t_write(txn, desc_base + (desc_at + k) % desc_blocks, m, 1)) != 0) {
			goto out;
		}
	}
	if ((error = t_sync(txn)) != 0) {
		goto out;
	}

	/* 3. the superblock: the commit */
	t_put64(sb + NX_NEXT_XID, xid + 1);
	t_put32(sb + NX_DESC_INDEX, desc_at);
	t_put32(sb + NX_DESC_LEN, maps + 1);
	t_put32(sb + NX_DESC_NEXT, (desc_at + maps + 1) % desc_blocks);
	t_put32(sb + NX_DATA_INDEX, data_at);
	t_put32(sb + NX_DATA_LEN, data_len);
	t_put32(sb + NX_DATA_NEXT, d);
	apfs_obj_seal(sb, bs, xid);
	if ((error = t_write(txn, desc_base + (desc_at + maps) % desc_blocks, sb, 1)) != 0 ||
	    (error = t_sync(txn)) != 0) {
		goto out;
	}
	/* 4. the copy at block 0, where a reader may start */
	if ((error = t_write(txn, 0, sb, 1)) != 0 || (error = t_sync(txn)) != 0) {
		goto out;
	}
	nx->xid = xid;
	txn->commits++;
out:
	apfs_free(map, (size_t)maps * bs);
	return error;
}
