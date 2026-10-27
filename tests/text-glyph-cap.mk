# text.c glyph scratch sized by extent: px 200..512 render. Host harness links
# the real text.c + fonts. The control builds the SAME sources and flags with
# -DTEXT_RASTBUF_LEGACY (old fixed 200x200 scratch); "FAIL ink400" must appear.
TGC_DIR := $(BUILD)/text-glyph-cap
TGC_SRC := tests/unit/text_bigglyph_test.c $(sort $(FONT_WEIGHT_SRC))
TGC_INC := $(FONT_WEIGHT_INC)
TGC_FLAGS := -DTEXT_GLYPH_CAP_TEST -DTEXT_GLYPH_BUDGET=262144
.PHONY: test-text-glyph-cap test-text-glyph-cap-negctl
$(TGC_DIR)/current: $(TGC_SRC) c/kernel/gui/text.h
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra $(TGC_FLAGS) $(TGC_INC) -o $@ $(TGC_SRC) -lm
$(TGC_DIR)/legacy: $(TGC_SRC) c/kernel/gui/text.h
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra $(TGC_FLAGS) -DTEXT_RASTBUF_LEGACY $(TGC_INC) -o $@ $(TGC_SRC) -lm
test-text-glyph-cap-negctl: $(TGC_DIR)/legacy
	@$(TGC_DIR)/legacy fsroot >$(TGC_DIR)/legacy.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -qx 'FAIL ink400' $(TGC_DIR)/legacy.log || { cat $(TGC_DIR)/legacy.log; echo "negctl did not fail with FAIL ink400"; exit 1; }; \
	 grep -x 'FAIL ink400' $(TGC_DIR)/legacy.log | head -1
test-text-glyph-cap: test-text-glyph-cap-negctl $(TGC_DIR)/current
	@$(TGC_DIR)/current fsroot >$(TGC_DIR)/current.log 2>&1; rc=$$?; \
	 cat $(TGC_DIR)/current.log; test $$rc -eq 0
ci-host: test-text-glyph-cap
