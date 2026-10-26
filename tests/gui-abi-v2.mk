# tests/gui-abi-v2.mk -- GUI ABI v2 transport and stubs (gui-abi-v2).
#
# One host binary, linked like test-fb-clip (fb.c + glass.c + $(GFX_SRC)) plus
# the v2 router and every module stub. text.c is not linked: the test defines
# the carets INFO constant itself. See tests/unit/gui_abi_v2_test.c for the
# checks.
#
# THE CONTROL builds the same sources with -DGUI_ABI_NEGCTL, which makes
# wm2_pt truncate negatives toward zero. It must fail, it must fail with
# 'FAIL scale-parity', it must fail ONLY that check (a broken build, a crash
# or a different FAIL is not the shear being detected), and it must exit 1
# rather than die on a signal.

GAV2_SRC := tests/unit/gui_abi_v2_test.c tests/unit/gui_abi_v2_wire.c \
            c/kernel/gui/fb/fb.c c/kernel/gui/fb/glass.c \
            c/kernel/gui/wm_draw2.c c/kernel/gui/wm_draw2_path.c \
            c/kernel/gui/wm_draw2_state.c c/kernel/gui/wm_draw2_text.c \
            c/kernel/gui/wm_draw2_fx.c c/kernel/gui/wm_backdrop.c

.PHONY: test-gui-abi-v2 test-gui-abi-v2-negctl

test-gui-abi-v2-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DGUI_ABI_NEGCTL -o $(BUILD)/gui_abi_v2_negctl $(GAV2_SRC) $(GFX_SRC) $(HOST_INCDIRS) -lm
	@$(BUILD)/gui_abi_v2_negctl > $(BUILD)/gui_abi_v2_negctl.log 2>&1; rc=$$?; \
	if [ $$rc -eq 0 ]; then \
		cat $(BUILD)/gui_abi_v2_negctl.log; \
		echo "NEGATIVE CONTROL FAILED: truncating wm2_pt passes the fb_pt cross-check"; exit 1; \
	elif [ $$rc -ne 1 ]; then \
		cat $(BUILD)/gui_abi_v2_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: exit $$rc (crash, not a verdict)"; exit 1; \
	elif ! grep -q '^FAIL scale-parity' $(BUILD)/gui_abi_v2_negctl.log; then \
		cat $(BUILD)/gui_abi_v2_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL scale-parity'"; exit 1; \
	elif grep '^FAIL' $(BUILD)/gui_abi_v2_negctl.log | grep -qv '^FAIL scale-parity'; then \
		cat $(BUILD)/gui_abi_v2_negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than scale-parity failed"; exit 1; \
	else \
		echo "negative control ok: $$(grep '^FAIL scale-parity' $(BUILD)/gui_abi_v2_negctl.log)"; \
	fi

test-gui-abi-v2: test-gui-abi-v2-negctl
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/gui_abi_v2_test $(GAV2_SRC) $(GFX_SRC) $(HOST_INCDIRS) -lm
	@$(BUILD)/gui_abi_v2_test

ci-host: test-gui-abi-v2
