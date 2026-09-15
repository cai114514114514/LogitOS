# Ordinary fetch scheduling time: a delayed first pump and an actively
# serviced idle connection must remain different. No real clock waiting.
FETCH_INITIAL_DIR = $(BUILD)/fetch-initial-deadline
FETCH_INITIAL_SRC = tests/unit/fetch_initial_deadline_test.c $(STREAM_TEST_SRC) $(QJS_SRC)
FETCH_INITIAL_DEP = $(FETCH_INITIAL_SRC) tests/unit/stream_net.h $(wildcard c/apps/browser/*.inc c/apps/browser/*.h c/net/http/*.inc c/net/http/*.h) $(RUST_LIB_HOST) tests/fetch_initial_deadline.mk
FETCH_INITIAL_CF = -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
.SECONDEXPANSION:
$(FETCH_INITIAL_DIR)/current: $$(FETCH_INITIAL_DEP)
	@mkdir -p $(FETCH_INITIAL_DIR)
	$(CC) $(FETCH_INITIAL_CF) -o $@ $(FETCH_INITIAL_SRC) $(RUST_LIB_HOST) -lm
$(FETCH_INITIAL_DIR)/old-baseline: $$(FETCH_INITIAL_DEP)
	@mkdir -p $(FETCH_INITIAL_DIR)
	$(CC) $(FETCH_INITIAL_CF) -DWEBAPI_FETCH_NO_INITIAL_BASELINE -o $@ $(FETCH_INITIAL_SRC) $(RUST_LIB_HOST) -lm
.PHONY: test-fetch-initial-deadline test-fetch-initial-deadline-negctl
test-fetch-initial-deadline-negctl: $(FETCH_INITIAL_DIR)/old-baseline
	@rc=0; $(FETCH_INITIAL_DIR)/old-baseline > $(FETCH_INITIAL_DIR)/old-baseline.log 2>&1 || rc=$$?; cat $(FETCH_INITIAL_DIR)/old-baseline.log; test $$rc -eq 1 && python3 tests/unit/fetch_initial_deadline_check.py old $(FETCH_INITIAL_DIR)/old-baseline.log
test-fetch-initial-deadline: test-fetch-initial-deadline-negctl $(FETCH_INITIAL_DIR)/current
	@$(FETCH_INITIAL_DIR)/current > $(FETCH_INITIAL_DIR)/current.log 2>&1
	@cat $(FETCH_INITIAL_DIR)/current.log
	@python3 tests/unit/fetch_initial_deadline_check.py current $(FETCH_INITIAL_DIR)/current.log
ci-host: test-fetch-initial-deadline
