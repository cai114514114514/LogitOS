/* Selftest for bench_budget.h. Expected-to-fail budgets are asserted through
 * their return value and then forgotten with bb_reset(). */
#include "bench_budget.h"

#define N 200000

static void spin(void *arg)
{
	unsigned long n = *(unsigned long *)arg, a = 1, i;
	for (i = 0; i < n; i++)
		a = a * 6364136223846793005UL + i;   /* dependent chain */
	bb_sink += a;
}

static int checks_failed;
#define CHECK(label, cond) do { if (!(cond)) { printf("FAIL %s\n", label); checks_failed++; } } while (0)

int main(void)
{
	unsigned long n1 = N, n3 = 3 * N;
	struct bb_stats ref = bb_measure(spin, &n1, 20);
	struct bb_stats same = bb_measure(spin, &n1, 20);
	struct bb_stats slow = bb_measure(spin, &n3, 20);
	int r;

	r = bb_ratio("selftest-3x-vs-1.5x", slow, ref, 1.5);
	CHECK("expected-3x-to-fail", r == 0);
	bb_reset();

	r = bb_ratio("selftest-equal-vs-1.3x", same, ref, 1.3);
	CHECK("equal-should-pass", r == 1);

	r = bb_count("selftest-count-5-vs-4", 5, 4);
	CHECK("expected-count-to-fail", r == 0);
	bb_reset();
	r = bb_count("selftest-count-4-vs-4", 4, 4);
	CHECK("count-at-limit-should-pass", r == 1);

	CHECK("bb_exit-clean", bb_exit() == 0);
	bb_failed = 1;
	CHECK("bb_exit-nonzero", bb_exit() != 0);
	bb_reset();

	if (checks_failed)
		return 1;
	puts("perf-budget-harness selftest ok");
	return bb_exit();
}
