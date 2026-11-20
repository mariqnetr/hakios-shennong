/* host: every operation, then the image is compared with what it should hold.  opstest IMAGE OUTDIR */
#include "src/apfs_fsw.h"
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
static int fd;
static int rd(void *c, uint64_t off, void *buf, size_t len) { return pread(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int wr(void *c, uint64_t off, const void *buf, size_t len) { return pwrite(fd, buf, len, (off_t)off) == (ssize_t)len ? 0 : EIO; }
static int sy(void *c) { return fsync(fd) ? EIO : 0; }
#define T(x) do { int _e = (x); if (_e) { printf("FAIL %s: %d (line %d)\n", #x, _e, __LINE__); return 1; } } while (0)
static uint8_t f1[300000]; static size_t f1len;
static void mirror(size_t off, const void *b, size_t n) { memcpy(f1 + off, b, n); if (off + n > f1len) f1len = off + n; }
int main(int argc, char **argv) {
	struct apfs_nx nx = {0}; struct apfs_vw vw;
	fd = open(argv[1], O_RDWR);
	struct apfs_dev dev = { .read = rd, .read_data = rd, .write = wr, .sync = sy };
	T(apfs_nx_open(&nx, &dev)); T(apfs_vw_open(&nx, 0, &vw));
	struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec, a, b, c, d, l, x;
	struct apfs_fsw_attr fa = { .mode = 0100644, .uid = 501, .gid = 20, .now = now }, da = fa; da.mode = 040755;
	uint8_t buf[120000];
	for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)('A' + i % 26);
	T(apfs_fsw_create(&vw, 2, "f1", 2, &fa, &a));
	T(apfs_fsw_write(&vw, a, 0, buf, 10000, now)); mirror(0, buf, 10000);
	memset(buf, 'z', 5000); T(apfs_fsw_write(&vw, a, 3000, buf, 5000, now)); mirror(3000, buf, 5000);
	for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)('a' + i % 23);
	T(apfs_vw_commit(&vw));
	T(apfs_fsw_write(&vw, a, 10000, buf, 100000, now)); mirror(10000, buf, 100000);
	T(apfs_fsw_truncate(&vw, a, 70001, now)); f1len = 70001;
	T(apfs_fsw_write(&vw, a, 70001, "TAIL", 4, now)); mirror(70001, "TAIL", 4);
	T(apfs_fsw_create(&vw, 2, "f2", 2, &fa, &b));
	T(apfs_fsw_write(&vw, b, 50000, "after a hole", 12, now));
	T(apfs_fsw_setxattr(&vw, a, "user.color", "green", 5, false, false));
	T(apfs_fsw_setxattr(&vw, a, "user.gone", "x", 1, false, false));
	T(apfs_fsw_rmxattr(&vw, a, "user.gone"));
	T(apfs_fsw_symlink(&vw, 2, "lnk", 3, "f1", &fa, &l));
	T(apfs_fsw_create(&vw, 2, "d", 1, &da, &d));
	T(apfs_vw_commit(&vw));
	T(apfs_fsw_rename(&vw, 2, "f2", 2, d, "g", 1, now));
	T(apfs_fsw_create(&vw, 2, "tmp", 3, &fa, &c));
	T(apfs_fsw_write(&vw, c, 0, buf, 50000, now));
	T(apfs_fsw_rename(&vw, 2, "tmp", 3, 2, "f3", 2, now));
	T(apfs_fsw_create(&vw, 2, "gone", 4, &fa, &x)); T(apfs_fsw_write(&vw, x, 0, buf, 9000, now));
	T(apfs_fsw_remove(&vw, 2, "gone", 4, false, now));
	T(apfs_fsw_create(&vw, d, "sub", 3, &da, &x)); T(apfs_fsw_remove(&vw, d, "sub", 3, true, now));
	struct apfs_fsw_set s = { .which = APFS_FSW_MODE, .mode = 0600, .now = now }; T(apfs_fsw_setattr(&vw, a, &s));
	T(apfs_vw_commit(&vw));
	char p[256]; snprintf(p, sizeof p, "%s/f1.expected", argv[2]); FILE *o = fopen(p, "w"); fwrite(f1, 1, f1len, o); fclose(o);
	printf("ok: xid %llu\n", (unsigned long long)nx.xid);
	return 0;
}
