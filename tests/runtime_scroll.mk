# Extend the real loader pipeline with CSSOM's required bindings, without
# importing every optional browser JS subsystem or copying a second source list.
RUNTIME_SCROLL_SRC = $(filter-out tests/unit/loader_test.c,$(LOADER_SRC)) $(filter-out tests/unit/cssom_test.c $(LOADER_SRC),$(CSSOM_TEST_SRC)) tests/unit/runtime_scroll_test.c
RUNTIME_SCROLL_DEPS = $(RUNTIME_SCROLL_SRC) tests/unit/loader_test.c tests/unit/loaderhost/logit.h tests/unit/painthost/logit.h $(wildcard c/apps/browser/*.h c/apps/browser/*.inc) tests/unit/loader_fakebfetch.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
RUNTIME_SCROLL_CF = -O2 -w -DLOADER_POLL_HOOK $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
.PHONY: test-runtime-scroll test-runtime-scroll-negctl
$(BUILD)/runtime_scroll_test: $(RUNTIME_SCROLL_DEPS)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(RUNTIME_SCROLL_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/runtime_scroll_negctl: $(RUNTIME_SCROLL_DEPS)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_SCROLL_SYNC_EVENTS -o $@ $(RUNTIME_SCROLL_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-runtime-scroll-negctl: $(BUILD)/runtime_scroll_negctl
	@$(BUILD)/runtime_scroll_negctl > $(BUILD)/runtime_scroll_negctl.log 2>&1; rc=$$?; cat $(BUILD)/runtime_scroll_negctl.log; test $$rc -eq 1 && grep -q 'FAIL: scroll listener never reenters itself' $(BUILD)/runtime_scroll_negctl.log
test-runtime-scroll: test-runtime-scroll-negctl $(BUILD)/runtime_scroll_test
	$(BUILD)/runtime_scroll_test

SCRIPT_RESOURCE_DIR = $(BUILD)/site-general/script-resources
SCRIPT_RESOURCE_BASE = $(filter-out tests/unit/runtime_scroll_test.c,$(RUNTIME_SCROLL_SRC)) tests/unit/script_resource_events_test.c
# Script.src is installed by the real platform installer. The smaller scroll
# link silently turns that IDL setter into an expando and never fetches code.
SCRIPT_RESOURCE_SRC = $(SCRIPT_RESOURCE_BASE) $(filter-out tests/unit/dom_iface_test.c $(SCRIPT_RESOURCE_BASE),$(DOMIFACE_SRC))
$(SCRIPT_RESOURCE_DIR)/events: $(SCRIPT_RESOURCE_SRC) $(RUNTIME_SCROLL_DEPS)
	@mkdir -p $(SCRIPT_RESOURCE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(SCRIPT_RESOURCE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(SCRIPT_RESOURCE_DIR)/events-old: $(SCRIPT_RESOURCE_SRC) $(RUNTIME_SCROLL_DEPS)
	@mkdir -p $(SCRIPT_RESOURCE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_NO_SCRIPT_RESOURCE_EVENTS -o $@ $(SCRIPT_RESOURCE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-script-resource-events test-script-resource-events-negctl
test-script-resource-events-negctl: $(SCRIPT_RESOURCE_DIR)/events-old
	@rc=0; $(SCRIPT_RESOURCE_DIR)/events-old > $(SCRIPT_RESOURCE_DIR)/old.log 2>&1 || rc=$$?; cat $(SCRIPT_RESOURCE_DIR)/old.log; test "$$rc" -eq 1 && grep -q 'FAIL: load callback can insert and finish the next script' $(SCRIPT_RESOURCE_DIR)/old.log
test-script-resource-events: test-script-resource-events-negctl $(SCRIPT_RESOURCE_DIR)/events
	@rc=0; $(SCRIPT_RESOURCE_DIR)/events > $(SCRIPT_RESOURCE_DIR)/current.log 2>&1 || rc=$$?; cat $(SCRIPT_RESOURCE_DIR)/current.log; exit $$rc
	@$(SCRIPT_RESOURCE_DIR)/events --editing > $(SCRIPT_RESOURCE_DIR)/editing-current.log 2>&1; rc=$$?; tail -14 $(SCRIPT_RESOURCE_DIR)/editing-current.log; exit $$rc
$(SCRIPT_RESOURCE_DIR)/events-address-old: $(SCRIPT_RESOURCE_SRC) $(RUNTIME_SCROLL_DEPS)
	@mkdir -p $(SCRIPT_RESOURCE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_SCRIPT_USES_ADDRESS_EDIT -o $@ $(SCRIPT_RESOURCE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-script-address-base-negctl
test-script-address-base-negctl: $(SCRIPT_RESOURCE_DIR)/events-address-old
	@rc=0; $(SCRIPT_RESOURCE_DIR)/events-address-old --editing > $(SCRIPT_RESOURCE_DIR)/editing-old.log 2>&1 || rc=$$?; tail -14 $(SCRIPT_RESOURCE_DIR)/editing-old.log; test $$rc -eq 1 && grep -q 'FAIL: success source really executed' $(SCRIPT_RESOURCE_DIR)/editing-old.log
test-script-resource-events: test-script-address-base-negctl
ci-host: test-script-resource-events
