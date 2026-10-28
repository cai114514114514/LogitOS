# tests/ui-wm-abi.mk -- ui-wm-abi: the shell ABI (design P7, P10, P11, P16,
# 5.10, P3): SYS_GUI_ICON2 and symbol set C with the Logit mark, wm_tok(),
# WINS_SET_TITLEBAR, WINS_SET_SIZE, SYS_UI_DARK_AT.
#
#   test-ui-wm-abi         both host gates below (each runs its control first)
#   test-ui-wm-abi-icons   set C + the Logit mark through the real icons.c and
#                          gfx engine (tests/unit/ui_wm_abi_icons_test.c);
#                          the control (-DUI_ICONS_NEGCTL) empties SPARKLES and
#                          must log exactly 'FAIL icon-nonempty SPARKLES'
#   test-ui-wm-abi-theme   wm_tok() through the real wm_theme.c
#                          (tests/unit/ui_wm_abi_theme_test.c); the control
#                          (-DUI_WMTOK_NEGCTL) never derives the accent and
#                          must log only 'FAIL wm-accent' lines
#   test-ui-wm-abi-guest   tests/qmp/qmp_ui_abi.py on the isolated image with
#                          the probes in /bin; its control runs the panel probe
#                          built -DUI_ABI_NEGCTL (it never calls WINS_SET_SIZE)
#                          and must fail exactly 'panel-resize'
#
# gui-abi-v2 links the v2 router (wm_draw2.c), which now routes SYS_GUI_ICON2
# to wm_draw2_icon.c: that file joins its explicit source list here, where
# the number is added (this fragment is included after tests/gui-abi-v2.mk).
GAV2_SRC += c/kernel/gui/wm_draw2_icon.c

.PHONY: test-ui-wm-abi test-ui-wm-abi-icons test-ui-wm-abi-icons-negctl \
        test-ui-wm-abi-theme test-ui-wm-abi-theme-negctl \
        test-ui-wm-abi-guest test-ui-wm-abi-guest-negctl

UWA_DIR := $(BUILD)/uwa

# ---- host: icon set C and the Logit mark -----------------------------------
UWA_ICONS_SRC := tests/unit/ui_wm_abi_icons_test.c c/kernel/gui/icons.c $(GFX_SRC)
UWA_ICONS_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 -DLOGIT_SYS_HOOK=uwa_test_sys \
	-Ic/kernel/gui -Ic/kernel/gui/fb -Ic/kernel/cpu -Ic/apps -Iinclude -Iinclude/abi $(GFX_INC)
UWA_ICONS_DEPS := $(UWA_ICONS_SRC) $(GFX_HEADERS) c/kernel/gui/icons.h c/apps/logit.h

$(UWA_DIR)/icons_test: $(UWA_ICONS_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(UWA_ICONS_FLAGS) $(UWA_ICONS_SRC) -lm -o $@

$(UWA_DIR)/icons_negctl: $(UWA_ICONS_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(UWA_ICONS_FLAGS) -DUI_ICONS_NEGCTL $(UWA_ICONS_SRC) -lm -o $@

test-ui-wm-abi-icons-negctl: $(UWA_DIR)/icons_negctl
	@rc=0; $(UWA_DIR)/icons_negctl > $(UWA_DIR)/icons-negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(UWA_DIR)/icons-negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1 (a verdict, not a crash or a pass)"; exit 1; fi; \
	grep -q '^FAIL icon-nonempty SPARKLES' $(UWA_DIR)/icons-negctl.log || { cat $(UWA_DIR)/icons-negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL icon-nonempty SPARKLES'"; exit 1; }; \
	if grep '^FAIL' $(UWA_DIR)/icons-negctl.log | grep -qv '^FAIL icon-nonempty SPARKLES'; then \
		cat $(UWA_DIR)/icons-negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: another check failed"; exit 1; fi; \
	echo "negative control ok: $$(grep -m1 '^FAIL icon-nonempty SPARKLES' $(UWA_DIR)/icons-negctl.log)"

test-ui-wm-abi-icons: test-ui-wm-abi-icons-negctl $(UWA_DIR)/icons_test
	$(UWA_DIR)/icons_test

# ---- host: wm_tok() --------------------------------------------------------
UWA_THEME_SRC := tests/unit/ui_wm_abi_theme_test.c c/kernel/gui/wm_theme.c
UWA_THEME_FLAGS := -O1 -g -Wall -Wextra -Ic/kernel/gui -Ic/kernel/core -Ic/include/ui -Iinclude/abi
UWA_THEME_DEPS := $(UWA_THEME_SRC) c/kernel/gui/wm_theme.h c/include/ui/logit_tokens.h

$(UWA_DIR)/theme_test: $(UWA_THEME_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(UWA_THEME_FLAGS) $(UWA_THEME_SRC) -o $@

$(UWA_DIR)/theme_negctl: $(UWA_THEME_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(UWA_THEME_FLAGS) -DUI_WMTOK_NEGCTL $(UWA_THEME_SRC) -o $@

test-ui-wm-abi-theme-negctl: $(UWA_DIR)/theme_negctl
	@rc=0; $(UWA_DIR)/theme_negctl > $(UWA_DIR)/theme-negctl.log 2>&1 || rc=$$?; \
	if [ $$rc -ne 1 ]; then cat $(UWA_DIR)/theme-negctl.log; \
		echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1 (a verdict, not a crash or a pass)"; exit 1; fi; \
	grep -q '^FAIL wm-accent' $(UWA_DIR)/theme-negctl.log || { cat $(UWA_DIR)/theme-negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL wm-accent'"; exit 1; }; \
	if grep '^FAIL' $(UWA_DIR)/theme-negctl.log | grep -qv '^FAIL wm-accent '; then \
		cat $(UWA_DIR)/theme-negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than wm-accent failed"; exit 1; fi; \
	echo "negative control ok: $$(grep -m1 '^FAIL wm-accent' $(UWA_DIR)/theme-negctl.log)"

test-ui-wm-abi-theme: test-ui-wm-abi-theme-negctl $(UWA_DIR)/theme_test
	$(UWA_DIR)/theme_test

test-ui-wm-abi: test-ui-wm-abi-icons test-ui-wm-abi-theme

ci-host: test-ui-wm-abi

# ---- guest -----------------------------------------------------------------
# Three probes from one source (tests/unit/ui_wm_abi_probe.c), plain logit.h
# clients, installed into /bin of the isolated test image only when
# UI_WM_ABI_GUEST=1 (the openlogit_scroll.mk way; never FS_SPECS).
UWA_PROBE_DEPS := tests/unit/ui_wm_abi_probe.c c/apps/logit.h include/abi/logit_abi.h

$(UWA_DIR)/crt0.o: c/apps/crt0.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@
$(UWA_DIR)/probe.o: $(UWA_PROBE_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@
$(UWA_DIR)/panel.o: $(UWA_PROBE_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -DUI_ABI_PANEL -c $< -o $@
$(UWA_DIR)/panel_neg.o: $(UWA_PROBE_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -DUI_ABI_PANEL -DUI_ABI_NEGCTL -c $< -o $@
$(UWA_DIR)/probe.elf: $(UWA_DIR)/crt0.o $(UWA_DIR)/probe.o
	$(LD) -nostdlib -e _start -Ttext=0x66000000 -o $@ $^
$(UWA_DIR)/panel.elf: $(UWA_DIR)/crt0.o $(UWA_DIR)/panel.o
	$(LD) -nostdlib -e _start -Ttext=0x66000000 -o $@ $^
$(UWA_DIR)/panel_neg.elf: $(UWA_DIR)/crt0.o $(UWA_DIR)/panel_neg.o
	$(LD) -nostdlib -e _start -Ttext=0x66000000 -o $@ $^
$(UWA_DIR)/installed.stamp: $(UWA_DIR)/probe.elf $(UWA_DIR)/panel.elf $(UWA_DIR)/panel_neg.elf $(BUILD)/sdk/installed.stamp
	@mkdir -p $(SYSROOT)/bin
	cp $(UWA_DIR)/probe.elf $(SYSROOT)/bin/ui-abi-probe
	cp $(UWA_DIR)/panel.elf $(SYSROOT)/bin/ui-abi-panel
	cp $(UWA_DIR)/panel_neg.elf $(SYSROOT)/bin/ui-abi-panel-neg
	@touch $@
ifeq ($(UI_WM_ABI_GUEST),1)
$(DISK): $(UWA_DIR)/installed.stamp
endif

test-ui-wm-abi-guest-negctl:
	@$(MAKE) --no-print-directory BUILD=$(BUILD) UI_WM_ABI_GUEST=1 $(ISO) $(DISK) >/dev/null
	@rc=0; python3 tests/qmp/qmp_ui_abi.py --iso $(ISO) --disk $(DISK) --only panel \
		--panel /bin/ui-abi-panel-neg --out $(UWA_DIR)/guest-ctl > $(UWA_DIR)/guest-ctl.log 2>&1 || rc=$$?; \
	cat $(UWA_DIR)/guest-ctl.log; \
	if [ $$rc -ne 1 ]; then echo "NEGATIVE CONTROL FAILED: exit $$rc, expected 1 (a verdict)"; exit 1; fi; \
	grep -q '^FAIL panel-resize' $(UWA_DIR)/guest-ctl.log || { \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL panel-resize'"; exit 1; }; \
	if grep '^FAIL' $(UWA_DIR)/guest-ctl.log | grep -qv '^FAIL panel-resize'; then \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: another check failed"; exit 1; fi; \
	echo "negative control ok: $$(grep -m1 '^FAIL panel-resize' $(UWA_DIR)/guest-ctl.log)"

test-ui-wm-abi-guest: test-ui-wm-abi-guest-negctl
	@$(MAKE) --no-print-directory BUILD=$(BUILD) UI_WM_ABI_GUEST=1 $(ISO) $(DISK) >/dev/null
	@python3 tests/qmp/qmp_ui_abi.py --iso $(ISO) --disk $(DISK) --out $(UWA_DIR)/guest

ci-boot: test-ui-wm-abi-guest
