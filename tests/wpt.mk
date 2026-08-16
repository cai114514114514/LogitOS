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

# ===========================================================================
# THE RUNNER MUST BE browser.aex, OR EVERY NUMBER IS OF A BROWSER THAT DOES
# NOT EXIST. There are TWO halves to that and they fail independently.
# ===========================================================================
# HALF ONE -- THE SOURCE LIST -- is below, as a subtraction from the
# Makefile's own variables.
#
# HALF TWO -- THE CALL SEQUENCE -- is in tests/unit/wpt_test.c, and it went
# wrong on its own after half one was fixed. LINKING A TRANSLATION UNIT IS NOT
# RUNNING IT: the runner linked css_extra.c and layout.c and then never called
# css_apply(), css_extra_apply() or layout_page(). `make test-wpt
# ONLY=css/css-grid` read 531/11152 with AND without the grid implementation,
# because with no cascade cstyle::grid_raw is never populated and grid_spec()
# returns -1 before doing anything -- 11,152 subtests structurally
# unreachable, and the line shipping grid unable to tell its work from a
# no-op. css_extra is also the sole producer of the logical properties,
# border-radius and the animation end-state.
#
# A comment cannot hold that. tests/unit/refhost/refrender.c had one -- the
# right one, naming 12d33d6 -- and this runner drifted anyway, because a
# comment in one harness is not a check in another. So the sequence is
# ASSERTED, in the self-check that `make test-wpt-harness` runs in seconds:
# a styled box must have its width (the cascade or layout stopped entirely),
# and a box with margin-inline-start must be offset by it (css_extra_apply
# specifically -- a logical property is one css_extra alone produces, so it is
# the narrowest available probe for that single call).
#
# THE RULE, for whoever is here next: if browser.c gains a step between
# dom_parse and the first paint, it belongs in run_one AND it needs a case in
# SELFCHECK that fails when it is missing. A step nothing asserts is a step
# that will be silently dropped, and the subset that goes quiet will not look
# like a harness bug for weeks.

# THE INVARIANT, in one sentence, because a hand-kept copy of another list is
# the thing that failed three times:
#
#   The runner links exactly what the browser links, MINUS the files named in
#   WPT_BROWSER_OUT below, each with a reason. Nothing here re-states a file
#   the browser already names.
#
# It is written as a SUBTRACTION from the Makefile's own variables --
# $(BROWSER_PIPE) and $(BROWSER_JS_SRC), the same ones $(BUILD)/browser.elf is
# built from -- rather than as a parallel list, because a parallel list has now
# drifted three times and each time the suite reported a browser that did not
# exist:
#
#   1. js_events.c committed, runner did not link it.
#   2. js_cssom.c committed, runner did not link it -- and the suite went on
#      reporting 841 dead `<body onload>` files as a browser defect while the
#      fix sat in the tree (commit 12d33d6, "the runner was measuring a browser
#      that does not exist").
#   3. TONIGHT, while this fragment was being edited: layout.c grew a call to
#      layout_flex_run() and the runner's hand-written pipeline list did not
#      have layout_flex.c, so `make test-wpt` stopped LINKING. That one was
#      loud. The first two were silent, which is the worse failure and the
#      reason a link check has to be structural rather than a habit.
#
# A subtraction cannot drift: a file added to the browser is in this runner the
# same minute, and a file the runner must NOT have has to be named here, once,
# with its reason. test-wpt-link then only has to check that every name in
# WPT_BROWSER_OUT is still a file the browser actually links -- i.e. that no
# exclusion has gone stale and is silently keeping nothing out.
#
# js_*.c is ALSO taken as a wildcard on top of the subtraction, belt and
# braces: BROWSER_JS_SRC is that same wildcard today, and if a line ever
# replaces it with a hand-written list this fragment does not follow it down.
#
# NOTHING ELSE IS WILDCARDED, and the third drift is the reason. The obvious
# reflex on hitting it was `$(wildcard c/apps/browser/layout_*.c)` -- take the
# whole layout family so the runner is never behind. That is WRONG here and
# fails loudly: layout.c integrates layout_flex.c with `#include
# "layout_flex.c"` (line 22), so the wildcard produces duplicate definitions of
# layout_flex_run. BROWSER_PIPE knew that and the wildcard did not. Which is
# the argument for the subtraction restated: the browser's own list is the only
# thing that knows how the browser is built, so ASK IT rather than guess
# alongside it.
ifeq ($(strip $(BROWSER_PIPE)),)
WPT_LINK_ERR := tests/wpt.mk: BROWSER_PIPE is empty -- this fragment must be included from the Makefile, AFTER it. Refusing to link a runner from a partial source list.
endif

# What is deliberately OUT, and why -- each of these is a claim, so each gets a
# reason rather than an omission:
#   browser.c browser_rt.c browser_paint.c tabs.c   the app shell: window
#       management, painting and the tab strip, all through `int 0x80` GUI
#       wrappers. Nothing in this corpus asks about them. browser_rt.c also
#       owns bfetch_*, which tests/unit/wpt_test.c supplies itself against the
#       checkout -- linking both would be two definitions of the fetch.
#   http2.c hpack.c hpool.c   transport. The host build answers requests out of
#       WEBAPI_FILE_ROOT through h1_conn, so no socket and no h2 is reached.
WPT_BROWSER_OUT := c/apps/browser/browser.c c/apps/browser/browser_rt.c \
                   c/apps/browser/browser_paint.c c/apps/browser/tabs.c \
                   c/net/http/http2.c c/net/http/hpack.c c/net/http/hpool.c

# $(HTML_PARSER_SRC) is subtracted only because the link line passes it
# separately (it is shared with test-html5lib and the dom tests); it is IN the
# runner, twice would be duplicate symbols.
WPT_FROM_BROWSER := $(filter-out $(WPT_BROWSER_OUT) $(HTML_PARSER_SRC),\
    $(sort $(BROWSER_PIPE) $(BROWSER_JS_SRC) c/apps/browser/css_engine.c \
           $(wildcard c/apps/browser/js_*.c)))
WPT_JS_SRC := $(filter c/apps/browser/js_%,$(WPT_FROM_BROWSER))

# LINKING layout.c IS A DELIBERATE CHOICE AND THE ALTERNATIVE WAS REAL, and it
# was re-put and re-decided the same way. Without it every box is 0x0, so
# getBoundingClientRect, offsetWidth and the whole check-layout-th.js family
# fail on geometry for a reason that has nothing to do with the layout engine --
# hundreds of css/ files that can never pass, sitting in the baseline forever as
# noise a reader has to learn to ignore. A ratchet whose entries are
# permanently unfixable is the thing this project already learned not to build,
# and css/ is the subset the 60% target turns on. The cost is that the runner
# is bigger and slower than a DOM-only harness, and that its text metric is a
# monospace approximation rather than a rasterised TrueType advance (see
# text_measure in the runner) -- so glyph-advance geometry is measured against
# an approximation while BOX-MODEL geometry, which is nearly all of it, is
# measured against layout.c. That is the cheaper of the two errors by a wide
# margin, and the expensive one is the one that cannot ever be repaired.
WPT_TEST_SRC := tests/unit/wpt_test.c $(WPT_FROM_BROWSER)
# js_media.c / js_media_src.c publish HTMLMediaElement, Audio and MediaSource,
# and the interface tests in the corpus DO ask for those -- so they are linked,
# and linking them drags in the demuxer and every codec behind it. That is the
# price of a link that matches the browser's; the alternative was a named
# exception, and a named exception is how the first drift started.
WPT_TEST_SRC += $(wildcard c/lib/media/*.c) $(wildcard c/lib/video/*.c)
WPT_TEST_SRC += $(wildcard c/lib/audio/*.c)
WPT_TEST_SRC += tests/unit/rust_host_shim.c

# The drift check, as a target rather than a habit. Two questions, and after
# the subtraction above they are the only two left that can go wrong:
#
#   1. Is every browser TU either linked here or NAMED in WPT_BROWSER_OUT?
#      (Structurally yes -- unless someone rewrites the derivation by hand,
#      which is exactly when this has to fire.)
#   2. Is every name in WPT_BROWSER_OUT still a file the browser links? A
#      stale exclusion excludes nothing and quietly stops being a decision;
#      worse, a RENAMED file leaves its old name here and its new one linked,
#      which reads as if the exclusion still holds.
.PHONY: test-wpt-link
test-wpt-link:
	@if [ -n "$(WPT_LINK_ERR)" ]; then echo "$(WPT_LINK_ERR)"; exit 1; fi
	@miss=""; for f in $(sort $(BROWSER_PIPE) $(BROWSER_JS_SRC) $(wildcard c/apps/browser/js_*.c)); do \
	    case " $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(WPT_BROWSER_OUT) " in *" $$f "*) ;; \
	    *) miss="$$miss $$f";; esac; done; \
	if [ -n "$$miss" ]; then \
	    echo "test-wpt-link: FAIL -- the browser links these, and the runner neither"; \
	    echo "  links them nor names them in WPT_BROWSER_OUT:"; \
	    for f in $$miss; do echo "    $$f"; done; \
	    echo "  Every measurement is then of a browser that does not exist."; exit 1; \
	 fi; \
	stale=""; for f in $(WPT_BROWSER_OUT); do \
	    case " $(BROWSER_PIPE) $(BROWSER_JS_SRC) " in *" $$f "*) ;; \
	    *) stale="$$stale $$f";; esac; done; \
	if [ -n "$$stale" ]; then \
	    echo "test-wpt-link: FAIL -- WPT_BROWSER_OUT names files the browser does"; \
	    echo "  not link. The exclusion excludes nothing, so it has stopped being"; \
	    echo "  a decision -- and if the file was RENAMED, its new name is linked"; \
	    echo "  here while this reads as if it were still out:"; \
	    for f in $$stale; do echo "    $$f"; done; exit 1; \
	 fi; \
	echo "test-wpt-link: ok -- runner = browser minus $(words $(WPT_BROWSER_OUT)) named files;"; \
	echo "  $(words $(WPT_FROM_BROWSER)) shared TUs, every exclusion still real."
# -Ic/apps is here and not in BTEST_INC because js_platform.c includes
# "logit.h" (the ring-3 syscall wrappers) unconditionally for its getrandom
# path. On the host those wrappers are never called -- the file's own fallback
# is -- but the header still has to resolve.
WPT_CF := $(BTEST_INC) -Ic/apps -Ic/kernel/mm -Ic/lib/media -Ic/lib/audio -Ic/lib/video $(CSS_INC) $(JS_INC) -Iinclude/abi -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

$(BUILD)/wpt_test: $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@if [ -n "$(WPT_LINK_ERR)" ]; then echo "$(WPT_LINK_ERR)"; exit 1; fi
	@# $(GFX_SRC): svg.c builds its shapes with gfx_path/gfx_fill since the
	@# phase-2 stroke work landed, so every host link that takes svg.c and
	@# not the engine now fails on gfx_path_ellipse. It has been failing here
	@# silently -- no suite reaches these targets -- until a sweep of all 522
	@# tried them.
	@$(CC) -O2 -w $(WPT_CF) -o $@ $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(GFX_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# J= the number of files run at once. Isolation is per-file and results are
# consumed in file order, so J changes wall time and no number; a full corpus
# pass is hours at J=1 and well under one at J=8. ORDER= seeds the shuffle.
WPT_ARGS = --root $(WPT_ROOT) -b $(WPT_BASELINE) $(if $(V),-v $(V),) \
           $(if $(ONLY),--only $(ONLY),) $(if $(SUBSET),--subset $(SUBSET),) \
           --jobs $(if $(J),$(J),8) $(if $(ORDER),--shuffle $(ORDER),)

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

# --- test-wpt-depth: dispatchEvent is bounded -------------------------------
# js_events.c's dispatchEvent is a native trampoline (JS -> C dispatcher -> JS
# listener), so a listener that dispatches again recurses through frames that
# are only partly JS. There was no depth counter in that file at all, and
# unbounded native recursion is a SIGSEGV -- no exception, no stack, no failing
# subtest, just a dead process. It is now bounded at 64, and the test asserts
# the bound EXACTLY rather than "it threw": with the guard removed the depth
# reached is 450 (where QuickJS's own 2 MiB limit trips on the cheapest
# dispatch path), so the first assertion is its own negative control. See the
# comment above MAX_DISPATCH_DEPTH in c/apps/browser/js_events.c for why 64.
.PHONY: test-wpt-depth
test-wpt-depth: $(BUILD)/wpt_test
	@if [ ! -d "$(WPT_ROOT)/resources" ]; then \
	    echo "test-wpt-depth: no corpus at $(WPT_ROOT) -- testharness.js is upstream's."; \
	    echo "  Not a regression; point WPT_ROOT at a checkout."; exit 0; fi
	@ln -sfn "$(abspath $(WPT_ROOT))/resources" tests/wpt-local/resources
	@$(BUILD)/wpt_test --root tests/wpt-local --subset platform \
	    --only event-dispatch-depth -b /dev/null > $(BUILD)/wpt_depth.log 2>&1; \
	 line=$$(grep -E '^WPT: ' $(BUILD)/wpt_depth.log); \
	 echo "test-wpt-depth: $$line"; \
	 case "$$line" in \
	   "WPT: 3/3 subtests passed"*) echo "test-wpt-depth: ok -- dispatch stops at 64 and throws";; \
	   *) echo "test-wpt-depth: FAILED -- the dispatch bound is not what it says:"; \
	      grep -E 'FAIL|CRASH|HARNESS' $(BUILD)/wpt_depth.log | head -6; exit 1;; \
	 esac

# --- test-wpt-order: the same corpus, two orders, identical numbers ---------
# THE ACCEPTANCE TEST FOR ISOLATION, and it is one line: run the corpus twice
# in two different random file orders and require the two baselines to be
# identical as SETS.
#
# That single assertion covers all three of the things this runner was rebuilt
# for. It cannot pass if a file crashes the process, because the run would not
# finish. It cannot pass if state leaks between files, because a leak makes a
# file's result depend on what ran before it and the orders differ. And it
# cannot pass if the numbers are not reproducible, which is what "97.5%" and
# every other percentage this project has quoted silently assumed.
#
# Compared as sorted sets, not as files: the baseline is written in file order,
# so a shuffled run writes the same entries in a different order by
# construction and diffing raw would fail on nothing.
#
# SUBSET= scopes it (default: the whole corpus, which is the point). Two seeds
# rather than sorted-vs-shuffled, so neither run is the "normal" one.
.PHONY: test-wpt-order
test-wpt-order: $(BUILD)/wpt_test
	@if [ ! -d "$(WPT_ROOT)" ]; then \
	    echo "test-wpt-order: no corpus at $(WPT_ROOT) -- nothing to measure."; exit 0; fi
	@$(BUILD)/wpt_test --root $(WPT_ROOT) $(if $(SUBSET),--subset $(SUBSET),) --list \
	    | sort > $(BUILD)/wpt_order_files_before.txt
	@echo "test-wpt-order: run 1 of 2 (order seed 20260809)"
	@$(WPT_ENV) $(BUILD)/wpt_test --root $(WPT_ROOT) $(if $(SUBSET),--subset $(SUBSET),) \
	    --jobs $(if $(J),$(J),8) --shuffle 20260809 \
	    --write-baseline -b $(BUILD)/wpt_order_a.txt > $(BUILD)/wpt_order_a.log 2>&1 || true
	@echo "test-wpt-order: run 2 of 2 (order seed 77)"
	@$(WPT_ENV) $(BUILD)/wpt_test --root $(WPT_ROOT) $(if $(SUBSET),--subset $(SUBSET),) \
	    --jobs $(if $(J),$(J),8) --shuffle 77 \
	    --write-baseline -b $(BUILD)/wpt_order_b.txt > $(BUILD)/wpt_order_b.log 2>&1 || true
	@grep -E '^WPT: ' $(BUILD)/wpt_order_a.log | sed 's/^/  order A: /'
	@grep -E '^WPT: ' $(BUILD)/wpt_order_b.log | sed 's/^/  order B: /'
	@grep -v '^#' $(BUILD)/wpt_order_a.txt | sort > $(BUILD)/wpt_order_a.set
	@grep -v '^#' $(BUILD)/wpt_order_b.txt | sort > $(BUILD)/wpt_order_b.set
# DID THE CORPUS HOLD STILL? Asked FIRST, because otherwise its answer is
# delivered as the other question's. It happened on the first full green run:
# the two orders differed by exactly one entry,
# css/css-text/zzprobe2.html::*, which was a probe file another line wrote
# into third_party/wpt between the runs and deleted afterwards. The corpus
# moved; isolation did not fail. Reported as "identical numbers except one"
# that is a confusing near-miss on the headline assertion of this suite, and
# somebody would have gone looking in the runner for a day.
	@$(BUILD)/wpt_test --root $(WPT_ROOT) $(if $(SUBSET),--subset $(SUBSET),) --list \
	    | sort > $(BUILD)/wpt_order_files_after.txt
	@if ! diff -q $(BUILD)/wpt_order_files_before.txt \
	              $(BUILD)/wpt_order_files_after.txt > /dev/null; then \
	    echo "test-wpt-order: INCONCLUSIVE -- the corpus changed while the two runs"; \
	    echo "  were in flight, so the two orders were not asked the same question."; \
	    echo "  This is not an isolation failure and not a result. Files that came"; \
	    echo "  or went (a stray probe written into $(WPT_ROOT) is the usual cause):"; \
	    diff $(BUILD)/wpt_order_files_before.txt $(BUILD)/wpt_order_files_after.txt | head -10; \
	    exit 1; \
	 fi
	@if ! diff -q $(BUILD)/wpt_order_a.set $(BUILD)/wpt_order_b.set > /dev/null; then \
	    echo "test-wpt-order: FAILED -- the same corpus in two orders is not the"; \
	    echo "  same result. Every percentage this suite prints is then one sample"; \
	    echo "  from an unknown distribution. Entries that differ:"; \
	    diff $(BUILD)/wpt_order_a.set $(BUILD)/wpt_order_b.set | head -20; \
	    echo "  ($$(diff $(BUILD)/wpt_order_a.set $(BUILD)/wpt_order_b.set | grep -c '^[<>]') lines)"; \
	    exit 1; \
	 fi
	@grep -E '^WPT: |subtests \|' $(BUILD)/wpt_order_a.log > $(BUILD)/wpt_order_a.sum
	@grep -E '^WPT: |subtests \|' $(BUILD)/wpt_order_b.log > $(BUILD)/wpt_order_b.sum
	@if ! diff -q $(BUILD)/wpt_order_a.sum $(BUILD)/wpt_order_b.sum > /dev/null; then \
	    echo "test-wpt-order: FAILED -- identical failure SETS but different counts."; \
	    diff $(BUILD)/wpt_order_a.sum $(BUILD)/wpt_order_b.sum; exit 1; \
	 fi
	@echo "test-wpt-order: ok -- two random orders, identical per-subtest results"
	@echo "  ($$(grep -vc '^#' $(BUILD)/wpt_order_a.set) failure entries, byte-identical as sets)"

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
