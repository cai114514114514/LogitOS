# Normal CSS geometry; the exact producer-off control is required by the gate.
PHYSICAL_SPACING_DIR = $(BUILD)/physical-spacing-calc
PHYSICAL_SPACING_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/physical_spacing_calc_test.c
PHYSICAL_SPACING_DEPS = $(PHYSICAL_SPACING_SRC) $(HTML_PARSER_SRC) tests/unit/intrinsic_test.c $(wildcard c/apps/browser/css*.h c/apps/browser/css*.inc c/apps/browser/layout*.h c/apps/browser/layout*.c c/apps/browser/layout*.inc) $(BUILD)/libcss_host.a tests/physical_spacing_calc.mk
$(PHYSICAL_SPACING_DIR)/test: $(PHYSICAL_SPACING_DEPS)
	@mkdir -p $(PHYSICAL_SPACING_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(PHYSICAL_SPACING_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(PHYSICAL_SPACING_DIR)/legacy: $(PHYSICAL_SPACING_DEPS)
	@mkdir -p $(PHYSICAL_SPACING_DIR)
	$(CC) -O2 -w -DCSS_PHYSICAL_SPACING_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(PHYSICAL_SPACING_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-physical-spacing-calc test-physical-spacing-calc-negctl
test-physical-spacing-calc-negctl: $(PHYSICAL_SPACING_DIR)/legacy
	@python3 tests/unit/physical_spacing_calc_check.py $< $(PHYSICAL_SPACING_DIR)/legacy.log legacy
test-physical-spacing-calc: test-physical-spacing-calc-negctl $(PHYSICAL_SPACING_DIR)/test
	@python3 tests/unit/physical_spacing_calc_check.py $(PHYSICAL_SPACING_DIR)/test $(PHYSICAL_SPACING_DIR)/current.log current
ci-host: test-physical-spacing-calc
