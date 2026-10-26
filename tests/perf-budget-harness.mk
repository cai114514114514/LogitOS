# tests/perf-budget-harness.mk -- host perf-budget harness (tests/unit/bench_budget.h).
# The selftest checks that a 3x slower candidate FAILs a 1.5x ratio budget, an
# equal candidate passes 1.3x, and bb_count(5,4) fails. The control compiles
# bb_ratio to always pass, so the selftest's "FAIL expected-3x-to-fail" fires.
# Later items append "test-gfx-budget: test-<id>-budget" from their fragments.

.PHONY: test-perf-budget-harness test-perf-budget-harness-negctl test-gfx-budget

test-perf-budget-harness-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -Wall -Wextra -Itests/unit -DBB_NEGCTL_ALWAYS_PASS \
		-o $(BUILD)/bench_budget_selftest_negctl tests/unit/bench_budget_selftest.c
	@if $(BUILD)/bench_budget_selftest_negctl > $(BUILD)/bench_budget_selftest_negctl.log 2>&1; then \
		cat $(BUILD)/bench_budget_selftest_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: selftest passes with bb_ratio always ok"; exit 1; \
	elif grep -q '^FAIL expected-3x-to-fail' $(BUILD)/bench_budget_selftest_negctl.log; then \
		echo "negative control ok: $$(grep '^FAIL' $(BUILD)/bench_budget_selftest_negctl.log | head -1)"; \
	else \
		cat $(BUILD)/bench_budget_selftest_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON"; exit 1; \
	fi

test-perf-budget-harness: test-perf-budget-harness-negctl
	@mkdir -p $(BUILD)
	@$(CC) -O1 -Wall -Wextra -Itests/unit \
		-o $(BUILD)/bench_budget_selftest tests/unit/bench_budget_selftest.c
	@$(BUILD)/bench_budget_selftest

test-gfx-budget: test-perf-budget-harness

ci-host: test-gfx-budget
