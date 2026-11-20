/*
 * bh_notify: what an iPhone's own daemons tell Settings > Battery Health, here where nothing does (shennong).
 *
 * powerd (BatteryTimeRemaining.m) waits for the notify states com.apple.system.batteryHealth.UPOAware and
 * .p0Threshold, and without them calls the battery's health "not determinable" (service state 5).
 * UPOAware "NO" (0x4e4f, kBatteryHealthWithoutUPO: no unexpected-power-off check, as on models that have
 * none) and a P0 threshold no battery reaches (its peak-power check never trips).
 *
 * Settings' page (BatteryUsageUI.bundle, +[BatteryUIResourceClass batteryDataUnavailable]) also wants the
 * peak performance state, which thermalmonitord (not run here) keeps in com.apple.thermalmonitor.
 * ageAwareMitigationState: 1, 2 or 3, anything else and the page turns its spinner for ever. 1: performance
 * management applied after an unexpected shutdown, 2: none, "normal peak performance", 3: turned off by the
 * user (PPC_PERFMGMT_ENABLED, _DISABLED, _USER_DISABLED). 2.
 *
 * It stays (in the background, the job list goes on): notifyd forgets a name's state once nobody holds the name,
 * and powerd registers for its names only when it sets up the battery's health, after the battery has appeared
 * -- which is later than this runs. So the names are held, and set and posted again every 30 seconds -- by a
 * child that registers them itself: a forked child cannot use its parent's registrations (libnotify starts
 * afresh in it), and the parent's go when it exits. What notifyd then holds is read back once and printed.
 */
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
extern int printf(const char *, ...);
extern int fflush(void *);

extern uint32_t notify_register_check(const char *name, int *out_token);
extern uint32_t notify_set_state(int token, uint64_t state);
extern uint32_t notify_post(const char *name);
extern uint32_t notify_get_state(int token, uint64_t *state);
extern int fork(void);
extern unsigned int sleep(unsigned int);
extern int setsid(void);

/* the page's other question, printed once: does this model check its battery is genuine? (MobileGestalt; yes
 * would need an AppleAuthCPRelay under IOPMPowerSource, with isTrustedForUI) */
typedef const void *CFTypeRef;
extern CFTypeRef CFStringCreateWithCString(CFTypeRef alloc, const char *s, uint32_t encoding);
extern unsigned char MGGetBoolAnswer(CFTypeRef question);

static struct {
	const char *name;
	uint64_t state;
	int token;
} names[] = {
	{ "com.apple.system.batteryHealth.UPOAware", 0x4e4f, -1 },
	{ "com.apple.system.batteryHealth.p0Threshold", 0x7fffffffULL, -1 },
	{ "com.apple.thermalmonitor.ageAwareMitigationState", 2, -1 },
};
#define NNAMES (int)(sizeof(names) / sizeof(names[0]))

static int
set(int i, int quiet)
{
	uint32_t r = 0;

	if (names[i].token == -1) {
		r = notify_register_check(names[i].name, &names[i].token);
	}
	if (r == 0) {
		r = notify_set_state(names[i].token, names[i].state);
	}
	if (r == 0) {
		r = notify_post(names[i].name);
	}
	if (!quiet || r != 0) {
		printf("bh_notify: %s = %#llx: %s (%u)\n", names[i].name, (unsigned long long)names[i].state,
		    r == 0 ? "set" : "FAILED", r);
	}
	return r == 0 ? 0 : 1;
}

int
main(void)
{
	int bad = 0;

	if (fork() != 0) {
		sleep(1);       /* (the child's lines first, in the job's log) */
		return 0;
	}
	setsid();       /* (out of the job's process group: launchd ends that group when the job's list is done) */
	for (int i = 0; i < NNAMES; i++) {
		bad |= set(i, 0);
	}
	printf("bh_notify: read back:");
	for (int i = 0; i < NNAMES; i++) {
		uint64_t v = 0;

		notify_get_state(names[i].token, &v);
		printf(" %#llx", (unsigned long long)v);
	}
	printf("; held from now on\n");
	printf("bh_notify: genuine battery check on this model (MobileGestalt D6/BMDrlb8V3WSiqL8gL+w): %d\n",
	    MGGetBoolAnswer(CFStringCreateWithCString(0, "D6/BMDrlb8V3WSiqL8gL+w", 0x08000100 /* UTF-8 */)));
	fflush(0);      /* (the console is not a terminal: nothing would come out of a process that never ends) */
	for (;;) {
		sleep(30);
		for (int i = 0; i < NNAMES; i++) {
			set(i, 1);
		}
		fflush(0);
	}
	return bad;
}
