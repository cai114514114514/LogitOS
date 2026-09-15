OPCODE_MATCH_CF = -O1 -g -w -Ithird_party/quickjs -DCONFIG_VERSION='"host"' -fsanitize=undefined -fno-sanitize-recover=all
.PHONY: test-opcode-match test-opcode-match-negctl
test-opcode-match-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(OPCODE_MATCH_CF) -DJS_OPCODE_SIGNED_SHIFT -o $(BUILD)/opcode_match_old tests/unit/opcode_match_test.c $(QJS_SRC) -lm
	@$(BUILD)/opcode_match_old >$(BUILD)/opcode_match_old.log 2>&1; rc=$$?; \
	 test $$rc -ne 0 && grep -E 'runtime error: left shift of .* by 24 places' $(BUILD)/opcode_match_old.log
test-opcode-match: test-opcode-match-negctl
	@$(CC) $(OPCODE_MATCH_CF) -o $(BUILD)/opcode_match tests/unit/opcode_match_test.c $(QJS_SRC) -lm
	@$(BUILD)/opcode_match
ci-host: test-opcode-match

# A separate control: the forms sanitizer discovered out-of-range narrowing
# in the shared numeric boxing header after the opcode issue was corrected.
.PHONY: test-number-boxing test-number-boxing-negctl
test-number-boxing-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(OPCODE_MATCH_CF) -DJS_FLOAT64_UNCHECKED_NARROW -o $(BUILD)/number_boxing_old tests/unit/number_boxing_test.c $(QJS_SRC) -lm
	@$(BUILD)/number_boxing_old > $(BUILD)/number_boxing_old.log 2>&1; rc=$$?; \
	 test $$rc -ne 0 && grep -E 'runtime error: .* is outside the range of representable values' $(BUILD)/number_boxing_old.log
test-number-boxing: test-number-boxing-negctl
	@$(CC) $(OPCODE_MATCH_CF) -o $(BUILD)/number_boxing tests/unit/number_boxing_test.c $(QJS_SRC) -lm
	@$(BUILD)/number_boxing
ci-host: test-number-boxing
