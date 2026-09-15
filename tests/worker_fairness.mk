# Finite normal callbacks with an injected monotonic clock. No busy loop,
# watchdog, native interruption or memory-failure scenario is involved.
WORKER_FAIR_DIR = $(BUILD)/worker-fairness
WORKER_FAIR_SRC = tests/unit/worker_fairness_test.c tests/unit/worker_fairness_runtime.c tests/unit/worker_fetch_net.c $(filter-out tests/unit/worker_test.c c/apps/browser/js_worker.c,$(WORKER_TEST_SRC))
WORKER_FAIR_DEP = $(WORKER_FAIR_SRC) c/apps/browser/js_worker.c c/apps/browser/js_worker.h c/apps/browser/js_task_budget.h tests/unit/worker_test.c tests/unit/worker_fetch_net.h tests/unit/stream_net.h $(BUILD)/libcss_host.a $(RUST_LIB_HOST) tests/worker_fairness.mk
$(WORKER_FAIR_DIR)/current: $(WORKER_FAIR_DEP)
	@mkdir -p $(WORKER_FAIR_DIR)
	$(CC) -O2 -w $(WORKER_CF) -o $@ $(WORKER_FAIR_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_FAIR_DIR)/unbounded: $(WORKER_FAIR_DEP)
	@mkdir -p $(WORKER_FAIR_DIR)
	# Restore the old complete batch: a separate parent-fetch handoff must not mask it.
	$(CC) -O2 -w $(WORKER_CF) -DJS_TASK_UNBOUNDED_TURN -DJS_TASK_NO_PARENT_FETCH_HANDOFF -o $@ $(WORKER_FAIR_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_FAIR_DIR)/hidden-jobs: $(WORKER_FAIR_DEP)
	@mkdir -p $(WORKER_FAIR_DIR)
	$(CC) -O2 -w $(WORKER_CF) -DJS_TASK_HIDE_PENDING_JOBS -o $@ $(WORKER_FAIR_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_FAIR_DIR)/no-parent-sweep: $(WORKER_FAIR_DEP)
	@mkdir -p $(WORKER_FAIR_DIR)
	$(CC) -O2 -w $(WORKER_CF) -DJS_TASK_NO_PARENT_SWEEP -o $@ $(WORKER_FAIR_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-worker-fairness test-worker-fairness-negctl test-worker-fairness-negctl-wake test-worker-fairness-negctl-parent
test-worker-fairness-negctl: $(WORKER_FAIR_DIR)/unbounded
	@rc=0; $(WORKER_FAIR_DIR)/unbounded > $(WORKER_FAIR_DIR)/unbounded.log 2>&1 || rc=$$?; cat $(WORKER_FAIR_DIR)/unbounded.log; test $$rc -eq 1 && python3 tests/unit/worker_fairness_check.py unbounded $(WORKER_FAIR_DIR)/unbounded.log
test-worker-fairness-negctl-wake: $(WORKER_FAIR_DIR)/hidden-jobs
	@rc=0; $(WORKER_FAIR_DIR)/hidden-jobs > $(WORKER_FAIR_DIR)/hidden-jobs.log 2>&1 || rc=$$?; cat $(WORKER_FAIR_DIR)/hidden-jobs.log; test $$rc -eq 1 && python3 tests/unit/worker_fairness_check.py hidden-jobs $(WORKER_FAIR_DIR)/hidden-jobs.log
test-worker-fairness-negctl-parent: $(WORKER_FAIR_DIR)/no-parent-sweep
	@rc=0; $(WORKER_FAIR_DIR)/no-parent-sweep > $(WORKER_FAIR_DIR)/no-parent-sweep.log 2>&1 || rc=$$?; cat $(WORKER_FAIR_DIR)/no-parent-sweep.log; test $$rc -eq 1 && python3 tests/unit/worker_fairness_check.py no-parent-sweep $(WORKER_FAIR_DIR)/no-parent-sweep.log
test-worker-fairness: test-worker-fairness-negctl test-worker-fairness-negctl-wake test-worker-fairness-negctl-parent $(WORKER_FAIR_DIR)/current
	@rc=0; $(WORKER_FAIR_DIR)/current > $(WORKER_FAIR_DIR)/current.log 2>&1 || rc=$$?; cat $(WORKER_FAIR_DIR)/current.log; test $$rc -eq 0 && python3 tests/unit/worker_fairness_check.py current $(WORKER_FAIR_DIR)/current.log
ci-host: test-worker-fairness
