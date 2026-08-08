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

# The runner links the SHIPPING files -- js_page.c, js_dom.c, js_webapi.c,
# js_platform.c, js_select.c, js_intl.c, js_module.c and the real HTML parser --
# for the same reason webapi_platform_test.c does: a runner over stubs measures
# the stubs. css_engine/css_vars come along because js_page.c's style paths
# call into them, and http1/url/cookies because js_webapi.c's fetch and URL do.
WPT_TEST_SRC := tests/unit/wpt_test.c c/apps/browser/js_page.c c/apps/browser/js_dom.c
WPT_TEST_SRC += c/apps/browser/css_engine.c c/apps/browser/css_vars.c
WPT_TEST_SRC += c/apps/browser/js_webapi.c c/apps/browser/js_platform.c
WPT_TEST_SRC += c/apps/browser/js_select.c c/apps/browser/js_intl.c c/apps/browser/js_module.c
WPT_TEST_SRC += c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c
WPT_TEST_SRC += tests/unit/rust_host_shim.c
# -Ic/apps is here and not in BTEST_INC because js_platform.c includes
# "logit.h" (the ring-3 syscall wrappers) unconditionally for its getrandom
# path. On the host those wrappers are never called -- the file's own fallback
# is -- but the header still has to resolve.
WPT_CF := $(BTEST_INC) -Ic/apps $(CSS_INC) $(JS_INC) -Iinclude/abi -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

$(BUILD)/wpt_test: $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WPT_CF) -o $@ $(WPT_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

WPT_ARGS = --root $(WPT_ROOT) -b $(WPT_BASELINE) $(if $(V),-v $(V),) \
           $(if $(ONLY),--only $(ONLY),) $(if $(SUBSET),--subset $(SUBSET),)

# THE GATE. tools/audit_tests.py classifies any `test-*` with a recipe as a
# host suite and `make ci` runs it, so this needs no wiring beyond the name --
# which is the point of that design and the reason 217 targets were once
# orphans.
test-wpt: $(BUILD)/wpt_test
	@$(BUILD)/wpt_test $(WPT_ARGS) --strict

# The same numbers with the gate off, for a tree that is already red for some
# other reason. Not named test-* on purpose: a target that cannot fail must not
# be counted as a test, which is exactly what test-audit looks for.
wpt: $(BUILD)/wpt_test
	@$(BUILD)/wpt_test $(WPT_ARGS)

wpt-baseline: $(BUILD)/wpt_test
	@$(BUILD)/wpt_test $(WPT_ARGS) --write-baseline

wpt-list: $(BUILD)/wpt_test
	@$(BUILD)/wpt_test $(WPT_ARGS) --list

# --- test-wpt-harness: does testharness.js RUN? -----------------------------
# Finding number one, kept as a target because it is the load-bearing
# assumption under every number this file prints. If testharness.js stops
# installing, test-wpt does not go red case by case -- it collapses to zero and
# the rate looks like a catastrophe in the DOM instead of a broken runner.
# This asserts the harness itself: it loads, test()/assert_equals work, an
# async_test resolves through the timer queue, and the completion callback
# fires with the statuses testharness defines.
test-wpt-harness: $(BUILD)/wpt_test
	@$(BUILD)/wpt_test --root $(WPT_ROOT) --subset _selfcheck -b /dev/null

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
	@$(BUILD)/wpt_test --root $(WPT_ROOT) --subset dom \
	    --write-baseline -b $(BUILD)/wpt_negctl_base.txt > $(BUILD)/wpt_negctl_base.log 2>&1
	@grep -E '^WPT:' $(BUILD)/wpt_negctl_base.log | sed 's/^/  reference: /'
	@if $(BUILD)/wpt_negctl --root $(WPT_ROOT) --subset dom --strict \
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
	@$(BUILD)/wpt_test $(WPT_ARGS) --write-baseline -b $(BUILD)/wpt_rank.txt > $(BUILD)/wpt_rank.log 2>&1 || true
	@python3 tools/wpt_rank.py $(BUILD)/wpt_rank.txt $(BUILD)/wpt_rank.log

# --- wpt-fetch: vendor the corpus -------------------------------------------
# A blobless sparse clone of the subsets, then the pieces that are pure size
# with no bearing on this engine are dropped (see tools/wpt_fetch.sh for the
# list and the reason for each). Data only: no runner, no wptserve.
wpt-fetch:
	@bash tools/wpt_fetch.sh $(WPT_ROOT)
