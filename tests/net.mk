# SOCK_RAW / ICMP raw sockets: the protocol layer (c/net/core/raw.c) and its
# host test.
#
# Kept in its own fragment for the reason tests/link.mk and tests/sockserv.mk
# record: several agents edit the Makefile at once and a shared-file edit
# cannot be committed without swallowing theirs. The Makefile pulls it in with
# one line, next to the other `-include tests/*.mk` lines (e.g. beside
# `-include tests/sockserv.mk`):
#
#     -include tests/net.mk
#
# Nothing here is needed to BUILD any of it: C_SRC globs c/net, so
# c/net/core/raw.c and the SOCK_RAW branch in c/net/core/lsock.c link with no
# Makefile change. /bin/ping is NOT yet built or shipped -- see the note at
# the bottom of this file for the one CLI list edit that would add it.

.PHONY: test-raw-host test-raw-negctl test-raw

RAW_INC := -Ic/net/core -Ic/net/link -Ic/net/ip -Ic/net/transport -Ic/net/dns \
           -Ic/drivers/timer -Ic/kernel/core

# White-box, same shape as tests/unit/net_proto_test.c (test-net-proto in the
# main Makefile): #includes ip.c/reasm.c/icmp.c/raw.c directly and stubs only
# ARP/Ethernet/the RNG/the timer, so a delivered message runs the REAL
# ip_input() -> icmp_input() -> raw_icmp_deliver() path. Covers: the socket
# table (open/refuse-when-full/close-and-reuse), raw_icmp_send() building only
# the IP header around a caller-supplied ICMP message (the IP_HDRINCL-refused
# contract), fan-out to every open socket off one real inbound frame, and the
# queue-full drop rule (newest loses, mirrors udp.c).
#
# WHAT IT DOES NOT COVER: c/net/core/lsock.c's LOGIT_SOCK_RAW branch (the
# SYS_SOCKET glue and the root-only privilege check against c/fs/vfs_cred.c)
# is not reachable from a host test -- lsock.c pulls in file.c, proc.c and
# vfs_cred.c, i.e. most of the kernel's process model. See the on-device note
# below for how that half is meant to be checked instead.
test-raw-host:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/raw_test \
		tests/unit/raw_test.c $(RAW_INC)
	@./$(BUILD)/raw_test

# Negative control: RAW_NEGCTL_FIRST_ONLY delivers an inbound message to only
# the FIRST open raw socket instead of every one of them -- as if the table
# were a single shared slot rather than a real fan-out. The suite MUST fail,
# and specifically on the fan-out check (the grep below), not on some
# unrelated check that would make this a control for the wrong property.
test-raw-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DLOGIT_NET_HOST -DRAW_NEGCTL_FIRST_ONLY -o $(BUILD)/raw_test_negctl \
		tests/unit/raw_test.c $(RAW_INC)
	@if ./$(BUILD)/raw_test_negctl >$(BUILD)/raw_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes when only the first raw socket gets delivery"; \
		exit 1; \
	elif ! grep -q "every open raw socket should receive its own identical copy" $(BUILD)/raw_negctl.log; then \
		echo "NEGATIVE CONTROL INCONCLUSIVE: it failed, but not on the fan-out check"; \
		sed -n '1,20p' $(BUILD)/raw_negctl.log; \
		exit 1; \
	else \
		echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/raw_negctl.log) checks fail without real fan-out"; \
	fi

test-raw: test-raw-host test-raw-negctl

# --- ON DEVICE: NOT ADDED HERE, AND HERE IS WHY -----------------------------
#
# /bin/ping (c/apps/coreutils/ping.c) is written and compiles standalone
# against the real toolchain (verified by hand, not by a `make` target --
# see this change's report), but it is not on the CLI list
# (`CLI := sh echo ls ...` in the main Makefile) and therefore not on
# build/disk.img, and the Makefile is the one file this change is explicitly
# told not to edit. Adding a boot harness here would either (a) build
# ping.aex some OTHER way, duplicating CLI_RULE's four lines and drifting
# from it the first time that rule changes, or (b) silently do nothing on
# every checkout that has not also had the one-line Makefile edit applied --
# a gate that looks wired and is not is worse than no gate (see CLAUDE.md's
# "test suite" section on exactly that failure mode). Neither is a boot
# harness added quietly; both are worse than saying so here.
#
# THE ONE LINE THE MAKEFILE NEEDS, when someone with write access to it is
# free: add `ping` to the CLI list, e.g.
#
#     CLI := sh echo ls cat pwd wc head true false sleep mkdir rm touch \
#            clear uname net cp mv smptest socktest ping show dir chart \
#            prog clip notify execinfo entropy httpd stat poweroff reboot pref
#
# After that one line, a device check is cheap and belongs here as
# `test-raw-os`: boot, run `net ping` (the OLD SYS_NET_PING path, already
# built) and `/bin/ping` (this one) against the same target (the gateway,
# net_cfg.gw, is always reachable) in the same boot, and compare the RTTs --
# that comparison, not "did it print a number", is what proves the raw socket
# actually carried the ICMP packet rather than reporting a plausible-looking
# fake. tests/boot/run-link-test.sh is the template to copy (QMP boot +
# serial-log grep); this file does not invent a new harness shape.

# =============================================================================
# The DNS resolver (c/net/dns/dns.c) -- a DIFFERENT change than the section
# above; added here (2026-08-20) only because this fragment is the shared
# net-test landing spot several agents append to, per its own opening
# comment. Owner-name matching + CNAME chasing, EDNS0 + a real DNS-over-TCP
# fallback on TC, and per-record TTLs replacing the old fixed 120 s -- see
# the header comment at the top of c/net/dns/dns.c for the full account of
# what changed and why. tests/unit/dns_test.c drives it with byte-array
# response packets (a CNAME chain, an answer for an unrelated name, a
# truncated response completed over a MODEL TCP connection with trickled
# short reads, a self-referencing compression pointer, TTLs read off the
# record including the floor/ceiling clamp, and the EDNS0 OPT record on
# every outgoing query) -- no network, no kernel.
#
# Deliberately does NOT re-test what tests/unit/ip6_dns_test.c already
# covers (AAAA, RFC 6724 dual-stack ordering) or what
# tests/unit/net_proto_test.c already covers (the legacy dns_start()/
# dns_result() API's arming and ICMP-error handling against REAL udp.c/
# icmp.c) -- both entry points share dns.c's one parsing/state-advancement
# implementation now (dq_advance()/collect_answers()), so exercising it
# through the async pool here exercises the legacy path's logic too.

.PHONY: test-dns test-dns-negctl

DNS_INC := -Ic/net/dns -Ic/net/ip -Ic/net/link -Ic/net/core -Ic/net/transport \
           -Ic/drivers/timer -Ic/kernel/core

test-dns: test-dns-negctl
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DLOGIT_NET_HOST -o $(BUILD)/dns_test \
		tests/unit/dns_test.c $(DNS_INC)
	@./$(BUILD)/dns_test

# Negative control: DNS_NO_NAME_MATCH restores the pre-fix behaviour --
# accept an A/AAAA record regardless of whose name it is for. It must redden
# EXACTLY ONE check: the record for an unrelated name is no longer refused.
# Every other check (the CNAME chain, the TCP fallback, the compression-
# pointer loop, the TTL clamp, EDNS0, the spoofing guard) is UNAFFECTED by
# this macro and must stay green, which is what "exactly one" verifies --
# a control that reddens the whole suite is not measuring the one thing it
# claims to.
test-dns-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DLOGIT_NET_HOST -DDNS_NO_NAME_MATCH \
		-o $(BUILD)/dns_negctl tests/unit/dns_test.c $(DNS_INC)
	@if ./$(BUILD)/dns_negctl >$(BUILD)/dns_negctl.log 2>&1; then \
		echo "NEGATIVE CONTROL FAILED: the suite passes without owner-name matching"; \
		exit 1; \
	elif [ "$$(grep -c '^FAIL' $(BUILD)/dns_negctl.log)" != "1" ]; then \
		echo "NEGATIVE CONTROL INCONCLUSIVE: expected exactly 1 failing check (the unrelated-name case), got $$(grep -c '^FAIL' $(BUILD)/dns_negctl.log)"; \
		sed -n '1,20p' $(BUILD)/dns_negctl.log; \
		exit 1; \
	elif ! grep -q "a record for a name we did not ask about is refused" $(BUILD)/dns_negctl.log; then \
		echo "NEGATIVE CONTROL INCONCLUSIVE: it failed, but not on the unrelated-name check"; \
		sed -n '1,20p' $(BUILD)/dns_negctl.log; \
		exit 1; \
	else \
		echo "negative control ok: exactly 1 check fails (the unrelated-name case) without owner-name matching"; \
	fi

# ============================================================================
# AF_UNIX (c/net/core/unix.c) -- the host gate and its three negative controls.
#
# Nothing here is needed to BUILD it either: C_SRC globs c/net, so unix.c and
# the AF_UNIX branch in lsock.c link with no Makefile change. What DOES need
# one Makefile edit is the consumer -- see the note at the bottom of this file.
#
# WHITE BOX. unix_test.c #includes unix.c whole and supplies the two services
# the kernel would (tests/unit/unixstub/{kheap.h,kernel/core/wait.h}), which is
# the same shape tests/unit/tcp_test.c uses on tcp.c. The include path order is
# load-bearing: -Itests/unit/unixstub comes FIRST so the stub wait.h wins over
# c/kernel/core/wait.h, exactly the ordering trap CLAUDE.md's "Source layout"
# section records about uonly/.
# ============================================================================

.PHONY: test-unix test-unix-host test-unix-negctl

UNIX_INC := -Itests/unit/unixstub -Ic/net/core -Iinclude/abi -Ic/fs

test-unix: test-unix-host test-unix-negctl

test-unix-host:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/unix_test tests/unit/unix_test.c $(UNIX_INC)
	@./$(BUILD)/unix_test

# THREE CONTROLS, EACH WATCHED FAILING AND EACH ON ITS OWN EXACT COUNT.
#
# One control would not do here, because the three properties that make this
# not-a-pipe fail independently: message boundaries, the wake, and having two
# directions. A single switch that reddened the whole suite would prove only
# that the build changed. So each is asserted on the number of checks it
# reddens AND on one check NAME, and the counts below are measured on this
# machine (2026-08-20), not remembered:
#
#   STREAMRECS  8 FAILs, every one tagged `bounds:`   124/132 still pass
#   NOWAKE      1 FAIL   -- the stub's wait_event abort, by queue name
#   ONEDIR     18 FAILs  -- 17 checks + the abort (see unix_read's comment)
#
# THE ONEDIR COUNT DEPENDS ON A flush, and it is worth knowing before anyone
# "simplifies" the stub: the checks print to buffered stdout and the abort to
# unbuffered stderr, so before ustub_stuck() gained its fflush(stdout) this
# number alternated between 17 and 18 run to run -- the abort landing inside a
# half-written check line and the two counting as one. That is a gate that
# fails intermittently while pointing at AF_UNIX. Verified stable at 18 over 8
# consecutive runs after the fix.
#
# WHY THE NAME AND NOT JUST THE COUNT: "8 checks fail" is satisfied by any
# eight, including eight that have nothing to do with record boundaries. The
# tag/name assertion is what ties the control to the property it claims.
test-unix-negctl:
	@mkdir -p $(BUILD)
	@fail=0; \
	for spec in "UNIX_NEGCTL_STREAMRECS 8 bounds: seqpacket truncates" \
	            "UNIX_NEGCTL_NOWAKE 1 block: close wakes reader" \
	            "UNIX_NEGCTL_ONEDIR 18 twoway: b reads a"; do \
	  set -- $$spec; d=$$1; want=$$2; shift 2; name="$$*"; \
	  $(CC) -O2 -w -D$$d -o $(BUILD)/unix_nc_$$d tests/unit/unix_test.c $(UNIX_INC); \
	  if ./$(BUILD)/unix_nc_$$d >$(BUILD)/unix_nc_$$d.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the suite passes with -D$$d"; fail=1; \
	  else \
	    got=`grep -c '^FAIL' $(BUILD)/unix_nc_$$d.log`; \
	    if [ "$$got" != "$$want" ]; then \
	      echo "NEGATIVE CONTROL INCONCLUSIVE: -D$$d reddened $$got checks, expected $$want"; \
	      grep '^FAIL' $(BUILD)/unix_nc_$$d.log | head -25; fail=1; \
	    elif ! grep -q "$$name" $(BUILD)/unix_nc_$$d.log; then \
	      echo "NEGATIVE CONTROL INCONCLUSIVE: -D$$d failed, but not on '$$name'"; \
	      grep '^FAIL' $(BUILD)/unix_nc_$$d.log | head -25; fail=1; \
	    else \
	      echo "negative control ok: -D$$d reddens exactly $$want, incl. '$$name'"; \
	    fi; \
	  fi; \
	done; \
	if [ "$$fail" != "0" ]; then exit 1; fi
	@echo "unix: 3/3 negative controls fire"

# --- THE CONSUMER NEEDS ONE MAKEFILE LINE -----------------------------------
# /bin/syslogd (c/apps/coreutils/syslogd.c) is the daemon on the other end of
# every mini-libc syslog() call. It builds under the existing CLI_RULE with no
# new rule of its own -- it includes only "clib.h", not mini-libc -- so packing
# it is one word appended to the CLI list (Makefile ~line 462):
#
#     CLI := sh echo ls ... stat poweroff reboot pref syslogd
#
# Verified compiling clean for x86_64-elf with the exact UCFLAGS+INCDIRS that
# rule uses; not added here because the Makefile is contended.
