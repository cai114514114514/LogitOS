# tests/ui-glass-adapt.mk -- P16: ADAPT_SMOOTH / ADAPT_FG reach the glass material
# from ring 3 through SYS_GUI_GLASS2 (c/kernel/gui/wm_draw2_fx.c) and
# SYS_GUI_BACKDROP (c/kernel/gui/wm_backdrop.c).
#
# HOST: tests/unit/ui_glass_adapt_test.c drives both real handlers over photo-like
# backdrops and compares with ol_display_liquid_glass_ex (checks listed in its
# header): ink contrast >= 4.5:1 with ADAPT_FG, unchanged without the flag,
# SMOOTH reaching the material, reduce transparency winning, unknown bits refused.
# THE CONTROLS drop the flag copy: -DUI_ADAPT_NEGCTL in wm_draw2_fx.c must print
# 'FAIL adapt-fg-glass2' and no backdrop failure; -DUI_ADAPT_NEGCTL_BD in
# wm_backdrop.c must print 'FAIL adapt-fg-backdrop' and no glass2 failure.

.PHONY: test-ui-glass-adapt test-ui-glass-adapt-negctl

UGA_DIR := $(BUILD)/ui-glass-adapt
UGA_SRC := tests/unit/ui_glass_adapt_test.c c/kernel/gui/wm_draw2_fx.c c/kernel/gui/wm_draw2_state.c \
           c/kernel/gui/wm_backdrop.c $(GFX_SRC)
UGA_FLAGS := -O1 -g -Wall -Wextra $(GFX_INC) $(HOST_INCDIRS)
UGA_DEPS := $(UGA_SRC) $(GFX_HEADERS) c/kernel/gui/wm_draw2.h c/kernel/gui/wm2_scale.h include/abi/logit_abi.h

$(UGA_DIR)/current: $(UGA_DEPS)
	@mkdir -p $(@D)
	$(CC) $(UGA_FLAGS) $(UGA_SRC) -lm -o $@
$(UGA_DIR)/negctl-glass2: $(UGA_DEPS)
	@mkdir -p $(@D)
	$(CC) $(UGA_FLAGS) -DUI_ADAPT_NEGCTL $(UGA_SRC) -lm -o $@
$(UGA_DIR)/negctl-backdrop: $(UGA_DEPS)
	@mkdir -p $(@D)
	$(CC) $(UGA_FLAGS) -DUI_ADAPT_NEGCTL_BD $(UGA_SRC) -lm -o $@

# $(1) binary, $(2) the one expected failing check, $(3) the check that must stay green
define UGA_CTL
	@rc=0; $(UGA_DIR)/$(1) > $(UGA_DIR)/$(1).log 2>&1 || rc=$$?; \
	 if [ $$rc -ne 1 ]; then cat $(UGA_DIR)/$(1).log; echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1"; exit 1; fi; \
	 grep -q '^FAIL $(2) ' $(UGA_DIR)/$(1).log || { cat $(UGA_DIR)/$(1).log; \
	   echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL $(2)'"; exit 1; }; \
	 if grep '^FAIL' $(UGA_DIR)/$(1).log | grep -q '^FAIL $(3)'; then cat $(UGA_DIR)/$(1).log; \
	   echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: $(3) failed too"; exit 1; fi; \
	 echo "ui-glass-adapt control ok: $$(grep -m1 '^FAIL $(2) ' $(UGA_DIR)/$(1).log)"
endef

test-ui-glass-adapt-negctl: $(UGA_DIR)/negctl-glass2 $(UGA_DIR)/negctl-backdrop
	$(call UGA_CTL,negctl-glass2,adapt-fg-glass2,\(adapt-fg-backdrop\|unflagged-\|smooth-backdrop\))
	$(call UGA_CTL,negctl-backdrop,adapt-fg-backdrop,\(adapt-fg-glass2\|unflagged-\|smooth-glass2\))

test-ui-glass-adapt: test-ui-glass-adapt-negctl $(UGA_DIR)/current
	$(UGA_DIR)/current

ci-host: test-ui-glass-adapt
