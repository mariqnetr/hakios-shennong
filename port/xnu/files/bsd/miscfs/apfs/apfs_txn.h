/*
 * Writing APFS: transactions and checkpoints.  See apfs_txn.c.
 */

#ifndef _MISCFS_APFS_APFS_TXN_H_
#define _MISCFS_APFS_APFS_TXN_H_

#include "apfs_lib.h"

/* The most ephemeral objects one checkpoint carries (a fresh container has four) */
#define APFS_TXN_MAX_EPHEMERAL 64

struct apfs_txn_ephemeral {
	uint32_t  type, subtype;        /* as the checkpoint map has them (type with its flags) */
	uint32_t  size;                 /* bytes, a whole number of blocks */
	uint64_t  fs_oid, oid;
	uint8_t  *data;                 /* the object, size bytes; ours, written at the next commit */
};

/*
 * A container being written.  Loaded from the live checkpoint (apfs_txn_load); every commit
 * writes a new checkpoint after it in both rings and becomes the live one.
 */
struct apfs_txn {
	struct apfs_nx            *nx;
	uint8_t                   *sb;          /* the live container superblock, one block */
	uint32_t                   count;
	struct apfs_txn_ephemeral  eph[APFS_TXN_MAX_EPHEMERAL];
	uint64_t                   commits;
	/* writes that follow each other on the device, kept here and written as one (apfs_txn.c) */
	uint8_t                   *gather;
	uint64_t                   gather_block;
	uint32_t                   gather_blocks;
	bool                       no_gather;
};

int apfs_txn_load(struct apfs_nx *nx, struct apfs_txn *txn);
/* A new checkpoint with the next transaction identifier: the ephemeral objects as they are now. */
int apfs_txn_commit(struct apfs_txn *txn);
void apfs_txn_fini(struct apfs_txn *txn);
/* the block's object header: its xid, then its checksum made right */
void apfs_obj_seal(uint8_t *obj, uint32_t size, uint64_t xid);
/* the transaction the next commit makes */
uint64_t apfs_txn_xid(const struct apfs_txn *txn);
/* whole blocks of the container */
int apfs_txn_read(struct apfs_txn *txn, uint64_t block, void *buf, uint32_t blocks);
int apfs_txn_write(struct apfs_txn *txn, uint64_t block, const void *buf, uint32_t blocks);
/* the ephemeral object of this type (low half), or NULL */
struct apfs_txn_ephemeral *apfs_txn_find(struct apfs_txn *txn, uint32_t type, uint64_t oid);

#endif /* _MISCFS_APFS_APFS_TXN_H_ */
