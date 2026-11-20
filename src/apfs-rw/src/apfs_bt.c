/*
 * Writing APFS: B-trees, changed by copying.
 *
 * A node is read into memory as a list of records and written back whole, never patched: the
 * table of contents, the keys from the front and the values from the back, as compact as they
 * go, no free lists.  Nothing is ever written where it was read from -- a changed node goes into
 * a new block at the next flush -- so the live checkpoint keeps seeing every node it had.
 *
 * Two kinds of tree:
 *   physical  a child is named by its block address (an object map's tree).  A node written
 *             anew has a new address, so its parent changes with it, up to the root, whose new
 *             address the caller puts where the tree's root is named (om_tree_oid).
 *   virtual   a child is named by an object id that the tree's object map turns into an address
 *             (a volume's file-system tree).  A node written anew keeps its id; only its entry in
 *             the object map changes -- (id, this transaction) -> the new block -- and the old
 *             entry and block go.  The parent does not change at all.
 *
 * New physical nodes get a stand-in id with the top bit set until they have an address.
 *
 * Kept simple where APFS allows it: a node too full is split in two, a node left empty goes
 * (and its entry in its parent), but nodes are not merged and a root with one child stays.
 */

#include "apfs_bt.h"

#define BTN_FLAGS        32
#define BTN_LEVEL        34
#define BTN_NKEYS        36
#define BTN_TOC_OFF      40
#define BTN_TOC_LEN      42
#define BTN_FREE_OFF     44
#define BTN_FREE_LEN     46
#define BTN_KFL_OFF      48
#define BTN_KFL_LEN      50
#define BTN_VFL_OFF      52
#define BTN_VFL_LEN      54
#define BTN_DATA         56
#define BTI_SIZE         40
#define BTNODE_ROOT      0x1
#define BTNODE_LEAF      0x2
#define BTNODE_FIXED     0x4
#define TEMP_ID          (1ull << 63)

static inline uint16_t b_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t b_le32(const uint8_t *p) { return (uint32_t)b_le16(p) | (uint32_t)b_le16(p + 2) << 16; }
static inline uint64_t b_le64(const uint8_t *p) { return (uint64_t)b_le32(p) | (uint64_t)b_le32(p + 4) << 32; }
static inline void b_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void b_put32(uint8_t *p, uint32_t v) { b_put16(p, (uint16_t)v); b_put16(p + 2, (uint16_t)(v >> 16)); }
static inline void b_put64(uint8_t *p, uint64_t v) { b_put32(p, (uint32_t)v); b_put32(p + 4, (uint32_t)(v >> 32)); }

static uint64_t temp_ids;

int
apfs_bt_cmp_omap(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen)
{
	uint64_t x = b_le64(a), y = b_le64(b);

	(void)alen;
	(void)blen;
	if (x != y) {
		return x < y ? -1 : 1;
	}
	x = b_le64(a + 8);
	y = b_le64(b + 8);
	return x < y ? -1 : x > y;
}

/* ------------------------------------------------------------------ records */

static void
rec_free(struct apfs_bt_rec *r)
{
	if (r->k != NULL) {
		apfs_free(r->k, r->klen);
	}
	if (r->v != NULL) {
		apfs_free(r->v, r->vlen ? r->vlen : 1);
	}
	r->k = r->v = NULL;
}

static int
rec_set(struct apfs_bt_rec *r, const void *k, uint16_t klen, const void *v, uint16_t vlen)
{
	uint8_t *nk = NULL, *nv;

	if (k != NULL) {
		nk = apfs_alloc(klen);
		if (nk == NULL) {
			return ENOMEM;
		}
		memcpy(nk, k, klen);
	}
	nv = apfs_alloc(vlen ? vlen : 1);
	if (nv == NULL) {
		if (nk != NULL) {
			apfs_free(nk, klen);
		}
		return ENOMEM;
	}
	memcpy(nv, v, vlen);
	if (k != NULL) {
		if (r->k != NULL) {
			apfs_free(r->k, r->klen);
		}
		r->k = nk;
		r->klen = klen;
	}
	if (r->v != NULL) {
		apfs_free(r->v, r->vlen ? r->vlen : 1);
	}
	r->v = nv;
	r->vlen = vlen;
	return 0;
}

static int
node_insert_at(struct apfs_bt_node *n, uint32_t at, const void *k, uint16_t klen, const void *v, uint16_t vlen)
{
	int error;

	if (n->nrec == n->maxrec) {
		uint32_t more = n->maxrec ? n->maxrec * 2 : 32;
		struct apfs_bt_rec *r = apfs_alloc(more * sizeof(*r));

		if (r == NULL) {
			return ENOMEM;
		}
		if (n->rec != NULL) {
			memcpy(r, n->rec, n->nrec * sizeof(*r));
			apfs_free(n->rec, n->maxrec * sizeof(*r));
		}
		n->rec = r;
		n->maxrec = more;
	}
	memmove(&n->rec[at + 1], &n->rec[at], (n->nrec - at) * sizeof(*n->rec));
	memset(&n->rec[at], 0, sizeof(*n->rec));
	n->nrec++;
	if ((error = rec_set(&n->rec[at], k, klen, v, vlen)) != 0) {
		memmove(&n->rec[at], &n->rec[at + 1], (n->nrec - at - 1) * sizeof(*n->rec));
		n->nrec--;
	}
	n->dirty = true;
	return error;
}

static void
node_remove_at(struct apfs_bt_node *n, uint32_t at)
{
	rec_free(&n->rec[at]);
	memmove(&n->rec[at], &n->rec[at + 1], (n->nrec - at - 1) * sizeof(*n->rec));
	n->nrec--;
	n->dirty = true;
}

static void
node_free(struct apfs_bt_node *n)
{
	for (uint32_t i = 0; i < n->nrec; i++) {
		rec_free(&n->rec[i]);
	}
	if (n->rec != NULL) {
		apfs_free(n->rec, n->maxrec * sizeof(*n->rec));
	}
	apfs_free(n, sizeof(*n));
}

/* ------------------------------------------------------------------ nodes on the device */

static uint32_t
node_vsize(const struct apfs_bt *bt, const struct apfs_bt_node *n)
{
	return n->level > 0 ? 8 : bt->val_size;
}

/* keys and values on 8-byte boundaries, unless the tree says they are not */
static uint32_t
kvalign(const struct apfs_bt *bt, uint32_t x)
{
	return (bt->flags & APFS_BTREE_KV_NONALIGNED) ? x : (x + 7) & ~7u;
}

static uint32_t
node_room(const struct apfs_bt *bt, bool root)
{
	return bt->node_size - BTN_DATA - (root ? BTI_SIZE : 0);
}

/*
 * Entries the table of contents has room for, as Apple's driver sizes it (fsck_apfs holds a
 * node to it): with fixed sizes, as many as could ever fit in the node; otherwise as many as
 * there are records; either way rounded up to a multiple of 8 (BTREE_TOC_ENTRY_INCREMENT).
 */
static uint32_t
toc_entries(const struct apfs_bt *bt, const struct apfs_bt_node *n, bool root)
{
	uint32_t e;

	if (bt->key_size != 0) {
		e = node_room(bt, root) / (4 + bt->key_size + node_vsize(bt, n));
	} else {
		e = n->nrec;
	}
	e = (e + 7) & ~7u;
	return e ? e : 8;
}

/* bytes the node's records take, table of contents included */
static uint32_t
node_bytes(const struct apfs_bt *bt, const struct apfs_bt_node *n, bool root)
{
	bool fixed = bt->key_size != 0;
	uint32_t total = toc_entries(bt, n, root) * (fixed ? 4 : 8);

	for (uint32_t i = 0; i < n->nrec; i++) {
		total += fixed ? bt->key_size + node_vsize(bt, n) : kvalign(bt, n->rec[i].klen) + kvalign(bt, n->rec[i].vlen);
	}
	return total;
}

static int
node_parse(struct apfs_bt *bt, struct apfs_bt_node *n, const uint8_t *b)
{
	uint16_t flags = b_le16(b + BTN_FLAGS);
	bool fixed = (flags & BTNODE_FIXED) != 0, root = (flags & BTNODE_ROOT) != 0;
	uint32_t nkeys = b_le32(b + BTN_NKEYS), toc_len = b_le16(b + BTN_TOC_LEN);
	uint32_t kbase = BTN_DATA + b_le16(b + BTN_TOC_OFF) + toc_len, vend = bt->node_size - (root ? BTI_SIZE : 0);
	int error;

	n->level = b_le16(b + BTN_LEVEL);
	if (fixed != (bt->key_size != 0) || nkeys * (fixed ? 4u : 8u) > toc_len || kbase > vend) {
		return EIO;
	}
	for (uint32_t i = 0; i < nkeys; i++) {
		const uint8_t *e = b + BTN_DATA + b_le16(b + BTN_TOC_OFF) + i * (fixed ? 4 : 8);
		uint32_t koff, klen, voff, vlen;

		if (fixed) {
			koff = b_le16(e);
			voff = b_le16(e + 2);
			klen = bt->key_size;
			vlen = node_vsize(bt, n);
		} else {
			koff = b_le16(e);
			klen = b_le16(e + 2);
			voff = b_le16(e + 4);
			vlen = b_le16(e + 6);
		}
		if (kbase + koff + klen > vend || voff == 0xffff || voff > vend || vlen > voff ||
		    vend - voff < kbase) {
			return EIO;
		}
		if ((error = node_insert_at(n, n->nrec, b + kbase + koff, (uint16_t)klen, b + vend - voff,
		    (uint16_t)vlen)) != 0) {
			return error;
		}
	}
	n->dirty = false;
	return 0;
}

/* the node's block, ready to be written: o_oid and o_xid sealed by the caller */
static int
node_serialize(struct apfs_bt *bt, struct apfs_bt_node *n, bool root, uint64_t oid, uint8_t *b)
{
	bool fixed = bt->key_size != 0;
	uint32_t es = fixed ? 4 : 8, toc = toc_entries(bt, n, root) * es;
	uint32_t kbase = BTN_DATA + toc, vend = bt->node_size - (root ? BTI_SIZE : 0);
	uint32_t ko = 0, vo = 0;
	uint32_t storage = bt->physical ? APFS_OBJ_PHYSICAL : APFS_OBJ_VIRTUAL;

	if (node_bytes(bt, n, root) > node_room(bt, root)) {
		return E2BIG;
	}
	memset(b, 0, bt->node_size);
	b_put64(b + 8, oid);
	b_put32(b + 24, storage | (root ? APFS_OBJECT_TYPE_BTREE : APFS_OBJECT_TYPE_BTREE_NODE));
	b_put32(b + 28, bt->subtype);
	b_put16(b + BTN_FLAGS, (uint16_t)((root ? BTNODE_ROOT : 0) | (n->level == 0 ? BTNODE_LEAF : 0) |
	    (fixed ? BTNODE_FIXED : 0)));
	b_put16(b + BTN_LEVEL, n->level);
	b_put32(b + BTN_NKEYS, n->nrec);
	b_put16(b + BTN_TOC_OFF, 0);
	b_put16(b + BTN_TOC_LEN, (uint16_t)toc);
	for (uint32_t i = 0; i < n->nrec; i++) {
		struct apfs_bt_rec *r = &n->rec[i];
		uint8_t *e = b + BTN_DATA + i * es;
		uint32_t kl = fixed ? bt->key_size : r->klen, vl = fixed ? node_vsize(bt, n) : r->vlen;

		if (r->klen != kl || r->vlen != vl) {
			return EINVAL;
		}
		memcpy(b + kbase + ko, r->k, kl);
		vo += fixed ? vl : kvalign(bt, vl);
		memcpy(b + vend - vo, r->v, vl);
		if (fixed) {
			b_put16(e, (uint16_t)ko);
			b_put16(e + 2, (uint16_t)vo);
		} else {
			b_put16(e, (uint16_t)ko);
			b_put16(e + 2, (uint16_t)kl);
			b_put16(e + 4, (uint16_t)vo);
			b_put16(e + 6, (uint16_t)vl);
		}
		ko += fixed ? kl : kvalign(bt, kl);
	}
	b_put16(b + BTN_FREE_OFF, (uint16_t)ko);
	b_put16(b + BTN_FREE_LEN, (uint16_t)(vend - vo - kbase - ko));
	b_put16(b + BTN_KFL_OFF, 0xffff);
	b_put16(b + BTN_VFL_OFF, 0xffff);
	if (root) {
		uint8_t *i = b + bt->node_size - BTI_SIZE;

		b_put32(i, bt->flags);
		b_put32(i + 4, bt->node_size);
		b_put32(i + 8, bt->key_size);
		b_put32(i + 12, bt->val_size);
		b_put32(i + 16, bt->longest_key);
		b_put32(i + 20, bt->longest_val);
		b_put64(i + 24, bt->key_count);
		b_put64(i + 32, bt->node_count);
	}
	return 0;
}

/* ------------------------------------------------------------------ finding nodes */

static uint64_t
latest_xid(void)
{
	return ~0ull >> 1;
}

/* a virtual tree's node id -> (address, xid of its entry) through its object map */
static int
resolve(struct apfs_bt *bt, uint64_t oid, uint64_t *addr, uint64_t *xid)
{
	uint8_t key[16], k[16], v[16];
	uint16_t kl = sizeof(k), vl = sizeof(v);
	int error;

	b_put64(key, oid);
	b_put64(key + 8, latest_xid());
	if ((error = apfs_bt_get_le(bt->omap, key, 16, k, &kl, v, &vl)) != 0) {
		return error;
	}
	if (b_le64(k) != oid || vl != 16 || (b_le32(v) & 1) /* OMAP_VAL_DELETED */) {
		return ENOENT;
	}
	*addr = b_le64(v + 8);
	*xid = b_le64(k + 8);
	return 0;
}

static struct apfs_bt_node *
cached(struct apfs_bt *bt, uint64_t id)
{
	for (struct apfs_bt_node *n = bt->nodes; n != NULL; n = n->next) {
		if (n->oid == id) {
			return n;
		}
	}
	return NULL;
}

/* a node by the id its parent (or the tree) names it with */
static int
node_get(struct apfs_bt *bt, uint64_t id, struct apfs_bt_node **out)
{
	struct apfs_bt_node *n = cached(bt, id);
	uint64_t addr = id, xid = 0;
	uint8_t *b;
	int error;

	if (n != NULL) {
		*out = n;
		return 0;
	}
	if (id & TEMP_ID) {
		return EIO;
	}
	if (!bt->physical && (error = resolve(bt, id, &addr, &xid)) != 0) {
		apfs_log("bt: node %llu is not in the object map\n", (unsigned long long)id);
		return EIO;
	}
	b = apfs_alloc(bt->node_size);
	n = apfs_alloc(sizeof(*n));
	if (b == NULL || n == NULL) {
		error = ENOMEM;
		goto fail;
	}
	if ((error = apfs_txn_read(bt->sp->txn, addr, b, 1)) != 0) {
		goto fail;
	}
	if (!apfs_checksum_ok(b, bt->node_size) || b_le64(b + 8) != id ||
	    ((b_le32(b + 24) & APFS_OBJECT_TYPE_MASK) != APFS_OBJECT_TYPE_BTREE &&
	    (b_le32(b + 24) & APFS_OBJECT_TYPE_MASK) != APFS_OBJECT_TYPE_BTREE_NODE)) {
		apfs_log("bt: the node at %llu does not check out\n", (unsigned long long)addr);
		error = EIO;
		goto fail;
	}
	n->oid = id;
	n->addr = addr;
	n->level = b_le16(b + BTN_LEVEL);
	if ((error = node_parse(bt, n, b)) != 0) {
		goto fail;
	}
	apfs_free(b, bt->node_size);
	n->next = bt->nodes;
	bt->nodes = n;
	(void)xid;              /* a virtual node's old map entries are found again at flush */
	*out = n;
	return 0;
fail:
	if (b != NULL) {
		apfs_free(b, bt->node_size);
	}
	if (n != NULL) {
		node_free(n);
	}
	return error;
}

int
apfs_bt_open(struct apfs_bt *bt, struct apfs_space *sp, struct apfs_bt *omap, uint64_t root,
    apfs_bt_cmp cmp, uint64_t *next_oid)
{
	uint8_t *b, *i;
	uint64_t addr = root, xid;
	int error;

	memset(bt, 0, sizeof(*bt));
	bt->sp = sp;
	bt->omap = omap;
	bt->physical = omap == NULL;
	bt->cmp = cmp;
	bt->root = root;
	bt->next_oid = next_oid;
	bt->node_size = sp->txn->nx->block_size;
	if (!bt->physical && (error = resolve(bt, root, &addr, &xid)) != 0) {
		return error;
	}
	b = apfs_alloc(bt->node_size);
	if (b == NULL) {
		return ENOMEM;
	}
	if ((error = apfs_txn_read(sp->txn, addr, b, 1)) != 0) {
		goto out;
	}
	if (!apfs_checksum_ok(b, bt->node_size) || !(b_le16(b + BTN_FLAGS) & BTNODE_ROOT)) {
		error = EIO;
		goto out;
	}
	i = b + bt->node_size - BTI_SIZE;
	bt->subtype = b_le32(b + 28);
	bt->flags = b_le32(i);
	if (b_le32(i + 4) != bt->node_size || ((bt->flags & APFS_BTREE_PHYSICAL) != 0) != bt->physical) {
		error = ENOTSUP;
		goto out;
	}
	bt->key_size = b_le32(i + 8);
	bt->val_size = b_le32(i + 12);
	bt->longest_key = b_le32(i + 16);
	bt->longest_val = b_le32(i + 20);
	bt->key_count = b_le64(i + 24);
	bt->node_count = b_le64(i + 32);
	if (bt->flags & (APFS_BTREE_HASHED | APFS_BTREE_NOHEADER | APFS_BTREE_EPHEMERAL)) {
		error = ENOTSUP;
	}
out:
	apfs_free(b, bt->node_size);
	return error;
}

void
apfs_bt_close(struct apfs_bt *bt)
{
	if (bt->gone_oid != NULL) {
		apfs_free(bt->gone_oid, bt->maxgone * 8);
		apfs_free(bt->gone_addr, bt->maxgone * 8);
		bt->gone_oid = bt->gone_addr = NULL;
	}
	while (bt->nodes != NULL) {
		struct apfs_bt_node *n = bt->nodes;

		bt->nodes = n->next;
		node_free(n);
	}
}

/* ------------------------------------------------------------------ searching */

/* the index of the last record with key <= key, or -1 */
static int32_t
find_le(struct apfs_bt *bt, const struct apfs_bt_node *n, const void *key, uint16_t klen)
{
	int32_t lo = 0, hi = (int32_t)n->nrec - 1, best = -1;

	while (lo <= hi) {
		int32_t mid = (lo + hi) / 2;
		int c = bt->cmp(n->rec[mid].k, n->rec[mid].klen, key, klen);

		if (c <= 0) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return best;
}

struct path {
	uint32_t depth;
	struct apfs_bt_node *node[APFS_BT_MAX_DEPTH];
	int32_t idx[APFS_BT_MAX_DEPTH];     /* the record taken at each internal node */
};

/* from the root down to the leaf where key belongs */
static int
descend(struct apfs_bt *bt, const void *key, uint16_t klen, struct path *p)
{
	struct apfs_bt_node *n;
	int error;

	p->depth = 0;
	if ((error = node_get(bt, bt->root, &n)) != 0) {
		return error;
	}
	for (;;) {
		int32_t i;

		if (p->depth == APFS_BT_MAX_DEPTH) {
			return EIO;
		}
		p->node[p->depth] = n;
		if (n->level == 0) {
			p->idx[p->depth++] = -1;
			return 0;
		}
		if (n->nrec == 0) {
			return EIO;
		}
		i = find_le(bt, n, key, klen);
		if (i < 0) {
			i = 0;
		}
		p->idx[p->depth++] = i;
		if (n->rec[i].vlen != 8) {
			return EIO;
		}
		if ((error = node_get(bt, b_le64(n->rec[i].v), &n)) != 0) {
			return error;
		}
		if (n->level != p->node[p->depth - 1]->level - 1) {
			return EIO;
		}
	}
}

int
apfs_bt_get_le(struct apfs_bt *bt, const void *key, uint16_t klen, void *kout, uint16_t *klen_out,
    void *val, uint16_t *vlen)
{
	struct path p;
	struct apfs_bt_node *leaf;
	int32_t i;
	int error;

	if ((error = descend(bt, key, klen, &p)) != 0) {
		return error;
	}
	leaf = p.node[p.depth - 1];
	i = find_le(bt, leaf, key, klen);
	if (i < 0) {
		/* the greatest key <= key may be the last of the leaf before: go back up */
		int32_t d = (int32_t)p.depth - 2;

		while (d >= 0 && p.idx[d] == 0) {
			d--;
		}
		if (d < 0) {
			return ENOENT;
		}
		{
			struct apfs_bt_node *n = p.node[d];
			uint64_t id = b_le64(n->rec[p.idx[d] - 1].v);

			for (;;) {
				if ((error = node_get(bt, id, &n)) != 0) {
					return error;
				}
				if (n->nrec == 0) {
					return EIO;
				}
				if (n->level == 0) {
					break;
				}
				id = b_le64(n->rec[n->nrec - 1].v);
			}
			leaf = n;
			i = (int32_t)n->nrec - 1;
		}
	}
	if (kout != NULL) {
		if (leaf->rec[i].klen > *klen_out) {
			return ERANGE;
		}
		memcpy(kout, leaf->rec[i].k, leaf->rec[i].klen);
		*klen_out = leaf->rec[i].klen;
	}
	if (val != NULL) {
		if (leaf->rec[i].vlen > *vlen) {
			return ERANGE;
		}
		memcpy(val, leaf->rec[i].v, leaf->rec[i].vlen);
		*vlen = leaf->rec[i].vlen;
	}
	return 0;
}

int
apfs_bt_get(struct apfs_bt *bt, const void *key, uint16_t klen, void *val, uint16_t *vlen)
{
	struct path p;
	struct apfs_bt_node *leaf;
	int32_t i;
	int error;

	if ((error = descend(bt, key, klen, &p)) != 0) {
		return error;
	}
	leaf = p.node[p.depth - 1];
	i = find_le(bt, leaf, key, klen);
	if (i < 0 || bt->cmp(leaf->rec[i].k, leaf->rec[i].klen, key, klen) != 0) {
		return ENOENT;
	}
	if (val != NULL) {
		if (leaf->rec[i].vlen > *vlen) {
			return ERANGE;
		}
		memcpy(val, leaf->rec[i].v, leaf->rec[i].vlen);
	}
	if (vlen != NULL) {
		*vlen = leaf->rec[i].vlen;
	}
	return 0;
}

static int
walk_node(struct apfs_bt *bt, struct apfs_bt_node *n, const void *from, uint16_t flen,
    int (*cb)(void *, const uint8_t *, uint16_t, const uint8_t *, uint16_t), void *ctx)
{
	int32_t start = 0;
	int r;

	if (from != NULL) {
		start = find_le(bt, n, from, flen);
		if (start < 0) {
			start = 0;
		} else if (n->level == 0 && bt->cmp(n->rec[start].k, n->rec[start].klen, from, flen) < 0) {
			start++;
		}
	}
	for (uint32_t i = (uint32_t)start; i < n->nrec; i++) {
		if (n->level == 0) {
			if ((r = cb(ctx, n->rec[i].k, n->rec[i].klen, n->rec[i].v, n->rec[i].vlen)) != 0) {
				return r;
			}
		} else {
			struct apfs_bt_node *c;

			if ((r = node_get(bt, b_le64(n->rec[i].v), &c)) != 0) {
				return r;
			}
			if ((r = walk_node(bt, c, i == (uint32_t)start ? from : NULL, flen, cb, ctx)) != 0) {
				return r;
			}
		}
	}
	return 0;
}

int
apfs_bt_walk(struct apfs_bt *bt, const void *from, uint16_t flen,
    int (*cb)(void *ctx, const uint8_t *k, uint16_t klen, const uint8_t *v, uint16_t vlen), void *ctx)
{
	struct apfs_bt_node *root;
	int error;

	if ((error = node_get(bt, bt->root, &root)) != 0) {
		return error;
	}
	return walk_node(bt, root, from, flen, cb, ctx);
}

/* ------------------------------------------------------------------ changing */

static int
new_node(struct apfs_bt *bt, uint16_t level, struct apfs_bt_node **out)
{
	struct apfs_bt_node *n = apfs_alloc(sizeof(*n));

	if (n == NULL) {
		return ENOMEM;
	}
	if (bt->physical) {
		n->oid = TEMP_ID | ++temp_ids;
	} else {
		n->oid = (*bt->next_oid)++;
	}
	n->level = level;
	n->dirty = true;
	n->next = bt->nodes;
	bt->nodes = n;
	bt->node_count++;
	*out = n;
	return 0;
}

static void
mark_path(struct path *p)
{
	for (uint32_t d = 0; d < p->depth; d++) {
		p->node[d]->dirty = true;
	}
}

/* node p->node[d] is too full: move its upper half into a new right sibling, named in its parent */
static int
split(struct apfs_bt *bt, struct path *p, uint32_t d)
{
	struct apfs_bt_node *n = p->node[d], *r, *parent;
	uint32_t half = node_bytes(bt, n, false) / 2, acc = 0, cut, pidx;
	bool fixed = bt->key_size != 0;
	uint8_t id[8];
	int error;

	for (cut = 0; cut < n->nrec - 1; cut++) {
		acc += fixed ? 4 + bt->key_size + node_vsize(bt, n) : 8 + kvalign(bt, n->rec[cut].klen) + kvalign(bt, n->rec[cut].vlen);
		if (acc >= half) {
			cut++;
			break;
		}
	}
	if (cut == 0 || cut >= n->nrec) {
		return EIO;
	}
	if (d == 0) {
		/* the root splits: a new root above it, with it as its only child for now */
		struct apfs_bt_node *nr;

		if (p->depth == APFS_BT_MAX_DEPTH) {
			return EIO;
		}
		if ((error = new_node(bt, (uint16_t)(n->level + 1), &nr)) != 0) {
			return error;
		}
		b_put64(id, n->oid);
		if ((error = node_insert_at(nr, 0, n->rec[0].k, n->rec[0].klen, id, 8)) != 0) {
			return error;
		}
		bt->root = nr->oid;
		memmove(&p->node[1], &p->node[0], p->depth * sizeof(p->node[0]));
		memmove(&p->idx[1], &p->idx[0], p->depth * sizeof(p->idx[0]));
		p->node[0] = nr;
		p->idx[0] = 0;
		p->depth++;
		d = 1;
	}
	parent = p->node[d - 1];
	pidx = (uint32_t)p->idx[d - 1];
	if ((error = new_node(bt, n->level, &r)) != 0) {
		return error;
	}
	for (uint32_t i = cut; i < n->nrec; i++) {
		if ((error = node_insert_at(r, r->nrec, n->rec[i].k, n->rec[i].klen, n->rec[i].v, n->rec[i].vlen)) != 0) {
			return error;
		}
	}
	while (n->nrec > cut) {
		node_remove_at(n, n->nrec - 1);
	}
	b_put64(id, r->oid);
	if ((error = node_insert_at(parent, pidx + 1, r->rec[0].k, r->rec[0].klen, id, 8)) != 0) {
		return error;
	}
	/* the parent may be too full now in its turn */
	if (node_bytes(bt, parent, parent->oid == bt->root) > node_room(bt, parent->oid == bt->root)) {
		return split(bt, p, d - 1);
	}
	return 0;
}

int
apfs_bt_put(struct apfs_bt *bt, const void *key, uint16_t klen, const void *val, uint16_t vlen)
{
	struct path p;
	struct apfs_bt_node *leaf;
	int32_t i;
	int error;

	if ((bt->key_size && klen != bt->key_size) || (bt->val_size && vlen != bt->val_size)) {
		return EINVAL;
	}
	if ((error = descend(bt, key, klen, &p)) != 0) {
		return error;
	}
	/* a key below everything in the tree: the first separators on the way down become it */
	for (uint32_t d = 0; d + 1 < p.depth; d++) {
		struct apfs_bt_node *n = p.node[d];

		if (p.idx[d] == 0 && bt->cmp(key, klen, n->rec[0].k, n->rec[0].klen) < 0) {
			if ((error = rec_set(&n->rec[0], key, klen, n->rec[0].v, n->rec[0].vlen)) != 0) {
				return error;
			}
		}
	}
	mark_path(&p);
	leaf = p.node[p.depth - 1];
	i = find_le(bt, leaf, key, klen);
	if (i >= 0 && bt->cmp(leaf->rec[i].k, leaf->rec[i].klen, key, klen) == 0) {
		if ((error = rec_set(&leaf->rec[i], NULL, 0, val, vlen)) != 0) {
			return error;
		}
	} else {
		if ((error = node_insert_at(leaf, (uint32_t)(i + 1), key, klen, val, vlen)) != 0) {
			return error;
		}
		bt->key_count++;
	}
	if (klen > bt->longest_key) {
		bt->longest_key = klen;
	}
	if (vlen > bt->longest_val) {
		bt->longest_val = vlen;
	}
	if (node_bytes(bt, leaf, leaf->oid == bt->root) > node_room(bt, leaf->oid == bt->root)) {
		return split(bt, &p, p.depth - 1);
	}
	return 0;
}

/* a node that is going: given up at the next flush (or now, if it never reached the device) */
static int retire(struct apfs_bt *bt, struct apfs_bt_node *n);

int
apfs_bt_del(struct apfs_bt *bt, const void *key, uint16_t klen)
{
	struct path p;
	struct apfs_bt_node *leaf;
	int32_t i;
	int error;

	if ((error = descend(bt, key, klen, &p)) != 0) {
		return error;
	}
	leaf = p.node[p.depth - 1];
	i = find_le(bt, leaf, key, klen);
	if (i < 0 || bt->cmp(leaf->rec[i].k, leaf->rec[i].klen, key, klen) != 0) {
		return ENOENT;
	}
	mark_path(&p);
	node_remove_at(leaf, (uint32_t)i);
	bt->key_count--;
	/*
	 * A parent's key for a child is that child's first key, exactly (fsck_apfs holds a tree to
	 * it): when the first record goes, the separators above follow the new first key.
	 */
	if (i == 0 && leaf->nrec > 0) {
		for (int32_t d = (int32_t)p.depth - 2; d >= 0; d--) {
			struct apfs_bt_node *n = p.node[d], *c = p.node[d + 1];
			struct apfs_bt_rec *r = &n->rec[p.idx[d]];

			if ((error = rec_set(r, c->rec[0].k, c->rec[0].klen, r->v, r->vlen)) != 0) {
				return error;
			}
			if (p.idx[d] != 0) {
				break;
			}
		}
	}
	/* an empty node other than the root goes, and its entry in its parent */
	for (int32_t d = (int32_t)p.depth - 1; d > 0 && p.node[d]->nrec == 0; d--) {
		if ((error = retire(bt, p.node[d])) != 0) {
			return error;
		}
		node_remove_at(p.node[d - 1], (uint32_t)p.idx[d - 1]);
	}
	/* an internal root left empty: the tree is empty, the root a leaf again */
	if (p.node[0]->nrec == 0 && p.node[0]->level > 0) {
		p.node[0]->level = 0;
	}
	return 0;
}

/* ------------------------------------------------------------------ writing */

static int
retire(struct apfs_bt *bt, struct apfs_bt_node *n)
{
	struct apfs_bt_node **pp;

	for (pp = &bt->nodes; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == n) {
			*pp = n->next;
			break;
		}
	}
	bt->node_count--;
	if (n->addr != 0) {
		if (bt->ngone == bt->maxgone) {
			uint32_t more = bt->maxgone ? bt->maxgone * 2 : 32;
			uint64_t *o = apfs_alloc(more * 8), *a = apfs_alloc(more * 8);

			if (o == NULL || a == NULL) {
				node_free(n);
				return ENOMEM;
			}
			if (bt->gone_oid != NULL) {
				memcpy(o, bt->gone_oid, bt->ngone * 8);
				memcpy(a, bt->gone_addr, bt->ngone * 8);
				apfs_free(bt->gone_oid, bt->maxgone * 8);
				apfs_free(bt->gone_addr, bt->maxgone * 8);
			}
			bt->gone_oid = o;
			bt->gone_addr = a;
			bt->maxgone = more;
		}
		bt->gone_oid[bt->ngone] = n->oid;
		bt->gone_addr[bt->ngone++] = n->addr;
	}
	node_free(n);
	return 0;
}

/* a physical node now at a new address: its parent's record for it says so */
static void
renamed(struct apfs_bt *bt, uint16_t level, uint64_t from, uint64_t to)
{
	for (struct apfs_bt_node *n = bt->nodes; n != NULL; n = n->next) {
		if (n->level != level + 1) {
			continue;
		}
		for (uint32_t i = 0; i < n->nrec; i++) {
			if (b_le64(n->rec[i].v) == from) {
				b_put64(n->rec[i].v, to);
				n->dirty = true;
			}
		}
	}
}

/* every old map entry of a virtual node: (oid, xid) for every xid < now */
static int
omap_drop_old(struct apfs_bt *bt, uint64_t oid, uint64_t now)
{
	for (;;) {
		uint8_t key[16], k[16], v[16];
		uint16_t kl = 16, vl = 16;
		int error;

		b_put64(key, oid);
		b_put64(key + 8, now - 1);
		error = apfs_bt_get_le(bt->omap, key, 16, k, &kl, v, &vl);
		if (error == ENOENT || (error == 0 && b_le64(k) != oid)) {
			return 0;
		}
		if (error) {
			return error;
		}
		if ((error = apfs_bt_del(bt->omap, k, 16)) != 0) {
			return error;
		}
	}
}

int
apfs_bt_flush(struct apfs_bt *bt)
{
	struct apfs_space *sp = bt->sp;
	uint64_t xid = apfs_txn_xid(sp->txn);
	uint8_t *b = apfs_alloc(bt->node_size);
	uint16_t top = 0;
	int error = 0;

	if (b == NULL) {
		return ENOMEM;
	}
	for (struct apfs_bt_node *n = bt->nodes; n != NULL; n = n->next) {
		if (n->level > top) {
			top = n->level;
		}
	}
	/* the nodes given up: their blocks, and a virtual one's map entries */
	for (uint32_t g = 0; g < bt->ngone; g++) {
		if (!bt->physical && (error = omap_drop_old(bt, bt->gone_oid[g], xid + 1)) != 0) {
			goto out;
		}
		if ((error = apfs_space_free(sp, bt->gone_addr[g], 1)) != 0) {
			goto out;
		}
		bt->blocks_delta--;
	}
	bt->ngone = 0;
	/* from the leaves up, so that a physical parent learns its children's new addresses first */
	for (uint32_t level = 0; level <= top; level++) {
		for (struct apfs_bt_node *n = bt->nodes; n != NULL; n = n->next) {
			uint64_t at, old = n->addr, id;
			bool root;

			if (n->level != level || !n->dirty) {
				continue;
			}
			if ((error = apfs_space_alloc(sp, 1, &at)) != 0) {
				goto out;
			}
			bt->blocks_delta++;
			root = n->oid == bt->root;
			id = bt->physical ? at : n->oid;
			if ((error = node_serialize(bt, n, root, id, b)) != 0) {
				goto out;
			}
			apfs_obj_seal(b, bt->node_size, xid);
			if ((error = apfs_txn_write(sp->txn, at, b, 1)) != 0) {
				goto out;
			}
			if (old != 0) {
				if ((error = apfs_space_free(sp, old, 1)) != 0) {
					goto out;
				}
				bt->blocks_delta--;
			}
			n->addr = at;
			n->dirty = false;
			if (bt->physical) {
				if (root) {
					bt->root = at;
				} else {
					renamed(bt, n->level, n->oid, at);
				}
				n->oid = at;
			} else {
				uint8_t k[16], v[16];

				b_put64(k, n->oid);
				b_put64(k + 8, xid);
				b_put32(v, 0);
				b_put32(v + 4, bt->node_size);
				b_put64(v + 8, at);
				if ((error = omap_drop_old(bt, n->oid, xid)) != 0 ||
				    (error = apfs_bt_put(bt->omap, k, 16, v, 16)) != 0) {
					goto out;
				}
			}
		}
	}
out:
	apfs_free(b, bt->node_size);
	return error;
}
