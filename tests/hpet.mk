# ACPI HPET clocksource: host parsing/capability gates plus an emulated guest.
# The absent-device QEMU boot is a prerequisite, so the positive cannot pass by
# matching a generic time line from a kernel that never found an HPET.
.PHONY: test-hpet-host test-hpet-negctl test-hpet-guest test-hpet-guest-negctl

test-hpet-host: test-hpet-negctl
	@mkdir -p $(BUILD)/hpet
	@$(CC) -O2 -Wall -Wextra -DLOGIT_HPET_HOST -Ic/drivers/timer \
	    tests/unit/hpet_test.c c/drivers/timer/hpet.c -o $(BUILD)/hpet/hpet_test
	@$(BUILD)/hpet/hpet_test

test-hpet-negctl:
	@mkdir -p $(BUILD)/hpet
	@$(CC) -O2 -Wall -Wextra -DLOGIT_HPET_HOST -DHPET_NEGCTL_SKIP_GAS \
	    -Ic/drivers/timer tests/unit/hpet_test.c c/drivers/timer/hpet.c \
	    -o $(BUILD)/hpet/hpet_test_neg
	@set +e; out=`$(BUILD)/hpet/hpet_test_neg 2>&1`; rc=$$?; set -e; \
	 echo "$$out" | grep -q 'FAIL: system-I/O GAS accepted as an MMIO pointer'; \
	 test `echo "$$out" | grep -c '^FAIL:'` -eq 1; test $$rc -ne 0; \
	 $(CC) -O2 -Wall -Wextra -DLOGIT_HPET_HOST -DHPET_NEGCTL_SKIP_BLOCK_ID \
	    -Ic/drivers/timer tests/unit/hpet_test.c c/drivers/timer/hpet.c \
	    -o $(BUILD)/hpet/hpet_test_block_neg; \
	 set +e; out=`$(BUILD)/hpet/hpet_test_block_neg 2>&1`; rc=$$?; set -e; \
	 echo "$$out" | grep -q 'FAIL: firmware/hardware block-ID mismatch accepted'; \
	 test `echo "$$out" | grep -c '^FAIL:'` -eq 1; test $$rc -ne 0; \
	 $(CC) -O2 -Wall -Wextra -DLOGIT_HPET_HOST -DHPET_NEGCTL_SKIP_REVISION \
	    -Ic/drivers/timer tests/unit/hpet_test.c c/drivers/timer/hpet.c \
	    -o $(BUILD)/hpet/hpet_test_revision_neg; \
	 set +e; out=`$(BUILD)/hpet/hpet_test_revision_neg 2>&1`; rc=$$?; set -e; \
	 echo "$$out" | grep -q 'FAIL: reserved HPET revision zero accepted'; \
	 test `echo "$$out" | grep -c '^FAIL:'` -eq 1; test $$rc -ne 0; \
	 echo 'HPET-NEGCTL-OK: unsafe GAS, block-ID, and revision acceptance each reddened the host gate'

test-hpet-guest: test-hpet-guest-negctl $(ISO)
	@python3 tests/boot/run-hpet-test.py --iso $(ISO) --enabled yes

test-hpet-guest-negctl: $(ISO)
	@python3 tests/boot/run-hpet-test.py --iso $(ISO) --enabled no
