# Complete parent callback/checkpoint, then service its ready fetch before
# starting another worker's finite native call. All sockets are local vtables.
WORKER_PARENT_FETCH_DIR = $(BUILD)/worker-parent-fetch
WORKER_PARENT_FETCH_SRC = tests/unit/worker_parent_fetch_test.c tests/unit/worker_parent_fetch_net.c tests/unit/worker_fairness_runtime.c $(filter-out tests/unit/worker_test.c c/apps/browser/js_worker.c,$(WORKER_TEST_SRC))
WORKER_PARENT_FETCH_DEP = $(WORKER_PARENT_FETCH_SRC) c/apps/browser/js_worker.c c/apps/browser/js_worker.h c/apps/browser/js_webapi.h c/apps/browser/js_task_budget.h $(wildcard c/apps/browser/js_*prelude.inc) tests/unit/worker_test.c tests/unit/stream_net.h $(BUILD)/libcss_host.a $(RUST_LIB_HOST) tests/worker_parent_fetch.mk
$(WORKER_PARENT_FETCH_DIR)/current: $(WORKER_PARENT_FETCH_DEP)
	@mkdir -p $(WORKER_PARENT_FETCH_DIR)
	$(CC) -O2 -w $(WORKER_CF) -o $@ $(WORKER_PARENT_FETCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_PARENT_FETCH_DIR)/control: $(WORKER_PARENT_FETCH_DEP)
	@mkdir -p $(WORKER_PARENT_FETCH_DIR)
	$(CC) -O2 -w $(WORKER_CF) -DJS_TASK_NO_PARENT_FETCH_HANDOFF -o $@ $(WORKER_PARENT_FETCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(WORKER_PARENT_FETCH_DIR)/san: $(WORKER_PARENT_FETCH_DEP)
	@mkdir -p $(WORKER_PARENT_FETCH_DIR)
	$(CC) -O1 -g -w $(WORKER_CF) -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(WORKER_PARENT_FETCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-worker-parent-fetch test-worker-parent-fetch-negctl test-worker-parent-fetch-san
test-worker-parent-fetch-negctl: $(WORKER_PARENT_FETCH_DIR)/control
	@rc=0; $(WORKER_PARENT_FETCH_DIR)/control > $(WORKER_PARENT_FETCH_DIR)/control.log 2>&1 || rc=$$?; cat $(WORKER_PARENT_FETCH_DIR)/control.log; test $$rc -eq 1 && python3 tests/unit/worker_parent_fetch_check.py control $(WORKER_PARENT_FETCH_DIR)/control.log
test-worker-parent-fetch: test-worker-parent-fetch-negctl $(WORKER_PARENT_FETCH_DIR)/current
	@rc=0; $(WORKER_PARENT_FETCH_DIR)/current > $(WORKER_PARENT_FETCH_DIR)/current.log 2>&1 || rc=$$?; cat $(WORKER_PARENT_FETCH_DIR)/current.log; test $$rc -eq 0 && python3 tests/unit/worker_parent_fetch_check.py current $(WORKER_PARENT_FETCH_DIR)/current.log
test-worker-parent-fetch-san: test-worker-parent-fetch $(WORKER_PARENT_FETCH_DIR)/san
	@rc=0; ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(WORKER_PARENT_FETCH_DIR)/san > $(WORKER_PARENT_FETCH_DIR)/san.log 2>&1 || rc=$$?; cat $(WORKER_PARENT_FETCH_DIR)/san.log; test $$rc -eq 0 && python3 tests/unit/worker_parent_fetch_check.py current $(WORKER_PARENT_FETCH_DIR)/san.log
ci-host: test-worker-parent-fetch
