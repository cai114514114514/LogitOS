# The IPv4 routing table and the interface table.
#
# Kept in its own fragment for the reason tests/link.mk, tests/nic.mk and
# tests/netperf.mk all record: several agents edit the Makefile at once and a
# shared-file edit cannot be committed without swallowing theirs. The Makefile
# pulls this in with ONE line, next to the others around line 3821:
#
#     -include tests/route.mk
#
# It is `route.mk` and not `net.mk` because tests/net.mk was taken -- by the
# SOCK_RAW line, while this change was in flight, which is the same hazard the
# paragraph above describes arriving one directory down. Check before naming a
# fragment.
#
# Nothing here is needed to BUILD the routing table: C_SRC globs c/net, so
# c/net/core/route.c links into the kernel with no Makefile change (verified:
# build/c/net/core/route.o is in the ld.lld line of a plain `make`).
#
# Both host tests are white-box -- they #include the implementation -- and both
# are built with ASan+UBSan. route.c is pure integer arithmetic with a
# left-shift on every lookup (route_plen_mask), and a shift count of 32 is
# undefined behaviour that produces a plausible mask on x86 and a different
# plausible mask elsewhere; UBSan is what makes that a failure instead of a
# portability surprise.
#
# WHY THREE TESTS AND NOT ONE. test-route proves the table answers correctly.
# test-ip-route proves ip_send() ASKS it. test-netif proves the interfaces the
# table's `oif` refers to actually exist and are more than one. Those are
# different claims and this tree has been caught conflating them before -- the
# WPT runner linked layout.c and never called layout_page(), and read the same
# 531/11152 with and without the grid implementation. A routing table nothing
# routes through would pass test-route perfectly.
#
# Each control is a PREREQUISITE of its positive rather than a target named
# only in a suite list. Naming it beside the positive on a ci-host: line
# satisfies the audit's UNWIRED check and still runs it never, which is the
# worse of the two failures because it looks fixed -- see the STRANDED
# CONTROLS category in CLAUDE.md.

.PHONY: test-route test-route-negctl test-ip-route test-ip-route-negctl \
        test-netif test-netif-negctl test-route-all

ROUTE_INC := -Ic/net/core -Ic/net/ip -Ic/net/link -Ic/drivers/net \
             -Ic/drivers/core -Ic/drivers/timer -Ic/kernel/core -Ic/kernel/pci

# The table itself: longest-prefix ranking, the metric tie-break, on-link
# resolution, loopback as an ordinary row, prefix arithmetic at both ends of
# the range, insertion refusals, a full table, and the address->routes bridge
# including a lease change. 64 checks. It stubs NOTHING -- there is nothing
# below route.c to stub, which is the property the file was written for.
test-route: test-route-negctl
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra \
		-o $(BUILD)/route_test tests/unit/route_test.c -Ic/net/core
	@./$(BUILD)/route_test

# Negative control: ROUTE_FIRST_MATCH removes the RANKING from route_lookup and
# nothing else. Every route still matches, every field is still carried out --
# the answer is a well-formed interface, next hop and source address that
# happens to be the wrong one, which is the only kind of control worth having
# for a table (an empty or crashing one would be caught by anything).
#
# MEASURED, not predicted: it reddens 9 of the 64 checks, and they are exactly
# the destinations that match more than one row. Six are the cases the table
# marks BY_PREFIX; two are marked BY_METRIC and redden for the same reason --
# with a default route installed first it shadows them before the metric rule
# is ever reached -- and the ninth is the RT_F_LOCAL flag check, which reads a
# field off whichever row won. That the metric RULE is nevertheless intact is
# shown separately by metric_order(), whose two checks stay green under this
# build because that phase has no default route to shadow them.
test-route-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -w -DROUTE_FIRST_MATCH \
		-o $(BUILD)/route_negctl tests/unit/route_test.c -Ic/net/core
	@if ./$(BUILD)/route_negctl >$(BUILD)/route_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes without longest-prefix ranking"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/route_negctl.log) checks fail without the ranking (expect 9)"; \
	fi

# THE CALL SITE: ip_send over the real route.c, with arp_output and eth_send
# captured. The headline check is 127.0.0.1, which under the old ternary was
# ARP'd for the default gateway and put on the wire.
test-ip-route: test-ip-route-negctl
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra \
		-DLOGIT_NET_HOST -o $(BUILD)/ip_route_test tests/unit/ip_route_test.c \
		$(ROUTE_INC)
	@./$(BUILD)/ip_route_test

# Negative control: IP_NEGCTL_TERNARY restores the routing decision this change
# replaced -- verbatim, not a broken version of it. It is the code that ran on
# this machine until today, so everything it does still looks right: an
# interface, a next hop, a source address, and every internet destination still
# reachable. MEASURED: 11 of 25 checks redden, and 14 keep passing, which is
# what says the suite is measuring the routing decision rather than measuring
# that ip_send compiles.
test-ip-route-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -w -DLOGIT_NET_HOST -DIP_NEGCTL_TERNARY \
		-o $(BUILD)/ip_route_negctl tests/unit/ip_route_test.c $(ROUTE_INC)
	@if ./$(BUILD)/ip_route_negctl >$(BUILD)/ip_route_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes with the old on-subnet ternary"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/ip_route_negctl.log) checks fail with the old ternary (expect 11)"; \
	fi

# THE INTERFACE TABLE, which is the other half of the same limitation:
# netdev.c's single `g_nic` pointer. Drives the REAL netdev_init() over a
# synthetic four-card PCI registry, so what is checked is the binding this
# kernel would perform -- including that the cards are taken in NIC-line
# priority order and not in enumeration order, which is why the synthetic
# registry lists them in the wrong one.
#
# It stubs seven symbols (dev_count/dev_at/dev_match_table and the four
# probes) and reads the match tables from the real net_ids.inc, the same way
# tests/unit/net_drv_test.c does.
test-netif: test-netif-negctl
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra \
		-o $(BUILD)/netif_test tests/unit/netif_test.c \
		-Ic/drivers/net -Ic/drivers/core -Ic/net/core -Ic/kernel/pci \
		-Itests/unit/pcistub
	@./$(BUILD)/netif_test

# Negative control: NETIF_NEGCTL_SINGLE puts back the `return 0;` that stopped
# netdev_init at the first bound card. Note what it does NOT break -- 36 of the
# 43 checks stay green, including every single-NIC property, because a machine
# with one card behaves identically. That is why the limitation survived
# unnoticed: it is invisible until the second card is in the slot.
# MEASURED: 7 of 43 redden, and they are exactly the multi-card ones.
test-netif-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -w -DNETIF_NEGCTL_SINGLE \
		-o $(BUILD)/netif_negctl tests/unit/netif_test.c \
		-Ic/drivers/net -Ic/drivers/core -Ic/net/core -Ic/kernel/pci \
		-Itests/unit/pcistub
	@if ./$(BUILD)/netif_negctl >$(BUILD)/netif_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes when only the first NIC binds"; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/netif_negctl.log) checks fail with one-NIC binding (expect 7)"; \
	fi

test-route-all: test-netif test-route test-ip-route
