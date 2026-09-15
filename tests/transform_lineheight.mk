# Actual geometry/paint/hit and the existing transform validator.
TRANSFORM_LINE_DIR = $(BUILD)/transform-lineheight
TRANSFORM_LINE_FIXTURE ?= tests/unit/transform_lineheight_test.c
TRANSFORM_LINE_SRC = $(filter-out tests/unit/modal_paint_test.c,$(MODAL_PAINT_SRC)) $(TRANSFORM_LINE_FIXTURE)
TRANSFORM_LINE_DEPS = $(TRANSFORM_LINE_SRC) tests/unit/modal_paint_test.c tests/unit/backface_test.c $(wildcard c/apps/browser/css*.h c/apps/browser/css*.inc c/apps/browser/layout*.h c/apps/browser/layout*.inc) c/apps/browser/browser_backface.inc $(BUILD)/libcss_host.a
$(TRANSFORM_LINE_DIR)/current: $(TRANSFORM_LINE_DEPS)
	@mkdir -p $(TRANSFORM_LINE_DIR)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(TRANSFORM_LINE_SRC) $(BUILD)/libcss_host.a -lm
$(TRANSFORM_LINE_DIR)/old: $(TRANSFORM_LINE_DEPS)
	@mkdir -p $(TRANSFORM_LINE_DIR)
	$(CC) -O2 -w -DCI_TRANSFORM_NO_LINEHEIGHT $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(TRANSFORM_LINE_SRC) $(BUILD)/libcss_host.a -lm
$(TRANSFORM_LINE_DIR)/san: $(TRANSFORM_LINE_DEPS)
	@mkdir -p $(TRANSFORM_LINE_DIR)
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(TRANSFORM_LINE_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-transform-lineheight test-transform-lineheight-negctl test-transform-lineheight-san
test-transform-lineheight-negctl: $(TRANSFORM_LINE_DIR)/old
	@python3 tests/unit/transform_lineheight_check.py $< $(TRANSFORM_LINE_DIR)/old.log old
test-transform-lineheight: test-transform-lineheight-negctl $(TRANSFORM_LINE_DIR)/current
	@python3 tests/unit/transform_lineheight_check.py $(TRANSFORM_LINE_DIR)/current $(TRANSFORM_LINE_DIR)/current.log current
test-transform-lineheight-san: test-transform-lineheight $(TRANSFORM_LINE_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/unit/transform_lineheight_check.py $(TRANSFORM_LINE_DIR)/san $(TRANSFORM_LINE_DIR)/san.log current
ci-host: test-transform-lineheight

TRANSFORM_LINE_CSSOM_SRC = $(filter-out tests/unit/cssom_test.c,$(CSSOM_TEST_SRC)) c/apps/browser/css_interp.c tests/unit/transform_lineheight_cssom_test.c
TRANSFORM_LINE_CSSOM_DEPS = $(TRANSFORM_LINE_CSSOM_SRC) c/apps/browser/js_matrix.inc c/apps/browser/css_transform_context.h tests/unit/cssom_test.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a
$(TRANSFORM_LINE_DIR)/cssom: $(TRANSFORM_LINE_CSSOM_DEPS)
	@mkdir -p $(TRANSFORM_LINE_DIR)
	$(CC) $(DOM_MATRIX_CF) -o $@ $(TRANSFORM_LINE_CSSOM_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(TRANSFORM_LINE_DIR)/cssom-old: $(TRANSFORM_LINE_CSSOM_DEPS)
	@mkdir -p $(TRANSFORM_LINE_DIR)
	$(CC) $(DOM_MATRIX_CF) -DCI_TRANSFORM_NO_LINEHEIGHT -o $@ $(TRANSFORM_LINE_CSSOM_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-transform-lineheight-cssom probe-transform-lineheight-known-hit-gap
test-transform-lineheight-cssom: $(TRANSFORM_LINE_DIR)/cssom $(TRANSFORM_LINE_DIR)/cssom-old
	@python3 tests/unit/transform_lineheight_check.py $(TRANSFORM_LINE_DIR)/cssom-old $(TRANSFORM_LINE_DIR)/cssom-old.log cssom-old
	@python3 tests/unit/transform_lineheight_check.py $(TRANSFORM_LINE_DIR)/cssom $(TRANSFORM_LINE_DIR)/cssom.log cssom
test-transform-lineheight: test-transform-lineheight-cssom
# Retain the original seven-check translate probe. The input projection gate
# below now covers this ordinary hit case and its coordinate consumers.
probe-transform-lineheight-known-hit-gap: $(TRANSFORM_LINE_DIR)/current
	@$(TRANSFORM_LINE_DIR)/current --known-translated-hit

include tests/translated_hit.mk
