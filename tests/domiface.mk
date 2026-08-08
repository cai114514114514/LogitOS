# tests/domiface.mk -- the DOM interface hierarchy (c/apps/browser/js_dom_iface.inc).
#
#   make test-dom-iface          the hierarchy, host-side, seconds
#   make test-dom-iface-negctl   the negative control: the SAME binary built
#                                without the hierarchy, which must FAIL
#
# Own fragment rather than lines in the Makefile, for the reason every other
# fragment here gives: several agents edit that file at once and a whole-file
# write loses somebody's work. One `-include` line is the entire footprint.
#
# WHY A SEPARATE TEST FROM js_dom_test. That one links js_dom.c + js_page.c and
# nothing else, which is right for what it measures (bindings, events, timers)
# and useless here: half of what this file asserts is that js_select.c's
# querySelector and js_platform.c's dataset still reach a NON-div element after
# the element prototype stopped being one shared object. That is a property of
# the three files TOGETHER, so all three are linked -- the same reasoning
# tests/wpt.mk gives for linking the shipping files instead of stubs.

.PHONY: test-dom-iface test-dom-iface-negctl

DOMIFACE_SRC := tests/unit/dom_iface_test.c \
                c/apps/browser/js_page.c c/apps/browser/js_dom.c \
                c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                c/apps/browser/js_webapi.c c/apps/browser/js_platform.c \
                c/apps/browser/js_select.c c/apps/browser/js_intl.c \
                c/apps/browser/js_module.c \
                c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c \
                tests/unit/rust_host_shim.c
# -Ic/apps because js_platform.c includes "logit.h" unconditionally; see the
# same note in tests/wpt.mk.
DOMIFACE_CF := $(BTEST_INC) -Ic/apps $(CSS_INC) $(JS_INC) -Iinclude/abi \
               -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

$(BUILD)/dom_iface_test: $(DOMIFACE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(DOMIFACE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-dom-iface: $(BUILD)/dom_iface_test
	@$(BUILD)/dom_iface_test

# --- the negative control ---------------------------------------------------
# -DJSDOM_NO_INTERFACE_HIERARCHY keeps every constructor NAME and every member
# but installs the members flat on one prototype and leaves the constructors'
# prototypes unchained -- which is exactly the JS facade js_platform.c used to
# provide and which this work replaced. Fourteen of the test's assertions are
# about SHAPE (which object is a wrapper's prototype, what is above it,
# whether a patch of a per-tag prototype reaches an element) and every one of
# them must go red in that build. If they do not, the test is passing for some
# reason other than the hierarchy and is not evidence of anything.
test-dom-iface-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(DOMIFACE_CF) -DJSDOM_NO_INTERFACE_HIERARCHY \
	    -o $(BUILD)/dom_iface_negctl $(DOMIFACE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/dom_iface_negctl > $(BUILD)/dom_iface_negctl.log 2>&1; then \
	    echo "test-dom-iface-negctl: FAILED -- the test PASSED without the"; \
	    echo "  hierarchy, so its assertions are not measuring it."; exit 1; \
	 else \
	    echo "test-dom-iface-negctl: ok -- without the hierarchy the test fails:"; \
	    grep '^FAIL' $(BUILD)/dom_iface_negctl.log | head -6; \
	    tail -2 $(BUILD)/dom_iface_negctl.log; \
	 fi
