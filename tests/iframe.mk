# tests/iframe.mk -- the nested browsing context (c/apps/browser/js_platform.c's
# installIframes()).
#
#   make test-iframe          run tests/fixtures/iframe/iframe-probe.html, a
#                              22-case termination probe, through $(BUILD)/wpt_test
#                              -- every asynchronous path this feature exposes,
#                              with a 2s-per-case budget so a hang FAILS instead
#                              of hanging the gate. Requires 21/22: the one
#                              permanent gap is C2 (window[i] indexed access,
#                              documented out of scope in js_platform.c's own
#                              comment on window.frames).
#
#                              A1-A7/B1-B5/C1-C2 (14 cases) exercise
#                              appendChild/insertBefore/replaceChild/
#                              setAttribute/innerHTML -- the doors that were
#                              already wrapped. D1-D8 (8 cases, added
#                              2026-08-29 after a refutation pass found a
#                              concrete hang) exercise the seven doors that
#                              were NOT: ParentNode.append/prepend,
#                              ChildNode.before/after/replaceWith,
#                              Element.insertAdjacentHTML and the outerHTML
#                              setter, plus one case (D8) that checks
#                              insertAdjacentHTML does not spuriously
#                              re-navigate an unrelated, already-settled
#                              sibling iframe. A fixture that only exercised
#                              the doors already covered could never have
#                              caught this class again -- see js_platform.c's
#                              installIframes() comment for the full account.
#   make test-iframe-negctl   the SAME probe against a build compiled with
#                              -DJS_IFRAME_NO_INSTALL (installIframes() itself
#                              compiled out -- see js_platform.c). Must FAIL on
#                              at least 20 of the 22 cases: proof the probe is
#                              measuring this feature and not, say, the
#                              testharness plumbing.
#
# grep -rn iframe tests/*.mk was 0 hits before this file -- CLAUDE.md rule 4,
# a gate nobody runs is a gate that rots, landing on a feature with 12 passing
# probe cases and no way for anyone downstream to notice a 13th regressing.
#
# WHY THIS RIDES $(BUILD)/wpt_test RATHER THAN A NEW HARNESS: the probe is a
# real testharness.js file (promise_test/test, the exact idiom every WPT file
# uses) run against a real fetch of a real second file
# (logit-iframe-child.html) -- which is the whole point, since the feature
# under test is "does a same-origin fetch through the frame's own <iframe src>
# actually happen". tests/wpt.mk already builds exactly that runner. Reusing
# it means this fragment adds zero new source files and cannot drift from what
# the WPT numbers in tests/wpt.mk itself are measuring.
#
# THE CORPUS IS OPTIONAL, THE PROBE IS NOT: unlike test-wpt, this fragment
# supplies its OWN two fixture files (tests/fixtures/iframe/) rather than
# reading them out of $(WPT_ROOT) -- a real WPT checkout is a large optional
# fetch (`make wpt-fetch`) and this gate must run without it. What IS required
# is testharness.js/testharnessreport.js, which this fragment does NOT vendor
# (WPT's own files, and every other WPT-driven gate in this tree reads them out
# of a fetched corpus rather than a second committed copy -- one jar, two
# doors, avoided by not opening a second door). If $(WPT_ROOT)/resources is
# absent, this SKIPS LOUDLY (CLAUDE.md rule: a gate that cannot run here must
# name the missing capability and the command that settles it, not fail red
# for an unrelated reason) rather than reporting a false negative against
# js_platform.c.
#
# THE FIXTURES ARE COPIED IN AND DELETED OUT, every run, on both success and
# failure paths (the shell trap below) -- copying them in and leaving them
# would add two files and two new subtests to $(WPT_ROOT)/dom that
# `make test-wpt`'s ratchet did not commit to, in every other agent's corpus.
# This was watched happen once already while building this fragment (see the
# commit message): forgetting the cleanup step silently changed test-wpt's
# denominator by 14 subtests.
.PHONY: test-iframe test-iframe-negctl

IFRAME_PROBE_DIR := $(WPT_ROOT)/dom
IFRAME_FIXTURES := tests/fixtures/iframe/iframe-probe.html tests/fixtures/iframe/logit-iframe-child.html

test-iframe: $(BUILD)/wpt_test
	@if [ ! -f "$(WPT_ROOT)/resources/testharness.js" ]; then \
	    echo "test-iframe: SKIP -- $(WPT_ROOT)/resources/testharness.js not present."; \
	    echo "  This gate reuses testharness.js out of the (optional) WPT checkout"; \
	    echo "  rather than vendoring a second copy. Settle it with: make wpt-fetch"; \
	    exit 0; \
	 fi
	@mkdir -p $(IFRAME_PROBE_DIR)
	@cp $(IFRAME_FIXTURES) $(IFRAME_PROBE_DIR)/
	@( WEBAPI_FILE_ROOT=$(WPT_ROOT) $(BUILD)/wpt_test --root $(WPT_ROOT) --subset dom --only dom/iframe-probe --jobs 1 > $(BUILD)/iframe_probe.log 2>&1; \
	   st=$$?; \
	   rm -f $(IFRAME_PROBE_DIR)/iframe-probe.html $(IFRAME_PROBE_DIR)/logit-iframe-child.html; \
	   grep -E '^  FAIL|^  dom |subtests passed' $(BUILD)/iframe_probe.log; \
	   got=$$(grep -oE '^  dom +[0-9]+/22' $(BUILD)/iframe_probe.log | grep -oE '[0-9]+' | head -1); \
	   if [ -z "$$got" ]; then echo "test-iframe: FAIL -- runner did not produce a dom N/22 line (see $(BUILD)/iframe_probe.log)"; exit 1; fi; \
	   if [ "$$got" -lt 21 ]; then echo "test-iframe: FAIL -- $$got/22, want >= 21 (C2/window[i] is the one documented permanent gap)"; exit 1; fi; \
	   echo "test-iframe: ok -- $$got/22" )

# -DJS_IFRAME_NO_INSTALL: installIframes() itself compiled out of
# js_platform.c (see that file). Every async case (A1-A7, B1-B5, D1-D8) must
# go from PASS to FAIL -- each is a waitLoad()-shaped promise with a 2s
# budget and nothing ever fires 'load' with the installer gone, so they time
# out and reject. Only C1/C2 (window.length / window.frames / window[0],
# which are plain property reads with no async wait) could theoretically
# still read something -- in practice both fail too, since window.length's
# getter is itself installed by installIframes(). The assertion below is
# <=2 passing rather than ==0, so a future harmless coincidence on C1/C2
# does not make this control flaky.
$(BUILD)/wpt_test_iframe_negctl: $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@if [ -n "$(WPT_LINK_ERR)" ]; then echo "$(WPT_LINK_ERR)"; exit 1; fi
	@$(CC) -O2 -w $(WPT_CF) -DJS_IFRAME_NO_INSTALL -o $@ $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(GFX_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-iframe-negctl: $(BUILD)/wpt_test_iframe_negctl
	@if [ ! -f "$(WPT_ROOT)/resources/testharness.js" ]; then \
	    echo "test-iframe-negctl: SKIP -- $(WPT_ROOT)/resources/testharness.js not present."; \
	    echo "  Settle it with: make wpt-fetch"; \
	    exit 0; \
	 fi
	@mkdir -p $(IFRAME_PROBE_DIR)
	@cp $(IFRAME_FIXTURES) $(IFRAME_PROBE_DIR)/
	@( WEBAPI_FILE_ROOT=$(WPT_ROOT) $(BUILD)/wpt_test_iframe_negctl --root $(WPT_ROOT) --subset dom --only dom/iframe-probe --jobs 1 > $(BUILD)/iframe_probe_negctl.log 2>&1; \
	   rm -f $(IFRAME_PROBE_DIR)/iframe-probe.html $(IFRAME_PROBE_DIR)/logit-iframe-child.html; \
	   got=$$(grep -oE '^  dom +[0-9]+/22' $(BUILD)/iframe_probe_negctl.log | grep -oE '[0-9]+' | head -1); \
	   if [ -z "$$got" ]; then echo "test-iframe-negctl: FAIL -- runner did not produce a dom N/22 line (see $(BUILD)/iframe_probe_negctl.log)"; exit 1; fi; \
	   if [ "$$got" -gt 2 ]; then \
	       echo "test-iframe-negctl: FAILED -- $$got/22 passed with installIframes() compiled out;"; \
	       echo "  the probe is not measuring this feature."; \
	       exit 1; \
	   fi; \
	   echo "test-iframe-negctl: ok -- the control catches it ($$got/22, feature build is >=21/22)" )
