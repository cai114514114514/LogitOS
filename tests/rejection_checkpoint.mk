REJECTION_CHECK_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/rejection_checkpoint_test.c
REJECTION_CHECK_DEPS = $(REJECTION_CHECK_SRC) tests/unit/dom_iface_test.c $(wildcard c/apps/browser/*.h c/apps/browser/*.inc) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-rejection-checkpoint test-rejection-checkpoint-negctl
$(BUILD)/rejection_checkpoint_test: $(REJECTION_CHECK_DEPS)
	$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(REJECTION_CHECK_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/rejection_checkpoint_negctl: $(REJECTION_CHECK_DEPS)
	$(CC) -O2 -w $(DOMIFACE_CF) -DJS_REJECTION_IMMEDIATE_NEGCTL -o $@ $(REJECTION_CHECK_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-rejection-checkpoint-negctl: $(BUILD)/rejection_checkpoint_negctl
	@rc=0; $< > $(BUILD)/rejection_checkpoint_negctl.log 2>&1 || rc=$$?; cat $(BUILD)/rejection_checkpoint_negctl.log; test $$rc -eq 1 && grep -q '^FAIL caught rejection is not synchronously reported' $(BUILD)/rejection_checkpoint_negctl.log
test-rejection-checkpoint: test-rejection-checkpoint-negctl $(BUILD)/rejection_checkpoint_test
	@$(BUILD)/rejection_checkpoint_test
