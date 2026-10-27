# Quarter-pixel glyph pen in c/kernel/gui/text.c and the optional glyph coverage
# LUT of ol_display_blit_glyph_a. Host harness links the real text.c, shaper and
# fonts. The control builds the SAME sources and flags with -DTEXT_INTEGER_PEN
# (whole-pixel pen); "FAIL gap" must appear; only gap and phase (no
# quarter-pixel phases exist without the fractional pen) may fail.
TSG_DIR := $(BUILD)/text-subpixel-gamma
TSG_SRC := tests/unit/text_subpixel_test.c $(sort $(FONT_WEIGHT_SRC))
TSG_INC := $(FONT_WEIGHT_INC)
TSG_DEP := c/kernel/gui/text.h c/lib/gfx/include/openlogit_display_blit.h
.PHONY: test-text-subpixel-gamma test-text-subpixel-gamma-negctl
$(TSG_DIR)/current: $(TSG_SRC) $(TSG_DEP)
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra $(TSG_INC) -o $@ $(TSG_SRC) -lm
$(TSG_DIR)/integer: $(TSG_SRC) $(TSG_DEP)
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -DTEXT_INTEGER_PEN $(TSG_INC) -o $@ $(TSG_SRC) -lm
test-text-subpixel-gamma-negctl: $(TSG_DIR)/integer
	@$(TSG_DIR)/integer fsroot >$(TSG_DIR)/integer.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -qx 'FAIL gap' $(TSG_DIR)/integer.log && ! grep '^FAIL' $(TSG_DIR)/integer.log | grep -qvx 'FAIL gap\|FAIL phase' || { cat $(TSG_DIR)/integer.log; echo "negctl did not fail with FAIL gap (and only gap/phase)"; exit 1; }; \
	 grep -x 'FAIL gap' $(TSG_DIR)/integer.log | head -1
test-text-subpixel-gamma: test-text-subpixel-gamma-negctl $(TSG_DIR)/current
	@$(TSG_DIR)/current fsroot >$(TSG_DIR)/current.log 2>&1; rc=$$?; \
	 cat $(TSG_DIR)/current.log; test $$rc -eq 0
ci-host: test-text-subpixel-gamma
