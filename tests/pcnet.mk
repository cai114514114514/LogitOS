# PCnet descriptor checks plus real guest DMA/ring-wrap transfer. Register I/O
# is measured in QEMU, never claimed from a register mock. BUILD must be a
# command-line assignment so concurrent driver work does not share outputs.
.PHONY: test-pcnet-ring test-pcnet-negctl test-pcnet-guest test-pcnet
test-pcnet-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -Wall -Wextra -DPCNET_NEGCTL_IGNORE_ERROR -Ic/drivers/net tests/unit/pcnet_test.c -o $(BUILD)/pcnet-negctl
	@if $(BUILD)/pcnet-negctl >$(BUILD)/pcnet-negctl.log 2>&1; then cat $(BUILD)/pcnet-negctl.log; exit 1; fi
	@grep -q '^FAIL: reject device error' $(BUILD)/pcnet-negctl.log
	@grep -q '^PCNET_RING: 16 checks, 1 failures' $(BUILD)/pcnet-negctl.log
	@echo 'PCNET_NEGCTL: rejected delivery of an errored RX descriptor (exactly one failure)'
test-pcnet-ring: test-pcnet-negctl
	@python3 tests/boot/run-pcnet-test.py --self-test
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Ic/drivers/net tests/unit/pcnet_test.c -o $(BUILD)/pcnet-ring
	@$(BUILD)/pcnet-ring
test-pcnet-guest: test-pcnet-ring $(ISO) $(DISK)
	@python3 tests/boot/run-pcnet-test.py --iso $(ISO) --disk $(DISK) --output $(BUILD)/pcnet-guest
test-pcnet: test-pcnet-guest
