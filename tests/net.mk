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
# SYS_SOCKET glue and the root-only privilege check against c/fs/vfs/vfs_cred.c)
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
#
# THE STUB DIRECTORY THAT SENTENCE NAMES DID NOT EXIST UNTIL 2026-08-28, and
# nothing said so. `UNIX_INC` has pointed at -Itests/unit/unixstub since this
# section landed; `git ls-files tests/unit/ | grep -i unix` returned exactly one
# file, unix_test.c. So `make test-unix` died at c/net/core/unix.c:6 --
# `fatal error: 'kheap.h' file not found` -- and the "132 checks and THREE
# controls" counted below had not been reproducible since. Both headers are
# rebuilt now from what unix.c and unix_test.c require; every number in this
# file was RE-MEASURED against them rather than carried over, and all four
# (132/132, then 8/1/18) came back identical, which is what says the
# reconstruction is the same instrument and not merely a working one.
# ============================================================================

.PHONY: test-unix test-unix-host test-unix-negctl

# AEX authenticated channel lifecycle, using actual Unix and file/FD code.
# Temporary negative fixtures precede the positive run even under make -j.
.PHONY: test-agent-channel-host test-agent-channel-negctl
test-agent-channel-negctl:
	@python3 tests/unit/agent_channel_test.py --build $(BUILD)/agent-channel --negative
test-agent-channel-host: test-agent-channel-negctl
	@python3 tests/unit/agent_channel_test.py --build $(BUILD)/agent-channel --positive

UNIX_INC := -Itests/unit/unixstub -Ic/net/core -Iinclude/abi $(FS_INC)

# -D_FORTIFY_SOURCE=0 IS NOT TIDINESS AND IT IS NOT OPTIONAL ON THIS HOST.
# unix_test.c includes the host <string.h> and then #includes unix.c, whose
# line 10 is `void *memset(void *, int, size_t);` -- a bare extern, correct for
# a freestanding kernel TU. On macOS <secure/_string.h> has by then made
# `memset` a MACRO expanding to __builtin___memset_chk, so that declaration is
# parsed as a redeclaration of the builtin with the wrong arity and the gate
# does not compile at all:
#
#   c/net/core/unix.c:10:7: error: conflicting types for '__builtin___memset_chk'
#
# This is the fourth instance of the shape already fixed in c/apps/browser/
# layout.c, c/net/core/sock.c and c/net/ip/ip.c with `#ifndef memset` guards.
# The guard in the source is the better fix and belongs to whoever owns that
# file; this flag is the half that can live in the test recipe, it is portable
# (glibc honours _FORTIFY_SOURCE=0 identically), and it becomes redundant
# rather than wrong the day the guard lands. What it costs, said plainly: the
# host's compile-time object-size checking is off for this TU, so a
# fixed-size overflow that clang would have caught is left to ASan. Every
# assertion in the gate is unaffected -- 132/132 with the flag, and the three
# controls redden by exactly the counts below.
UNIX_HOSTDEF := -D_FORTIFY_SOURCE=0

test-unix: test-unix-host test-unix-negctl

test-unix-host:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra $(UNIX_HOSTDEF) -o $(BUILD)/unix_test \
		tests/unit/unix_test.c $(UNIX_INC)
	@$(BUILD)/unix_test

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
#   RE-MEASURED 2026-08-28 against the rebuilt stub, and the alternation did
#   NOT reproduce: 18 over 8 consecutive runs WITH the flush and 18 over 8
#   WITHOUT it. The reason is worth more than the correction -- these recipes
#   redirect to a file, so stdout is FULLY buffered and cannot split mid-line;
#   every check line is emitted in one write at exit. What the flush changes
#   here is WHERE the abort lands: without it the whole stdout buffer is
#   written after the process's stderr, so the abort appears as the FIRST FAIL
#   in the log and reads as the first thing that went wrong, when it is the
#   last. With it, the log is in the order the failures happened. The 17/18
#   alternation is what a TTY (line-buffered) or a `| tee` gives, which is how
#   a person runs it by hand. Keep the flush for both reasons.
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
	  $(CC) -O2 -w $(UNIX_HOSTDEF) -D$$d -o $(BUILD)/unix_nc_$$d \
	      tests/unit/unix_test.c $(UNIX_INC); \
	  if $(BUILD)/unix_nc_$$d >$(BUILD)/unix_nc_$$d.log 2>&1; then \
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

# =============================================================================
# TCP'S TIMERS OFF THE WINDOW MANAGER'S LOOP (2026-08-21).
#
# Added to this fragment for the reason its opening comment gives -- it is the
# shared net-test landing spot several agents append to. Nothing here is needed
# to BUILD the change: it is c/net/core/net.c, c/net/transport/tcp.{c,h} and one
# verb in c/kernel/core/kdiag.c, all of which C_SRC already globs.
#
# WHAT CHANGED, in one sentence: tcp_poll() is TCP's timer wheel (retransmit,
# delayed-ACK flush, zero-window persist, TIME_WAIT reaping, the drain that
# pushes queued bytes when the window reopens) and its only steady-state caller
# was `if (!g_net_busy) net_poll();` in c/kernel/gui/wm.c -- once per
# composited frame. It is now raised by a 10 ms ktimer onto SOFTIRQ_NET, the
# same context the receive path already ran in. net_poll() still calls it, but
# only to discharge a pass the timer already owed, so that line is harmless and
# its owner may delete it.
#
# THE HOST SIDE IS UNCHANGED AND THAT IS THE POINT. tests/unit/tcp_test.c drives
# tcp_poll() directly with a fake clock; it does not care who calls it on the
# machine, so `make test-tcp-host` is the regression check that the timer PASS
# still does what it did (241 checks, unchanged before and after) while this
# gate checks WHO RUNS IT.
# =============================================================================

.PHONY: test-tcp-timer test-tcp-timer-wedge

# THE GATE. Boots LogitOS, backgrounds /bin/httpd, and fetches a 35 KB file
# from the HOST three times -- cutting the virtual wire (QMP set_link) for 1.5 s
# mid-response each time, so that only a retransmission TIMEOUT can restart the
# flow. Round 1 has the WM running (the apparatus check), round 2 has net_poll
# parked (`echo netwedge 30000 > /dev/ktrigger`), and round 3 parks net_poll AND
# forces TCP's timers back onto it (`netwedge 30000 wm`) -- the pre-change
# wiring at runtime, which MUST stall.
#
# WHY THE CUT IS NOT OPTIONAL, and why "wedge the WM and fetch a file" is not
# this test: lsock_file_write() calls tcp_send_nb(), which calls tcp_output()
# itself, and every subsequent push is driven by the peer's ACK arriving on the
# receive softirq. Over SLIRP, which drops nothing, a whole response is
# delivered with tcp_poll() never running once. A gate written that way would
# pass identically before and after the change -- the exact shape CLAUDE.md
# records for the WPT runner that linked layout.c and never called layout_page().
#
# WHY THE NEGATIVE CONTROL IS IN-RUN rather than a `test-tcp-timer-wedge-negctl`
# target: a second boot doubles a three-minute device test, and -- the reason
# that matters -- it would not share round 1's apparatus check, so a control
# that "failed" because set_link had silently stopped cutting the wire would
# read as a pass. The harness fails loudly with NEGATIVE CONTROL FAILED if
# round 3 recovers. (It also sidesteps the stranded-control trap in
# tools/audit_tests.py: a `*-negctl` target that no suite names is run never.)
#
# -DTCP_TIMERS_ON_WM is the same control at BUILD time and is the one the
# design was written against; it is not a target here because adding it needs a
# knob in the main Makefile (`ifeq ($(TCPONWM),1)`), which is contended. The
# runtime switch sets the same variable, so one boot measures both wirings --
# and in that build the runtime switch REFUSES to turn the timers back on, on
# purpose: no ktimer was armed there, so honouring it would leave a machine
# with no driver for tcp_poll at all and the gate would fail for a reason that
# has nothing to do with the property.
#
# BOTH CONTROLS WERE WATCHED FAILING on this machine, 2026-08-21, and here is
# what they printed. Runtime (round 3, every run): `complete=False
# t_recover=never`, 8,192-16,384 of 35,149 body bytes, and the park's own
# report reading `tcp timer fires +0, softirq passes +0`. Build-time (a whole
# ISO compiled with the define, run through this same harness): R1 still
# recovers in 1.60 s -- the WM is running, so net_poll still drives the pass --
# and R2 reads `never` with `fires +0`, exit status 1, on the two lines
#
#   - R2: with the WM wedged the response never recovered from a 1.5 s wire
#     cut -- the timers are still on the WM loop
#   - R2: the park reported 0 ktimer fires / 0 softirq passes
#
# That R1 keeps passing in the control build is the part worth keeping: it is
# what separates "the change is absent" from "the harness broke".
test-tcp-timer-wedge: $(ISO) $(DISK)
	@bash tests/boot/run-tcp-timer-wedge.sh $(ISO) $(DISK)

test-tcp-timer: test-tcp-timer-wedge

# THROUGHPUT, BEFORE AND AFTER -- not a gate, a measurement, and it needs two
# ISOs so it cannot be a plain target. net_poll() no longer calls tcp_poll() on
# every trip round a blocking fetch's loop; it runs the pass at most once per
# 10 ms tick. Nothing in tcp.c can TIME differently -- every deadline in it is
# compared against timer_ticks(), which only advances at 100 Hz -- but
# tcp_output()'s unconditional call at the end of the pass did get rarer (~490
# calls/s down to 100/s, measured from the park counters), so the claim had to
# be measured rather than argued.
#
# MEASURED, 2026-08-21, tests/boot/run-net-ab.sh (paired, both arms booted
# together, fetches round-robin so the samples are seconds apart -- read that
# script's header on why unpaired numbers on this host are noise):
#
#   arm      n  median Mbit/s   IQR            retrans
#   before   9  301.4           267.7-311.2    0/5757     (-DTCP_TIMERS_ON_WM)
#   after    9  300.8           291.0-322.2    0/5760
#   paired per-rep ratio: median 1.004, range 0.88-1.28, faster in 5/9
#
# 917,504-byte body, e1000 over netwire (delay 0, loss 0), TCG, -smp 4 -m 512M.
# The two kernels differed ONLY by the define: the "after" kernel was rebuilt
# after the "before" arm and came back md5-identical to the one saved before it
# (944fa749d7c244f693b29f58a2a00329), which is what rules out a concurrent
# agent's edit landing between the arms.
#
# THE DENOMINATOR, because a throughput number from a run that was silently
# dropping frames is not a throughput number. e1000's own counters over the
# same body on each build (tests/nic.mk; build/net-bench-{before,after}):
#
#   [e1000] stats: rx 1939 pkt / 2868822 B, tx 1034 pkt / 67430 B;
#                  drop rnbc 0 mpc 0; err crc 0 rlec 0        (before)
#   [e1000] stats: rx 1937 pkt / 2867260 B, tx 1036 pkt / 67558 B;
#                  drop rnbc 0 mpc 0; err crc 0 rlec 0        (after)
#
# To repeat it:
#     # add `#define TCP_TIMERS_ON_WM 1` to the top of c/net/core/net.c
#     make build/logit.iso && cp build/logit.iso /tmp/before.iso
#     # remove it again
#     make build/logit.iso && md5sum build/kernel.elf   # must match the after
#     make test-net-ab BEFORE=/tmp/before.iso REPS=9
#
# ONE APPARATUS NOTE, unrelated to this change and unresolved: both bench
# harnesses print `!! bound eth0, wanted e1000`. The guest now says
# `[net] NIC bound: eth0 = e1000` and the scripts still grep for the bare
# driver name. It hits both arms identically so it cannot bias the comparison,
# but run-net-bench.sh treats it as WRONG DRIVER and refuses to print a table,
# which is why the counters above were read out of its serial log by hand.

# WIRED, deliberately, and this is the one line that does it. The audit's
# UNWIRED baseline is a set that must not GROW (CLAUDE.md, "the test suite"), so
# a new test-* target that names no suite is debt the moment it lands -- and
# worse, a gate nobody runs is a gate that rots silently. It is a QEMU boot of
# about three minutes, the same order as the other ci-boot members
# (test-sched, test-virtio-balloon, test-lm-os).
#
# THE PARENT, NOT THE MEMBER. Naming `test-tcp-timer-wedge` here satisfies the
# audit for that one target and leaves the aggregate `test-tcp-timer` reported
# as unwired -- CLAUDE.md's "wire the parent, not the member", measured: the
# audit named `test-tcp-timer` as NEW UNWIRED with the member wired, and named
# neither once the parent was.
ci-boot: test-tcp-timer

# =============================================================================
# STEP 4a OF THE BKL REMOVAL -- net_lock() IS A REAL LOCK, GATED ON THE MACHINE.
#
# WHY THIS BLOCK EXISTS AT ALL. c/net/link/eth.c already carried a negative
# control -- `#ifdef NETLOCK_NEGCTL_NO_TX_LOCK`, the transmit path exactly as it
# was before the step -- and its own comment ends "A control that cannot be
# watched failing reads like a control and is not one, so this is the one that
# must be run." Nothing ran it: the define appeared in no Makefile, no fragment
# and no script. That is rule 5 of CLAUDE.md happening to the control written to
# satisfy rule 5, and it is fixed here rather than noted.
#
# AND WHY IT HAS TO BE A BOOT. net.h degenerates net_lock() to a no-op under
# -DLOGIT_NET_HOST and tests/unit/tcpstub/net.h has its own no-op copy, so every
# host gate that touches the stack compiles the lock away and runs one thread.
# `make test-net`, `test-tcp-host`, `test-route` and `test-ip6-*` are all still
# the right regression checks for the PROTOCOL work -- 241 + 50 + 16 + 148 + 119
# + 51 + 42 checks that did not move -- and not one of them can observe this
# lock existing. Only a boot can.
#
# THE TWO ARMS DIFFER BY ONE -D AND BY NOTHING A PERSON CAN SEE: both boot, both
# take a DHCP lease, both fetch. The only difference is the `violations` field.
.PHONY: test-netlock test-netlock-negctl

# The control is a PREREQUISITE of the positive gate, which is the one-line fix
# CLAUDE.md names for the 61 stranded controls -- `test-X: test-X-negctl`. Naming
# it on a ci-boot: line instead would satisfy tools/audit_tests.py and still run
# it never, which is worse because it looks fixed. It is first so a broken
# apparatus is reported before a green positive run can be believed.
test-netlock: test-netlock-negctl $(ISO) $(DISK)
	@bash tests/boot/run-netlock-test.sh $(ISO) $(DISK)

# A SECOND ISO, into a sub-BUILD, because the control is a build-time define.
# $(MAKE) with BUILD= overridden is how this tree already lets several agents
# build at once (CLAUDE.md, "a sweep that manufactures bugs"); the disk image is
# NOT rebuilt -- ring 3 is identical in both arms and the property is entirely
# in the kernel, so the control shares $(DISK) and costs one kernel + one ISO.
test-netlock-negctl:
	@$(MAKE) --no-print-directory $(BUILD)/netlock-negctl/logit.iso \
		BUILD=$(BUILD)/netlock-negctl NETNOTXLOCK=1
	@NETLOCK=negctl bash tests/boot/run-netlock-test.sh \
		$(BUILD)/netlock-negctl/logit.iso $(DISK)

ci-boot: test-netlock

# =============================================================================
# CAN THIS MACHINE SUSTAIN VIDEO? -- calibrating the "269.9 Mbit/s, no
# denominator" number CLAUDE.md names, against a load shaped like streaming
# rather than a single burst. The e1000 MPC/RNBC investigation running
# alongside this one settled that MPC's growth in a live douyin session was a
# STICKY, non-clearing QEMU register re-added every sample (verdict:
# MPC_IS_ARTEFACT, four independent angles) -- so the counter the user quoted
# does not answer "will video choke", and this block is the part that does.
#
# THE INSTRUMENTS ALREADY EXISTED. `test-net-bench` (tests/netperf.mk) and
# `run-net-ab.sh`'s paired A/B are the right apparatus -- real TCP over a wire
# this tree controls (netwire.py; SLIRP alone cannot show a window-limited
# transfer at all, see that file's header) -- so nothing new was built for the
# receive side. One real bug in the SECOND stood between "exists" and "can be
# run": `run-net-ab.sh` used bash 4's `exec {fd}<>file` auto-allocation in
# three places, and macOS's shipped `/bin/bash` is 3.2.57 (CLAUDE.md's rule 1
# table, a new row for it) with no bash 4+ installed anywhere on this host.
# Every invocation failed before booting anything:
#
#   tests/boot/run-net-ab.sh: line 130: exec: {fd}: not found
#
# Fixed by replacing the three `{fd}` sites with a plain counted FD (10, 11,
# ...) opened via `eval "exec $fd<>...`" -- bash 3.2 has always supported
# fixed-number redirection, it is only the brace-allocation FORM that is new.
# BEFORE: every `run-net-ab.sh` call on this host exits before the first QEMU
# boots. AFTER: the same commands below, unchanged, complete and print a
# table. That is the control for this one-line-shaped fix: it was watched
# failing in exactly the form above, on every arm count, before the patch.
#
# THE NUMBERS, all TCG on a contended host (`uptime` mid-run: load 19-25 on 16
# cores -- CLAUDE.md's own "2.6x spread from host load alone" applies; treat
# every Mbit/s here as this-host-this-hour, not an absolute).
#
#   near-zero RTT (delay=0), e1000, 917,504 B x 5:
#     goodput  median 391.6 Mbit/s   spread 333.9-425.1   retrans 0/3194 (0%)
#   realistic RTT (delay=15, i.e. ~30-34ms guest-experienced RTT -- ordinary
#   CDN/broadband, not a pathological case), e1000, same body x 5:
#     goodput  median  10.7 Mbit/s   spread  10.5-10.9    retrans 0/3195 (0%)
#
# THAT IS A 36x DROP WITH ZERO LOSS AT THE TCP LAYER BOTH TIMES. Not a single
# segment was retransmitted or arrived out of order in either run -- this is
# NOT a story about dropped frames (the MPC investigation already closed that
# question), it is a story about RATE: a short (897 KB) flow needs ~21 round
# trips to finish at this RTT, standard TCP slow start over 21 RTTs is
# inherently far below the zero-RTT ceiling, and something on this machine
# makes each of those round trips cost more than an idealised stack would (a
# 900 KB transfer at this RTT under textbook slow start -- IW≈14.6KB doubling
# each RTT -- finishes in 6-8 RTTs, not 21).
#
# WHERE THE CEILING IS -- decided by a PAIRED, CONTEMPORANEOUS comparison
# (run-net-ab.sh, same delay=15, same host-minute, fetches alternating so
# neither arm can blame the other's host-load moment), because a driver guess
# from one arm's number is exactly the kind of unpaired claim CLAUDE.md's own
# run-net-ab.sh header was written to rule out:
#
#   e1000        n=5  median 10.7  IQR 10.6-10.8  rtt 33.25ms  retrans 0/3193
#     rx->ack turnaround  median 18.972ms  p90 20.332  p99 21.546  max 21.723
#   virtio       n=5  median  8.1  IQR  8.0-8.2   rtt 33.58ms  retrans 5/3198
#     rx->ack turnaround  median 19.036ms  p90 20.478  p99 21.720  max 22.080
#   paired ratio (virtio/e1000): median 0.759, virtio SLOWER in 5 of 5 reps
#
# virtio-net -- a completely different driver with none of e1000's descriptor-
# ring or QEMU-model quirks -- is not faster, it is 24% SLOWER, and the two
# drivers' rx-to-ack turnaround (time from a data frame crossing the wire to
# this guest's ACK crossing back, measured by the relay so the guest cannot be
# blamed for the relay's own queueing -- see netwire.py's comment on that
# number) agree to within 0.3%. That rules the NIC OUT: whatever costs the
# 36x is in code both drivers sit on top of -- the TCP stack's ACK/window
# handling and/or the softirq or BKL scheduling under it -- not in e1000's
# ring, MPC, or anything else specific to one card.
#
# ~19ms of that turnaround is worth naming against CLAUDE.md's own numbers.
# net.c documents receive as SOFTIRQ_NET raised by the NIC ISR, and
# irq_isr_entry() (c/drivers/core/irq.c:186) takes the BKL on any non-nested
# entry -- so an ACK cannot leave until whichever core is servicing this
# connection's softirq can get the global lock. The MACHINE SAID SO WHILE
# THIS RAN: kdiag's tickloss self-check (c/kernel/core/ktime.c, always live,
# not something this job turned on) printed, during the delay=15 run --
#
#   [time] tickloss +79/s worstgap=786039us missed=170 of 3921 expected
#          (4.335% of the clock); uptime 39s
#   [time] tickloss   worst gap: bkl held by cpu 3 ra=0x136d5e via=bare;
#          tick on cpu 0, pid 2, not in kernel
#
# -- i.e. up to ~79 of that second's 100 expected 10ms ticks lost, and one
# single gap of 786 ms the kernel's OWN bookkeeping attributes to a bare
# (non-irqsave) BKL hold on another core. That figure is not this job's
# arithmetic; ktime.c prints it from a live ticket sample with no
# argument attached.
#
# ONE MORE CAP, FOUND BY HITTING IT: SYS_HTTP_GET's underlying kernel client
# (c/net/http/http.c) buffers the whole response in a fixed `raw[RAW_MAX]`,
# RAW_MAX = 1024*1024 -- one mebibyte, PER REQUEST, HARD. Asked for 6,000,000
# bytes at delay=15, three separate reps all delivered ~1,179,648 wire bytes
# (body + headers) and no more -- tight enough across reps (not a random
# truncation) to be the cap, not noise. run-net-bench.sh's own --bytes flag
# already documented this ("under http.c's 1 MiB cap"); this job just ran into
# it by asking for more, which is why every number above uses 917,504 B.
#
# WHICH CODE PATH THIS ACTUALLY MEASURES, stated because it changes what the
# 10.7 Mbit/s number is evidence FOR. SYS_HTTP_GET is TOP-LEVEL PAGE
# NAVIGATION ONLY (browser.c: "the kernel does DNS+TCP+TLS+HTTP (SYS_HTTP_GET)
# and hands us the raw body") and its own comment above HTTP_FETCH_CAP_TICKS
# in wm.c says outright that it "holds the big kernel lock" for the fetch's
# whole duration, up to a hard 15 s wall-clock cap, during which "the WM
# thread cannot enter the kernel to drain input... the machine appears hung,
# not merely slow". BROWSER SUBRESOURCES -- images today, and structurally
# whatever a video player's own segment fetches would use -- do NOT take this
# path: browser_rt.c:1855 overrides the `res_fetch()` symbol the kernel's
# http.c also defines with a ring-3 version that calls `bfetch_sync()`, and
# bfetch.h's own header names its mechanism: "the non-blocking socket ABI
# (sock_open/poll/send/recv)". So the 36x collapse measured here is a real,
# reproducible property of PAGE LOADS, not proven to be identically the
# number a video segment's own fetch would see -- this job had no existing
# on-device instrument to drive bfetch's non-blocking path at the scale
# needed for an equally load-bearing number, and that is a named gap rather
# than an assumption filled in either direction.
#
# THE ANSWER THAT DOES NOT DEPEND ON WHICH PATH: the mechanism under both
# paths is the same shared machinery -- one BKL, taken by every kernel entry
# including the NIC's own interrupt (irq.c:186), and net.c's own SOFTIRQ_NET
# receive path runs under it regardless of which ring 3 call started the
# socket. CLAUDE.md's own already-verified, independently-measured numbers
# (2026-08-28, "The desktop is slow" section) put the COMPOSITOR'S share of
# BKL-held time at up to 99% on one core while painting, 94 ms per composited
# frame at 10.6 fps during an ordinary drag or keystroke. This job's own
# instrumentation, on this same host, independently caught the SAME lock
# stalling a different consumer -- a plain network fetch -- for up to 786 ms
# at a stretch. A real viewing session needs BOTH: a socket pulling segments
# and a compositor painting the frames those segments decode into, and both
# now have their OWN measured evidence of blocking on the one lock between
# them.
#
# THE VERDICT. NO, the raw pipe is not what would choke a video site: at
# near-zero RTT the NIC+ring+TCP stack delivers a clean, lossless 391.6
# Mbit/s -- 78x 1080p's ~5 Mbit/s and 16-25x 4K's 15-25 Mbit/s -- and a
# controlled driver swap shows the NIC is not even the marginal cost. YES,
# something on this machine chokes sustained throughput under conditions
# closer to real use: realistic RTT alone cost 36x on the one path fully
# instrumented here, with zero packet loss either time, and the mechanism is
# the same global lock CLAUDE.md's compositor investigation already
# convicted for painting -- now independently caught, by this job, doing the
# same thing to a socket.
#
# make test-net-video           -- reruns the paired e1000/virtio comparison
#                                   above (delay=15, 5 reps, 917,504 B/rep)
# make test-net-video DELAY=0   -- the near-zero-RTT arm for comparison
.PHONY: test-net-video

# Actual /bin/net consumers, through the host's file/socket syscall adapter.
# --gc-sections / -dead_strip is the same identity-only HTTP link contract as
# the product; no fake inflater is linked. Sanitizers cover the shipped parser
# and primitives here; BIOS/device evidence lives in the companion boot gate.
NET_CONSUMER_SRC := c/apps/coreutils/net.c tests/unit/net_cli_host.c \
                    $(filter-out c/apps/libc/%,$(NET_CLI_SRC))
NET_CONSUMER_INC := -Ic/apps -Ic/crypto -Ic/crypto/hash -Ic/net/http -Iinclude/abi
# Do not depend on another fragment defining a make comma helper.
NET_CONSUMER_COMMA := ,
NET_CONSUMER_GC := $(if $(filter Darwin,$(shell uname -s)),-Wl$(NET_CONSUMER_COMMA)-dead_strip,-Wl$(NET_CONSUMER_COMMA)--gc-sections)
NET_CONSUMER_CF := -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
    -DNET_FETCH_TIMEOUT_MS=5000 -include tests/unit/net_cli_host.h $(NET_CONSUMER_INC)
NET_CONSUMER_DEPS := $(NET_CONSUMER_SRC) tests/unit/net_cli_host.h \
    c/apps/coreutils/net_digest.inc c/apps/coreutils/net_fetch.inc c/apps/download_name.h \
    c/crypto/crypto.h c/crypto/hash/blake3.h c/crypto/hash/blake2b.h c/net/http/http1.h

$(BUILD)/net-consumer-host: $(NET_CONSUMER_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(NET_CONSUMER_CF) $(NET_CONSUMER_GC) $(NET_CONSUMER_SRC) -o $@
$(BUILD)/net-consumer-negative: $(NET_CONSUMER_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(NET_CONSUMER_CF) $(NET_CONSUMER_GC) -DNET_DIGEST_NEGCTL $(NET_CONSUMER_SRC) -o $@

.PHONY: test-net-consumers test-net-consumers-negctl test-net-consumers-os test-net-consumers-tls-os
test-net-consumers: test-net-consumers-negctl
test-net-consumers-negctl: $(BUILD)/net-consumer-host $(BUILD)/net-consumer-negative
	python3 tests/unit/net_consumers_run.py --binary $(BUILD)/net-consumer-host
	python3 tests/unit/net_consumers_run.py --binary $(BUILD)/net-consumer-negative --negative

$(BUILD)/net-consumers-guest.elf: tests/unit/net_consumers_guest.c $(APPDIR)/clib.h $(APPDIR)/logit.h $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -c $< -o $(BUILD)/apps/net-consumers-guest.o
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/net-consumers-guest.crt.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/net-consumers-guest.crt.o $(BUILD)/apps/net-consumers-guest.o

test-net-consumers-os: test-net-consumers $(ISO) $(DISK) $(BUILD)/net-consumers-guest.elf
	python3 tests/boot/run-net-consumers.py --build $(BUILD)
test-net-consumers-tls-os: test-net-consumers $(ISO) $(DISK) $(BUILD)/net-consumers-guest.elf
	python3 tests/boot/run-net-consumers.py --build $(BUILD) --tls
ci-host: test-net-consumers
ci-boot: test-net-consumers-os test-net-consumers-tls-os

test-net-video: $(ISO) $(DISK)
	@bash tests/boot/run-net-ab.sh --disk $(DISK) \
	    --arm e1000:$(ISO):e1000 \
	    --arm virtio:$(ISO):virtio-net-pci:virtio-net \
	    --reps $(if $(REPS),$(REPS),5) --delay $(if $(DELAY),$(DELAY),15) \
	    --bytes $(if $(BYTES),$(BYTES),917504)

# The former absent lsock_file_poll weak hook returned NVAL for valid Unix
# descriptors. Test real Unix/lsock/poll/wait code, including query-to-sleep
# registration and named-datagram capacity wakes; each semantic control must
# fail by its named assertion before the positive suite or agent gate runs.
.PHONY: test-unix-poll test-unix-poll-negctl
test-unix-poll-negctl:
	python3 tests/unit/unix_poll_test.py --build $(BUILD)/unix-poll --negative
test-unix-poll: test-unix-poll-negctl
	python3 tests/unit/unix_poll_test.py --build $(BUILD)/unix-poll
test-agent: test-unix-poll
