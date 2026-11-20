/*
 * Writing APFS: the file system's own records.
 *
 * Laid out as Apple's driver lays them out on the same volume (the reference is an image
 * newfs_apfs made and macOS wrote files into, read back record by record):
 *
 *   inode       key (id, INODE); value j_inode_val: parent, private id (= id), four times,
 *               internal flags (INODE_NO_RSRC_FORK), children or links, protection class,
 *               write generation, BSD flags, owner, group, mode, uncompressed size; then the
 *               extended fields: always the name (INO_EXT_TYPE_NAME, XF_DO_NOT_COPY), and for a
 *               file with contents its data stream (INO_EXT_TYPE_DSTREAM, XF_SYSTEM_FIELD).
 *   dir record  key (parent, DIR_REC) + (hash << 10 | length with NUL) + name with NUL;
 *               value: id, date added, the type as DT_*.
 *
 * The directory's inode counts its children and its times change; the volume superblock
 * counts files and directories and hands out ids (apfs_next_obj_id).
 */

#include "apfs_fsw.h"

static inline uint16_t f_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t f_le32(const uint8_t *p) { return (uint32_t)f_le16(p) | (uint32_t)f_le16(p + 2) << 16; }
static inline uint64_t f_le64(const uint8_t *p) { return (uint64_t)f_le32(p) | (uint64_t)f_le32(p + 4) << 32; }
static inline void f_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void f_put32(uint8_t *p, uint32_t v) { f_put16(p, (uint16_t)v); f_put16(p + 2, (uint16_t)(v >> 16)); }
static inline void f_put64(uint8_t *p, uint64_t v) { f_put32(p, (uint32_t)v); f_put32(p + 4, (uint32_t)(v >> 32)); }

#define VO(f)   ((uint32_t)__builtin_offsetof(struct apfs_superblock, f))

#define T_INODE         3
#define T_DIR_REC       9
#define J_TYPE_SHIFT    60

/* j_inode_val_t */
#define INO_PARENT      0
#define INO_PRIVATE     8
#define INO_CREATE      16
#define INO_MOD         24
#define INO_CHANGE      32
#define INO_ACCESS      40
#define INO_IFLAGS      48
#define INO_NCHILDREN   56
#define INO_PROT        60
#define INO_WGEN        64
#define INO_BSDFLAGS    68
#define INO_OWNER       72
#define INO_GROUP       76
#define INO_MODE        80
#define INO_USIZE       84
#define INO_XFIELDS     92
#define INODE_NO_RSRC_FORK 0x8000
#define INO_EXT_TYPE_NAME  4
#ifndef ENOATTR
#define ENOATTR_ 93
#else
#define ENOATTR_ ENOATTR
#endif
#define XF_DO_NOT_COPY     0x02
#define APFS_INCOMPAT_CASE_INSENSITIVE 0x1

#define S_IFMT_         0170000
#define S_IFDIR_        0040000

static uint64_t
jkey(uint64_t id, uint32_t type)
{
	return id | (uint64_t)type << J_TYPE_SHIFT;
}

static bool
case_insensitive(struct apfs_vw *vw)
{
	return (f_le64(vw->vsb + VO(apfs_incompatible_features)) & APFS_INCOMPAT_CASE_INSENSITIVE) != 0;
}

/* the directory record's key for name in parent */
static int
drec_key(struct apfs_vw *vw, uint64_t parent, const char *name, size_t len, uint8_t *k, uint16_t *klen)
{
	uint32_t hash;

	if (len == 0 || len > 255 || memchr(name, '\0', len) != NULL || !apfs_name_hash(name, len, case_insensitive(vw), &hash)) {
		return EINVAL;
	}
	f_put64(k, jkey(parent, T_DIR_REC));
	f_put32(k + 8, hash << 10 | (uint32_t)(len + 1));
	memcpy(k + 12, name, len);
	k[12 + len] = '\0';
	*klen = (uint16_t)(12 + len + 1);
	return 0;
}

static uint8_t
dtype(uint16_t mode)
{
	return (uint8_t)((mode & S_IFMT_) >> 12);
}

int
apfs_fsw_create(struct apfs_vw *vw, uint64_t parent, const char *name, size_t name_len,
    const struct apfs_fsw_attr *a, uint64_t *id_out)
{
	uint8_t dk[12 + 256], k[8], ino[INO_XFIELDS + 8 + 256 + 8], pino[1024], dv[18];
	uint16_t dklen, plen = sizeof(pino), x;
	uint64_t id;
	bool dir = (a->mode & S_IFMT_) == S_IFDIR_;
	int error;

	if ((error = drec_key(vw, parent, name, name_len, dk, &dklen)) != 0) {
		return error;
	}
	if (apfs_bt_get(&vw->fs, dk, dklen, NULL, NULL) == 0) {
		return EEXIST;
	}
	/* the parent: a directory */
	f_put64(k, jkey(parent, T_INODE));
	if ((error = apfs_bt_get(&vw->fs, k, 8, pino, &plen)) != 0) {
		return error;
	}
	if (plen < INO_XFIELDS || (f_le16(pino + INO_MODE) & S_IFMT_) != S_IFDIR_) {
		return ENOTDIR;
	}
	id = f_le64(vw->vsb + VO(apfs_next_obj_id));

	/* the inode, its name as an extended field */
	memset(ino, 0, sizeof(ino));
	f_put64(ino + INO_PARENT, parent);
	f_put64(ino + INO_PRIVATE, id);
	f_put64(ino + INO_CREATE, a->now);
	f_put64(ino + INO_MOD, a->now);
	f_put64(ino + INO_CHANGE, a->now);
	f_put64(ino + INO_ACCESS, a->now);
	f_put64(ino + INO_IFLAGS, INODE_NO_RSRC_FORK);
	f_put32(ino + INO_NCHILDREN, dir ? 0 : 1);
	f_put32(ino + INO_WGEN, 1);
	f_put32(ino + INO_OWNER, a->uid);
	f_put32(ino + INO_GROUP, a->gid);
	f_put16(ino + INO_MODE, a->mode);
	f_put16(ino + INO_XFIELDS, 1);                          /* xf_num_exts */
	x = (uint16_t)((name_len + 1 + 7) & ~7u);
	f_put16(ino + INO_XFIELDS + 2, x);                      /* xf_used_data */
	ino[INO_XFIELDS + 4] = INO_EXT_TYPE_NAME;
	ino[INO_XFIELDS + 5] = XF_DO_NOT_COPY;
	f_put16(ino + INO_XFIELDS + 6, (uint16_t)(name_len + 1));
	memcpy(ino + INO_XFIELDS + 8, name, name_len);
	f_put64(k, jkey(id, T_INODE));
	if ((error = apfs_bt_put(&vw->fs, k, 8, ino, (uint16_t)(INO_XFIELDS + 8 + x))) != 0) {
		return error;
	}
	/* its record in the directory */
	f_put64(dv, id);
	f_put64(dv + 8, a->now);
	f_put16(dv + 16, dtype(a->mode));
	if ((error = apfs_bt_put(&vw->fs, dk, dklen, dv, sizeof(dv))) != 0) {
		return error;
	}
	/* the directory: one child more, changed now */
	f_put32(pino + INO_NCHILDREN, f_le32(pino + INO_NCHILDREN) + 1);
	f_put64(pino + INO_MOD, a->now);
	f_put64(pino + INO_CHANGE, a->now);
	f_put32(pino + INO_WGEN, f_le32(pino + INO_WGEN) + 1);
	f_put64(k, jkey(parent, T_INODE));
	if ((error = apfs_bt_put(&vw->fs, k, 8, pino, plen)) != 0) {
		return error;
	}
	/* the volume */
	f_put64(vw->vsb + VO(apfs_next_obj_id), id + 1);
	if (dir) {
		f_put64(vw->vsb + VO(apfs_num_directories), f_le64(vw->vsb + VO(apfs_num_directories)) + 1);
	} else if ((a->mode & S_IFMT_) == 0100000) {
		f_put64(vw->vsb + VO(apfs_num_files), f_le64(vw->vsb + VO(apfs_num_files)) + 1);
	} else if ((a->mode & S_IFMT_) == 0120000) {
		f_put64(vw->vsb + VO(apfs_num_symlinks), f_le64(vw->vsb + VO(apfs_num_symlinks)) + 1);
	} else {
		f_put64(vw->vsb + VO(apfs_num_other_fsobjects), f_le64(vw->vsb + VO(apfs_num_other_fsobjects)) + 1);
	}
	f_put64(vw->vsb + VO(apfs_last_mod_time), a->now);
	*id_out = id;
	return 0;
}

/* ------------------------------------------------------------------ inodes and their extended fields */

#define T_XATTR         4
#define T_DSTREAM_ID    6
#define T_EXTENT        8
#define T_PHYS_EXT      2
#define INO_EXT_TYPE_DSTREAM 8
#define XF_SYSTEM_FIELD 0x20
#define INO_MAX         1024
#define XF_MAX          16
#define XATTR_EMBEDDED  0x2
#define XATTR_FS_OWNED  0x4
#define PEXT_KIND_NEW   1ull
#define EXT_LEN_MASK    0x00ffffffffffffffULL
#define S_IFLNK_        0120000
#define S_IFREG_        0100000
#define SYMLINK_EA      "com.apple.fs.symlink"

struct xfield {
	uint8_t  type, flags;
	uint16_t size;
	uint8_t  data[300];
};

struct inode {
	uint64_t     id;
	uint8_t      v[INO_MAX];
	uint16_t     len;
	uint32_t     nxf;
	struct xfield xf[XF_MAX];
};

static int
ino_load(struct apfs_vw *vw, uint64_t id, struct inode *in)
{
	uint8_t k[8];
	uint16_t len = sizeof(in->v);
	uint32_t at;
	int error;

	f_put64(k, jkey(id, T_INODE));
	if ((error = apfs_bt_get(&vw->fs, k, 8, in->v, &len)) != 0) {
		return error;
	}
	if (len < INO_XFIELDS) {
		return EIO;
	}
	in->id = id;
	in->len = len;
	in->nxf = 0;
	if (len < INO_XFIELDS + 4) {
		return 0;
	}
	in->nxf = f_le16(in->v + INO_XFIELDS);
	if (in->nxf > XF_MAX) {
		return EIO;
	}
	at = INO_XFIELDS + 4 + 4 * in->nxf;
	for (uint32_t i = 0; i < in->nxf; i++) {
		const uint8_t *h = in->v + INO_XFIELDS + 4 + 4 * i;
		struct xfield *x = &in->xf[i];

		x->type = h[0];
		x->flags = h[1];
		x->size = f_le16(h + 2);
		if (x->size > sizeof(x->data) || at + x->size > len) {
			return EIO;
		}
		memcpy(x->data, in->v + at, x->size);
		at += (x->size + 7u) & ~7u;
	}
	return 0;
}

static int
ino_store(struct apfs_vw *vw, struct inode *in)
{
	uint8_t k[8];
	uint32_t at, used = 0;

	/* the fields sorted by type, as Apple's driver keeps them */
	for (uint32_t i = 1; i < in->nxf; i++) {
		for (uint32_t j = i; j > 0 && in->xf[j - 1].type > in->xf[j].type; j--) {
			struct xfield t = in->xf[j];

			in->xf[j] = in->xf[j - 1];
			in->xf[j - 1] = t;
		}
	}
	for (uint32_t i = 0; i < in->nxf; i++) {
		used += (in->xf[i].size + 7u) & ~7u;
	}
	if (in->nxf == 0) {
		in->len = INO_XFIELDS;
	} else {
		at = INO_XFIELDS + 4 + 4 * in->nxf;
		if (at + used > sizeof(in->v)) {
			return E2BIG;
		}
		memset(in->v + INO_XFIELDS, 0, sizeof(in->v) - INO_XFIELDS);
		f_put16(in->v + INO_XFIELDS, (uint16_t)in->nxf);
		f_put16(in->v + INO_XFIELDS + 2, (uint16_t)used);
		for (uint32_t i = 0; i < in->nxf; i++) {
			uint8_t *h = in->v + INO_XFIELDS + 4 + 4 * i;

			h[0] = in->xf[i].type;
			h[1] = in->xf[i].flags;
			f_put16(h + 2, in->xf[i].size);
			memcpy(in->v + at, in->xf[i].data, in->xf[i].size);
			at += (in->xf[i].size + 7u) & ~7u;
		}
		in->len = (uint16_t)at;
	}
	f_put64(k, jkey(in->id, T_INODE));
	return apfs_bt_put(&vw->fs, k, 8, in->v, in->len);
}

static struct xfield *
xf_find(struct inode *in, uint8_t type)
{
	for (uint32_t i = 0; i < in->nxf; i++) {
		if (in->xf[i].type == type) {
			return &in->xf[i];
		}
	}
	return NULL;
}

static struct xfield *
xf_add(struct inode *in, uint8_t type, uint8_t flags, uint16_t size)
{
	struct xfield *x = xf_find(in, type);

	if (x == NULL) {
		if (in->nxf == XF_MAX) {
			return NULL;
		}
		x = &in->xf[in->nxf++];
		memset(x, 0, sizeof(*x));
		x->type = type;
		x->flags = flags;
	}
	x->size = size;
	return x;
}

static void
xf_remove(struct inode *in, uint8_t type)
{
	for (uint32_t i = 0; i < in->nxf; i++) {
		if (in->xf[i].type == type) {
			memmove(&in->xf[i], &in->xf[i + 1], (in->nxf - i - 1) * sizeof(in->xf[0]));
			in->nxf--;
			return;
		}
	}
}

/* the data stream: size, alloced, crypto id, bytes written, bytes read */
#define DS_SIZE     0
#define DS_ALLOCED  8
#define DS_WRITTEN  24

static uint8_t *
dstream(struct inode *in, bool make)
{
	struct xfield *x = xf_find(in, INO_EXT_TYPE_DSTREAM);

	if (x == NULL && make) {
		x = xf_add(in, INO_EXT_TYPE_DSTREAM, XF_SYSTEM_FIELD, 40);
		if (x != NULL) {
			memset(x->data, 0, 40);
		}
	}
	return x != NULL && x->size >= 40 ? x->data : NULL;
}

static void
touch(struct inode *in, uint64_t now, bool mod)
{
	if (mod) {
		f_put64(in->v + INO_MOD, now);
	}
	f_put64(in->v + INO_CHANGE, now);
	f_put32(in->v + INO_WGEN, f_le32(in->v + INO_WGEN) + 1);
}

/* ------------------------------------------------------------------ extents */

struct extent {
	uint64_t laddr, len, pblk;      /* bytes, bytes, block (0: a hole) */
};

struct ext_list {
	uint64_t id;
	struct extent *e;
	uint32_t n, max;
	int error;
};

static int
ext_cb(void *ctx, const uint8_t *k, uint16_t klen, const uint8_t *v, uint16_t vlen)
{
	struct ext_list *l = ctx;
	uint64_t h = f_le64(k);

	if ((h & 0x0fffffffffffffffULL) != l->id || (h >> 60) != T_EXTENT) {
		return 1;
	}
	if (klen < 16 || vlen < 24) {
		l->error = EIO;
		return 1;
	}
	if (l->n == l->max) {
		uint32_t more = l->max ? l->max * 2 : 16;
		struct extent *e = apfs_alloc(more * sizeof(*e));

		if (e == NULL) {
			l->error = ENOMEM;
			return 1;
		}
		if (l->e != NULL) {
			memcpy(e, l->e, l->n * sizeof(*e));
			apfs_free(l->e, l->max * sizeof(*e));
		}
		l->e = e;
		l->max = more;
	}
	l->e[l->n++] = (struct extent){ .laddr = f_le64(k + 8), .len = f_le64(v) & EXT_LEN_MASK, .pblk = f_le64(v + 8) };
	return 0;
}

static int
ext_load(struct apfs_vw *vw, uint64_t dsid, struct ext_list *l)
{
	uint8_t k[16];
	int r;

	memset(l, 0, sizeof(*l));
	l->id = dsid;
	f_put64(k, jkey(dsid, T_EXTENT));
	f_put64(k + 8, 0);
	r = apfs_bt_walk(&vw->fs, k, 16, ext_cb, l);
	return l->error ? l->error : (r < 0 ? -r : (r == 1 ? 0 : r));
}

static void
ext_fini(struct ext_list *l)
{
	if (l->e != NULL) {
		apfs_free(l->e, l->max * sizeof(*l->e));
	}
}

static int
ext_put(struct apfs_vw *vw, uint64_t dsid, const struct extent *e)
{
	uint8_t k[16], v[24];

	f_put64(k, jkey(dsid, T_EXTENT));
	f_put64(k + 8, e->laddr);
	f_put64(v, e->len);
	f_put64(v + 8, e->pblk);
	f_put64(v + 16, 0);
	return apfs_bt_put(&vw->fs, k, 16, v, 24);
}

static int
ext_del(struct apfs_vw *vw, uint64_t dsid, uint64_t laddr)
{
	uint8_t k[16];

	f_put64(k, jkey(dsid, T_EXTENT));
	f_put64(k + 8, laddr);
	return apfs_bt_del(&vw->fs, k, 16);
}

/* the extent-reference record of blocks [pblk, pblk + blocks) owned by dsid (0 blocks: none) */
static int
pext_set(struct apfs_vw *vw, uint64_t pblk, uint64_t blocks, uint64_t dsid)
{
	uint8_t k[8], v[20];

	f_put64(k, jkey(pblk, T_PHYS_EXT));
	if (blocks == 0) {
		return apfs_bt_del(&vw->er, k, 8);
	}
	f_put64(v, blocks | PEXT_KIND_NEW << 60);
	f_put64(v + 8, dsid);
	f_put32(v + 16, 1);
	return apfs_bt_put(&vw->er, k, 8, v, 20);
}

/* blocks of a file given up: the space manager frees them with the commit */
static int
blocks_free(struct apfs_vw *vw, uint64_t pblk, uint64_t n)
{
	int error = apfs_space_free(&vw->sp, pblk, n);

	if (error == 0) {
		vw->data_delta -= (int64_t)n;
	}
	return error;
}

static int
dsid_ensure(struct apfs_vw *vw, uint64_t dsid)
{
	uint8_t k[8], v[4];

	f_put64(k, jkey(dsid, T_DSTREAM_ID));
	if (apfs_bt_get(&vw->fs, k, 8, NULL, NULL) == 0) {
		return 0;
	}
	f_put32(v, 1);
	return apfs_bt_put(&vw->fs, k, 8, v, 4);
}

static uint64_t
private_id(const struct inode *in)
{
	return f_le64(in->v + INO_PRIVATE);
}

/* ------------------------------------------------------------------ contents */

int
apfs_fsw_write(struct apfs_vw *vw, uint64_t id, uint64_t off, const void *buf, size_t len, uint64_t now)
{
	uint32_t bs = vw->nx->block_size;
	const uint8_t *src = buf;
	struct inode in;
	struct ext_list l;
	uint8_t *ds, *blk = NULL;
	uint64_t dsid, end = off + len, pos, alloced = 0;
	int error;

	if (len == 0) {
		return 0;
	}
	if ((error = ino_load(vw, id, &in)) != 0) {
		return error;
	}
	if ((f_le16(in.v + INO_MODE) & S_IFMT_) != S_IFREG_) {
		return EISDIR;
	}
	dsid = private_id(&in);
	if ((ds = dstream(&in, true)) == NULL) {
		return E2BIG;
	}
	if ((error = ext_load(vw, dsid, &l)) != 0) {
		return error;
	}
	blk = apfs_alloc(bs);
	if (blk == NULL) {
		error = ENOMEM;
		goto out;
	}
	for (uint32_t i = 0; i < l.n; i++) {
		if (l.e[i].laddr + l.e[i].len > alloced) {
			alloced = l.e[i].laddr + l.e[i].len;
		}
	}
	/* 1. what lies within the blocks the file has: in place */
	for (pos = off; pos < end && pos < alloced;) {
		uint64_t b = pos / bs * bs, in_blk = pos - b, n = bs - in_blk;
		struct extent *e = NULL;

		if (n > end - pos) {
			n = end - pos;
		}
		for (uint32_t i = 0; i < l.n; i++) {
			if (b >= l.e[i].laddr && b < l.e[i].laddr + l.e[i].len) {
				e = &l.e[i];
				break;
			}
		}
		if (e == NULL || e->pblk == 0) {
			/* a hole inside the file: written below, as part of a new extent of its own */
			break;
		}
		{
			uint64_t pb = e->pblk + (b - e->laddr) / bs;

			if (n != bs) {
				if ((error = apfs_txn_read(&vw->txn, pb, blk, 1)) != 0) {
					goto out;
				}
			}
			memcpy(blk + in_blk, src + (pos - off), n);
			if ((error = apfs_txn_write(&vw->txn, pb, blk, 1)) != 0) {
				goto out;
			}
		}
		pos += n;
	}
	/* 2. the rest: new blocks, after a hole extent for any gap; one extent per run taken */
	if (pos < end) {
		uint64_t first = pos / bs * bs, nblocks = (end + bs - 1) / bs - first / bs;

		if (first > alloced) {
			struct extent hole = { .laddr = alloced, .len = first - alloced, .pblk = 0 };

			if ((error = ext_put(vw, dsid, &hole)) != 0) {
				goto out;
			}
		}
		if (first < alloced) {
			/* a hole within the file is filled: only appending is done so far */
			error = ENOTSUP;
			goto out;
		}
		while (nblocks > 0) {
			uint64_t want = nblocks, pb;
			struct extent e;

			while ((error = apfs_space_alloc(&vw->sp, want, &pb)) == ENOSPC && want > 1) {
				want /= 2;
			}
			if (error) {
				goto out;
			}
			vw->data_delta += (int64_t)want;
			for (uint64_t k = 0; k < want; k++) {
				uint64_t lb = first + k * bs;

				memset(blk, 0, bs);
				if (lb + bs > off && lb < end) {
					uint64_t from = lb > off ? lb : off, to = lb + bs < end ? lb + bs : end;

					memcpy(blk + (from - lb), src + (from - off), to - from);
				}
				if ((error = apfs_txn_write(&vw->txn, pb + k, blk, 1)) != 0) {
					goto out;
				}
			}
			e = (struct extent){ .laddr = first, .len = want * bs, .pblk = pb };
			if ((error = ext_put(vw, dsid, &e)) != 0 || (error = pext_set(vw, pb, want, dsid)) != 0) {
				goto out;
			}
			first += want * bs;
			nblocks -= want;
			alloced = first;
		}
		if ((error = dsid_ensure(vw, dsid)) != 0) {
			goto out;
		}
	}
	/* 3. the inode: size, blocks, bytes written, times */
	if (end > f_le64(ds + DS_SIZE)) {
		f_put64(ds + DS_SIZE, end);
	}
	f_put64(ds + DS_ALLOCED, (f_le64(ds + DS_SIZE) + bs - 1) / bs * bs > alloced ? alloced : alloced);
	f_put64(ds + DS_WRITTEN, f_le64(ds + DS_WRITTEN) + len);
	touch(&in, now, true);
	error = ino_store(vw, &in);
out:
	if (blk != NULL) {
		apfs_free(blk, bs);
	}
	ext_fini(&l);
	return error;
}

int
apfs_fsw_truncate(struct apfs_vw *vw, uint64_t id, uint64_t size, uint64_t now)
{
	uint32_t bs = vw->nx->block_size;
	struct inode in;
	struct ext_list l;
	uint8_t *ds;
	uint64_t dsid, keep = (size + bs - 1) / bs * bs, alloced = 0;
	int error;

	if ((error = ino_load(vw, id, &in)) != 0) {
		return error;
	}
	if ((f_le16(in.v + INO_MODE) & S_IFMT_) != S_IFREG_) {
		return EISDIR;
	}
	dsid = private_id(&in);
	if ((ds = dstream(&in, true)) == NULL) {
		return E2BIG;
	}
	if ((error = ext_load(vw, dsid, &l)) != 0) {
		return error;
	}
	for (uint32_t i = 0; i < l.n; i++) {
		struct extent *e = &l.e[i];

		if (e->laddr >= keep) {
			/* wholly past the new end */
			if ((error = ext_del(vw, dsid, e->laddr)) != 0) {
				goto out;
			}
			if (e->pblk != 0 && ((error = pext_set(vw, e->pblk, 0, dsid)) != 0 ||
			    (error = blocks_free(vw, e->pblk, e->len / bs)) != 0)) {
				goto out;
			}
		} else if (e->laddr + e->len > keep) {
			/* across it: its tail goes */
			uint64_t kept = keep - e->laddr, gone = e->len - kept;
			struct extent t = { .laddr = e->laddr, .len = kept, .pblk = e->pblk };

			if ((error = ext_put(vw, dsid, &t)) != 0) {
				goto out;
			}
			if (e->pblk != 0 && ((error = pext_set(vw, e->pblk, kept / bs, dsid)) != 0 ||
			    (error = blocks_free(vw, e->pblk + kept / bs, gone / bs)) != 0)) {
				goto out;
			}
			alloced = keep;
		} else if (e->laddr + e->len > alloced) {
			alloced = e->laddr + e->len;
		}
	}
	/* the bytes of the last block past the new end read as zeros if the file grows again */
	if (size % bs != 0 && size < f_le64(ds + DS_SIZE)) {
		for (uint32_t i = 0; i < l.n; i++) {
			struct extent *e = &l.e[i];
			uint64_t b = size / bs * bs;

			if (e->pblk != 0 && b >= e->laddr && b < e->laddr + e->len) {
				uint8_t *blk = apfs_alloc(bs);
				uint64_t pb = e->pblk + (b - e->laddr) / bs;

				if (blk == NULL) {
					error = ENOMEM;
					goto out;
				}
				if ((error = apfs_txn_read(&vw->txn, pb, blk, 1)) == 0) {
					memset(blk + size % bs, 0, bs - size % bs);
					error = apfs_txn_write(&vw->txn, pb, blk, 1);
				}
				apfs_free(blk, bs);
				if (error) {
					goto out;
				}
			}
		}
	}
	f_put64(ds + DS_SIZE, size);
	f_put64(ds + DS_ALLOCED, alloced);
	touch(&in, now, true);
	error = ino_store(vw, &in);
out:
	ext_fini(&l);
	return error;
}

/* ------------------------------------------------------------------ names */

int
apfs_fsw_lookup(struct apfs_vw *vw, uint64_t parent, const char *name, size_t name_len, uint64_t *id, uint8_t *dt)
{
	uint8_t dk[12 + 256], dv[64];
	uint16_t dklen, dvlen = sizeof(dv);
	int error;

	if ((error = drec_key(vw, parent, name, name_len, dk, &dklen)) != 0 ||
	    (error = apfs_bt_get(&vw->fs, dk, dklen, dv, &dvlen)) != 0) {
		return error;
	}
	if (dvlen < 18) {
		return EIO;
	}
	*id = f_le64(dv);
	if (dt != NULL) {
		*dt = (uint8_t)(f_le16(dv + 16) & 0xf);
	}
	return 0;
}

struct key_list {
	uint64_t id;
	struct apfs_bt_rec *r;
	uint32_t n, max;
	int error;
};

static int
keys_cb(void *ctx, const uint8_t *k, uint16_t klen, const uint8_t *v, uint16_t vlen)
{
	struct key_list *l = ctx;

	if ((f_le64(k) & 0x0fffffffffffffffULL) != l->id) {
		return 1;
	}
	if (l->n == l->max) {
		uint32_t more = l->max ? l->max * 2 : 16;
		struct apfs_bt_rec *r = apfs_alloc(more * sizeof(*r));

		if (r == NULL) {
			l->error = ENOMEM;
			return 1;
		}
		if (l->r != NULL) {
			memcpy(r, l->r, l->n * sizeof(*r));
			apfs_free(l->r, l->max * sizeof(*r));
		}
		l->r = r;
		l->max = more;
	}
	l->r[l->n].k = apfs_alloc(klen);
	if (l->r[l->n].k == NULL) {
		l->error = ENOMEM;
		return 1;
	}
	memcpy(l->r[l->n].k, k, klen);
	l->r[l->n].klen = klen;
	l->n++;
	(void)v;
	(void)vlen;
	return 0;
}

/* every record of object id goes; its blocks too */
static int
destroy(struct apfs_vw *vw, struct inode *in)
{
	uint32_t bs = vw->nx->block_size;
	struct key_list kl;
	struct ext_list el;
	uint8_t k[8];
	uint64_t dsid = private_id(in);
	int error;

	if ((error = ext_load(vw, dsid, &el)) != 0) {
		return error;
	}
	for (uint32_t i = 0; i < el.n && error == 0; i++) {
		if (el.e[i].pblk != 0 && (error = pext_set(vw, el.e[i].pblk, 0, dsid)) == 0) {
			error = blocks_free(vw, el.e[i].pblk, el.e[i].len / bs);
		}
	}
	ext_fini(&el);
	if (error) {
		return error;
	}
	for (int pass = 0; pass < 2; pass++) {
		uint64_t which = pass == 0 ? in->id : dsid;

		if (pass == 1 && dsid == in->id) {
			break;
		}
		memset(&kl, 0, sizeof(kl));
		kl.id = which;
		f_put64(k, jkey(which, 0));
		(void)apfs_bt_walk(&vw->fs, k, 8, keys_cb, &kl);
		error = kl.error;
		for (uint32_t i = 0; i < kl.n; i++) {
			if (error == 0) {
				error = apfs_bt_del(&vw->fs, kl.r[i].k, kl.r[i].klen);
			}
			apfs_free(kl.r[i].k, kl.r[i].klen);
		}
		if (kl.r != NULL) {
			apfs_free(kl.r, kl.max * sizeof(*kl.r));
		}
		if (error) {
			return error;
		}
	}
	return 0;
}

static void
count(struct apfs_vw *vw, uint16_t mode, int delta)
{
	uint32_t f;

	switch (mode & S_IFMT_) {
	case S_IFDIR_:
		f = VO(apfs_num_directories);
		break;
	case S_IFREG_:
		f = VO(apfs_num_files);
		break;
	case S_IFLNK_:
		f = VO(apfs_num_symlinks);
		break;
	default:
		f = VO(apfs_num_other_fsobjects);
		break;
	}
	f_put64(vw->vsb + f, f_le64(vw->vsb + f) + (uint64_t)(int64_t)delta);
}

static int
parent_adjust(struct apfs_vw *vw, uint64_t parent, int delta, uint64_t now)
{
	struct inode p;
	int error;

	if ((error = ino_load(vw, parent, &p)) != 0) {
		return error;
	}
	f_put32(p.v + INO_NCHILDREN, (uint32_t)((int32_t)f_le32(p.v + INO_NCHILDREN) + delta));
	touch(&p, now, true);
	return ino_store(vw, &p);
}

int
apfs_fsw_remove(struct apfs_vw *vw, uint64_t parent, const char *name, size_t name_len, bool dir, uint64_t now)
{
	uint8_t dk[12 + 256];
	uint16_t dklen;
	uint64_t id;
	struct inode in;
	bool is_dir;
	int error;

	if ((error = apfs_fsw_lookup(vw, parent, name, name_len, &id, NULL)) != 0 ||
	    (error = ino_load(vw, id, &in)) != 0) {
		return error;
	}
	is_dir = (f_le16(in.v + INO_MODE) & S_IFMT_) == S_IFDIR_;
	if (dir != is_dir) {
		return dir ? ENOTDIR : EISDIR;
	}
	if (is_dir && f_le32(in.v + INO_NCHILDREN) != 0) {
		return ENOTEMPTY;
	}
	if ((error = drec_key(vw, parent, name, name_len, dk, &dklen)) != 0 ||
	    (error = apfs_bt_del(&vw->fs, dk, dklen)) != 0 ||
	    (error = parent_adjust(vw, parent, -1, now)) != 0) {
		return error;
	}
	if (!is_dir && f_le32(in.v + INO_NCHILDREN) > 1) {
		/* another name is left (a hard link): not made by this driver, not undone either */
		return ENOTSUP;
	}
	if ((error = destroy(vw, &in)) != 0) {
		return error;
	}
	count(vw, f_le16(in.v + INO_MODE), -1);
	f_put64(vw->vsb + VO(apfs_last_mod_time), now);
	return 0;
}

int
apfs_fsw_rename(struct apfs_vw *vw, uint64_t sparent, const char *sname, size_t slen,
    uint64_t dparent, const char *dname, size_t dlen, uint64_t now)
{
	uint8_t sk[12 + 256], dk[12 + 256], dv[64];
	uint16_t sklen, dklen, dvlen = sizeof(dv);
	uint64_t id, old;
	uint8_t odt;
	struct inode in;
	struct xfield *x;
	int error;

	if ((error = drec_key(vw, sparent, sname, slen, sk, &sklen)) != 0 ||
	    (error = drec_key(vw, dparent, dname, dlen, dk, &dklen)) != 0 ||
	    (error = apfs_bt_get(&vw->fs, sk, sklen, dv, &dvlen)) != 0) {
		return error;
	}
	id = f_le64(dv);
	if ((error = ino_load(vw, id, &in)) != 0) {
		return error;
	}
	/* what the new name names now goes first */
	if (apfs_fsw_lookup(vw, dparent, dname, dlen, &old, &odt) == 0) {
		if (old == id) {
			return 0;
		}
		if ((error = apfs_fsw_remove(vw, dparent, dname, dlen, odt == 4 /* DT_DIR */, now)) != 0) {
			return error;
		}
	}
	if ((error = apfs_bt_del(&vw->fs, sk, sklen)) != 0 || (error = apfs_bt_put(&vw->fs, dk, dklen, dv, dvlen)) != 0) {
		return error;
	}
	if (sparent != dparent && ((error = parent_adjust(vw, sparent, -1, now)) != 0 ||
	    (error = parent_adjust(vw, dparent, 1, now)) != 0)) {
		return error;
	}
	if (sparent == dparent && (error = parent_adjust(vw, sparent, 0, now)) != 0) {
		return error;
	}
	/* the inode: its parent and its name */
	f_put64(in.v + INO_PARENT, dparent);
	if ((x = xf_add(&in, INO_EXT_TYPE_NAME, XF_DO_NOT_COPY, (uint16_t)(dlen + 1))) == NULL) {
		return E2BIG;
	}
	memcpy(x->data, dname, dlen);
	x->data[dlen] = '\0';
	touch(&in, now, false);
	return ino_store(vw, &in);
}

/* ------------------------------------------------------------------ attributes */

int
apfs_fsw_setattr(struct apfs_vw *vw, uint64_t id, const struct apfs_fsw_set *s)
{
	struct inode in;
	int error;

	if ((error = ino_load(vw, id, &in)) != 0) {
		return error;
	}
	if (s->which & APFS_FSW_MODE) {
		f_put16(in.v + INO_MODE, (uint16_t)((f_le16(in.v + INO_MODE) & S_IFMT_) | (s->mode & 07777)));
	}
	if (s->which & APFS_FSW_UID) {
		f_put32(in.v + INO_OWNER, s->uid);
	}
	if (s->which & APFS_FSW_GID) {
		f_put32(in.v + INO_GROUP, s->gid);
	}
	if (s->which & APFS_FSW_FLAGS) {
		f_put32(in.v + INO_BSDFLAGS, s->flags);
	}
	if (s->which & APFS_FSW_MTIME) {
		f_put64(in.v + INO_MOD, s->mtime);
	}
	if (s->which & APFS_FSW_ATIME) {
		f_put64(in.v + INO_ACCESS, s->atime);
	}
	if (s->which & APFS_FSW_CRTIME) {
		f_put64(in.v + INO_CREATE, s->crtime);
	}
	touch(&in, s->now, false);
	return ino_store(vw, &in);
}

static int
xattr_key(uint64_t id, const char *name, uint8_t *k, uint16_t *klen)
{
	size_t n = strlen(name);

	if (n == 0 || n > 127) {
		return EINVAL;
	}
	f_put64(k, jkey(id, T_XATTR));
	f_put16(k + 8, (uint16_t)(n + 1));
	memcpy(k + 10, name, n + 1);
	*klen = (uint16_t)(10 + n + 1);
	return 0;
}

static int
xattr_put(struct apfs_vw *vw, uint64_t id, const char *name, const void *val, size_t len, uint16_t flags,
    bool create, bool replace)
{
	uint8_t k[10 + 128], *v;
	uint16_t klen;
	bool exists;
	int error;

	if (len > APFS_FSW_XATTR_MAX) {
		return E2BIG;
	}
	if ((error = xattr_key(id, name, k, &klen)) != 0) {
		return error;
	}
	exists = apfs_bt_get(&vw->fs, k, klen, NULL, NULL) == 0;
	if ((create && exists) || (replace && !exists)) {
		return create ? EEXIST : ENOATTR_;
	}
	v = apfs_alloc(4 + len);
	if (v == NULL) {
		return ENOMEM;
	}
	f_put16(v, flags);
	f_put16(v + 2, (uint16_t)len);
	memcpy(v + 4, val, len);
	error = apfs_bt_put(&vw->fs, k, klen, v, (uint16_t)(4 + len));
	apfs_free(v, 4 + len);
	return error;
}

int
apfs_fsw_setxattr(struct apfs_vw *vw, uint64_t id, const char *name, const void *val, size_t len, bool create, bool replace)
{
	if (strcmp(name, SYMLINK_EA) == 0) {
		return EPERM;
	}
	return xattr_put(vw, id, name, val, len, XATTR_EMBEDDED, create, replace);
}

int
apfs_fsw_rmxattr(struct apfs_vw *vw, uint64_t id, const char *name)
{
	uint8_t k[10 + 128];
	uint16_t klen;
	int error;

	if (strcmp(name, SYMLINK_EA) == 0) {
		return EPERM;
	}
	if ((error = xattr_key(id, name, k, &klen)) != 0) {
		return error;
	}
	error = apfs_bt_del(&vw->fs, k, klen);
	return error == ENOENT ? ENOATTR_ : error;
}

int
apfs_fsw_symlink(struct apfs_vw *vw, uint64_t parent, const char *name, size_t name_len,
    const char *target, const struct apfs_fsw_attr *a, uint64_t *id)
{
	struct apfs_fsw_attr la = *a;
	size_t tl = strlen(target);
	int error;

	if (tl == 0 || tl >= 1024) {
		return ENAMETOOLONG;
	}
	la.mode = (uint16_t)(S_IFLNK_ | (a->mode & 07777));
	if ((error = apfs_fsw_create(vw, parent, name, name_len, &la, id)) != 0) {
		return error;
	}
	return xattr_put(vw, *id, SYMLINK_EA, target, tl + 1, XATTR_EMBEDDED | XATTR_FS_OWNED, true, false);
}
