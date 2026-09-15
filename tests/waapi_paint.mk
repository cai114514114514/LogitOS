# Production DOM + deadline + layout, with the computed-style-only world as
# the negative control. Its JS timing still runs; only native commit is absent.
WAAPI_DIR := $(BUILD)/wiring-next/waapi
WAAPI_SRC = tests/unit/waapi_paint_test.c $(sort $(JSDOM_HOST_SRC) $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) c/apps/browser/js_anim.c c/apps/browser/css_interp.c)
WAAPI_DEPS = $(WAAPI_SRC) c/apps/browser/js_waapi_native.inc c/apps/browser/js_anim.h tests/unit/layout_box_test.c $(BUILD)/libcss_host.a
.PHONY: test-waapi-paint test-waapi-paint-negctl
$(WAAPI_DIR)/test: $(WAAPI_DEPS)
	@mkdir -p $(WAAPI_DIR)
	$(CC) -O1 -g -w $(JSDOM_HOST_CF) -o $@ $(WAAPI_SRC) $(BUILD)/libcss_host.a -lm
$(WAAPI_DIR)/negctl: $(WAAPI_DEPS)
	@mkdir -p $(WAAPI_DIR)
	$(CC) -O1 -g -w $(JSDOM_HOST_CF) -DJS_WAAPI_NO_PAINT -o $@ $(WAAPI_SRC) $(BUILD)/libcss_host.a -lm
test-waapi-paint-negctl: $(WAAPI_DIR)/negctl
	@rc=0; $< >$(WAAPI_DIR)/negctl.log 2>&1 || rc=$$?; cat $(WAAPI_DIR)/negctl.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: WAAPI midpoint reaches display-list opacity' $(WAAPI_DIR)/negctl.log && grep -q 'FAIL: WAAPI midpoint reaches painter transform' $(WAAPI_DIR)/negctl.log
test-waapi-paint: test-waapi-paint-negctl test-waapi-checkpoint-negctl $(WAAPI_DIR)/test
	@$(WAAPI_DIR)/test
ci-host: test-waapi-paint
$(WAAPI_DIR)/address: $(WAAPI_DEPS)
	@mkdir -p $(WAAPI_DIR)
	$(CC) -O1 -g -w -fsanitize=address -fno-omit-frame-pointer $(JSDOM_HOST_CF) -o $@ $(WAAPI_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-waapi-paint-asan
test-waapi-paint-asan: test-waapi-paint $(WAAPI_DIR)/address
	@ASAN_OPTIONS=detect_leaks=0 $(WAAPI_DIR)/address

$(WAAPI_DIR)/checkpoint-negctl: $(WAAPI_DEPS)
	@mkdir -p $(WAAPI_DIR)
	$(CC) -O1 -g -w $(JSDOM_HOST_CF) -DJS_WAAPI_NO_CHECKPOINT -o $@ $(WAAPI_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-waapi-checkpoint-negctl
test-waapi-checkpoint-negctl: $(WAAPI_DIR)/checkpoint-negctl
	@rc=0; $< >$(WAAPI_DIR)/checkpoint-negctl.log 2>&1 || rc=$$?; cat $(WAAPI_DIR)/checkpoint-negctl.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: automatic finish reactions drain without timers or pixel changes' $(WAAPI_DIR)/checkpoint-negctl.log && grep -q 'FAIL: microtask-only completion requests browser settlement' $(WAAPI_DIR)/checkpoint-negctl.log
