POWER_EC_DIR := $(BUILD)/power-ec
POWER_EC_FLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Ic/drivers/power/acpi/ec -Ic/drivers/power/acpi -Ithird_party/uacpi/include -DUACPI_DEFAULT_LOOP_TIMEOUT_SECONDS=2 -DUACPI_DEFAULT_MAX_CALL_STACK_DEPTH=64
POWER_EC_TRANSPORT := c/drivers/power/acpi/ec/transport.c
POWER_EC_DRIVER := c/drivers/power/acpi/ec/driver.c c/drivers/power/acpi/ec/resources.c
POWER_EC_MODEL := tests/drivers/power/acpi/ec/model.c
POWER_EC_UNIT := tests/drivers/power/acpi/ec/test.c
POWER_EC_AML := $(wildcard third_party/uacpi/source/*.c) c/drivers/power/acpi/devices.c $(POWER_EC_DRIVER) $(POWER_EC_TRANSPORT) tests/drivers/power/acpi/host.c tests/drivers/power/acpi/ec/board.c $(POWER_EC_MODEL) tests/drivers/power/acpi/ec/aml_test.c
POWER_EC_SCENARIOS := success ecdt pending-query shared decode10 fixed-io overlap gpe-package gpe-owned ecdt-mismatch ecdt-unterminated timeout

.PHONY: test-power-ec-negctl test-power-ec-order-negctl test-power-ec-host test-power-ec-aml
# Remove the visibility delay from a copy. The independent bus model must
# reject an early overwrite, rather than accepting a synchronously cleared IBF.
test-power-ec-negctl:
	@mkdir -p $(POWER_EC_DIR)
	@python3 -c 'from pathlib import Path; source=Path("$(POWER_EC_TRANSPORT)").read_text(); old="transport->ops.delay_us(transport->ops.context, 1);"; assert source.count(old)==1; Path("$(POWER_EC_DIR)/negative.c").write_text(source.replace(old,"transport->ops.delay_us(transport->ops.context, 0);"))'
	@$(CC) $(POWER_EC_FLAGS) $(POWER_EC_DIR)/negative.c $(POWER_EC_MODEL) $(POWER_EC_UNIT) -o $(POWER_EC_DIR)/negative
	@if ASAN_OPTIONS=detect_leaks=0 $(POWER_EC_DIR)/negative >$(POWER_EC_DIR)/negative.log 2>&1; then cat $(POWER_EC_DIR)/negative.log; exit 1; fi
	@grep -q '^FAIL: model.early_writes == 0$$' $(POWER_EC_DIR)/negative.log
	@grep -q '^EC_TRANSPORT: .* checks, [1-9][0-9]* failures$$' $(POWER_EC_DIR)/negative.log
	@echo 'EC_NEGCTL: missing IBF visibility delay caused observable early writes'

test-power-ec-host: test-power-ec-negctl
	@$(CC) $(POWER_EC_FLAGS) $(POWER_EC_TRANSPORT) $(POWER_EC_MODEL) $(POWER_EC_UNIT) -o $(POWER_EC_DIR)/transport
	@ASAN_OPTIONS=detect_leaks=0 $(POWER_EC_DIR)/transport

# _INI must see an already connected EC region. Swap the real initialization
# calls in a copy and watch the firmware's EC-backed initialization byte fail.
test-power-ec-order-negctl:
	@mkdir -p $(POWER_EC_DIR)
	@python3 -c 'from pathlib import Path; source=Path("c/drivers/power/acpi/devices.c").read_text(); old="        power_ec_initialize();\n        status = uacpi_namespace_initialize();"; assert source.count(old)==1; replacement="        status = uacpi_namespace_initialize();\n        power_ec_initialize();"; Path("$(POWER_EC_DIR)/negative-init.c").write_text(source.replace(old,replacement))'
	@$(CC) $(POWER_EC_FLAGS) -DPOWER_EC_BOARD $(filter-out c/drivers/power/acpi/devices.c,$(POWER_EC_AML)) $(POWER_EC_DIR)/negative-init.c -o $(POWER_EC_DIR)/negative-init
	@python3 tests/drivers/power/acpi/ec/fixture.py success $(POWER_EC_DIR)/negative-init.bin
	@if ASAN_OPTIONS=detect_leaks=0 $(POWER_EC_DIR)/negative-init success $(POWER_EC_DIR)/negative-init.bin >$(POWER_EC_DIR)/negative-init.log 2>&1; then cat $(POWER_EC_DIR)/negative-init.log; exit 1; fi
	@grep -q '^FAIL: power_ec_model.memory\[0x11\] == 1$$' $(POWER_EC_DIR)/negative-init.log
	@grep -q '^EC_AML success: 32 checks, 1 failures$$' $(POWER_EC_DIR)/negative-init.log
	@echo 'EC_ORDER_NEGCTL: late EC handler left the real AML _INI write unexecuted'

test-power-ec-aml: test-power-ec-host test-power-ec-order-negctl
	@$(CC) $(POWER_EC_FLAGS) -DPOWER_EC_BOARD $(POWER_EC_AML) -o $(POWER_EC_DIR)/aml
	@for scenario in $(POWER_EC_SCENARIOS); do \
	    python3 tests/drivers/power/acpi/ec/fixture.py $$scenario $(POWER_EC_DIR)/$$scenario.bin && \
	    ASAN_OPTIONS=detect_leaks=0 $(POWER_EC_DIR)/aml $$scenario $(POWER_EC_DIR)/$$scenario.bin || exit 1; \
	done

ci-host: test-power-ec-aml
