# One shipping loader/loop/painter source set; CSSOM and forms provide their
# real bindings so ordinary element.focus and document.activeElement are tested.
INTERACTION_RUNTIME_SRC = $(filter-out tests/unit/runtime_scroll_test.c,$(RUNTIME_SCROLL_SRC)) c/apps/browser/js_forms.c tests/unit/interaction_runtime_test.c
# Include textual implementation seams too: a changed .inc must not leave an
# up-to-date host binary measuring the previous cascade or ownership code.
INTERACTION_RUNTIME_DEPS = $(INTERACTION_RUNTIME_SRC) tests/unit/loader_test.c tests/unit/loaderhost/logit.h tests/unit/painthost/logit.h $(wildcard c/apps/browser/*.h c/apps/browser/*.inc) c/apps/browser/page_runtime.c c/apps/browser/storage_backend.c c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) tests/fixtures/engine-interaction/index.html
INTERACTION_RUNTIME_CF = -O2 -w -DLOADER_POLL_HOOK $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
.PHONY: test-interaction-runtime test-interaction-runtime-negctl
$(BUILD)/interaction_runtime_test: $(INTERACTION_RUNTIME_DEPS)
	$(CC) $(INTERACTION_RUNTIME_CF) -o $@ $(INTERACTION_RUNTIME_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/interaction_runtime_negctl: $(INTERACTION_RUNTIME_DEPS)
	$(CC) $(INTERACTION_RUNTIME_CF) -DCSS_NEGCTL_STATIC_INTERACTION -o $@ $(INTERACTION_RUNTIME_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-interaction-runtime-negctl: $(BUILD)/interaction_runtime_negctl
	@rc=0; $(BUILD)/interaction_runtime_negctl > $(BUILD)/interaction_runtime_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/interaction_runtime_negctl.log; \
	 test "$$rc" -eq 1 && grep -q '^FAIL: native hover opens menu without JS listeners' $(BUILD)/interaction_runtime_negctl.log && \
	 grep -q '^FAIL: native mouse down paints active color' $(BUILD)/interaction_runtime_negctl.log && \
	 grep -q '^FAIL: native Tab paints focus selector' $(BUILD)/interaction_runtime_negctl.log
	@echo 'interaction-runtime-negctl: PASS -- static pseudo-classes fail visible native interaction assertions'
test-interaction-runtime: test-interaction-runtime-negctl $(BUILD)/interaction_runtime_test
	@$(BUILD)/interaction_runtime_test
