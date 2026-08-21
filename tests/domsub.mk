# tests/domsub.mk -- the URL Standard where it meets an element.
#
#   make test-urlelem          <a>/<area> URL decomposition against WPT's own
#                              urltestdata.json + setters_tests.json, host-side,
#                              seconds
#   make test-urlelem-negctl   the negative control: the SAME binary built with
#                              a split-and-concatenate decomposition, which must
#                              FAIL
#
# Own fragment rather than lines in the Makefile, for the reason every other
# fragment here gives: several agents edit that file at once and a whole-file
# write loses somebody's work. One `-include` line is the entire footprint.
#
# WHY THIS EXISTS ALONGSIDE test-url AND test-wpt, which both already cover
# some of it -- because neither answers the question this one asks.
#
#   test-url  drives urltestdata.json straight into js_url.c's parser and has
#             had it at 1201/1201 since it landed. That measures the ALGORITHM.
#             `<a href>` is a different question with three more moving parts
#             (the document base URL, the null-url case, and href agreeing with
#             the ten components), and every one of them was wrong while that
#             number said 100%.
#   test-wpt  does measure url/a-element.html, and is the authority on the
#             rate. It is also a full corpus pass: minutes, a QuickJS runtime
#             per file, and a ratchet whose job is to notice ANY change
#             anywhere. This is seconds, it is scoped to one surface, and --
#             the part test-wpt structurally cannot have -- it has a negative
#             control that makes the surface fail on demand.
#
# THE CORPUS IS OPTIONAL AND THE CAPABILITY IS NOT, the rule tests/wpt.mk
# states and this follows: with no checkout the binary prints why and exits 0,
# and the control below detects that and does not claim a red run it did not
# get.

.PHONY: test-urlelem test-urlelem-negctl

URLELEM_ROOT ?= build/wpt

# The link. It is the SHIPPING browser's JS layer, not a stub of it -- the
# reason tests/wpt.mk gives for the same choice: a harness over stubs measures
# the stubs. js_urlbind.c needs js_url.c (the parser), js_dom.c (the node and
# its attributes), js_reflect.c (which owns `href` on every OTHER element and
# whose resolve_url now reads document.baseURI from here), js_tokenlist.c
# (js_reflect reaches for it) and js_page.c (the runtime). js_forms.c,
# js_events.c, js_cssom.c and js_media.c are absent on purpose: every installer
# in this directory is weak and conditional, so their absence is a supported
# link and not a special case -- which is itself worth having one target prove.
URLELEM_SRC := tests/unit/urlelem_test.c \
               c/apps/browser/js_page.c c/apps/browser/js_dom.c \
               c/apps/browser/js_webapi.c c/apps/browser/js_platform.c \
               c/apps/browser/js_select.c c/apps/browser/js_intl.c \
               c/apps/browser/js_module.c c/apps/browser/js_tokenlist.c \
               c/apps/browser/js_reflect.c \
               c/apps/browser/js_url.c c/apps/browser/js_urlbind.c \
               c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
               c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c \
               tests/unit/rust_host_shim.c
# -Ic/apps because js_platform.c includes "logit.h" unconditionally; see the
# same note in tests/wpt.mk.
URLELEM_CF := $(BTEST_INC) -Ic/apps $(CSS_INC) $(JS_INC) -Iinclude/abi \
              -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

$(BUILD)/urlelem_test: $(URLELEM_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(URLELEM_CF) -o $@ $(URLELEM_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-urlelem: $(BUILD)/urlelem_test
	@$(BUILD)/urlelem_test $(URLELEM_ROOT)

# --- the negative control ---------------------------------------------------
# -DURLELEM_SPLITTER swaps js_urlbind.c's decomposition for a split on ':' '/'
# '?' '#' with reassembly by concatenation. Read the block that implements it
# in that file for why this is the honest control: it is the implementation
# almost everyone writes first, it answers correctly for every URL a human
# types, and the URL line's equivalent for `URL` itself scored 251/891 on the
# same corpus. Nothing else about the file changes -- the base URL, the
# install, the three refusing entry points are identical in both builds -- so
# a red run names the decomposition and nothing else.
#
# The two failures it CANNOT avoid, which is what makes it a control rather
# than a coincidence:
#   - every failure case in the corpus, because a splitter has no such thing
#     as a URL that does not parse, so `protocol` can never be ':'
#   - every case that needs normalization: default ports, dot segments,
#     case folding, percent-encoding sets
#
# The corpus-absent path is handled explicitly. Without a checkout the binary
# exits 0 by design, and a control that read that as "it passed, so the control
# is broken" would report a failure that is really a missing download.
test-urlelem-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@if [ ! -f $(URLELEM_ROOT)/url/resources/urltestdata.json ]; then \
	    echo "test-urlelem-negctl: skipped -- no corpus at $(URLELEM_ROOT)."; \
	    echo "  Run \`make wpt-fetch\` to vendor it; this is not a failure."; \
	    exit 0; \
	 fi; \
	 $(CC) -O2 -w $(URLELEM_CF) -DURLELEM_SPLITTER \
	    -o $(BUILD)/urlelem_negctl $(URLELEM_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm; \
	 if $(BUILD)/urlelem_negctl $(URLELEM_ROOT) > $(BUILD)/urlelem_negctl.log 2>&1; then \
	    echo "test-urlelem-negctl: FAILED -- the test PASSED with the"; \
	    echo "  decomposition replaced by a splitter, so its assertions are not"; \
	    echo "  measuring a URL parser."; exit 1; \
	 else \
	    echo "test-urlelem-negctl: ok -- a splitter fails the corpus:"; \
	    head -2 $(BUILD)/urlelem_negctl.log | sed 's/^/    /'; \
	 fi
