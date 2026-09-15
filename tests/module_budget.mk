# Same native page/module/watchdog TUs as the existing lifetime gate. Network
-include tests/module_retry.mk
# stubs advance an injected clock, never host wall time or a public server.
MODULE_BUDGET_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/module_budget_test.c
MODULE_BUDGET_DEPS = $(MODULE_BUDGET_SRC) c/apps/browser/js_page.h $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
MODULE_BUDGET_OUT = $(BUILD)/site-general/runtime
.PHONY: test-module-budget test-module-budget-negctl
$(MODULE_BUDGET_OUT)/module_budget_test: $(MODULE_BUDGET_DEPS)
	@mkdir -p $(MODULE_BUDGET_OUT)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(MODULE_BUDGET_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(MODULE_BUDGET_OUT)/module_budget_negctl: $(MODULE_BUDGET_DEPS)
	@mkdir -p $(MODULE_BUDGET_OUT)
	$(CC) -O2 -w $(DOMIFACE_CF) -DJS_MODULE_NETWORK_TIME_OLD -o $@ $(MODULE_BUDGET_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-module-budget-negctl: $(MODULE_BUDGET_OUT)/module_budget_negctl
	@rc=0; $< > $(MODULE_BUDGET_OUT)/module_budget_negctl.log 2>&1 || rc=$$?; cat $(MODULE_BUDGET_OUT)/module_budget_negctl.log; test $$rc -eq 1 && grep -q '^FAIL prefetch wait does not consume' $(MODULE_BUDGET_OUT)/module_budget_negctl.log && grep -q '^FAIL cache miss fetch does not consume' $(MODULE_BUDGET_OUT)/module_budget_negctl.log && grep -q '^PASS module CPU wall time still interrupts' $(MODULE_BUDGET_OUT)/module_budget_negctl.log && grep -q '^PASS module frozen-clock fuel still interrupts' $(MODULE_BUDGET_OUT)/module_budget_negctl.log
test-module-budget: test-module-budget-negctl $(MODULE_BUDGET_OUT)/module_budget_test
	@$(MODULE_BUDGET_OUT)/module_budget_test

MODULE_PROFILE_SRC = $(filter-out tests/unit/module_budget_test.c,$(MODULE_BUDGET_SRC)) tests/unit/module_profile_test.c
MODULE_PROFILE_DEPS = $(MODULE_PROFILE_SRC) $(MODULE_BUDGET_DEPS) c/apps/browser/js_module.h tests/module_budget.mk
$(MODULE_BUDGET_OUT)/module_profile_test: $(MODULE_PROFILE_DEPS)
	@mkdir -p $(MODULE_BUDGET_OUT)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(MODULE_PROFILE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(MODULE_BUDGET_OUT)/module_profile_negctl: $(MODULE_PROFILE_DEPS)
	@mkdir -p $(MODULE_BUDGET_OUT)
	$(CC) -O2 -w $(DOMIFACE_CF) -DJS_MODULE_PROFILE_MISCHARGE_IO -o $@ $(MODULE_PROFILE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-module-profile test-module-profile-negctl
test-module-profile-negctl: $(MODULE_BUDGET_OUT)/module_profile_negctl
	@rc=0; $< > $(MODULE_BUDGET_OUT)/module_profile_negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL prefetch interval belongs to network wait' $(MODULE_BUDGET_OUT)/module_profile_negctl.log && \
	 grep -F 'FAIL cache-miss interval belongs to fetch' $(MODULE_BUDGET_OUT)/module_profile_negctl.log && \
	 grep -F 'PASS nested loader partitions elapsed time without double counting' $(MODULE_BUDGET_OUT)/module_profile_negctl.log
test-module-profile: test-module-profile-negctl $(MODULE_BUDGET_OUT)/module_profile_test
	@$(MODULE_BUDGET_OUT)/module_profile_test
ci-host: test-module-profile

# Shared/cyclic imports must not occupy prefetch slots that QuickJS will never
# consume. The control restores precisely that redundant speculation.
MODULE_PREFETCH_SRC = $(filter-out tests/unit/module_budget_test.c,$(MODULE_BUDGET_SRC)) tests/unit/module_prefetch_loaded_test.c
MODULE_PREFETCH_DEPS = $(MODULE_PREFETCH_SRC) $(MODULE_BUDGET_DEPS) tests/module_budget.mk
$(MODULE_BUDGET_OUT)/module_prefetch_loaded: $(MODULE_PREFETCH_DEPS)
	@mkdir -p $(MODULE_BUDGET_OUT)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(MODULE_PREFETCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(MODULE_BUDGET_OUT)/module_prefetch_loaded_negctl: $(MODULE_PREFETCH_DEPS)
	@mkdir -p $(MODULE_BUDGET_OUT)
	$(CC) -O2 -w $(DOMIFACE_CF) -DJS_MODULE_PREFETCH_LOADED -o $@ $(MODULE_PREFETCH_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-module-prefetch-loaded test-module-prefetch-loaded-negctl
test-module-prefetch-loaded-negctl: $(MODULE_BUDGET_OUT)/module_prefetch_loaded_negctl
	@rc=0; $< > $(MODULE_BUDGET_OUT)/module_prefetch_loaded_negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL compiled shared dependency is not prefetched again' $(MODULE_BUDGET_OUT)/module_prefetch_loaded_negctl.log && \
	 grep -F 'FAIL back edge never prefetches the compiling root' $(MODULE_BUDGET_OUT)/module_prefetch_loaded_negctl.log && \
	 grep -F 'PASS all 48 branch bodies execute exactly once' $(MODULE_BUDGET_OUT)/module_prefetch_loaded_negctl.log
test-module-prefetch-loaded: test-module-prefetch-loaded-negctl $(MODULE_BUDGET_OUT)/module_prefetch_loaded
	@$(MODULE_BUDGET_OUT)/module_prefetch_loaded
ci-host: test-module-prefetch-loaded
