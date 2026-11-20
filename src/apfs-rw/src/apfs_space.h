/*
 * Writing APFS: the space manager.  See apfs_space.c.
 */

#ifndef _MISCFS_APFS_APFS_SPACE_H_
#define _MISCFS_APFS_APFS_SPACE_H_

#include "apfs_txn.h"

struct apfs_space_chunk {
	uint64_t  bitmap_addr;          /* 0: no bitmap, every block of the chunk is free */
	uint32_t  free_count;
	bool      dirty;                /* its bitmap changed in this transaction */
	uint8_t  *bitmap;               /* one block, or NULL while bitmap_addr is 0 and nothing changed */
};

struct apfs_space_free {
	uint64_t addr, count;
	bool     ip;
};

struct apfs_space {
	struct apfs_txn          *txn;
	struct apfs_txn_ephemeral *sm;  /* the space manager among txn's ephemeral objects */
	uint32_t                  blocks_per_chunk, chunks_per_cib;
	uint64_t                  block_count, chunk_count;
	uint32_t                  cib_count;
	uint64_t                 *cib_addr;     /* cib_count */
	bool                     *cib_dirty;
	uint8_t                 **cib;          /* cib_count blocks */
	struct apfs_space_chunk  *chunk;        /* chunk_count */
	uint64_t                  free_count;
	/* the internal pool: the space manager's own blocks (chunk bitmaps, chunk-info blocks) */
	uint64_t                  ip_base, ip_blocks;
	uint64_t                  ip_bm_base;
	uint32_t                  ip_bm_blocks;
	uint8_t                  *ip_bitmap;    /* one block */
	/* blocks given up in this transaction: free from its checkpoint on, never taken before it */
	struct apfs_space_free   *frees;
	uint32_t                  nfrees, maxfrees;
};

int apfs_space_load(struct apfs_txn *txn, struct apfs_space *sp);
void apfs_space_fini(struct apfs_space *sp);
/* count contiguous blocks of the main device, free since the last checkpoint: their first in *addr */
int apfs_space_alloc(struct apfs_space *sp, uint64_t count, uint64_t *addr);
/* blocks no longer used from this transaction's checkpoint on */
int apfs_space_free(struct apfs_space *sp, uint64_t addr, uint64_t count);
/* everything the space manager has to write for the next commit; then apfs_txn_commit */
int apfs_space_flush(struct apfs_space *sp);

#endif /* _MISCFS_APFS_APFS_SPACE_H_ */
