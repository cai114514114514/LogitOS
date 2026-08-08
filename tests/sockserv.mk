# Server sockets: the passive open, the socket syscalls, and /bin/httpd.
#
# Kept in its own fragment for the reason tests/nic.mk gives -- several agents
# edit the Makefile at once and a shared-file edit cannot be committed without
# swallowing theirs. The Makefile pulls it in with one line:
#
#     -include tests/sockserv.mk
#
# Nothing else is needed to BUILD any of it: C_SRC globs c/net, so
# c/net/core/lsock.c links with no Makefile change, and /bin/httpd is one name
# added to the CLI list.

.PHONY: test-tcp-server test-tcp-server-negctl test-httpd test-sockserv

# The passive-open state machine, host-side and white-box (tcp_test.c #includes
# tcp.c). Covers LISTEN, the three-way handshake from the receiving end, the
# SYN-ACK's option negotiation, the accept backlog and its limit, the client
# reserve, CLOSE_WAIT -> LAST_ACK and shutdown(SHUT_WR) -- alongside the 178
# client-path checks that were already here, which is the point of putting them
# in the same binary: the passive path must not have cost the active one
# anything.
test-tcp-server:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/tcp_test tests/unit/tcp_test.c \
		-Itests/unit/tcpstub -Ic/net/transport
	@./$(BUILD)/tcp_test

# THE NEGATIVE CONTROL, and it is not "remove listen" -- that fails everything
# and proves nothing. This builds a stack that ACCEPTS the connection but
# reuses one connection block instead of allocating a new one. A single client
# works perfectly against it; the second simultaneous connection corrupts the
# first. The suite MUST fail, and the checks that fail must be the two-client
# ones, which is what the grep asserts -- a control that failed for some
# unrelated reason would otherwise look like a passing control.
test-tcp-server-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DTCP_ACCEPT_NEGCTL -o $(BUILD)/tcp_test_srv_negctl \
		tests/unit/tcp_test.c -Itests/unit/tcpstub -Ic/net/transport
	@if ./$(BUILD)/tcp_test_srv_negctl >$(BUILD)/tcp_srv_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes with one shared connection block"; \
		exit 1; \
	elif ! grep -q "two conns:" $(BUILD)/tcp_srv_negctl.log; then \
		echo "NEGATIVE CONTROL INCONCLUSIVE: it failed, but not on the two-client checks"; \
		sed -n '1,20p' $(BUILD)/tcp_srv_negctl.log; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/tcp_srv_negctl.log) checks fail when accepted connections share a block"; \
		grep '^FAIL: two conns' $(BUILD)/tcp_srv_negctl.log; \
	fi

# INBOUND, FROM THE HOST. Boots LogitOS, starts /bin/httpd, and fetches files
# from the HOST side of QEMU's SLIRP through a hostfwd rule -- so the bytes
# cross a real NIC, a real IP stack and a real TCP handshake in the inbound
# direction. Loopback inside the guest would prove none of that.
test-httpd: $(ISO) $(DISK)
	@bash tests/boot/run-httpd-test.sh $(ISO) $(DISK)

test-sockserv: test-tcp-server test-tcp-server-negctl test-httpd
