/* host: create files.  mktest IMAGE COUNT PER_COMMIT [DIRS] */
#include "src/apfs_fsw.h"
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
static int fd;
static int rd(void *c, uint64_t off, void *buf, size_t len) { return pread(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int wr(void *c, uint64_t off, const void *buf, size_t len) { return pwrite(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int sy(void *c) { return fsync(fd) ? EIO : 0; }
int main(int argc, char **argv) {
	struct apfs_nx nx = {0}; struct apfs_vw vw; int e; uint32_t h;
	int n = atoi(argv[2]), per = atoi(argv[3]), dirs = argc > 4 ? atoi(argv[4]) : 0;
	apfs_name_hash("empty", 5, true, &h); printf("hash(empty) word %08x (Apple: 2f5d1c06)\n", h << 10 | 6);
	fd = open(argv[1], O_RDWR);
	struct apfs_dev dev = { .read = rd, .read_data = rd, .write = wr, .sync = sy };
	if ((e = apfs_nx_open(&nx, &dev)) || (e = apfs_vw_open(&nx, 0, &vw))) { printf("open %d\n", e); return 1; }
	struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	struct apfs_fsw_attr fa = { .mode = 0100644, .uid = 501, .gid = 20, .now = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec };
	struct apfs_fsw_attr da = fa; da.mode = 040755;
	uint64_t parent = 2, id;
	for (int d = 0; d < dirs; d++) { char nm[32]; snprintf(nm, sizeof nm, "Dir %d", d); if ((e = apfs_fsw_create(&vw, 2, nm, strlen(nm), &da, &id))) { printf("mkdir %d\n", e); return 1; } parent = id; }
	for (int i = 0; i < n; i++) {
		char nm[64]; snprintf(nm, sizeof nm, "file-%05d.txt", i);
		if ((e = apfs_fsw_create(&vw, i % 2 ? parent : 2, nm, strlen(nm), &fa, &id))) { printf("create %d: %d\n", i, e); return 1; }
		if ((i + 1) % per == 0 && (e = apfs_vw_commit(&vw))) { printf("commit %d\n", e); return 1; }
	}
	if ((e = apfs_vw_commit(&vw))) { printf("commit %d\n", e); return 1; }
	printf("fs tree %llu keys %llu nodes, vol omap %llu keys %llu nodes, xid %llu\n", (unsigned long long)vw.fs.key_count, (unsigned long long)vw.fs.node_count, (unsigned long long)vw.vmap.key_count, (unsigned long long)vw.vmap.node_count, (unsigned long long)nx.xid);
	return 0;
}
