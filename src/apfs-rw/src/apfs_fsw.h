/*
 * Writing APFS: the file system's own records (inodes, directory records).  See apfs_fsw.c.
 */

#ifndef _MISCFS_APFS_APFS_FSW_H_
#define _MISCFS_APFS_APFS_FSW_H_

#include "apfs_vw.h"

struct apfs_fsw_attr {
	uint16_t mode;                  /* type and permissions, as st_mode */
	uint32_t uid, gid;
	uint64_t now;                   /* nanoseconds since 1970 */
};

/* a new file, directory (or other) named name in directory parent: its id in *id */
int apfs_fsw_create(struct apfs_vw *vw, uint64_t parent, const char *name, size_t name_len,
    const struct apfs_fsw_attr *a, uint64_t *id);

/* the directory record for name in parent: the id and DT_ type it names */
int apfs_fsw_lookup(struct apfs_vw *vw, uint64_t parent, const char *name, size_t name_len, uint64_t *id, uint8_t *dtype);
/* len bytes at off of file id: in place where it has blocks, into new ones where it has none */
int apfs_fsw_write(struct apfs_vw *vw, uint64_t id, uint64_t off, const void *buf, size_t len, uint64_t now);
/* file id's length; what goes past it is given up */
int apfs_fsw_truncate(struct apfs_vw *vw, uint64_t id, uint64_t size, uint64_t now);
/* the name in parent goes; a file whose last name it was goes too, a directory only if empty */
int apfs_fsw_remove(struct apfs_vw *vw, uint64_t parent, const char *name, size_t name_len, bool dir, uint64_t now);
/* from (sparent, sname) to (dparent, dname); what dname named goes */
int apfs_fsw_rename(struct apfs_vw *vw, uint64_t sparent, const char *sname, size_t slen,
    uint64_t dparent, const char *dname, size_t dlen, uint64_t now);

#define APFS_FSW_MODE   0x01
#define APFS_FSW_UID    0x02
#define APFS_FSW_GID    0x04
#define APFS_FSW_MTIME  0x08
#define APFS_FSW_ATIME  0x10
#define APFS_FSW_CRTIME 0x20
#define APFS_FSW_FLAGS  0x40
struct apfs_fsw_set {
	uint32_t which;
	uint16_t mode;                  /* permissions only: the type stays */
	uint32_t uid, gid, flags;
	uint64_t mtime, atime, crtime, now;
};
int apfs_fsw_setattr(struct apfs_vw *vw, uint64_t id, const struct apfs_fsw_set *s);
/* an extended attribute, kept in its record (up to APFS_FSW_XATTR_MAX bytes) */
#define APFS_FSW_XATTR_MAX 3804
int apfs_fsw_setxattr(struct apfs_vw *vw, uint64_t id, const char *name, const void *val, size_t len, bool create, bool replace);
int apfs_fsw_rmxattr(struct apfs_vw *vw, uint64_t id, const char *name);
/* a symbolic link to target */
int apfs_fsw_symlink(struct apfs_vw *vw, uint64_t parent, const char *name, size_t name_len,
    const char *target, const struct apfs_fsw_attr *a, uint64_t *id);

#endif /* _MISCFS_APFS_APFS_FSW_H_ */
