# Cancellation is exercised through the same network/Fetch source group as
# test-webapi. No second copy of the dependency list may silently omit it.
ABORT_DIR := $(BUILD)/abort
ABORT_SRC := tests/unit/abort_test.c $(filter-out tests/unit/webapi_test.c,$(WEBAPI_TEST_SRC)) $(QJS_SRC)
ABORT_CF := -O1 -g -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
ABORT_DEPS := $(ABORT_SRC) tests/unit/stream_net.h $(BROWSER_INTERFACE_DEPS) tests/abort.mk $(RUST_LIB_HOST)

$(ABORT_DIR)/current: $(ABORT_DEPS)
	@mkdir -p $(ABORT_DIR)
	@$(CC) $(ABORT_CF) -o $@ $(ABORT_SRC) $(RUST_LIB_HOST) -lm

.PHONY: test-abort
test-abort: test-abort-negctl $(ABORT_DIR)/current
	@$(ABORT_DIR)/current > $(ABORT_DIR)/current.log 2>&1; rc=$$?; \
	 grep -E '^(FAIL|abort:)' $(ABORT_DIR)/current.log; exit $$rc
ci-host: test-abort

$(ABORT_DIR)/early: $(ABORT_DEPS)
	@mkdir -p $(ABORT_DIR)
	@$(CC) $(ABORT_CF) -DABORT_NOTIFY_EARLY -o $@ $(ABORT_SRC) $(RUST_LIB_HOST) -lm

# A plausible implementation notifies the source and then updates dependents.
# It must fail only the state-observation check; crashes or unrelated failures
# do not count as evidence that this ordering boundary is being tested.
.PHONY: test-abort-negctl
test-abort-negctl: $(ABORT_DIR)/early
	@rc=0; $(ABORT_DIR)/early > $(ABORT_DIR)/early.log 2>&1 || rc=$$?; \
	 count=$$(grep -c '^FAIL:' $(ABORT_DIR)/early.log); \
	 if [ $$rc -eq 1 ] && [ $$count -eq 1 ] && \
	    grep -qx 'FAIL: all dependent states settle before source listeners run' $(ABORT_DIR)/early.log; then \
	   echo 'abort negative control: source listener observed pending dependents (1 expected failure)'; \
	 else cat $(ABORT_DIR)/early.log; echo 'abort negative control failed'; exit 1; fi

$(ABORT_DIR)/san: $(ABORT_DEPS)
	@mkdir -p $(ABORT_DIR)
	@$(CC) $(ABORT_CF) -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(ABORT_SRC) $(RUST_LIB_HOST) -lm
.PHONY: test-abort-san
test-abort-san: test-abort $(ABORT_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 $(ABORT_DIR)/san > $(ABORT_DIR)/san.log 2>&1; rc=$$?; \
	 grep -E '^(FAIL|abort:)|ERROR: AddressSanitizer|runtime error' $(ABORT_DIR)/san.log; exit $$rc
ci-host: test-abort-san

# Replace only the cancellation producer in the shipping link. Compiling the
# whole browser with a test switch could accidentally change unrelated paths.
ABORT_GUEST_OBJ := $(ABORT_DIR)/early.o
$(ABORT_GUEST_OBJ): c/apps/gui/browser/js/net/abort.c c/apps/gui/browser/js/net/abort.h
	@mkdir -p $(ABORT_DIR)
	$(COMPILE) $(BROWSER_JS_CF) -DABORT_NOTIFY_EARLY -c $< -o $@

ABORT_GUEST_JS = $(filter-out $(BUILD)/jsobj/c/apps/gui/browser/js/net/abort.o,$(BROWSER_JS_OBJ)) $(ABORT_GUEST_OBJ)
ABORT_GUEST_LINK = $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(ABORT_GUEST_JS) $(BROWSER_OBJ) \
                   $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o
$(ABORT_DIR)/browser-early.elf: $(ABORT_GUEST_LINK)
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(ABORT_GUEST_LINK) --end-group
$(ABORT_DIR)/browser-early.aex: $(ABORT_DIR)/browser-early.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ Browser - 'B' 120 130 240 --stack-pages 2048

.PHONY: test-abort-guest test-abort-guest-negctl
test-abort-guest-negctl: test-abort-negctl $(ISO) $(DISK) $(ABORT_DIR)/browser-early.aex
	$(MAKE) BUILD=$(BUILD) DISK=$(ABORT_DIR)/disk-early.img BROWSER_AEX=$(ABORT_DIR)/browser-early.aex $(ABORT_DIR)/disk-early.img
	python3 tests/qmp/qmp_abort.py $(ISO) $(ABORT_DIR)/disk-early.img --expect-order-failure --out $(ABORT_DIR)/guest-early-$$(date +%s)
test-abort-guest: test-abort-guest-negctl
	python3 tests/qmp/qmp_abort.py $(ISO) $(DISK) --out $(ABORT_DIR)/guest-current-$$(date +%s)
ci-boot: test-abort-guest
