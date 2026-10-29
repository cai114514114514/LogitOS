# JS single-select state must be the state the native form painter displays.
# Derive the shared installers from the existing interface gate; add the actual
# form state and semantics owners, which that narrower gate does not link.
.PHONY: test-select-state test-select-state-negctl test-select-state-reset-negctl
SELECT_STATE_SRC = $(sort $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/select_state_test.c $(BROWSER_FORM_JS_SRC) c/apps/gui/browser/js/dom/events.c c/apps/gui/browser/js/net/url.c c/apps/gui/browser/js/dom/semantics.c $(BROWSER_FORM_SRC) c/apps/gui/browser/dom/focus.c)

$(BUILD)/select_state_test: $(SELECT_STATE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(SELECT_STATE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

$(BUILD)/select_state_negctl: $(SELECT_STATE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -DSELECT_STATE_JS_ONLY -DSELECT_STATE_CLAMP_INDEX -o $@ $(SELECT_STATE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-select-state-negctl: $(BUILD)/select_state_negctl
	@rc=0; $(BUILD)/select_state_negctl > $(BUILD)/select_state_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/select_state_negctl.log; \
	 test "$$rc" -eq 1 && grep -q '^FAIL script value reaches painter' $(BUILD)/select_state_negctl.log && \
	 grep -q '^FAIL native selection reaches JS' $(BUILD)/select_state_negctl.log && \
	 grep -q '^FAIL negative index deselects' $(BUILD)/select_state_negctl.log && \
	 grep -q '^FAIL native oversized index clears label' $(BUILD)/select_state_negctl.log || \
	 { echo 'test-select-state-negctl: FAIL -- expected JS/native state divergence'; exit 1; }; \
	 echo 'test-select-state-negctl: PASS -- JS-only state fails both directions'

$(BUILD)/select_state_reset_negctl: $(SELECT_STATE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -DSELECT_STATE_NO_RESET -o $@ $(SELECT_STATE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-select-state-reset-negctl: $(BUILD)/select_state_reset_negctl
	@rc=0; $(BUILD)/select_state_reset_negctl > $(BUILD)/select_state_reset_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/select_state_reset_negctl.log; \
	 test "$$rc" -eq 1 && grep -q '^FAIL reset button resets live selection' $(BUILD)/select_state_reset_negctl.log && \
	 grep -q '^FAIL reset button native label' $(BUILD)/select_state_reset_negctl.log || \
	 { echo 'test-select-state-reset-negctl: FAIL -- old Map reset did not fail'; exit 1; }; \
	 echo 'test-select-state-reset-negctl: PASS -- bypassing native reset leaves the live selection unchanged'

test-select-state: test-select-state-negctl test-select-state-reset-negctl $(BUILD)/select_state_test
	@$(BUILD)/select_state_test
