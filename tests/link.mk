# Link-layer test targets: Ethernet framing.
#
# Kept in its own fragment for the reason tests/nic.mk and tests/netperf.mk
# record: several agents edit the Makefile at once and a shared-file edit
# cannot be committed without swallowing theirs. The Makefile pulls it in with
# one line:
#
#     -include tests/link.mk
#
# Nothing here is needed to BUILD the link layer -- C_SRC globs c/net, so
# eth.c links with no Makefile change.
#
# The host test is white-box: it #includes eth.c and stubs only what is
# genuinely below it (netdev_tx, the console), so what it checks is the real
# code path and not a re-implementation of it. It is built with ASan+UBSan,
# because eth_input parses untrusted frames in ring 0 and the interesting
# failures there are memory-safety ones that a functional check would pass
# straight through.
#
# -Wno-address: eth.c tests the weak upper-layer handler symbols for NULL
# (`if (ip_input)`), which is correct in the kernel -- ip.c may not be linked --
# but is a tautology in a test that defines them.

.PHONY: test-link test-link-host test-eth-host test-eth-negctl

LINK_INC := -Ic/net/core -Ic/net/link -Ic/drivers/net -Ic/drivers/core \
            -Ic/drivers/timer -Ic/kernel/core -Ic/kernel/pci

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

test-link-host: test-eth-host test-eth-negctl

test-link: test-link-host
