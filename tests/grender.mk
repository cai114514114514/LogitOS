# Legacy home-page markup through the shipping DOM/CSS/layout/painter.
# This fragment was an empty placeholder since 2026-08-30. The specimen now
# has a general geometry gate; no product rule depends on a site name.
LEGACY_HOME_DIR = $(BUILD)/legacy-home
LEGACY_HOME_SRC = $(filter-out tests/unit/form_caret_test.c,$(FORM_CARET_SRC)) tests/unit/legacy_home_test.c
LEGACY_HOME_DEPS = $(LEGACY_HOME_SRC) $(FORM_CARET_DEPS) tests/unit/modal_paint_test.c tests/grender.mk
# The shared painter list is defined by a later include. Expand prerequisites
# after all fragments are read, or a changed layout.c silently leaves a stale
# gate binary even though the compile recipe would link the right sources.
.SECONDEXPANSION:
$(LEGACY_HOME_DIR)/current: $$(LEGACY_HOME_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(LEGACY_HOME_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(LEGACY_HOME_SRC) $(BUILD)/libcss_host.a -lm
$(LEGACY_HOME_DIR)/old: $$(LEGACY_HOME_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(LEGACY_HOME_DIR)
	@$(CC) -O2 -w -DLEGACY_HOME_NEGCTL $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(LEGACY_HOME_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-legacy-home test-legacy-home-negctl
test-legacy-home-negctl: $(LEGACY_HOME_DIR)/old
	@rc=0; $< >$(LEGACY_HOME_DIR)/old.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -q 'FAIL: center aligns inline image' $(LEGACY_HOME_DIR)/old.log && \
	 grep -q 'FAIL: table cell contains its input' $(LEGACY_HOME_DIR)/old.log && \
	 grep -q 'FAIL: image pixels exclude padding and border' $(LEGACY_HOME_DIR)/old.log
	@echo 'legacy-home negative control: centering, cell sizing and image inset failures observed'
test-legacy-home: test-legacy-home-negctl $(LEGACY_HOME_DIR)/current
	@$(LEGACY_HOME_DIR)/current
ci-host: test-legacy-home
