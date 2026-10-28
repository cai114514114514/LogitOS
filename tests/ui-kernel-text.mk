# tests/ui-kernel-text.mk -- kernel chrome text helpers (backlog ui-kernel-text,
# design 3.6 / 5.2): c/kernel/gui/wm_text.c.
#
# HOST: tests/unit/ui_kernel_text_test.c links the real wm_text.c and the real
# text.c ($(FONT_WEIGHT_SRC), against fsroot/fonts) under a recording
# text_draw_run_a and a measure that can turn proportional (the shipped fonts
# are already tabular). Weight -> face, points -> device px at 100/125/150/200 %,
# the tabular cell, metric centring, shadow-then-ink order, the U+2026 cut and
# the cache re-key are all asserted as numbers. The control builds the SAME
# sources and flags plus -DUI_KTEXT_NEGCTL (digits at their natural advances)
# and must print `FAIL ktnum` and still reach the `done` line.
#
# text.c is not touched and gains no fb_* call; the harness defines fb_scale.

.PHONY: test-ui-kernel-text test-ui-kernel-text-negctl

UKT_DIR := $(BUILD)/ui-kernel-text
UKT_SRC := tests/unit/ui_kernel_text_test.c c/kernel/gui/wm_text.c \
           $(sort $(filter-out c/kernel/gui/text.c,$(FONT_WEIGHT_SRC)))
UKT_INC := $(FONT_WEIGHT_INC) -Ic/include
UKT_DEP := c/kernel/gui/text.c c/kernel/gui/text.h c/kernel/gui/wm_text.h \
           c/kernel/gui/wm2_scale.h c/include/ui/logit_tokens.h include/abi/logit_abi.h

$(UKT_DIR)/current: $(UKT_SRC) $(UKT_DEP)
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra $(UKT_INC) -o $@ $(UKT_SRC) -lm
$(UKT_DIR)/negctl: $(UKT_SRC) $(UKT_DEP)
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -DUI_KTEXT_NEGCTL $(UKT_INC) -o $@ $(UKT_SRC) -lm

test-ui-kernel-text-negctl: $(UKT_DIR)/negctl
	@rc=0; $(UKT_DIR)/negctl fsroot > $(UKT_DIR)/negctl.log 2>&1 || rc=$$?; \
	 if [ $$rc -ne 1 ]; then cat $(UKT_DIR)/negctl.log; \
	   echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	 grep -q '^FAIL ktnum ' $(UKT_DIR)/negctl.log || { cat $(UKT_DIR)/negctl.log; \
	   echo "NEGATIVE CONTROL FAILED: no 'FAIL ktnum' line"; exit 1; }; \
	 grep -q '^done [0-9]* checks$$' $(UKT_DIR)/negctl.log || { cat $(UKT_DIR)/negctl.log; \
	   echo "NEGATIVE CONTROL FAILED FOR ANOTHER REASON: the harness did not finish"; exit 1; }; \
	 echo "negative control ok: $$(grep -m1 '^FAIL ktnum ' $(UKT_DIR)/negctl.log)"

test-ui-kernel-text: test-ui-kernel-text-negctl $(UKT_DIR)/current
	@rc=0; $(UKT_DIR)/current fsroot > $(UKT_DIR)/current.log 2>&1 || rc=$$?; \
	 cat $(UKT_DIR)/current.log; test $$rc -eq 0

ci-host: test-ui-kernel-text
