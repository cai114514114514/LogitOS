# Derive the production prelude/native DOM link from the established platform
# consumer. The control retains every TU and removes only this CE correction.
CUSTOM_ELEMENTS_SRC = $(filter-out tests/unit/webapi_platform_test.c,$(PLATFORM_TEST_SRC)) tests/unit/custom_elements_test.c
CUSTOM_ELEMENTS_DEPS = $(CUSTOM_ELEMENTS_SRC) $(PLATFORM_MOD) tests/unit/webapi_platform_test.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
CUSTOM_ELEMENTS_OUT = $(BUILD)/site-general/runtime
.PHONY: test-custom-elements test-custom-elements-negctl
$(CUSTOM_ELEMENTS_OUT)/custom_elements_test: $(CUSTOM_ELEMENTS_DEPS)
	@mkdir -p $(CUSTOM_ELEMENTS_OUT)
	$(CC) -O2 -w $(PLATFORM_CF) -o $@ $(CUSTOM_ELEMENTS_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(CUSTOM_ELEMENTS_OUT)/custom_elements_negctl: $(CUSTOM_ELEMENTS_DEPS)
	@mkdir -p $(CUSTOM_ELEMENTS_OUT)
	$(CC) -O2 -w $(PLATFORM_CF) -DCE_LEGACY_CREATION_LIFECYCLE -o $@ $(CUSTOM_ELEMENTS_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-custom-elements-negctl: $(CUSTOM_ELEMENTS_OUT)/custom_elements_negctl
	@rc=0; $< > $(CUSTOM_ELEMENTS_OUT)/custom_elements_negctl.log 2>&1 || rc=$$?; cat $(CUSTOM_ELEMENTS_OUT)/custom_elements_negctl.log; test $$rc -eq 1 && grep -q '^FAIL: detached createElement controller' $(CUSTOM_ELEMENTS_OUT)/custom_elements_negctl.log && grep -q '^FAIL: direct new connects' $(CUSTOM_ELEMENTS_OUT)/custom_elements_negctl.log
test-custom-elements: test-custom-elements-negctl $(CUSTOM_ELEMENTS_OUT)/custom_elements_test
	@$(CUSTOM_ELEMENTS_OUT)/custom_elements_test
