# Strict nullable FileList assignment must reach the following render. Restore
# the getter-only descriptor and the markup-as-native-value behavior for the
# control; no constructor or DOM stubs. Both JS and native failures are required.
INPUT_FILES_SRC = $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) tests/unit/input_files_test.c
INPUT_FILES_DEPS = tests/unit/select_state_test.c tests/fixtures/engine-expansion/input-files.html $(QJS_SRC) third_party/quickjs/quickjs.h tests/input_files.mk
.PHONY: test-input-files test-input-files-negctl test-input-files-asan
$(BUILD)/input_files_test: $(INPUT_FILES_SRC) $(INPUT_FILES_DEPS) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(INPUT_FILES_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/input_files_negctl: $(INPUT_FILES_SRC) $(INPUT_FILES_DEPS) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	$(CC) -O2 -w $(DOMIFACE_CF) -DFORM_FILES_GETTER_ONLY -DFORM_FILES_MARKUP_VALUE -o $@ $(INPUT_FILES_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-input-files-negctl: $(BUILD)/input_files_negctl
	@rc=0; $(BUILD)/input_files_negctl > $(BUILD)/input_files_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/input_files_negctl.log; \
	 test "$$rc" -eq 1 && grep -q 'FAIL strict undefined assignment continues' $(BUILD)/input_files_negctl.log && \
	 grep -q '^FAIL render after strict binding' $(BUILD)/input_files_negctl.log && \
	 grep -q '^FAIL binding callback remains callable' $(BUILD)/input_files_negctl.log && \
	 grep -q '^FAIL native file label ignores markup' $(BUILD)/input_files_negctl.log && \
	 grep -q '^FAIL native file value ignores markup' $(BUILD)/input_files_negctl.log && \
	 grep -q '^FAIL native file value refuses invented path' $(BUILD)/input_files_negctl.log
test-input-files: test-input-files-negctl $(BUILD)/input_files_test
	@$(BUILD)/input_files_test
test-input-files-asan: test-input-files-negctl $(INPUT_FILES_SRC) $(INPUT_FILES_DEPS) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(DOMIFACE_CF) -o $(BUILD)/input_files_asan $(INPUT_FILES_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/input_files_asan
ci-host: test-input-files
