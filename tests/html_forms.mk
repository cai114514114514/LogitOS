# Constraint checking must reach the same state and submission doors as native
# input. Derive the JS apparatus from the existing select-state gate rather
# than copying another drifting installer list. Include after select_state.mk.
.PHONY: test-html-forms test-html-forms-negctl test-html-forms-asan

HTML_FORMS_SRC = $(sort $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) c/apps/gui/browser/js/dom/events.c tests/unit/html_forms_test.c)
HTML_FORMS_DEPS = $(HTML_FORMS_SRC) $(HTML_PARSER_SRC) c/apps/gui/browser/dom/validation.h c/apps/gui/browser/js/dom/validation.h tests/fixtures/html-priority/forms.js tests/html_forms.mk

$(BUILD)/html_forms_test: $(HTML_FORMS_DEPS) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(HTML_FORMS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

$(BUILD)/html_forms_negctl: $(HTML_FORMS_DEPS) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -DFORM_VALIDATION_IGNORE_ERRORS -o $@ $(HTML_FORMS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-html-forms-negctl: $(BUILD)/html_forms_negctl
	@rc=0; $(BUILD)/html_forms_negctl > $(BUILD)/html_forms_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/html_forms_negctl.log; \
	 test "$$rc" -eq 1 && \
	 grep -q '^FAIL native required prevents default submission' $(BUILD)/html_forms_negctl.log && \
	 grep -q '^FAIL js invalid requestSubmit blocks submit before navigation' $(BUILD)/html_forms_negctl.log || \
	 { echo 'test-html-forms-negctl: FAIL -- ignored constraints did not fail both native and JS consumers'; exit 1; }; \
	 echo 'test-html-forms-negctl: PASS -- ignored constraints fail native and JS submission consumers'

test-html-forms: test-html-forms-negctl $(BUILD)/html_forms_test
	@$(BUILD)/html_forms_test

test-html-forms-asan: $(HTML_FORMS_DEPS) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(DOMIFACE_CF) -o $(BUILD)/html_forms_asan $(HTML_FORMS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/html_forms_asan

ci-host: test-html-forms
