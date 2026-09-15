# Positioned replaced elements must emit their own paint item after the parent
# allocator assigns geometry. The negative build restores the empty-child walk.
POSITIONED_REPLACED_DIR := $(BUILD)/positioned-replaced
POSITIONED_REPLACED_SRC := tests/unit/positioned_replaced_test.c \
    $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC))
POSITIONED_REPLACED_DEPS := $(POSITIONED_REPLACED_SRC) $(HTML_PARSER_SRC) \
    c/apps/browser/layout.h $(BUILD)/libcss_host.a

.PHONY: test-positioned-replaced test-positioned-replaced-negctl

$(POSITIONED_REPLACED_DIR)/current: $(POSITIONED_REPLACED_DEPS)
	@mkdir -p $(POSITIONED_REPLACED_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(POSITIONED_REPLACED_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm

$(POSITIONED_REPLACED_DIR)/old: $(POSITIONED_REPLACED_DEPS)
	@mkdir -p $(POSITIONED_REPLACED_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_NEGCTL_REPLACED_SELF \
	    -o $@ $(POSITIONED_REPLACED_SRC) $(HTML_PARSER_SRC) \
	    $(BUILD)/libcss_host.a -lm

test-positioned-replaced-negctl: $(POSITIONED_REPLACED_DIR)/old
	@$(POSITIONED_REPLACED_DIR)/old >$(POSITIONED_REPLACED_DIR)/old.log 2>&1; \
	 rc=$$?; test $$rc -eq 1 && \
	 grep -q 'FAIL: positioned video emits a 640x360 IT_VIDEO paint item' \
	 $(POSITIONED_REPLACED_DIR)/old.log || { cat $(POSITIONED_REPLACED_DIR)/old.log; exit 1; }
	@echo "test-positioned-replaced-negctl: ok -- empty-child walk loses IT_VIDEO"

test-positioned-replaced: test-positioned-replaced-negctl $(POSITIONED_REPLACED_DIR)/current
	@$(POSITIONED_REPLACED_DIR)/current
