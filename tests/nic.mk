# NIC driver test targets.
#
# Kept in their own fragment rather than in the Makefile because several agents
# were editing the Makefile at the same time and a shared-file edit could not be
# committed without swallowing theirs. The Makefile pulls it in with a single
# line:
#
#     -include tests/nic.mk
#
# Nothing else is needed to BUILD the drivers -- C_SRC globs c/drivers, so
# netdev.c / rtl8139.c / rtl8169.c / virtio_net.c link with no Makefile change
# (verified: build/c/drivers/net/*.o).

.PHONY: test-nic test-nic-drv test-nic-e1000 test-nic-virtio test-nic-rtl8139 test-nic-none

# Host unit test: descriptor-ring arithmetic, device header/status accessors,
# and PCI match-table resolution -- read from the REAL tables (net_ids.inc), not
# a copy. See the header comment in tests/unit/net_drv_test.c for why the
# register programming is deliberately not mocked.
test-nic-drv:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -DLOGIT_HOST_TEST \
		-o $(BUILD)/net_drv_test tests/unit/net_drv_test.c c/drivers/core/device.c \
		-Ic/drivers/net -Ic/drivers/core -Ic/kernel/pci -Itests/unit/pcistub
	@./$(BUILD)/net_drv_test

# On-device, one target per card: enumerate -> DHCP lease -> complete 32 KiB
# HTTP fetch. Link-up is not the claim; bytes arriving is.
test-nic-e1000: $(ISO) $(DISK)
	@bash tests/boot/run-nic-test.sh $(ISO) $(DISK) e1000 e1000

test-nic-virtio: $(ISO) $(DISK)
	@bash tests/boot/run-nic-test.sh $(ISO) $(DISK) virtio-net-pci virtio-net

test-nic-rtl8139: $(ISO) $(DISK)
	@bash tests/boot/run-nic-test.sh $(ISO) $(DISK) rtl8139 rtl8139

# A machine whose NIC we cannot drive must boot cleanly and lose only the
# network. Covers both "no device at all" and "a real NIC we deliberately do
# not claim" (ne2k_pci).
test-nic-none: $(ISO) $(DISK)
	@bash tests/boot/run-nic-none-test.sh $(ISO) $(DISK)

# There is no test-nic-rtl8169: QEMU has no rtl8169 device model (`-device
# help` lists rtl8139 and nothing else Realtek), so that driver cannot be
# booted here. Its ring arithmetic is covered by test-nic-drv; the register
# programming is unverified until someone runs it on metal.
test-nic: test-nic-drv test-nic-e1000 test-nic-virtio test-nic-rtl8139 test-nic-none
