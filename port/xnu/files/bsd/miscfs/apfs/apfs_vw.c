/*
 * Writing APFS: a volume, from its file-system tree up to the container superblock.
 *
 * What a commit writes, from the bottom up (apfs_write.c's list, steps 2 to 8):
 *   1. the file-system tree's changed nodes, each into a new block entered in the volume's
 *      object map under this transaction (apfs_bt_flush, virtual tree);
 *   2. the volume's object map: its changed nodes up to a new root, then its omap_phys anew;
 *   3. the volume superblock, anew, naming that omap_phys, entered in the container's map;
 *   4. the container's object map the same way as 2; the container superblock names it;
 *   5. the space manager's bitmaps (apfs_space_flush), then the checkpoint (apfs_txn_commit).
 * A block that is replaced is given up to the space manager, which frees it with this
 * checkpoint and lets nothing take it before.
 */

#include "apfs_vw.h"

static inline uint32_t v_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t v_le64(const uint8_t *p) { return (uint64_t)v_le32(p) | (uint64_t)v_le32(p + 4) << 32; }
static inline void v_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline void v_put64(uint8_t *p, uint64_t v) { v_put32(p, (uint32_t)v); v_put32(p + 4, (uint32_t)(v >> 32)); }

#define NXO(f)  ((uint32_t)__builtin_offsetof(struct apfs_nx_superblock, f))
#define VO(f)   ((uint32_t)__builtin_offsetof(struct apfs_superblock, f))
#define OM_TREE_OID   ((uint32_t)__builtin_offsetof(struct apfs_omap_phys, om_tree_oid))

#define J_TYPE_SHIFT  60
#define J_ID_MASK     0x0fffffffffffffffULL
#define T_XATTR       4
#define T_SIBLING     5
#define T_EXTENT      8
#define T_DIR_REC     9

static int
cmp_bytes(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl)
{
	uint32_t n = al < bl ? al : bl;
	int c = memcmp(a, b, n);

	if (c != 0) {
		return c < 0 ? -1 : 1;
	}
	return al < bl ? -1 : al > bl;
}

int
apfs_bt_cmp_fs(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen)
{
	uint64_t ha = v_le64(a), hb = v_le64(b);
	uint64_t ia = ha & J_ID_MASK, ib = hb & J_ID_MASK;
	uint32_t ta = (uint32_t)(ha >> J_TYPE_SHIFT), tb = (uint32_t)(hb >> J_TYPE_SHIFT);

	if (ia != ib) {
		return ia < ib ? -1 : 1;
	}
	if (ta != tb) {
		return ta < tb ? -1 : 1;
	}
	switch (ta) {
	case T_DIR_REC:
		/* hashed: the word of hash and length, then the name (with its NUL) */
		if (alen >= 12 && blen >= 12) {
			uint32_t x = v_le32(a + 8), y = v_le32(b + 8);

			if (x != y) {
				return x < y ? -1 : 1;
			}
			return cmp_bytes(a + 12, alen - 12u, b + 12, blen - 12u);
		}
		return cmp_bytes(a + 8, alen - 8u, b + 8, blen - 8u);
	case T_XATTR:
		if (alen >= 10 && blen >= 10) {
			return cmp_bytes(a + 10, alen - 10u, b + 10, blen - 10u);
		}
		return 0;
	case T_EXTENT:
	case T_SIBLING:
		if (alen >= 16 && blen >= 16) {
			uint64_t x = v_le64(a + 8), y = v_le64(b + 8);

			return x < y ? -1 : x > y;
		}
		return 0;
	default:
		return 0;
	}
}

void
apfs_vw_close(struct apfs_vw *vw)
{
	uint32_t bs = vw->nx != NULL ? vw->nx->block_size : 0;

	apfs_bt_close(&vw->fs);
	apfs_bt_close(&vw->er);
	apfs_bt_close(&vw->vmap);
	apfs_bt_close(&vw->cmap);
	apfs_space_fini(&vw->sp);
	apfs_txn_fini(&vw->txn);
	if (vw->vsb != NULL) {
		apfs_free(vw->vsb, bs);
	}
	memset(vw, 0, sizeof(*vw));
}

/* an object map's omap_phys at addr: the root of its tree */
static int
omap_root(struct apfs_vw *vw, uint64_t addr, uint64_t *root)
{
	uint32_t bs = vw->nx->block_size;
	uint8_t *b = apfs_alloc(bs);
	int error;

	if (b == NULL) {
		return ENOMEM;
	}
	if ((error = apfs_txn_read(&vw->txn, addr, b, 1)) == 0) {
		if (!apfs_checksum_ok(b, bs) || (v_le32(b + 24) & APFS_OBJECT_TYPE_MASK) != APFS_OBJECT_TYPE_OMAP ||
		    v_le32(b + 36) != 0 /* om_snap_count: no snapshots */) {
			error = ENOTSUP;
		} else {
			*root = v_le64(b + OM_TREE_OID);
		}
	}
	apfs_free(b, bs);
	return error;
}

int
apfs_vw_open(struct apfs_nx *nx, uint32_t index, struct apfs_vw *vw)
{
	uint32_t bs = nx->block_size;
	uint8_t key[16], k[16], v[16];
	uint16_t kl = 16, vl = 16;
	uint64_t root;
	int error;

	memset(vw, 0, sizeof(*vw));
	vw->nx = nx;
	if ((error = apfs_txn_load(nx, &vw->txn)) != 0 || (error = apfs_space_load(&vw->txn, &vw->sp)) != 0) {
		goto fail;
	}
	vw->next_oid = v_le64(vw->txn.sb + NXO(nx_next_oid));
	vw->comap_addr = v_le64(vw->txn.sb + NXO(nx_omap_oid));
	vw->vol_oid = v_le64(vw->txn.sb + NXO(nx_fs_oid) + 8 * index);
	if (vw->vol_oid == 0) {
		error = ENOENT;
		goto fail;
	}
	if ((error = omap_root(vw, vw->comap_addr, &root)) != 0 ||
	    (error = apfs_bt_open(&vw->cmap, &vw->sp, NULL, root, apfs_bt_cmp_omap, NULL)) != 0) {
		goto fail;
	}
	/* the volume superblock, through the container's map */
	v_put64(key, vw->vol_oid);
	v_put64(key + 8, ~0ull >> 1);
	if ((error = apfs_bt_get_le(&vw->cmap, key, 16, k, &kl, v, &vl)) != 0 || v_le64(k) != vw->vol_oid) {
		error = error ? error : EIO;
		goto fail;
	}
	vw->vsb_addr = v_le64(v + 8);
	vw->vsb = apfs_alloc(bs);
	if (vw->vsb == NULL) {
		error = ENOMEM;
		goto fail;
	}
	if ((error = apfs_txn_read(&vw->txn, vw->vsb_addr, vw->vsb, 1)) != 0) {
		goto fail;
	}
	if (!apfs_checksum_ok(vw->vsb, bs) || v_le32(vw->vsb + VO(apfs_magic)) != 0x42535041 /* APSB */ ||
	    (v_le64(vw->vsb + VO(apfs_incompatible_features)) & ~0x9ull) != 0 /* only case and normalization insensitivity */) {
		apfs_log("vw: a volume this does not write (features %llx)\n",
		    (unsigned long long)v_le64(vw->vsb + VO(apfs_incompatible_features)));
		error = ENOTSUP;
		goto fail;
	}
	if ((v_le64(vw->vsb + VO(apfs_fs_flags)) & 0x1) == 0 /* APFS_FS_UNENCRYPTED */) {
		error = ENOTSUP;
		goto fail;
	}
	vw->vomap_addr = v_le64(vw->vsb + VO(apfs_omap_oid));
	if ((error = omap_root(vw, vw->vomap_addr, &root)) != 0 ||
	    (error = apfs_bt_open(&vw->vmap, &vw->sp, NULL, root, apfs_bt_cmp_omap, NULL)) != 0 ||
	    (error = apfs_bt_open(&vw->fs, &vw->sp, &vw->vmap, v_le64(vw->vsb + VO(apfs_root_tree_oid)),
	    apfs_bt_cmp_fs, &vw->next_oid)) != 0 ||
	    (error = apfs_bt_open(&vw->er, &vw->sp, NULL, v_le64(vw->vsb + VO(apfs_extentref_tree_oid)),
	    apfs_bt_cmp_fs, NULL)) != 0) {
		goto fail;
	}
	vw->open = true;
	return 0;
fail:
	apfs_vw_close(vw);
	return error;
}

/* an omap_phys anew, naming the tree's new root: its new address */
static int
omap_rewrite(struct apfs_vw *vw, uint64_t *addr, uint64_t root, int64_t *delta)
{
	uint32_t bs = vw->nx->block_size;
	uint8_t *b = apfs_alloc(bs);
	uint64_t at;
	int error;

	if (b == NULL) {
		return ENOMEM;
	}
	if ((error = apfs_txn_read(&vw->txn, *addr, b, 1)) != 0 || (error = apfs_space_alloc(&vw->sp, 1, &at)) != 0) {
		goto out;
	}
	v_put64(b + 8, at);
	v_put64(b + OM_TREE_OID, root);
	apfs_obj_seal(b, bs, apfs_txn_xid(&vw->txn));
	if ((error = apfs_txn_write(&vw->txn, at, b, 1)) != 0 || (error = apfs_space_free(&vw->sp, *addr, 1)) != 0) {
		goto out;
	}
	*addr = at;
	(void)delta;
out:
	apfs_free(b, bs);
	return error;
}

int
apfs_vw_commit(struct apfs_vw *vw)
{
	uint32_t bs = vw->nx->block_size;
	uint64_t xid = apfs_txn_xid(&vw->txn), at;
	uint8_t k[16], v[16];
	int64_t vol_blocks;
	int error;

	/* 1, 2: the file-system tree and the extent references, then the volume's object map, which 1 changed */
	if ((error = apfs_bt_flush(&vw->fs)) != 0 || (error = apfs_bt_flush(&vw->er)) != 0 ||
	    (error = apfs_bt_flush(&vw->vmap)) != 0 ||
	    (error = omap_rewrite(vw, &vw->vomap_addr, vw->vmap.root, NULL)) != 0) {
		return error;
	}
	/* 3: the volume superblock */
	vol_blocks = vw->fs.blocks_delta + vw->er.blocks_delta + vw->vmap.blocks_delta + vw->data_delta;
	vw->fs.blocks_delta = vw->er.blocks_delta = vw->vmap.blocks_delta = vw->data_delta = 0;
	v_put64(vw->vsb + VO(apfs_fs_alloc_count), v_le64(vw->vsb + VO(apfs_fs_alloc_count)) + (uint64_t)vol_blocks);
	if (vol_blocks > 0) {
		v_put64(vw->vsb + VO(apfs_total_blocks_alloced), v_le64(vw->vsb + VO(apfs_total_blocks_alloced)) + (uint64_t)vol_blocks);
	} else if (vol_blocks < 0) {
		v_put64(vw->vsb + VO(apfs_total_blocks_freed), v_le64(vw->vsb + VO(apfs_total_blocks_freed)) + (uint64_t)-vol_blocks);
	}
	v_put64(vw->vsb + VO(apfs_omap_oid), vw->vomap_addr);
	v_put64(vw->vsb + VO(apfs_root_tree_oid), vw->fs.root);
	v_put64(vw->vsb + VO(apfs_extentref_tree_oid), vw->er.root);
	if ((error = apfs_space_alloc(&vw->sp, 1, &at)) != 0) {
		return error;
	}
	apfs_obj_seal(vw->vsb, bs, xid);
	if ((error = apfs_txn_write(&vw->txn, at, vw->vsb, 1)) != 0 || (error = apfs_space_free(&vw->sp, vw->vsb_addr, 1)) != 0) {
		return error;
	}
	vw->vsb_addr = at;
	/* its entry in the container's map: the new one in, the old ones out */
	for (;;) {
		uint8_t key[16];
		uint16_t kl = 16, vl = 16;

		v_put64(key, vw->vol_oid);
		v_put64(key + 8, xid - 1);
		error = apfs_bt_get_le(&vw->cmap, key, 16, k, &kl, v, &vl);
		if (error == ENOENT || (error == 0 && v_le64(k) != vw->vol_oid)) {
			break;
		}
		if (error != 0 || (error = apfs_bt_del(&vw->cmap, k, 16)) != 0) {
			return error;
		}
	}
	v_put64(k, vw->vol_oid);
	v_put64(k + 8, xid);
	v_put32(v, 0);
	v_put32(v + 4, bs);
	v_put64(v + 8, at);
	if ((error = apfs_bt_put(&vw->cmap, k, 16, v, 16)) != 0) {
		return error;
	}
	/* 4: the container's object map, and the container superblock names it */
	if ((error = apfs_bt_flush(&vw->cmap)) != 0 || (error = omap_rewrite(vw, &vw->comap_addr, vw->cmap.root, NULL)) != 0) {
		return error;
	}
	v_put64(vw->txn.sb + NXO(nx_omap_oid), vw->comap_addr);
	v_put64(vw->txn.sb + NXO(nx_next_oid), vw->next_oid);
	/* 5 */
	if ((error = apfs_space_flush(&vw->sp)) != 0) {
		return error;
	}
	return apfs_txn_commit(&vw->txn);
}
