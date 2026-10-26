# tests/display-blit-ex.mk -- ol_display_blit_rgba_ex / ol_display_blit_glyph_a.
# The control builds the same test with -DOL_DISPLAY_SCALE_NEAREST (BOX routes
# to point sampling); the area-average check must fail with "FAIL box-downscale".
DBX_SRC := tests/unit/display_blit_ex_test.c $(GFX_SRC)
DBX_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)
DBX_DEPS := $(DBX_SRC) $(GFX_HEADERS)

$(BUILD)/display_blit_ex_test: $(DBX_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(DBX_FLAGS) $(DBX_SRC) -lm -o $@

$(BUILD)/display_blit_ex_negctl: $(DBX_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(DBX_FLAGS) -DOL_DISPLAY_SCALE_NEAREST $(DBX_SRC) -lm -o $@

test-display-blit-ex-negctl: $(BUILD)/display_blit_ex_negctl
	@rc=0; $(BUILD)/display_blit_ex_negctl > $(BUILD)/display-blit-ex-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/display-blit-ex-negctl.log; exit 1; }
	@grep -q '^FAIL box-downscale$$' $(BUILD)/display-blit-ex-negctl.log || { echo "negctl did not fail with 'FAIL box-downscale'"; cat $(BUILD)/display-blit-ex-negctl.log; exit 1; }
	@echo 'display-blit-ex control: BOX routed to nearest failed FAIL box-downscale.'

test-display-blit-ex: test-display-blit-ex-negctl $(BUILD)/display_blit_ex_test
	$(BUILD)/display_blit_ex_test

DBXB_SRC := tests/unit/display_blit_ex_bench.c $(GFX_SRC)
$(BUILD)/display_blit_ex_bench: $(DBXB_SRC) $(GFX_HEADERS) tests/unit/bench_budget.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -Itests/unit $(GFX_INC) $(DBXB_SRC) -lm -o $@

test-display-blit-ex-budget: $(BUILD)/display_blit_ex_bench
	$(BUILD)/display_blit_ex_bench

test-gfx-budget: test-display-blit-ex-budget
ci-host: test-display-blit-ex
.PHONY: test-display-blit-ex test-display-blit-ex-negctl test-display-blit-ex-budget
