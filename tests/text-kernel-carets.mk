# LOGIT_TEXT_CARETS_QUERY: the real dispatch branch, text engine and fonts.
# The control builds the SAME sources and flags with -DTEXT_CARETS_BYTEWISE
# (a stop at every byte); its "utf8-split" assertion must fail.
TKC_DIR := $(BUILD)/text-kernel-carets
TKC_SRC := tests/unit/text_carets_test.c $(sort $(FONT_WEIGHT_SRC))
TKC_INC := $(FONT_WEIGHT_INC)
TKC_DEP := c/apps/text_metrics_wiring.inc c/kernel/gui/text_measure_dispatch.inc include/abi/logit_abi.h c/kernel/gui/text.h
.PHONY: test-text-kernel-carets test-text-kernel-carets-negctl
$(TKC_DIR)/current: $(TKC_SRC) $(TKC_DEP)
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra $(TKC_INC) -o $@ $(TKC_SRC) -lm
$(TKC_DIR)/bytewise: $(TKC_SRC) $(TKC_DEP)
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -DTEXT_CARETS_BYTEWISE $(TKC_INC) -o $@ $(TKC_SRC) -lm
test-text-kernel-carets-negctl: $(TKC_DIR)/bytewise
	@$(TKC_DIR)/bytewise fsroot >$(TKC_DIR)/bytewise.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -qx 'FAIL utf8-split' $(TKC_DIR)/bytewise.log || { cat $(TKC_DIR)/bytewise.log; echo "negctl did not fail with FAIL utf8-split"; exit 1; }; \
	 grep -x 'FAIL utf8-split' $(TKC_DIR)/bytewise.log | head -1
test-text-kernel-carets: test-text-kernel-carets-negctl $(TKC_DIR)/current
	@$(TKC_DIR)/current fsroot >$(TKC_DIR)/current.log 2>&1; rc=$$?; \
	 tail -5 $(TKC_DIR)/current.log; test $$rc -eq 0
ci-host: test-text-kernel-carets
