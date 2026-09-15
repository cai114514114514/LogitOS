# Narrow normal arithmetic acceptance for reader code generation.  The baseline
# intentionally stays green: this is an optimization with unchanged semantics,
# not a behavior repair. Guest before/after timing is separate evidence.
WASM_FINITE_DIR = $(BUILD)/wasm-finite-arithmetic
WASM_FINITE_SRC = tests/unit/wasm_finite_arithmetic_test.c
WASM_FINITE_DEP = $(WASM_FINITE_SRC) $(wildcard c/lib/wasm/*.[ch])
WASM_FINITE_CF = -std=c11 -O2 -fno-math-errno -Wall -Wextra
$(WASM_FINITE_DIR)/baseline: $(WASM_FINITE_DEP)
	@mkdir -p $(WASM_FINITE_DIR)
	$(CC) $(WASM_FINITE_CF) -DWASM_FINITE_BASELINE -o $@ $(WASM_FINITE_SRC) -lm
$(WASM_FINITE_DIR)/current: $(WASM_FINITE_DEP)
	@mkdir -p $(WASM_FINITE_DIR)
	$(CC) $(WASM_FINITE_CF) -o $@ $(WASM_FINITE_SRC) -lm
.PHONY: test-wasm-finite-arithmetic
test-wasm-finite-arithmetic: $(WASM_FINITE_DIR)/baseline $(WASM_FINITE_DIR)/current
	@$(WASM_FINITE_DIR)/baseline
	@$(WASM_FINITE_DIR)/current
ci-host: test-wasm-finite-arithmetic
