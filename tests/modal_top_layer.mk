# Paint gate links actual parser/CSS/layout/painter/native focus. Runtime gate
# adds the actual JS APIs and IDL reflection (the inherited select harness
# omits js_reflect.c; without it dialog.open is undefined despite native open); neither substitutes a second modal implementation.
MODAL_PAINT_SRC = $(filter-out tests/unit/inline_hit_test.c,$(INLINE_HIT_SRC)) tests/unit/modal_paint_test.c c/apps/browser/focus.c
MODAL_RUNTIME_SRC = $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) tests/unit/modal_runtime_test.c c/apps/browser/js_reflect.c
MODAL_DEPS = c/apps/browser/top_layer.h c/apps/browser/top_layer.inc c/apps/browser/focus.h c/apps/browser/css.h c/apps/browser/layout.h c/apps/browser/browser_paint.h
.PHONY: test-modal-top-layer test-modal-top-layer-negctl test-modal-runtime
$(BUILD)/modal_paint_test: $(MODAL_PAINT_SRC) $(MODAL_DEPS) $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(MODAL_PAINT_SRC) $(BUILD)/libcss_host.a -lm
$(BUILD)/modal_runtime_test: $(MODAL_RUNTIME_SRC) $(MODAL_DEPS) tests/unit/select_state_test.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(MODAL_RUNTIME_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
# The completed stacking tree now also isolates top-layer contexts. Disabling
# only the painter pass would retain that protection, so the ordinary-order
# control restores its original flat layout order too; the exact assertion is
# unchanged and still proves that a maximum-z page cannot cover a modal.
test-modal-top-layer-negctl: $(MODAL_PAINT_SRC) $(MODAL_DEPS) $(BUILD)/libcss_host.a
	@set -e; for ctl in LAYOUT_MODAL_IN_FLOW PAINT_MODAL_ORDINARY_ORDER TOP_LAYER_NO_INPUT; do \
	 extra=; test "$$ctl" != PAINT_MODAL_ORDINARY_ORDER || extra=-DLAYOUT_STACKING_FLAT; \
	 $(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -D$$ctl $$extra -o $(BUILD)/modal_$$ctl $(MODAL_PAINT_SRC) $(BUILD)/libcss_host.a -lm; \
	 set +e; $(BUILD)/modal_$$ctl > $(BUILD)/modal_$$ctl.log 2>&1; rc=$$?; set -e; cat $(BUILD)/modal_$$ctl.log; \
	 test $$rc -eq 1 || exit 1; \
	 case $$ctl in LAYOUT_MODAL_IN_FLOW) grep -q 'FAIL: modal is excluded from ordinary document flow' $(BUILD)/modal_$$ctl.log;; \
	 PAINT_MODAL_ORDINARY_ORDER) grep -q 'FAIL: modal paints above maximum page z index' $(BUILD)/modal_$$ctl.log;; \
	 TOP_LAYER_NO_INPUT) grep -q 'FAIL: modal rejects programmatic background focus' $(BUILD)/modal_$$ctl.log;; esac; done
test-modal-runtime: test-modal-top-layer-negctl $(BUILD)/modal_runtime_test
	@$(BUILD)/modal_runtime_test
test-modal-top-layer: test-modal-top-layer-negctl $(BUILD)/modal_paint_test test-modal-runtime
	@$(BUILD)/modal_paint_test
