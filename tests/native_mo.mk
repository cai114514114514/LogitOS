NATIVE_MO_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/native_mo_test.c
NATIVE_MO_DEPS = $(NATIVE_MO_SRC) tests/unit/dom_iface_test.c $(wildcard c/apps/browser/*.h c/apps/browser/*.inc) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-native-mo test-native-mo-negctl
$(BUILD)/native_mo_test: $(NATIVE_MO_DEPS)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(NATIVE_MO_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/native_mo_negctl: $(NATIVE_MO_DEPS)
	$(CC) -O2 -w $(DOMIFACE_CF) -DMO_NATIVE_CHAR_NEGCTL -o $@ $(NATIVE_MO_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-native-mo-negctl: $(BUILD)/native_mo_negctl
	@rc=0; $< > $(BUILD)/native_mo_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/native_mo_negctl.log; test $$rc -eq 1 && grep -q '^FAIL native text edit delivers one record' $(BUILD)/native_mo_negctl.log
test-native-mo: test-native-mo-negctl $(BUILD)/native_mo_test
	@$(BUILD)/native_mo_test
