SCENE_TEST_DEPS := tests/unit/openlogit_scene_test.c examples/openlogit/studio/shaders.h $(OL3D_SRC) $(OL3D_HEADERS) $(GFX_SRC) $(GFX_HEADERS)
$(BUILD)/openlogit_scene_test: $(SCENE_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $(OL3D_INC) $< $(OL3D_SRC) $(GFX_SRC) -lm -o $@
$(BUILD)/openlogit_scene_neg: $(SCENE_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -DOPENLOGIT_SHADER_DISABLED $(GFX_INC) $(OL3D_INC) $< $(OL3D_SRC) $(GFX_SRC) -lm -o $@
$(BUILD)/openlogit_scene_sanitize: $(SCENE_TEST_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC) $(OL3D_INC) $< $(OL3D_SRC) $(GFX_SRC) -lm -o $@
test-openlogit-scene-neg: $(BUILD)/openlogit_scene_neg
	@rc=0; $< > $(BUILD)/openlogit-scene-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL scene fragment executes' $(BUILD)/openlogit-scene-neg.log
test-openlogit-scene: test-openlogit-scene-neg $(BUILD)/openlogit_scene_test $(BUILD)/openlogit_scene_sanitize
	$(BUILD)/openlogit_scene_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/openlogit_scene_sanitize
test-openlogit: test-openlogit-scene
ci-host: test-openlogit-scene
.PHONY: test-openlogit-scene test-openlogit-scene-neg
