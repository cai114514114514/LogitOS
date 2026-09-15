# HTTP/2 in the browser's fetch transport.
#
# Targets live here rather than in the main Makefile on purpose: several lines
# are editing that file today and this is the one addition that does not have
# to be in it. (The one hunk that DID have to be is two filenames in
# BROWSER_PIPE -- http2.c and hpack.c were never linked into browser.aex, so
# without it there is no HTTP/2 in the browser at all.)
#
#   test-h2mux          the wiring, host-side: real browser_rt.c over a stubbed
#                       socket layer, against an in-memory HTTP/2 server.
#   test-h2mux-control  the same file against a browser_rt.c built with
#                       -DBXFER_H1_ONLY. REQUIRED to fail.
#   test-h2mux-reuse-control  the same loader paths with ready-h2 joining and
#                       idle-h2 retention disabled. REQUIRED to fail.
#   test-h2mux-asan     the same, under ASan/UBSan with the leak checker on.
#
# ON-DEVICE COVERAGE, and what is NOT here. The protocol itself is already
# proven on real servers by `make test-h2-os` (tests/boot/run-h2-smoke.sh drives
# /bin/h2check over the full e1000 -> DHCP -> DNS -> TCP -> TLS(ALPN) path), and
# `make test-live-page` / `test-browser-https` prove the browser still loads
# pages with http2.c linked into it. What has NO on-device harness yet is the
# browser's own fetch() over h2 -- it needs a fixture page whose script issues
# concurrent fetches, and the fixture has to be served from the host while the
# fetches target a real h2 origin, because our TLS verifies strictly against the
# built-in roots and therefore CANNOT talk to a local HTTPS server. That is the
# named gap, not an oversight; the counters it would read are already printed on
# the serial line ("[bxfer] h2 <host> closed: streams=N peak=M") whenever an h2
# connection is torn down, so the harness is a script away rather than a change.
#
# The stub directory must come FIRST on the include path: it is how
# tests/unit/h2stub/logit.h shadows the ring-3 c/apps/logit.h, which is what
# lets the real browser_rt.c be compiled on the host. c/apps is deliberately
# NOT on the path -- if it were, the real syscall wrappers would win.
H2MUX_INC := -Itests/unit/h2stub -Iinclude/abi -Ic/apps/browser -Ic/net/http -Ic/lib/image

# --- H2MUX_SRC WAS SUSPECTED OF THE DRIFT tests/canvas.mk AND
# --- tests/webapi_platform.mk BOTH HAD. IT WAS MEASURED, AND IT DOES NOT.
# On 2026-08-28 this target was failing to link with two undefined symbols,
# webapi_cookie_line and webapi_cookie_store_line, and the obvious reading was
# the one that was true next door: browser_rt.c grew a dependency (commit
# c1f024abb, "which requests carry the session -- SameSite reaches the
# transport") and the hand-written list did not follow it.
#
# It is NOT that, and the difference matters because the plausible repair is
# expensive and wrong. Both symbols are defined in js_webapi.c and DECLARED
# WEAK in browser_rt.c:35-38, with the contract written above them: "a build
# that links neither js_webapi.c nor cookies.c (the loader host tests)
# resolves both to NULL and runs cookieless." This harness is one of those
# builds -- tests/unit/h2mux_test.c asks nothing about cookies -- so the
# absence is the design. Linked with those two resolving to NULL, as ELF does
# for an undefined weak symbol, the suite is 98 checks / 0 failures.
#
# So DO NOT add js_webapi.c here to make the link succeed. It would pull
# QuickJS, the DOM and the whole Web-API surface into a test whose entire
# point is browser_rt.c over six stubbed socket syscalls, and it would turn a
# cookieless transport test into one that carries a session it never asked
# for. The undefined symbols on a Mach-O host are that host lacking ELF's
# weak-undefined semantics (it needs weak_import), which is a property of the
# DECLARATION, not of this list.
#
# What IS worth having is that the transport half can no longer drift: it is
# now the browser's own c/net/http files, taken from $(BROWSER_PIPE)
# (Makefile:790) rather than restated beside it, minus the one named below.
ifeq ($(strip $(BROWSER_PIPE)),)
H2MUX_LINK_ERR := tests/http2.mk: BROWSER_PIPE is empty -- this fragment must be -included from the Makefile, AFTER it. Refusing to link a transport test from a partial source list.
endif
# cookies.c is OUT: browser_rt.c includes cookies.h for CK_HEADER_MAX only and
# reaches the jar exclusively through the two weak doors above, so with
# js_webapi.c absent every byte of cookies.c is unreachable. A jar in the
# binary with neither door is more misleading than no jar. (The resulting set
# is byte-for-byte the list this line held before -- url, http1, hpool, http2,
# hpack -- so nothing about what this gate measures has changed.)
#
# ws.c is OUT too, added the day WebSocket landed in BROWSER_PIPE: it is
# transport-free protocol code (frame codec + handshake key), reachable from
# nothing h2mux_test.c exercises, and its ONE dependency outside itself is
# `extern void ocsp_sha1(...)` (c/net/ssh/base64.h + c/crypto/hash/sha1.c,
# neither of which this fragment links) -- so pulling it in here would not
# multiplex anything, it would just fail this harness's link on a symbol that
# has nothing to do with HTTP/2. Same shape as cookies.c: a file the browser
# links that this harness's two doors cannot reach.
H2MUX_HTTP_OUT := c/net/http/cookies.c c/net/http/ws.c
H2MUX_SRC := tests/unit/h2mux_test.c c/apps/browser/browser_rt.c \
             $(filter-out $(H2MUX_HTTP_OUT),$(filter c/net/http/%,$(BROWSER_PIPE)))

PREFETCH_QUEUE_SRC = $(filter-out tests/unit/h2mux_test.c,$(H2MUX_SRC)) tests/unit/prefetch_queue_test.c
PREFETCH_QUEUE_DEPS = $(PREFETCH_QUEUE_SRC) tests/unit/h2mux_test.c c/apps/browser/bfetch.h
$(BUILD)/prefetch_queue: $(PREFETCH_QUEUE_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -w $(H2MUX_INC) -o $@ $(PREFETCH_QUEUE_SRC)
$(BUILD)/prefetch_queue_old: $(PREFETCH_QUEUE_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -w $(H2MUX_INC) -DBFETCH_DROP_BUSY_PREFETCH -o $@ $(PREFETCH_QUEUE_SRC)
.PHONY: test-prefetch-queue test-prefetch-queue-negctl
test-prefetch-queue-negctl: $(BUILD)/prefetch_queue_old
	@rc=0; $< > $(BUILD)/prefetch_queue_old.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'all 24 offered resources prefetched before consumption (got 16)' $(BUILD)/prefetch_queue_old.log && \
	 grep -F 'prefetch-queue: 31 checks, 1 failures' $(BUILD)/prefetch_queue_old.log
test-prefetch-queue: test-prefetch-queue-negctl $(BUILD)/prefetch_queue
	@$(BUILD)/prefetch_queue
ci-host: test-prefetch-queue

.PHONY: test-prefetch-queue-asan
test-prefetch-queue-asan: test-prefetch-queue-negctl
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(H2MUX_INC) -o $(BUILD)/prefetch_queue_asan $(PREFETCH_QUEUE_SRC)
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/prefetch_queue_asan

# Only finite ordinary pending-owner/resource scheduling cases run here;
# including the in-memory peer does not invoke the separate h2mux suite.
BFETCH_PENDING_SRC = $(filter-out tests/unit/h2mux_test.c,$(H2MUX_SRC)) tests/unit/bfetch_pending_owner_test.c
BFETCH_PENDING_DEPS = $(BFETCH_PENDING_SRC) tests/unit/h2mux_test.c c/apps/browser/bfetch.h
$(BUILD)/bfetch_pending_owner: $(BFETCH_PENDING_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -w $(H2MUX_INC) -o $@ $(BFETCH_PENDING_SRC)
$(BUILD)/bfetch_pending_owner_old: $(BFETCH_PENDING_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -w $(H2MUX_INC) -DBFETCH_NO_PENDING_JOIN -o $@ $(BFETCH_PENDING_SRC)
.PHONY: test-bfetch-pending-owner test-bfetch-pending-owner-negctl
test-bfetch-pending-owner-negctl: $(BUILD)/bfetch_pending_owner_old
	@rc=0; $< > $(BUILD)/bfetch_pending_owner_old.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && \
	 grep -F 'pending owner must not block resource completion (h2.example)' $(BUILD)/bfetch_pending_owner_old.log && \
	 grep -F 'pending owner must not block resource completion (h1.example)' $(BUILD)/bfetch_pending_owner_old.log && \
	 grep -F 'fresh dialer must queue behind a full H1 budget (3)' $(BUILD)/bfetch_pending_owner_old.log && \
	 grep -F 'fresh resource redial exceeded H1 pool cap (3)' $(BUILD)/bfetch_pending_owner_old.log && \
	 grep -F 'bfetch-pending-owner: 37 checks, 4 failures' $(BUILD)/bfetch_pending_owner_old.log
test-bfetch-pending-owner: h2mux-link-check test-bfetch-pending-owner-negctl $(BUILD)/bfetch_pending_owner
	@$(BUILD)/bfetch_pending_owner
ci-host: test-bfetch-pending-owner

.PHONY: test-h2mux test-h2mux-control test-h2mux-reuse-control \
	test-h2mux-slot-control \
	test-h2mux-asan h2mux-link-check

# Two questions, and after the subtraction they are the only two left. The
# dangerous direction is impossible now -- a c/net/http file the browser links
# is in this test the same minute -- so what remains is a stale exclusion,
# which excludes nothing and has quietly stopped being a decision.
h2mux-link-check:
	@if [ -n "$(H2MUX_LINK_ERR)" ]; then echo "$(H2MUX_LINK_ERR)"; exit 1; fi
	@stale=""; for f in $(H2MUX_HTTP_OUT); do \
	    case " $(BROWSER_PIPE) " in *" $$f "*) ;; *) stale="$$stale $$f";; esac; \
	  done; \
	  if [ -n "$$stale" ]; then \
	    echo "h2mux-link-check: FAIL -- H2MUX_HTTP_OUT names files the browser no"; \
	    echo "  longer links, so the exclusion is a lie rather than a decision:"; \
	    for f in $$stale; do echo "    $$f"; done; exit 1; \
	  fi

test-h2mux: h2mux-link-check test-h2mux-control test-h2mux-reuse-control test-h2mux-slot-control test-h2mux-lifetime-control test-h2mux-presend-control
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w $(H2MUX_INC) -o $(BUILD)/h2mux_test $(H2MUX_SRC)
	@$(BUILD)/h2mux_test

# ASan matters more here than in most places: bxfer hands ONE connection to
# several exchanges and frees the serialized request only once the borrowed
# body has stopped being read, so the failure mode is a use-after-free that a
# passing functional test cannot see. Apple Clang's runtime aborts before main
# when detect_leaks=1 ("not supported on this platform"), so Darwin still runs
# ASan+UBSan but says out loud that LeakSanitizer is the one unavailable layer;
# treating that apparatus abort as a product failure hid a clean 115-check run.
test-h2mux-asan: h2mux-link-check test-h2mux-control test-h2mux-reuse-control test-h2mux-slot-control test-h2mux-lifetime-control test-h2mux-presend-control
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all \
	    $(H2MUX_INC) -o $(BUILD)/h2mux_asan $(H2MUX_SRC)
	@if [ "`uname -s`" = Darwin ]; then \
	    echo "test-h2mux-asan: Darwin -- ASan/UBSan active; LeakSanitizer unavailable"; \
	    ASAN_OPTIONS=detect_leaks=0 $(BUILD)/h2mux_asan; \
	  else \
	    ASAN_OPTIONS=detect_leaks=1 $(BUILD)/h2mux_asan; \
	  fi

# The negative control. Same test file, same server; the only difference is
# that browser_rt.c never offers h2 in ALPN, so there is no h2 connection for
# four requests to share. It MUST fail, and it must fail on the multiplexing
# assertions specifically -- an assertion nobody has watched fail is not a
# known-failing assertion.
test-h2mux-control: h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DBXFER_H1_ONLY $(H2MUX_INC) \
	    -o $(BUILD)/h2mux_ctl $(H2MUX_SRC)
	@if $(BUILD)/h2mux_ctl >$(BUILD)/h2mux_ctl.log 2>&1; then \
	    echo "test-h2mux-control: FAIL -- the suite passed with HTTP/2 disabled,"; \
	    echo "  which means test-h2mux is not measuring multiplexing at all."; \
	    exit 1; \
	else \
	    grep -q 'connections: expected 1, got 4' $(BUILD)/h2mux_ctl.log || exit 1; \
	    echo "test-h2mux-control: PASS -- disabling h2 is caught. It failed on:"; \
	    grep -m6 '^FAIL' $(BUILD)/h2mux_ctl.log || true; \
	    grep -c '^FAIL' $(BUILD)/h2mux_ctl.log | sed 's/^/  total failures: /'; \
	fi

# Exact lifetime failures, not a catch-all nonzero exit: the draining control
# wedges new work behind a long accepted response; the active-drop control
# recycles its fd/session under an owner which still holds both identities.
.PHONY: test-h2mux-lifetime-control

# Keep multiplexing intact; remove only pre-send retirement. Both production
# consumers must fail at that precise boundary, not on an unrelated assertion.
.PHONY: test-h2mux-presend-control
test-h2mux-presend-control: h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DBXFER_NO_PRESEND_REPLACE $(H2MUX_INC) -o $(BUILD)/h2mux_presend_ctl $(H2MUX_SRC)
	@$(BUILD)/h2mux_presend_ctl >$(BUILD)/h2mux_presend_ctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -m1 'pre-send POST failed instead of waiting' $(BUILD)/h2mux_presend_ctl.log && \
	 grep -m1 'pre-send loader failed instead of waiting' $(BUILD)/h2mux_presend_ctl.log

test-h2mux-lifetime-control: h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DBXFER_NO_DRAINING -DBXFER_CASE_SENSITIVE_HOST $(H2MUX_INC) -o $(BUILD)/h2mux_drain_ctl $(H2MUX_SRC)
	@$(BUILD)/h2mux_drain_ctl >$(BUILD)/h2mux_drain_ctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -m1 'draining origin blocked replacement' $(BUILD)/h2mux_drain_ctl.log && \
	 grep -m1 'mixed-case origin failed shared-session admission' $(BUILD)/h2mux_drain_ctl.log
	@$(CC) -O1 -g -w -DBXFER_DROP_ACTIVE_SESSION $(H2MUX_INC) -o $(BUILD)/h2mux_active_ctl $(H2MUX_SRC)
	@$(BUILD)/h2mux_active_ctl >$(BUILD)/h2mux_active_ctl.log 2>&1; rc=$$?; \
	 test $$rc -eq 1 && grep -m1 'active session was recycled before its owner released it' $(BUILD)/h2mux_active_ctl.log

# Two old behaviours reintroduced by name: bfetch cannot join a negotiated h2
# connection before its new-dial gate, and the last request handle closes an
# otherwise healthy h2 connection.  The first strands a resource forever; the
# second costs one TCP+TLS connection per serial resource batch.  This control
# is separate from BXFER_H1_ONLY because disabling the entire protocol cannot
# tell us that either h2-specific lifetime assertion is watching the intended
# branch.  Both named assertions must be seen failing, not merely a non-zero
# process status from some unrelated test.
test-h2mux-reuse-control: h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DBFETCH_NO_H2_JOIN -DBXFER_DROP_IDLE_H2 $(H2MUX_INC) \
	    -o $(BUILD)/h2mux_reuse_ctl $(H2MUX_SRC)
	@if $(BUILD)/h2mux_reuse_ctl >$(BUILD)/h2mux_reuse_ctl.log 2>&1; then \
	    echo "test-h2mux-reuse-control: FAIL -- both removed behaviours escaped detection"; \
	    exit 1; \
	elif ! grep -q 'bfetch stayed queued behind the h2 connection' $(BUILD)/h2mux_reuse_ctl.log; then \
	    echo "test-h2mux-reuse-control: FAIL -- ready-h2 join assertion did not fail"; \
	    grep -m8 '^FAIL' $(BUILD)/h2mux_reuse_ctl.log || true; exit 1; \
	elif ! grep -q 'two sequential h2 resources opened' $(BUILD)/h2mux_reuse_ctl.log; then \
	    echo "test-h2mux-reuse-control: FAIL -- idle-h2 lifetime assertion did not fail"; \
	    grep -m8 '^FAIL' $(BUILD)/h2mux_reuse_ctl.log || true; exit 1; \
	else \
	    echo "test-h2mux-reuse-control: PASS -- both removed behaviours are caught. They failed on:"; \
	    grep 'bfetch stayed queued behind\|two sequential h2 resources opened' \
	        $(BUILD)/h2mux_reuse_ctl.log; \
	fi

# Keep HTTP/2 enabled and restore only the old admission mistake.  The cap=1
# fixture must then fail on "failed instead of waiting" specifically; a generic
# non-zero exit (or the H1-only control) cannot prove this scheduler boundary.
test-h2mux-slot-control: h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DBXFER_IGNORE_H2_STREAM_CAP $(H2MUX_INC) \
	    -o $(BUILD)/h2mux_slot_ctl $(H2MUX_SRC)
	@if $(BUILD)/h2mux_slot_ctl >$(BUILD)/h2mux_slot_ctl.log 2>&1; then \
	    echo "test-h2mux-slot-control: FAIL -- ignoring peer stream caps escaped detection"; \
	    exit 1; \
	elif ! grep -q 'peer-cap request failed instead of waiting' $(BUILD)/h2mux_slot_ctl.log; then \
	    echo "test-h2mux-slot-control: FAIL -- cap assertion was not the failure"; \
	    grep -m8 '^FAIL' $(BUILD)/h2mux_slot_ctl.log || true; exit 1; \
	else \
	    echo "test-h2mux-slot-control: PASS -- ignoring the peer cap is caught. It failed on:"; \
	    grep -m1 'peer-cap request failed instead of waiting' $(BUILD)/h2mux_slot_ctl.log; \
	    grep -m1 'pending peer-cap handle rejected instead of waiting' $(BUILD)/h2mux_slot_ctl.log; \
	fi
