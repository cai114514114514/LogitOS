# tests/ui-aui-theme.mk -- the theme flip to 「澄」 in aui (ui-aui-theme; design
# 3.1-3.6 and section 4): the 23 legacy aui_t roles from logit_tokens per mode,
# ui.accent read and derived (with Settings' own call, re-read on EV_THEME and
# at most every 500 ms), the type / height / radius metrics, AUI_DISABLED_ALPHA
# 102, aui_text_in on the metric baseline, the single focus ring, aui_card,
# aui_glass on the REGULAR material.
#
# tests/unit/ui_aui_theme_test.c #includes the real aui.c over the aui-host
# transport with the REAL v2 handlers for SHADOW2/GLASS2 (v2_fx.c), PATH
# (v2_path.c) and TEXT_RUN2 (v2_text.c), and runs every case in a forked child.
#
# THE CONTROL builds the SAME sources and flags plus -DUI_THEME_NEGCTL (the
# pre-「澄」 old blue default accent, and the old store default not mapped to
# indigo). It must exit 1 with 'FAIL default-accent' and no other failing check;
# a crash, a link error or a pass is not that failure.

.PHONY: test-ui-aui-theme test-ui-aui-theme-negctl

UATH_SRC   := tests/unit/ui_aui_theme_test.c tests/unit/aui_host/aui_host.c tests/unit/aui_host/v2_fx.c \
              tests/unit/aui_host/v2_path.c tests/unit/aui_host/v2_text.c $(AUI_TL_SRC) $(GFX_SRC)
UATH_FLAGS := -O1 -g -Wall -Wextra -Wno-unused-function -DLOGIT_SYS_HOOK=aui_host_sys \
              -Itests/unit/aui_host -Ic/apps/gui -Ic/apps \
              $(GFX_INC) -Iinclude -Iinclude/abi -Ic/kernel/gui $(KGUI_INC)
UATH_DEPS  := $(UATH_SRC) c/apps/gui/aui.c c/apps/gui/aui.h $(wildcard c/apps/gui/aui_*.inc c/apps/gui/aui_*.h) \
              $(wildcard c/include/ui/*.h) c/kernel/gui/wm_draw2_fx.c c/kernel/gui/wm_draw2_path.c \
              c/kernel/gui/wm_draw2_text.c tests/unit/aui_host/aui_host.h $(GFX_HEADERS)

$(BUILD)/ui_aui_theme/test: $(UATH_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UATH_FLAGS) $(UATH_SRC) -lm -o $@

$(BUILD)/ui_aui_theme/negctl: $(UATH_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) $(UATH_FLAGS) -DUI_THEME_NEGCTL $(UATH_SRC) -lm -o $@

test-ui-aui-theme-negctl: $(BUILD)/ui_aui_theme/negctl
	@rc=0; $(BUILD)/ui_aui_theme/negctl > $(BUILD)/ui_aui_theme/negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(BUILD)/ui_aui_theme/negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1 (a verdict, not a crash or a pass)"; exit 1; fi; \
	grep -q '^FAIL default-accent' $(BUILD)/ui_aui_theme/negctl.log || { cat $(BUILD)/ui_aui_theme/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL default-accent' line"; exit 1; }; \
	if grep '^FAIL' $(BUILD)/ui_aui_theme/negctl.log | grep -v '^FAIL ui-aui-theme:' | grep -qv '^FAIL default-accent'; then \
		cat $(BUILD)/ui_aui_theme/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than default-accent failed"; exit 1; fi; \
	echo "negative control ok: the old blue default fails: $$(grep -m1 '^FAIL default-accent' $(BUILD)/ui_aui_theme/negctl.log | cut -c1-120)"

test-ui-aui-theme: test-ui-aui-theme-negctl $(BUILD)/ui_aui_theme/test
	@$(BUILD)/ui_aui_theme/test

ci-host: test-ui-aui-theme
