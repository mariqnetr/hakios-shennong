#include "src/apfs_vw.h"
#include <fcntl.h>
#include <unistd.h>
static int fd;
static int rd(void *c, uint64_t off, void *buf, size_t len) { return pread(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int cb(void *ctx, const uint8_t *k, uint16_t kl, const uint8_t *v, uint16_t vl) {
	uint64_t h; memcpy(&h, k, 8); printf("er: id %llu type %u vlen %u v:", (unsigned long long)(h & 0x0fffffffffffffffULL), (unsigned)(h >> 60), vl);
	for (int i = 0; i < vl; i++) printf("%02x%s", v[i], (i % 8 == 7) ? " " : ""); printf("\n"); return 0; }
#define VO(f) __builtin_offsetof(struct apfs_superblock, f)
int main(int argc, char **argv) {
	struct apfs_nx nx = {0}; struct apfs_vw vw; struct apfs_bt er;
	fd = open(argv[1], O_RDONLY);
	struct apfs_dev dev = { .read = rd, .read_data = rd };
	if (apfs_nx_open(&nx, &dev) || apfs_vw_open(&nx, 0, &vw)) return 1;
	uint64_t o; uint32_t t; memcpy(&o, vw.vsb + VO(apfs_extentref_tree_oid), 8); memcpy(&t, vw.vsb + VO(apfs_extentref_tree_type), 4);
	printf("extentref oid %llu type %x; snap_meta %llu; incompat %llx features %llx fs_flags %llx\n", (unsigned long long)o, t,
	  *(unsigned long long*)(vw.vsb+VO(apfs_snap_meta_tree_oid)), *(unsigned long long*)(vw.vsb+VO(apfs_incompatible_features)), *(unsigned long long*)(vw.vsb+VO(apfs_features)), *(unsigned long long*)(vw.vsb+VO(apfs_fs_flags)));
	if (apfs_bt_open(&er, &vw.sp, NULL, o, apfs_bt_cmp_omap, NULL)) { printf("er open fail\n"); return 1; }
	return apfs_bt_walk(&er, NULL, 0, cb, NULL);
}
