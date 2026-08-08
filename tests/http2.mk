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
H2MUX_SRC := tests/unit/h2mux_test.c c/apps/browser/browser_rt.c \
             c/net/http/http1.c c/net/http/http2.c c/net/http/hpack.c \
             c/net/http/hpool.c c/net/http/url.c

.PHONY: test-h2mux test-h2mux-control test-h2mux-asan

test-h2mux:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w $(H2MUX_INC) -o $(BUILD)/h2mux_test $(H2MUX_SRC)
	@$(BUILD)/h2mux_test

# ASan matters more here than in most places: bxfer hands ONE connection to
# several exchanges and frees the serialized request only once the borrowed
# body has stopped being read, so the failure mode is a use-after-free that a
# passing functional test cannot see.
test-h2mux-asan:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all \
	    $(H2MUX_INC) -o $(BUILD)/h2mux_asan $(H2MUX_SRC)
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/h2mux_asan

# The negative control. Same test file, same server; the only difference is
# that browser_rt.c never offers h2 in ALPN, so there is no h2 connection for
# four requests to share. It MUST fail, and it must fail on the multiplexing
# assertions specifically -- an assertion nobody has watched fail is not a
# known-failing assertion.
test-h2mux-control:
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
