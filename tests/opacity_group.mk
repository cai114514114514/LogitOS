OPACITY_GROUP_DIR := $(BUILD)/site-general/layout/opacity-group
OPACITY_GROUP_SRC = $(filter-out tests/unit/element_scroll_test.c,$(ELEMENT_SCROLL_SRC)) tests/unit/opacity_group_test.c
OPACITY_GROUP_DEPS = $(OPACITY_GROUP_SRC) tests/unit/max_height_test.c tests/unit/cssom_test.c c/apps/browser/layout.h c/apps/browser/text_paint_wiring.inc $(QJS_SRC) $(BUILD)/libcss_host.a tests/opacity_group.mk
.PHONY: test-opacity-group test-opacity-group-negctl
$(OPACITY_GROUP_DIR)/test: $(OPACITY_GROUP_DEPS)
	@mkdir -p $(OPACITY_GROUP_DIR)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -o $@ $(OPACITY_GROUP_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(OPACITY_GROUP_DIR)/negctl: $(OPACITY_GROUP_DEPS)
	@mkdir -p $(OPACITY_GROUP_DIR)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -DBROWSER_NO_ZERO_OPACITY_ANCESTOR -o $@ $(OPACITY_GROUP_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
test-opacity-group-negctl: $(OPACITY_GROUP_DIR)/negctl
	@$(OPACITY_GROUP_DIR)/negctl > $(OPACITY_GROUP_DIR)/negctl.log 2>&1; rc=$$?; cat $(OPACITY_GROUP_DIR)/negctl.log; test $$rc -eq 1 && grep -q 'FAIL zero-opacity ancestor suppresses descendant ink' $(OPACITY_GROUP_DIR)/negctl.log
test-opacity-group: test-opacity-group-negctl $(OPACITY_GROUP_DIR)/test
	@$(OPACITY_GROUP_DIR)/test

# Same app_main/deadline/paint pipeline as the scrolling gate. Add the clock
# producer explicitly: the small loader source set otherwise takes its weak stub.
ANIMATION_REFRESH_SRC = $(sort $(filter-out tests/unit/runtime_scroll_test.c,$(RUNTIME_SCROLL_SRC)) $(filter-out $(QJS_SRC),$(ANIMCLK_SRC))) tests/unit/animation_refresh_test.c
ANIMATION_REFRESH_DEPS = $(ANIMATION_REFRESH_SRC) $(RUNTIME_SCROLL_DEPS) tests/opacity_group.mk
$(OPACITY_GROUP_DIR)/refresh: $(ANIMATION_REFRESH_DEPS)
	@mkdir -p $(OPACITY_GROUP_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(ANIMATION_REFRESH_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(OPACITY_GROUP_DIR)/refresh-old: $(ANIMATION_REFRESH_DEPS)
	@mkdir -p $(OPACITY_GROUP_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_ANIMATION_FULL_LAYOUT -o $@ $(ANIMATION_REFRESH_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-animation-refresh test-animation-refresh-negctl
test-animation-refresh-negctl: $(OPACITY_GROUP_DIR)/refresh-old
	@rc=0; $< > $(OPACITY_GROUP_DIR)/refresh-old.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: animation frames do not rebuild geometry' $(OPACITY_GROUP_DIR)/refresh-old.log && \
	 grep -F 'ok: animation publishes changing display-list alpha' $(OPACITY_GROUP_DIR)/refresh-old.log
test-animation-refresh: test-animation-refresh-negctl $(OPACITY_GROUP_DIR)/refresh
	@$(OPACITY_GROUP_DIR)/refresh
ci-host: test-animation-refresh

$(OPACITY_GROUP_DIR)/refresh-san: $(ANIMATION_REFRESH_DEPS)
	@mkdir -p $(OPACITY_GROUP_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(ANIMATION_REFRESH_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-animation-refresh-san
test-animation-refresh-san: test-animation-refresh $(OPACITY_GROUP_DIR)/refresh-san
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(OPACITY_GROUP_DIR)/refresh-san
