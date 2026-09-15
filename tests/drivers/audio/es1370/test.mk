ES1370_HOST_SOURCES := tests/drivers/audio/es1370/test.c $(wildcard c/drivers/audio/es1370/*.c)
ES1370_HOST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror \
    -Itests/drivers/audio/es1370/stubs -Ic/drivers/audio/es1370 \
    -Ic/drivers/core -Ic/kernel/audio -Ic/kernel/pci -Ic/kernel/core \
    -Ic/kernel/mm/phys -Ic/kernel/diag -Iinclude/abi

.PHONY: test-es1370-host test-es1370-negctl test-es1370-capture-negctl
test-es1370-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(ES1370_HOST_FLAGS) -fsanitize=address,undefined \
	    -DES1370_NEGCTL_DROP_ELAPSED $(ES1370_HOST_SOURCES) -o $(BUILD)/es1370-negctl
	@if $(BUILD)/es1370-negctl >$(BUILD)/es1370-negctl.log 2>&1; then \
	    cat $(BUILD)/es1370-negctl.log; exit 1; fi
	@grep -q 'FAIL line .*callbacks == 3 && sound->position(sound) == 3072' $(BUILD)/es1370-negctl.log
	@grep -q '^ES1370_HOST: .* checks, 6 failures$$' $(BUILD)/es1370-negctl.log
	@echo 'ES1370_NEGCTL: hardware PCM consumption without mixer period notifications rejected'

test-es1370-capture-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(ES1370_HOST_FLAGS) -fsanitize=address,undefined \
	    -DES1370_NEGCTL_DROP_CAPTURE $(ES1370_HOST_SOURCES) -o $(BUILD)/es1370-capture-negctl
	@if $(BUILD)/es1370-capture-negctl >$(BUILD)/es1370-capture-negctl.log 2>&1; then \
	    cat $(BUILD)/es1370-capture-negctl.log; exit 1; fi
	@grep -q 'FAIL line .*capture_callbacks == 3' $(BUILD)/es1370-capture-negctl.log
	@grep -q '^ES1370_HOST: .* checks, 4 failures$$' $(BUILD)/es1370-capture-negctl.log
	@echo 'ES1370_CAPTURE_NEGCTL: DMA input without capture period publication rejected'

test-es1370-host: test-es1370-negctl test-es1370-capture-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(ES1370_HOST_FLAGS) -fsanitize=address,undefined \
	    $(ES1370_HOST_SOURCES) -o $(BUILD)/es1370-host
	@$(BUILD)/es1370-host
