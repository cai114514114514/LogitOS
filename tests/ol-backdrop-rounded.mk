# ol_cmd_backdrop_rrect: rounded frosted panels (retained/SDK path).
OLBR_SRC := tests/unit/openlogit_backdrop_rrect_test.c
$(BUILD)/olbr/test: $(OLBR_SRC) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $< $(GFX_SRC) -lm -o $@
$(BUILD)/olbr/neg: $(OLBR_SRC) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -DOL_BACKDROP_IGNORE_RADIUS $(GFX_INC) $< $(GFX_SRC) -lm -o $@
$(BUILD)/olbr/sanitize: $(OLBR_SRC) $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC) $< $(GFX_SRC) -lm -o $@
test-ol-backdrop-rounded-negctl: $(BUILD)/olbr/neg
	@rc=0; $< > $(BUILD)/olbr/neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL corner' $(BUILD)/olbr/neg.log
test-ol-backdrop-rounded: test-ol-backdrop-rounded-negctl $(BUILD)/olbr/test $(BUILD)/olbr/sanitize
	$(BUILD)/olbr/test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/olbr/sanitize
ci-host: test-ol-backdrop-rounded
test-openlogit: test-ol-backdrop-rounded
.PHONY: test-ol-backdrop-rounded test-ol-backdrop-rounded-negctl
