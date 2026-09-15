# Real framebuffer transport integration. The deliberate CPU-after-GPU mutation
# must visibly overwrite the callback oracle; this catches fake acceleration
# that merely calls a hook and then follows the old CPU present unconditionally.
FB_NATIVE_PRESENT_SRC := tests/unit/fb_native_present_test.c c/kernel/gui/fb.c c/lib/gfx/openlogit_display.c
FB_NATIVE_PRESENT_INC := -Ic/kernel/gui -Ic/drivers/virtio -Ic/kernel/mm -Ic/lib/text -Ic/lib/gfx
.PHONY: test-fb-native-present-negctl test-fb-native-present-host

test-fb-native-present-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -DFB_DMA_HOSTTEST \
	    -DFB_NATIVE_PRESENT_NEGCTL_CPU_FALLBACK $(FB_NATIVE_PRESENT_INC) \
	    $(FB_NATIVE_PRESENT_SRC) -o $(BUILD)/fb-native-present-negctl
	@rc=0; $(BUILD)/fb-native-present-negctl >$(BUILD)/fb-native-present-negctl.log 2>&1 || rc=$$?; \
	    test $$rc -eq 1 && grep -F 'FAIL GPU success preserves callback pixels without CPU overwrite' $(BUILD)/fb-native-present-negctl.log
	@grep -q '^FB_NATIVE_PRESENT: 21 checks, 1 failures$$' $(BUILD)/fb-native-present-negctl.log
	@echo 'FB_NATIVE_PRESENT_NEGCTL: CPU overwrite detected (exactly one failure)'

test-fb-native-present-host: test-fb-native-present-negctl
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
	    -DFB_DMA_HOSTTEST $(FB_NATIVE_PRESENT_INC) $(FB_NATIVE_PRESENT_SRC) \
	    -o $(BUILD)/fb-native-present-host
	@$(BUILD)/fb-native-present-host

ci-host: test-fb-native-present-host
