# Only the VM and material pass are linked here: accidental reliance on the 3D
# triangle/depth context fails the link, preserving the SDK module boundary.
OL_MATERIAL_SRC := $(wildcard c/lib/gfx3d/material/*.c c/lib/gfx3d/shader/ir/*.c c/lib/gfx3d/shader/runtime/*.c) $(GFX_SRC)
$(BUILD)/openlogit_material_test: tests/unit/openlogit_material_test.c $(OL_MATERIAL_SRC) $(OL3D_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $(OL3D_INC) $< $(OL_MATERIAL_SRC) -lm -o $@
$(BUILD)/openlogit_material_neg: tests/unit/openlogit_material_test.c $(OL_MATERIAL_SRC) $(OL3D_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -DOPENLOGIT_SHADER_DISABLED $(GFX_INC) $(OL3D_INC) $< $(OL_MATERIAL_SRC) -lm -o $@
$(BUILD)/openlogit_material_sanitize: tests/unit/openlogit_material_test.c $(OL_MATERIAL_SRC) $(OL3D_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC) $(OL3D_INC) $< $(OL_MATERIAL_SRC) -lm -o $@
test-openlogit-material-neg: $(BUILD)/openlogit_material_neg
	@rc=0; $< > $(BUILD)/openlogit-material-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL material UV centers' $(BUILD)/openlogit-material-neg.log
test-openlogit-material: test-openlogit-material-neg $(BUILD)/openlogit_material_test $(BUILD)/openlogit_material_sanitize
	$(BUILD)/openlogit_material_test
	@echo "Material sanitizer: ASan/UBSan; leak detection excluded (unsupported by Apple ASan)."
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/openlogit_material_sanitize
ci-host: test-openlogit-material
.PHONY: test-openlogit-material test-openlogit-material-neg
