# Actual forms + parser/layout/painter. The host metric records unequal glyph
# advances and faithfully refuses >1024 bytes, just as SYS_TEXT_MEASURE does;
# this is a geometry/ABI gate, not evidence about guest font pixels or IME.
FORM_CARET_SRC = $(filter-out tests/unit/modal_paint_test.c,$(MODAL_PAINT_SRC)) tests/unit/form_caret_test.c c/apps/browser/forms.c
FORM_CARET_DIR = $(BUILD)/site-general/caret
FORM_CARET_DEPS = $(MODAL_DEPS) c/apps/browser/forms.h tests/unit/modal_paint_test.c tests/unit/painthost/logit.h
.PHONY: test-form-caret test-form-caret-negctl
$(FORM_CARET_DIR)/form_caret_test: $(FORM_CARET_SRC) $(FORM_CARET_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(FORM_CARET_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(FORM_CARET_SRC) $(BUILD)/libcss_host.a -lm
test-form-caret-negctl: $(FORM_CARET_SRC) $(FORM_CARET_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(FORM_CARET_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -DFC_CARET_OLD_GEOMETRY -o $(FORM_CARET_DIR)/form_caret_old $(FORM_CARET_SRC) $(BUILD)/libcss_host.a -lm
	@rc=0; $(FORM_CARET_DIR)/form_caret_old > $(FORM_CARET_DIR)/old.log 2>&1 || rc=$$?; \
	 cat $(FORM_CARET_DIR)/old.log; test "$$rc" -eq 1 && \
	 grep -q 'FAIL: end caret lies inside the exclusive content clip' $(FORM_CARET_DIR)/old.log && \
	 grep -q 'FAIL: long value uses bounded native measurement calls' $(FORM_CARET_DIR)/old.log && \
	 grep -q 'FAIL: each painted text run fits the real GUI syscall' $(FORM_CARET_DIR)/old.log && \
	 grep -q 'FAIL: actual painter emits one visible caret at the final glyph edge' $(FORM_CARET_DIR)/old.log && \
	 grep -q 'FAIL: password mouse placement measures displayed bullets' $(FORM_CARET_DIR)/old.log
test-form-caret: test-form-caret-negctl $(FORM_CARET_DIR)/form_caret_test
	@rc=0; $(FORM_CARET_DIR)/form_caret_test > $(FORM_CARET_DIR)/current.log 2>&1 || rc=$$?; cat $(FORM_CARET_DIR)/current.log; exit $$rc
ci-host: test-form-caret

FORM_CONTAINER_SRC = $(filter-out tests/unit/form_caret_test.c,$(FORM_CARET_SRC)) tests/unit/form_control_container_test.c
$(FORM_CARET_DIR)/form_control_container_test: $(FORM_CONTAINER_SRC) $(FORM_CARET_DEPS) tests/unit/form_caret_test.c $(BUILD)/libcss_host.a
	@mkdir -p $(FORM_CARET_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(FORM_CONTAINER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-form-control-container test-form-control-container-negctl
test-form-control-container-negctl: $(FORM_CONTAINER_SRC) $(FORM_CARET_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(FORM_CARET_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -DLAYOUT_NO_SELF_CONTROL -o $(FORM_CARET_DIR)/form_control_container_old $(FORM_CONTAINER_SRC) $(BUILD)/libcss_host.a -lm
	@rc=0; $(FORM_CARET_DIR)/form_control_container_old > $(FORM_CARET_DIR)/container_old.log 2>&1 || rc=$$?; cat $(FORM_CARET_DIR)/container_old.log; test "$$rc" -eq 1 && grep -q 'FAIL: direct container input value reaches real painter' $(FORM_CARET_DIR)/container_old.log
test-form-control-container: test-form-control-container-negctl $(FORM_CARET_DIR)/form_control_container_test
	@$(FORM_CARET_DIR)/form_control_container_test > $(FORM_CARET_DIR)/container_current.log 2>&1; rc=$$?; cat $(FORM_CARET_DIR)/container_current.log; exit $$rc
test-form-caret: test-form-control-container
ci-host: test-form-control-container

CONTROL_CHROME_SRC = $(filter-out tests/unit/form_control_container_test.c,$(FORM_CONTAINER_SRC)) tests/unit/control_chrome_test.c
$(FORM_CARET_DIR)/control_chrome_test: $(CONTROL_CHROME_SRC) $(FORM_CARET_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(FORM_CARET_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(CONTROL_CHROME_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-control-chrome test-control-chrome-negctl
test-control-chrome-negctl: $(CONTROL_CHROME_SRC) $(FORM_CARET_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(FORM_CARET_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -DBROWSER_CONTROL_OLD_CHROME -o $(FORM_CARET_DIR)/control_chrome_old $(CONTROL_CHROME_SRC) $(BUILD)/libcss_host.a -lm
	@rc=0; $(FORM_CARET_DIR)/control_chrome_old > $(FORM_CARET_DIR)/chrome_old.log 2>&1 || rc=$$?; cat $(FORM_CARET_DIR)/chrome_old.log; test "$$rc" -eq 1 && grep -q 'FAIL: computed reset and transparent borders never restore native fill' $(FORM_CARET_DIR)/chrome_old.log
test-control-chrome: test-control-chrome-negctl $(FORM_CARET_DIR)/control_chrome_test
	@rc=0; $(FORM_CARET_DIR)/control_chrome_test > $(FORM_CARET_DIR)/chrome_current.log 2>&1 || rc=$$?; cat $(FORM_CARET_DIR)/chrome_current.log; exit $$rc
test-form-caret: test-control-chrome

include tests/form_content_box.mk
