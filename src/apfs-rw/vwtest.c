/* host: a volume written through apfs_vw.  vwtest IMAGE MODE [N] */
#include "src/apfs_vw.h"
#include <fcntl.h>
#include <unistd.h>
static int fd;
static int rd(void *c, uint64_t off, void *buf, size_t len) { return pread(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int wr(void *c, uint64_t off, const void *buf, size_t len) { return pwrite(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int sy(void *c) { return fsync(fd) ? EIO : 0; }
int main(int argc, char **argv) {
	struct apfs_nx nx = {0}; struct apfs_vw vw; int e;
	const char *mode = argv[2]; int n = argc > 3 ? atoi(argv[3]) : 1;
	fd = open(argv[1], O_RDWR);
	struct apfs_dev dev = { .read = rd, .read_data = rd, .write = wr, .sync = sy };
	if ((e = apfs_nx_open(&nx, &dev)) || (e = apfs_vw_open(&nx, 0, &vw))) { printf("open %d\n", e); return 1; }
	printf("fs tree: %llu keys, %llu nodes; vol omap %llu keys; next oid %llu\n", (unsigned long long)vw.fs.key_count,
	    (unsigned long long)vw.fs.node_count, (unsigned long long)vw.vmap.key_count, (unsigned long long)vw.next_oid);
	if (!strcmp(mode, "dirty")) {          /* rewrite the fs root node with nothing changed */
		struct apfs_bt_node *r; uint8_t k[8]; uint16_t vl = 0;
		memset(k, 0, 8); apfs_bt_get(&vw.fs, k, 8, NULL, &vl);
		for (r = vw.fs.nodes; r; r = r->next) r->dirty = true;
	}
	for (int i = 0; i < n; i++) if ((e = apfs_vw_commit(&vw))) { printf("commit %d\n", e); return 1; }
	printf("committed xid %llu\n", (unsigned long long)nx.xid);
	return 0;
}
