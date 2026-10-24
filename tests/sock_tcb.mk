# Unlike the drain/fallback models, this fixture compiles actual sock.c and
# tcp.c in the same process and injects checksum-valid peer packets. Header
# dependencies come from the compiler because both production units are
# included for read-only slot observations, not copied into another model.
SOCK_TCB_DIR := $(BUILD)/sock-tcb
SOCK_TCB_SRC := tests/unit/sock_tcb_test.c
SOCK_TCB_DEPS := $(SOCK_TCB_SRC) c/net/core/sock.c c/net/transport/tcp.c \
                 c/net/transport/tcp.h c/net/ip/ip6_addr.c tests/sock_tcb.mk
SOCK_TCB_CF := -O1 -g -Wall -Wextra -DLOGIT_NET_HOST -MMD -MP \
               -Ic/net/core -Itests/unit/tcpstub -Ic/net -Ic/net/ip \
               -Ic/net/link -Ic/net/transport -Ic/net/dns -Ic/net/tls \
               -Ic/net/http -Ic/drivers/timer -Ic/drivers/char -Ic/kernel/diag \
               -Iinclude/abi -Ic
SOCK_TCB_CASES := rst4 rst6 icmp4 fin4 fin6
SOCK_TCB_CLOCK_CASES := clock4 clock6
SOCK_TCB_ALL_CASES := $(SOCK_TCB_CASES) $(SOCK_TCB_CLOCK_CASES)

$(SOCK_TCB_DIR)/current: $(SOCK_TCB_DEPS)
	@mkdir -p $(SOCK_TCB_DIR)
	$(CC) $(SOCK_TCB_CF) -o $@ $(SOCK_TCB_SRC)
$(SOCK_TCB_DIR)/unowned: $(SOCK_TCB_DEPS)
	@mkdir -p $(SOCK_TCB_DIR)
	$(CC) $(SOCK_TCB_CF) -DSOCK_UNPIN_TCP -o $@ $(SOCK_TCB_SRC)
$(SOCK_TCB_DIR)/icmp: $(SOCK_TCB_DEPS)
	@mkdir -p $(SOCK_TCB_DIR)
	$(CC) $(SOCK_TCB_CF) -DTCP_ICMP_UNPIN -o $@ $(SOCK_TCB_SRC)
$(SOCK_TCB_DIR)/stale-clock: $(SOCK_TCB_DEPS)
	@mkdir -p $(SOCK_TCB_DIR)
	$(CC) $(SOCK_TCB_CF) -DSOCK_STALE_PHASE_CLOCK -o $@ $(SOCK_TCB_SRC)

.PHONY: test-sock-tcb test-sock-tcb-negctl test-sock-tcb-icmp-negctl test-sock-tcb-clock-negctl test-sock-tcb-san
test-sock-tcb-negctl: $(SOCK_TCB_DIR)/unowned
	@rc=0; for name in $(SOCK_TCB_CASES); do \
	  $(SOCK_TCB_DIR)/unowned $$name || rc=$$?; \
	  if [ $$rc -gt 1 ]; then break; fi; \
	done > $(SOCK_TCB_DIR)/unowned.log 2>&1; \
	count=$$(grep -c '^FAIL:' $(SOCK_TCB_DIR)/unowned.log); \
	if [ $$rc -eq 1 ] && [ $$count -eq 15 ] && \
	grep -qx 'sock-tcb: RST IPv4: 8 checks, 5 failures' $(SOCK_TCB_DIR)/unowned.log && \
	grep -qx 'sock-tcb: RST IPv6: 8 checks, 5 failures' $(SOCK_TCB_DIR)/unowned.log && \
	grep -qx 'sock-tcb: ICMP IPv4: 8 checks, 5 failures' $(SOCK_TCB_DIR)/unowned.log && \
	grep -qx 'sock-tcb: FIN IPv4: 13 checks, 0 failures' $(SOCK_TCB_DIR)/unowned.log && \
	grep -qx 'sock-tcb: FIN IPv6: 13 checks, 0 failures' $(SOCK_TCB_DIR)/unowned.log; then \
	  cat $(SOCK_TCB_DIR)/unowned.log; \
	  echo 'sock-tcb negative control: old socket deletes recycled TCP dial (15 expected failures)'; \
	else cat $(SOCK_TCB_DIR)/unowned.log; exit 1; fi

test-sock-tcb-icmp-negctl: $(SOCK_TCB_DIR)/icmp
	@rc=0; for name in $(SOCK_TCB_CASES); do \
	  $(SOCK_TCB_DIR)/icmp $$name || rc=$$?; \
	  if [ $$rc -gt 1 ]; then break; fi; \
	done > $(SOCK_TCB_DIR)/icmp.log 2>&1; \
	count=$$(grep -c '^FAIL:' $(SOCK_TCB_DIR)/icmp.log); \
	if [ $$rc -eq 1 ] && [ $$count -eq 5 ] && \
	grep -qx 'sock-tcb: RST IPv4: 8 checks, 0 failures' $(SOCK_TCB_DIR)/icmp.log && \
	grep -qx 'sock-tcb: RST IPv6: 8 checks, 0 failures' $(SOCK_TCB_DIR)/icmp.log && \
	grep -qx 'sock-tcb: ICMP IPv4: 8 checks, 5 failures' $(SOCK_TCB_DIR)/icmp.log && \
	grep -qx 'sock-tcb: FIN IPv4: 13 checks, 0 failures' $(SOCK_TCB_DIR)/icmp.log && \
	grep -qx 'sock-tcb: FIN IPv6: 13 checks, 0 failures' $(SOCK_TCB_DIR)/icmp.log; then \
	  cat $(SOCK_TCB_DIR)/icmp.log; \
	  echo 'sock-tcb ICMP negative control: hard error loses an owned slot (5 expected failures)'; \
	else cat $(SOCK_TCB_DIR)/icmp.log; exit 1; fi

# Keep the real timer boundary separate from the two ownership controls:
# advancing apparatus time must not widen either one's existing fail set.
test-sock-tcb-clock-negctl: $(SOCK_TCB_DIR)/stale-clock
	@rc=0; for name in $(SOCK_TCB_CLOCK_CASES); do \
	  $(SOCK_TCB_DIR)/stale-clock $$name || rc=$$?; \
	  if [ $$rc -gt 1 ]; then break; fi; \
	done > $(SOCK_TCB_DIR)/stale-clock.log 2>&1; \
	count=$$(grep -c '^FAIL:' $(SOCK_TCB_DIR)/stale-clock.log); \
	if [ $$rc -eq 1 ] && [ $$count -eq 8 ] && \
	grep -qx 'sock-tcb: Clock IPv4: 9 checks, 4 failures' $(SOCK_TCB_DIR)/stale-clock.log && \
	grep -qx 'sock-tcb: Clock IPv6: 9 checks, 4 failures' $(SOCK_TCB_DIR)/stale-clock.log && \
	grep -qx 'Clock IPv4 after tick: poll=784 error=-3 sock_state=6 tcp_used=0 tcp_state=1' $(SOCK_TCB_DIR)/stale-clock.log && \
	grep -qx 'Clock IPv6 after tick: poll=784 error=-3 sock_state=6 tcp_used=0 tcp_state=1' $(SOCK_TCB_DIR)/stale-clock.log && \
	grep -Eq '^Clock IPv4 new SYN-ACK response: flags=0x04 ' $(SOCK_TCB_DIR)/stale-clock.log && \
	grep -Eq '^Clock IPv6 new SYN-ACK response: flags=0x04 ' $(SOCK_TCB_DIR)/stale-clock.log; then \
	  for family in IPv4 IPv6; do \
	    for label in 'one tick cannot expire the new socket connection' \
	      'clock boundary keeps the new TCP SYN pending' \
	      'valid new SYN-ACK receives ACK instead of guest RST' \
	      'new socket connects through the actual consumer'; do \
	      grep -qx "FAIL: Clock $$family: $$label" $(SOCK_TCB_DIR)/stale-clock.log || exit 1; \
	    done; \
	  done; \
	  cat $(SOCK_TCB_DIR)/stale-clock.log; \
	  echo 'sock-tcb clock negative control: one tick releases the new SYN and receives RST (8 expected failures)'; \
	else cat $(SOCK_TCB_DIR)/stale-clock.log; exit 1; fi

# Both release doors and the phase clock must fail before accepting the positive.
test-sock-tcb: test-sock-tcb-negctl test-sock-tcb-icmp-negctl test-sock-tcb-clock-negctl $(SOCK_TCB_DIR)/current
	@rc=0; for name in $(SOCK_TCB_ALL_CASES); do \
	  $(SOCK_TCB_DIR)/current $$name || { rc=$$?; break; }; \
	done > $(SOCK_TCB_DIR)/current.log 2>&1; cat $(SOCK_TCB_DIR)/current.log; exit $$rc
$(SOCK_TCB_DIR)/san: $(SOCK_TCB_DEPS)
	@mkdir -p $(SOCK_TCB_DIR)
	$(CC) $(SOCK_TCB_CF) -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -fno-omit-frame-pointer -o $@ $(SOCK_TCB_SRC)
test-sock-tcb-san: test-sock-tcb $(SOCK_TCB_DIR)/san
	@rc=0; for name in $(SOCK_TCB_ALL_CASES); do \
	  $(SOCK_TCB_DIR)/san $$name || { rc=$$?; break; }; \
	done > $(SOCK_TCB_DIR)/san.log 2>&1; cat $(SOCK_TCB_DIR)/san.log; exit $$rc
ci-host: test-sock-tcb-san

-include $(SOCK_TCB_DIR)/current.d $(SOCK_TCB_DIR)/unowned.d \
         $(SOCK_TCB_DIR)/icmp.d $(SOCK_TCB_DIR)/stale-clock.d $(SOCK_TCB_DIR)/san.d
