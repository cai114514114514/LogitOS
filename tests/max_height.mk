MAX_HEIGHT_SRC = $(filter-out tests/unit/element_scroll_test.c,$(ELEMENT_SCROLL_SRC)) tests/unit/max_height_test.c
MAX_HEIGHT_DEPS = $(MAX_HEIGHT_SRC) tests/unit/cssom_test.c c/apps/browser/layout.h c/apps/browser/element_scroll_wiring.inc c/apps/browser/text_paint_wiring.inc tests/unit/painthost/logit.h $(QJS_SRC) $(BUILD)/libcss_host.a
.PHONY: test-max-height test-max-height-negctl
$(BUILD)/wiring-next/max_height_test: $(MAX_HEIGHT_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -o $@ $(MAX_HEIGHT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
test-max-height-negctl: $(MAX_HEIGHT_DEPS)
	@mkdir -p $(BUILD)/wiring-next
	@set -e; for ctl in LAYOUT_NO_AUTO_MAX_HEIGHT LAYOUT_NO_AUTO_MAX_CLIP; do \
	 $(CC) $(ELEMENT_SCROLL_FLAGS) -D$$ctl -o $(BUILD)/wiring-next/max_$$ctl $(MAX_HEIGHT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm; \
	 set +e; $(BUILD)/wiring-next/max_$$ctl > $(BUILD)/wiring-next/max_$$ctl.log 2>&1; rc=$$?; set -e; cat $(BUILD)/wiring-next/max_$$ctl.log; test $$rc -eq 1; \
	 case $$ctl in LAYOUT_NO_AUTO_MAX_HEIGHT) grep -q 'FAIL auto max-height limits border box' $(BUILD)/wiring-next/max_$$ctl.log;; LAYOUT_NO_AUTO_MAX_CLIP) grep -q 'FAIL auto max-height stamps final descendant clip' $(BUILD)/wiring-next/max_$$ctl.log;; esac; done
test-max-height: test-max-height-negctl $(BUILD)/wiring-next/max_height_test
	@$(BUILD)/wiring-next/max_height_test
