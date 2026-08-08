# tests/selectors.mk -- the query-side selector engine and DOMTokenList
# (c/apps/browser/js_select.c + c/apps/browser/js_tokenlist.c).
#
#   make test-selectors          the suite, host-side, seconds
#   make test-selectors-negctl   THE NEGATIVE CONTROL: the same binary built
#                                with every NAME compared case-sensitively,
#                                which must FAIL
#
# Own fragment rather than lines in the Makefile, for the reason every other
# fragment here gives: several agents edit that file at once and a whole-file
# write loses somebody's work. One `-include` line is the entire footprint.
#
# WHY A SUITE AT ALL WHEN WPT ALREADY MEASURES THIS. `make test-wpt` covers
# these two files far more thoroughly than this ever will, and takes minutes to
# do it over a corpus that has to be vendored. This is for the other half of a
# suite's job: it runs in seconds, it needs no corpus, and -- the part that
# matters -- it is a thing that can be MADE TO FAIL on purpose, which is the
# only evidence that its passing runs mean anything.
#
# WHY THE CONTROL IS CASE AND NOT "DELETE THE MATCHER". A control that removes
# selector matching would go red even if the assertions were nonsense, and
# would prove only that the file is linked. -DSELECT_CASE_SENSITIVE instead
# compares every NAME case-sensitively -- element names in type selectors,
# attribute names in attribute selectors, the argument to getElementsByTagName
# -- and turns off quirks-mode class/id folding. The result is a browser that
# renders every lowercase page in the world exactly as it does today. What
# stops working is precisely:
#
#     document.querySelector('DIV')            -> null
#     [ALIGN=left] against align="LEFT"        -> no match
#     .Warning against class="warning"         -> no match on a quirks page
#     getElementsByTagName('P')                -> empty
#
# That is not a strawman: it is what a careful implementation looks like when
# nobody has told the author that HTML is ASCII-case-insensitive about names,
# and it is exactly the bug the two case files in this cluster
# (css/selectors/attribute-selectors/attribute-case/semantics.html and
# dom/nodes/case.html, 1,137 subtests between them) exist to catch.
#
# The control is ONE FLAG, not a second copy of the prelude, so the doctored
# build runs the same parser and the same matcher as the real one. A forked
# copy would be a control over the fork.
#
# The suite counts its case assertions separately and prints the split, so a
# reader can see that the control is breaking the group it is aimed at rather
# than knocking the binary over some other way.

.PHONY: test-selectors test-selectors-negctl

# The SHIPPING files, not stubs, and the same set dom_iface_test.c links: this
# measures js_select.c's methods reaching a real element prototype through the
# real install order in js_page.c, and half of that property belongs to the
# files around it. A runner over stubbed DOM files would measure the stubs.
SELECTORS_SRC := tests/unit/selectors_test.c \
                 c/apps/browser/js_page.c c/apps/browser/js_dom.c \
                 c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                 c/apps/browser/js_webapi.c c/apps/browser/js_platform.c \
                 c/apps/browser/js_select.c c/apps/browser/js_tokenlist.c \
                 c/apps/browser/js_intl.c c/apps/browser/js_module.c \
                 c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c \
                 tests/unit/rust_host_shim.c
# -Ic/apps because js_platform.c includes "logit.h" unconditionally; see the
# same note in tests/wpt.mk.
SELECTORS_CF := $(BTEST_INC) -Ic/apps $(CSS_INC) $(JS_INC) -Iinclude/abi \
                -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

$(BUILD)/selectors_test: $(SELECTORS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(SELECTORS_CF) -o $@ $(SELECTORS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-selectors: $(BUILD)/selectors_test
	@$(BUILD)/selectors_test

test-selectors-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(SELECTORS_CF) -DSELECT_CASE_SENSITIVE \
	    -o $(BUILD)/selectors_negctl $(SELECTORS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/selectors_negctl > $(BUILD)/selectors_negctl.log 2>&1; then \
	    echo "test-selectors-negctl: FAILED -- the suite PASSED with every name"; \
	    echo "  compared case-sensitively, so its case assertions are measuring"; \
	    echo "  nothing and test-selectors cannot catch that regression."; exit 1; \
	 else \
	    echo "test-selectors-negctl: ok -- case-sensitive names break the suite:"; \
	    grep '^FAIL' $(BUILD)/selectors_negctl.log | head -6; \
	    tail -2 $(BUILD)/selectors_negctl.log; \
	 fi
