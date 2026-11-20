#include "src/apfs_vw.h"
#include <fcntl.h>
#include <unistd.h>
static int fd;
static int rd(void *c, uint64_t off, void *buf, size_t len) { return pread(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int cb(void *ctx, const uint8_t *k, uint16_t kl, const uint8_t *v, uint16_t vl) {
	uint64_t h; memcpy(&h, k, 8);
	printf("id %llu type %u klen %u vlen %u\n  k:", (unsigned long long)(h & 0x0fffffffffffffffULL), (unsigned)(h >> 60), kl, vl);
	for (int i = 8; i < kl; i++) printf("%02x", k[i]); printf("\n  v:");
	for (int i = 0; i < vl; i++) printf("%02x%s", v[i], (i % 8 == 7) ? " " : ""); printf("\n"); return 0; }
int main(int argc, char **argv) {
	struct apfs_nx nx = {0}; struct apfs_vw vw;
	fd = open(argv[1], O_RDONLY);
	struct apfs_dev dev = { .read = rd, .read_data = rd };
	if (apfs_nx_open(&nx, &dev) || apfs_vw_open(&nx, 0, &vw)) return 1;
	printf("vol: files %llu dirs %llu next_obj %llu alloc %llu\n", *(unsigned long long*)(vw.vsb + __builtin_offsetof(struct apfs_superblock, apfs_num_files)), *(unsigned long long*)(vw.vsb + __builtin_offsetof(struct apfs_superblock, apfs_num_directories)), *(unsigned long long*)(vw.vsb + __builtin_offsetof(struct apfs_superblock, apfs_next_obj_id)), *(unsigned long long*)(vw.vsb + __builtin_offsetof(struct apfs_superblock, apfs_fs_alloc_count)));
	return apfs_bt_walk(&vw.fs, NULL, 0, cb, NULL);
}
