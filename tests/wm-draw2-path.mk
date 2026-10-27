# SYS_GUI_PATH handler (c/kernel/gui/wm_draw2_path.c) against direct
# ol_display_fill_path / stroke_path, with a host env whose copyin enforces the
# bounds of the buffers the test declared. test-wm-draw2-path: oracle equality,
# a malformed-input table (status + target CRC), 125/150/200% scale and SNAP
# edges, OL_UNSUPPORTED cases, per-display scratch.
#
# THE CONTROL builds the same sources with -DWM_DRAW2_NO_VALIDATE (no verb
# sequence or count checks). The malformed table must then draw instead of
# refusing: 'FAIL malformed <case> touched pixels'. The host copyin keeps the
# unvalidated handler in bounds, so it draws rather than crashes; exit 1 only,
# and only malformed-table failures, count as the expected failure.
WDP_SRC := tests/unit/wm_draw2_path_test.c c/kernel/gui/wm_draw2_path.c $(GFX_SRC)
WDP_FLAGS := -O1 -g -Wall -Wextra $(GFX_INC) $(HOST_INCDIRS)
WDP_DEPS := $(WDP_SRC) $(GFX_HEADERS) c/kernel/gui/wm_draw2.h c/kernel/gui/wm2_scale.h include/abi/logit_abi.h

$(BUILD)/wm_draw2_path_test: $(WDP_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(WDP_FLAGS) $(WDP_SRC) -lm -o $@

$(BUILD)/wm_draw2_path_negctl: $(WDP_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(WDP_FLAGS) -DWM_DRAW2_NO_VALIDATE $(WDP_SRC) -lm -o $@

test-wm-draw2-path-negctl: $(BUILD)/wm_draw2_path_negctl
	@rc=0; $(BUILD)/wm_draw2_path_negctl > $(BUILD)/wm-draw2-path-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/wm-draw2-path-negctl.log; exit 1; }
	@grep -q '^FAIL malformed .* touched pixels' $(BUILD)/wm-draw2-path-negctl.log || { echo "negctl did not fail with 'FAIL malformed <case> touched pixels'"; cat $(BUILD)/wm-draw2-path-negctl.log; exit 1; }
	@! grep '^FAIL' $(BUILD)/wm-draw2-path-negctl.log | grep -qv '^FAIL malformed' || { echo "negctl failed a check other than the malformed table"; cat $(BUILD)/wm-draw2-path-negctl.log; exit 1; }
	@echo "wm-draw2-path control: unvalidated handler failed: $$(grep -m1 '^FAIL malformed' $(BUILD)/wm-draw2-path-negctl.log)"

test-wm-draw2-path: test-wm-draw2-path-negctl $(BUILD)/wm_draw2_path_test
	$(BUILD)/wm_draw2_path_test

ci-host: test-wm-draw2-path

# Budget: one wm2_path rrect incl. copy-in <= 1.5x the 7 legacy rrect calls for the same shape.
WDPB_SRC := tests/unit/wm_draw2_path_bench.c c/kernel/gui/wm_draw2_path.c $(GFX_SRC)
WDPB_FLAGS := -O2 -g -Wall -Wextra -Itests/unit $(GFX_INC) $(HOST_INCDIRS)

$(BUILD)/wm_draw2_path_bench: $(WDPB_SRC) $(GFX_HEADERS) c/kernel/gui/wm_draw2.h tests/unit/bench_budget.h
	@mkdir -p $(dir $@)
	$(CC) $(WDPB_FLAGS) $(WDPB_SRC) -lm -o $@

test-wm-draw2-path-budget: $(BUILD)/wm_draw2_path_bench
	$(BUILD)/wm_draw2_path_bench

test-gfx-budget: test-wm-draw2-path-budget

.PHONY: test-wm-draw2-path test-wm-draw2-path-negctl test-wm-draw2-path-budget
