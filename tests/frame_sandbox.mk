# tests/frame_sandbox.mk -- HTML iframe sandboxing (frame/sandbox.h).
#
# A sandboxed <iframe> used to be refused WHOLE: no network frame, and a
# SecurityError "Blocked access to a sandboxed frame" on every contentWindow
# read (16 of them on one chatgpt.com load, because Cloudflare's widget iframe
# carries sandbox="allow-same-origin allow-scripts allow-popups"). Frames now
# load with the HTML sandboxing flag set enforced. Three gates:
#
#   test-frame-sandbox-parse   host: the token parser against the spec's flag
#                              table (unknown/duplicate tokens, ASCII-only case
#                              folding, the CSP directive's first-wins/union).
#   test-frame-sandbox-host    host: the real browser (app_main, frame loader,
#                              child runtimes) over mocked HTTP, three modes.
#   test-frame-sandbox-guest   QEMU: a real two-origin page with four sandboxed
#                              network frames, observed off the serial log and
#                              the host server's own request log.
#
# Each has a control that was watched failing for the reason it names. The
# about:blank/srcdoc half (platform.c) is gated by test-frame-bootstrap-wiring,
# whose fixture now asserts the flag semantics and whose two controls
# reproduce the two historical bypass shapes.

FRAME_SANDBOX_DIR := $(BUILD)/frame-sandbox
FRAME_SANDBOX_TEST_SRC := tests/unit/frame_sandbox_test.c
FRAME_SANDBOX_TEST_DEP := $(FRAME_SANDBOX_TEST_SRC) c/apps/gui/browser/frame/sandbox.h tests/frame_sandbox.mk

$(FRAME_SANDBOX_DIR)/parse: $(FRAME_SANDBOX_TEST_DEP)
	@mkdir -p $(FRAME_SANDBOX_DIR)
	@$(CC) -O1 -Wall -Wextra -Werror -Ic/apps/gui/browser -o $@ $(FRAME_SANDBOX_TEST_SRC)
$(FRAME_SANDBOX_DIR)/parse-case-negctl: $(FRAME_SANDBOX_TEST_DEP)
	@mkdir -p $(FRAME_SANDBOX_DIR)
	@$(CC) -O1 -Wall -Wextra -Werror -Ic/apps/gui/browser -DSANDBOX_NEGCTL_CASE_SENSITIVE -o $@ $(FRAME_SANDBOX_TEST_SRC)

# The control compares keywords byte-for-byte. Exactly the three checks that
# spell a keyword or the directive name in another ASCII case must fail; the
# Unicode non-folding checks must still pass (they pass BECAUSE nothing folds).
.PHONY: test-frame-sandbox-parse test-frame-sandbox-parse-negctl
test-frame-sandbox-parse-negctl: $(FRAME_SANDBOX_DIR)/parse-case-negctl
	@rc=0; $< > $(FRAME_SANDBOX_DIR)/parse-case-negctl.log 2>&1 || rc=$$?; \
	 if [ $$rc -eq 1 ] && grep -q '^FAIL: upper-case keyword ' $(FRAME_SANDBOX_DIR)/parse-case-negctl.log && \
	    grep -q '^FAIL: mixed-case keywords ' $(FRAME_SANDBOX_DIR)/parse-case-negctl.log && \
	    grep -q '^FAIL: directive name is ASCII case-insensitive ' $(FRAME_SANDBOX_DIR)/parse-case-negctl.log && \
	    [ "$$(grep -c '^FAIL' $(FRAME_SANDBOX_DIR)/parse-case-negctl.log)" = 3 ]; then \
		echo "negative control ok: case-sensitive keywords fail exactly the three ASCII case checks"; \
	 else cat $(FRAME_SANDBOX_DIR)/parse-case-negctl.log; echo "NEGATIVE CONTROL FAILED"; exit 1; fi
test-frame-sandbox-parse: test-frame-sandbox-parse-negctl $(FRAME_SANDBOX_DIR)/parse
	@$(FRAME_SANDBOX_DIR)/parse
ci-host: test-frame-sandbox-parse

# ---- test-frame-sandbox-host: the real browser, not a fake -----------------
# tests/unit/passive_frame_test.c runs the production app_main, network frame
# loader, child page/WebAPI/platform runtimes and message bridge over a mocked
# HTTP transport. Three modes belong to this gate:
#   sandbox          sandbox=""; the child has <script>s and NO CSP, so only
#                    the scripts flag can keep /never.js unfetched and the
#                    inline script unrun (the document still paints).
#   active-sandbox   allow-scripts allow-same-origin: the unsandboxed frame's
#                    whole click/message round trip, plus modals/popups flags.
#   active-opaque    allow-scripts: "null" message origins both ways, a
#                    non-"*" targetOrigin never delivered, SecurityError for
#                    localStorage/sessionStorage/document.cookie, no Worker.
# Both active modes also judge the console, this browser's only dialog
# surface: no alert/confirm/prompt text may reach it.
#
# This fragment is read BEFORE passive_frame.mk (it sits after jsprobe.mk), so
# no prerequisite here may name a variable that fragment defines: those expand
# empty at parse time. The shipping harness binary itself is the prerequisite
# that carries the whole source list; recipes expand the variables late.
FRAME_SANDBOX_HARNESS := $(BUILD)/passive-frame/current
FRAME_SANDBOX_HOST_MODES := sandbox active-sandbox active-opaque
$(FRAME_SANDBOX_DIR)/ignore-flags: $(FRAME_SANDBOX_HARNESS) c/apps/gui/browser/frame/sandbox.h tests/frame_sandbox.mk
	@mkdir -p $(FRAME_SANDBOX_DIR)
	@$(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) -DSANDBOX_NEGCTL_IGNORE_FLAGS -o $@ $(PASSIVE_FRAME_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# The control ignores every flag (the old "present and ignored" shape the
# refusal once guarded against). Each restriction assertion must FAIL, by
# name, and each positive assertion must still pass -- a control that failed
# everything would show only that the harness broke.
FRAME_SANDBOX_NEG_FAILS_sandbox := \
  'policy-blocked scripts and nested documents never fetch' \
  'native preview notice appears only for documents whose scripts were refused'
FRAME_SANDBOX_NEG_PASSES_sandbox := \
  'actual child document text reaches embedded paint' \
  'child DOM and scripts do not enter the parent realm'
FRAME_SANDBOX_NEG_FAILS_active-sandbox := \
  'allow-same-origin frame keeps its origin, storage and cookies; modals and popups stay sandboxed' \
  'no simple dialog message reaches the console without allow-modals' \
  'the blocked dialog is reported to the developer console' \
  'the blocked popup is reported to the developer console'
FRAME_SANDBOX_NEG_PASSES_active-sandbox := \
  'native child click and bidirectional message repaint embedded pixels' \
  'parent receives exact child origin and stable source identity'
FRAME_SANDBOX_NEG_FAILS_active-opaque := \
  'native child click and bidirectional message repaint embedded pixels' \
  'opaque-origin frame: storage/cookie SecurityError, null origin, no dialogs, popup null, no Worker' \
  'parent receives exact child origin and stable source identity' \
  'no simple dialog message reaches the console without allow-modals'
FRAME_SANDBOX_NEG_PASSES_active-opaque := \
  'external classic child script loaded once per document runtime' \
  'frame WindowProxy identity survives child load'
.PHONY: test-frame-sandbox-host test-frame-sandbox-host-negctl
test-frame-sandbox-host-negctl: $(FRAME_SANDBOX_DIR)/ignore-flags
	@for mode in $(FRAME_SANDBOX_HOST_MODES); do \
	  rc=0; $(FRAME_SANDBOX_DIR)/ignore-flags $$mode > $(FRAME_SANDBOX_DIR)/ignore-$$mode.log 2>&1 || rc=$$?; \
	  if [ $$rc -ne 1 ]; then tail -30 $(FRAME_SANDBOX_DIR)/ignore-$$mode.log; echo "NEGATIVE CONTROL FAILED: $$mode exited $$rc"; exit 1; fi; \
	  case $$mode in \
	    sandbox) fails="$(FRAME_SANDBOX_NEG_FAILS_sandbox)"; passes="$(FRAME_SANDBOX_NEG_PASSES_sandbox)";; \
	    active-sandbox) fails="$(FRAME_SANDBOX_NEG_FAILS_active-sandbox)"; passes="$(FRAME_SANDBOX_NEG_PASSES_active-sandbox)";; \
	    active-opaque) fails="$(FRAME_SANDBOX_NEG_FAILS_active-opaque)"; passes="$(FRAME_SANDBOX_NEG_PASSES_active-opaque)";; \
	  esac; \
	  eval "set -- $$fails"; for want; do grep -qF "FAIL: $$want" $(FRAME_SANDBOX_DIR)/ignore-$$mode.log || \
	    { echo "NEGATIVE CONTROL FAILED: $$mode did not fail: $$want"; exit 1; }; done; \
	  eval "set -- $$passes"; for want; do grep -qF "ok: $$want" $(FRAME_SANDBOX_DIR)/ignore-$$mode.log || \
	    { echo "NEGATIVE CONTROL FAILED: $$mode lost a positive check: $$want"; exit 1; }; done; \
	  echo "negative control ok: $$mode with flags ignored fails its restriction checks, keeps its positive ones"; \
	done
test-frame-sandbox-host: test-frame-sandbox-host-negctl $(FRAME_SANDBOX_HARNESS)
	@for mode in $(FRAME_SANDBOX_HOST_MODES); do \
	  rc=0; $(FRAME_SANDBOX_HARNESS) $$mode > $(FRAME_SANDBOX_DIR)/$$mode.log 2>&1 || rc=$$?; \
	  grep -E '^FAIL' $(FRAME_SANDBOX_DIR)/$$mode.log; tail -1 $(FRAME_SANDBOX_DIR)/$$mode.log; \
	  test $$rc -eq 0 || exit $$rc; done
ci-host: test-frame-sandbox-host

# ---- test-frame-sandbox-guest: the device, two real origins ----------------
# tests/qmp/frame_sandbox_guest.py boots the ISO, serves a parent page and
# four sandboxed child pages from two host ports (two origins), and judges the
# serial log and its own server's request log (Origin headers). Every
# assertion is classified POSITIVE / RESTRICTION / NOT DISCRIMINATING; see
# the driver's header.
#
# The control is a browser.aex whose frame loader ignores every flag
# (frame/passive.c built with -DSANDBOX_NEGCTL_IGNORE_FLAGS, everything else
# the shipping objects) on its own disk image: all RESTRICTION assertions
# must fail and all POSITIVE ones must still pass, or the driver exits 1.
# Both boot QEMU (several minutes each under TCG); the default disk is only
# read (-snapshot), and the control never touches it.
FRAME_SANDBOX_NEG_BROWSER_OBJ = $(filter-out $(BUILD)/browserobj/c/apps/gui/browser/frame/passive.o,$(BROWSER_OBJ)) \
                                $(FRAME_SANDBOX_DIR)/passive-ignore-flags.o
$(FRAME_SANDBOX_DIR)/passive-ignore-flags.o: c/apps/gui/browser/frame/passive.c c/apps/gui/browser/frame/sandbox.h \
        c/apps/gui/browser/frame/window.inc c/apps/gui/browser/frame/focus.inc $(BUILD)/browser.elf
	@mkdir -p $(dir $@)
	$(COMPILE) $(BROWSER_JS_CF) $(CSS_INC) -DSANDBOX_NEGCTL_IGNORE_FLAGS -c $< -o $@
$(FRAME_SANDBOX_DIR)/browser-ignore-flags.elf: $(BUILD)/browser.elf $(FRAME_SANDBOX_DIR)/passive-ignore-flags.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BROWSER_JS_OBJ) \
	    $(FRAME_SANDBOX_NEG_BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group
$(FRAME_SANDBOX_DIR)/browser-ignore-flags.aex: $(FRAME_SANDBOX_DIR)/browser-ignore-flags.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ Browser - 'B' 120 130 240 --stack-pages 2048

.PHONY: test-frame-sandbox-guest test-frame-sandbox-guest-negctl
test-frame-sandbox-guest-negctl: $(ISO) $(FRAME_SANDBOX_DIR)/browser-ignore-flags.aex
	@$(MAKE) --no-print-directory DISK=$(FRAME_SANDBOX_DIR)/disk-ignore-flags.img \
	    BROWSER_AEX=$(FRAME_SANDBOX_DIR)/browser-ignore-flags.aex $(FRAME_SANDBOX_DIR)/disk-ignore-flags.img
	python3 tests/qmp/frame_sandbox_guest.py --iso $(ISO) --disk $(FRAME_SANDBOX_DIR)/disk-ignore-flags.img \
	    --out $(FRAME_SANDBOX_DIR)/guest-ignore-flags --expect-flags-ignored
test-frame-sandbox-guest: test-frame-sandbox-guest-negctl $(ISO) $(DISK)
	python3 tests/qmp/frame_sandbox_guest.py --iso $(ISO) --disk $(DISK) --out $(FRAME_SANDBOX_DIR)/guest
ci-boot: test-frame-sandbox-guest

# The host modes again under ASan/UBSan (Darwin has no leak sanitizer): the
# opaque-origin getters, the skipped runtime of a scripts-sandboxed frame and
# the sandbox prelude all run inside teardown-heavy child documents.
.PHONY: test-frame-sandbox-host-san
test-frame-sandbox-host-san: test-frame-sandbox-host $(BUILD)/passive-frame/san
	@for mode in $(FRAME_SANDBOX_HOST_MODES); do \
	  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/passive-frame/san $$mode > $(FRAME_SANDBOX_DIR)/san-$$mode.log 2>&1 || \
	    { tail -60 $(FRAME_SANDBOX_DIR)/san-$$mode.log; exit 1; }; tail -1 $(FRAME_SANDBOX_DIR)/san-$$mode.log; done
ci-host: test-frame-sandbox-host-san
