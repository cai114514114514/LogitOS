# Link the actual compatibility adapter and complete display implementation.
# A direct ol_display test misses fb_sdk's failed-bind publication bug.
FB_DISPLAY_SRC := tests/unit/fb_display_test.c c/kernel/gui/fb/fb.c c/kernel/gui/fb/glass.c $(GFX_SRC)
FB_DISPLAY_INC := $(KGUI_INC) $(KMM_INC) -Ic/drivers/virtio -Ic/lib/text $(GFX_INC)
FB_DISPLAY_FLAGS := -O1 -g -Wall -Wextra $(FB_DISPLAY_INC)
FB_DISPLAY_DEPS := $(FB_DISPLAY_SRC) $(GFX_HEADERS) c/kernel/gui/fb/fb.h c/kernel/gui/fb/glass.h

$(BUILD)/fb_display_test: $(FB_DISPLAY_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(FB_DISPLAY_FLAGS) $(FB_DISPLAY_SRC) -lm -o $@

$(BUILD)/fb_display_sanitize: $(FB_DISPLAY_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(FB_DISPLAY_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(FB_DISPLAY_SRC) -lm -o $@

$(BUILD)/fb_display_small: $(FB_DISPLAY_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(FB_DISPLAY_FLAGS) -DDISPLAY_PATH_LIMIT $(FB_DISPLAY_SRC) -lm -o $@

$(BUILD)/fb_display_unchecked: $(FB_DISPLAY_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(FB_DISPLAY_FLAGS) -DFB_BIND_UNCHECKED $(FB_DISPLAY_SRC) -lm -o $@

test-fb-display-control: $(BUILD)/fb_display_small $(BUILD)/fb_display_unchecked
	@rc=0; $(BUILD)/fb_display_small > $(BUILD)/fb-display-small.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg '^FAIL fresh 5K bind initializes RGB channels$$' $(BUILD)/fb-display-small.log
	@rc=0; $(BUILD)/fb_display_unchecked > $(BUILD)/fb-display-unchecked.log 2>&1 || rc=$$?; test $$rc -eq 1
	@rg '^FAIL failed bind never writes the unvalidated target$$' $(BUILD)/fb-display-unchecked.log
	@echo 'Framebuffer controls: 4096 cap and unvalidated-target writes were both observed failing.'

test-fb-display: test-fb-display-control $(BUILD)/fb_display_test $(BUILD)/fb_display_sanitize
	$(BUILD)/fb_display_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/fb_display_sanitize

ci-host: test-fb-display
.PHONY: test-fb-display test-fb-display-control
