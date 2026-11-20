/*
 * tsboot: TrollStore on HackiOS (shennong). Run at every boot from build/boot-bin (first lines of rc.jobs).
 *
 * Why TrollStore works here at all: this kernel checks no signatures (libkern/amfi/miniamfi.c) and takes a
 * process's entitlements from whatever signature its binary carries. TrollStore's CoreTrust trick is not
 * needed, but harmless.
 *
 * Why it does not work without this: TrollStore runs its root helper (trollstorehelper) with
 *   posix_spawnattr_set_persona_np(&attr, 99, POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE) + uid 0 / gid 0,
 * and the kernel refuses a spawn into a persona that does not exist (kern_exec.c spawn_validate_persona:
 * ESRCH). On an iPhone persona 99 exists; here no persona exists at all (launchd's log: "kpersona_find_by_type()
 * failed ... No such process"). So the helper never starts and every install fails.
 *
 * What it does:
 *   1. makes persona 99 if there is none (kpersona_alloc needs only com.apple.private.persona-mgmt,
 *      which tsboot.entitlements gives it; no root needed);
 *   2. in the background (the job list goes on at once), after LaunchServices and the container manager are up:
 *      - no TrollStore in /var/containers/Bundle/Application yet: trollstorehelper install-trollstore
 *        /redwood/share/trollstore/TrollStore.tar  (once: /private/var is on the flash, var_apfs)
 *      - TrollStore there: trollstorehelper refresh  (registers TrollStore and every app it installed
 *        with LaunchServices again, in case the boot-time database rebuild dropped them).
 *        Not "refresh-all": that one also rebuilds LaunchServices' whole database (lsd and pkd churn for
 *        ~20 s, every widget and poster extension is killed) and then does killall backboardd, so the
 *        home screen froze and SpringBoard restarted 40-60 s after every boot. "refresh" re-registers
 *        the same apps without a rebuild and without a respring; SpringBoard picks the icons up from
 *        LaunchServices' notifications.
 * Everything it does is printed on the console (the session log) with the prefix "tsboot:".
 *
 * Built without the SDK's headers (as bh_notify is): the few declarations it needs are below.
 */
typedef unsigned int uint32_t;
typedef unsigned long size_t;
typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef void *posix_spawnattr_t;

extern int printf(const char *, ...);
extern int fflush(void *);
extern int snprintf(char *, size_t, const char *, ...);
extern char *strerror(int);
extern int *__error(void);
extern int fork(void);
extern int setsid(void);
extern unsigned int sleep(unsigned int);
extern int access(const char *, int);
extern int posix_spawnattr_init(posix_spawnattr_t *);
extern int posix_spawnattr_destroy(posix_spawnattr_t *);
extern int posix_spawnattr_set_persona_np(const posix_spawnattr_t *, uid_t, uint32_t);
extern int posix_spawnattr_set_persona_uid_np(const posix_spawnattr_t *, uid_t);
extern int posix_spawnattr_set_persona_gid_np(const posix_spawnattr_t *, gid_t);
extern int posix_spawn(pid_t *, const char *, void *, const posix_spawnattr_t *, char *const[], char *const[]);
extern pid_t waitpid(pid_t, int *, int);
extern char ***_NSGetEnviron(void);

/* <dirent.h>, arm64 (64-bit inodes) */
struct dirent {
	unsigned long long d_ino, d_seekoff;
	unsigned short d_reclen, d_namlen;
	unsigned char d_type;
	char d_name[1024];
};
typedef struct _DIR DIR;
extern DIR *opendir(const char *);
extern struct dirent *readdir(DIR *);
extern int closedir(DIR *);

/* <sys/persona.h> (xnu-8796) */
#define PERSONA_INFO_V2                         2
#define PERSONA_SYSTEM                          4
#define POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE      1
#define TROLLSTORE_PERSONA                      99
struct kpersona_info {
	uint32_t persona_info_version;
	uid_t persona_id;
	int persona_type;
	gid_t persona_gid;
	uint32_t persona_ngroups;
	gid_t persona_groups[16];
	uid_t persona_gmuid;
	char persona_name[256];
	uid_t persona_uid;
} __attribute__((packed));
_Static_assert(sizeof(struct kpersona_info) == 348, "kpersona_info");
extern int kpersona_alloc(struct kpersona_info *, uid_t *);
extern int kpersona_info(uid_t, struct kpersona_info *);

#define SHARE           "/redwood/share/trollstore"
#define BUNDLES         "/var/containers/Bundle/Application"

static void
say(const char *what)
{
	printf("tsboot: %s\n", what);
	fflush((void *)0);
}

static int
make_persona(void)
{
	struct kpersona_info info;
	char line[200];
	uid_t id = 0;
	char *p = (char *)&info;

	for (size_t i = 0; i < sizeof(info); i++) {
		p[i] = 0;
	}
	info.persona_info_version = PERSONA_INFO_V2;
	if (kpersona_info(TROLLSTORE_PERSONA, &info) == 0) {
		snprintf(line, sizeof(line), "persona %u is there (type %d, '%s')", TROLLSTORE_PERSONA, info.persona_type,
		    info.persona_name);
		say(line);
		return 0;
	}
	for (size_t i = 0; i < sizeof(info); i++) {
		p[i] = 0;
	}
	info.persona_info_version = PERSONA_INFO_V2;
	info.persona_id = TROLLSTORE_PERSONA;
	info.persona_type = PERSONA_SYSTEM;
	info.persona_uid = TROLLSTORE_PERSONA;
	snprintf(info.persona_name, sizeof(info.persona_name), "_hackios_trollstore");
	if (kpersona_alloc(&info, &id) != 0) {
		snprintf(line, sizeof(line), "kpersona_alloc(%u) FAILED: %s (the entitlement com.apple.private.persona-mgmt "
		    "is in tsboot's signature?)", TROLLSTORE_PERSONA, strerror(*__error()));
		say(line);
		return -1;
	}
	snprintf(line, sizeof(line), "persona %u made (got id %u): TrollStore can start its root helper now",
	    TROLLSTORE_PERSONA, id);
	say(line);
	return 0;
}

/* The way TrollStore itself starts the helper: persona 99, uid 0, gid 0. Returns the exit status or -1. */
static int
spawn_root(const char *path, char *const argv[])
{
	posix_spawnattr_t attr;
	pid_t pid;
	int status = 0, error;
	char line[400];

	posix_spawnattr_init(&attr);
	posix_spawnattr_set_persona_np(&attr, TROLLSTORE_PERSONA, POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE);
	posix_spawnattr_set_persona_uid_np(&attr, 0);
	posix_spawnattr_set_persona_gid_np(&attr, 0);
	error = posix_spawn(&pid, path, (void *)0, &attr, argv, *_NSGetEnviron());
	posix_spawnattr_destroy(&attr);
	if (error != 0) {
		snprintf(line, sizeof(line), "spawn %s %s: %s", path, argv[1], strerror(error));
		say(line);
		return -1;
	}
	if (waitpid(pid, &status, 0) != pid) {
		say("waitpid failed");
		return -1;
	}
	if ((status & 0177) != 0) {
		snprintf(line, sizeof(line), "%s %s killed by signal %d", argv[0], argv[1], status & 0177);
		say(line);
		return -1;
	}
	snprintf(line, sizeof(line), "%s %s: exit %d", argv[0], argv[1], (status >> 8) & 0xff);
	say(line);
	return (status >> 8) & 0xff;
}

/* the installed TrollStore's helper ("<bundle>/<uuid>/TrollStore.app/trollstorehelper"), or 0 */
static int
find_trollstore(char *helper, size_t size)
{
	DIR *dir = opendir(BUNDLES);
	struct dirent *entry;
	char marker[1200];
	int found = 0;

	if (dir == (DIR *)0) {
		return 0;
	}
	while (!found && (entry = readdir(dir)) != (struct dirent *)0) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		snprintf(marker, sizeof(marker), BUNDLES "/%s/_TrollStore", entry->d_name);
		if (access(marker, 0) == 0) {
			snprintf(helper, size, BUNDLES "/%s/TrollStore.app/trollstorehelper", entry->d_name);
			found = access(helper, 0) == 0;
		}
	}
	closedir(dir);
	return found;
}

/* our own apps, put next to TrollStore in the image (/redwood/share/trollstore/*.ipa): installed (again) by its helper
 * at every boot, so that a new build of them is on the phone after the next boot */
static void
install_apps(const char *helper)
{
	static const char *const apps[] = { SHARE "/GPUStress.ipa" };
	char line[300];

	for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
		if (access(apps[i], 0) != 0) {
			continue;
		}
		/* (the helper takes the path as its LAST argument: a "force" after it was read as the path, exit 166) */
		char *argv[] = { "trollstorehelper", "install", (char *)apps[i], (char *)0 };
		snprintf(line, sizeof(line), "installing %s", apps[i]);
		say(line);
		(void)spawn_root(helper, argv);
	}
}

int
main(void)
{
	char helper[1200], line[1400];
	int tries;

	make_persona();
	if (fork() != 0) {
		return 0;               /* the job list goes on; the rest happens in the background */
	}
	setsid();
	/* LaunchServices (lsd) and containermanagerd must be up: they are, well before SpringBoard's first frame */
	sleep(40);
	for (tries = 0; tries < 3; tries++) {
		if (find_trollstore(helper, sizeof(helper))) {
			char *argv[] = { "trollstorehelper", "refresh", (char *)0 };
			snprintf(line, sizeof(line), "TrollStore installed: %s; re-registering its apps", helper);
			say(line);
			if (spawn_root(helper, argv) == 0) {
				say("done: TrollStore and its apps are registered");
				install_apps(helper);
				return 0;
			}
		} else {
			char *argv[] = { "trollstorehelper", "install-trollstore", SHARE "/TrollStore.tar", (char *)0 };
			say("no TrollStore in " BUNDLES ": installing it from " SHARE "/TrollStore.tar");
			if (spawn_root(SHARE "/trollstorehelper", argv) == 0 && find_trollstore(helper, sizeof(helper))) {
				snprintf(line, sizeof(line), "done: TrollStore installed (%s): its icon is on the home screen", helper);
				say(line);
				install_apps(helper);
				return 0;
			}
		}
		sleep(30);
	}
	say("gave up after 3 tries (the lines above say why)");
	return 1;
}
