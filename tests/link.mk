# Link-layer test targets: Ethernet framing and the IPv4 neighbour cache.
#
# Kept in its own fragment for the reason tests/nic.mk and tests/netperf.mk
# record: several agents edit the Makefile at once and a shared-file edit
# cannot be committed without swallowing theirs. The Makefile pulls it in with
# one line:
#
#     -include tests/link.mk
#
# Nothing here is needed to BUILD the link layer -- C_SRC globs c/net, so
# eth.c and arp.c link with no Makefile change.
#
# Both host tests are white-box: they #include the implementation and stub only
# what is genuinely below them (netdev_tx, the clock, the console), so what
# they check is the real code path and not a re-implementation of it. Both are
# built with ASan+UBSan, because both parse untrusted frames in ring 0 and the
# interesting failures there are memory-safety ones that a functional check
# would pass straight through.
#
# -Wno-address: eth.c tests the weak upper-layer handler symbols for NULL
# (`if (ip_input)`), which is correct in the kernel -- ip.c may not be linked --
# but is a tautology in a test that defines them.

.PHONY: test-link test-link-host test-arp-host test-arp-negctl test-eth-host \
        test-eth-negctl test-link-os test-link-nics \
        test-ip-arp-host test-ip-arp-negctl

LINK_INC := -Ic/net/core -Ic/net/link -Ic/drivers/net -Ic/drivers/core \
            -Ic/drivers/timer -Ic/kernel/core -Ic/kernel/pci

# The neighbour cache: the five-state ladder against a controllable clock, the
# RFC 826 merge rule, RFC 5227 conflict detection, the pending-packet queue,
# every malformed frame shape, and 300k fuzzed frames.
test-arp-host:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -DLOGIT_NET_HOST \
		-o $(BUILD)/arp_test tests/unit/arp_test.c $(LINK_INC)
	@./$(BUILD)/arp_test

# Negative control for the property the whole rewrite exists for: an
# unsolicited frame may not replace a binding we already hold, and a frame that
# is neither addressed to us nor about a host we know may not create one.
# ARP_NEGCTL_TRUST_ANY restores exactly what the old cache_put() did -- believe
# everybody -- and the suite MUST fail: the gateway's MAC gets replaced by one
# broadcast frame, and a flood of strangers evicts every real binding.
test-arp-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w -DLOGIT_NET_HOST -DARP_NEGCTL_TRUST_ANY \
		-o $(BUILD)/arp_negctl tests/unit/arp_test.c $(LINK_INC)
	@if ./$(BUILD)/arp_negctl >$(BUILD)/arp_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes while believing any ARP sender"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/arp_negctl.log) checks fail without the trust rule"; \
	fi

# Framing: header construction, the 60-byte minimum, destination filtering,
# 802.1Q/802.1ad de-tagging (every truncation of every layout), loopback and
# its recursion bound, and 400k fuzzed frames.
test-eth-host:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-address \
		-DLOGIT_NET_HOST -o $(BUILD)/eth_test tests/unit/eth_test.c $(LINK_INC)
	@./$(BUILD)/eth_test

# Negative control: ETH_NEGCTL_LEGACY restores the three things the 47-line
# version did -- no pad, no destination filter, no VLAN. The suite MUST fail,
# and the VLAN failures are the interesting ones: on a trunk port that build
# receives nothing at all.
test-eth-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w -DLOGIT_NET_HOST -DETH_NEGCTL_LEGACY \
		-o $(BUILD)/eth_negctl tests/unit/eth_test.c $(LINK_INC)
	@if ./$(BUILD)/eth_negctl >$(BUILD)/eth_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes without framing, filtering or VLAN"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/eth_negctl.log) checks fail without them"; \
	fi

# On the machine: boot, and require the link layer to report itself having
# actually worked -- an ARP binding for the gateway, frames dispatched by
# ethertype, and the announcement sent. Counters, not "it booted".
test-link-os: $(ISO) $(DISK)
	@bash tests/boot/run-link-test.sh $(ISO) $(DISK) $(if $(NIC),$(NIC),e1000)

# The padding property is the one that is per-driver: two of the four cards pad
# short frames themselves and two do not, so a build verified on one of them
# says nothing about the others. Run every card QEMU can emulate. (There is no
# rtl8169 arm for the reason tests/nic.mk records: QEMU has no device model.)
test-link-nics: $(ISO) $(DISK)
	@bash tests/boot/run-link-test.sh $(ISO) $(DISK) e1000
	@bash tests/boot/run-link-test.sh $(ISO) $(DISK) virtio-net-pci
	@bash tests/boot/run-link-test.sh $(ISO) $(DISK) rtl8139

# The CALL SITE. arp_output has been built, tested and documented since the
# link-layer rewrite and ip.c did not call it -- it dropped the datagram on a
# miss, so the first packet to every cold next hop was destroyed on every boot.
# This links ip.c on top of the real arp.c and asserts the held datagram comes
# out byte for byte when the reply lands. It belongs in this fragment and not
# with the IP tests because what it is testing is the neighbour queue.
test-ip-arp-host:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -Wall -Wextra -DLOGIT_NET_HOST \
		-o $(BUILD)/ip_arp_test tests/unit/ip_arp_test.c c/net/core/route.c \
		$(LINK_INC) -Ic/net/ip
	@./$(BUILD)/ip_arp_test

# Negative control: IP_NEGCTL_ARP_DROP restores the old `if (arp_resolve(...)
# != 0) return -1;`, and the suite MUST fail. Five checks go red, and the one
# to read is "the held datagram is sent on the ARP reply" -- under the old code
# there is nothing left to send.
test-ip-arp-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w -DLOGIT_NET_HOST -DIP_NEGCTL_ARP_DROP \
		-o $(BUILD)/ip_arp_negctl tests/unit/ip_arp_test.c c/net/core/route.c \
		$(LINK_INC) -Ic/net/ip
	@if ./$(BUILD)/ip_arp_negctl >$(BUILD)/ip_arp_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes while ip.c drops on an ARP miss"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/ip_arp_negctl.log) checks fail without the queue"; \
	fi

test-link-host: test-eth-host test-eth-negctl test-arp-host test-arp-negctl \
                test-ip-arp-host test-ip-arp-negctl

test-link: test-link-host test-link-nics
