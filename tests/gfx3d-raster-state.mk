# gfx3d pipeline raster state (blend_op, depth_func, depth write mask, cull). The
# control builds with -DOL3D_IGNORE_DEPTH_FUNC (every func degrades to LESS) and must
# fail 'FAIL depth-func'. Same sources and flags as the positive; only the -D differs.
G3RS_SRC := tests/unit/ol3d_state_test.c $(OL3D_SRC) $(GFX_SRC)
G3RS_FLAGS := -O1 -g -Wall -Wextra -DOL3D_STATE_SCENE_HASH=0x166547a6c644989bull $(GFX_INC) $(OL3D_INC)
G3RS_DEPS := $(G3RS_SRC) $(OL3D_HEADERS) $(GFX_HEADERS)

$(BUILD)/ol3d_state_test: $(G3RS_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(G3RS_FLAGS) $(G3RS_SRC) -lm -o $@

$(BUILD)/ol3d_state_negctl: $(G3RS_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(G3RS_FLAGS) -DOL3D_IGNORE_DEPTH_FUNC $(G3RS_SRC) -lm -o $@

test-gfx3d-raster-state-negctl: $(BUILD)/ol3d_state_negctl
	@rc=0; $(BUILD)/ol3d_state_negctl > $(BUILD)/gfx3d-raster-state-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/gfx3d-raster-state-negctl.log; exit 1; }
	@grep -q '^FAIL depth-func$$' $(BUILD)/gfx3d-raster-state-negctl.log || { echo "negctl did not fail with 'FAIL depth-func'"; cat $(BUILD)/gfx3d-raster-state-negctl.log; exit 1; }
	@echo 'gfx3d-raster-state control: depth_func ignored failed FAIL depth-func.'

test-gfx3d-raster-state: test-gfx3d-raster-state-negctl $(BUILD)/ol3d_state_test
	$(BUILD)/ol3d_state_test

ci-host: test-gfx3d-raster-state
.PHONY: test-gfx3d-raster-state test-gfx3d-raster-state-negctl
