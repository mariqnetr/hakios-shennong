/* host: the space manager against fsck_apfs.  sptest IMAGE MODE: drain | allocfree | leak | unleak */
#include "src/apfs_space.h"
#include <fcntl.h>
#include <unistd.h>
static int fd;
static int rd(void *c, uint64_t off, void *buf, size_t len) { return pread(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int cut = -1; static int wr(void *c, uint64_t off, const void *buf, size_t len) { if (cut >= 0 && cut-- == 0) return EIO;  return pwrite(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int sy(void *c) { return fsync(fd) ? EIO : 0; }
int main(int argc, char **argv) {
	struct apfs_nx nx = {0}; struct apfs_txn txn; struct apfs_space sp; uint64_t a = 0;
	int e; const char *mode = argv[2]; if (argc > 3) cut = atoi(argv[3]);
	fd = open(argv[1], O_RDWR);
	struct apfs_dev dev = { .read = rd, .read_data = rd, .write = wr, .sync = sy };
	if ((e = apfs_nx_open(&nx, &dev)) || (e = apfs_txn_load(&nx, &txn)) || (e = apfs_space_load(&txn, &sp))) { printf("load %d\n", e); return 1; }
	printf("free %llu of %llu, %u pending from the queues\n", (unsigned long long)sp.free_count, (unsigned long long)sp.block_count, sp.nfrees);
	if (!strcmp(mode, "allocfree")) {
		for (int i = 0; i < 50; i++) { if ((e = apfs_space_alloc(&sp, 1 + i, &a))) { printf("alloc %d\n", e); return 1; } apfs_space_free(&sp, a, 1 + i); }
	} else if (!strcmp(mode, "leak")) {
		if ((e = apfs_space_alloc(&sp, 100, &a))) { printf("alloc %d\n", e); return 1; }
		printf("allocated 100 at %llu\n", (unsigned long long)a);
		FILE *f = fopen("leak.addr", "w"); fprintf(f, "%llu\n", (unsigned long long)a); fclose(f);
	} else if (!strcmp(mode, "unleak")) {
		FILE *f = fopen("leak.addr", "r"); unsigned long long x; fscanf(f, "%llu", &x); fclose(f);
		apfs_space_free(&sp, x, 100);
	} else if (!strcmp(mode, "big")) {
		if ((e = apfs_space_alloc(&sp, 40000, &a))) { printf("alloc %d\n", e); return 1; }
		apfs_space_free(&sp, a, 40000);
	}
	if ((e = apfs_space_flush(&sp)) || (e = apfs_txn_commit(&txn))) { printf("commit %d\n", e); return 1; }
	printf("committed xid %llu, free %llu\n", (unsigned long long)nx.xid, (unsigned long long)sp.free_count);
	return 0;
}
