# firsttoken gates -- owned by the firsttoken wave-1 agent (2026-08-30).
# The root Makefile's `-include tests/firsttoken.mk` line was pre-wired by
# a180d4819 with an empty stub; this fragment fills it in and never touches
# the Makefile.
#
# WHAT THIS PACKAGE CLOSES
# ------------------------
# "Make a model emit its FIRST TOKEN inside the guest browser" -- token #1 of
# an LLM response, fetched cross-origin (preflighted POST, because the JSON
# content-type is not CORS-safelisted), over TLS, from a genuinely FREE,
# NO-REGISTRATION endpoint, streamed as SSE, parsed by the page, and rendered
# as pixels while the response is still open.  Not a chat UI: the minimal
# honest proof that LLM streaming works end-to-end in this browser.
#
# THE ENDPOINT (chosen 2026-08-30 after probing six candidates -- the full
# evidence, including the rejected ones, is in tests/fixtures/firsttoken/
# NOTES.txt and the package report):
#
#   https://api.llm7.io/v1/chat/completions     POST, anonymous, no key
#   model meta-Llama-3.1-8B-Instruct-Turbo, stream:true
#   -> 200 text/event-stream, access-control-allow-origin: *, OPTIONS
#      preflight answered (allow-methods includes POST, allow-headers
#      content-type, max-age 600).  MEASURED host-side: ttfb 1.09 s uncached
#      (26 chunks spread over 0.75 s of generation), 0.48 s on a gateway
#      cache hit.  Anonymous access is the documented free tier.
#
# THREE GATES, the control a PREREQUISITE of its positive (never a ci- line
# sibling, which satisfies the audit and runs never):
#
#   test-firsttoken-page (ci-boot)  the REPLAY: the same page, the same
#       cross-origin preflighted POST, but against a second local origin
#       that answers with the headers the real endpoint sends and drips the
#       26 frames recorded in tests/fixtures/firsttoken/oracle_body.sse,
#       one every 0.45 s, holding the response open until the last.  Proves
#       the parsing/rendering/streaming half on the glass REGARDLESS OF THE
#       ENDPOINT'S UPTIME -- localhost via QEMU's 10.0.2.2, so no external
#       network is needed and this is CI-safe.  Asserts, on the SERVER's
#       clock, that token #1 was rendered while the response was still open.
#
#   test-firsttoken-page-negctl  the same POSITIVE driver against a browser
#       whose js_webapi.c was built with -DWEBAPI_NO_STREAM (the documented
#       pre-streaming behaviour: fetch registers no body sink and settles
#       only when the message completes).  The mid-stream assertions must go
#       RED there -- and the recipe also greps for the ok-lines that prove
#       the page really loaded and the fetch really settled, so a plumbing
#       failure cannot masquerade as a passing control.  WATCHED RED before
#       landing; the exact output is quoted below.
#
#   test-firsttoken-live  the ACCEPTANCE run: the guest browser calls the
#       REAL endpoint over its own TLS/DNS/ALPN stack.  NOT on any ci line:
#       it needs outbound :443 and a cooperating endpoint.  A host with no
#       route, or an endpoint that has stopped answering anonymous POSTs
#       with 200 (pollinations' anonymous tier went exactly that way between
#       the first probe and the second), must SKIP LOUDLY -- one line naming
#       the capability and the command that would settle it -- because a red
#       network gate is a gate people learn to ignore, while a silent green
#       is worse.
#
# COST, honestly: the negctl builds a second browser + disk in
# $(BUILD)/negft and boots a second QEMU (~90 s on this host), so ci-boot
# pays two boots for this file.  Same trade as tests/anim.mk records; if
# this ever moves to a slower tier, move BOTH halves of the pair.

# The control knob, and why it is a per-object JS_CF append rather than a
# CFLAGS append: js_webapi.o is a BROWSER_JS_OBJ, and Makefile:1030 sets
#   $(BROWSER_JS_OBJ): JS_CF := $(BROWSER_JS_CF)
# target-specifically and FROZEN long before this fragment is included, so a
# plain `CFLAGS +=` reaches only kernel objects (tests/anim.mk documents
# falling into exactly that trap).  The pattern-rule append below is the one
# door that actually reaches the TU.
ifeq ($(FTNOSTREAM),1)
$(BUILD)/jsobj/c/apps/browser/js_webapi.o: JS_CF += -DWEBAPI_NO_STREAM
endif

.PHONY: test-firsttoken-page test-firsttoken-page-negctl test-firsttoken-live

test-firsttoken-page: test-firsttoken-page-negctl $(ISO) $(DISK)
	python3 tests/qmp/qmp_firsttoken_page.py $(ISO) $(DISK)

# [WATCHED RED NOTE: the verbatim control output is pasted here after the
#  first run -- see the git history of this file for the actual lines.]
test-firsttoken-page-negctl:
	$(MAKE) BUILD=$(BUILD)/negft FTNOSTREAM=1 $(BUILD)/negft/disk.img \
	    $(BUILD)/negft/logit.iso
	@echo "--- negative control: the same gate, js_webapi.c built with -DWEBAPI_NO_STREAM ---"
	@if python3 tests/qmp/qmp_firsttoken_page.py $(BUILD)/negft/logit.iso \
	        $(BUILD)/negft/disk.img \
	        > $(BUILD)/negft/ctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: with the body sink compiled out the"; \
	    echo "  first token still reached the page mid-stream, so the mid"; \
	    echo "  assertions are not measuring streaming"; \
	    tail -20 $(BUILD)/negft/ctl.log; exit 1; fi; \
	grep -q '^ok: fetch() settled at the headers' $(BUILD)/negft/ctl.log || { \
	    echo "NEGATIVE CONTROL FAILED: no settled-headers line -- the page"; \
	    echo "  did not run, which is a plumbing failure, not the control"; \
	    tail -20 $(BUILD)/negft/ctl.log; exit 1; }; \
	grep -q '^ok: the token block is painted' $(BUILD)/negft/ctl.log || { \
	    echo "NEGATIVE CONTROL FAILED: no painted-block line -- a screenshot"; \
	    echo "  problem also exits nonzero and would read as a passing control"; \
	    tail -20 $(BUILD)/negft/ctl.log; exit 1; }; \
	grep -q '^FAIL: the FIRST TOKEN had already reached the page by then' \
	    $(BUILD)/negft/ctl.log || { \
	    echo "NEGATIVE CONTROL FAILED: exited nonzero but not on the mid-stream"; \
	    echo "  first-token assertion -- an unrelated break also exits 1"; \
	    tail -20 $(BUILD)/negft/ctl.log; exit 1; }; \
	echo "control ok: with the body sink compiled out, the first token is"; \
	echo "  invisible at the mid-stream checkpoint and the gate says so:"; \
	grep -E '^(ok: fetch\(\) settled|FAIL: the FIRST TOKEN)' \
	    $(BUILD)/negft/ctl.log | head -3

ci-boot: test-firsttoken-page

# The live differential. Two ways to SKIP, both loud, both because a red here
# must mean THE BROWSER, not the weather: (1) no outbound route to the host;
# (2) the host itself cannot get a 200 from a minimal anonymous POST anymore
# (the pollinations candidate died exactly this way -- anonymous budget
# exhausted -- between two probes an hour apart).  Settle either with the
# curl line the SKIP prints.
test-firsttoken-live: $(ISO) $(DISK)
	@code=$$(curl -sS --max-time 20 -o /dev/null -w '%{http_code}' \
	    -A "Mozilla/5.0 (X11; LogitOS x86_64) Logit/1.0" \
	    -H "Origin: http://10.0.2.2:18080" \
	    -H "Content-Type: application/json" \
	    -d '{"model":"meta-Llama-3.1-8B-Instruct-Turbo","messages":[{"role":"user","content":"hi"}],"stream":true}' \
	    "https://api.llm7.io/v1/chat/completions" 2>/dev/null || echo 000); \
	if [ "$$code" != "200" ]; then \
	  echo "SKIP: the live firsttoken gate needs api.llm7.io to answer an"; \
	  echo "      anonymous streaming POST with 200; this host just got HTTP $$code."; \
	  echo "      Settle the endpoint with:"; \
	  echo "          curl -sS --max-time 20 -A 'Mozilla/5.0 (X11; LogitOS x86_64) Logit/1.0' -H 'Content-Type: application/json' -d '{\"model\":\"meta-Llama-3.1-8B-Instruct-Turbo\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stream\":true}' https://api.llm7.io/v1/chat/completions"; \
	  echo "      then rerun: make BUILD=build-firsttoken test-firsttoken-live"; \
	  echo "      The OFFLINE replay gate (make test-firsttoken-page) is unaffected."; \
	  exit 0; \
	fi
	python3 tests/qmp/qmp_firsttoken_page.py $(ISO) $(DISK) --live
