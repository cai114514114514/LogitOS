# ACPI is the common discovery path on both the X79/Xeon E5 and Z790/14700KF
# targets.  The production gate is host-side because it validates byte-format
# firmware records; a boot after it proves the helper is also linked into the
# target kernel.  The control restores the old missing checksum decisions and
# must visibly accept exactly the two corrupted records below.
ACPI_INTEGRITY_SRC := tests/unit/acpi_integrity_test.c c/kernel/cpu/acpi/acpi_integrity.c
ACPI_INTEGRITY_INC := $(KCPU_INC)

.PHONY: test-acpi-integrity-negctl test-acpi-integrity-host

test-acpi-integrity-negctl:
	@mkdir -p $(BUILD)/acpi-integrity
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror \
	    -DACPI_INTEGRITY_NEGCTL_PRECHECKSUM $(ACPI_INTEGRITY_INC) \
	    $(ACPI_INTEGRITY_SRC) -o $(BUILD)/acpi-integrity/negctl
	@if $(BUILD)/acpi-integrity/negctl >$(BUILD)/acpi-integrity/negctl.log 2>&1; then \
	    cat $(BUILD)/acpi-integrity/negctl.log; exit 1; fi
	@grep -q '^FAIL: bad SDT checksum is rejected$$' $(BUILD)/acpi-integrity/negctl.log
	@grep -q '^FAIL: bad ACPI 2.0 extended checksum is rejected$$' $(BUILD)/acpi-integrity/negctl.log
	@grep -q '^ACPI_INTEGRITY: 15 checks, 2 failures$$' $(BUILD)/acpi-integrity/negctl.log
	@echo 'ACPI_INTEGRITY_NEGCTL: pre-fix parser accepted corrupt SDT and ACPI 2.0 RSDP (2 failures)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror \
	    -DACPI_INTEGRITY_NEGCTL_SKIP_SIGNATURE $(ACPI_INTEGRITY_INC) \
	    $(ACPI_INTEGRITY_SRC) -o $(BUILD)/acpi-integrity/signature-negctl
	@set +e; out=`$(BUILD)/acpi-integrity/signature-negctl 2>&1`; rc=$$?; set -e; \
	 echo "$$out" | grep -q '^FAIL: wrong root SDT signature is rejected$$'; \
	 test `echo "$$out" | grep -c '^FAIL:'` -eq 1; test $$rc -ne 0; \
	 echo 'ACPI-SIGNATURE-NEGCTL: accepting the wrong root kind reddened exactly one check'

test-acpi-integrity-host: test-acpi-integrity-negctl
	@mkdir -p $(BUILD)/acpi-integrity
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
	    $(ACPI_INTEGRITY_INC) $(ACPI_INTEGRITY_SRC) \
	    -o $(BUILD)/acpi-integrity/host
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/acpi-integrity/host

ci-host: test-acpi-integrity-host
