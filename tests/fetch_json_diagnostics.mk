# Narrow diagnostic acceptance over real Fetch and HTTP streaming, never a
# synthetic Response used as evidence of a network consumer. The flag-off
# binary must retain every functional result and fail only observability.
FETCH_JSON_DIAG_DIR = $(BUILD)/fetch-json-diagnostics
FETCH_JSON_DIAG_SRC = tests/unit/fetch_json_diagnostics_test.c $(STREAM_TEST_SRC) $(QJS_SRC)
FETCH_JSON_DIAG_DEP = $(FETCH_JSON_DIAG_SRC) tests/unit/stream_net.h $(wildcard c/apps/browser/js_fetch_*prelude.inc) c/apps/browser/js_runtime_diagnostics.inc tests/unit/fetch_json_diagnostics_check.py tests/fetch_json_diagnostics.mk $(RUST_LIB_HOST)
$(FETCH_JSON_DIAG_DIR)/on: $(FETCH_JSON_DIAG_DEP)
	@mkdir -p $(FETCH_JSON_DIAG_DIR)
	@$(CC) $(XHR_PROGRESS_CF) -DJS_RUNTIME_DIAGNOSTICS -o $@ $(FETCH_JSON_DIAG_SRC) $(RUST_LIB_HOST) -lm
$(FETCH_JSON_DIAG_DIR)/off: $(FETCH_JSON_DIAG_DEP)
	@mkdir -p $(FETCH_JSON_DIAG_DIR)
	@$(CC) $(XHR_PROGRESS_CF) -o $@ $(FETCH_JSON_DIAG_SRC) $(RUST_LIB_HOST) -lm
.PHONY: test-fetch-json-diagnostics test-fetch-json-diagnostics-negctl
test-fetch-json-diagnostics-negctl: $(FETCH_JSON_DIAG_DIR)/off
	@$(FETCH_JSON_DIAG_DIR)/off > $(FETCH_JSON_DIAG_DIR)/off.log 2>&1
	@rc=0; python3 tests/unit/fetch_json_diagnostics_check.py $(FETCH_JSON_DIAG_DIR)/off.log $(FETCH_JSON_DIAG_DIR)/off.log > $(FETCH_JSON_DIAG_DIR)/off-check.log 2>&1 || rc=$$?; test "$$rc" -eq 1 && grep -q '^AssertionError: missing Response.json validation diagnostics$$' $(FETCH_JSON_DIAG_DIR)/off-check.log
test-fetch-json-diagnostics: test-fetch-json-diagnostics-negctl $(FETCH_JSON_DIAG_DIR)/on
	@$(FETCH_JSON_DIAG_DIR)/on > $(FETCH_JSON_DIAG_DIR)/on.log 2>&1
	@python3 tests/unit/fetch_json_diagnostics_check.py $(FETCH_JSON_DIAG_DIR)/off.log $(FETCH_JSON_DIAG_DIR)/on.log
ci-host: test-fetch-json-diagnostics
