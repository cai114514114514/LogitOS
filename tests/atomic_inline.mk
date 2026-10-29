# One outer atomic box path, with each real inner formatting context.
ATOMIC_DIR := $(BUILD)/atomic-inline
ATOMIC_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/atomic_inline_test.c
ATOMIC_DEPS = $(ATOMIC_SRC) $(HTML_PARSER_SRC) c/apps/gui/browser/css/style.h c/apps/gui/browser/layout/grid.c c/apps/gui/browser/layout/grid.h $(BUILD)/libcss_host.a
$(ATOMIC_DIR)/test: $(ATOMIC_DEPS)
	@mkdir -p $(ATOMIC_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(ATOMIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(ATOMIC_DIR)/negctl: $(ATOMIC_DEPS)
	@mkdir -p $(ATOMIC_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_NEGCTL_ATOMIC_OUTER -o $@ $(ATOMIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
