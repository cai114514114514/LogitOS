# This gate links the complete display renderer, including both glass field
# paths. A lookup-table probe alone cannot show readable material over pixels.
GLASS_MATERIAL_TEST := tests/unit/glass_material_test.c
GLASS_MATERIAL_FLAGS := -O1 -g -Wall -Wextra $(GFX_INC)

$(BUILD)/glass_material_test: $(GLASS_MATERIAL_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GLASS_MATERIAL_FLAGS) $< $(GFX_SRC) -lm -o $@

$(BUILD)/glass_material_sanitize: $(GLASS_MATERIAL_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GLASS_MATERIAL_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $< $(GFX_SRC) -lm -o $@

$(BUILD)/glass_material_static: $(GLASS_MATERIAL_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GLASS_MATERIAL_FLAGS) -DGLASS_STATIC_TINT $< $(GFX_SRC) -lm -o $@

$(BUILD)/glass_material_slow: $(GLASS_MATERIAL_TEST) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(GLASS_MATERIAL_FLAGS) -DGLASS_FIELD_SLOW $< $(GFX_SRC) -lm -o $@

test-glass-material-negctl: $(BUILD)/glass_material_static
	@rc=0; $< > $(BUILD)/glass-material-static.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg '^FAIL light neutral body keeps content contrast: gray=0 ' $(BUILD)/glass-material-static.log
	@rg '^FAIL dark neutral body keeps content contrast: gray=255 ' $(BUILD)/glass-material-static.log
	@echo 'Glass material control: fixed tint observed failing both light and dark content contrast.'

test-glass-material-parity: $(BUILD)/glass_material_test $(BUILD)/glass_material_slow
	$(BUILD)/glass_material_test > $(BUILD)/glass-material-fast.log
	$(BUILD)/glass_material_slow > $(BUILD)/glass-material-slow.log
	@cat $(BUILD)/glass-material-fast.log
	@rg '^glass patterned hash: [0-9a-f]{16}$$' $(BUILD)/glass-material-fast.log > $(BUILD)/glass-material-fast.hash
	@rg '^glass patterned hash: [0-9a-f]{16}$$' $(BUILD)/glass-material-slow.log > $(BUILD)/glass-material-slow.hash
	@cmp $(BUILD)/glass-material-fast.hash $(BUILD)/glass-material-slow.hash
	@echo 'Glass material parity: complete patterned scenes match between fast and general rendering.'

test-glass-material: test-glass-material-negctl test-glass-material-parity $(BUILD)/glass_material_sanitize
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/glass_material_sanitize

ci-host: test-glass-material
.PHONY: test-glass-material test-glass-material-negctl test-glass-material-parity
