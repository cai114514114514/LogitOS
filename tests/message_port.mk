PORT_DIR := $(BUILD)/wiring-next/message-port
PORT_SRC = tests/unit/message_port_test.c $(filter-out tests/unit/semantics_test.c,$(SEM_SRC))
# Keep the fallback and its own controls paired. The production wildcard now
# links js_ports.c; the old JS-only mutations cannot break that implementation.
# Run the identical page assertions against BOTH paths, with native ownership
# controls required separately, rather than quietly losing the legacy gate.
PORT_LEGACY_SRC = $(filter-out c/apps/browser/js_ports.c,$(PORT_SRC))
PORT_DEPS = tests/message_port.mk c/apps/browser/js_message_port.inc $(PORT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-message-port test-message-port-negctl
$(PORT_DIR)/test: $(PORT_DEPS)
	@mkdir -p $(PORT_DIR)
	$(CC) -O1 -g -w $(SEM_CF) -o $@ $(PORT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(PORT_DIR)/clone-negctl: $(PORT_DEPS)
	@mkdir -p $(PORT_DIR)
	$(CC) -O1 -g -w $(SEM_CF) -DMESSAGE_PORT_NO_CLONE -o $@ $(PORT_LEGACY_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(PORT_DIR)/close-negctl: $(PORT_DEPS)
	@mkdir -p $(PORT_DIR)
	$(CC) -O1 -g -w $(SEM_CF) -DMESSAGE_PORT_NO_CLOSE_FENCE -o $@ $(PORT_LEGACY_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-message-port-negctl: $(PORT_DIR)/clone-negctl $(PORT_DIR)/close-negctl
	@rc=0; $(PORT_DIR)/clone-negctl >$(PORT_DIR)/clone-negctl.log 2>&1 || rc=$$?; cat $(PORT_DIR)/clone-negctl.log; test $$rc -eq 1 && grep -q '^FAIL message clone is taken before sender mutation' $(PORT_DIR)/clone-negctl.log
	@rc=0; $(PORT_DIR)/close-negctl >$(PORT_DIR)/close-negctl.log 2>&1 || rc=$$?; cat $(PORT_DIR)/close-negctl.log; test $$rc -eq 1 && grep -q '^FAIL closed destination never receives queued or new messages' $(PORT_DIR)/close-negctl.log
$(PORT_DIR)/legacy: $(PORT_DEPS)
	@mkdir -p $(PORT_DIR)
	$(CC) -O1 -g -w $(SEM_CF) -o $@ $(PORT_LEGACY_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-message-port: test-message-port-negctl test-native-ports $(PORT_DIR)/legacy $(PORT_DIR)/test
	@$(PORT_DIR)/legacy
	@$(PORT_DIR)/test
ci-host: test-message-port
$(PORT_DIR)/address: $(PORT_DEPS)
	@mkdir -p $(PORT_DIR)
	$(CC) -O1 -g -w -fsanitize=address -fno-omit-frame-pointer $(SEM_CF) -o $@ $(PORT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-message-port-asan
test-message-port-asan: test-message-port $(PORT_DIR)/address
	@ASAN_OPTIONS=detect_leaks=0 $(PORT_DIR)/address
$(PORT_DIR)/platform-regression: $(PLATFORM_TEST_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) c/apps/browser/js_message_port.inc
	@mkdir -p $(PORT_DIR)
	$(CC) -O1 -g -w $(SEM_CF) -o $@ $(PLATFORM_TEST_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-message-port-platform
test-message-port-platform: test-message-port $(PORT_DIR)/platform-regression
	@$(PORT_DIR)/platform-regression
