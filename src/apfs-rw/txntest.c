/* host: null transactions on an image, for fsck_apfs to judge */
#include "src/apfs_txn.h"
#include <fcntl.h>
#include <unistd.h>
static int fd;
static int rd(void *c, uint64_t off, void *buf, size_t len) { return pread(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int wr(void *c, uint64_t off, const void *buf, size_t len) { return pwrite(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int sy(void *c) { return fsync(fd) ? EIO : 0; }
int main(int argc, char **argv) {
	struct apfs_nx nx = {0}; struct apfs_txn txn;
	int n = argc > 2 ? atoi(argv[2]) : 1, e;
	fd = open(argv[1], O_RDWR);
	struct apfs_dev dev = { .read = rd, .read_data = rd, .write = wr, .sync = sy };
	if ((e = apfs_nx_open(&nx, &dev)) != 0) { printf("nx_open %d\n", e); return 1; }
	if ((e = apfs_txn_load(&nx, &txn)) != 0) { printf("load %d\n", e); return 1; }
	printf("xid %llu, %u ephemeral objects\n", (unsigned long long)nx.xid, txn.count);
	for (int i = 0; i < n; i++) if ((e = apfs_txn_commit(&txn)) != 0) { printf("commit %d: %d\n", i, e); return 1; }
	printf("now xid %llu\n", (unsigned long long)nx.xid);
	apfs_txn_fini(&txn); apfs_nx_close(&nx);
	struct apfs_nx nx2 = {0};
	e = apfs_nx_open(&nx2, &dev); printf("reopen %d xid %llu\n", e, (unsigned long long)nx2.xid);
	return 0;
}
