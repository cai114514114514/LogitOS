# Reuse the union of real CSSOM and painter pipelines, removing only its main.
# No copied production source inventory or substitute hit algorithm. The
# control is a prerequisite and must fail the specific transparent-center case,
# not merely an unrelated pointer-events assertion.
TRANSPARENT_BOX_HIT_SRC = $(filter-out tests/unit/element_scroll_test.c,$(ELEMENT_SCROLL_SRC)) tests/unit/transparent_box_hit_test.c
TRANSPARENT_BOX_HIT_DIR = $(BUILD)/site-general/transparent-box-hit
TRANSPARENT_BOX_HIT_DEPS = $(TRANSPARENT_BOX_HIT_SRC) tests/unit/cssom_test.c c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/browser_paint.h c/apps/browser/element_scroll_wiring.inc c/apps/browser/text_paint_wiring.inc $(QJS_SRC) $(BUILD)/libcss_host.a
.PHONY: test-transparent-box-hit test-transparent-box-hit-negctl
$(TRANSPARENT_BOX_HIT_DIR)/current: $(TRANSPARENT_BOX_HIT_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(ELEMENT_SCROLL_FLAGS) -o $@ $(TRANSPARENT_BOX_HIT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(TRANSPARENT_BOX_HIT_DIR)/legacy: $(TRANSPARENT_BOX_HIT_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(ELEMENT_SCROLL_FLAGS) -DLAYOUT_HIT_INK_ONLY -o $@ $(TRANSPARENT_BOX_HIT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
test-transparent-box-hit-negctl: $(TRANSPARENT_BOX_HIT_DIR)/legacy
	@rc=0; $(TRANSPARENT_BOX_HIT_DIR)/legacy > $(TRANSPARENT_BOX_HIT_DIR)/legacy.log 2>&1 || rc=$$?; cat $(TRANSPARENT_BOX_HIT_DIR)/legacy.log; test $$rc -eq 1 && grep -q 'FAIL block blank center native target' $(TRANSPARENT_BOX_HIT_DIR)/legacy.log
test-transparent-box-hit: test-transparent-box-hit-negctl $(TRANSPARENT_BOX_HIT_DIR)/current
	@rc=0; $(TRANSPARENT_BOX_HIT_DIR)/current > $(TRANSPARENT_BOX_HIT_DIR)/current.log 2>&1 || rc=$$?; cat $(TRANSPARENT_BOX_HIT_DIR)/current.log; exit $$rc
ci-host: test-transparent-box-hit
