# tests/domparser.mk -- `new DOMParser().parseFromString(...)`.
#
#   make test-domparser   host, seconds: parse a fragment, query it, read/write
#                         textContent, traverse it, refuse XML loudly, and (the
#                         negative control) watch a wrapper into a node that a
#                         later textContent= recycled come back "undefined"
#                         instead of a wrong-but-plausible live answer. A second
#                         half opens a REAL js_page.c runtime, proves the parsed
#                         document never touches the live page's `document`,
#                         then js_page_close()s with live references to the
#                         parsed document still held in globalThis and opens a
#                         second page afterwards -- the sharpest place a wrong
#                         lifetime scheme (arena freed too early, a finalizer
#                         touching memory the runtime already dropped) would
#                         show up.
#
# Own fragment for the reason every other one here gives (tests/domsub.mk's
# header states it fully): several agents edit the Makefile at once and a
# whole-file write loses somebody's work. One `-include` line is the footprint.
#
# The link is deliberately narrower than test-dom-bindings': no layout.c, no
# image codecs. js_domparser.c never touches layout or CSS (js_dom.c's own
# layout access is behind WEAK externs it resolves to "absent" without
# layout.c on the link line), so pulling that in here would only blur what a
# red run points at. css_engine.c/css_vars.c, by contrast, ARE required even
# though js_domparser.c never calls them: js_dom.c's `style` object (installed
# by js_dom_init, which js_page_open calls unconditionally in Part B) calls
# css_known_prop_count/css_prop_lookup/css_specified_canon/etc. directly, not
# through a weak extern -- link without them and it's an undefined-reference
# error, not a quiet "absent" fallback. (Checked by actually dropping them
# from DOMPARSER_SRC and watching the link fail with exactly those symbols.)
# What IS required: the HTML parser js_domparser.c calls straight into
# (dom_parse == html_parse), QuickJS, and js_dom.c + js_page.c for Part B's
# real page-runtime lifecycle -- built with every OTHER installer they call
# absent (weak, "only if present"), which is itself a supported link and not
# a special case.

.PHONY: test-domparser

DOMPARSER_SRC := tests/unit/domparser_test.c \
                  c/apps/browser/js_domparser.c c/apps/browser/js_dom.c \
                  c/apps/browser/js_page.c \
                  c/apps/browser/css_engine.c c/apps/browser/css_vars.c

$(BUILD)/domparser_test: $(DOMPARSER_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' \
	    -o $@ $(DOMPARSER_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm

test-domparser: $(BUILD)/domparser_test
	@$(BUILD)/domparser_test
