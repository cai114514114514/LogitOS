# Blend modes and Porter-Duff operators (c/lib/gfx/raster/gfx_compose.c, gfx_paint.c
# span dispatch, adapters/display_path.c). The control builds with -DGFX_BLEND_NEGCTL
# (every operator degrades to src-over) and must fail 'FAIL MULTIPLY'.
GBO_SRC := tests/unit/gfx_blend_test.c $(GFX_SRC)
GBO_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)
GBO_DEPS := $(GBO_SRC) $(GFX_HEADERS)

$(BUILD)/gfx_blend_test: $(GBO_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(GBO_FLAGS) $(GBO_SRC) -lm -o $@

$(BUILD)/gfx_blend_negctl: $(GBO_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(GBO_FLAGS) -DGFX_BLEND_NEGCTL $(GBO_SRC) -lm -o $@

test-gfx-blend-ops-negctl: $(BUILD)/gfx_blend_negctl
	@rc=0; $(BUILD)/gfx_blend_negctl > $(BUILD)/gfx-blend-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/gfx-blend-negctl.log; exit 1; }
	@grep -q '^FAIL MULTIPLY$$' $(BUILD)/gfx-blend-negctl.log || { echo "negctl did not fail with 'FAIL MULTIPLY'"; cat $(BUILD)/gfx-blend-negctl.log; exit 1; }
	@echo 'gfx-blend-ops control: all-src-over build failed FAIL MULTIPLY.'

test-gfx-blend-ops: test-gfx-blend-ops-negctl $(BUILD)/gfx_blend_test
	$(BUILD)/gfx_blend_test

ci-host: test-gfx-blend-ops

# Budget: op 0 within +-5% of the pre-change src-over loop; other ops <= 3x src-over.
GBOB_SRC := tests/unit/gfx_blend_bench.c $(GFX_SRC)
GBOB_FLAGS := -O2 -g -Wall -Wextra -Itests/unit $(GFX_INC)

$(BUILD)/gfx_blend_bench: $(GBOB_SRC) $(GFX_HEADERS) tests/unit/bench_budget.h
	@mkdir -p $(dir $@)
	$(CC) $(GBOB_FLAGS) $(GBOB_SRC) -lm -o $@

test-gfx-blend-ops-budget: $(BUILD)/gfx_blend_bench
	$(BUILD)/gfx_blend_bench

test-gfx-budget: test-gfx-blend-ops-budget

.PHONY: test-gfx-blend-ops test-gfx-blend-ops-negctl test-gfx-blend-ops-budget
