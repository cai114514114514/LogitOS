# Current tree order and shadow scope for ID queries. Derive the shipping JS
# source link from the interface gate so installers/dependencies cannot drift.
.PHONY: test-dom-id test-dom-id-negctl
DOM_ID_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/dom_id_test.c

$(BUILD)/dom_id_test: $(DOM_ID_SRC) $(HTML_PARSER_SRC) c/apps/browser/dom.h $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(DOM_ID_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# The control restores the old bucket-first, shadow-including lookup. Its
# nonzero exit must be an assertion failure (1), not a compiler error or crash.
$(BUILD)/dom_id_negctl: $(DOM_ID_SRC) $(HTML_PARSER_SRC) c/apps/browser/dom.h $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -DDOM_ID_LEGACY_LOOKUP -o $@ $(DOM_ID_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-dom-id-negctl: $(BUILD)/dom_id_negctl
	@rc=0; $(BUILD)/dom_id_negctl > $(BUILD)/dom_id_negctl.log 2>&1 || rc=$$?; \
	 cat $(BUILD)/dom_id_negctl.log; \
	 test "$$rc" -eq 1 && grep -q '^FAIL duplicate tree order' $(BUILD)/dom_id_negctl.log && \
	 grep -q '^FAIL document excludes shadow' $(BUILD)/dom_id_negctl.log || \
	 { echo 'test-dom-id-negctl: FAIL -- expected both real lookup defects'; exit 1; }; \
	 echo 'test-dom-id-negctl: PASS -- old lookup fails tree order and shadow scope'

test-dom-id: test-dom-id-negctl $(BUILD)/dom_id_test
	@$(BUILD)/dom_id_test
