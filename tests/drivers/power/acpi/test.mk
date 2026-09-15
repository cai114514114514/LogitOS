POWER_AML_DIR := $(BUILD)/power-aml
POWER_AML_FLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Ithird_party/uacpi/include -Ic/drivers/power/acpi -DUACPI_DEFAULT_LOOP_TIMEOUT_SECONDS=2 -DUACPI_DEFAULT_MAX_CALL_STACK_DEPTH=64
POWER_AML_UPSTREAM := $(wildcard third_party/uacpi/source/*.c)
POWER_AML_BACKEND := tests/drivers/power/acpi/host.c tests/drivers/power/acpi/test.c
POWER_AML_EC := $(wildcard c/drivers/power/acpi/ec/*.c)
POWER_AML_DEVICE := c/drivers/power/acpi/devices.c
POWER_AML_SCENARIOS := success empty unknown bad-battery missing-battery-method bad-temperature bad-sleep bad-checksum

.PHONY: test-power-aml-negctl test-power-aml-host
# Mutate a build-directory copy, so a passing test cannot be explained by
# compiling a fixture parser instead of the production AML consumer.
test-power-aml-negctl:
	@mkdir -p $(POWER_AML_DIR)
	@python3 -c 'from pathlib import Path; text=Path("$(POWER_AML_DEVICE)").read_text(); old="KELVIN_ZERO_MILLICELSIUS = 273150"; assert text.count(old)==1; Path("$(POWER_AML_DIR)/negative.c").write_text(text.replace(old,"KELVIN_ZERO_MILLICELSIUS = 273050"))'
	@$(CC) $(POWER_AML_FLAGS) $(POWER_AML_UPSTREAM) $(POWER_AML_EC) $(POWER_AML_BACKEND) $(POWER_AML_DIR)/negative.c -o $(POWER_AML_DIR)/negative
	@python3 tests/drivers/power/acpi/fixture.py success $(POWER_AML_DIR)/success.bin
	@if ASAN_OPTIONS=detect_leaks=0 $(POWER_AML_DIR)/negative success $(POWER_AML_DIR)/success.bin >$(POWER_AML_DIR)/negative.log 2>&1; then cat $(POWER_AML_DIR)/negative.log; exit 1; fi
	@grep -q '^FAIL: thermal->temperature_millic == 26950$$' $(POWER_AML_DIR)/negative.log
	@grep -q '^POWER_AML success: 25 checks, 3 failures$$' $(POWER_AML_DIR)/negative.log
	@echo 'POWER_AML_NEGCTL: wrong Kelvin conversion caused 3 real AML consumer failures'

test-power-aml-host: test-power-aml-negctl
	@$(CC) $(POWER_AML_FLAGS) $(POWER_AML_UPSTREAM) $(POWER_AML_EC) $(POWER_AML_BACKEND) $(POWER_AML_DEVICE) -o $(POWER_AML_DIR)/test
	@for scenario in $(POWER_AML_SCENARIOS); do \
	    python3 tests/drivers/power/acpi/fixture.py $$scenario $(POWER_AML_DIR)/$$scenario.bin && \
	    ASAN_OPTIONS=detect_leaks=0 $(POWER_AML_DIR)/test $$scenario $(POWER_AML_DIR)/$$scenario.bin || exit 1; \
	done

ci-host: test-power-aml-host
