# Share the actual transparent-box consumer pipeline; focus remains independent.
POINTER_EVENTS_DIR := $(BUILD)/site-general/layout/pointer-events
POINTER_EVENTS_SRC = $(filter-out tests/unit/transparent_box_hit_test.c,$(TRANSPARENT_BOX_HIT_SRC)) tests/unit/pointer_events_test.c c/apps/gui/browser/dom/focus.c
POINTER_EVENTS_DEPS = $(POINTER_EVENTS_SRC) tests/unit/transparent_box_hit_test.c tests/unit/cssom_test.c c/apps/gui/browser/css/style.h c/apps/gui/browser/paint/element_scroll.inc c/apps/gui/browser/paint/text.inc $(QJS_SRC) $(BUILD)/libcss_host.a
$(POINTER_EVENTS_DIR)/test: $(POINTER_EVENTS_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -o $@ $(POINTER_EVENTS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(POINTER_EVENTS_DIR)/legacy: $(POINTER_EVENTS_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -DCSS_POINTER_HIT_LEGACY -o $@ $(POINTER_EVENTS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(POINTER_EVENTS_DIR)/all-legacy: $(POINTER_EVENTS_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -DCSS_POINTER_ALL_HIT_LEGACY -o $@ $(POINTER_EVENTS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-pointer-events-all-negctl
test-pointer-events-all-negctl: $(POINTER_EVENTS_DIR)/all-legacy
	@rc=0; $< > $(POINTER_EVENTS_DIR)/all-legacy.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -F 'FAIL all dialog escapes none wrapper native target' $(POINTER_EVENTS_DIR)/all-legacy.log && grep -F 'FAIL all dialog escapes none wrapper CSSOM target' $(POINTER_EVENTS_DIR)/all-legacy.log
.PHONY: test-pointer-events test-pointer-events-negctl
test-pointer-events-negctl: $(POINTER_EVENTS_DIR)/legacy
	@rc=0; $< > $(POINTER_EVENTS_DIR)/legacy.log 2>&1 || rc=$$?; cat $(POINTER_EVENTS_DIR)/legacy.log; test $$rc -eq 1 && grep -q 'FAIL none overlay passes through blank region native target' $(POINTER_EVENTS_DIR)/legacy.log && grep -q 'FAIL inherited none excludes descendant text CSSOM target' $(POINTER_EVENTS_DIR)/legacy.log
test-pointer-events: test-pointer-events-negctl test-pointer-events-all-negctl $(POINTER_EVENTS_DIR)/test
	@$(POINTER_EVENTS_DIR)/test

# Immutable pre-fix/current disks are explicit inputs. Never turn a missing
# artifact, failed boot, or a page that never ran into an expected red control.
POINTER_ALL_GUEST_ISO ?=
POINTER_ALL_GUEST_DISK ?=
POINTER_ALL_GUEST_NEG_DISK ?=
