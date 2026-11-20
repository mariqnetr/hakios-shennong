/*
 * Writing APFS: B-trees, changed by copying.  See apfs_bt.c.
 */

#ifndef _MISCFS_APFS_APFS_BT_H_
#define _MISCFS_APFS_APFS_BT_H_

#include "apfs_space.h"

#define APFS_BT_MAX_DEPTH 12

/* how a tree's keys sort: <0, 0, >0 */
typedef int (*apfs_bt_cmp)(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen);

/* a node in memory: its records, one buffer each, in key order */
struct apfs_bt_rec {
	uint8_t  *k, *v;
	uint16_t  klen, vlen;
};

struct apfs_bt_node {
	struct apfs_bt_node *next;      /* the tree's list of nodes in memory */
	uint64_t  oid;                  /* physical tree: the block it was read from (0: new) */
	uint64_t  addr;                 /* where it is on the device now (0: not written yet) */
	uint16_t  level;
	bool      dirty;
	uint32_t  nrec, maxrec;
	struct apfs_bt_rec *rec;
};

struct apfs_bt {
	struct apfs_space   *sp;
	struct apfs_bt      *omap;      /* a virtual tree: the object map its node ids are looked up in */
	bool                 physical;  /* child ids are block addresses */
	uint32_t             subtype;   /* o_subtype of its nodes (APFS_OBJECT_TYPE_OMAP, _FSTREE, ...) */
	uint32_t             flags;     /* bt_flags */
	uint32_t             key_size, val_size;    /* fixed sizes, 0 if variable */
	uint32_t             node_size;
	uint32_t             longest_key, longest_val;
	uint64_t             key_count, node_count;
	apfs_bt_cmp          cmp;
	uint64_t             root;      /* physical: address of the root; virtual: its oid */
	struct apfs_bt_node *nodes;
	uint64_t            *next_oid;  /* a virtual tree: where new node ids come from (nx_next_oid) */
	int64_t              blocks_delta;  /* blocks taken less blocks given up (a volume's alloc count) */
	uint64_t            *gone_oid, *gone_addr;   /* nodes given up since the last flush */
	uint32_t             ngone, maxgone;
};

/* open a tree whose root is at root (an address, or an oid for a virtual tree) */
int apfs_bt_open(struct apfs_bt *bt, struct apfs_space *sp, struct apfs_bt *omap, uint64_t root,
    apfs_bt_cmp cmp, uint64_t *next_oid);
void apfs_bt_close(struct apfs_bt *bt);
/* the record with this key: its value into val (up to *vlen bytes), its length into *vlen; ENOENT */
int apfs_bt_get(struct apfs_bt *bt, const void *key, uint16_t klen, void *val, uint16_t *vlen);
/* the record with the greatest key <= key */
int apfs_bt_get_le(struct apfs_bt *bt, const void *key, uint16_t klen, void *kout, uint16_t *klen_out,
    void *val, uint16_t *vlen);
/* insert, or replace the value of the record with the same key */
int apfs_bt_put(struct apfs_bt *bt, const void *key, uint16_t klen, const void *val, uint16_t vlen);
int apfs_bt_del(struct apfs_bt *bt, const void *key, uint16_t klen);
/* every record from the first with key >= from, until cb returns nonzero (which is returned) */
int apfs_bt_walk(struct apfs_bt *bt, const void *from, uint16_t flen,
    int (*cb)(void *ctx, const uint8_t *k, uint16_t klen, const uint8_t *v, uint16_t vlen), void *ctx);
/*
 * Write every changed node: a physical tree's into new blocks (the root's new address in bt->root),
 * a virtual tree's into new blocks entered in its object map under the commit's transaction.
 */
int apfs_bt_flush(struct apfs_bt *bt);
/* the nodes in memory that are as on the device: let go of (read again when wanted) */
void apfs_bt_trim(struct apfs_bt *bt);

/* key orders of the trees this writes */
int apfs_bt_cmp_omap(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen);

#endif /* _MISCFS_APFS_APFS_BT_H_ */
