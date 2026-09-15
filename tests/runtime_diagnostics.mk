# Opt-in metadata diagnostics: build into a fresh BUILD directory so normal
# and instrumented browser objects never share a stale compiler-flag result.
ifeq ($(RUNTIME_DIAGNOSTICS),1)
$(BUILD)/jsobj/c/apps/browser/js_worker.o $(BUILD)/jsobj/c/apps/browser/js_webapi.o $(BUILD)/jsobj/c/apps/browser/js_wasm.o: JS_CF += -DJS_RUNTIME_DIAGNOSTICS
endif
$(BUILD)/jsobj/c/apps/browser/js_webapi.o: c/apps/browser/js_runtime_diagnostics.inc
$(BUILD)/jsobj/c/apps/browser/js_worker.o: c/apps/browser/js_worker_promise_diagnostics.inc

RD_DIR = $(BUILD)/runtime-diagnostics
$(RD_DIR)/worker-on $(RD_DIR)/worker-off: c/apps/browser/js_worker_promise_diagnostics.inc
RD_XHR_SRC = tests/unit/xhr_diagnostics_test.c $(STREAM_TEST_SRC) $(QJS_SRC)
RD_XHR_DEP = $(RD_XHR_SRC) tests/unit/xhr_progress_test.c tests/unit/stream_net.h c/apps/browser/js_runtime_diagnostics.inc tests/runtime_diagnostics.mk $(RUST_LIB_HOST)
$(RD_DIR)/xhr-on: $(RD_XHR_DEP)
	@mkdir -p $(RD_DIR)
	@$(CC) $(XHR_PROGRESS_CF) -DJS_RUNTIME_DIAGNOSTICS -o $@ $(RD_XHR_SRC) $(RUST_LIB_HOST) -lm
$(RD_DIR)/xhr-off: $(RD_XHR_DEP)
	@mkdir -p $(RD_DIR)
	@$(CC) $(XHR_PROGRESS_CF) -o $@ $(RD_XHR_SRC) $(RUST_LIB_HOST) -lm
$(RD_DIR)/worker-on: $(WORKER_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) tests/runtime_diagnostics.mk
	@mkdir -p $(RD_DIR)
	@$(CC) -O2 -w $(WORKER_CF) -DJS_RUNTIME_DIAGNOSTICS -o $@ $(WORKER_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(RD_DIR)/worker-off: $(WORKER_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) tests/runtime_diagnostics.mk
	@mkdir -p $(RD_DIR)
	@$(CC) -O2 -w $(WORKER_CF) -o $@ $(WORKER_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-runtime-diagnostics test-runtime-diagnostics-negctl
test-runtime-diagnostics-negctl: $(RD_DIR)/xhr-off $(RD_DIR)/worker-off
	@$(RD_DIR)/xhr-off > $(RD_DIR)/xhr-off.log 2>&1
	@$(RD_DIR)/worker-off > $(RD_DIR)/worker-off.log 2>&1
	@! grep -q '^\[runtime-diag\]' $(RD_DIR)/xhr-off.log
	@! grep -q '^\[runtime-diag\]' $(RD_DIR)/worker-off.log
	@rc=0; python3 tests/unit/runtime_diagnostics_check.py $(RD_DIR)/xhr-off.log $(RD_DIR)/worker-off.log > $(RD_DIR)/off-check.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL: xhr diagnostic metadata observable' $(RD_DIR)/off-check.log && grep -q '^FAIL: worker startup failure observable' $(RD_DIR)/off-check.log
test-runtime-diagnostics: test-runtime-diagnostics-negctl $(RD_DIR)/xhr-on $(RD_DIR)/worker-on
	@$(RD_DIR)/xhr-on > $(RD_DIR)/xhr-on.log 2>&1
	@$(RD_DIR)/worker-on > $(RD_DIR)/worker-on.log 2>&1
	@python3 tests/unit/runtime_diagnostics_check.py $(RD_DIR)/xhr-on.log $(RD_DIR)/worker-on.log
ci-host: test-runtime-diagnostics

# The old control removes only the Promise tracker. Both binaries must still
# execute every local callback and leave getters/Proxy traps at zero.
RD_PROMISE_SRC = tests/unit/worker_promise_diagnostics_test.c $(filter-out tests/unit/worker_test.c,$(WORKER_TEST_SRC))
RD_PROMISE_DEP = $(RD_PROMISE_SRC) tests/unit/worker_test.c c/apps/browser/js_worker_promise_diagnostics.inc tests/runtime_diagnostics.mk $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(RD_DIR)/promise-on: $(RD_PROMISE_DEP)
	@mkdir -p $(RD_DIR)
	@$(CC) -O2 -w $(WORKER_CF) -DJS_RUNTIME_DIAGNOSTICS -o $@ $(RD_PROMISE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(RD_DIR)/promise-old: $(RD_PROMISE_DEP)
	@mkdir -p $(RD_DIR)
	@$(CC) -O2 -w $(WORKER_CF) -DJS_RUNTIME_DIAGNOSTICS -DJS_WORKER_NO_PROMISE_DIAGNOSTICS -o $@ $(RD_PROMISE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(RD_DIR)/promise-san: $(RD_PROMISE_DEP)
	@mkdir -p $(RD_DIR)
	@$(CC) -O1 -g -w $(WORKER_CF) -DJS_RUNTIME_DIAGNOSTICS -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(RD_PROMISE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-worker-promise-diagnostics test-worker-promise-diagnostics-negctl test-worker-promise-diagnostics-san
test-worker-promise-diagnostics-negctl: $(RD_DIR)/promise-old
	@$(RD_DIR)/promise-old > $(RD_DIR)/promise-old.log 2>&1
	@rc=0; python3 tests/unit/worker_promise_diagnostics_check.py $(RD_DIR)/promise-old.log > $(RD_DIR)/promise-old-check.log 2>&1 || rc=$$?; cat $(RD_DIR)/promise-old-check.log; test $$rc -eq 1 && test "$$(grep -c '^FAIL:' $(RD_DIR)/promise-old-check.log)" -eq 8 && grep -q '^FAIL: worker promise rejection metadata observable$$' $(RD_DIR)/promise-old-check.log
test-worker-promise-diagnostics: test-worker-promise-diagnostics-negctl $(RD_DIR)/promise-on
	@$(RD_DIR)/promise-on > $(RD_DIR)/promise-on.log 2>&1
	@cat $(RD_DIR)/promise-on.log
	@python3 tests/unit/worker_promise_diagnostics_check.py $(RD_DIR)/promise-on.log
test-worker-promise-diagnostics-san: test-worker-promise-diagnostics $(RD_DIR)/promise-san
	@ASAN_OPTIONS=detect_leaks=0 $(RD_DIR)/promise-san > $(RD_DIR)/promise-san.log 2>&1
	@python3 tests/unit/worker_promise_diagnostics_check.py $(RD_DIR)/promise-san.log
test-runtime-diagnostics: test-worker-promise-diagnostics
