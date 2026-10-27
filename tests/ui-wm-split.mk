# tests/ui-wm-split.mk -- the window manager split into per-surface files
# (backlog ui-wm-split): wm.c keeps the compositor, damage, input dispatch,
# syscalls and the main loop; each chrome surface is wm_<m>.c + wm_<m>.h.
# tools/ui_wm_split_check.py checks that each surface's functions are defined
# once, in their own file and not in wm.c; that the source-text negative
# controls' needles (qmp_damage, qmp_window, qmp_motion, qmp_flash) and every
# #if reading them stay in wm.c; and that no wm*.c but wm_glass.c calls
# fb_liquid_glass* / profiled_glass*. The control (--fixture) checks a copy in
# which wm.c also defines draw_dock and must log "FAIL placement draw_dock".

UIWS_CHECK := python3 -I tools/ui_wm_split_check.py
UIWS_SRC := $(wildcard c/kernel/gui/wm.c c/kernel/gui/wm_*.c c/kernel/gui/wm_*.h)

test-ui-wm-split-negctl: tools/ui_wm_split_check.py $(UIWS_SRC)
	@mkdir -p $(BUILD)
	@rc=0; $(UIWS_CHECK) --root . --fixture > $(BUILD)/ui-wm-split-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/ui-wm-split-negctl.log; exit 1; }
	@grep -q '^FAIL placement draw_dock ' $(BUILD)/ui-wm-split-negctl.log || { echo "negctl did not fail with 'FAIL placement draw_dock'"; cat $(BUILD)/ui-wm-split-negctl.log; exit 1; }
	@test "$$(grep -c '^FAIL ' $(BUILD)/ui-wm-split-negctl.log)" = 1 || { echo "negctl failed for another reason too"; cat $(BUILD)/ui-wm-split-negctl.log; exit 1; }
	@echo 'ui-wm-split control: draw_dock defined in wm.c rejected, failed FAIL placement draw_dock.'

test-ui-wm-split: test-ui-wm-split-negctl
	$(UIWS_CHECK) --root .

ci-host: test-ui-wm-split
.PHONY: test-ui-wm-split test-ui-wm-split-negctl
