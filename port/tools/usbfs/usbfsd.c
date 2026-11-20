/*
 * usbfsd: the phone's files for the Mac, over the USB cable.
 *
 * Runs on HackiOS (build/boot-bin: started at boot by rc.jobs, as root). Reads requests from
 * /dev/hostpipe (the kernel's end of the loader's pipe, bsd/dev/redwood_ufs.c), does them with
 * plain POSIX calls on the whole filesystem, and writes the answers back. The Mac's end is
 * usbfs (usbfs/bridge): it serves them to macOS as an NFS volume.
 *
 * Frames, both ways, little-endian: u32 magic "UFS1", u32 id, u16 op, u16 flags, u32 length, then
 * length bytes. An answer has op | 0x8000 and its payload begins with a u32 errno (0: done).
 * Strings are u32 length + bytes. A stream that loses its place looks for the next magic.
 *
 *   xcrun -sdk iphoneos clang -target arm64-apple-ios16.0 -O2 -o usbfsd usbfsd.c
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

#define MAGIC 0x31534655u       /* "UFS1" */
#define MAX_PAYLOAD (4u << 20)
#define MAX_IO (1u << 20)

enum {
	OP_PING = 1, OP_STAT, OP_READDIR, OP_READ, OP_WRITE, OP_CREATE, OP_MKDIR, OP_REMOVE, OP_RENAME,
	OP_TRUNCATE, OP_CHMOD, OP_UTIMES, OP_SYMLINK, OP_READLINK, OP_STATFS, OP_CHOWN, OP_LINK,
};

static int pipe_fd = -1, out_fd = -1;     /* (the same fd but for "-": stdin and stdout, for tests) */

/* ---- the stream ---- */

static int
read_all(void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len) {
		ssize_t n = read(pipe_fd, p, len);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int
write_all(const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len) {
		ssize_t n = write(out_fd, p, len);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* ---- building an answer ---- */

static uint8_t *out;
static size_t out_len, out_cap;

static void reserve(size_t more);

static void
put(const void *p, size_t n)
{
	reserve(n);
	memcpy(out + out_len, p, n);
	out_len += n;
}
static void put32(uint32_t v) { put(&v, 4); }
static void put64(uint64_t v) { put(&v, 8); }
static void puts_(const char *s) { uint32_t n = (uint32_t)strlen(s); put32(n); put(s, n); }

static void
put_stat(const struct stat *st)
{
	put32(st->st_mode);
	put32(st->st_nlink);
	put32(st->st_uid);
	put32(st->st_gid);
	put64((uint64_t)st->st_size);
	put64(st->st_ino);
	put64((uint64_t)st->st_atimespec.tv_sec);
	put32((uint32_t)st->st_atimespec.tv_nsec);
	put64((uint64_t)st->st_mtimespec.tv_sec);
	put32((uint32_t)st->st_mtimespec.tv_nsec);
	put64((uint64_t)st->st_ctimespec.tv_sec);
	put32((uint32_t)st->st_ctimespec.tv_nsec);
	put64((uint64_t)st->st_birthtimespec.tv_sec);
}

/* ---- reading a request ---- */

struct in {
	const uint8_t *p;
	size_t left;
	int bad;
};

static uint32_t
get32(struct in *in)
{
	uint32_t v = 0;
	if (in->left < 4) {
		in->bad = 1;
		return 0;
	}
	memcpy(&v, in->p, 4);
	in->p += 4;
	in->left -= 4;
	return v;
}

static uint64_t
get64(struct in *in)
{
	uint64_t v = 0;
	if (in->left < 8) {
		in->bad = 1;
		return 0;
	}
	memcpy(&v, in->p, 8);
	in->p += 8;
	in->left -= 8;
	return v;
}

/* a string as a path: copied out with its terminator; NULL when it is not one */
static char *
getstr(struct in *in)
{
	uint32_t n = get32(in);
	char *s;
	if (in->bad || n > in->left || n >= 4096 || memchr(in->p, 0, n) != NULL) {
		in->bad = 1;
		return NULL;
	}
	s = malloc(n + 1);
	memcpy(s, in->p, n);
	s[n] = 0;
	in->p += n;
	in->left -= n;
	return s;
}

/* ---- the operations: each leaves its answer after the errno in out ---- */

static int
op_stat_of(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) {
		return errno;
	}
	put_stat(&st);
	return 0;
}

static int
op_readdir(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	size_t count_at;
	uint32_t count = 0;
	int dfd;

	if (d == NULL) {
		return errno;
	}
	dfd = dirfd(d);
	count_at = out_len;
	put32(0);
	while ((e = readdir(d)) != NULL) {
		struct stat st;
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
			continue;       /* went away meanwhile */
		}
		puts_(e->d_name);
		put_stat(&st);
		count++;
	}
	closedir(d);
	memcpy(out + count_at, &count, 4);
	return 0;
}

static void
reserve(size_t more)
{
	if (out_len + more > out_cap) {
		size_t cap = out_cap ? out_cap : 65536;
		while (cap < out_len + more) {
			cap *= 2;
		}
		out = realloc(out, cap);
		if (out == NULL) {
			abort();
		}
		out_cap = cap;
	}
}

static int
op_read(const char *path, uint64_t off, uint32_t len)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	ssize_t n;
	uint32_t got;

	if (fd < 0) {
		return errno;
	}
	if (len > MAX_IO) {
		len = MAX_IO;
	}
	reserve(4 + (size_t)len);
	n = pread(fd, out + out_len + 4, len, (off_t)off);
	close(fd);
	if (n < 0) {
		return errno;
	}
	got = (uint32_t)n;
	memcpy(out + out_len, &got, 4);
	out_len += 4 + (size_t)n;
	return 0;
}

static int
op_write(const char *path, uint64_t off, struct in *in)
{
	uint32_t len = get32(in);
	int fd;
	ssize_t n;

	if (in->bad || len > in->left) {
		return EINVAL;
	}
	fd = open(path, O_WRONLY | O_NOFOLLOW);
	if (fd < 0) {
		return errno;
	}
	n = pwrite(fd, in->p, len, (off_t)off);
	if (n < 0) {
		int e = errno;
		close(fd);
		return e;
	}
	close(fd);
	put32((uint32_t)n);
	return 0;
}

static void
serve(uint32_t id, uint16_t op, const uint8_t *payload, uint32_t len)
{
	struct in in = { payload, len, 0 };
	char *a = NULL, *b = NULL;
	int err = 0;
	uint32_t header[4];

	out_len = 0;
	put(header, sizeof(header));            /* filled in below */
	put32(0);                               /* errno, likewise */
	switch (op) {
	case OP_PING:
		puts_("usbfsd 1");
		break;
	case OP_STAT:
		a = getstr(&in);
		err = in.bad ? EINVAL : op_stat_of(a);
		break;
	case OP_READDIR:
		a = getstr(&in);
		err = in.bad ? EINVAL : op_readdir(a);
		break;
	case OP_READ: {
		a = getstr(&in);
		uint64_t off = get64(&in);
		uint32_t n = get32(&in);
		err = in.bad ? EINVAL : op_read(a, off, n);
		break;
	}
	case OP_WRITE: {
		a = getstr(&in);
		uint64_t off = get64(&in);
		err = in.bad ? EINVAL : op_write(a, off, &in);
		break;
	}
	case OP_CREATE: {
		a = getstr(&in);
		uint32_t mode = get32(&in), excl = get32(&in);
		if (in.bad) {
			err = EINVAL;
			break;
		}
		int fd = open(a, O_CREAT | O_WRONLY | O_NOFOLLOW | (excl ? O_EXCL : 0), mode & 07777);
		if (fd < 0) {
			err = errno;
			break;
		}
		close(fd);
		err = op_stat_of(a);
		break;
	}
	case OP_MKDIR: {
		a = getstr(&in);
		uint32_t mode = get32(&in);
		err = in.bad ? EINVAL : mkdir(a, mode & 07777) != 0 ? errno : op_stat_of(a);
		break;
	}
	case OP_REMOVE: {
		struct stat st;
		a = getstr(&in);
		if (in.bad) {
			err = EINVAL;
		} else if (lstat(a, &st) != 0) {
			err = errno;
		} else if (S_ISDIR(st.st_mode)) {
			err = rmdir(a) != 0 ? errno : 0;
		} else {
			err = unlink(a) != 0 ? errno : 0;
		}
		break;
	}
	case OP_RENAME:
		a = getstr(&in);
		b = getstr(&in);
		err = in.bad ? EINVAL : rename(a, b) != 0 ? errno : 0;
		break;
	case OP_TRUNCATE: {
		a = getstr(&in);
		uint64_t size = get64(&in);
		err = in.bad ? EINVAL : truncate(a, (off_t)size) != 0 ? errno : 0;
		break;
	}
	case OP_CHMOD: {
		a = getstr(&in);
		uint32_t mode = get32(&in);
		err = in.bad ? EINVAL : lchmod(a, mode & 07777) != 0 ? errno : 0;
		break;
	}
	case OP_CHOWN: {
		a = getstr(&in);
		uint32_t uid = get32(&in), gid = get32(&in);
		err = in.bad ? EINVAL : lchown(a, uid, gid) != 0 ? errno : 0;
		break;
	}
	case OP_UTIMES: {
		a = getstr(&in);
		struct timespec ts[2];
		ts[0].tv_sec = (time_t)get64(&in);
		ts[0].tv_nsec = get32(&in);
		ts[1].tv_sec = (time_t)get64(&in);
		ts[1].tv_nsec = get32(&in);
		err = in.bad ? EINVAL : utimensat(AT_FDCWD, a, ts, AT_SYMLINK_NOFOLLOW) != 0 ? errno : 0;
		break;
	}
	case OP_SYMLINK:
		a = getstr(&in);        /* target */
		b = getstr(&in);        /* the link */
		err = in.bad ? EINVAL : symlink(a, b) != 0 ? errno : op_stat_of(b);
		break;
	case OP_LINK:
		a = getstr(&in);
		b = getstr(&in);
		err = in.bad ? EINVAL : link(a, b) != 0 ? errno : 0;
		break;
	case OP_READLINK: {
		char buf[4096];
		ssize_t n;
		a = getstr(&in);
		if (in.bad) {
			err = EINVAL;
		} else if ((n = readlink(a, buf, sizeof(buf) - 1)) < 0) {
			err = errno;
		} else {
			buf[n] = 0;
			puts_(buf);
		}
		break;
	}
	case OP_STATFS: {
		struct statfs sf;
		a = getstr(&in);
		if (in.bad) {
			err = EINVAL;
		} else if (statfs(a, &sf) != 0) {
			err = errno;
		} else {
			put64(sf.f_bsize);
			put64(sf.f_blocks);
			put64(sf.f_bfree);
			put64(sf.f_bavail);
			put64(sf.f_files);
			put64(sf.f_ffree);
		}
		break;
	}
	default:
		err = ENOSYS;
	}
	free(a);
	free(b);
	if (err != 0) {
		out_len = 20;           /* nothing but the errno */
	}
	header[0] = MAGIC;
	header[1] = id;
	header[2] = (uint32_t)(op | 0x8000);
	header[3] = (uint32_t)(out_len - 16);
	memcpy(out, header, 16);
	memcpy(out + 16, &(uint32_t){ (uint32_t)err }, 4);
	if (write_all(out, out_len) != 0) {
		syslog(LOG_ERR, "usbfsd: writing to /dev/hostpipe: %s", strerror(errno));
	}
}

int
main(int argc, char **argv)
{
	static uint8_t *payload;
	const char *dev = argc > 1 ? argv[1] : "/dev/hostpipe";

	if (argc <= 2 || strcmp(argv[2], "-f") != 0) {
		/* run at boot from rc.jobs, whose next line waits for this one: into the background */
		pid_t pid = fork();
		if (pid < 0) {
			return 1;
		}
		if (pid > 0) {
			return 0;
		}
		setsid();
	}
	signal(SIGPIPE, SIG_IGN);
	payload = malloc(MAX_PAYLOAD);
	for (;;) {
		uint32_t magic = 0, rest[3];

		if (pipe_fd < 0) {
			if (strcmp(dev, "-") == 0) {
				pipe_fd = 0;
				out_fd = 1;
			} else {
				pipe_fd = out_fd = open(dev, O_RDWR);
				if (pipe_fd < 0) {
					sleep(2);
					continue;
				}
			}
			fprintf(stderr, "usbfsd: serving the phone's files on %s\n", dev);
		}
		/* the next magic, a byte at a time if the stream has lost its place */
		if (read_all(&magic, 4) != 0) {
			goto lost;
		}
		while (magic != MAGIC) {
			uint8_t c;
			if (read_all(&c, 1) != 0) {
				goto lost;
			}
			magic = (magic >> 8) | ((uint32_t)c << 24);
		}
		if (read_all(rest, sizeof(rest)) != 0) {
			goto lost;
		}
		if (rest[2] > MAX_PAYLOAD) {
			continue;       /* not a frame after all: look for the next magic */
		}
		if (read_all(payload, rest[2]) != 0) {
			goto lost;
		}
		serve(rest[0], (uint16_t)(rest[1] & 0xffff), payload, rest[2]);
		continue;
lost:
		if (pipe_fd == 0) {
			return 0;       /* (a test's end) */
		}
		close(pipe_fd);
		pipe_fd = -1;
		sleep(1);
	}
}
