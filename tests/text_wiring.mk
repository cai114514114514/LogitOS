# Actual browser layout and painter, derived from the production-shaped hit
# harness; controls disable the formatter consumer and glyph-spacing consumer.
TEXT_WIRING_SRC = $(filter-out tests/unit/inline_hit_test.c,$(INLINE_HIT_SRC)) tests/unit/text_wiring_test.c
TEXT_WIRING_DEPS = tests/unit/painthost/logit.h c/apps/browser/layout.h c/apps/browser/layout_text.h c/apps/browser/text_paint_wiring.inc
.PHONY: test-text-wiring test-text-wiring-negctl
$(BUILD)/wiring-next/text_wiring_test: $(TEXT_WIRING_SRC) $(TEXT_WIRING_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)/wiring-next
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(TEXT_WIRING_SRC) $(BUILD)/libcss_host.a -lm
test-text-wiring-negctl: $(TEXT_WIRING_SRC) $(TEXT_WIRING_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)/wiring-next
	@set -e; for ctl in LAYOUT_NO_TEXT_FORMATTER PAINT_NO_TEXT_SPACING LAYOUT_FIXED_HEIGHT_MINIMUM; do \
	 $(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -D$$ctl -o $(BUILD)/wiring-next/text_$$ctl $(TEXT_WIRING_SRC) $(BUILD)/libcss_host.a -lm; \
	 set +e; $(BUILD)/wiring-next/text_$$ctl > $(BUILD)/wiring-next/text_$$ctl.log 2>&1; rc=$$?; set -e; cat $(BUILD)/wiring-next/text_$$ctl.log; test $$rc -eq 1; \
	 case $$ctl in LAYOUT_FIXED_HEIGHT_MINIMUM) grep -q 'FAIL: specified height remains scroll viewport height' $(BUILD)/wiring-next/text_$$ctl.log;; LAYOUT_NO_TEXT_FORMATTER) grep -q 'FAIL: formatter applies first-line indent' $(BUILD)/wiring-next/text_$$ctl.log;; PAINT_NO_TEXT_SPACING) grep -q 'FAIL: painter consumes letter spacing' $(BUILD)/wiring-next/text_$$ctl.log;; esac; done
test-text-wiring: test-text-wiring-negctl $(BUILD)/wiring-next/text_wiring_test
	@$(BUILD)/wiring-next/text_wiring_test
