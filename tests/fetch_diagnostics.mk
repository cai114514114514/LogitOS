# SPDX-License-Identifier: MIT
# Use the production fetch and parser source authority. Keep the diagnostic
# removal control a prerequisite: HTTP behavior passing alone is insufficient.
FETCH_DIAG_DIR = $(BUILD)/fetch-diagnostics
FETCH_DIAG_SRC = tests/unit/fetch_diagnostics_test.c $(STREAM_TEST_SRC) $(QJS_SRC)
FETCH_DIAG_DEPS = $(FETCH_DIAG_SRC) tests/unit/stream_net.h $(wildcard c/apps/browser/*.inc c/apps/browser/*.h c/net/http/*.inc c/net/http/*.h) $(RUST_LIB_HOST)
FETCH_DIAG_CF = -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
.SECONDEXPANSION:
$(FETCH_DIAG_DIR)/current: $$(FETCH_DIAG_DEPS)
	@mkdir -p $(FETCH_DIAG_DIR)
	@$(CC) $(FETCH_DIAG_CF) -o $@ $(FETCH_DIAG_SRC) $(RUST_LIB_HOST) -lm
$(FETCH_DIAG_DIR)/without-diagnostics: $$(FETCH_DIAG_DEPS)
	@mkdir -p $(FETCH_DIAG_DIR)
	@$(CC) $(FETCH_DIAG_CF) -DWEBAPI_NO_FETCH_DIAGNOSTICS -o $@ $(FETCH_DIAG_SRC) $(RUST_LIB_HOST) -lm
.PHONY: test-fetch-diagnostics test-fetch-diagnostics-negctl test-fetch-diagnostics-guest
test-fetch-diagnostics-negctl: $(FETCH_DIAG_DIR)/without-diagnostics
	@$(FETCH_DIAG_DIR)/without-diagnostics > $(FETCH_DIAG_DIR)/without-diagnostics.log 2>&1
	@python3 tests/unit/fetch_diagnostics_run.py --negative-control --log $(FETCH_DIAG_DIR)/without-diagnostics.log
test-fetch-diagnostics: test-fetch-diagnostics-negctl $(FETCH_DIAG_DIR)/current
	@$(FETCH_DIAG_DIR)/current > $(FETCH_DIAG_DIR)/current.log 2>&1
	@python3 tests/unit/fetch_diagnostics_run.py --log $(FETCH_DIAG_DIR)/current.log
test-fetch-diagnostics-guest: test-fetch-diagnostics $(BUILD)/logit.iso $(BUILD)/disk.img
	@python3 tests/unit/fetch_diagnostics_run.py --guest --iso $(BUILD)/logit.iso --disk $(BUILD)/disk.img --out $(FETCH_DIAG_DIR)/guest
ci-host: test-fetch-diagnostics
ci-boot: test-fetch-diagnostics-guest
