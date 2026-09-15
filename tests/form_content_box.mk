# Finite ordinary empty-field, padding, caret and selection geometry.
FORM_CONTENT_DIR = $(BUILD)/form-content-box
FORM_CONTENT_SRC = $(filter-out tests/unit/form_caret_test.c,$(FORM_CARET_SRC)) tests/unit/form_content_box_test.c
FORM_CONTENT_DEPS = $(FORM_CONTENT_SRC) $(FORM_CARET_DEPS) c/apps/browser/forms.h c/apps/browser/css.h tests/form_content_box.mk
$(FORM_CONTENT_DIR)/current: $(FORM_CONTENT_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(FORM_CONTENT_DIR)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(FORM_CONTENT_SRC) $(BUILD)/libcss_host.a -lm
$(FORM_CONTENT_DIR)/old: $(FORM_CONTENT_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(FORM_CONTENT_DIR)
	$(CC) -O2 -w -DFC_CONTENT_BOX_LEGACY $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(FORM_CONTENT_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-form-content-box test-form-content-box-negctl
test-form-content-box-negctl: $(FORM_CONTENT_DIR)/old
	@python3 tests/unit/form_content_box_check.py $< $(FORM_CONTENT_DIR)/old.log old
test-form-content-box: test-form-content-box-negctl $(FORM_CONTENT_DIR)/current
	@python3 tests/unit/form_content_box_check.py $(FORM_CONTENT_DIR)/current $(FORM_CONTENT_DIR)/current.log current
test-form-caret: test-form-content-box
ci-host: test-form-content-box

# The same computed clip must fit native input-button labels at auto size.
FORM_NATIVE_BUTTON_SRC = $(filter-out tests/unit/form_content_box_test.c,$(FORM_CONTENT_SRC)) tests/unit/form_native_button_edges_test.c
$(FORM_CONTENT_DIR)/native-button-current: $(FORM_NATIVE_BUTTON_SRC) $(FORM_CONTENT_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(FORM_CONTENT_DIR)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(FORM_NATIVE_BUTTON_SRC) $(BUILD)/libcss_host.a -lm
$(FORM_CONTENT_DIR)/native-button-old: $(FORM_NATIVE_BUTTON_SRC) $(FORM_CONTENT_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(FORM_CONTENT_DIR)
	$(CC) -O2 -w -DFC_NATIVE_BUTTON_EDGES_LEGACY $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(FORM_NATIVE_BUTTON_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-form-native-button-edges test-form-native-button-edges-negctl
test-form-native-button-edges-negctl: $(FORM_CONTENT_DIR)/native-button-old
	@python3 tests/unit/form_native_button_edges_check.py $< $(FORM_CONTENT_DIR)/native-button-old.log old
test-form-native-button-edges: test-form-native-button-edges-negctl $(FORM_CONTENT_DIR)/native-button-current
	@python3 tests/unit/form_native_button_edges_check.py $(FORM_CONTENT_DIR)/native-button-current $(FORM_CONTENT_DIR)/native-button-current.log current
test-form-content-box: test-form-native-button-edges

include tests/control_text_ink.mk
