# tests/wm-draw2-state.mk -- GUI ABI v2 clip/group stacks and BLIT2
# (c/kernel/gui/wm_draw2_state.c, wm-draw2-state).
#
# One host binary: the state module and $(GFX_SRC) against a fake wm2_env
# whose clip_push/clip_pop are fb_clip_push/pop's per-context stack on a fake
# surface, with a display rebind before every call (as fb_target +
# fb_display_cur do). See tests/unit/wm_draw2_state_test.c for the checks.
#
# THE CONTROL builds the same sources with -DWM_DRAW2_CLIP_DISPLAY_ONLY: the
# clip is set on e->d only, never through clip_push. It must exit 1 (a
# verdict, not a crash) with the PIXEL verdict of rebind-clip ('FAIL
# rebind-clip (nested ...: N pixels differ'): the rebind before the next call
# forgets the clip and the children paint outside it. A status mismatch alone
# does not count. Other checks that clip across calls fail too (same cause).

WDS_SRC := tests/unit/wm_draw2_state_test.c c/kernel/gui/wm_draw2_state.c
WDS_FLAGS := -O2 -g -Wall -Wextra

.PHONY: test-wm-draw2-state test-wm-draw2-state-negctl

test-wm-draw2-state-negctl:
	@mkdir -p $(BUILD)/wds
	@$(CC) $(WDS_FLAGS) -DWM_DRAW2_CLIP_DISPLAY_ONLY -o $(BUILD)/wds/wm_draw2_state_negctl $(WDS_SRC) $(GFX_SRC) $(HOST_INCDIRS) -lm
	@$(BUILD)/wds/wm_draw2_state_negctl > $(BUILD)/wds/negctl.log 2>&1; rc=$$?; \
	if [ $$rc -eq 0 ]; then \
		cat $(BUILD)/wds/negctl.log; \
		echo "NEGATIVE CONTROL FAILED: a display-only clip passes the rebind check"; exit 1; \
	elif [ $$rc -ne 1 ]; then \
		cat $(BUILD)/wds/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: exit $$rc (crash, not a verdict)"; exit 1; \
	elif ! grep -q '^FAIL rebind-clip (nested.*pixels differ' $(BUILD)/wds/negctl.log; then \
		cat $(BUILD)/wds/negctl.log; \
		echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL rebind-clip' pixel verdict"; exit 1; \
	else \
		echo "negative control ok: $$(grep -m1 '^FAIL rebind-clip (nested' $(BUILD)/wds/negctl.log)"; \
	fi

test-wm-draw2-state: test-wm-draw2-state-negctl
	@mkdir -p $(BUILD)/wds
	@$(CC) $(WDS_FLAGS) -o $(BUILD)/wds/wm_draw2_state_test $(WDS_SRC) $(GFX_SRC) $(HOST_INCDIRS) -lm
	@$(BUILD)/wds/wm_draw2_state_test

ci-host: test-wm-draw2-state
