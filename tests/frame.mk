# tests/frame.mk -- a same-origin second browsing context
# (c/apps/browser/js_frame.c) and the mutation surface it needed from
# js_domparser.c (createElement/createTextNode/createComment/appendChild/
# insertBefore/removeChild/setAttribute/removeAttribute/innerHTML=/
# getElementsByTagName).
#
#   make test-frame           the specimen (Cloudflare's own JS-detections
#                              bootstrap, reduced to its mechanism -- no
#                              hostname anywhere in this file or the C it
#                              gates) plus the <script src> refusal and the
#                              never-adopted-document inertness check.
#   make test-frame-negctl    the SAME binary, linked with -DJS_FRAME_NO_EXEC
#                              (js_frame.c's own control: the script sink still
#                              fires and every DOM method is still present --
#                              only the JS_Eval that would run the script is
#                              compiled out) -- must FAIL, specifically on the
#                              "the specimen's script ran" assertion, and pass
#                              every other assertion unchanged (the src-refusal
#                              and never-adopted checks do not depend on
#                              JS_Eval running at all).
#   make test-frame-lifecycle-negctl builds a copy whose __frameRelease is a
#                              no-op. Four earlier live contexts plus four new
#                              ones fill the 8 slots; the remaining eight
#                              sequential frames must fail the 12/12 assertion.
#
# THIS FRAGMENT DOES NOT LINK BROWSER_JS_SRC OR js_platform.c. Per the build's
# own allocation, js_platform.c owns the contentDocument/contentWindow
# getters that would call __frameAdopt from a real <iframe> element, and was
# being actively edited by other work as this was written. What this
# fragment gates -- js_domparser.c's mutation surface and js_frame.c itself
# -- needs neither: DOMParser builds documents on its own, and the driver
# calls __frameAdopt directly, standing in for what js_platform.c's settle()
# does. Mirrors test-html5lib's shape (a bare quickjs + HTML_PARSER_SRC +
# libcss_host.a link), not tests/worker.mk's BROWSER_JS_SRC-minus-exclusions
# shape, because this feature does not need the rest of the browser to exist
# to be tested -- see the file's own header for why that is a deliberate
# choice, not an oversight.
FRAME_TEST_BASE_SRC := tests/unit/frame_test.c \
                  c/apps/browser/js_domparser.c \
                  $(HTML_PARSER_SRC)
FRAME_TEST_SRC := $(FRAME_TEST_BASE_SRC) c/apps/browser/js_frame.c
FRAME_CF := $(BTEST_INC) $(CSS_INC) $(JS_INC) -Iinclude/abi -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

.PHONY: test-frame test-frame-negctl test-frame-lifecycle-negctl

$(BUILD)/frame_test: $(FRAME_TEST_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(FRAME_CF) -o $@ $(FRAME_TEST_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm

# test-frame-negctl is a PREREQUISITE, not a sibling -- NOT_CI drops every
# `test-*-negctl` from the suite listing on the ground that a control is "run
# by its positive counterpart", and tools/audit_tests.py flags one that is
# not a prerequisite of anything as a NEW stranded control (CLAUDE.md rule 5
# names this exact trap and gives tests/idb.mk/tests/worker.mk as the worked
# examples this fragment follows).
test-frame: test-frame-negctl test-frame-lifecycle-negctl $(BUILD)/frame_test
	@$(BUILD)/frame_test

$(BUILD)/frame_test_negctl: $(FRAME_TEST_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(FRAME_CF) -DJS_FRAME_NO_EXEC -o $@ $(FRAME_TEST_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm

test-frame-negctl: $(BUILD)/frame_test_negctl
	@if $(BUILD)/frame_test_negctl > $(BUILD)/frame_negctl.log 2>&1; then \
	    echo "test-frame-negctl: FAILED -- the suite PASSED against a build where the frame"; \
	    echo "  script sink never actually runs the script, so it is not measuring the one"; \
	    echo "  property this feature exists for (a contentWindow that exists and does nothing"; \
	    echo "  is the exact hang js_platform.c:2570 records)."; \
	    exit 1; \
	 else \
	    echo "test-frame-negctl: ok -- the control catches it:"; \
	    grep -E 'FAIL' $(BUILD)/frame_negctl.log | head -8; \
	 fi

# The lifecycle control is sed-built instead of carried as a shipping #ifdef:
# there must not be a production switch that turns a context release into a
# no-op. The sed target is checked before compilation; otherwise a future
# refactor could make the patch match nothing and leave a green fake control.
$(BUILD)/frame_test_lifecycle_negctl: $(FRAME_TEST_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)/frame-negctl
	@sed 's/if (doc) (void)frame_release_doc(doc);/(void)doc;/' \
	    c/apps/browser/js_frame.c > $(BUILD)/frame-negctl/js_frame_no_release.c
	@cmp -s c/apps/browser/js_frame.c $(BUILD)/frame-negctl/js_frame_no_release.c && \
	    { echo "test-frame-lifecycle-negctl: FAILED -- sed patched nothing"; exit 1; } || true
	@$(CC) -O2 -w $(FRAME_CF) -o $@ $(FRAME_TEST_BASE_SRC) \
	    $(BUILD)/frame-negctl/js_frame_no_release.c $(QJS_SRC) $(BUILD)/libcss_host.a -lm

test-frame-lifecycle-negctl: $(BUILD)/frame_test_lifecycle_negctl
	@if $(BUILD)/frame_test_lifecycle_negctl > $(BUILD)/frame_lifecycle_negctl.log 2>&1; then \
	    echo "test-frame-lifecycle-negctl: FAILED -- 12 sequential frames passed with release disabled"; \
	    exit 1; \
	 else \
	    grep -q 'FAIL: 12 sequential adopt/release cycles' $(BUILD)/frame_lifecycle_negctl.log || \
	      { echo "test-frame-lifecycle-negctl: FAILED somewhere unrelated:"; \
	        grep -E 'FAIL|refused: too many live frames' $(BUILD)/frame_lifecycle_negctl.log | head -12; exit 1; }; \
	    echo "test-frame-lifecycle-negctl: ok -- no release fills 8 slots and fails the 12/12 assertion"; \
	    grep -E 'refused: too many live frames|FAIL: 12 sequential' $(BUILD)/frame_lifecycle_negctl.log | head -4; \
	 fi

# ===========================================================================
# test-frame-wired -- CLAUDE.md RULE 4: LINKING A TU IS NOT RUNNING IT.
# ===========================================================================
# test-frame above proves js_frame.c works. It proves NOTHING about whether a
# page can reach it, because it calls __frameAdopt by hand. The one line that
# connects a real <iframe> to a frame context lives in js_platform.c's
# settle(), in a different file, owned by a different line of work -- exactly
# the shape this tree has already paid for twice (the WPT runner that linked
# css_extra.c and layout.c and then never called them, and read the same
# 531/11152 with and without an entire grid implementation).
#
# So this gate drives a REAL <iframe> element through js_platform.c's own
# contentDocument getter and asserts the frame's script ran. It reuses
# $(BUILD)/wpt_test, which links js_platform.c AND js_frame.c (tests/wpt.mk's
# source list is a SUBTRACTION from $(BROWSER_JS_SRC), so both arrived on it
# the minute they existed -- verified with nm, not assumed), and it borrows
# testharness.js out of the optional WPT checkout the same way tests/iframe.mk
# does rather than vendoring a second copy. Absent corpus => SKIP LOUDLY,
# naming the command that settles it; a gate that cannot run here must never
# fail red for an unrelated reason.
#
# The fixture is copied in and deleted out on BOTH paths (the trap-shaped
# subshell below), because leaving it in $(WPT_ROOT)/dom would silently add
# subtests to every other agent's `make test-wpt` denominator -- tests/iframe.mk
# records watching exactly that happen while it was being written.
# The three variants use three DESTINATION names. They are prerequisites and
# therefore run in parallel under `make -j`; sharing frame-script-probe.html
# let one control delete another's input and report 0/0. That apparatus failure
# was watched under `make -j4` before these names were split.
FRAME_PROBE_DIR := $(WPT_ROOT)/dom
FRAME_FIXTURE   := tests/fixtures/iframe/frame-script-probe.html
FRAME_LIFECYCLE_NEG_NAME := frame-script-probe-lifecycle-negctl
FRAME_NOEXEC_NEG_NAME := frame-script-probe-noexec-negctl

.PHONY: test-frame-wired test-frame-wired-negctl test-frame-wired-lifecycle-negctl

test-frame-wired: test-frame-wired-negctl test-frame-wired-lifecycle-negctl $(BUILD)/wpt_test
	@if [ ! -f "$(WPT_ROOT)/resources/testharness.js" ]; then \
	    echo "test-frame-wired: SKIP -- $(WPT_ROOT)/resources/testharness.js not present."; \
	    echo "  This gate reuses testharness.js out of the (optional) WPT checkout"; \
	    echo "  rather than vendoring a second copy. Settle it with: make wpt-fetch"; \
	    exit 0; \
	 fi
	@mkdir -p $(FRAME_PROBE_DIR)
	@cp $(FRAME_FIXTURE) $(FRAME_PROBE_DIR)/
	@( WEBAPI_FILE_ROOT=$(WPT_ROOT) $(BUILD)/wpt_test --root $(WPT_ROOT) --subset dom \
	     --only dom/frame-script-probe --jobs 1 > $(BUILD)/frame_wired.log 2>&1; \
	   rm -f $(FRAME_PROBE_DIR)/frame-script-probe.html; \
	   grep -E '^  FAIL|^  dom |subtests passed' $(BUILD)/frame_wired.log; \
	   got=$$(grep -oE '^  dom +[0-9]+/5' $(BUILD)/frame_wired.log | grep -oE '[0-9]+' | head -1); \
	   if [ -z "$$got" ]; then echo "test-frame-wired: FAIL -- runner produced no dom N/5 line (see $(BUILD)/frame_wired.log)"; exit 1; fi; \
	   if [ "$$got" != "5" ]; then echo "test-frame-wired: FAIL -- $$got/5"; exit 1; fi; \
	   ran=$$(grep -c 'FRAME-WIRED-INSERTED' $(BUILD)/frame_wired.log); \
	   mk=$$(grep -c 'FRAME-WIRED-MARKUP' $(BUILD)/frame_wired.log); \
	   life=$$(grep -c 'FRAME-WIRED-LIFECYCLE' $(BUILD)/frame_wired.log); \
	   if [ "$$ran" = "0" ] || [ "$$mk" = "0" ]; then \
	     echo "test-frame-wired: FAIL -- all 5 subtests passed but the frame scripts never RAN"; \
	     echo "  (inserted=$$ran markup=$$mk). That combination is the exact shape this gate"; \
	     echo "  exists for: js_platform.c's iframe surface is intact and js_frame.c is linked,"; \
	     echo "  and nothing connects them. The 5/5 alone would have read as success."; \
	     exit 1; \
	   fi; \
	   if [ "$$life" != "12" ]; then \
	     echo "test-frame-wired: FAIL -- only $$life/12 sequential frame scripts ran"; exit 1; \
	   fi; \
	   echo "test-frame-wired: ok -- 5/5; inserted=$$ran markup=$$mk lifecycle=$$life/12" )

# This control removes the DOM-removal release at the JS/C seam while leaving
# adoption, script execution, load events and every mutation wrapper intact.
# The harness still reaches 5/5: refusal also fires load, so subtest counts
# cannot distinguish a live context from a blank terminating frame. The twelve
# console markers are the control. With the former behaviour only the free
# slots run and the rest print "too many live frames (8)".
FRAME_WIRED_LIFECYCLE_SRC := $(filter-out c/apps/browser/js_platform.c,$(WPT_TEST_SRC))
$(BUILD)/wpt_test_frame_lifecycle_negctl: $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)/frame-negctl
	@sed 's/^"      releaseDoc(r);/"      r.doc = r.doc;/' \
	    c/apps/browser/js_platform.c > $(BUILD)/frame-negctl/js_platform_no_frame_release.c
	@cmp -s c/apps/browser/js_platform.c $(BUILD)/frame-negctl/js_platform_no_frame_release.c && \
	    { echo "test-frame-wired-lifecycle-negctl: FAILED -- sed patched nothing"; exit 1; } || true
	@if [ -n "$(WPT_LINK_ERR)" ]; then echo "$(WPT_LINK_ERR)"; exit 1; fi
	@$(CC) -O2 -w $(WPT_CF) -o $@ $(FRAME_WIRED_LIFECYCLE_SRC) \
	    $(BUILD)/frame-negctl/js_platform_no_frame_release.c $(HTML_PARSER_SRC) \
	    $(QJS_SRC) $(GFX_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-frame-wired-lifecycle-negctl: $(BUILD)/wpt_test_frame_lifecycle_negctl
	@if [ ! -f "$(WPT_ROOT)/resources/testharness.js" ]; then \
	    echo "test-frame-wired-lifecycle-negctl: SKIP -- $(WPT_ROOT)/resources/testharness.js not present."; \
	    echo "  Settle it with: make wpt-fetch"; exit 0; \
	 fi
	@mkdir -p $(FRAME_PROBE_DIR)
	@cp $(FRAME_FIXTURE) $(FRAME_PROBE_DIR)/$(FRAME_LIFECYCLE_NEG_NAME).html
	@( WEBAPI_FILE_ROOT=$(WPT_ROOT) $(BUILD)/wpt_test_frame_lifecycle_negctl --root $(WPT_ROOT) --subset dom \
	     --only dom/$(FRAME_LIFECYCLE_NEG_NAME) --jobs 1 > $(BUILD)/frame_wired_lifecycle_negctl.log 2>&1; \
	   rm -f $(FRAME_PROBE_DIR)/$(FRAME_LIFECYCLE_NEG_NAME).html; \
	   life=$$(grep -c 'FRAME-WIRED-LIFECYCLE' $(BUILD)/frame_wired_lifecycle_negctl.log); \
	   if [ "$$life" = "12" ]; then \
	     echo "test-frame-wired-lifecycle-negctl: FAILED -- all 12 ran with removal release patched out"; exit 1; \
	   fi; \
	   grep -q 'refused: too many live frames (8)' $(BUILD)/frame_wired_lifecycle_negctl.log || \
	     { echo "test-frame-wired-lifecycle-negctl: FAILED for an unrelated reason"; \
	       grep -E '^  FAIL|^  dom |subtests passed' $(BUILD)/frame_wired_lifecycle_negctl.log; exit 1; }; \
	   echo "test-frame-wired-lifecycle-negctl: ok -- only $$life/12 scripts ran without removal release" )

# THE CONTROL, and it is aimed at the WIRING rather than at js_frame.c.
# -DJS_FRAME_NO_EXEC leaves __frameAdopt installed, the script sink firing and
# every DOM method present -- ONLY the JS_Eval is compiled out. So the five
# subtests still pass (they assert the surface and the cross-origin refusal,
# neither of which needs a script to run) and the two "really ran" greps go to
# zero. That is precisely the "a contentWindow that exists and does nothing"
# hang js_platform.c records, manufactured on purpose, and it is why the
# positive gate greps for the script output instead of trusting 4/4.
$(BUILD)/wpt_test_frame_negctl: $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@if [ -n "$(WPT_LINK_ERR)" ]; then echo "$(WPT_LINK_ERR)"; exit 1; fi
	@$(CC) -O2 -w $(WPT_CF) -DJS_FRAME_NO_EXEC -o $@ $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(GFX_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-frame-wired-negctl: $(BUILD)/wpt_test_frame_negctl
	@if [ ! -f "$(WPT_ROOT)/resources/testharness.js" ]; then \
	    echo "test-frame-wired-negctl: SKIP -- $(WPT_ROOT)/resources/testharness.js not present."; \
	    echo "  Settle it with: make wpt-fetch"; \
	    exit 0; \
	 fi
	@mkdir -p $(FRAME_PROBE_DIR)
	@cp $(FRAME_FIXTURE) $(FRAME_PROBE_DIR)/$(FRAME_NOEXEC_NEG_NAME).html
	@( WEBAPI_FILE_ROOT=$(WPT_ROOT) $(BUILD)/wpt_test_frame_negctl --root $(WPT_ROOT) --subset dom \
	     --only dom/$(FRAME_NOEXEC_NEG_NAME) --jobs 1 > $(BUILD)/frame_wired_negctl.log 2>&1; \
	   rm -f $(FRAME_PROBE_DIR)/$(FRAME_NOEXEC_NEG_NAME).html; \
	   ran=$$(grep -c 'FRAME-WIRED-INSERTED' $(BUILD)/frame_wired_negctl.log); \
	   mk=$$(grep -c 'FRAME-WIRED-MARKUP' $(BUILD)/frame_wired_negctl.log); \
	   if [ "$$ran" != "0" ] || [ "$$mk" != "0" ]; then \
	     echo "test-frame-wired-negctl: FAILED -- a frame script RAN in a build with js_frame.c's"; \
	     echo "  JS_Eval compiled out (inserted=$$ran markup=$$mk), so the positive gate's"; \
	     echo "  'really ran' greps are matching something other than a frame script."; \
	     exit 1; \
	   fi; \
	   echo "test-frame-wired-negctl: ok -- no frame script runs with JS_FRAME_NO_EXEC," ; \
	   echo "  which is what makes the positive gate's grep evidence rather than decoration." )

ci-host: test-frame test-frame-wired
