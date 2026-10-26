# Per-display scratch slots (display_scratch). The control swaps in one global
# buffer shared by every display; the interleave check must catch it.
DINT_SRC := tests/unit/display_scratch_test.c $(GFX_SRC)
DINT_FLAGS := -O1 -g -Wall -Wextra -DGFX_NCTX=2 $(GFX_INC)
DINT_DEPS := $(DINT_SRC) $(GFX_HEADERS)

$(BUILD)/display_scratch_test: $(DINT_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(DINT_FLAGS) $(DINT_SRC) -lm -o $@

$(BUILD)/display_scratch_negctl: $(DINT_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(DINT_FLAGS) -DOL_DISPLAY_SCRATCH_NEGCTL_GLOBAL $(DINT_SRC) -lm -o $@

test-display-internals-negctl: $(BUILD)/display_scratch_negctl
	@rc=0; $(BUILD)/display_scratch_negctl > $(BUILD)/display-scratch-negctl.log 2>&1 || rc=$$?; \
	test $$rc -eq 1 || { echo "negctl exited $$rc, expected 1"; cat $(BUILD)/display-scratch-negctl.log; exit 1; }
	@grep -q '^FAIL interleave$$' $(BUILD)/display-scratch-negctl.log || { echo "negctl did not fail with 'FAIL interleave'"; cat $(BUILD)/display-scratch-negctl.log; exit 1; }
	@echo 'display-internals control: shared scratch buffer failed FAIL interleave.'

test-display-internals: test-display-internals-negctl $(BUILD)/display_scratch_test
	$(BUILD)/display_scratch_test

ci-host: test-display-internals
.PHONY: test-display-internals test-display-internals-negctl
