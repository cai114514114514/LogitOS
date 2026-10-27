# font-weights: CSS weight 100..900 -> the baked UI face slots (400/500/600/700).
# The positive and the control link the SAME sources and flags; the control
# differs only by -DTEXT_WEIGHT_BOOL (any weight > 400 is bold), which must fail
# the 'weight-order' check and nothing else's absence.
FWM_SRC := tests/unit/font_weight_map_test.c $(sort $(FONT_WEIGHT_SRC))

$(BUILD)/font_weight_map_test: $(FWM_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ $(FWM_SRC) $(FONT_WEIGHT_INC) -lm

$(BUILD)/font_weight_map_test_neg: $(FWM_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DTEXT_WEIGHT_BOOL -o $@ $(FWM_SRC) $(FONT_WEIGHT_INC) -lm

test-font-weights-negctl: $(BUILD)/font_weight_map_test_neg $(FONTS)
	@if $(BUILD)/font_weight_map_test_neg fsroot c/kernel/gui/wm_draw2_text.c \
	    >$(BUILD)/font-weights-negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: boolean weights passed the ordering check"; exit 1; \
	 elif grep -q '^  FAIL  weight-order' $(BUILD)/font-weights-negctl.log; then \
	    echo "negctl ok: $$(grep -m1 '^  FAIL  weight-order' $(BUILD)/font-weights-negctl.log)"; \
	 else \
	    echo "NEGATIVE CONTROL failed for another reason:"; cat $(BUILD)/font-weights-negctl.log; exit 1; \
	 fi

test-font-weights: test-font-weights-negctl
test-font-weights: $(BUILD)/font_weight_map_test $(FONTS)
	@$(BUILD)/font_weight_map_test fsroot c/kernel/gui/wm_draw2_text.c

ci-host: test-font-weights
.PHONY: test-font-weights test-font-weights-negctl
