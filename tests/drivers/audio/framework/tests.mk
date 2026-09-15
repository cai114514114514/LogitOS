CAPTURE_FRAMEWORK_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -Itests/drivers/audio/framework/stubs \
    -I. -Ic/kernel/audio -Iinclude/abi

.PHONY: test-capture-framework test-capture-framework-negctl
test-capture-framework-negctl:
	@mkdir -p $(BUILD)/capture-framework
	@set -e; for mutation in active-slot wrong-device; do \
	    python3 tests/drivers/audio/framework/negative.py $$mutation \
	      $(BUILD)/capture-framework/$$mutation.c $(BUILD)/capture-framework/$$mutation-test.c; \
	    $(CC) $(CAPTURE_FRAMEWORK_FLAGS) $(BUILD)/capture-framework/$$mutation-test.c \
	      c/kernel/audio/pcm.c -o $(BUILD)/capture-framework/$$mutation; \
	    if $(BUILD)/capture-framework/$$mutation >$(BUILD)/capture-framework/$$mutation.log 2>&1; then \
	      cat $(BUILD)/capture-framework/$$mutation.log; exit 1; fi; \
	    grep -q '^CAPTURE_FRAMEWORK_FAIL' $(BUILD)/capture-framework/$$mutation.log; \
	    echo "CAPTURE_FRAMEWORK_NEGCTL: $$mutation rejected"; \
	 done
	@grep -q 'received == 3 \* PERIOD_BYTES' $(BUILD)/capture-framework/active-slot.log
	@grep -q 'g_cap_periods_done == 0' $(BUILD)/capture-framework/wrong-device.log

test-capture-framework: test-capture-framework-negctl
	@$(CC) $(CAPTURE_FRAMEWORK_FLAGS) tests/drivers/audio/framework/capture_test.c \
	    c/kernel/audio/pcm.c -o $(BUILD)/capture-framework/test
	@$(BUILD)/capture-framework/test
