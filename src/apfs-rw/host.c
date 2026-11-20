/* host harness: open a raw APFS image with apfs_lib and list a directory */
#include "src/apfs_lib.h"
#include <fcntl.h>
#include <unistd.h>
static int fd;
static int rd(void *c, uint64_t off, void *buf, size_t len) { return pread(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int cb(void *ctx, const char *name, size_t nl, uint64_t id, uint8_t type) { printf("  %.*s id=%llu type=%u\n", (int)nl, name, (unsigned long long)id, type); return 0; }
int main(int argc, char **argv) {
	struct apfs_nx nx = {0}; struct apfs_vol vol = {0};
	fd = open(argv[1], O_RDONLY);
	struct apfs_dev dev = { .ctx = NULL, .read = rd, .read_data = rd };
	int e = apfs_nx_open(&nx, &dev); printf("nx_open %d xid %llu blocks %llu fs %u\n", e, (unsigned long long)nx.xid, (unsigned long long)nx.block_count, nx.fs_count);
	if (e) return 1;
	e = apfs_vol_open(&nx, 0, &vol); printf("vol_open %d\n", e);
	if (e) return 1;
	e = apfs_readdir(&vol, 2, cb, NULL); printf("readdir root %d\n", e);
	return 0;
}
