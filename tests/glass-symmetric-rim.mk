# tests/glass-symmetric-rim.mk -- OL_GLASS_RIM_SYMMETRIC: a view-independent
# rim ring, variance-gated environment reflection and the 2px edge floor for
# panels under 40px. Built with -DGFX_NCTX=2 and no fb.c; the test defines
# gfx_ctx_hook. The control forces the directional rim for every material, so
# the 32-ray uniformity check over a flat panel must fail.
GSR_TEST := tests/unit/glass_rim_test.c
GSR_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 -DGLASS_RIM_TEST_GAIN $(GFX_INC)

$(BUILD)/glass_rim_test: $(GSR_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GSR_FLAGS) $< $(GFX_SRC) -lm -o $@

$(BUILD)/glass_rim_directional: $(GSR_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GSR_FLAGS) -DGLASS_RIM_FORCE_DIRECTIONAL $< $(GFX_SRC) -lm -o $@

# Exit 1 (checks failed, not a crash), the uniformity failure present, and no
# failure outside the symmetric-only checks: DIRECTIONAL identity, reduce
# transparency and the striped refraction must still pass under the control.
# rim-gain is symmetric-only: the directional rim never reads the gain.
test-glass-symmetric-rim-negctl: $(BUILD)/glass_rim_directional
	@rc=0; $(BUILD)/glass_rim_directional > $(BUILD)/glass-rim-directional.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg '^FAIL rim-uniform 80x28 r14$$' $(BUILD)/glass-rim-directional.log
	@! rg '^FAIL ' $(BUILD)/glass-rim-directional.log | rg -v '^FAIL (rim-uniform|rim-dark|rim-gain-busy-[a-z]+|edge-floor)( |$$)'
	@echo 'Glass rim control: the directional rim failed the flat-panel uniformity check.'

test-glass-symmetric-rim: test-glass-symmetric-rim-negctl $(BUILD)/glass_rim_test
	$(BUILD)/glass_rim_test

ci-host: test-glass-symmetric-rim
.PHONY: test-glass-symmetric-rim test-glass-symmetric-rim-negctl
