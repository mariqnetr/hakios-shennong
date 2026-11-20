/*
 * A writable APFS volume: the vnode operations, on top of apfs_fsw.c.
 *
 * Included by apfs_vnops.c; the operations there hand a vnode of a writable mount to the ones
 * here (APFS_RW()).  A writable mount reads through the same trees it writes (apfs_bt.c), never
 * through apfs_lib's read-only view of the container -- that view is of the last checkpoint and
 * knows nothing of what is not committed yet.
 *
 * One lock round the whole volume (rw_lock): every operation takes it for all it does, so the
 * trees in memory are only ever touched by one thread.  What changes is certain to be on the flash after the
 * next commit (its device syncs are the only barriers it needs: apfs_txn_commit): a fsync, the filesystem's sync, the unmount, or at most APFS_RW_COMMIT_MS after the
 * first change since the last one.  Until then it is in the trees in memory -- and the file
 * contents themselves on the flash already, written where the new extents are (the checkpoint
 * that will name them is what is missing).
 *
 * File contents do not go through the buffer cache.  read() and write() go straight to the
 * volume; mapped pages come in (pagein) and go out (pageout) the same way, and a write() or a
 * truncation drops the pages of what it changed from the page cache, so a mapping sees it.
 */

#include <kern/thread_call.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/fsctl.h>
#include <sys/attr.h>
#include <sys/ubc.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include "apfs_fsw.h"

#define APFS_RW_COMMIT_MS  2000
#define APFS_RW_IOCHUNK    (256 * 1024)

/* Where a writable volume's time goes (boot-arg apfs_stats=1: a line every APFS_RW_STATS_MS it was busy). */
#define APFS_RW_STATS_MS 5000

struct rw_stats {
	uint64_t commits, commit_ticks, commit_max, by_timer;
	uint64_t syncs, syncs_clean;    /* asked by a fsync and the like; of them, with nothing to commit */
	uint64_t lock_waits, lock_ticks, lock_max;
	uint64_t writes, written, write_ticks, reads, read_bytes, read_ticks;
	uint64_t recycled, nowait;      /* fsyncs that are no one's asking for a commit (rw_fsync) */
};

struct apfs_rw {
	lck_mtx_t        lock;
	struct apfs_vw   vw;
	thread_call_t    tc;
	bool             dirty, armed;
	uint64_t         commits, commit_errors;
	int              failed;        /* a commit that went wrong: nothing is written any more */
	struct rw_stats  now, said;     /* boot-arg apfs_stats=1: the report; what it said last */
	uint64_t         said_at;
	struct {
		char     name[17];
		uint32_t count;
	} fsync_by[8];                  /* who asked for the fsyncs since the last line */
};

#define APFS_RW(am)  ((am)->am_rw)
#define RW_PRIVATE_DIR 3                /* the volume's private directory (newfs_apfs makes it: "private-dir") */
#define VO_NUM_FILES ((uint32_t)__builtin_offsetof(struct apfs_superblock, apfs_num_files))
#define VO_NUM_DIRS  ((uint32_t)__builtin_offsetof(struct apfs_superblock, apfs_num_directories))

static LCK_GRP_DECLARE(apfs_rw_lck_grp, "apfs_rw");

static uint64_t
rw_ms(uint64_t ticks)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(ticks, &ns);
	return ns / 1000000;
}

/* the volume's lock, and how long it was waited for */
static void
rw_lock(struct apfs_rw *rw)
{
	uint64_t started, took;

	if (lck_mtx_try_lock(&rw->lock)) {
		return;
	}
	started = mach_absolute_time();
	lck_mtx_lock(&rw->lock);
	took = mach_absolute_time() - started;
	rw->now.lock_waits++;
	rw->now.lock_ticks += took;
	if (took > rw->now.lock_max) {
		rw->now.lock_max = took;
	}
}

static bool
rw_stats_on(void)
{
	static int state;

	if (state == 0) {
		uint32_t on = 0;

		(void)PE_parse_boot_argn("apfs_stats", &on, sizeof(on));
		state = on != 0 ? 2 : 1;
	}
	return state == 2;
}

/* every APFS_RW_STATS_MS, from a commit: what happened since the last line (with the lock held) */
static void
rw_stats_say(struct apfs_rw *rw)
{
	static struct apfs_rw_io io_said;
	static uint64_t lw_said, lf_said, ln_said, lb_said;
	struct rw_stats *n = &rw->now, *o = &rw->said;
	struct apfs_rw_io io = apfs_rw_io;
	uint64_t at = mach_absolute_time(), lw = 0, lf = 0, ln = 0, lb = 0;

	if (!rw_stats_on() || (rw->said_at != 0 && rw_ms(at - rw->said_at) < APFS_RW_STATS_MS)) {
		return;
	}
	redwood_ufs_write_stats(&lw, &lf, &ln, &lb);
	/* a few at a time: one printf of too many numbers does not fit the kernel's log and panics */
	printf("apfs: stats: %llu ms: commits %llu (timer %llu) %llu ms, longest %llu; fsyncs %llu (%llu clean)\n",
	    rw_ms(at - rw->said_at), n->commits - o->commits, n->by_timer - o->by_timer,
	    rw_ms(n->commit_ticks - o->commit_ticks), rw_ms(n->commit_max),
	    n->syncs - o->syncs, n->syncs_clean - o->syncs_clean);
	printf("apfs: stats: fsyncs of a vnode going %llu, not to wait %llu; by %s %u, %s %u, %s %u, %s %u\n",
	    n->recycled - o->recycled, n->nowait - o->nowait,
	    rw->fsync_by[0].name, rw->fsync_by[0].count, rw->fsync_by[1].name, rw->fsync_by[1].count,
	    rw->fsync_by[2].name, rw->fsync_by[2].count, rw->fsync_by[3].name, rw->fsync_by[3].count);
	printf("apfs: stats: and by %s %u, %s %u, %s %u, %s %u\n",
	    rw->fsync_by[4].name, rw->fsync_by[4].count, rw->fsync_by[5].name, rw->fsync_by[5].count,
	    rw->fsync_by[6].name, rw->fsync_by[6].count, rw->fsync_by[7].name, rw->fsync_by[7].count);
	bzero(rw->fsync_by, sizeof(rw->fsync_by));
	printf("apfs: stats: write %llu, %llu KB, %llu ms; read %llu, %llu KB, %llu ms\n",
	    n->writes - o->writes, (n->written - o->written) / 1024, rw_ms(n->write_ticks - o->write_ticks),
	    n->reads - o->reads, (n->read_bytes - o->read_bytes) / 1024, rw_ms(n->read_ticks - o->read_ticks));
	printf("apfs: stats: lock waits %llu, %llu ms, longest %llu; device writes %llu, %llu KB, %llu ms; syncs %llu, %llu ms\n",
	    n->lock_waits - o->lock_waits, rw_ms(n->lock_ticks - o->lock_ticks), rw_ms(n->lock_max),
	    io.writes - io_said.writes, (io.bytes - io_said.bytes) / 1024, rw_ms(io.ticks - io_said.ticks),
	    io.syncs - io_said.syncs, rw_ms(io.sync_ticks - io_said.sync_ticks));
	printf("apfs: stats: loader writing %llu ms, flushing %llu ms (%llu), reading back %llu ms\n",
	    rw_ms(lw - lw_said), rw_ms(lf - lf_said), ln - ln_said, rw_ms(lb - lb_said));
	n->commit_max = 0;
	n->lock_max = 0;
	*o = *n;
	io_said = io;
	lw_said = lw;
	lf_said = lf;
	ln_said = ln;
	lb_said = lb;
	rw->said_at = at;
}

static uint64_t
rw_vsb64(struct apfs_rw *rw, uint32_t off)
{
	uint64_t v;

	memcpy(&v, rw->vw.vsb + off, sizeof(v));
	return v;
}

/* the same buckets as apfs_vfsops.c's apfs_hash_index: the two share am_hash */
static uint32_t
rw_hash(uint64_t id)
{
	return (uint32_t)((id ^ (id >> 17)) % APFS_HASH_BUCKETS);
}

static uint64_t
rw_now(void)
{
	clock_sec_t s;
	clock_usec_t us;

	clock_get_calendar_microtime(&s, &us);
	return (uint64_t)s * 1000000000ull + (uint64_t)us * 1000ull;
}

static void rw_commit_locked(struct apfs_rw *rw);

/*
 * Names to watch, said with every operation on them (bring-up: containermanagerd loses the
 * metadata of the containers it makes, and we want to see where).
 */
/* processes whose every name operation on this volume is said (bring-up: Files, the storage figures) */
bool
apfs_rw_watching(void)
{
	/* boot-arg apfs_watch=1: every name operation of the processes and names below is said */
	static int state;       /* 0: not read yet, 1: no, 2: yes */

	if (state == 0) {
		uint32_t on = 0;

		(void)PE_parse_boot_argn("apfs_watch", &on, sizeof(on));
		state = on != 0 ? 2 : 1;
	}
	return state == 2;
}

static bool
rw_watched_proc(void)
{
	static const char *const procs[] = { "LocalStorageFile", "SaveToFiles", "fileproviderd", "deleted" };
	const char *me = proc_best_name(current_proc());

	for (unsigned i = 0; i < sizeof(procs) / sizeof(procs[0]); i++) {
		if (strncmp(me, procs[i], strlen(procs[i])) == 0) {
			return true;
		}
	}
	return false;
}

static bool
rw_watched(const char *name, size_t len)
{
	static const char *const pats[] = { "container_manager", "Staging", "metadata" };

	for (unsigned i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
		size_t n = strlen(pats[i]);

		for (size_t at = 0; at + n <= len; at++) {
			if (memcmp(name + at, pats[i], n) == 0) {
				return true;
			}
		}
	}
	return false;
}

static void
rw_watch(const char *op, uint64_t dir, const char *name, size_t len, const char *to, size_t tolen, int error)
{
	if (apfs_rw_watching() &&
	    (rw_watched_proc() || rw_watched(name, len) || (to != NULL && rw_watched(to, tolen)))) {
		printf("apfs: rw: watch: %s %llu/%.*s%s%.*s by %s: %d\n", op, (unsigned long long)dir, (int)len, name,
		    to != NULL ? " -> " : "", to != NULL ? (int)tolen : 0, to != NULL ? to : "",
		    proc_best_name(current_proc()), error);
	}
}

/* an operation of the volume that failed, said (bring-up): the first 400, not the expected ones */
static void
rw_said(const char *op, uint64_t id, int error)
{
	static uint32_t said;

	if (apfs_rw_watching() && error != 0 && error != EJUSTRETURN && rw_watched_proc()) {
		printf("apfs: rw: watch: %s (object %llu, by %s): %d\n", op, (unsigned long long)id, proc_best_name(current_proc()), error);
		return;
	}
	if (error == 0 || error == ENOENT || error == EJUSTRETURN || error == ENOATTR) {
		return;
	}
	if (said++ < 400) {
		printf("apfs: rw: %s (object %llu, by %s): %d\n", op, (unsigned long long)id,
		    proc_best_name(current_proc()), error);
	}
}

/* paging, said: every one that fails, and the first 32 of all (bring-up) */
static void
rw_trace(const char *what, uint64_t id, off_t off, size_t size, int flags, int error)
{
	static uint32_t said;

	if (error != 0 || (apfs_rw_watching() && said < 32)) {
		said++;
		printf("apfs: rw: %s: file %llu, %lld + %zu, flags 0x%x: %d\n", what, (unsigned long long)id,
		    (long long)off, size, flags, error);
	}
}

static void
rw_timer(thread_call_param_t p0, __unused thread_call_param_t p1)
{
	struct apfs_rw *rw = p0;

	rw_lock(rw);
	rw->armed = false;
	if (rw->dirty) {
		rw->now.by_timer++;
		rw_commit_locked(rw);
	}
	lck_mtx_unlock(&rw->lock);
}

/* something changed: it is committed within APFS_RW_COMMIT_MS */
static void
rw_dirty(struct apfs_rw *rw)
{
	rw->dirty = true;
	if (!rw->armed && rw->tc != NULL) {
		uint64_t deadline;

		clock_interval_to_deadline(APFS_RW_COMMIT_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(rw->tc, deadline);
		rw->armed = true;
	}
}

static void
rw_commit_locked(struct apfs_rw *rw)
{
	uint64_t started, took;
	int error;

	if (!rw->dirty || rw->failed) {
		return;
	}
	started = mach_absolute_time();
	error = apfs_vw_commit(&rw->vw);
	took = mach_absolute_time() - started;
	rw->now.commit_ticks += took;
	if (took > rw->now.commit_max) {
		rw->now.commit_max = took;
	}
	if (error) {
		/*
		 * The trees in memory and the space manager may be half way through; the checkpoint on
		 * the flash is the last good one.  Nothing more is written this mount.
		 */
		rw->failed = error;
		rw->commit_errors++;
		printf("apfs: rw: the commit failed (%d): the volume is read only from now on\n", error);
		return;
	}
	rw->commits++;
	rw->now.commits++;
	rw->dirty = false;
	apfs_bt_trim(&rw->vw.fs);
	apfs_bt_trim(&rw->vw.er);
	apfs_bt_trim(&rw->vw.vmap);
	apfs_bt_trim(&rw->vw.cmap);
	rw_stats_say(rw);
}

/* an operation that changes the volume: refused once a commit has failed */
static int
rw_begin_change(struct apfs_rw *rw)
{
	rw_lock(rw);
	if (rw->failed) {
		lck_mtx_unlock(&rw->lock);
		return EROFS;
	}
	return 0;
}

static void
rw_end_change(struct apfs_rw *rw, int error)
{
	if (error == 0) {
		rw_dirty(rw);
	}
	lck_mtx_unlock(&rw->lock);
}

/* ------------------------------------------------------------------ the mount */

int
apfs_rw_attach(struct apfs_mount *am)
{
	struct apfs_rw *rw = kalloc_type(struct apfs_rw, Z_WAITOK | Z_ZERO);
	int error;

	if (rw == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&rw->lock, &apfs_rw_lck_grp, LCK_ATTR_NULL);
	error = apfs_vw_open(&am->am_nx, 0, &rw->vw);
	if (error) {
		printf("apfs: rw: the volume cannot be written (%d)\n", error);
		lck_mtx_destroy(&rw->lock, &apfs_rw_lck_grp);
		kfree_type(struct apfs_rw, rw);
		return error;
	}
	{
		uint32_t old = 0;

		/* boot-arg apfs_rw_old=1: file contents a block at a time and every write flushed, for comparing */
		(void)PE_parse_boot_argn("apfs_rw_old", &old, sizeof(old));
		rw->vw.one_by_one = old != 0;
		rw->vw.txn.no_gather = old != 0;
	}
	rw->tc = thread_call_allocate_with_options(rw_timer, rw, THREAD_CALL_PRIORITY_KERNEL, 0);
	am->am_rw = rw;
	/* ACLs and owner UUIDs: VFS keeps them in an extended attribute of the file, as on ramfs */
	vfs_setextendedsecurity(am->am_mp);
	printf("apfs: rw: volume open for writing: %llu files, %llu directories, %llu of %llu blocks free\n",
	    (unsigned long long)rw_vsb64(rw, VO_NUM_FILES), (unsigned long long)rw_vsb64(rw, VO_NUM_DIRS),
	    (unsigned long long)rw->vw.sp.free_count, (unsigned long long)rw->vw.sp.block_count);
	return 0;
}

int
apfs_rw_sync(struct apfs_mount *am)
{
	struct apfs_rw *rw = APFS_RW(am);
	int error;

	rw_lock(rw);
	rw->now.syncs++;
	if (!rw->dirty) {
		rw->now.syncs_clean++;
	}
	rw_commit_locked(rw);
	error = rw->failed;
	lck_mtx_unlock(&rw->lock);
	return error;
}

void
apfs_rw_detach(struct apfs_mount *am)
{
	struct apfs_rw *rw = APFS_RW(am);

	if (rw == NULL) {
		return;
	}
	if (rw->tc != NULL) {
		thread_call_cancel_wait(rw->tc);
		thread_call_free(rw->tc);
	}
	(void)apfs_rw_sync(am);
	printf("apfs: rw: unmounted after %llu commits (%llu failed)\n", (unsigned long long)rw->commits,
	    (unsigned long long)rw->commit_errors);
	apfs_vw_close(&rw->vw);
	lck_mtx_destroy(&rw->lock, &apfs_rw_lck_grp);
	kfree_type(struct apfs_rw, rw);
	am->am_rw = NULL;
}

/* ------------------------------------------------------------------ vnodes */

static enum vtype
rw_vtype(uint16_t mode)
{
	switch (mode & APFS_S_IFMT) {
	case APFS_S_IFREG:  return VREG;
	case APFS_S_IFDIR:  return VDIR;
	case APFS_S_IFLNK:  return VLNK;
	case APFS_S_IFCHR:  return VCHR;
	case APFS_S_IFBLK:  return VBLK;
	case APFS_S_IFIFO:  return VFIFO;
	case APFS_S_IFSOCK: return VSOCK;
	default:            return VNON;
	}
}

/* the vnode for object id: the one there is, or a new one (with the volume lock held: it is let go of meanwhile) */
static int
rw_node_vnode(struct apfs_mount *am, uint64_t id, vnode_t dvp, struct componentname *cnp, vnode_t *vpp)
{
	struct apfs_rw *rw = APFS_RW(am);
	struct vnode_fsparam vfsp;
	struct apfs_fsw_stat st;
	struct apfs_node *an;
	uint32_t b = rw_hash(id);
	enum vtype vtype;
	int error;

	for (an = am->am_hash[b]; an != NULL; an = an->an_next) {
		if (an->an_id == id && an->an_attached) {
			vnode_t vp = an->an_vp;
			uint32_t vid = vnode_vid(vp);

			lck_mtx_unlock(&rw->lock);
			error = vnode_getwithvid(vp, vid);
			rw_lock(rw);
			if (error == 0) {
				*vpp = vp;
				return 0;
			}
			break;
		}
	}
	if ((error = apfs_fsw_stat(&rw->vw, id, &st)) != 0) {
		return error;
	}
	if ((vtype = rw_vtype(st.mode)) == VNON) {
		return EIO;
	}
	an = kalloc_type(struct apfs_node, Z_WAITOK | Z_ZERO);
	if (an == NULL) {
		return ENOMEM;
	}
	an->an_mount = am;
	an->an_id = id;
	an->an_parent = st.parent;
	an->an_mode = st.mode;
	an->an_size = st.size;

	bzero(&vfsp, sizeof(vfsp));
	vfsp.vnfs_mp = am->am_mp;
	vfsp.vnfs_vtype = vtype;
	vfsp.vnfs_str = "apfs";
	vfsp.vnfs_dvp = dvp;
	vfsp.vnfs_fsnode = an;
	vfsp.vnfs_vops = apfs_vnodeop_p;
	vfsp.vnfs_markroot = (id == am->am_root_id);
	vfsp.vnfs_rdev = (vtype == VCHR || vtype == VBLK) ? (dev_t)st.rdev : 0;
	vfsp.vnfs_filesize = st.size;
	vfsp.vnfs_cnp = cnp;
	vfsp.vnfs_flags = (dvp != NULL && cnp != NULL && (cnp->cn_flags & MAKEENTRY)) ? 0 : VNFS_NOCACHE;

	/* vnode_create may call back into the filesystem (reclaim of another vnode): not under our lock */
	lck_mtx_unlock(&rw->lock);
	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, &an->an_vp);
	rw_lock(rw);
	if (error) {
		kfree_type(struct apfs_node, an);
		return error;
	}
	vnode_settag(an->an_vp, VT_OTHER);
	an->an_next = am->am_hash[b];
	am->am_hash[b] = an;
	an->an_attached = true;
	*vpp = an->an_vp;
	return 0;
}

int
apfs_rw_root(struct apfs_mount *am, vnode_t *vpp)
{
	struct apfs_rw *rw = APFS_RW(am);
	int error;

	rw_lock(rw);
	error = rw_node_vnode(am, am->am_root_id, NULLVP, NULL, vpp);
	lck_mtx_unlock(&rw->lock);
	return error;
}

int
apfs_rw_vget(struct apfs_mount *am, uint64_t id, vnode_t *vpp)
{
	struct apfs_rw *rw = APFS_RW(am);
	int error;

	rw_lock(rw);
	error = rw_node_vnode(am, id, NULLVP, NULL, vpp);
	lck_mtx_unlock(&rw->lock);
	return error;
}

/* the last use of a file whose last name went while it was open: now it goes */
static void
rw_inactive(struct apfs_mount *am, struct apfs_node *an, vnode_t vp)
{
	struct apfs_rw *rw = APFS_RW(am);
	char name[32];
	int error;

	if (!an->an_orphan || rw_begin_change(rw) != 0) {
		return;
	}
	snprintf(name, sizeof(name), "orphan-%llu", (unsigned long long)an->an_id);
	error = apfs_fsw_remove(&rw->vw, RW_PRIVATE_DIR, name, strlen(name), false, rw_now());
	rw_end_change(rw, error);
	if (error) {
		printf("apfs: rw: orphan %llu: %d\n", (unsigned long long)an->an_id, error);
	}
	an->an_orphan = false;
	vnode_recycle(vp);
}

static void
rw_reclaim(struct apfs_mount *am, struct apfs_node *an)
{
	struct apfs_rw *rw = APFS_RW(am);
	uint32_t b = rw_hash(an->an_id);

	rw_lock(rw);
	for (struct apfs_node **pp = &am->am_hash[b]; *pp != NULL; pp = &(*pp)->an_next) {
		if (*pp == an) {
			*pp = an->an_next;
			break;
		}
	}
	an->an_attached = false;
	lck_mtx_unlock(&rw->lock);
}

/* ------------------------------------------------------------------ names */

static int
rw_lookup(struct vnop_lookup_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_rw *rw = APFS_RW(am);
	struct apfs_node *dn = APFS_VP(dvp);
	uint64_t id = 0;
	int error;

	*ap->a_vpp = NULLVP;
	if (!vnode_isdir(dvp)) {
		return ENOTDIR;
	}
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		error = vnode_get(dvp);
		if (error == 0) {
			*ap->a_vpp = dvp;
		}
		return error;
	}
	error = cache_lookup(dvp, ap->a_vpp, cnp);
	if (error == -1) {
		return 0;
	}
	if (error == ENOENT) {
		return ENOENT;
	}
	rw_lock(rw);
	if (cnp->cn_flags & ISDOTDOT) {
		struct apfs_fsw_stat st;

		error = apfs_fsw_stat(&rw->vw, dn->an_id, &st);
		id = st.parent;
	} else {
		error = apfs_fsw_lookup(&rw->vw, dn->an_id, cnp->cn_nameptr, (size_t)cnp->cn_namelen, &id, NULL);
	}
	if (error == 0) {
		error = rw_node_vnode(am, id, dvp, cnp, ap->a_vpp);
	}
	lck_mtx_unlock(&rw->lock);
	if (error == ENOENT && (cnp->cn_flags & ISLASTCN) &&
	    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME)) {
		return EJUSTRETURN;
	}
	if (error == ENOENT && cnp->cn_nameiop != CREATE && (cnp->cn_flags & MAKEENTRY)) {
		cache_enter(dvp, NULLVP, cnp);
	}
	rw_watch("lookup", dn->an_id, cnp->cn_nameptr, (size_t)cnp->cn_namelen, NULL, 0, error);
	return error;
}

static int
rw_make(vnode_t dvp, vnode_t *vpp, struct componentname *cnp, struct vnode_attr *vap, uint16_t type,
    const char *target, vfs_context_t ctx)
{
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_rw *rw = APFS_RW(am);
	struct apfs_fsw_attr a;
	uint64_t id = 0;
	int error;

	if ((error = rw_begin_change(rw)) != 0) {
		return error;
	}
	a.mode = (uint16_t)(type | (VATTR_IS_ACTIVE(vap, va_mode) ? (vap->va_mode & 07777) : 0644));
	a.uid = VATTR_IS_ACTIVE(vap, va_uid) ? vap->va_uid : kauth_cred_getuid(vfs_context_ucred(ctx));
	a.gid = VATTR_IS_ACTIVE(vap, va_gid) ? vap->va_gid : kauth_cred_getgid(vfs_context_ucred(ctx));
	a.now = rw_now();
	if (target != NULL) {
		error = apfs_fsw_symlink(&rw->vw, APFS_VP(dvp)->an_id, cnp->cn_nameptr, (size_t)cnp->cn_namelen,
		    target, &a, &id);
	} else {
		error = apfs_fsw_create(&rw->vw, APFS_VP(dvp)->an_id, cnp->cn_nameptr, (size_t)cnp->cn_namelen, &a, &id);
	}
	if (error == 0) {
		VATTR_SET_SUPPORTED(vap, va_mode);
		VATTR_SET_SUPPORTED(vap, va_uid);
		VATTR_SET_SUPPORTED(vap, va_gid);
		cache_purge_negatives(dvp);
		error = rw_node_vnode(am, id, dvp, cnp, vpp);
	}
	rw_end_change(rw, error == 0 || id != 0 ? 0 : error);
	rw_watch(target != NULL ? "symlink" : (type == APFS_S_IFDIR ? "mkdir" : "create"), APFS_VP(dvp)->an_id,
	    cnp->cn_nameptr, (size_t)cnp->cn_namelen, NULL, 0, error);
	return error;
}

static int
rw_create(struct vnop_create_args *ap)
{
	return rw_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, APFS_S_IFREG, NULL, ap->a_context);
}

static int
rw_mkdir(struct vnop_mkdir_args *ap)
{
	return rw_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, APFS_S_IFDIR, NULL, ap->a_context);
}

static int
rw_symlink(struct vnop_symlink_args *ap)
{
	return rw_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, APFS_S_IFLNK, ap->a_target, ap->a_context);
}

static int
rw_mknod(struct vnop_mknod_args *ap)
{
	struct vnode_attr *vap = ap->a_vap;
	uint16_t type;

	switch (vap->va_type) {
	case VFIFO: type = APFS_S_IFIFO; break;
	case VSOCK: type = APFS_S_IFSOCK; break;
	default:    return ENOTSUP;     /* devices: not on a data volume */
	}
	return rw_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, vap, type, NULL, ap->a_context);
}

static int
rw_unname(vnode_t dvp, vnode_t vp, struct componentname *cnp, bool dir)
{
	struct apfs_mount *am = APFS_VP_MOUNT(dvp);
	struct apfs_rw *rw = APFS_RW(am);
	int error;

	if ((error = rw_begin_change(rw)) != 0) {
		return error;
	}
	if (!dir && vnode_isinuse(vp, 0)) {
		/*
		 * Still open: the file has to stay readable and writable until the last close
		 * (POSIX). Its name moves into the volume's private directory, from where it goes
		 * at inactive (rw_inactive).
		 */
		char name[32];

		snprintf(name, sizeof(name), "orphan-%llu", (unsigned long long)APFS_VP(vp)->an_id);
		error = apfs_fsw_rename(&rw->vw, APFS_VP(dvp)->an_id, cnp->cn_nameptr, (size_t)cnp->cn_namelen,
		    RW_PRIVATE_DIR, name, strlen(name), rw_now());
		if (error == 0) {
			APFS_VP(vp)->an_orphan = true;
			APFS_VP(vp)->an_parent = RW_PRIVATE_DIR;
		}
	} else {
		error = apfs_fsw_remove(&rw->vw, APFS_VP(dvp)->an_id, cnp->cn_nameptr, (size_t)cnp->cn_namelen, dir, rw_now());
	}
	rw_end_change(rw, error);
	if (error) {
		printf("apfs: rw: remove %.*s: %d\n", (int)cnp->cn_namelen, cnp->cn_nameptr, error);
	}
	rw_watch(dir ? "rmdir" : "remove", APFS_VP(dvp)->an_id, cnp->cn_nameptr, (size_t)cnp->cn_namelen, NULL, 0, error);
	if (error == 0) {
		cache_purge(vp);
	}
	return error;
}

static int
rw_remove(struct vnop_remove_args *ap)
{
	return rw_unname(ap->a_dvp, ap->a_vp, ap->a_cnp, false);
}

static int
rw_rmdir(struct vnop_rmdir_args *ap)
{
	return rw_unname(ap->a_dvp, ap->a_vp, ap->a_cnp, true);
}

static int
rw_link(struct vnop_link_args *ap)
{
	struct apfs_mount *am = APFS_VP_MOUNT(ap->a_tdvp);
	struct apfs_rw *rw = APFS_RW(am);
	int error;

	if (APFS_VP_MOUNT(ap->a_vp) != am) {
		return EXDEV;
	}
	if (vnode_isdir(ap->a_vp)) {
		return EPERM;
	}
	if ((error = rw_begin_change(rw)) != 0) {
		return error;
	}
	error = apfs_fsw_link(&rw->vw, APFS_VP(ap->a_vp)->an_id, APFS_VP(ap->a_tdvp)->an_id, ap->a_cnp->cn_nameptr,
	    (size_t)ap->a_cnp->cn_namelen, rw_now());
	rw_end_change(rw, error);
	rw_watch("link", APFS_VP(ap->a_tdvp)->an_id, ap->a_cnp->cn_nameptr, (size_t)ap->a_cnp->cn_namelen, NULL, 0, error);
	if (error == 0) {
		cache_purge_negatives(ap->a_tdvp);
	}
	return error;
}

static int
rw_rename(struct vnop_rename_args *ap)
{
	struct apfs_mount *am = APFS_VP_MOUNT(ap->a_fdvp);
	struct apfs_rw *rw = APFS_RW(am);
	int error;

	if (APFS_VP_MOUNT(ap->a_tdvp) != am) {
		return EXDEV;
	}
	if ((error = rw_begin_change(rw)) != 0) {
		return error;
	}
	error = apfs_fsw_rename(&rw->vw, APFS_VP(ap->a_fdvp)->an_id, ap->a_fcnp->cn_nameptr, (size_t)ap->a_fcnp->cn_namelen,
	    APFS_VP(ap->a_tdvp)->an_id, ap->a_tcnp->cn_nameptr, (size_t)ap->a_tcnp->cn_namelen, rw_now());
	if (error == 0) {
		APFS_VP(ap->a_fvp)->an_parent = APFS_VP(ap->a_tdvp)->an_id;
	}
	rw_end_change(rw, error);
	rw_watch("rename", APFS_VP(ap->a_fdvp)->an_id, ap->a_fcnp->cn_nameptr, (size_t)ap->a_fcnp->cn_namelen,
	    ap->a_tcnp->cn_nameptr, (size_t)ap->a_tcnp->cn_namelen, error);
	if (error == 0) {
		cache_purge(ap->a_fvp);
		if (ap->a_tvp != NULLVP) {
			cache_purge(ap->a_tvp);
		}
		cache_purge_negatives(ap->a_tdvp);
	}
	return error;
}

/* ------------------------------------------------------------------ attributes */

static int
rw_getattr(struct vnop_getattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_rw *rw = APFS_RW(am);
	struct apfs_fsw_stat st;
	int error;

	rw_lock(rw);
	error = apfs_fsw_stat(&rw->vw, APFS_VP(vp)->an_id, &st);
	lck_mtx_unlock(&rw->lock);
	if (error) {
		return error;
	}
	VATTR_RETURN(vap, va_rdev, (dev_t)st.rdev);
	VATTR_RETURN(vap, va_nlink, vnode_isdir(vp) ? 2 + st.nchildren : APFS_VP(vp)->an_orphan ? 0 :
	    (st.nchildren > 0 ? st.nchildren : 1));
	VATTR_RETURN(vap, va_data_size, st.size);
	VATTR_RETURN(vap, va_total_size, st.size);
	VATTR_RETURN(vap, va_total_alloc, st.alloced);
	VATTR_RETURN(vap, va_data_alloc, st.alloced);
	VATTR_RETURN(vap, va_iosize, am->am_nx.block_size);
	VATTR_RETURN(vap, va_uid, st.uid);
	VATTR_RETURN(vap, va_gid, st.gid);
	VATTR_RETURN(vap, va_mode, st.mode & 07777);
	VATTR_RETURN(vap, va_flags, st.bsd_flags);
	VATTR_RETURN(vap, va_fileid, st.id);
	VATTR_RETURN(vap, va_linkid, st.id);
	VATTR_RETURN(vap, va_parentid, st.parent);
	VATTR_RETURN(vap, va_fsid, (uint32_t)vfs_statfs(am->am_mp)->f_fsid.val[0]);
	VATTR_RETURN(vap, va_filerev, 0);
	VATTR_RETURN(vap, va_gen, 0);
	VATTR_RETURN(vap, va_write_gencount, st.write_gen);
	VATTR_RETURN(vap, va_document_id, st.document_id);
	VATTR_RETURN(vap, va_type, vnode_vtype(vp));
	if (vnode_isdir(vp)) {
		VATTR_RETURN(vap, va_nchildren, st.nchildren);
		VATTR_RETURN(vap, va_dirlinkcount, 1);
	}
	if (VATTR_IS_ACTIVE(vap, va_name) && vap->va_name != NULL && st.name[0] != '\0' && !vnode_isvroot(vp)) {
		strlcpy(vap->va_name, st.name, MAXPATHLEN);
		VATTR_SET_SUPPORTED(vap, va_name);
	}
#define RW_TIME(field, ns) \
	if (VATTR_IS_ACTIVE(vap, field)) { \
	        vap->field.tv_sec = (long)((ns) / 1000000000ULL); \
	        vap->field.tv_nsec = (long)((ns) % 1000000000ULL); \
	        VATTR_SET_SUPPORTED(vap, field); \
	}
	RW_TIME(va_access_time, st.access_time);
	RW_TIME(va_modify_time, st.mod_time);
	RW_TIME(va_change_time, st.change_time);
	RW_TIME(va_create_time, st.create_time);
#undef RW_TIME
	return 0;
}

static uint64_t
ts_ns(const struct timespec *t)
{
	return (uint64_t)t->tv_sec * 1000000000ULL + (uint64_t)t->tv_nsec;
}

static int
rw_setattr(struct vnop_setattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_rw *rw = APFS_RW(am);
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_fsw_set s;
	int error;

	bzero(&s, sizeof(s));
	if ((error = rw_begin_change(rw)) != 0) {
		return error;
	}
	s.now = rw_now();
	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		if (!vnode_isreg(vp)) {
			error = vnode_isdir(vp) ? EISDIR : EINVAL;
		} else {
			error = apfs_fsw_truncate(&rw->vw, an->an_id, (uint64_t)vap->va_data_size, s.now);
			if (error == 0) {
				an->an_size = (uint64_t)vap->va_data_size;
			}
		}
		VATTR_SET_SUPPORTED(vap, va_data_size);
	}
	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		s.which |= APFS_FSW_MODE;
		s.mode = (uint16_t)vap->va_mode;
		VATTR_SET_SUPPORTED(vap, va_mode);
	}
	if (VATTR_IS_ACTIVE(vap, va_uid)) {
		s.which |= APFS_FSW_UID;
		s.uid = vap->va_uid;
		VATTR_SET_SUPPORTED(vap, va_uid);
	}
	if (VATTR_IS_ACTIVE(vap, va_gid)) {
		s.which |= APFS_FSW_GID;
		s.gid = vap->va_gid;
		VATTR_SET_SUPPORTED(vap, va_gid);
	}
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		s.which |= APFS_FSW_FLAGS;
		s.flags = vap->va_flags;
		VATTR_SET_SUPPORTED(vap, va_flags);
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		s.which |= APFS_FSW_MTIME;
		s.mtime = ts_ns(&vap->va_modify_time);
		VATTR_SET_SUPPORTED(vap, va_modify_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		s.which |= APFS_FSW_ATIME;
		s.atime = ts_ns(&vap->va_access_time);
		VATTR_SET_SUPPORTED(vap, va_access_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_create_time)) {
		s.which |= APFS_FSW_CRTIME;
		s.crtime = ts_ns(&vap->va_create_time);
		VATTR_SET_SUPPORTED(vap, va_create_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_change_time)) {
		VATTR_SET_SUPPORTED(vap, va_change_time);       /* it is now, whatever is asked */
	}
	if (error == 0 && s.which != 0) {
		error = apfs_fsw_setattr(&rw->vw, an->an_id, &s);
	}
	if ((vap->va_active & ~vap->va_supported) != 0) {
		static uint64_t told;
		uint64_t missing = vap->va_active & ~vap->va_supported;

		if ((told & missing) != missing) {
			told |= missing;
			printf("apfs: rw: setattr: attributes not kept here: 0x%llx (by %s)\n", (unsigned long long)missing,
			    proc_best_name(current_proc()));
		}
	}
	rw_end_change(rw, error);
	if (error == 0 && VATTR_IS_ACTIVE(vap, va_data_size)) {
		ubc_setsize(vp, (off_t)vap->va_data_size);
	}
	return error;
}

/* ------------------------------------------------------------------ contents */

static int
rw_read(struct vnop_read_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_rw *rw = APFS_RW(am);
	uint8_t *buf;
	int error = 0;

	if (vnode_isdir(vp)) {
		return EISDIR;
	}
	if (!vnode_isreg(vp)) {
		return EPERM;
	}
	if (uio_offset(uio) < 0) {
		return EINVAL;
	}
	/* what a mapping has changed and not written back yet goes out first */
	if (ubc_pages_resident(vp)) {
		(void)ubc_msync(vp, 0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY | UBC_SYNC);
	}
	buf = kalloc_data(APFS_RW_IOCHUNK, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	while (uio_resid(uio) > 0) {
		size_t want = (size_t)MIN(uio_resid(uio), APFS_RW_IOCHUNK), got = 0;

		rw_lock(rw);
		{
			uint64_t t0 = mach_absolute_time();

			error = apfs_fsw_read(&rw->vw, APFS_VP(vp)->an_id, (uint64_t)uio_offset(uio), buf, want, &got);
			rw->now.reads++;
			rw->now.read_bytes += got;
			rw->now.read_ticks += mach_absolute_time() - t0;
		}
		lck_mtx_unlock(&rw->lock);
		if (error || got == 0) {
			break;
		}
		if ((error = uiomove((caddr_t)buf, (int)got, uio)) != 0) {
			break;
		}
	}
	kfree_data(buf, APFS_RW_IOCHUNK);
	return error;
}

static int
rw_write(struct vnop_write_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_rw *rw = APFS_RW(am);
	struct apfs_node *an = APFS_VP(vp);
	off_t start, end;
	uint8_t *buf;
	int error = 0;

	if (!vnode_isreg(vp)) {
		return vnode_isdir(vp) ? EISDIR : EPERM;
	}
	if (ubc_pages_resident(vp)) {
		(void)ubc_msync(vp, 0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY | UBC_SYNC);
	}
	buf = kalloc_data(APFS_RW_IOCHUNK, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	/*
	 * The caller's bytes are copied in without the volume's lock: they may be a mapping of a file
	 * of this very volume, and touching them pages it in (rw_pagein), which takes the lock. So a
	 * piece at a time: copied in, then written with the lock held.
	 */
	if (ap->a_ioflag & IO_APPEND) {
		struct apfs_fsw_stat st;

		rw_lock(rw);
		error = apfs_fsw_stat(&rw->vw, an->an_id, &st);
		lck_mtx_unlock(&rw->lock);
		if (error == 0) {
			uio_setoffset(uio, (off_t)st.size);
		}
	}
	start = uio_offset(uio);
	while (error == 0 && uio_resid(uio) > 0) {
		size_t n = (size_t)MIN(uio_resid(uio), APFS_RW_IOCHUNK);
		off_t at = uio_offset(uio);

		if ((error = uiomove((caddr_t)buf, (int)n, uio)) != 0) {
			break;
		}
		if ((error = rw_begin_change(rw)) != 0) {
			break;
		}
		{
			uint64_t t0 = mach_absolute_time();

			error = apfs_fsw_write(&rw->vw, an->an_id, (uint64_t)at, buf, n, rw_now());
			rw->now.writes++;
			rw->now.written += n;
			rw->now.write_ticks += mach_absolute_time() - t0;
		}
		rw_end_change(rw, error);
	}
	end = uio_offset(uio);
	if (end > (off_t)an->an_size) {
		an->an_size = (uint64_t)end;
	}
	kfree_data(buf, APFS_RW_IOCHUNK);
	if (end > start) {
		if ((off_t)ubc_getsize(vp) < end) {
			ubc_setsize(vp, end);
		}
		/* the page cache's copy of what was written is stale now */
		(void)ubc_msync(vp, start & ~PAGE_MASK_64, (end + PAGE_MASK_64) & ~PAGE_MASK_64, NULL, UBC_INVALIDATE);
	}
	if (error == 0 && (ap->a_ioflag & IO_SYNC)) {
		error = apfs_rw_sync(am);
	}
	return error;
}

static int
rw_pagein(struct vnop_pagein_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_rw *rw = APFS_RW(am);
	upl_t upl = ap->a_pl;
	vm_offset_t address = 0;
	size_t got = 0;
	int error = 0;

	if (upl == NULL) {
		rw_trace("pagein without a UPL", APFS_VP(vp)->an_id, ap->a_f_offset, ap->a_size, ap->a_flags, EINVAL);
		return EINVAL;
	}
	if (ubc_upl_map(upl, &address) != KERN_SUCCESS) {
		error = ENOMEM;
	} else {
		uint8_t *dst = (uint8_t *)address + ap->a_pl_offset;

		rw_lock(rw);
		{
			uint64_t t0 = mach_absolute_time();

			error = apfs_fsw_read(&rw->vw, APFS_VP(vp)->an_id, (uint64_t)ap->a_f_offset, dst, ap->a_size, &got);
			rw->now.reads++;
			rw->now.read_bytes += got;
			rw->now.read_ticks += mach_absolute_time() - t0;
		}
		lck_mtx_unlock(&rw->lock);
		if (error == 0 && got < ap->a_size) {
			bzero(dst + got, ap->a_size - got);
		}
		ubc_upl_unmap(upl);
	}
	rw_trace("pagein", APFS_VP(vp)->an_id, ap->a_f_offset, ap->a_size, ap->a_flags, error);
	if (!(ap->a_flags & UPL_NOCOMMIT)) {
		if (error) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, (upl_size_t)ap->a_size, UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
		} else {
			ubc_upl_commit_range(upl, ap->a_pl_offset, (upl_size_t)ap->a_size,
			    UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
	}
	return error;
}

static int
rw_pageout(struct vnop_pageout_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_rw *rw = APFS_RW(am);
	upl_t upl = ap->a_pl;
	vm_offset_t address = 0;
	off_t size = ubc_getsize(vp);
	size_t n;
	int error = 0;

	if (upl == NULL) {
		/* the vnode is to make its own UPL (vfs_setflags(mp, MNT_...) not asked for): not done here */
		rw_trace("pageout without a UPL", APFS_VP(vp)->an_id, ap->a_f_offset, ap->a_size, ap->a_flags, EINVAL);
		return EINVAL;
	}
	if (ap->a_f_offset >= size) {
		n = 0;
	} else {
		n = (size_t)MIN((off_t)ap->a_size, size - ap->a_f_offset);
	}
	if (n > 0) {
		if (ubc_upl_map(upl, &address) != KERN_SUCCESS) {
			error = ENOMEM;
		} else if ((error = rw_begin_change(rw)) == 0) {
			uint64_t t0 = mach_absolute_time();

			error = apfs_fsw_write(&rw->vw, APFS_VP(vp)->an_id, (uint64_t)ap->a_f_offset,
			    (uint8_t *)address + ap->a_pl_offset, n, rw_now());
			rw->now.writes++;
			rw->now.written += n;
			rw->now.write_ticks += mach_absolute_time() - t0;
			rw_end_change(rw, error);
			ubc_upl_unmap(upl);
		} else {
			ubc_upl_unmap(upl);
		}
	}
	rw_trace("pageout", APFS_VP(vp)->an_id, ap->a_f_offset, ap->a_size, ap->a_flags, error);
	if (!(ap->a_flags & UPL_NOCOMMIT)) {
		if (error) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, (upl_size_t)ap->a_size, UPL_ABORT_FREE_ON_EMPTY);
		} else {
			ubc_upl_commit_range(upl, ap->a_pl_offset, (upl_size_t)ap->a_size,
			    UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
	}
	return error;
}

static int
rw_fsync(struct vnop_fsync_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_rw *rw = APFS_RW(APFS_VP_MOUNT(vp));
	bool going = vnode_isrecycled(vp), now = ap->a_waitfor == MNT_WAIT;

	if (vnode_isreg(vp) && ubc_pages_resident(vp)) {
		(void)ubc_msync(vp, 0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY | UBC_SYNC);
	}
	/*
	 * The kernel fsyncs every vnode it lets go of (vclean), and F_FLUSH_DATA fsyncs without
	 * waiting. Neither is anyone asking for what has changed to be safe now, and a whole
	 * checkpoint for each is what made an install crawl. The contents have gone to the device
	 * and what names them goes with the commit that is due anyway (rw_dirty armed the timer; the
	 * unmount and sync(2) commit too).
	 */
	if (going || !now) {
		rw_lock(rw);
		if (going) {
			rw->now.recycled++;
		} else {
			rw->now.nowait++;
		}
		lck_mtx_unlock(&rw->lock);
		return 0;
	}
	if (rw_stats_on()) {
		char me[17];

		proc_selfname(me, sizeof(me));
		rw_lock(rw);
		for (uint32_t i = 0; i < 8; i++) {
			if (rw->fsync_by[i].count == 0) {
				strlcpy(rw->fsync_by[i].name, me, sizeof(rw->fsync_by[i].name));
			}
			if (strcmp(rw->fsync_by[i].name, me) == 0) {
				rw->fsync_by[i].count++;
				break;
			}
		}
		lck_mtx_unlock(&rw->lock);
	}
	return apfs_rw_sync(APFS_VP_MOUNT(vp));
}

/* ------------------------------------------------------------------ directories and links */

struct rw_rd {
	struct apfs_rd_ctx rc;
};

static int
rw_readdir_cb(void *ctx, const char *name, size_t len, uint64_t id, uint8_t dtype)
{
	return apfs_vnop_readdir_one(ctx, name, len, id, dtype) != 0;
}

/*
 * A uio of the kernel's own over a buffer, for an operation that fills one with the volume's lock
 * held: the caller's memory may be a mapping of a file of this volume, and touching it would page
 * that in (rw_pagein), which takes the lock. Filled first, copied out after.
 */
#define RW_STAGE_MAX (64 * 1024)

static uio_t
rw_stage(uio_t to, uint8_t **buf, size_t *len)
{
	uio_t k;

	*len = (size_t)MIN(uio_resid(to), RW_STAGE_MAX);
	*buf = kalloc_data(*len > 0 ? *len : 1, Z_WAITOK);
	if (*buf == NULL) {
		return NULL;
	}
	k = uio_create(1, uio_offset(to), UIO_SYSSPACE, UIO_READ);
	if (k == NULL) {
		kfree_data(*buf, *len > 0 ? *len : 1);
		return NULL;
	}
	uio_addiov(k, CAST_USER_ADDR_T(*buf), *len);
	return k;
}

static int
rw_unstage(uio_t k, uio_t to, uint8_t *buf, size_t len, int error)
{
	size_t made = len - (size_t)uio_resid(k);

	if (error == 0 && made > 0) {
		error = uiomove((caddr_t)buf, (int)made, to);
	}
	uio_free(k);
	kfree_data(buf, len > 0 ? len : 1);
	return error;
}

static int
rw_readdir(struct vnop_readdir_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_rw *rw = APFS_RW(am);
	struct apfs_node *an = APFS_VP(vp);
	struct apfs_rd_ctx rc;
	struct apfs_fsw_stat st;
	int error;

	if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF)) {
		return EINVAL;
	}
	if (!vnode_isdir(vp)) {
		return ENOTDIR;
	}
	uint8_t *kbuf;
	size_t klen;
	uio_t kuio = rw_stage(ap->a_uio, &kbuf, &klen);

	if (kuio == NULL) {
		return ENOMEM;
	}
	bzero(&rc, sizeof(rc));
	rc.rd_uio = kuio;
	rc.rd_start = uio_offset(ap->a_uio);
	rw_lock(rw);
	error = apfs_fsw_stat(&rw->vw, an->an_id, &st);
	if (error == 0) {
		error = apfs_vnop_readdir_one(&rc, ".", 1, an->an_id, DT_DIR);
	}
	if (error == 0) {
		error = apfs_vnop_readdir_one(&rc, "..", 2, st.parent, DT_DIR);
	}
	if (error == 0) {
		error = apfs_fsw_readdir(&rw->vw, an->an_id, rw_readdir_cb, &rc);
	}
	lck_mtx_unlock(&rw->lock);
	if (error == EEXIST) {
		error = 0;
	}
	if (error == 0) {
		error = rc.rd_error;
	}
	if ((error = rw_unstage(kuio, ap->a_uio, kbuf, klen, error)) != 0) {
		return error;
	}
	if (error != 0) {
		return error;
	}
	uio_setoffset(ap->a_uio, rc.rd_pos);
	if (ap->a_eofflag != NULL) {
		*ap->a_eofflag = rc.rd_full ? 0 : 1;
	}
	if (ap->a_numdirent != NULL) {
		*ap->a_numdirent = rc.rd_count;
	}
	return 0;
}

static int
rw_readlink(struct vnop_readlink_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_rw *rw = APFS_RW(APFS_VP_MOUNT(vp));
	char *buf;
	size_t n = MAXPATHLEN;
	int error;

	if (!vnode_islnk(vp)) {
		return EINVAL;
	}
	buf = kalloc_data(MAXPATHLEN, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	rw_lock(rw);
	error = apfs_fsw_getxattr(&rw->vw, APFS_VP(vp)->an_id, APFS_SYMLINK_EA_NAME, buf, &n);
	lck_mtx_unlock(&rw->lock);
	if (error == 0) {
		/* the attribute holds the target and its NUL; readlink gives the target alone */
		if (n > 0 && buf[n - 1] == '\0') {
			n--;
		}
		error = uiomove(buf, (int)n, ap->a_uio);
	}
	kfree_data(buf, MAXPATHLEN);
	return error;
}

/* ------------------------------------------------------------------ extended attributes */

static int
rw_getxattr(struct vnop_getxattr_args *ap)
{
	struct apfs_rw *rw = APFS_RW(APFS_VP_MOUNT(ap->a_vp));
	uint8_t *buf;
	size_t n = APFS_FSW_XATTR_MAX;
	int error;

	if (apfs_xattr_hidden(ap->a_name)) {
		return ENOATTR;
	}
	buf = kalloc_data(APFS_FSW_XATTR_MAX, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	rw_lock(rw);
	error = apfs_fsw_getxattr(&rw->vw, APFS_VP(ap->a_vp)->an_id, ap->a_name, buf, &n);
	lck_mtx_unlock(&rw->lock);
	if (error == 0) {
		if (ap->a_uio == NULL) {
			*ap->a_size = n;
		} else if ((user_ssize_t)n > uio_resid(ap->a_uio)) {
			error = ERANGE;
		} else {
			error = uiomove((caddr_t)buf, (int)n, ap->a_uio);
		}
	}
	kfree_data(buf, APFS_FSW_XATTR_MAX);
	return error;
}

static int
rw_setxattr(struct vnop_setxattr_args *ap)
{
	struct apfs_rw *rw = APFS_RW(APFS_VP_MOUNT(ap->a_vp));
	user_ssize_t len = uio_resid(ap->a_uio);
	uint8_t *buf;
	int error;

	if (apfs_xattr_hidden(ap->a_name)) {
		return EPERM;
	}
	if (len < 0 || len > APFS_FSW_XATTR_MAX) {
		return E2BIG;
	}
	buf = kalloc_data(len > 0 ? (size_t)len : 1, Z_WAITOK);
	if (buf == NULL) {
		return ENOMEM;
	}
	if ((error = uiomove((caddr_t)buf, (int)len, ap->a_uio)) == 0 && (error = rw_begin_change(rw)) == 0) {
		error = apfs_fsw_setxattr(&rw->vw, APFS_VP(ap->a_vp)->an_id, ap->a_name, buf, (size_t)len,
		    (ap->a_options & XATTR_CREATE) != 0, (ap->a_options & XATTR_REPLACE) != 0);
		rw_end_change(rw, error);
	}
	kfree_data(buf, len > 0 ? (size_t)len : 1);
	return error;
}

static int
rw_removexattr(struct vnop_removexattr_args *ap)
{
	struct apfs_rw *rw = APFS_RW(APFS_VP_MOUNT(ap->a_vp));
	int error;

	if (apfs_xattr_hidden(ap->a_name)) {
		return ENOATTR;
	}
	if ((error = rw_begin_change(rw)) != 0) {
		return error;
	}
	error = apfs_fsw_rmxattr(&rw->vw, APFS_VP(ap->a_vp)->an_id, ap->a_name);
	rw_end_change(rw, error);
	return error;
}

struct rw_lx {
	struct uio *uio;
	size_t size;
	int error;
};

static int
rw_listxattr_cb(void *ctx, const char *name, size_t len)
{
	struct rw_lx *l = ctx;
	char tmp[128];

	if (len >= sizeof(tmp)) {
		return 0;
	}
	memcpy(tmp, name, len);
	tmp[len] = '\0';
	if (apfs_xattr_hidden(tmp)) {
		return 0;
	}
	l->size += len + 1;
	if (l->uio != NULL) {
		if (uio_resid(l->uio) < (user_ssize_t)(len + 1)) {
			l->error = ERANGE;
			return 1;
		}
		l->error = uiomove(tmp, (int)(len + 1), l->uio);
		return l->error != 0;
	}
	return 0;
}

static int
rw_listxattr(struct vnop_listxattr_args *ap)
{
	struct apfs_rw *rw = APFS_RW(APFS_VP_MOUNT(ap->a_vp));
	struct rw_lx l = { .uio = NULL };
	uint8_t *kbuf = NULL;
	size_t klen = 0;
	uio_t kuio = NULL;
	int error;

	if (ap->a_uio != NULL && (kuio = l.uio = rw_stage(ap->a_uio, &kbuf, &klen)) == NULL) {
		return ENOMEM;
	}
	rw_lock(rw);
	error = apfs_fsw_listxattr(&rw->vw, APFS_VP(ap->a_vp)->an_id, rw_listxattr_cb, &l);
	lck_mtx_unlock(&rw->lock);
	if (error == 0) {
		error = l.error;
	}
	if (kuio != NULL) {
		error = rw_unstage(kuio, ap->a_uio, kbuf, klen, error);
	}
	if (error == 0 && ap->a_uio == NULL) {
		*ap->a_size = l.size;
	}
	return error;
}

/* ------------------------------------------------------------------ ioctls and fsctls */

/*
 * Apple's private fsctls that FileProvider cannot go without: to it ENOTTY on these means the item
 * cannot be read at all ("can't obtain VFSItem", and "On My iPhone" stays read-only), where a
 * filesystem without the feature says so. These volumes keep no purgeable marks and no directory
 * statistics, and that is what the answers say. Names and meanings are read from how
 * FileProvider.framework uses them: the layouts are not public.
 */
#define RW_IOC_PURGEABLE_FLAGS  _IOR('J', 0x47, uint64_t)       /* a file's purgeable flags */
#define RW_IOC_MARK_PURGEABLE   _IOWR('J', 0x44, uint64_t)      /* 0: not purgeable */
#define RW_IOC_DIR_STATS_ID     _IOR('J', 0x4a, uint64_t)       /* a directory's statistics; 0: none */
#define RW_IOC_DIR_STATS_GET    _IOR('J', 0x48, uint32_t)       /* are they kept here? */
#define RW_IOC_DIR_STATS_SET    _IOW('J', 0x48, uint32_t)       /* keep them */
/*
 * What Apple's APFS answers, seen on a Mac: EINVAL for a file; for a directory success, and that
 * directory has a statistics key in its inode afterwards. No statistics are kept here.
 */
#define RW_IOC_J2               _IOW('J', 0x02, uint64_t)
/*
 * A directory's statistics (40 bytes). On a Mac: ENOTSUP for a directory that has none, which is
 * what FileProvider takes for "work it out yourself", EINVAL for a file. ENOTTY here made it say
 * that a folder just created does not exist.
 */
#define RW_IOC_DIR_STATS        _IOC(IOC_INOUT, 'J', 0x20, 40)

/*
 * What iOS asks of a volume with fcntl and fsctl, answered from what the volume holds: a full
 * sync (F_FULLFSYNC: the commit, and the flash has it when the loader answers -- it reads every
 * write back), a barrier (F_BARRIERFSYNC: the same), and the compare-and-swap of a file's BSD
 * flags (FSIOC_CAS_BSDFLAGS). Anything else: ENOTTY, "not this filesystem's" -- Apple's own APFS
 * fsctls among them, whose layouts are not public.
 */
static int
rw_ioctl(struct vnop_ioctl_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct apfs_mount *am = APFS_VP_MOUNT(vp);
	struct apfs_rw *rw = APFS_RW(am);

	switch (ap->a_command) {
	case F_FULLFSYNC:
	case F_BARRIERFSYNC:
		if (vnode_isreg(vp) && ubc_pages_resident(vp)) {
			(void)ubc_msync(vp, 0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY | UBC_SYNC);
		}
		return apfs_rw_sync(am);
	case FSIOC_CAS_BSDFLAGS: {
		struct fsioc_cas_bsdflags *cas = (struct fsioc_cas_bsdflags *)ap->a_data;
		struct apfs_fsw_stat st;
		int error;

		if ((error = rw_begin_change(rw)) != 0) {
			return error;
		}
		error = apfs_fsw_stat(&rw->vw, APFS_VP(vp)->an_id, &st);
		if (error == 0) {
			cas->actual_flags = st.bsd_flags;
			if (st.bsd_flags == cas->expected_flags) {
				struct apfs_fsw_set set = { .which = APFS_FSW_FLAGS, .flags = cas->new_flags, .now = rw_now() };

				error = apfs_fsw_setattr(&rw->vw, APFS_VP(vp)->an_id, &set);
			}
		}
		rw_end_change(rw, error);
		return error;
	}
	case RW_IOC_PURGEABLE_FLAGS:
		*(uint64_t *)ap->a_data = 0;
		return 0;
	case RW_IOC_MARK_PURGEABLE:
		return *(uint64_t *)ap->a_data == 0 ? 0 : ENOTSUP;
	case RW_IOC_DIR_STATS_ID:
		*(uint64_t *)ap->a_data = 0;
		return 0;
	case RW_IOC_DIR_STATS_GET:
		*(uint32_t *)ap->a_data = 0;
		return 0;
	case RW_IOC_DIR_STATS_SET:
		return ENOTSUP;
	case RW_IOC_J2:
		return vnode_isdir(vp) ? 0 : EINVAL;
	case RW_IOC_DIR_STATS:
		return vnode_isdir(vp) ? ENOTSUP : EINVAL;
	default:
		return ENOTTY;
	}
}

/* ------------------------------------------------------------------ the filesystem */

/* how big the volume's container is and how much of it is free, in its blocks */
void
apfs_rw_space(struct apfs_mount *am, uint64_t *blocks, uint64_t *free_blocks)
{
	struct apfs_rw *rw = APFS_RW(am);

	rw_lock(rw);
	*blocks = am->am_nx.block_count;
	*free_blocks = rw->vw.sp.free_count;
	lck_mtx_unlock(&rw->lock);
}

int
apfs_rw_statfs(struct apfs_mount *am, struct vfs_attr *fsap)
{
	struct apfs_rw *rw = APFS_RW(am);
	uint64_t files, free_blocks, files_only, dirs, last_mod;
	uint8_t uuid[16];

	rw_lock(rw);
	files_only = rw_vsb64(rw, VO_NUM_FILES);
	dirs = rw_vsb64(rw, VO_NUM_DIRS);
	files = files_only + dirs;
	last_mod = rw_vsb64(rw, (uint32_t)__builtin_offsetof(struct apfs_superblock, apfs_last_mod_time));
	memcpy(uuid, rw->vw.vsb + __builtin_offsetof(struct apfs_superblock, apfs_vol_uuid), sizeof(uuid));
	free_blocks = rw->vw.sp.free_count;
	lck_mtx_unlock(&rw->lock);
	VFSATTR_RETURN(fsap, f_objcount, files);
	VFSATTR_RETURN(fsap, f_bsize, am->am_nx.block_size);
	VFSATTR_RETURN(fsap, f_iosize, APFS_RW_IOCHUNK);
	VFSATTR_RETURN(fsap, f_blocks, am->am_nx.block_count);
	VFSATTR_RETURN(fsap, f_bfree, free_blocks);
	VFSATTR_RETURN(fsap, f_bavail, free_blocks);
	VFSATTR_RETURN(fsap, f_bused, am->am_nx.block_count - free_blocks);
	VFSATTR_RETURN(fsap, f_files, files);
	VFSATTR_RETURN(fsap, f_ffree, free_blocks);
	/*
	 * The rest of what getattrlist(2) may be asked about a volume, as ramfs answers it: unless the
	 * caller also asks which attributes came back, one left out fails the whole call with EINVAL.
	 * CacheDelete asks for the counts and the UUID together; without them it has a volume with no
	 * UUID, and assetsd and imagent died of the nil key it made of that.
	 */
	VFSATTR_RETURN(fsap, f_filecount, files_only);
	VFSATTR_RETURN(fsap, f_dircount, dirs);
	VFSATTR_RETURN(fsap, f_maxobjcount, am->am_nx.block_count);
	VFSATTR_RETURN(fsap, f_owner, 0);
	VFSATTR_RETURN(fsap, f_fssubtype, 0);
	VFSATTR_RETURN(fsap, f_signature, 0x4244);              /* 'BD', what the VFS layer defaults to */
	VFSATTR_RETURN(fsap, f_carbon_fsid, 0);
	VFSATTR_RETURN(fsap, f_quota, 0);
	VFSATTR_RETURN(fsap, f_reserved, 0);
	if (VFSATTR_IS_ACTIVE(fsap, f_fsid)) {
		fsap->f_fsid = vfs_statfs(am->am_mp)->f_fsid;
		VFSATTR_SET_SUPPORTED(fsap, f_fsid);
	}
	{
		struct timespec changed = { .tv_sec = (long)(last_mod / 1000000000ULL), .tv_nsec = (long)(last_mod % 1000000000ULL) };
		struct timespec never = { .tv_sec = 0, .tv_nsec = 0 };

		VFSATTR_RETURN(fsap, f_create_time, changed);
		VFSATTR_RETURN(fsap, f_modify_time, changed);
		VFSATTR_RETURN(fsap, f_access_time, changed);
		VFSATTR_RETURN(fsap, f_backup_time, never);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_uuid)) {
		memcpy(fsap->f_uuid, uuid, sizeof(fsap->f_uuid));       /* the volume's own, as it is on the flash */
		VFSATTR_SET_SUPPORTED(fsap, f_uuid);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_attributes)) {
		vol_attributes_attr_t *attr = &fsap->f_attributes;

		attr->validattr.commonattr = ATTR_CMN_VALIDMASK;
		attr->validattr.volattr = ATTR_VOL_VALIDMASK;
		attr->validattr.dirattr = ATTR_DIR_VALIDMASK;
		attr->validattr.fileattr = ATTR_FILE_VALIDMASK;
		attr->validattr.forkattr = 0;
		attr->nativeattr = attr->validattr;
		VFSATTR_SET_SUPPORTED(fsap, f_attributes);
	}
	return 0;
}
