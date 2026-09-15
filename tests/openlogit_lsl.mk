# Compile real source and execute the emitted native program. Disabling the VM
# must fail the same independent numeric oracle used by the positive test.
$(BUILD)/openlogit_lsl_test: tests/unit/openlogit_lsl_test.c $(OL3D_SRC) $(OL3D_HEADERS) $(GFX_SRC)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $(OL3D_INC) $< $(OL3D_SRC) $(GFX_SRC) -lm -o $@
$(BUILD)/openlogit_lsl_neg: tests/unit/openlogit_lsl_test.c $(OL3D_SRC) $(OL3D_HEADERS) $(GFX_SRC)
	@mkdir -p $(dir $@)
	$(CC) -O2 -DOPENLOGIT_SHADER_DISABLED $(GFX_INC) $(OL3D_INC) $< $(OL3D_SRC) $(GFX_SRC) -lm -o $@
$(BUILD)/openlogit_lsl_sanitize: tests/unit/openlogit_lsl_test.c $(OL3D_SRC) $(OL3D_HEADERS) $(GFX_SRC)
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC) $(OL3D_INC) $< $(OL3D_SRC) $(GFX_SRC) -lm -o $@
test-openlogit-lsl-neg: $(BUILD)/openlogit_lsl_neg
	@rc=0; $< > $(BUILD)/openlogit-lsl-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL LSL constructor executes' $(BUILD)/openlogit-lsl-neg.log
test-openlogit-lsl: test-openlogit-lsl-neg $(BUILD)/openlogit_lsl_test $(BUILD)/openlogit_lsl_sanitize
	$(BUILD)/openlogit_lsl_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/openlogit_lsl_sanitize
test-openlogit: test-openlogit-lsl
ci-host: test-openlogit-lsl
.PHONY: test-openlogit-lsl test-openlogit-lsl-neg
