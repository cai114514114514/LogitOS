# Compatibility boundaries for the same physical edge producer.
PHYSICAL_BOUNDARY_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/physical_spacing_boundary_test.c
PHYSICAL_BOUNDARY_DEPS = $(PHYSICAL_BOUNDARY_SRC) $(HTML_PARSER_SRC) tests/unit/intrinsic_test.c $(wildcard c/apps/browser/css*.h c/apps/browser/css*.inc) $(BUILD)/libcss_host.a
$(PHYSICAL_SPACING_DIR)/boundary: $(PHYSICAL_BOUNDARY_DEPS)
	@mkdir -p $(PHYSICAL_SPACING_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(PHYSICAL_BOUNDARY_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(PHYSICAL_SPACING_DIR)/boundary-legacy: $(PHYSICAL_BOUNDARY_DEPS)
	@mkdir -p $(PHYSICAL_SPACING_DIR)
	$(CC) -O2 -w -DCSS_PHYSICAL_SPACING_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(PHYSICAL_BOUNDARY_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-physical-spacing-boundary test-physical-spacing-boundary-negctl
test-physical-spacing-boundary-negctl: $(PHYSICAL_SPACING_DIR)/boundary-legacy
	@python3 tests/unit/physical_spacing_boundary_check.py $< $(PHYSICAL_SPACING_DIR)/boundary-legacy.log legacy
test-physical-spacing-boundary: test-physical-spacing-boundary-negctl $(PHYSICAL_SPACING_DIR)/boundary
	@python3 tests/unit/physical_spacing_boundary_check.py $(PHYSICAL_SPACING_DIR)/boundary $(PHYSICAL_SPACING_DIR)/boundary.log current
test-physical-spacing-calc: test-physical-spacing-boundary
