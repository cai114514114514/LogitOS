# tests/wpt.mk -- Web Platform Tests against the DOM and the Web-API layer.
#
# The html5lib precedent, applied one layer up. The HTML parser stopped being a
# tag-soup scanner when third_party/html5lib-tests arrived and gave it a number;
# the DOM and the Web APIs had no corpus at all, so every defect in them was
# found by loading a real site and reading a stack. That is unbounded work: the
# supply of sites is infinite and fixing one failure does not predict the next.
#
# Same shape as test-html5lib, deliberately: upstream DATA only, the runner is
# ours (tests/unit/wpt_test.c), and the report is a RATE ratcheted against a
# committed expected-failure list (tests/unit/wpt_expected_fail.txt).
#
#   make test-wpt                per-subset rates + the total, and THE GATE:
#                                non-zero if any subtest that passes today stops
#   make wpt                     the same measurement WITHOUT the gate, for a
#                                tree that is already broken
#   make test-wpt V=20           ... plus the first 20 UNEXPECTED failures
#   make test-wpt ONLY=dom/nodes restrict to paths containing a substring
#   make test-wpt-harness        does testharness.js run at all? (seconds)
#   make wpt-baseline            rewrite the expected-failure list from this run
#   make wpt-rank                the ranked cause table -- the work order
#   make wpt-fetch               vendor/refresh the corpus from upstream
#
# test-html5lib is a MEASUREMENT that always exits 0, and its header explains
# why: "a gate that is red on every run for weeks only teaches people to stop
# reading the build; it becomes a gate when there is a rate worth defending."
# test-wpt is a gate from the first commit, and that is not a disagreement --
# the ratchet is what makes the difference. 16.2% is not a rate worth
# defending; the 28,329 subtests that produce it are, one by one, and the
# expected-failure list is how the target asks about those and not about the
# percentage. It is green on a tree that changes nothing and red only when
# something that worked stops working.
#
# THE CORPUS IS OPTIONAL AND THE CAPABILITY IS NOT. WPT_ROOT points the runner
# at any checkout; when the directory is absent the runner says so and exits 0,
# because a missing corpus is not a regression in the code under test. That is
# what lets the vendored data be deleted later without deleting the ratchet --
# `make wpt-fetch WPT_ROOT=/somewhere` brings it back.
#
# Own fragment rather than lines in the Makefile, for the reason the other
# fragments give: concurrent agents overwrite that file wholesale.

.PHONY: test-wpt wpt wpt-baseline wpt-rank wpt-fetch wpt-list
.PHONY: test-wpt-negctl test-wpt-harness

WPT_ROOT ?= third_party/wpt
WPT_BASELINE := tests/unit/wpt_expected_fail.txt

# THE LINK MUST MATCH browser.aex's, OR EVERY NUMBER IS OF A BROWSER THAT DOES
# NOT EXIST. This list was hand-written and drifted twice in one night:
# js_events.c and then js_cssom.c were committed, the runner did not link
# either, and the suite went on reporting the bugs they fixed as still broken --
# 841 dead `<body onload>` files reported as a browser defect when the fix was
# sitting in the tree. That is worse than a missing test: it is a measurement
# that actively misinforms.
#
# So the JS side is a WILDCARD, exactly as the Makefile's BROWSER_JS_SRC is, and
# for the reason its comment gives: "the JS/DOM side is being extended by
# several parallel lines at once and a hand-kept list makes this one line the
# thing they all have to edit". browser.c itself is excluded -- it is the app
# shell (window, event loop, address bar) and draws through logit.h's `int 0x80`
# wrappers a host process cannot execute.
WPT_JS_SRC := $(filter-out c/apps/browser/browser.c,$(sort $(wildcard c/apps/browser/js_*.c)))

# The pipeline, tracked against BROWSER_PIPE. What is IN, and why:
#   dom/html_tokenizer/html_tree/dom_serialize  the DOM and its parser
#   layout.c                                    see the note below -- this one
#                                               was a real decision
#   forms.c focus.c                             form control state and focus:
#                                               html/semantics/forms is a whole
#                                               subset and "typing into a page"
#                                               is the user-visible complaint
#   css_engine/css_vars/css_extra               the cascade LibCSS drives
#   url/http1/cookies                           fetch, URL and document.cookie
#   lib/image/*                                 img decode, reached from
#                                               js_dom's <img> and from layout
#
# LINKING layout.c IS A DELIBERATE CHOICE AND THE ALTERNATIVE WAS REAL. Without
# it every box is 0x0, so getBoundingClientRect, offsetWidth and the whole
# check-layout-th.js family fail on geometry for a reason that has nothing to do
# with the layout engine -- hundreds of css/ files that can never pass, sitting
# in the baseline forever as noise a reader has to learn to ignore. A ratchet
# whose entries are permanently unfixable is the thing this project already
# learned not to build. The cost is that the runner is bigger and slower than a
# DOM-only harness; that is the cheaper of the two.
#
# What is deliberately OUT, and why -- each of these is a claim, so each gets a
# reason rather than an omission:
#   browser.c browser_rt.c browser_paint.c tabs.c   the app shell: window
#       management, painting and the tab strip, all through `int 0x80` GUI
#       wrappers. Nothing in this corpus asks about them. browser_rt.c also
#       owns bfetch_*, which tests/unit/wpt_test.c supplies itself against the
#       checkout -- linking both would be two definitions of the fetch.
#   http2.c hpack.c hpool.c   transport. The host build answers requests out of
#       WEBAPI_FILE_ROOT through h1_conn, so no socket and no h2 is reached.
WPT_TEST_SRC := tests/unit/wpt_test.c $(WPT_JS_SRC)
WPT_TEST_SRC += c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_extra.c
WPT_TEST_SRC += c/apps/browser/layout.c c/apps/browser/forms.c c/apps/browser/focus.c
WPT_TEST_SRC += c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c
WPT_TEST_SRC += c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c
WPT_TEST_SRC += c/lib/image/svg.c c/lib/image/exif.c
# js_media.c / js_media_src.c publish HTMLMediaElement, Audio and MediaSource,
# and the interface tests in the corpus DO ask for those -- so they are linked,
# and linking them drags in the demuxer and every codec behind it. That is the
# price of a link that matches the browser's; the alternative was a named
# exception, and a named exception is how the first drift started.
WPT_TEST_SRC += $(wildcard c/lib/media/*.c) $(wildcard c/lib/video/*.c)
WPT_TEST_SRC += $(wildcard c/lib/audio/*.c)
WPT_TEST_SRC += tests/unit/rust_host_shim.c

# The drift check, as a target rather than a habit: every c/apps/browser/js_*.c
# the browser links must be in this runner's list. It is a wildcard on both
# sides now, so this can only fire if someone hand-edits one of them -- which is
# exactly when it needs to fire.
.PHONY: test-wpt-link
test-wpt-link:
	@miss=""; for f in $(sort $(wildcard c/apps/browser/js_*.c)); do \
	    case " $(WPT_TEST_SRC) c/apps/browser/browser.c " in *" $$f "*) ;; \
	    *) miss="$$miss $$f";; esac; done; \
	if [ -n "$$miss" ]; then \
	    echo "test-wpt-link: FAIL -- the browser links these and the runner does not:"; \
	    for f in $$miss; do echo "    $$f"; done; \
	    echo "  Every measurement is then of a browser that does not exist."; exit 1; \
	 else echo "test-wpt-link: ok -- runner links every c/apps/browser/js_*.c"; fi
# -Ic/apps is here and not in BTEST_INC because js_platform.c includes
# "logit.h" (the ring-3 syscall wrappers) unconditionally for its getrandom
# path. On the host those wrappers are never called -- the file's own fallback
# is -- but the header still has to resolve.
WPT_CF := $(BTEST_INC) -Ic/apps -Ic/kernel/mm -Ic/lib/media -Ic/lib/audio -Ic/lib/video $(CSS_INC) $(JS_INC) -Iinclude/abi -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

$(BUILD)/wpt_test: $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WPT_CF) -o $@ $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

WPT_ARGS = --root $(WPT_ROOT) -b $(WPT_BASELINE) $(if $(V),-v $(V),) \
           $(if $(ONLY),--only $(ONLY),) $(if $(SUBSET),--subset $(SUBSET),)

# A large part of WPT is DATA-DRIVEN: the test file's first act is to fetch its
# own JSON and everything after that depends on it. Without a net installed,
# every one of those fetches fails with "could not open a socket" and the file
# reports one red line -- url/url-constructor.any.js is exactly that, one
# subtest in the report standing over 1,004 URL cases that never executed. It
# was not a URL parser scoring 21.7%; it was a URL parser that was never asked.
#
# js_webapi.c's host build answers GETs out of WEBAPI_FILE_ROOT when the
# variable is set, and it does so through the REAL h1_conn parser rather than
# around it -- so the corpus still exercises response parsing, headers, CORS
# and the streaming sink instead of having them bypassed by the harness meant
# to find bugs in them. Unset, nothing changes for any other host test.
WPT_ENV = WEBAPI_FILE_ROOT=$(WPT_ROOT)

# THE GATE. tools/audit_tests.py classifies any `test-*` with a recipe as a
# host suite and `make ci` runs it, so this needs no wiring beyond the name --
# which is the point of that design and the reason 217 targets were once
# orphans.
test-wpt: $(BUILD)/wpt_test
	@$(WPT_ENV) $(BUILD)/wpt_test $(WPT_ARGS) --strict

# The same numbers with the gate off, for a tree that is already red for some
# other reason. Not named test-* on purpose: a target that cannot fail must not
# be counted as a test, which is exactly what test-audit looks for.
wpt: $(BUILD)/wpt_test
	@$(WPT_ENV) $(BUILD)/wpt_test $(WPT_ARGS)

wpt-baseline: $(BUILD)/wpt_test
	@$(WPT_ENV) $(BUILD)/wpt_test $(WPT_ARGS) --write-baseline

wpt-list: $(BUILD)/wpt_test
	@$(WPT_ENV) $(BUILD)/wpt_test $(WPT_ARGS) --list

# --- test-wpt-harness: does testharness.js RUN? -----------------------------
# Finding number one, kept as a target because it is the load-bearing
# assumption under every number this file prints. If testharness.js stops
# installing, test-wpt does not go red case by case -- it collapses to zero and
# the rate looks like a catastrophe in the DOM instead of a broken runner.
# This asserts the harness itself: it loads, test()/assert_equals work, an
# async_test resolves through the timer queue, and the completion callback
# fires with the statuses testharness defines.
test-wpt-harness: $(BUILD)/wpt_test
	@$(WPT_ENV) $(BUILD)/wpt_test --root $(WPT_ROOT) --subset _selfcheck -b /dev/null

# --- test-wpt-fire-negctl: the load event fires ONCE -------------------------
# The runner used to dispatch `load` four times over -- dispatchEvent plus a
# manual on-property call, at document and again at the global, which js_dom.c
# makes the same EventTarget. Nothing failed and the rate went UP, because a
# doubled handler doubles numerator and denominator together; css/css-align
# read 9,296 subtests where it should read 3,308. It also pushed a
# fired-already guard into the shipping browser, which breaks repeating
# handlers like <body onscroll>.
#
# So the self-check counts the load event on both channels, and this proves
# that count can fail: -DWPT_DOUBLE_FIRE restores the second dispatch and the
# self-check must go red.
.PHONY: test-wpt-fire-negctl
test-wpt-fire-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WPT_CF) -DWPT_DOUBLE_FIRE -o $(BUILD)/wpt_fire2 \
	    $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(WPT_ENV) $(BUILD)/wpt_fire2 --root $(WPT_ROOT) --subset _selfcheck \
	       -b /dev/null > $(BUILD)/wpt_fire2.log 2>&1; then \
	    echo "test-wpt-fire-negctl: FAILED -- the self-check passed with the load"; \
	    echo "  event dispatched twice, so it is not counting anything."; exit 1; \
	 else \
	    echo "test-wpt-fire-negctl: ok -- the self-check catches a doubled load:"; \
	    grep -E 'FAIL' $(BUILD)/wpt_fire2.log | head -3; \
	 fi

# --- test-wpt-negctl: the negative control ----------------------------------
# An assertion nobody has watched fail is not a known-failing assertion, and
# the assertion this whole fragment exists to make is "the ratchet goes red
# when a DOM capability that works today stops working". A green run does not
# demonstrate that; only a red one does.
#
# -DWPT_NEGCTL deletes exactly one method the corpus measures --
# document.getElementById, which works today -- and requires `--strict` to exit
# non-zero and to NAME the subtests that regressed. One named method rather
# than a wholesale break on purpose: a control that removes half the DOM would
# go red even if the diff logic were broken, and would prove nothing.
#
# Scoped to dom/ so the control is minutes rather than a full corpus pass; the
# ratchet logic is the same code either way.
# The baseline it diffs against is generated HERE, by the un-doctored runner,
# from the same working tree in the same minute -- NOT the committed one. That
# is the whole difference between a control and a coincidence: against the
# committed baseline any unrelated in-flight browser change also shows as new
# failures, and the target would pass while measuring nothing. With a
# same-tree baseline the ONLY difference between the two runs is the deleted
# method, so every new failure is attributable to it.
test-wpt-negctl: $(BUILD)/wpt_test $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WPT_CF) -DWPT_NEGCTL \
	    -o $(BUILD)/wpt_negctl $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(WPT_ENV) $(BUILD)/wpt_test --root $(WPT_ROOT) --subset dom \
	    --write-baseline -b $(BUILD)/wpt_negctl_base.txt > $(BUILD)/wpt_negctl_base.log 2>&1
	@grep -E '^WPT:' $(BUILD)/wpt_negctl_base.log | sed 's/^/  reference: /'
	@if $(WPT_ENV) $(BUILD)/wpt_negctl --root $(WPT_ROOT) --subset dom --strict \
	       -b $(BUILD)/wpt_negctl_base.txt > $(BUILD)/wpt_negctl.log 2>&1; then \
	    echo "test-wpt-negctl: FAILED -- the suite stayed green with"; \
	    echo "  document.createElement deleted, so the ratchet is not measuring"; \
	    echo "  anything and test-wpt cannot catch a regression."; exit 1; \
	 else \
	    echo "test-wpt-negctl: ok -- the ratchet goes red and names the damage:"; \
	    grep 'NEW FAILURE' $(BUILD)/wpt_negctl.log | head -6; \
	    grep -E '^baseline' $(BUILD)/wpt_negctl.log; \
	 fi

# --- wpt-rank: the ranked cause table ---------------------------------------
# The failure list is thousands of lines and is not a work order. This groups
# them by the CAUSE the message names -- a missing global, a missing property,
# a wrong return -- and ranks the causes by how many subtests each one takes
# out. That ranking is the thing this layer has never had.
wpt-rank: $(BUILD)/wpt_test
	@$(WPT_ENV) $(BUILD)/wpt_test $(WPT_ARGS) --write-baseline -b $(BUILD)/wpt_rank.txt > $(BUILD)/wpt_rank.log 2>&1 || true
	@python3 tools/wpt_rank.py $(BUILD)/wpt_rank.txt $(BUILD)/wpt_rank.log

# --- wpt-fetch: vendor the corpus -------------------------------------------
# A blobless sparse clone of the subsets, then the pieces that are pure size
# with no bearing on this engine are dropped (see tools/wpt_fetch.sh for the
# list and the reason for each). Data only: no runner, no wptserve.
wpt-fetch:
	@bash tools/wpt_fetch.sh $(WPT_ROOT)
