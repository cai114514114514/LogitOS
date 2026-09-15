# Real kernel sock.c with a controllable TCP sender. The negative build changes
# only error cleanup after application close; every open queues actual bytes.
SOCK_DRAIN_DIR = $(BUILD)/site-general/continue
SOCK_DRAIN_INC = -Itests/unit -Ic/net/core -Ic/net/ip -Ic/net/link -Ic/net/transport -Ic/net/dns -Ic/net/tls -Ic/net/http -Ic/drivers/timer -Ic/drivers/char -Ic/kernel/core -Iinclude/abi
.PHONY: test-sock-closed-drain test-sock-closed-drain-negctl

test-sock-closed-drain-negctl:
	@mkdir -p $(SOCK_DRAIN_DIR)
	@$(CC) -O1 -g -Wall -Wextra -DLOGIT_NET_HOST -DSOCK_CLOSED_DRAIN_LEAK $(SOCK_DRAIN_INC) tests/unit/sock_closed_drain_test.c -o $(SOCK_DRAIN_DIR)/sock-drain-old
	@rc=0; $(SOCK_DRAIN_DIR)/sock-drain-old > $(SOCK_DRAIN_DIR)/sock-drain-old.log 2>&1 || rc=$$?; if [ $$rc -ne 1 ] || ! grep -q 'completed=16 live=16 dead_shut=16' $(SOCK_DRAIN_DIR)/sock-drain-old.log || ! grep -q '17th transfer must start' $(SOCK_DRAIN_DIR)/sock-drain-old.log; then cat $(SOCK_DRAIN_DIR)/sock-drain-old.log; exit 1; fi; tail -4 $(SOCK_DRAIN_DIR)/sock-drain-old.log

test-sock-closed-drain: test-sock-closed-drain-negctl
	@$(CC) -O1 -g -Wall -Wextra -DLOGIT_NET_HOST $(SOCK_DRAIN_INC) tests/unit/sock_closed_drain_test.c -o $(SOCK_DRAIN_DIR)/sock-drain
	@$(SOCK_DRAIN_DIR)/sock-drain > $(SOCK_DRAIN_DIR)/sock-drain.log
	@cat $(SOCK_DRAIN_DIR)/sock-drain.log

# Source inventory follows h2mux's production-derived browser transport list.
BFETCH_OWNER_SRC = tests/unit/bfetch_owner_loop_test.c $(filter-out tests/unit/h2mux_test.c,$(H2MUX_SRC))
.PHONY: test-bfetch-owner-loop test-bfetch-owner-loop-negctl
test-bfetch-owner-loop-negctl: h2mux-link-check
	@mkdir -p $(SOCK_DRAIN_DIR)
	@$(CC) -O1 -g -w -DBFETCH_OWNER_DROP_CLOSE $(H2MUX_INC) $(BFETCH_OWNER_SRC) -o $(SOCK_DRAIN_DIR)/bfetch-owner-old
	@rc=0; $(SOCK_DRAIN_DIR)/bfetch-owner-old > $(SOCK_DRAIN_DIR)/bfetch-owner-old.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q 'owner-loop production close left socket owned' $(SOCK_DRAIN_DIR)/bfetch-owner-old.log
	@tail -4 $(SOCK_DRAIN_DIR)/bfetch-owner-old.log
test-bfetch-owner-loop: test-bfetch-owner-loop-negctl
	@$(CC) -O1 -g -w $(H2MUX_INC) $(BFETCH_OWNER_SRC) -o $(SOCK_DRAIN_DIR)/bfetch-owner
	@$(SOCK_DRAIN_DIR)/bfetch-owner > $(SOCK_DRAIN_DIR)/bfetch-owner.log
	@tail -6 $(SOCK_DRAIN_DIR)/bfetch-owner.log
