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
#                              child runtimes, srcdoc and Worker doors) over
#                              mocked HTTP: seven modes, attribute AND CSP.
#   test-frame-sandbox-guest   QEMU: a real two-origin page with four sandboxed
#                              network frames (one sandboxed by its response
#                              header alone), observed off the serial log and
#                              the host server's own request log.
#
# Each has a control that was watched failing for the reason it names. The
# about:blank/srcdoc half (platform.c) is gated by test-frame-bootstrap-wiring,
# whose fixture asserts the flag semantics and whose three controls reproduce
# the two historical bypass shapes and the opaque-origin adoption.

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
# HTTP transport. The modes that belong to this gate:
#   sandbox          sandbox=""; the child has <script>s and NO CSP, so only
#                    the scripts flag can keep /never.js unfetched and the
#                    inline script unrun (the document still paints), and the
#                    parent's <iframe> still gets its one `load`.
#   active-sandbox   allow-scripts allow-same-origin: the unsandboxed frame's
#                    whole click/message round trip, plus modals/popups flags;
#                    its fetch keeps its Referer (the recorder's own control).
#   active-opaque    allow-scripts: "null" message origins both ways, a
#                    non-"*" targetOrigin never delivered, SecurityError for
#                    localStorage/sessionStorage/document.cookie, no Worker,
#                    and a fetch with `Origin: null` and NO Referer.
# The same flags through the CSP doors, no sandbox attribute anywhere:
#   csp-sandbox               child header `Content-Security-Policy: sandbox`
#   active-csp-opaque         child header `... sandbox allow-scripts`
#   active-parent-csp-opaque  TOP-LEVEL header `... sandbox allow-scripts`,
#                             inherited by the network frame AND by a srcdoc
#                             frame (platform.c's door)
#   active-worker-csp         a network Worker whose response header sandboxes
#                             it (CSP3: scripts/origin flag -> Blocked)
# Every active mode also judges the console, this browser's only dialog
# surface: no alert/confirm/prompt text may reach it.
#
# The harness is the realm fixture (ports + frame.c + domparser.c, so the
# srcdoc door is real) plus js/workers/worker.c (so the Worker door is real).
# This fragment is read BEFORE passive_frame.mk (it sits after jsprobe.mk), so
# no prerequisite here may name a variable that fragment defines: those expand
# empty at parse time. The realm binary is the prerequisite that carries that
# source list; recipes expand the variables late.
FRAME_SANDBOX_HARNESS := $(FRAME_SANDBOX_DIR)/harness
FRAME_SANDBOX_HOST_SRC = $(FRAME_REALM_SRC) c/apps/gui/browser/js/workers/worker.c
FRAME_SANDBOX_HOST_DEP := $(BUILD)/passive-frame/realm c/apps/gui/browser/js/workers/worker.c \
  c/apps/gui/browser/frame/sandbox.h c/apps/gui/browser/frame/passive.h tests/frame_sandbox.mk
FRAME_SANDBOX_BUILD = @mkdir -p $(FRAME_SANDBOX_DIR); $(CC) $(RUNTIME_SCROLL_CF) $(IMG_HOST_INC) $(1) -o $@ \
  $(FRAME_SANDBOX_HOST_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
FRAME_SANDBOX_HOST_MODES := sandbox active-sandbox active-opaque \
  csp-sandbox active-csp-opaque active-parent-csp-opaque active-worker-csp
$(FRAME_SANDBOX_DIR)/harness: $(FRAME_SANDBOX_HOST_DEP)
	$(call FRAME_SANDBOX_BUILD,)
$(FRAME_SANDBOX_DIR)/ignore-flags: $(FRAME_SANDBOX_HOST_DEP)
	$(call FRAME_SANDBOX_BUILD,-DSANDBOX_NEGCTL_IGNORE_FLAGS)
$(FRAME_SANDBOX_DIR)/ignore-csp: $(FRAME_SANDBOX_HOST_DEP)
	$(call FRAME_SANDBOX_BUILD,-DSANDBOX_NEGCTL_IGNORE_CSP)
$(FRAME_SANDBOX_DIR)/no-pageless-load: $(FRAME_SANDBOX_HOST_DEP)
	$(call FRAME_SANDBOX_BUILD,-DPF_TEST_NO_PAGELESS_LOAD)
$(FRAME_SANDBOX_DIR)/opaque-referrer: $(FRAME_SANDBOX_HOST_DEP)
	$(call FRAME_SANDBOX_BUILD,-DWEBAPI_NEGCTL_OPAQUE_REFERRER)
$(FRAME_SANDBOX_DIR)/san: $(FRAME_SANDBOX_HOST_DEP)
	$(call FRAME_SANDBOX_BUILD,-O1 -g -fsanitize=address$(FRAME_SANDBOX_COMMA)undefined -fno-omit-frame-pointer)

# One control runner: `$(call FRAME_SANDBOX_NEG,binary,mode,fails,passes)`.
# The binary must exit 1 on the mode, name every listed restriction check as
# FAIL and every listed positive one as ok -- a control that failed
# everything would show only that the harness broke. An empty fail list means
# the mode must PASS under the control (the half it does not remove).
define FRAME_SANDBOX_NEG
	@log=$(FRAME_SANDBOX_DIR)/$(notdir $(1))-$(2).log; rc=0; $(1) $(2) > $$log 2>&1 || rc=$$?; \
	 fails="$(3)"; passes="$(4)"; \
	 if [ -z "$$fails" ]; then \
	   if [ $$rc -ne 0 ]; then grep '^FAIL' $$log; echo "NEGATIVE CONTROL FAILED: $(notdir $(1)) $(2) must still pass, exited $$rc"; exit 1; fi; \
	   echo "negative control ok: $(notdir $(1)) leaves $(2) passing"; exit 0; fi; \
	 if [ $$rc -ne 1 ]; then tail -30 $$log; echo "NEGATIVE CONTROL FAILED: $(notdir $(1)) $(2) exited $$rc"; exit 1; fi; \
	 eval "set -- $$fails"; for want; do grep -qF "FAIL: $$want" $$log || \
	   { grep '^FAIL' $$log; echo "NEGATIVE CONTROL FAILED: $(notdir $(1)) $(2) did not fail: $$want"; exit 1; }; done; \
	 eval "set -- $$passes"; for want; do grep -qF "ok: $$want" $$log || \
	   { echo "NEGATIVE CONTROL FAILED: $(notdir $(1)) $(2) lost a positive check: $$want"; exit 1; }; done; \
	 echo "negative control ok: $(notdir $(1)) $(2) fails its restriction checks, keeps its positive ones"
endef
FRAME_SANDBOX_COMMA := ,

# ignore-flags: every flag ignored (the old "present and ignored" shape the
# refusal once guarded against), on the three ATTRIBUTE modes.
FRAME_SANDBOX_NEG_FAILS_sandbox := \
  'policy-blocked scripts and nested documents never fetch' \
  'native preview notice appears only for documents whose scripts were refused'
FRAME_SANDBOX_NEG_PASSES_sandbox := \
  'actual child document text reaches embedded paint' \
  'child DOM and scripts do not enter the parent realm' \
  'the parent iframe element gets exactly one load event' 
FRAME_SANDBOX_NEG_FAILS_active-sandbox := \
  'allow-same-origin frame keeps its origin, storage and cookies; modals and popups stay sandboxed' \
  'no simple dialog message reaches the console without allow-modals' \
  'the blocked dialog is reported to the developer console' \
  'the blocked popup is reported to the developer console'
FRAME_SANDBOX_NEG_PASSES_active-sandbox := \
  'native child click and bidirectional message repaint embedded pixels' \
  'parent receives exact child origin and stable source identity' \
  'a same-origin sandboxed document fetch keeps its Referer' 
FRAME_SANDBOX_NEG_FAILS_active-opaque := \
  'native child click and bidirectional message repaint embedded pixels' \
  'opaque-origin frame: storage/cookie SecurityError, null origin, no dialogs, popup null, no Worker' \
  'parent receives exact child origin and stable source identity' \
  'no simple dialog message reaches the console without allow-modals' \
  'an opaque-origin document fetch carries Origin: null and no Referer' 
FRAME_SANDBOX_NEG_PASSES_active-opaque := \
  'external classic child script loaded once per document runtime' \
  'frame WindowProxy identity survives child load'
# ignore-csp: ONLY the response-header CSP sandbox doors answer "no directive"
# (passive.c csp_sandbox). The CSP modes fail as their attribute twins do;
# the attribute mode must still pass, so the control removes the CSP half only.
FRAME_SANDBOX_NEG_FAILS_csp-sandbox := $(FRAME_SANDBOX_NEG_FAILS_sandbox)
FRAME_SANDBOX_NEG_PASSES_csp-sandbox := $(FRAME_SANDBOX_NEG_PASSES_sandbox)
FRAME_SANDBOX_NEG_FAILS_active-csp-opaque := $(FRAME_SANDBOX_NEG_FAILS_active-opaque)
FRAME_SANDBOX_NEG_PASSES_active-csp-opaque := $(FRAME_SANDBOX_NEG_PASSES_active-opaque)
FRAME_SANDBOX_NEG_FAILS_active-parent-csp-opaque := $(FRAME_SANDBOX_NEG_FAILS_active-opaque) \
  'a srcdoc frame inherits the top-level CSP sandbox exactly as the network frame does'
FRAME_SANDBOX_NEG_PASSES_active-parent-csp-opaque := $(FRAME_SANDBOX_NEG_PASSES_active-opaque)
FRAME_SANDBOX_NEG_FAILS_active-worker-csp := 'a network Worker whose response CSP sandboxes it never runs'
FRAME_SANDBOX_NEG_PASSES_active-worker-csp := \
  'native child click and bidirectional message repaint embedded pixels' \
  'parent receives exact child origin and stable source identity'

# A check name with commas cannot be a literal $(call) argument.
FRAME_SANDBOX_REFERRER_KEEPS := 'opaque-origin frame: storage/cookie SecurityError, null origin, no dialogs, popup null, no Worker'

.PHONY: test-frame-sandbox-host test-frame-sandbox-host-negctl
test-frame-sandbox-host-negctl: $(FRAME_SANDBOX_DIR)/ignore-flags $(FRAME_SANDBOX_DIR)/ignore-csp \
    $(FRAME_SANDBOX_DIR)/no-pageless-load $(FRAME_SANDBOX_DIR)/opaque-referrer
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/ignore-flags,sandbox,$(FRAME_SANDBOX_NEG_FAILS_sandbox),$(FRAME_SANDBOX_NEG_PASSES_sandbox))
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/ignore-flags,active-sandbox,$(FRAME_SANDBOX_NEG_FAILS_active-sandbox),$(FRAME_SANDBOX_NEG_PASSES_active-sandbox))
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/ignore-flags,active-opaque,$(FRAME_SANDBOX_NEG_FAILS_active-opaque),$(FRAME_SANDBOX_NEG_PASSES_active-opaque))
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/ignore-csp,csp-sandbox,$(FRAME_SANDBOX_NEG_FAILS_csp-sandbox),$(FRAME_SANDBOX_NEG_PASSES_csp-sandbox))
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/ignore-csp,active-csp-opaque,$(FRAME_SANDBOX_NEG_FAILS_active-csp-opaque),$(FRAME_SANDBOX_NEG_PASSES_active-csp-opaque))
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/ignore-csp,active-parent-csp-opaque,$(FRAME_SANDBOX_NEG_FAILS_active-parent-csp-opaque),$(FRAME_SANDBOX_NEG_PASSES_active-parent-csp-opaque))
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/ignore-csp,active-worker-csp,$(FRAME_SANDBOX_NEG_FAILS_active-worker-csp),$(FRAME_SANDBOX_NEG_PASSES_active-worker-csp))
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/ignore-csp,active-opaque,,)
# no-pageless-load: a runtime-less frame's parent <iframe> gets no `load`
# (the old shape). Exactly that check fails, nothing else; a frame WITH a
# runtime is unaffected (its load comes from the runtime pump).
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/no-pageless-load,sandbox,'the parent iframe element gets exactly one load event','actual child document text reaches embedded paint')
	@test "$$(grep -c '^FAIL' $(FRAME_SANDBOX_DIR)/no-pageless-load-sandbox.log)" = 1 || { echo "NEGATIVE CONTROL FAILED: no-pageless-load sandbox failed more than the load check"; exit 1; }
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/no-pageless-load,csp-sandbox,'the parent iframe element gets exactly one load event','actual child document text reaches embedded paint')
	@test "$$(grep -c '^FAIL' $(FRAME_SANDBOX_DIR)/no-pageless-load-csp-sandbox.log)" = 1 || { echo "NEGATIVE CONTROL FAILED: no-pageless-load csp-sandbox failed more than the load check"; exit 1; }
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/no-pageless-load,paint,,)
# opaque-referrer: an opaque-origin document's fetch names its URL again.
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/opaque-referrer,active-opaque,'an opaque-origin document fetch carries Origin: null and no Referer',$(FRAME_SANDBOX_REFERRER_KEEPS))
	@test "$$(grep -c '^FAIL' $(FRAME_SANDBOX_DIR)/opaque-referrer-active-opaque.log)" = 1 || { echo "NEGATIVE CONTROL FAILED: opaque-referrer failed more than the Referer check"; exit 1; }
	$(call FRAME_SANDBOX_NEG,$(FRAME_SANDBOX_DIR)/opaque-referrer,active-sandbox,,)
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
test-frame-sandbox-host-san: test-frame-sandbox-host $(FRAME_SANDBOX_DIR)/san
	@for mode in $(FRAME_SANDBOX_HOST_MODES); do \
	  ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(FRAME_SANDBOX_DIR)/san $$mode > $(FRAME_SANDBOX_DIR)/san-$$mode.log 2>&1 || \
	    { tail -60 $(FRAME_SANDBOX_DIR)/san-$$mode.log; exit 1; }; tail -1 $(FRAME_SANDBOX_DIR)/san-$$mode.log; done
ci-host: test-frame-sandbox-host-san
