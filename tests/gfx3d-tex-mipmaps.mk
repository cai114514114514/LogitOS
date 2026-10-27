# gfx3d mip chain + LOD selection. The control builds with -DOL3D_MIP_FORCE_LOD0 (the
# sampler ignores the chain) and must fail 'FAIL aliasing'. Same sources and flags as the
# positive; only the -D differs.
G3MM_SRC := tests/unit/ol3d_mip_test.c $(OL3D_SRC) $(GFX_SRC)
G3MM_FLAGS := -O1 -g -Wall -Wextra -DOL3D_MIP_SCENE_HASH=0xe5714a8b2c9eb606ull $(GFX_INC) $(OL3D_INC)
G3MM_DEPS := $(G3MM_SRC) $(OL3D_HEADERS) $(GFX_HEADERS)

$(BUILD)/ol3d_mip_test: $(G3MM_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(G3MM_FLAGS) $(G3MM_SRC) -lm -o $@

$(BUILD)/ol3d_mip_negctl: $(G3MM_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(G3MM_FLAGS) -DOL3D_MIP_FORCE_LOD0 $(G3MM_SRC) -lm -o $@

test-gfx3d-tex-mipmaps-negctl: $(BUILD)/ol3d_mip_negctl
	@rc=0; $(BUILD)/ol3d_mip_negctl > $(BUILD)/gfx3d-tex-mipmaps-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/gfx3d-tex-mipmaps-negctl.log; exit 1; }
	@grep -q '^FAIL aliasing$$' $(BUILD)/gfx3d-tex-mipmaps-negctl.log || { echo "negctl did not fail with 'FAIL aliasing'"; cat $(BUILD)/gfx3d-tex-mipmaps-negctl.log; exit 1; }
	@echo 'gfx3d-tex-mipmaps control: LOD forced to 0 failed FAIL aliasing.'

test-gfx3d-tex-mipmaps: test-gfx3d-tex-mipmaps-negctl $(BUILD)/ol3d_mip_test
	$(BUILD)/ol3d_mip_test

ci-host: test-gfx3d-tex-mipmaps
.PHONY: test-gfx3d-tex-mipmaps test-gfx3d-tex-mipmaps-negctl
