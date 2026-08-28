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
H2MUX_HTTP_OUT := c/net/http/cookies.c
H2MUX_SRC := tests/unit/h2mux_test.c c/apps/browser/browser_rt.c \
             $(filter-out $(H2MUX_HTTP_OUT),$(filter c/net/http/%,$(BROWSER_PIPE)))

.PHONY: test-h2mux test-h2mux-control test-h2mux-asan h2mux-link-check

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

test-h2mux: h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w $(H2MUX_INC) -o $(BUILD)/h2mux_test $(H2MUX_SRC)
	@$(BUILD)/h2mux_test

# ASan matters more here than in most places: bxfer hands ONE connection to
# several exchanges and frees the serialized request only once the borrowed
# body has stopped being read, so the failure mode is a use-after-free that a
# passing functional test cannot see.
test-h2mux-asan: h2mux-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all \
	    $(H2MUX_INC) -o $(BUILD)/h2mux_asan $(H2MUX_SRC)
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/h2mux_asan

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
	    echo "test-h2mux-control: PASS -- disabling h2 is caught. It failed on:"; \
	    grep -m6 '^FAIL' $(BUILD)/h2mux_ctl.log || true; \
	    grep -c '^FAIL' $(BUILD)/h2mux_ctl.log | sed 's/^/  total failures: /'; \
	fi
