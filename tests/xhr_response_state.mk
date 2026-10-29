XHR_STATE_SRC = tests/unit/xhr_response_state_test.c $(filter-out tests/unit/xhr_progress_test.c,$(XHR_PROGRESS_SRC))
XHR_STATE_DEP = $(XHR_STATE_SRC) tests/unit/xhr_progress_test.c $(XHR_PROGRESS_DEP)
$(XHR_PROGRESS_DIR)/state: $(XHR_STATE_DEP)
	@mkdir -p $(XHR_PROGRESS_DIR)
	$(CC) $(XHR_PROGRESS_CF) -o $@ $(XHR_STATE_SRC) $(RUST_LIB_HOST) -lm
$(XHR_PROGRESS_DIR)/state-old: $(XHR_STATE_DEP)
	@mkdir -p $(XHR_PROGRESS_DIR)
	$(CC) $(XHR_PROGRESS_CF) -DXHR_PUBLIC_RESPONSE_LEGACY -o $@ $(XHR_STATE_SRC) $(RUST_LIB_HOST) -lm
$(XHR_PROGRESS_DIR)/state-san: $(XHR_STATE_DEP)
	@mkdir -p $(XHR_PROGRESS_DIR)
	$(CC) $(XHR_PROGRESS_CF) -O1 -g -fsanitize=address,undefined -o $@ $(XHR_STATE_SRC) $(RUST_LIB_HOST) -lm
.PHONY: test-xhr-response-state test-xhr-response-state-negctl test-xhr-response-state-san
test-xhr-response-state-negctl: $(XHR_PROGRESS_DIR)/state-old
	@rc=0; $< > $(XHR_PROGRESS_DIR)/state-old.log 2>&1 || rc=$$?; cat $(XHR_PROGRESS_DIR)/state-old.log; test $$rc -eq 1 && grep -q '^FAIL: wrapped readonly getter permits XHR construction' $(XHR_PROGRESS_DIR)/state-old.log
test-xhr-response-state: test-xhr-response-state-negctl $(XHR_PROGRESS_DIR)/state
	@$(XHR_PROGRESS_DIR)/state
test-xhr-response-state-san: test-xhr-response-state $(XHR_PROGRESS_DIR)/state-san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(XHR_PROGRESS_DIR)/state-san
ci-host: test-xhr-response-state
