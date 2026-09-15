# Derive the full loader/painter source set; replace only the transport fixture.
INSERTED_QUEUE_SRC = $(filter-out tests/unit/inserted_script_async_test.c tests/unit/inserted_script_held_fake.c,$(INSERTED_ASYNC_SRC)) tests/unit/inserted_script_queue_test.c tests/unit/inserted_script_queue_fake.c
INSERTED_QUEUE_DIR = $(BUILD)/site-general/inserted-script-queue
INSERTED_QUEUE_DEPS = $(INSERTED_QUEUE_SRC) $(INSERTED_ASYNC_DEPS)
$(INSERTED_QUEUE_DIR)/current: $(INSERTED_QUEUE_DEPS)
	@mkdir -p $(INSERTED_QUEUE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(INSERTED_QUEUE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(INSERTED_QUEUE_DIR)/fixed: $(INSERTED_QUEUE_DEPS)
	@mkdir -p $(INSERTED_QUEUE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_INSERTED_QUEUE_FIXED -o $@ $(INSERTED_QUEUE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(INSERTED_QUEUE_DIR)/serial: $(INSERTED_QUEUE_DEPS)
	@mkdir -p $(INSERTED_QUEUE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_INSERTED_FETCH_SERIAL -o $@ $(INSERTED_QUEUE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-inserted-script-queue test-inserted-script-queue-negctl test-inserted-script-queue-san
test-inserted-script-queue-negctl: $(INSERTED_QUEUE_DIR)/fixed $(INSERTED_QUEUE_DIR)/serial
	@rc=0; $(INSERTED_QUEUE_DIR)/fixed > $(INSERTED_QUEUE_DIR)/fixed.log 2>&1 || rc=$$?; tail -14 $(INSERTED_QUEUE_DIR)/fixed.log; test $$rc -eq 1 && grep -q 'FAIL: all 96 inserted resources' $(INSERTED_QUEUE_DIR)/fixed.log
	@rc=0; $(INSERTED_QUEUE_DIR)/serial > $(INSERTED_QUEUE_DIR)/serial.log 2>&1 || rc=$$?; tail -14 $(INSERTED_QUEUE_DIR)/serial.log; test $$rc -eq 1 && grep -q 'FAIL: inserted downloads overlap' $(INSERTED_QUEUE_DIR)/serial.log && grep -q 'ok: all 96 inserted resources' $(INSERTED_QUEUE_DIR)/serial.log
test-inserted-script-queue: test-inserted-script-queue-negctl $(INSERTED_QUEUE_DIR)/current
	@$(INSERTED_QUEUE_DIR)/current > $(INSERTED_QUEUE_DIR)/current.log 2>&1; rc=$$?; tail -14 $(INSERTED_QUEUE_DIR)/current.log; exit $$rc
$(INSERTED_QUEUE_DIR)/san: $(INSERTED_QUEUE_DEPS)
	@mkdir -p $(INSERTED_QUEUE_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(INSERTED_QUEUE_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-inserted-script-queue-san: test-inserted-script-queue $(INSERTED_QUEUE_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(INSERTED_QUEUE_DIR)/san > $(INSERTED_QUEUE_DIR)/san.log 2>&1; rc=$$?; tail -14 $(INSERTED_QUEUE_DIR)/san.log; exit $$rc
ci-host: test-inserted-script-queue
