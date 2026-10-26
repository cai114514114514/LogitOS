# font-ui-kerning: ui.ttf / ui-bold.ttf keep GPOS `kern` (+GDEF); mono does not.
FUK_SRC := c/lib/text/bidi.c c/lib/text/script.c c/lib/text/shape.c \
           c/lib/text/otlayout.c c/lib/text/ttf.c c/lib/text/cff.c \
           c/lib/text/utf8.c
FUK_CC  := $(CC) -O2 -Wall -Wextra -Ic/lib/text
FUK_NEG := $(BUILD)/fuk-neg

# Scratch fonts built by the same mkfont.py with --drop-layout (no GPOS/GDEF).
$(FUK_NEG)/stamp: tools/mkfont.py $(FONT_UI_SRC) $(FONT_MONO_SRC)
	@mkdir -p $(FUK_NEG)
	@python3 tools/mkfont.py --drop-layout $(FUK_NEG)/ui.ttf $(FUK_NEG)/mono.ttf \
	    $(FUK_NEG)/ui-bold.ttf $(FUK_NEG)/mono-bold.ttf 2>/dev/null
	@touch $@

$(BUILD)/font_ui_kern_test: tests/unit/font_ui_kern_test.c $(FUK_SRC)
	@mkdir -p $(BUILD)
	$(FUK_CC) -o $@ tests/unit/font_ui_kern_test.c $(FUK_SRC)

test-font-ui-kerning-negctl: $(BUILD)/font_ui_kern_test $(FUK_NEG)/stamp
	@if $(BUILD)/font_ui_kern_test $(FUK_NEG)/ui.ttf $(FUK_NEG)/ui-bold.ttf \
	    >$(BUILD)/fuk-negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: font without GPOS still kerns"; exit 1; \
	 elif grep -q '^FAIL kern AV' $(BUILD)/fuk-negctl.log; then \
	    echo "negctl ok: $$(grep -m1 '^FAIL kern AV' $(BUILD)/fuk-negctl.log)"; \
	 else \
	    echo "NEGATIVE CONTROL failed for another reason:"; cat $(BUILD)/fuk-negctl.log; exit 1; \
	 fi

test-font-ui-kerning: test-font-ui-kerning-negctl $(BUILD)/font_ui_kern_test
	@$(BUILD)/font_ui_kern_test fsroot/fonts/ui.ttf fsroot/fonts/ui-bold.ttf
	@$(MAKE) --no-print-directory BUILD=$(BUILD) verify-fonts

ci-host: test-font-ui-kerning
.PHONY: test-font-ui-kerning test-font-ui-kerning-negctl
