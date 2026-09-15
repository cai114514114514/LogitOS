# Inherit the actual browser loader/DOM/painter link; do not maintain another
# source inventory. Native form controls come from the browser's existing path.
# Native C form activation did not need the JS adapter. Script-initiated submit
# must link its real installer or form.submit is absent and this gate tests an
# incomplete harness instead of the active JS-frame lifetime defect.
NAVIGATION_BASE_SRC = $(sort $(filter-out tests/unit/script_resource_events_test.c,$(SCRIPT_RESOURCE_SRC)) c/apps/browser/js_forms.c) tests/unit/navigation_base_test.c
NAVIGATION_BASE_DEPS = $(NAVIGATION_BASE_SRC) $(RUNTIME_SCROLL_DEPS) $(wildcard c/apps/browser/*.inc)
NAVIGATION_BASE_DIR = $(BUILD)/site-general/navigation-base
$(NAVIGATION_BASE_DIR)/current: $(NAVIGATION_BASE_DEPS)
	@mkdir -p $(NAVIGATION_BASE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(NAVIGATION_BASE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(NAVIGATION_BASE_DIR)/old: $(NAVIGATION_BASE_DEPS)
	@mkdir -p $(NAVIGATION_BASE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_NAV_USES_ADDRESS_EDIT -o $@ $(NAVIGATION_BASE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-navigation-base test-navigation-base-negctl
test-navigation-base-negctl: $(NAVIGATION_BASE_DIR)/old
	@for mode in link relative-form no-action-form tab-link; do \
	 rc=0; $(NAVIGATION_BASE_DIR)/old $$mode > $(NAVIGATION_BASE_DIR)/old-$$mode.log 2>&1 || rc=$$?; \
	 tail -12 $(NAVIGATION_BASE_DIR)/old-$$mode.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: native navigation requests exact document-origin destination' $(NAVIGATION_BASE_DIR)/old-$$mode.log && \
	 grep -Fq 'request[1]=http://uncommitted.test/' $(NAVIGATION_BASE_DIR)/old-$$mode.log || exit 1; \
	 expected_failures=3; if test "$$mode" = tab-link; then expected_failures=4; \
	 grep -q '^ok: native tab round trip actually completed' $(NAVIGATION_BASE_DIR)/old-$$mode.log && \
	 grep -q '^FAIL: tab dehydration saves the committed document URL' $(NAVIGATION_BASE_DIR)/old-$$mode.log || exit 1; fi; \
	 test "$$(grep -c '^FAIL:' $(NAVIGATION_BASE_DIR)/old-$$mode.log)" -eq $$expected_failures || exit 1; \
	 done
	@echo 'navigation-base-negctl: PASS -- all four native paths visibly fail with the address edit buffer'
test-navigation-base: test-navigation-base-negctl $(NAVIGATION_BASE_DIR)/current
	@for mode in link relative-form no-action-form tab-link; do \
	 rc=0; $(NAVIGATION_BASE_DIR)/current $$mode > $(NAVIGATION_BASE_DIR)/current-$$mode.log 2>&1 || rc=$$?; \
	 tail -12 $(NAVIGATION_BASE_DIR)/current-$$mode.log; test $$rc -eq 0 || exit $$rc; \
	 done
ci-host: test-navigation-base

$(NAVIGATION_BASE_DIR)/form-sync-old: $(NAVIGATION_BASE_DEPS)
	@mkdir -p $(NAVIGATION_BASE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_FORM_SUBMIT_SYNC_LOAD -o $@ $(NAVIGATION_BASE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-navigation-form-stack test-navigation-form-stack-negctl
# This control intentionally aborts inside QuickJS, not at a generic signal.
# Require the witnessed runtime assertion plus the handler/form-load markers;
# an unrelated segmentation fault must never satisfy the regression gate.
test-navigation-form-stack-negctl: $(NAVIGATION_BASE_DIR)/form-sync-old
	@ulimit -c 0; for mode in key-submit key-request-submit; do \
	 rc=0; $(NAVIGATION_BASE_DIR)/form-sync-old $$mode > $(NAVIGATION_BASE_DIR)/form-sync-old-$$mode.log 2>&1 || rc=$$?; \
	 tail -12 $(NAVIGATION_BASE_DIR)/form-sync-old-$$mode.log; \
	 test $$rc -eq 134 && \
	 grep -Eq 'JS_FreeRuntime.*gc_obj_list|gc_obj_list.*JS_FreeRuntime' $(NAVIGATION_BASE_DIR)/form-sync-old-$$mode.log && \
	 grep -q '^\[browser\] FORM-GET ' $(NAVIGATION_BASE_DIR)/form-sync-old-$$mode.log && \
	 grep -q '^FORM-HANDLER before' $(NAVIGATION_BASE_DIR)/form-sync-old-$$mode.log && \
	 ! grep -q '^FORM-HANDLER after' $(NAVIGATION_BASE_DIR)/form-sync-old-$$mode.log || exit 1; \
	 done
	@echo 'form-stack-negctl: PASS -- synchronous navigation destroys active handler in both modes'
test-navigation-form-stack: test-navigation-form-stack-negctl $(NAVIGATION_BASE_DIR)/current
	@for mode in key-submit key-request-submit key-same-url key-submit-then-location key-location-then-submit; do \
	 rc=0; $(NAVIGATION_BASE_DIR)/current $$mode > $(NAVIGATION_BASE_DIR)/current-$$mode.log 2>&1 || rc=$$?; \
	 tail -16 $(NAVIGATION_BASE_DIR)/current-$$mode.log; test $$rc -eq 0 || exit $$rc; \
	 done
test-navigation-base: test-navigation-form-stack
