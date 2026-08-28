# WebSocket (RFC 6455) -- the frame codec (c/net/http/ws.c) and the browser's
# `WebSocket` (c/apps/browser/js_websocket.c), host-tested together against
# an in-memory server. See tests/unit/ws_test.c's own header for exactly what
# is and is not graded here, and js_websocket.c's file header for the
# termination argument every state transition is held to.
#
# Lives in its own fragment for the same reason tests/http2.mk does: several
# lines are editing the main Makefile today, and BROWSER_PIPE already carries
# the three names this needs (ws.c, base64.c, sha1.c) -- see that list's own
# comment for why they ride together.
ifeq ($(strip $(BROWSER_PIPE)),)
WS_LINK_ERR := tests/ws.mk: BROWSER_PIPE is empty -- this fragment must be -included from the Makefile, AFTER it. Refusing to link a WebSocket test from a partial source list.
endif

WS_INC := -Ic/net/http -Ic/net/ssh -Ic/crypto -Ic/apps/browser -Iinclude/abi \
          -Ithird_party/quickjs -Ithird_party/libm -DWEBAPI_HOST -DCONFIG_VERSION='"host"'
WS_SRC := tests/unit/ws_test.c c/apps/browser/js_websocket.c c/net/http/ws.c \
          c/net/http/http1.c c/net/ssh/base64.c c/crypto/hash/sha1.c $(QJS_SRC)

.PHONY: test-ws test-ws-mask-negctl test-ws-accept-negctl test-ws-settle-negctl ws-link-check test-wpt-ws wpt-ws-baseline

# The two questions every BROWSER_PIPE-derived list reduces to (see
# tests/http2.mk's own check for the fuller argument): is ws.c/base64.c/
# sha1.c still there, and are they still real files. Both are structurally
# true here (WS_SRC names them directly rather than filtering BROWSER_PIPE),
# so this only has to catch the second: a rename that left a stale path.
ws-link-check:
	@if [ -n "$(WS_LINK_ERR)" ]; then echo "$(WS_LINK_ERR)"; exit 1; fi
	@for f in c/net/http/ws.c c/net/ssh/base64.c c/crypto/hash/sha1.c; do \
	    case " $(BROWSER_PIPE) " in *" $$f "*) ;; \
	    *) echo "ws-link-check: FAIL -- $$f is no longer in BROWSER_PIPE, so the"; \
	       echo "  browser does not link it even though this test still does."; exit 1;; \
	    esac; done

# test-ws names all three negctls as prerequisites so tools/audit_tests.py's
# find_stranded_controls() sees them as invoked (rule 5 -- a control nobody
# runs is worse than no control; these three were stranded exactly like that
# until this line: excluded from CI by NOT_CI's "-negctl" pattern on the
# unchecked assumption that a positive counterpart runs them, and named by
# nothing, which is invisible from both ends at once).
test-ws: ws-link-check test-ws-mask-negctl test-ws-accept-negctl test-ws-settle-negctl
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w $(WS_INC) -o $(BUILD)/ws_test $(WS_SRC) -lm
	@$(BUILD)/ws_test

# THE NEGATIVE CONTROLS. Each rebuilds the SAME test with ONE protocol check
# compiled out and requires the suite to go RED specifically because of it --
# a control nobody has watched fail is not a known-failing control (rule 5).

# RFC 6455 5.1: "a client MUST mask all frames it sends to the server."
# -DWS_NO_MASK sends unmasked frames from js_websocket.c's own outgoing path;
# the in-memory server's parser (require_masked=1, unmodified) must refuse
# the very first frame after the handshake, which tears down the connection
# before ws.send('hello there') is ever echoed back -- test_open_message_close
# is what catches it.
test-ws-mask-negctl: ws-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DWS_NO_MASK $(WS_INC) -o $(BUILD)/ws_test_nomask $(WS_SRC) -lm
	@if $(BUILD)/ws_test_nomask >$(BUILD)/ws_test_nomask.log 2>&1; then \
	    echo "test-ws-mask-negctl: FAIL -- the suite passed with outgoing masking"; \
	    echo "  disabled, which means test-ws is not measuring RFC 6455 5.1 at all."; \
	    exit 1; \
	  else \
	    echo "test-ws-mask-negctl: PASS -- disabling client masking is caught. It failed on:"; \
	    grep '^FAIL' $(BUILD)/ws_test_nomask.log | sed 's/^/    /'; \
	  fi

# The Sec-WebSocket-Accept check is the ONE thing standing between "the
# handshake response we got" and "any 101 response, from anyone" -- see
# js_websocket.c's file header. -DWS_ACCEPT_ANY skips it; test_bad_accept
# (which deliberately corrupts the accept value the in-memory server sends)
# must now see an `open` it should not, which is what the assertion there
# checks for explicitly (`!strstr(s, "\"open\"")`).
test-ws-accept-negctl: ws-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DWS_ACCEPT_ANY $(WS_INC) -o $(BUILD)/ws_test_anyaccept $(WS_SRC) -lm
	@if $(BUILD)/ws_test_anyaccept >$(BUILD)/ws_test_anyaccept.log 2>&1; then \
	    echo "test-ws-accept-negctl: FAIL -- the suite passed with Sec-WebSocket-Accept"; \
	    echo "  verification disabled, which means test-ws is not measuring it at all."; \
	    exit 1; \
	  else \
	    echo "test-ws-accept-negctl: PASS -- skipping Accept verification is caught. It failed on:"; \
	    grep '^FAIL' $(BUILD)/ws_test_anyaccept.log | sed 's/^/    /'; \
	  fi

# THE TERMINATION CONTROL. -DWS_NO_SETTLE stubs ws_fail_connect/
# ws_finish_close to do nothing -- a connection that reaches a terminal
# condition and never fires open/error/close, the exact shape js_platform.h
# names as worse than not shipping the feature at all. This build's own
# main() runs test_no_settle_control, whose assertion is written backwards
# on purpose (asserting an event DID fire) so that build's run prints FAIL --
# the target below just has to observe that it does.
test-ws-settle-negctl: ws-link-check
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DWS_NO_SETTLE $(WS_INC) -o $(BUILD)/ws_test_nosettle $(WS_SRC) -lm
	@if $(BUILD)/ws_test_nosettle >$(BUILD)/ws_test_nosettle.log 2>&1; then \
	    echo "test-ws-settle-negctl: FAIL -- a build that never fires a terminal event"; \
	    echo "  still reported all-green, which means nothing here would catch a real hang."; \
	    exit 1; \
	  else \
	    echo "test-ws-settle-negctl: PASS -- a connection that never settles is caught (no hang -- the pump loop is bounded). It failed on:"; \
	    grep '^FAIL' $(BUILD)/ws_test_nosettle.log | sed 's/^/    /'; \
	  fi

# --- test-wpt-ws: the WPT subset, js_websocket.c linked (it rides
# WPT_TEST_SRC's own $(wildcard c/apps/browser/js_*.c) automatically -- no
# source list to keep in sync, same as test-wpt-idb / test-wpt-worker).
#
# BEFORE THIS TARGET EXISTED, NOTHING IN THIS TREE WOULD GO RED ON A
# WEBSOCKET WPT REGRESSION: websockets/ is not one of the directories
# tests/unit/wpt_expected_fail.txt covers (grep -c '^websockets/' on it is
# 0), and there was no wpt_ws_fail.txt the way IndexedDB and workers each
# have one. wpt-ws-baseline writes tests/unit/wpt_ws_fail.txt against
# build/wpt-full; test-wpt-ws checks against it and --strict fails on a NEW
# failure or a regression below what already passes.
#
# THE CEILING IS THE APPARATUS, NOT THE PROTOCOL, and it is worth stating
# next to the target that measures it: 64 of 212 websockets/ files pull in
# constants.sub.js, whose `{{host}}`/`{{ports[ws][0]}}` are never substituted
# because there is no wptserve here -- confirmed to FAIL FAST rather than
# hang (websockets/Close-1000.any.js completes and reports a clean
# "Connection should be closed" failure). So this ratchet mostly protects the
# subset judgeable without a live server: constructor validation, synchronous
# throws, blocked ports, IDL shape. It is real regression protection for
# that subset, not a claim about wire-protocol coverage -- ws_test.c's
# in-memory-server suite above is where masking/framing/close-handshake are
# actually graded, against RFC 6455's own byte vectors.
WS_BASELINE := tests/unit/wpt_ws_fail.txt
WS_WPT_ROOT := $(if $(wildcard build/wpt-full/websockets),build/wpt-full,$(WPT_ROOT))

test-wpt-ws: $(BUILD)/wpt_test
	@if [ ! -d $(WS_WPT_ROOT)/websockets ]; then \
	    echo "test-wpt-ws: SKIPPED (no websockets/ under $(WS_WPT_ROOT) -- run 'make wpt-fetch' or fetch build/wpt-full)"; exit 0; fi
	@$(BUILD)/wpt_test --root $(WS_WPT_ROOT) --subset websockets -b $(WS_BASELINE) \
	    $(if $(V),-v $(V),) $(if $(STRICT),--strict,)

wpt-ws-baseline: $(BUILD)/wpt_test
	@if [ ! -d $(WS_WPT_ROOT)/websockets ]; then \
	    echo "wpt-ws-baseline: SKIPPED (no websockets/ under $(WS_WPT_ROOT))"; exit 0; fi
	@$(BUILD)/wpt_test --root $(WS_WPT_ROOT) --subset websockets -b $(WS_BASELINE) --write-baseline

# Wired onto ci-host so tools/ci.sh's recipe-derived host/boot split picks
# both up -- test-ws was reachable from nowhere before this line (not even a
# ci-host: mention), which is a real gap distinct from the "UNWIRED just
# means not in the hand-written aggregate" note in CLAUDE.md.
ci-host: test-ws test-wpt-ws
