AC97_DRIVER_SOURCES := $(filter-out c/drivers/audio/ac97/ports.c,$(wildcard c/drivers/audio/ac97/*.c))
AC97_HOST_SOURCES := tests/drivers/audio/ac97/host_test.c $(AC97_DRIVER_SOURCES)
AC97_HOST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I. -Ic/drivers/audio/ac97 -Iinclude/abi -Ic/kernel/diag
.PHONY: test-ac97-host test-ac97-negctl

test-ac97-negctl:
	@mkdir -p $(BUILD)/audio/ac97
	@python3 tests/drivers/audio/ac97/negative.py c/drivers/audio/ac97/controller.c $(BUILD)/audio/ac97/control.c
	@$(CC) $(AC97_HOST_FLAGS) tests/drivers/audio/ac97/host_test.c $(BUILD)/audio/ac97/control.c $(filter-out c/drivers/audio/ac97/controller.c,$(AC97_DRIVER_SOURCES)) -o $(BUILD)/audio/ac97/negative
	@if ASAN_OPTIONS=detect_leaks=0 $(BUILD)/audio/ac97/negative >$(BUILD)/audio/ac97/negative.log 2>&1; then cat $(BUILD)/audio/ac97/negative.log; exit 1; fi
	@grep -q 'FAIL .*entry\[1\] == 0x80000800u' $(BUILD)/audio/ac97/negative.log
	@grep -q '^AC97_HOST: .* checks, [1-9][0-9]* failures$$' $(BUILD)/audio/ac97/negative.log
	@echo 'AC97_NEGCTL: stereo-frame descriptor length rejected'

test-ac97-host: test-ac97-negctl
	@$(CC) $(AC97_HOST_FLAGS) $(AC97_HOST_SOURCES) -o $(BUILD)/audio/ac97/host
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/audio/ac97/host >$(BUILD)/audio/ac97/host.log 2>&1 || { cat $(BUILD)/audio/ac97/host.log; exit 1; }
	@cat $(BUILD)/audio/ac97/host.log
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/audio/ac97/host complete >$(BUILD)/audio/ac97/host-complete.log 2>&1 || { cat $(BUILD)/audio/ac97/host-complete.log; exit 1; }
	@cat $(BUILD)/audio/ac97/host-complete.log

.PHONY: test-ac97-position-negctl
test-ac97-position-negctl:
	@mkdir -p $(BUILD)/audio/ac97
	@python3 tests/drivers/audio/ac97/negative.py c/drivers/audio/ac97/controller.c $(BUILD)/audio/ac97/position-control.c position
	@$(CC) $(AC97_HOST_FLAGS) tests/drivers/audio/ac97/host_test.c $(BUILD)/audio/ac97/position-control.c $(filter-out c/drivers/audio/ac97/controller.c,$(AC97_DRIVER_SOURCES)) -o $(BUILD)/audio/ac97/position-negative
	@if ASAN_OPTIONS=detect_leaks=0 $(BUILD)/audio/ac97/position-negative >$(BUILD)/audio/ac97/position-negative.log 2>&1; then cat $(BUILD)/audio/ac97/position-negative.log; exit 1; fi
	@grep -q 'FAIL .*sound->position(sound) == 32768' $(BUILD)/audio/ac97/position-negative.log
	@grep -q '^AC97_HOST: .* checks, [1-9][0-9]* failures$$' $(BUILD)/audio/ac97/position-negative.log
	@echo 'AC97_POSITION_NEGCTL: double-counting terminal CIV is detected'
test-ac97-host: test-ac97-position-negctl

.PHONY: test-ac97-capture-negctl
test-ac97-capture-negctl:
	@mkdir -p $(BUILD)/audio/ac97
	@python3 tests/drivers/audio/ac97/negative.py c/drivers/audio/ac97/streams.c $(BUILD)/audio/ac97/capture-control.c capture
	@$(CC) $(AC97_HOST_FLAGS) tests/drivers/audio/ac97/host_test.c $(BUILD)/audio/ac97/capture-control.c $(filter-out c/drivers/audio/ac97/streams.c,$(AC97_DRIVER_SOURCES)) -o $(BUILD)/audio/ac97/capture-negative
	@if ASAN_OPTIONS=detect_leaks=0 $(BUILD)/audio/ac97/capture-negative >$(BUILD)/audio/ac97/capture-negative.log 2>&1; then cat $(BUILD)/audio/ac97/capture-negative.log; exit 1; fi
	@grep -q 'FAIL .*capture_notifications == period + 1' $(BUILD)/audio/ac97/capture-negative.log
	@grep -q '^AC97_HOST: .* checks, [1-9][0-9]* failures$$' $(BUILD)/audio/ac97/capture-negative.log
	@echo 'AC97_CAPTURE_NEGCTL: discarded input period notifications are detected'
test-ac97-host: test-ac97-capture-negctl

.PHONY: test-ac97-model-negctl test-ac97-capture-only-negctl
test-ac97-model-negctl:
	@mkdir -p $(BUILD)/audio/ac97
	@python3 tests/drivers/audio/ac97/negative.py c/drivers/audio/ac97/controller.c $(BUILD)/audio/ac97/model-control.c channel-readback
	@$(CC) $(AC97_HOST_FLAGS) tests/drivers/audio/ac97/host_test.c $(BUILD)/audio/ac97/model-control.c $(filter-out c/drivers/audio/ac97/controller.c,$(AC97_DRIVER_SOURCES)) -o $(BUILD)/audio/ac97/model-negative
	@if ASAN_OPTIONS=detect_leaks=0 $(BUILD)/audio/ac97/model-negative >$(BUILD)/audio/ac97/model-negative.log 2>&1; then cat $(BUILD)/audio/ac97/model-negative.log; exit 1; fi
	@grep -q 'FAIL .*delay_count == 1000 && !sound && !capture' $(BUILD)/audio/ac97/model-negative.log
	@grep -q '^AC97_HOST: .* checks, [1-9][0-9]* failures$$' $(BUILD)/audio/ac97/model-negative.log
	@echo 'AC97_MODEL_NEGCTL: ignored ICH2 stereo-mode readback is detected'

test-ac97-capture-only-negctl:
	@mkdir -p $(BUILD)/audio/ac97
	@python3 tests/drivers/audio/ac97/negative.py c/drivers/audio/ac97/pci.c $(BUILD)/audio/ac97/capture-only-control.c capture-only
	@$(CC) $(AC97_HOST_FLAGS) tests/drivers/audio/ac97/host_test.c $(BUILD)/audio/ac97/capture-only-control.c $(filter-out c/drivers/audio/ac97/pci.c,$(AC97_DRIVER_SOURCES)) -o $(BUILD)/audio/ac97/capture-only-negative
	@if ASAN_OPTIONS=detect_leaks=0 $(BUILD)/audio/ac97/capture-only-negative >$(BUILD)/audio/ac97/capture-only-negative.log 2>&1; then cat $(BUILD)/audio/ac97/capture-only-negative.log; exit 1; fi
	@grep -q 'FAIL .*init_calls == previous_init' $(BUILD)/audio/ac97/capture-only-negative.log
	@grep -q '^AC97_HOST: .* checks, [1-9][0-9]* failures$$' $(BUILD)/audio/ac97/capture-only-negative.log
	@echo 'AC97_CAPTURE_ONLY_NEGCTL: resetting the other playback owner is detected'
test-ac97-host: test-ac97-model-negctl test-ac97-capture-only-negctl
