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
.PHONY: test-e1000-stats test-e1000-stats-negctl test-e1000-link
.PHONY: test-e1000-linkmask-negctl

# e1000 interrupt moderation, as a build knob rather than an edit, so that the
# A/B for it is two builds of the same tree:
#
#     make test-net-bench NIC=e1000                  # ITR off (the default)
#     make clean && make test-net-bench E1000_ITR=61 # ~250 us floor under QEMU
#
# The unit is QEMU's, not the datasheet's -- 4096 ns per count against the
# silicon's 256 -- which is why this is a raw number and not microseconds. See
# the write site in c/drivers/net/e1000.c and e1000_stats.h.
ifneq ($(E1000_ITR),)
CFLAGS += -DE1000_ITR_VALUE=$(E1000_ITR)
endif

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

# The e1000 statistics accumulator and link-status decode, host-side, against a
# MODEL of a read-to-clear register file -- two models, because the 8254x manual
# and QEMU's device model clear a 64-bit counter pair on opposite halves. See
# the header of tests/unit/e1000_stats_test.c for why this half of a NIC driver
# is the half that must be modelled, where the register programming must not be.
#
# test-e1000-stats depends on its own control, which is the wiring the audit's
# STRANDED CONTROLS category exists to demand (CLAUDE.md: naming a control on a
# ci-host: line instead satisfies UNWIRED and still runs it never).
test-e1000-stats: test-e1000-stats-negctl test-e1000-linkmask-negctl
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra \
		-o $(BUILD)/e1000_stats_test tests/unit/e1000_stats_test.c -Ic/drivers/net
	@./$(BUILD)/e1000_stats_test

# NEGATIVE CONTROL. -DE1000_STATS_NO_ACC compiles `sw = read()` in place of
# `sw += read()` -- the naive read of a read-to-clear register, which is the
# plausible wrong implementation rather than the absent one: every counter still
# exists, still moves, and still looks like a total. REQUIRED TO FAIL, and the
# count is pinned: 10 of the 61 checks redden and no others. If the number
# changes, either a check moved or the control stopped controlling.
test-e1000-stats-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -DE1000_STATS_NO_ACC \
		-o $(BUILD)/e1000_stats_negctl tests/unit/e1000_stats_test.c -Ic/drivers/net
	@out=$$(./$(BUILD)/e1000_stats_negctl 2>&1); rc=$$?; \
	 n=$$(printf '%s\n' "$$out" | grep -c '^FAIL '); \
	 if [ $$rc -eq 0 ]; then \
	   echo "NEGCTL FAIL: -DE1000_STATS_NO_ACC still passes -- the gate proves nothing"; \
	   exit 1; \
	 elif [ "$$n" != "10" ]; then \
	   echo "NEGCTL FAIL: expected exactly 10 reddened checks, got $$n"; \
	   printf '%s\n' "$$out"; exit 1; \
	 else \
	   echo "e1000_stats negctl OK: -DE1000_STATS_NO_ACC reddens exactly $$n checks"; \
	 fi

# NEGATIVE CONTROL for the link-transition mask, and it lives HERE rather than
# on device for a measured reason: the wrong implementation was built and run
# through tests/boot/run-e1000-link-test.sh on 2026-08-20 and the gate PASSED,
# because nothing but LU moves in QEMU's STATUS during a boot. The emulator
# cannot exercise the property, so the control that can be watched failing is
# this one. -DE1000_LINK_NEGCTL compiles `a != b` -- the obvious
# whole-register compare, which still reports every real transition correctly
# and additionally reports things that are not link events. REQUIRED TO FAIL,
# and pinned at exactly 3: the three checks that feed STATUS words differing
# only in TXOFF, an unreported high bit, and bit 31. If more than 3 redden the
# mask has widened; if fewer, the control has stopped controlling.
test-e1000-linkmask-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -DE1000_LINK_NEGCTL \
		-o $(BUILD)/e1000_link_negctl tests/unit/e1000_stats_test.c -Ic/drivers/net
	@out=$$(./$(BUILD)/e1000_link_negctl 2>&1); rc=$$?; \
	 n=$$(printf '%s\n' "$$out" | grep -c '^FAIL '); \
	 if [ $$rc -eq 0 ]; then \
	   echo "NEGCTL FAIL: -DE1000_LINK_NEGCTL still passes -- the mask proves nothing"; \
	   exit 1; \
	 elif [ "$$n" != "3" ]; then \
	   echo "NEGCTL FAIL: expected exactly 3 reddened checks, got $$n"; \
	   printf '%s\n' "$$out"; exit 1; \
	 else \
	   echo "e1000 linkmask negctl OK: -DE1000_LINK_NEGCTL reddens exactly $$n checks"; \
	 fi

# On device: unplug the cable and plug it back in, from the host, while the
# guest is running (QMP set_link). Asserts the driver prints the transition and
# prints it ONCE -- a report per poll is what a whole-STATUS comparison gives,
# and it is indistinguishable from a correct one in a log that only greps for
# the word "link".
test-e1000-link: $(ISO) $(DISK)
	@bash tests/boot/run-e1000-link-test.sh $(ISO) $(DISK)

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

# Wired into the host suite. The two negative controls are prerequisites of
# test-e1000-stats rather than named here, which is the distinction CLAUDE.md
# draws: naming a control on a ci-host: line satisfies the UNWIRED audit and
# still runs it never.
ci-host: test-e1000-stats
