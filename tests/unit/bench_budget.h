/* tests/unit/bench_budget.h -- header-only host perf-budget helpers.
 *
 * Budgets compare new code against a reference measured IN THE SAME BINARY
 * (a ratio), or count deterministic quantities (syscalls, pixels). Never
 * compare against absolute times: they differ per machine and load.
 *
 *   BB_SLACK (env, >= 1, default 1) scales every ratio limit for noisy hosts.
 *   -DBB_NEGCTL_ALWAYS_PASS makes bb_ratio always report ok (negative control).
 */
#ifndef BENCH_BUDGET_H
#define BENCH_BUDGET_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct bb_stats { double median_ns, min_ns; };

static volatile unsigned long bb_sink;   /* defeats dead-code elimination */
static int bb_failed;

static inline double bb_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* One warm-up plus 9 timed trials of inner_reps calls to fn; per-call ns. */
static inline struct bb_stats bb_measure(void (*fn)(void *), void *arg, int inner_reps)
{
	enum { TRIALS = 9 };
	double t[TRIALS], tmp;
	struct bb_stats s;
	int i, j, r;

	if (inner_reps < 1)
		inner_reps = 1;
	for (r = 0; r < inner_reps; r++)    /* warm-up */
		fn(arg);
	for (i = 0; i < TRIALS; i++) {
		double t0 = bb_now_ns();
		for (r = 0; r < inner_reps; r++)
			fn(arg);
		t[i] = (bb_now_ns() - t0) / inner_reps;
		bb_sink += (unsigned long)i;
	}
	for (i = 1; i < TRIALS; i++)        /* insertion sort */
		for (j = i; j > 0 && t[j - 1] > t[j]; j--) {
			tmp = t[j]; t[j] = t[j - 1]; t[j - 1] = tmp;
		}
	s.min_ns = t[0];
	s.median_ns = t[TRIALS / 2];
	return s;
}

static inline double bb_slack(void)
{
	const char *e = getenv("BB_SLACK");
	double v = e ? atof(e) : 1.0;
	return v < 1.0 ? 1.0 : v;
}

/* cand/ref median ratio must be <= max_ratio * BB_SLACK. Returns 1 ok, 0 FAIL. */
static inline int bb_ratio(const char *name, struct bb_stats cand,
			   struct bb_stats ref, double max_ratio)
{
	double lim = max_ratio * bb_slack();
	double ratio = ref.median_ns > 0 ? cand.median_ns / ref.median_ns : 1e9;
	int ok = ratio <= lim;
#ifdef BB_NEGCTL_ALWAYS_PASS
	ok = 1;
#endif
	printf("BUDGET %s ratio=%.3f max=%.3f %s\n", name, ratio, lim, ok ? "ok" : "FAIL");
	if (!ok)
		bb_failed = 1;
	return ok;
}

/* Deterministic budget: got must be <= max. Returns 1 ok, 0 FAIL. */
static inline int bb_count(const char *name, long got, long max)
{
	int ok = got <= max;
	printf("BUDGET %s count=%ld max=%ld %s\n", name, got, max, ok ? "ok" : "FAIL");
	if (!ok)
		bb_failed = 1;
	return ok;
}

/* Forget recorded failures (selftest only: it asserts deliberate FAILs). */
static inline void bb_reset(void) { bb_failed = 0; }

/* Nonzero if any budget failed. */
static inline int bb_exit(void) { return bb_failed ? 1 : 0; }

#endif
