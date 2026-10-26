# text_draw_run_a + the SYS_GUI_TEXT_RUN2 handler, on a host display with the
# real text engine and fonts. The control builds the SAME sources and flags
# with -DTEXT_ALPHA_IGNORED (alpha dropped); "FAIL alpha128" must appear.
TKA_DIR := $(BUILD)/text-kernel-alpha
TKA_SRC := tests/unit/text_alpha_test.c c/kernel/gui/wm_draw2_text.c $(sort $(FONT_WEIGHT_SRC))
TKA_INC := $(FONT_WEIGHT_INC)
TKA_DEP := c/kernel/gui/text.h c/kernel/gui/wm_draw2.h c/kernel/gui/wm2_scale.h include/abi/logit_abi.h
.PHONY: test-text-kernel-alpha test-text-kernel-alpha-negctl
$(TKA_DIR)/current: $(TKA_SRC) $(TKA_DEP)
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra $(TKA_INC) -o $@ $(TKA_SRC) -lm
$(TKA_DIR)/ignored: $(TKA_SRC) $(TKA_DEP)
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -DTEXT_ALPHA_IGNORED $(TKA_INC) -o $@ $(TKA_SRC) -lm
test-text-kernel-alpha-negctl: $(TKA_DIR)/ignored
	@$(TKA_DIR)/ignored fsroot >$(TKA_DIR)/ignored.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -qx 'FAIL alpha128' $(TKA_DIR)/ignored.log || { cat $(TKA_DIR)/ignored.log; echo "negctl did not fail with FAIL alpha128"; exit 1; }; \
	 grep -x 'FAIL alpha128' $(TKA_DIR)/ignored.log | head -1
test-text-kernel-alpha: test-text-kernel-alpha-negctl $(TKA_DIR)/current
	@$(TKA_DIR)/current fsroot >$(TKA_DIR)/current.log 2>&1; rc=$$?; \
	 cat $(TKA_DIR)/current.log; test $$rc -eq 0
ci-host: test-text-kernel-alpha
