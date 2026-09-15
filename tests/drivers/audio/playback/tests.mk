PLAYBACK_FRAMEWORK_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -pthread -Itests/drivers/audio/playback/stubs \
    -I. -Ic/kernel/audio -Iinclude/abi

.PHONY: test-playback-framework test-playback-framework-negctl
test-playback-framework-negctl:
	@mkdir -p $(BUILD)/playback-framework
	@set -e; for mutation in reinitialize wrong-device active-slot; do \
	    python3 tests/drivers/audio/playback/negative.py $$mutation \
	      $(BUILD)/playback-framework/$$mutation.c $(BUILD)/playback-framework/$$mutation-test.c; \
	    $(CC) $(PLAYBACK_FRAMEWORK_FLAGS) $(BUILD)/playback-framework/$$mutation-test.c \
	      c/kernel/audio/pcm.c -o $(BUILD)/playback-framework/$$mutation; \
	    if $(BUILD)/playback-framework/$$mutation >$(BUILD)/playback-framework/$$mutation.log 2>&1; then \
	      cat $(BUILD)/playback-framework/$$mutation.log; exit 1; fi; \
	    grep -q '^PLAYBACK_FRAMEWORK_FAIL' $(BUILD)/playback-framework/$$mutation.log; \
	    echo "PLAYBACK_FRAMEWORK_NEGCTL: $$mutation rejected"; \
	 done
	@grep -q 'semaphore_inits == 1 && queue_inits == 8' $(BUILD)/playback-framework/reinitialize.log
	@grep -q 'g_periods_done == 1 && g_period.tokens == tokens' $(BUILD)/playback-framework/wrong-device.log
	@grep -q 'second_ring\[index\] == 0x6d' $(BUILD)/playback-framework/active-slot.log
	@python3 tests/drivers/audio/playback/negative.py scratch-channels \
	    $(BUILD)/playback-framework/scratch-channels.c $(BUILD)/playback-framework/scratch-channels-test.c
	@$(CC) $(PLAYBACK_FRAMEWORK_FLAGS) $(BUILD)/playback-framework/scratch-channels-test.c \
	    c/kernel/audio/pcm.c -o $(BUILD)/playback-framework/scratch-channels
	@if $(BUILD)/playback-framework/scratch-channels >$(BUILD)/playback-framework/scratch-channels.log 2>&1; then \
	    cat $(BUILD)/playback-framework/scratch-channels.log; exit 1; fi
	@grep -q 'AddressSanitizer: heap-buffer-overflow' $(BUILD)/playback-framework/scratch-channels.log
	@grep -q 'pcm_ring_peek' $(BUILD)/playback-framework/scratch-channels.log
	@grep -q 'eight_channel_float_input' $(BUILD)/playback-framework/scratch-channels.log
	@echo 'PLAYBACK_FRAMEWORK_NEGCTL: eight-channel input overflowed restored two-channel scratch'

test-playback-framework: test-playback-framework-negctl
	@$(CC) $(PLAYBACK_FRAMEWORK_FLAGS) tests/drivers/audio/playback/test.c \
	    c/kernel/audio/pcm.c -o $(BUILD)/playback-framework/test
	@$(BUILD)/playback-framework/test
