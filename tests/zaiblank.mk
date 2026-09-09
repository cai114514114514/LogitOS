# tests/zaiblank.mk -- the z.ai BLANK specimen: its deaths, fixed and gated.
#
# In its own fragment for the reason every other tests/*.mk here gives: several
# lines edit the tree at once and a fragment is the only way to add targets
# without a commit sweeping somebody else's half-finished work. Owned by the
# zaiblank wave-1 agent (2026-08-30); the -include line landed pre-wired and
# empty at fe7a8141e.
#
# WHAT THE GATES GATE, AND WHERE THE DEFECTS ACTUALLY WERE
#
# The owner reported z.ai renders NOTHING AT ALL. The specimen (captured
# host-side with the browser's own honest UA, see tests/fixtures/zaiblank/) was
# replayed in the guest with its real subresources served locally
# (qmp_zaiblank_page.py, this package's driver), and died twice:
#
#   1. BLANK-after-JS-death. The 3.2 MB entry module rejected at evaluation:
#        [browser] module rejected .../index-B9hfiqvt.js:
#          ReferenceError: 'BroadcastChannel' is not defined
#      The bundle's multi-tab "active-tab-channel" lock instantiates the
#      channel at module init -- one statement, whole page gone. Fixed in
#      js_platform.c (spec IDL, same-context delivery, clone at call time,
#      silent no-op after close). Verified in the guest: the module then
#      boots, dynamic-imports its auth and error route components, and the
#      SPA's own session/config handling runs ("rendering as guest").
#      The two crypto.subtle / ServiceWorker candidates from the brief were
#      measured and CLEARED: zero references in the bundle.
#
#   2. The page's /api/config and /api/v1/auths fetches rejected
#      "fetch: timed out" with the server NEVER CONSULTED (the replay driver
#      records every 404 its server answers: none arrived). Mechanism, read
#      off the serial timeline: js_module.c compiles the whole module graph
#      inside one JS_Eval (its own header says why), which on TCG blocked the
#      single page loop for ~50 s; the fetches' 30 s idle deadline kept
#      running in wall time and failed on the first pump after the block. A
#      network-fault report for a condition the network caused none of. Fixed
#      in js_webapi.c: the deadline charges SERVICED time only (a gap between
#      successive steps > 250 ms is loop-blocked time, and the connection
#      cannot idle while nobody polls it).
#
# WHAT REMAINS, HONESTLY (the gate does not assert it away): after both fixes
# the SPA's mount does not converge -- it churns allocation at full CPU
# (measured host-side through webapi_probe: 99% CPU, 3+ minutes, no
# completion; malloc + QuickJS property creation dominate, LibCSS media-query
# parsing rides along). At TCG speed that is a de-facto freeze: no exception,
# no paint, no timer -- the "silent freeze" class. That is a separate defect
# (reactive-loop/webperf shape, likely needing its own package) and the guest
# gate below asserts only what these two fixes own, not "the site works".
#
#   make test-zaiblank                 everything below, negctls first
#   make test-zaiblank-host            BroadcastChannel + fetch deadline, host
#   make test-zaiblank-bc-negctl       host control: BC compiled out, section red
#   make test-zaiblank-fetch-negctl    host control: deadline fix compiled out
#   make test-zaiblank-guest           the z.ai specimen replayed in the guest
#   make test-zaiblank-guest-negctl    the same against a BC-less browser build
#
# NOT on ci- lines: two QEMU boots for the guest pair. Run it across a change
# to js_platform.c's message-queue family, js_webapi.c's fetch stack, or
# js_module.c's loader (which owns the blocking compile that made defect 2
# reachable).

.PHONY: test-zaiblank test-zaiblank-host test-zaiblank-bc-negctl \
        test-zaiblank-fetch-negctl test-zaiblank-guest test-zaiblank-guest-negctl

# --- the host half ----------------------------------------------------------
# PLATFORM_TEST_SRC (tests/webapi_platform.mk, included at Makefile:3365,
# before this fragment at :5085) is the browser-JS-surface subtraction list
# this tree already maintains; re-deriving it here would be the hand-copied
# source list CLAUDE.md warns about. Swap the one file that is ours, and the
# three TUs that list subtracts back in (PLATFORM_MOD) the same way its own
# positive link does -- minus nothing else.
ZAIBLANK_TEST_SRC := $(filter-out tests/unit/webapi_platform_test.c,$(PLATFORM_TEST_SRC)) \
                     tests/fixtures/zaiblank/zaiblank_test.c
ZAIBLANK_CF := $(PLATFORM_CF)

test-zaiblank-host: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(ZAIBLANK_CF) -o $(BUILD)/zaiblank_test $(ZAIBLANK_TEST_SRC) \
	    c/apps/browser/js_platform.c c/apps/browser/js_select.c c/apps/browser/js_intl.c \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/zaiblank_test

# Control 1: BroadcastChannel compiled out of js_platform.c
# (-DZAIBLANK_BC_ABSENT, the JS_DOCWRITE_NO_INSTALL idiom). $(BROWSER_JS_CF),
# not $(JS_CF), for the reason fragmk.mk states at its own negmod rule: the
# browser's own TUs must compile with the target-specific flags the root
# Makefile pins for them.
$(BUILD)/nbc/js_platform.o: c/apps/browser/js_platform.c
	@mkdir -p $(dir $@)
	$(CC) $(BROWSER_JS_CF) -DZAIBLANK_BC_ABSENT -c $< -o $@

test-zaiblank-bc-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(ZAIBLANK_CF) -o $(BUILD)/zaiblank_bc_control $(ZAIBLANK_TEST_SRC) \
	    $(BUILD)/nbc/js_platform.o c/apps/browser/js_select.c c/apps/browser/js_intl.c \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@out=`$(BUILD)/zaiblank_bc_control --control 2>&1`; rc=$$?; \
	 echo "$$out"; \
	 fails=`echo "$$out" | grep -c '^FAIL:'`; \
	 oks=`echo "$$out" | grep -c '^ok  :'`; \
	 if [ "$$fails" -lt 3 ] || [ "$$oks" -lt 2 ]; then \
	   echo "test-zaiblank-bc-negctl: FAILED -- expected the BroadcastChannel section red (>=3 FAILs) with the MessageEvent/MessageChannel checks still green, saw $$fails FAIL / $$oks ok"; exit 1; \
	 fi; \
	 echo "test-zaiblank-bc-negctl: red as designed -- $$fails FAIL / $$oks ok (BroadcastChannel checks fail without the feature; the not-this-feature checks pass, as the control file states)"

# Control 2: the fetch-deadline compensation compiled out. Check 2a (blocked
# time forgiven) must FAIL; check 2b (honest timeout kept) must PASS -- a
# control that reddens everything is measuring nothing.
$(BUILD)/nfd/js_webapi.o: c/apps/browser/js_webapi.c
	@mkdir -p $(dir $@)
	$(CC) $(ZAIBLANK_CF) -DZAIBLANK_FETCH_DEADLINE_OLD -c $< -o $@

# js_webapi.c is normally reached through the subtraction list; take it out
# and put the old-behaviour object in its place (one file, stated).
ZAIBLANK_FETCH_SRC := $(filter-out c/apps/browser/js_webapi.c,$(ZAIBLANK_TEST_SRC)) \
                      $(BUILD)/nfd/js_webapi.o

test-zaiblank-fetch-negctl: $(BUILD)/nfd/js_webapi.o $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(ZAIBLANK_CF) -o $(BUILD)/zaiblank_fetch_control $(ZAIBLANK_FETCH_SRC) \
	    c/apps/browser/js_platform.c c/apps/browser/js_select.c c/apps/browser/js_intl.c \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/zaiblank_fetch_control --fetch-only > $(BUILD)/zaiblank_fetch_negctl.log 2>&1 || true
	@cat $(BUILD)/zaiblank_fetch_negctl.log
	@if grep -q "FAIL: a fetch is NOT failed for loop-blocked time" $(BUILD)/zaiblank_fetch_negctl.log; then \
	   if grep -q "FAIL: serviced idle time still times out" $(BUILD)/zaiblank_fetch_negctl.log; then \
	     echo "test-zaiblank-fetch-negctl: FAILED -- the honest-timeout check went red too; this control is broken"; exit 1; \
	   fi; \
	   echo "test-zaiblank-fetch-negctl: red as designed -- blocked-time check fails with the compensation compiled out, honest-timeout check stays green"; \
	 else \
	   echo "test-zaiblank-fetch-negctl: FAILED -- the blocked-time check passed with the fix compiled out; it is measuring something else"; exit 1; \
	 fi

# --- the guest half ---------------------------------------------------------
# The specimen replay: the real z.ai shell + its real captured bundle, served
# from a local server by tests/qmp/qmp_zaiblank_page.py. Assertions are on the
# CONCRETE signals the two fixes enable, not on "the site works" (see the
# header): the BroadcastChannel rejection is gone and the /api fetches reach
# the server's 404 log -- before the fixes the module rejected and the fetches
# never left the machine.
ZAIBLANK_PAGE := tests/qmp/qmp_zaiblank_page.py
ZAIBLANK_FIXTURE := tests/fixtures/zaiblank/zai

test-zaiblank-guest: test-zaiblank-guest-negctl
test-zaiblank-guest: $(ISO) $(DISK)
	@python3 $(ZAIBLANK_PAGE) --iso $(ISO) --disk $(DISK) \
	    --fixture $(ZAIBLANK_FIXTURE) --name zai --out $(BUILD)/zaiblank_guest.json \
	    --serial $(BUILD)/zaiblank_guest.serial.txt > $(BUILD)/zaiblank_guest.log 2>&1 || true
	@cat $(BUILD)/zaiblank_guest.json
	@python3 tests/fixtures/zaiblank/zaiblank_guest_check.py $(BUILD)/zaiblank_guest.json

# The device negative control: the SAME replay against a browser.aex linked
# with -DZAIBLANK_BC_ABSENT. The BroadcastChannel rejection must COME BACK --
# that is what proves the guest gate measures this feature and not some other
# reason a replay happens to boot. (The fetch fix stays in this build; the
# control isolates one variable, the same discipline as fragmk's negmod.)
NBC_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_platform.o,$(BROWSER_JS_OBJ)) \
              $(BUILD)/nbc/js_platform.o

$(BUILD)/browser-nbc.elf: $(ENGINE_OBJ) $(NBC_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group \
	    $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NBC_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-nbc.aex: $(BUILD)/browser-nbc.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-nbc.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

test-zaiblank-guest-negctl: $(ISO) $(BUILD)/browser-nbc.aex
	@$(MAKE) DISK=$(BUILD)/disk-zai-nbc.img BROWSER_AEX=$(BUILD)/browser-nbc.aex $(BUILD)/disk-zai-nbc.img
	@python3 $(ZAIBLANK_PAGE) --iso $(ISO) --disk $(BUILD)/disk-zai-nbc.img \
	    --fixture $(ZAIBLANK_FIXTURE) --name zai-nbc --out $(BUILD)/zaiblank_nbc.json \
	    --serial $(BUILD)/zaiblank_nbc.serial.txt > $(BUILD)/zaiblank_nbc.log 2>&1 || true
	@cat $(BUILD)/zaiblank_nbc.json
	@python3 tests/fixtures/zaiblank/zaiblank_guest_negctl_check.py $(BUILD)/zaiblank_nbc.json

test-zaiblank: test-zaiblank-host test-zaiblank-bc-negctl test-zaiblank-fetch-negctl \
               test-zaiblank-guest
