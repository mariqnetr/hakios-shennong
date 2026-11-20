/*
 * Writing APFS: a volume, from its file-system tree up to the container superblock.  See apfs_vw.c.
 */

#ifndef _MISCFS_APFS_APFS_VW_H_
#define _MISCFS_APFS_APFS_VW_H_

#include "apfs_bt.h"

struct apfs_vw {
	struct apfs_nx    *nx;
	struct apfs_txn    txn;
	struct apfs_space  sp;
	struct apfs_bt     cmap;        /* the container's object map */
	struct apfs_bt     vmap;        /* the volume's object map */
	struct apfs_bt     fs;          /* the volume's file-system tree */
	struct apfs_bt     er;          /* its extent-reference tree (physical) */
	int64_t            data_delta;  /* blocks of file contents taken less given up, since the last commit */
	uint64_t           next_oid;    /* nx_next_oid */
	uint64_t           vol_oid;
	uint64_t           vsb_addr;    /* where the volume superblock is now */
	uint64_t           comap_addr;  /* the container's omap_phys */
	uint64_t           vomap_addr;  /* the volume's omap_phys */
	uint8_t           *vsb;         /* the volume superblock, changed in place until the commit */
	bool               open;
};

/* the first volume of the container nx opened (read only so far) for writing */
int apfs_vw_open(struct apfs_nx *nx, uint32_t index, struct apfs_vw *vw);
/* every change so far, in one checkpoint */
int apfs_vw_commit(struct apfs_vw *vw);
void apfs_vw_close(struct apfs_vw *vw);

/* how the records of a file-system tree sort (j_key and what follows it) */
int apfs_bt_cmp_fs(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen);

#endif /* _MISCFS_APFS_APFS_VW_H_ */
