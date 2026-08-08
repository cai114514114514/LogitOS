# tests/url.mk -- the WHATWG URL Standard (c/apps/browser/js_url.c).
#
#   make test-url            the parser against the corpus's OWN data, seconds
#   make test-url V=40       ... plus the first 40 unexpected failures
#   make test-url-js         the JS surface: URL, URLSearchParams, the live link
#   make test-url-negctl     the negative control -- the SAME file built as a
#                            splitter, which MUST fail the suite
#   make url-baseline        re-cut tests/unit/url_expected_fail.txt
#
# WHY THE DATA AND NOT THE RUNNER. url/url-constructor.any.js is a twelve-line
# loop over url/resources/urltestdata.json, and url/url-setters.any.js is the
# same over setters_tests.json. Those two files hold 891 + 278 cases. Under the
# WPT runner they are ONE red line each today, because the runner cannot finish
# the 267 KB fetch that loads them (tests/wpt.mk's line owns that; this one
# must not touch it). Driving the JSON directly measures the same property, in
# a second, and does not go stale when the runner is fixed -- at which point
# the two numbers should agree and any disagreement is a bug in one of them.
#
# What the JSON CANNOT measure is the JS surface: that the getters are
# accessors on the prototype, that the constructor throws a TypeError instead
# of returning null, that `url.searchParams` mutates the URL and the URL's
# `search` mutates the params. That is test-url-js, which links QuickJS.
#
# Own fragment rather than lines in the Makefile, for the reason every other
# fragment here gives: several lines edit that file at once and a whole-file
# write loses somebody's targets. One `-include` line is the entire footprint.

.PHONY: test-url test-url-js test-url-negctl test-url-all url-baseline

URL_SRC     := tests/unit/url_test.c c/apps/browser/js_url.c
URL_CF      := -Ic/apps/browser -DURL_CORE_ONLY
URL_BASE    := tests/unit/url_expected_fail.txt

# -DURL_CORE_ONLY compiles js_url.c with the algorithm and no QuickJS at all,
# which is why this builds and runs in a second. The bindings at the bottom of
# that file are covered by test-url-js below and by the corpus run.
$(BUILD)/url_test: $(URL_SRC) c/apps/browser/js_url.h $(URL_BASE)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(URL_CF) -o $@ $(URL_SRC)

test-url: $(BUILD)/url_test
	@$(BUILD)/url_test $(if $(V),-v$(V),)

url-baseline: $(BUILD)/url_test
	@echo "# tests/unit/url_expected_fail.txt -- cases known to fail." > $(URL_BASE).new
	@echo "# Re-cut with: make url-baseline. Everything not listed here is gated." >> $(URL_BASE).new
	@URL_EXPECT=/dev/null $(BUILD)/url_test -n 2>&1 | sed -n 's/^  FAIL \([^:]*:[^:]*\(:[^:]*\)\?\):.*/\1/p' >> $(URL_BASE).new || true
	@mv $(URL_BASE).new $(URL_BASE)
	@echo "url-baseline: wrote $(URL_BASE)"

# --- the JS surface ---------------------------------------------------------
# Links the SHIPPING file with its bindings compiled in, plus QuickJS. No DOM,
# no network: js_url.c installs onto a bare context, which is also the property
# this asserts -- the URL globals must not need the rest of the browser.
URLJS_SRC := tests/unit/url_js_test.c c/apps/browser/js_url.c
# -DCONFIG_BIGNUM for libbf's decimal symbols -- the same note CLAUDE.md
# records for the host LibCSS tests; without it libbf links short.
URLJS_CF  := -Ic/apps/browser $(JS_INC) -DCONFIG_VERSION='"host"' -DCONFIG_BIGNUM

$(BUILD)/url_js_test: $(URLJS_SRC) c/apps/browser/js_url.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(URLJS_CF) -o $@ $(URLJS_SRC) $(QJS_SRC) -lm

test-url-js: $(BUILD)/url_js_test
	@$(BUILD)/url_js_test

# --- the negative control ---------------------------------------------------
# NOT "remove the parser" -- that proves only that a missing thing is missing.
# -DURL_NAIVE_SPLIT replaces the state machine inside the SAME shipping file
# with the parser a competent person writes first: split the input on ':', '/',
# '?' and '#', keep each piece verbatim, and let the serializer put it back
# together with the same delimiters. Everything else -- the record, the
# serializer, all eleven setters, the bindings -- is the real code.
#
# That build parses every URL a human would type and produces correct-LOOKING
# output. What it cannot do is NORMALIZE: no dot-segment removal, no default
# port dropped, no lowercased scheme in a host, no percent-encoding set, no
# IPv4 or IPv6 host, no opaque-vs-special distinction. So the suite must go
# deep red, and this target FAILS if it does not -- a suite that stays green on
# a splitter is not measuring a URL parser, it is measuring that strings come
# back out.
test-url-negctl: $(URL_SRC) $(URL_BASE)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(URL_CF) -DURL_NAIVE_SPLIT -o $(BUILD)/url_negctl $(URL_SRC)
	@if $(BUILD)/url_negctl > $(BUILD)/url_negctl.log 2>&1; then \
	    echo "test-url-negctl: FAILED -- the suite PASSED on a parser that is"; \
	    echo "  just a splitter, so it is not measuring the URL algorithm."; \
	    exit 1; \
	 else \
	    echo "test-url-negctl: ok -- the splitter fails the corpus:"; \
	    grep -E '^  (constructor|setters|encoding|URLSearch)' $(BUILD)/url_negctl.log; \
	    tail -2 $(BUILD)/url_negctl.log; \
	 fi

test-url-all: test-url test-url-js test-url-negctl

# Wired into an aggregate, which is the whole point of tools/audit_tests.py's
# ORPHAN finding: a target no suite reaches is a target nobody runs. `ci-host`
# is one of the roots that script treats as a suite; adding prerequisites to it
# from a fragment is additive, so this does not collide with anyone else doing
# the same.
ci-host: test-url test-url-js test-url-negctl
