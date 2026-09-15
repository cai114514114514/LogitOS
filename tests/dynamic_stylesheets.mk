# SPDX-License-Identifier: MIT
# Link the actual IDL reflection installer: without it link.rel is an expando
# in this host subset, while the shipped browser writes the rel attribute.
DYNAMIC_SHEETS_SRC = $(filter-out tests/unit/parser_script_events_test.c,$(PARSER_SCRIPT_SRC)) tests/unit/dynamic_stylesheets_test.c c/apps/browser/js_reflect.c
DYNAMIC_SHEETS_DEPS = tests/dynamic_stylesheets.mk $(DYNAMIC_SHEETS_SRC) $(RUNTIME_SCROLL_DEPS) c/apps/browser/browser_stylesheets.inc tests/fixtures/browser/dynamic-styles.html
DYNAMIC_SHEETS_DIR = $(BUILD)/dynamic-styles
$(DYNAMIC_SHEETS_DIR)/current: $(DYNAMIC_SHEETS_DEPS)
	@mkdir -p $(DYNAMIC_SHEETS_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(DYNAMIC_SHEETS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(DYNAMIC_SHEETS_DIR)/old: $(DYNAMIC_SHEETS_DEPS)
	@mkdir -p $(DYNAMIC_SHEETS_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_NO_DYNAMIC_STYLESHEETS -o $@ $(DYNAMIC_SHEETS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-dynamic-stylesheets test-dynamic-stylesheets-negctl
test-dynamic-stylesheets-negctl: $(DYNAMIC_SHEETS_DIR)/old
	@rc=0; $< > $(DYNAMIC_SHEETS_DIR)/old.log 2>&1 || rc=$$?; tail -20 $(DYNAMIC_SHEETS_DIR)/old.log; \
	 test $$rc -eq 1 && grep -q '^FAIL dynamic stylesheet delivers load' $(DYNAMIC_SHEETS_DIR)/old.log && \
	 grep -q '^FAIL: both stylesheet URLs reached transport' $(DYNAMIC_SHEETS_DIR)/old.log
test-dynamic-stylesheets: test-dynamic-stylesheets-negctl $(DYNAMIC_SHEETS_DIR)/current
	@$(DYNAMIC_SHEETS_DIR)/current > $(DYNAMIC_SHEETS_DIR)/current.log 2>&1; rc=$$?; tail -24 $(DYNAMIC_SHEETS_DIR)/current.log; exit $$rc
ci-host: test-dynamic-stylesheets

INITIAL_SHEETS_SRC = $(filter-out tests/unit/dynamic_stylesheets_test.c,$(DYNAMIC_SHEETS_SRC)) tests/unit/initial_stylesheet_test.c
INITIAL_SHEETS_DEPS = $(INITIAL_SHEETS_SRC) $(DYNAMIC_SHEETS_DEPS)
$(DYNAMIC_SHEETS_DIR)/initial: $(INITIAL_SHEETS_DEPS)
	@mkdir -p $(DYNAMIC_SHEETS_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(INITIAL_SHEETS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(DYNAMIC_SHEETS_DIR)/initial-old: $(INITIAL_SHEETS_DEPS)
	@mkdir -p $(DYNAMIC_SHEETS_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_REBUILD_INITIAL_SHEETS -o $@ $(INITIAL_SHEETS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-initial-stylesheet test-initial-stylesheet-negctl
test-initial-stylesheet-negctl: $(DYNAMIC_SHEETS_DIR)/initial-old
	@rc=0; $< > $(DYNAMIC_SHEETS_DIR)/initial-old.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: initial committed stylesheet is not rebuilt' $(DYNAMIC_SHEETS_DIR)/initial-old.log && \
	 grep -F 'ok: later stylesheet edit still changes real width' $(DYNAMIC_SHEETS_DIR)/initial-old.log
test-initial-stylesheet: test-initial-stylesheet-negctl $(DYNAMIC_SHEETS_DIR)/initial
	@$(DYNAMIC_SHEETS_DIR)/initial
ci-host: test-initial-stylesheet

$(DYNAMIC_SHEETS_DIR)/initial-reapply: $(INITIAL_SHEETS_DEPS)
	@mkdir -p $(DYNAMIC_SHEETS_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_RESTYLE_APPLIED_SHEET -o $@ $(INITIAL_SHEETS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-stylesheet-reapply-negctl
test-stylesheet-reapply-negctl: $(DYNAMIC_SHEETS_DIR)/initial-reapply
	@rc=0; $< > $(DYNAMIC_SHEETS_DIR)/initial-reapply.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: applied author edit does not recascade through DOM dirty again' $(DYNAMIC_SHEETS_DIR)/initial-reapply.log && \
	 grep -F 'ok: later stylesheet edit still changes real width' $(DYNAMIC_SHEETS_DIR)/initial-reapply.log
test-initial-stylesheet: test-stylesheet-reapply-negctl
