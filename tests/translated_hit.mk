# Finite ordinary translation only, using actual layout and paint coordinates.
TRANSLATED_HIT_DIR = $(BUILD)/translated-hit
TRANSLATED_HIT_CSSLIB ?= $(BUILD)/libcss_host.a
TRANSLATED_HIT_SRC = $(filter-out tests/unit/modal_paint_test.c,$(MODAL_PAINT_SRC)) tests/unit/translated_hit_test.c c/apps/browser/forms.c
TRANSLATED_HIT_DEPS = $(TRANSLATED_HIT_SRC) $(MODAL_DEPS) c/apps/browser/forms.h tests/unit/modal_paint_test.c tests/translated_hit.mk tests/unit/translated_hit_check.py $(wildcard c/apps/browser/css*.h c/apps/browser/css*.inc c/apps/browser/layout*.h c/apps/browser/layout*.inc) c/apps/browser/browser_backface.inc
$(TRANSLATED_HIT_DIR)/current: $(TRANSLATED_HIT_DEPS) $(TRANSLATED_HIT_CSSLIB)
	@mkdir -p $(TRANSLATED_HIT_DIR)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(TRANSLATED_HIT_SRC) $(TRANSLATED_HIT_CSSLIB) -lm
$(TRANSLATED_HIT_DIR)/old: $(TRANSLATED_HIT_DEPS) $(TRANSLATED_HIT_CSSLIB)
	@mkdir -p $(TRANSLATED_HIT_DIR)
	$(CC) -O2 -w -DBROWSER_TRANSLATED_HIT_LEGACY $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(TRANSLATED_HIT_SRC) $(TRANSLATED_HIT_CSSLIB) -lm
.PHONY: test-translated-hit test-translated-hit-negctl
test-translated-hit-negctl: $(TRANSLATED_HIT_DIR)/old
	@python3 tests/unit/translated_hit_check.py $< $(TRANSLATED_HIT_DIR)/old.log old
test-translated-hit: test-translated-hit-negctl $(TRANSLATED_HIT_DIR)/current
	@python3 tests/unit/translated_hit_check.py $(TRANSLATED_HIT_DIR)/current $(TRANSLATED_HIT_DIR)/current.log current
ci-host: test-translated-hit
