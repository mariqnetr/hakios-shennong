/*
 * The Xiaomi 14 Pro's (shennong, SM8650) thermal service: the governor that Android's thermal engine and the
 * kernel's cpufreq governor would be.
 *
 * Why: cpufreq=1 (bsd/dev/redwood_cpufreq.c) puts every group of cores at the top of its table and nothing ever
 * lowered it again: the X4 at 3.3 GHz on its top voltage with the desktop idle, 90+ C on a touch, the battery at 46 C
 * after a while. The hardware's own limits management (LMh) only steps in near 95..110 C. This service:
 *
 *   1. speed by load (cpu_dvfs=1, the default), every 20 ms per group, as schedutil does: the row whose frequency is
 *      1.25 times what the busiest core of the group used; at 85% busy straight to the top that is allowed (a touch
 *      gets its speed at once); down only after 60 ms of wanting less;
 *   2. junction limits, every 100 ms: a group whose sensors pass thermal_cpu (80 C) is held a row below where it runs
 *      (two when 5 C past), the GPU's DCVS ceiling a speed lower past thermal_gpu (80 C); a row back up a second after
 *      being 5 C under. Heat of the rest of the package (DSPs, memory, modem, camera) past the limit + 5 slows every group;
 *   3. the battery as the skin's thermometer (the phone's skin sensors are behind the PMIC's ADC, not read here):
 *      every 10 s a "budget" step 0..10 goes up while the battery is at thermal_batt (40 C) or more, two steps from
 *      +2 C, and down from 2 C under. Each step takes 7% off every group's highest allowed frequency and one speed off
 *      the GPU's: the slow loop that keeps a long game at a temperature a hand and a battery can live with;
 *   4. past thermal_crit (105 C) on any sensor: every group at its lowest row and the GPU at its lowest at once;
 *   5. iOS's pressure level (kern.redwood.thermal_pressure, which /redwood/bin/thermald posts to apps as
 *      com.apple.system.thermalpressurelevel): moderate while held back, heavy from budget 4 or battery +3 C,
 *      trapping from budget 8 or battery +6 C, sleeping past thermal_crit or a battery of 50 C.
 *
 *   kern.redwood.thermal              the table: each group (load, row, MHz now / allowed / top, LMh), the GPU, battery,
 *                                     every sensor
 *   kern.redwood.thermal_pressure     0 nominal .. 4 sleeping
 *   kern.redwood.thermal_hottest      the hottest sensor, tenths of a degree
 *   kern.redwood.thermal_throttles    how many times a limit came down
 *   kern.redwood.thermal_running      1 while it runs
 *
 * Boot-args: thermal=0 off, thermal=2 watch and report only; thermal_cpu=, thermal_gpu=, thermal_batt=, thermal_crit=
 * (whole degrees); cpu_dvfs=0 every group at its allowed top (the old behaviour, limits still kept); thermal_log=S a
 * line every S seconds (10; 0 none). Only reads TSENS: its own thresholds stay as the firmware set them.
 *
 * Sensors (the phone's dt, thermal-zones; TSENS v2 as in shennong_adreno.c: status word 0xa0 + 4 n, low 12 bits signed
 * tenths, bit 21 valid): tsens0 0xc228000: 1-4 cpuss, 5-10 cores 2-4 (cpu-2-*), 11-14 cores 5-6 (cpu-1-0/1-*);
 * tsens1 0xc229000: 1-3 core 7, the X4 (cpu-1-2-*), 4-5 cores 0-1, 6-11 NSP, 12 video, 13 DDR, 14-15 camera;
 * tsens2 0xc22a000: 1-8 GPU, 9-12 modem.
 */
#ifndef SHENNONG_THERMAL_HOST
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#endif

#define TH_GROUPS               4               /* = EPSS domains: 0 A520 (cores 0-1), 1 A720 (5-6), 2 X4 (7), 3 A720 (2-4) */
#define TH_ROWS                 40
#define TH_TICK_MS              20
#define TH_LIMIT_TICKS          5               /* the junction limits every 100 ms */
#define TH_BUDGET_TICKS         500             /* the battery's budget every 10 s */
#define TH_BUDGET_MAX           6       /* 42% off the cores, six GPU speeds: no further for a battery alone */
#define TH_BUDGET_PERCENT       7
#define TH_INVALID              (-32768)

/* ---- the governor's decisions: no kernel in here (tools/shennong-thermal-test runs them on the computer) ---- */

struct th_group {
	uint32_t        rows;                   /* 0: not ours */
	uint32_t        khz[TH_ROWS];
	uint32_t        row;                    /* asked for now */
	uint32_t        cap;                    /* the junction limit's highest row */
	uint32_t        ceiling;                /* the budget's highest row */
	uint32_t        down_ticks;             /* ticks in a row that wanted less */
	uint32_t        down_want;              /* the most that was wanted meanwhile */
	uint32_t        cool_ticks;             /* limit ticks in a row 5 C under */
	uint32_t        load;                   /* the busiest core's share of the last tick, percent */
	int             temp;                   /* tenths; TH_INVALID none */
};

struct th_gov {
	int             cpu_limit, gpu_limit, batt_limit, crit;         /* tenths */
	bool            dvfs;
	struct th_group g[TH_GROUPS];
	uint32_t        gpu_most;               /* DCVS's own top index (0: no DCVS) */
	uint32_t        gpu_cap;                /* the junction limit's index */
	uint32_t        gpu_ceiling;            /* the budget's */
	uint32_t        gpu_cool_ticks;
	uint32_t        gpu_now;                /* the index DCVS has now */
	int             gpu_temp, other_temp, hottest;
	int             battery;                /* tenths; TH_INVALID none */
	int             battery_before;         /* at the budget's last look */
	uint32_t        budget;
	bool            emergency;
	uint32_t        throttles;
	int             level;                  /* iOS's pressure */
};

static uint32_t
th_min(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static uint32_t
th_allowed(const struct th_group *g)
{
	return th_min(g->cap, g->ceiling);
}

/* the budget's highest row: the top row's frequency less TH_BUDGET_PERCENT a step */
static uint32_t
th_ceiling_row(const struct th_group *g, uint32_t budget)
{
	uint64_t most = (uint64_t)g->khz[g->rows - 1] * (100 - TH_BUDGET_PERCENT * budget) / 100;
	uint32_t row = 0;

	for (uint32_t r = 0; r < g->rows; r++) {
		if (g->khz[r] <= most) {
			row = r;
		}
	}
	return row;
}

/*
 * A touch boost, as Android's perf HAL does it: while a finger is on the glass and touch_boost ms after (default 600,
 * the length of an app's opening zoom), every group runs at the top it is allowed and the GPU's DCVS at its ceiling,
 * so that what a touch starts does not wait for the speeds to climb. The limits above still hold: a boost never goes
 * past a ceiling the temperature set. Called from the touch driver's every finger report (shennong_synaptics.c).
 */
static uint64_t touch_boost_abs;                /* touch_boost in mach_absolute_time units; 0 off */
static volatile uint64_t touch_boost_until;

void
shennong_touch_boost(void)
{
	if (touch_boost_abs != 0) {
		touch_boost_until = mach_absolute_time() + touch_boost_abs;
	}
}

bool
shennong_touch_boosted(void)
{
	return touch_boost_abs != 0 && mach_absolute_time() < touch_boost_until;
}

static void
th_init(struct th_gov *t)
{
	for (int d = 0; d < TH_GROUPS; d++) {
		struct th_group *g = &t->g[d];

		g->cap = g->ceiling = g->rows ? g->rows - 1 : 0;
		g->row = g->cap;
		g->temp = TH_INVALID;
	}
	t->gpu_cap = t->gpu_ceiling = t->gpu_most;
	t->gpu_temp = t->other_temp = t->hottest = t->battery = t->battery_before = TH_INVALID;
}

/* every TH_TICK_MS: the row each group should run at, by its load and what is allowed */
static void
th_speed(struct th_gov *t)
{
	for (int d = 0; d < TH_GROUPS; d++) {
		struct th_group *g = &t->g[d];
		uint32_t allowed = th_allowed(g), want = allowed;

		if (g->rows == 0) {
			continue;
		}
		if (t->dvfs && g->load < 85 && !shennong_touch_boosted()) {
			uint64_t need = (uint64_t)g->khz[th_min(g->row, g->rows - 1)] * g->load * 125 / 10000;

			for (want = 0; want < allowed && g->khz[want] < need; want++) {
			}
		}
		want = th_min(want, allowed);
		if (want < g->row && g->row <= allowed) {
			/* wanting less: only after three ticks of it, and then the most of what those wanted */
			g->down_want = g->down_ticks ? (want > g->down_want ? want : g->down_want) : want;
			if (++g->down_ticks < 3) {
				continue;
			}
			want = g->down_want;
		}
		g->down_ticks = 0;
		g->row = want;
	}
}

/* every TH_LIMIT_TICKS: the junction limits; true when a limit came down */
static bool
th_limits(struct th_gov *t)
{
	bool down = false;
	bool package = t->other_temp != TH_INVALID && t->other_temp >= t->cpu_limit + 50;

	t->emergency = t->hottest != TH_INVALID && t->hottest >= t->crit;
	for (int d = 0; d < TH_GROUPS; d++) {
		struct th_group *g = &t->g[d];
		uint32_t from = g->cap, step = 0;

		if (g->rows == 0) {
			continue;
		}
		if (t->emergency) {
			down |= g->cap != 0;
			g->cap = 0;
			g->cool_ticks = 0;
			continue;
		}
		if (g->temp != TH_INVALID && g->temp >= t->cpu_limit + 50) {
			step = 2;
		} else if ((g->temp != TH_INVALID && g->temp >= t->cpu_limit) || package) {
			step = 1;
		}
		if (step) {
			/*
			 * A cap far above where the group runs changes nothing: it comes down to just above that row first
			 * (the heat may be a neighbour's, the group's own speed is no reason to cut it further), and from
			 * there a row or two a tick.
			 */
			uint32_t cap = g->row + 1 < g->cap ? g->row + 1 : from > step ? from - step : 0;

			down |= cap < g->cap;
			g->cap = cap;
			g->cool_ticks = 0;
		} else if (g->temp == TH_INVALID || g->temp < t->cpu_limit - 50) {
			/* back up a row a second after being 5 C under, then a row every half second */
			if (++g->cool_ticks >= 10 && g->cap < g->rows - 1) {
				g->cap++;
				g->cool_ticks = 5;
			}
		} else {
			g->cool_ticks = 0;
		}
	}
	if (t->gpu_most != 0) {
		uint32_t from = t->gpu_cap;

		if (t->emergency) {
			down |= t->gpu_cap != 1;
			t->gpu_cap = 1;
		} else if (t->gpu_temp != TH_INVALID && t->gpu_temp >= t->gpu_limit) {
			uint32_t step = t->gpu_temp >= t->gpu_limit + 50 ? 2 : 1;

			if (t->gpu_now + 1 < from) {
				t->gpu_cap = t->gpu_now + 1;    /* as for the cores: to just above where it runs first */
			} else {
				t->gpu_cap = from > step + 1 ? from - step : 1;
			}
			down |= t->gpu_cap < from;
			t->gpu_cool_ticks = 0;
		} else if (t->gpu_temp == TH_INVALID || t->gpu_temp < t->gpu_limit - 50) {
			if (++t->gpu_cool_ticks >= 10 && t->gpu_cap < t->gpu_most) {
				t->gpu_cap++;
				t->gpu_cool_ticks = 5;
			}
		} else {
			t->gpu_cool_ticks = 0;
		}
	}
	if (down) {
		t->throttles++;
	}
	return down;
}

/* every TH_BUDGET_TICKS: the battery's budget */
static void
th_budget(struct th_gov *t)
{
	/*
	 * The battery answers minutes late: a step only while it still goes the wrong way (or stays), not while the
	 * last steps are already taking it back -- else the budget swings far past where it should settle.
	 */
	if (t->battery != TH_INVALID) {
		bool rising = t->battery_before != TH_INVALID && t->battery > t->battery_before;
		bool falling = t->battery_before != TH_INVALID && t->battery < t->battery_before;

		if (t->battery >= t->batt_limit + 20 && !falling) {
			t->budget = th_min(t->budget + 2, TH_BUDGET_MAX);
		} else if (t->battery >= t->batt_limit && !falling) {
			t->budget = th_min(t->budget + 1, TH_BUDGET_MAX);
		} else if (t->battery <= t->batt_limit - 20 && !rising && t->budget > 0) {
			t->budget--;
		} else if (falling && t->battery < t->batt_limit + 10 && t->budget > 0) {
			/* (on its way back and nearly there: give a step back, else a battery that came in warm -- from a run
			 * before, or charging on the cable -- keeps the budget at its top for minutes after the SoC cooled) */
			t->budget--;
		}
		t->battery_before = t->battery;
	}
	for (int d = 0; d < TH_GROUPS; d++) {
		if (t->g[d].rows != 0) {
			t->g[d].ceiling = th_ceiling_row(&t->g[d], t->budget);
		}
	}
	if (t->gpu_most != 0) {
		t->gpu_ceiling = t->gpu_most > t->budget + 1 ? t->gpu_most - t->budget : 1;
	}
}

/* iOS's level: 0 nominal, 1 moderate, 2 heavy, 3 trapping, 4 sleeping */
static int
th_level(const struct th_gov *t)
{
	bool held = t->budget > 0 || (t->gpu_most != 0 && t->gpu_cap < t->gpu_most);
	int b = t->battery;

	for (int d = 0; d < TH_GROUPS; d++) {
		held |= t->g[d].rows != 0 && t->g[d].cap < t->g[d].rows - 1;
	}
	if (t->emergency || (b != TH_INVALID && b >= 500)) {
		return 4;
	}
	if (t->budget >= 6 || (b != TH_INVALID && b >= t->batt_limit + 60)) {
		return 3;
	}
	if (t->budget >= 3 || (b != TH_INVALID && b >= t->batt_limit + 30)) {
		return 2;
	}
	return held ? 1 : 0;
}

static uint32_t
th_gpu_allowed(const struct th_gov *t)
{
	return th_min(t->gpu_cap, t->gpu_ceiling);
}

#ifndef SHENNONG_THERMAL_HOST
/* ---- the service on the phone ---- */

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern int redwood_battery_temperature(void);                  /* bsd/dev/redwood_battery.c: whole degrees, -273 none */
extern uint32_t redwood_cpufreq_rows(uint32_t domain);         /* bsd/dev/redwood_cpufreq.c */
extern uint32_t redwood_cpufreq_khz(uint32_t domain, uint32_t row);
extern uint32_t redwood_cpufreq_row(uint32_t domain);
extern void redwood_cpufreq_set(uint32_t domain, uint32_t row);
extern uint32_t redwood_cpufreq_held_khz(uint32_t domain);
extern uint32_t redwood_cpufreq_domain_of(uint32_t phys_id);
extern void shennong_gpu_thermal_cap(uint32_t index);          /* bsd/dev/shennong_adreno.c */
extern uint32_t shennong_gpu_speed_khz(uint32_t index);
extern uint32_t shennong_gpu_speed_now(uint32_t *most);

static const struct th_sensor {
	const char *    name;
	uint8_t         block, sensor;
	int8_t          group;          /* 0..3 a group of cores; 4 the GPU; 5 the rest of the package */
} sensors[] = {
	{ "cpuss-0", 0, 1, 5 }, { "cpuss-1", 0, 2, 5 }, { "cpuss-2", 0, 3, 5 }, { "cpuss-3", 0, 4, 5 },
	{ "cpu2-0", 0, 5, 3 }, { "cpu2-1", 0, 6, 3 }, { "cpu3-0", 0, 7, 3 }, { "cpu3-1", 0, 8, 3 },
	{ "cpu4-0", 0, 9, 3 }, { "cpu4-1", 0, 10, 3 },
	{ "cpu5-0", 0, 11, 1 }, { "cpu5-1", 0, 12, 1 }, { "cpu6-0", 0, 13, 1 }, { "cpu6-1", 0, 14, 1 },
	{ "cpu7-0", 1, 1, 2 }, { "cpu7-1", 1, 2, 2 }, { "cpu7-2", 1, 3, 2 },
	{ "cpu0", 1, 4, 0 }, { "cpu1", 1, 5, 0 },
	{ "nsphvx-0", 1, 6, 5 }, { "nsphvx-1", 1, 7, 5 }, { "nsphmx-0", 1, 8, 5 }, { "nsphmx-1", 1, 9, 5 },
	{ "nsphmx-2", 1, 10, 5 }, { "nsphmx-3", 1, 11, 5 }, { "video", 1, 12, 5 }, { "ddr", 1, 13, 5 },
	{ "camera-0", 1, 14, 5 }, { "camera-1", 1, 15, 5 },
	{ "gpuss-0", 2, 1, 4 }, { "gpuss-1", 2, 2, 4 }, { "gpuss-2", 2, 3, 4 }, { "gpuss-3", 2, 4, 4 },
	{ "gpuss-4", 2, 5, 4 }, { "gpuss-5", 2, 6, 4 }, { "gpuss-6", 2, 7, 4 }, { "gpuss-7", 2, 8, 4 },
	{ "mdmss-0", 2, 9, 5 }, { "mdmss-1", 2, 10, 5 }, { "mdmss-2", 2, 11, 5 }, { "mdmss-3", 2, 12, 5 },
};
#define TH_SENSORS      (sizeof(sensors) / sizeof(sensors[0]))
static const vm_offset_t tsens_phys[3] = { 0x0c228000, 0x0c229000, 0x0c22a000 };
static vm_offset_t tsens[3];
static int16_t readings[TH_SENSORS];
static const char *const group_names[TH_GROUPS] = { "A520 0-1", "A720 5-6", "X4 7", "A720 2-4" };
static const char *const level_names[] = { "nominal", "moderate", "heavy", "trapping", "sleeping" };

static struct th_gov gov;
static bool acting;                     /* thermal=2: watch only */
static uint32_t log_seconds = 10;

static LCK_GRP_DECLARE(sn_thermal_lck_grp, "shennong-thermal");
static LCK_MTX_DECLARE(sn_thermal_mtx, &sn_thermal_lck_grp);
static char thermal_text[4096];
static int thermal_pressure, thermal_hottest = TH_INVALID, thermal_throttles, thermal_running;

SYSCTL_DECL(_kern_redwood);
SYSCTL_INT(_kern_redwood, OID_AUTO, thermal_pressure, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_pressure, 0,
    "iOS thermal pressure level: 0 nominal, 1 moderate, 2 heavy, 3 trapping, 4 sleeping");
SYSCTL_INT(_kern_redwood, OID_AUTO, thermal_hottest, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_hottest, 0,
    "the hottest sensor's reading, tenths of a degree Celsius");
SYSCTL_INT(_kern_redwood, OID_AUTO, thermal_throttles, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_throttles, 0,
    "how many times a limit came down");
SYSCTL_INT(_kern_redwood, OID_AUTO, thermal_running, CTLFLAG_RD | CTLFLAG_LOCKED, &thermal_running, 0,
    "1 while the thermal service runs");

static int
thermal_sysctl_text(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	static char copy[sizeof(thermal_text)];

	lck_mtx_lock(&sn_thermal_mtx);
	strlcpy(copy, thermal_running ? thermal_text : "thermal: not running (thermal=0, or not shennong)\n", sizeof(copy));
	lck_mtx_unlock(&sn_thermal_mtx);
	return SYSCTL_OUT(req, copy, strlen(copy) + 1);
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, thermal, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    thermal_sysctl_text, "A", "every temperature sensor and every group of cores, as a table");

static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[512];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

/* "80.3" from 803; "-" for none */
static const char *
tenths(int t, char *buf, size_t size)
{
	if (t == TH_INVALID) {
		strlcpy(buf, "-", size);
	} else {
		snprintf(buf, size, "%s%d.%d", t < 0 ? "-" : "", (t < 0 ? -t : t) / 10, (t < 0 ? -t : t) % 10);
	}
	return buf;
}

static int16_t
read_sensor(unsigned block, unsigned sensor)
{
	for (int i = 0; i < 4; i++) {
		uint32_t status = *(volatile uint32_t *)(tsens[block] + 0xa0 + 4 * sensor);

		if (status & (1u << 21)) {
			return (int16_t)((int32_t)(status << 20) >> 20);
		}
	}
	return TH_INVALID;
}

static void
read_sensors(void)
{
	int by[6] = { TH_INVALID, TH_INVALID, TH_INVALID, TH_INVALID, TH_INVALID, TH_INVALID }, hottest = TH_INVALID;

	for (unsigned i = 0; i < TH_SENSORS; i++) {
		int t = readings[i] = read_sensor(sensors[i].block, sensors[i].sensor);

		/* (a reading past 150 C or under -40 is a sensor caught mid-conversion, not heat) */
		if (t == TH_INVALID || t > 1500 || t < -400) {
			readings[i] = TH_INVALID;
			continue;
		}
		if (by[sensors[i].group] == TH_INVALID || t > by[sensors[i].group]) {
			by[sensors[i].group] = t;
		}
		if (hottest == TH_INVALID || t > hottest) {
			hottest = t;
		}
	}
	for (int d = 0; d < TH_GROUPS; d++) {
		gov.g[d].temp = by[d];
	}
	gov.gpu_temp = by[4];
	gov.other_temp = by[5];
	gov.hottest = hottest;
}

/* each group's busiest core over the last tick, from the scheduler's idle time */
static void
read_loads(void)
{
	static uint64_t idle_before[MAX_CPUS], then;
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t now = mach_absolute_time();
	uint32_t load[TH_GROUPS] = { 0 };

	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		uint64_t idle = 0, busy;
		uint32_t d = redwood_cpufreq_domain_of(topology->cpus[i].phys_id), percent;

		if (!ml_cpu_idle_time((int)i, &idle) || d >= TH_GROUPS) {
			continue;
		}
		if (then != 0 && now > then && idle >= idle_before[i]) {
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			percent = (uint32_t)(100 * busy / (now - then));
			load[d] = percent > load[d] ? percent : load[d];
		}
		idle_before[i] = idle;
	}
	then = now;
	for (int d = 0; d < TH_GROUPS; d++) {
		gov.g[d].load = load[d] > 100 ? 100 : load[d];
	}
}

static void
write_text(void)
{
	static char text[sizeof(thermal_text)];
	size_t n = 0;
	char a[16], b[16];

	n += snprintf(text + n, sizeof(text) - n, "pressure %s (%d), hottest %s C, battery %s C (limit %d), budget %u of %u, "
	    "limits came down %u times%s\n", level_names[gov.level], gov.level, tenths(gov.hottest, a, sizeof(a)),
	    tenths(gov.battery, b, sizeof(b)), gov.batt_limit / 10, gov.budget, TH_BUDGET_MAX, gov.throttles,
	    gov.emergency ? "; EMERGENCY: everything at its lowest" : acting ? "" : "; watching only (thermal=2)");
	for (int d = 0; d < TH_GROUPS && n < sizeof(text); d++) {
		struct th_group *g = &gov.g[d];

		if (g->rows == 0) {
			n += snprintf(text + n, sizeof(text) - n, "%-9s not raised (cpufreq=1, cpus=8)\n", group_names[d]);
			continue;
		}
		n += snprintf(text + n, sizeof(text) - n, "%-9s %5s C  load %3u%%  %4u MHz (row %u), allowed %u (heat %u, budget %u) "
		    "of %u, LMh %u\n", group_names[d], tenths(g->temp, a, sizeof(a)), g->load, g->khz[g->row] / 1000, g->row,
		    g->khz[th_allowed(g)] / 1000, g->khz[g->cap] / 1000, g->khz[g->ceiling] / 1000, g->khz[g->rows - 1] / 1000,
		    redwood_cpufreq_held_khz((uint32_t)d) / 1000);
	}
	if (gov.gpu_most != 0) {
		n += snprintf(text + n, sizeof(text) - n, "GPU       %5s C  %4u MHz, allowed %u (heat %u, budget %u) of %u\n",
		    tenths(gov.gpu_temp, a, sizeof(a)), shennong_gpu_speed_khz(shennong_gpu_speed_now(NULL)) / 1000,
		    shennong_gpu_speed_khz(th_gpu_allowed(&gov)) / 1000, shennong_gpu_speed_khz(gov.gpu_cap) / 1000,
		    shennong_gpu_speed_khz(gov.gpu_ceiling) / 1000, shennong_gpu_speed_khz(gov.gpu_most) / 1000);
	} else {
		n += snprintf(text + n, sizeof(text) - n, "GPU       %5s C  (no sgpu_dcvs=1: its speed is not governed)\n",
		    tenths(gov.gpu_temp, a, sizeof(a)));
	}
	n += snprintf(text + n, sizeof(text) - n, "limits: cores %d C, GPU %d C, battery %d C, all down at %d C; speed by load %s\n",
	    gov.cpu_limit / 10, gov.gpu_limit / 10, gov.batt_limit / 10, gov.crit / 10, gov.dvfs ? "on" : "off (cpu_dvfs=0)");
	for (unsigned i = 0; i < TH_SENSORS && n < sizeof(text); i++) {
		n += snprintf(text + n, sizeof(text) - n, "%-9s tsens%u/%-2u %6s C%s", sensors[i].name, sensors[i].block,
		    sensors[i].sensor, tenths(readings[i], a, sizeof(a)), i % 3 == 2 || i + 1 == TH_SENSORS ? "\n" : "   ");
	}
	lck_mtx_lock(&sn_thermal_mtx);
	memcpy(thermal_text, text, sizeof(thermal_text));
	thermal_text[sizeof(thermal_text) - 1] = 0;
	thermal_pressure = gov.level;
	thermal_hottest = gov.hottest;
	thermal_throttles = (int)gov.throttles;
	lck_mtx_unlock(&sn_thermal_mtx);
}

static void
log_line(const char *why)
{
	static const int order[TH_GROUPS] = { 2, 3, 1, 0 };     /* the X4 first: it is the one that heats */
	char line[400], a[16], b[16];
	size_t n = 0;

	for (int k = 0; k < TH_GROUPS && n < sizeof(line); k++) {
		struct th_group *g = &gov.g[order[k]];

		if (g->rows == 0) {
			continue;
		}
		n += snprintf(line + n, sizeof(line) - n, "%s%s %s C %u%% %u/%u MHz", n ? ", " : "", group_names[order[k]],
		    tenths(g->temp, a, sizeof(a)), g->load, g->khz[g->row] / 1000, g->khz[th_allowed(g)] / 1000);
	}
	tell("thermal: %s%s; gpu %s C %u/%u MHz; battery %s C, budget %u; %s, %u limits down\n", why, line,
	    tenths(gov.gpu_temp, a, sizeof(a)), shennong_gpu_speed_khz(shennong_gpu_speed_now(NULL)) / 1000,
	    shennong_gpu_speed_khz(gov.gpu_most ? th_gpu_allowed(&gov) : shennong_gpu_speed_now(NULL)) / 1000,
	    tenths(gov.battery, b, sizeof(b)), gov.budget, level_names[gov.level], gov.throttles);
}

static void
carry_out(void)
{
	if (!acting) {
		return;
	}
	for (int d = 0; d < TH_GROUPS; d++) {
		if (gov.g[d].rows != 0) {
			redwood_cpufreq_set((uint32_t)d, gov.g[d].row);
		}
	}
	if (gov.gpu_most != 0) {
		shennong_gpu_thermal_cap(th_gpu_allowed(&gov));
	}
}

static void
thermal_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t ticks = 0;
	bool was_emergency = false;

	for (;; ticks++) {
		if (gov.gpu_most == 0) {
			uint32_t most = 0;

			/* DCVS starts with the GPU, later than this service */
			(void)shennong_gpu_speed_now(&most);
			if (most != 0) {
				gov.gpu_most = gov.gpu_cap = gov.gpu_ceiling = most;
				th_budget(&gov);
			}
		}
		read_loads();
		if (ticks % TH_LIMIT_TICKS == 0) {
			gov.gpu_now = shennong_gpu_speed_now(NULL);
			read_sensors();
			(void)th_limits(&gov);
			if (gov.emergency != was_emergency) {
				log_line(gov.emergency ? "PAST THE CRITICAL TEMPERATURE, all at the lowest: " : "back under the critical temperature: ");
				was_emergency = gov.emergency;
			}
		}
		if (ticks % TH_BUDGET_TICKS == 0) {
			int b = redwood_battery_temperature();

			gov.battery = b <= -273 ? TH_INVALID : b * 10;
			th_budget(&gov);
		}
		th_speed(&gov);
		carry_out();
		if (ticks % TH_LIMIT_TICKS == 0) {
			int level = th_level(&gov);

			if (level != gov.level) {
				gov.level = level;
				log_line(level > 0 ? "pressure up: " : "pressure back to nominal: ");
			}
			write_text();
		}
		if (log_seconds != 0 && ticks % (log_seconds * 1000 / TH_TICK_MS) == 0) {
			log_line("");
		}
		delay_for_interval(TH_TICK_MS, NSEC_PER_MSEC);
	}
}

void
shennong_thermal_init(void)
{
	uint32_t mode = 1, cpu = 80, gpu = 80, batt = 45, crit = 105, dvfs = 1, raised = 0;
	thread_t thread;
	char a[16];

	if (strcmp(redwood_phone(), "shennong") != 0) {
		return;
	}
	PE_parse_boot_argn("thermal", &mode, sizeof(mode));
	if (mode == 0) {
		tell("thermal: off (thermal=0): nothing but the hardware's own limits keeps the cores cool\n");
		return;
	}
	PE_parse_boot_argn("thermal_cpu", &cpu, sizeof(cpu));
	PE_parse_boot_argn("thermal_gpu", &gpu, sizeof(gpu));
	PE_parse_boot_argn("thermal_batt", &batt, sizeof(batt));
	PE_parse_boot_argn("thermal_crit", &crit, sizeof(crit));
	PE_parse_boot_argn("cpu_dvfs", &dvfs, sizeof(dvfs));
	{
		uint32_t boost_ms = 600;

		PE_parse_boot_argn("touch_boost", &boost_ms, sizeof(boost_ms));
		nanoseconds_to_absolutetime((uint64_t)boost_ms * NSEC_PER_MSEC, &touch_boost_abs);
		tell("thermal: touch boost %u ms%s\n", boost_ms, boost_ms ? "" : " (off: touch_boost=0)");
	}
	PE_parse_boot_argn("thermal_log", &log_seconds, sizeof(log_seconds));
	gov.cpu_limit = (int)cpu * 10;
	gov.gpu_limit = (int)gpu * 10;
	gov.batt_limit = (int)batt * 10;
	gov.crit = (int)crit * 10;
	gov.dvfs = dvfs != 0;
	for (uint32_t d = 0; d < TH_GROUPS; d++) {
		struct th_group *g = &gov.g[d];

		g->rows = redwood_cpufreq_rows(d);
		if (g->rows > TH_ROWS) {
			g->rows = TH_ROWS;
		}
		for (uint32_t r = 0; r < g->rows; r++) {
			g->khz[r] = redwood_cpufreq_khz(d, r);
		}
		raised += g->rows != 0;
	}
	th_init(&gov);
	for (int d = 0; d < TH_GROUPS; d++) {
		gov.g[d].row = gov.g[d].rows ? redwood_cpufreq_row((uint32_t)d) : 0;
	}
	acting = mode == 1;
	for (int i = 0; i < 3; i++) {
		tsens[i] = ml_io_map(tsens_phys[i], 0x1000);
	}
	read_sensors();
	th_budget(&gov);
	gov.level = th_level(&gov);
	write_text();
	tell("thermal: %u groups of cores governed, %u sensors (hottest %s C); cores %u C, GPU %u C, battery %u C, all down "
	    "at %u C; speed by load %s%s\n", raised, (unsigned)TH_SENSORS, tenths(gov.hottest, a, sizeof(a)), cpu, gpu, batt,
	    crit, gov.dvfs ? "on" : "off", acting ? "" : "; watching only (thermal=2)");
	if (kernel_thread_start(thermal_thread, NULL, &thread) != KERN_SUCCESS) {
		tell("thermal: no thread: the cores are NOT governed\n");
		return;
	}
	thread_deallocate(thread);
	thermal_running = 1;
}
#endif /* SHENNONG_THERMAL_HOST */
