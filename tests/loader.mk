# Targets for the browser loader test. Included from the top-level Makefile.
#
# test-browser gains these as PREREQUISITES rather than as recipe lines, so
# the hook lives in here too -- nothing about this test can then be lost by an
# overwrite of the Makefile, which is exactly how it was lost once already.

.PHONY: test-loader test-loader-negctl test-loader-asan test-script-nav \
        test-tabs test-tabs-negctl test-tabs-asan

test-browser: test-loader test-loader-negctl test-tabs-negctl

# --- test-loader: the REAL browser.c load path, host-side ------------------
# Every other host test in test-browser links a piece of the pipeline. This one
# links the LOADER -- c/apps/browser/browser.c itself -- because the bug it
# exists for lives between the pieces: https://www.baidu.com/ serves our
# User-Agent a 227-byte stub whose only content is location.replace(), and the
# loader used to ignore it and render the stub for ever (blank page, zero
# sub-resources -- one fact, not two).
#
# browser.c needs exactly two things a host process cannot give it, and both are
# replaced here and nothing else is: the window (tests/unit/loaderhost/logit.h,
# which re-uses painthost's five drawing recorders) and the network
# (tests/unit/loader_fakebfetch.c, an in-memory site implementing bfetch.h).
# The tokenizer, tree builder, DOM, LibCSS cascade, layout, painter, QuickJS
# runtime and DOM bindings in the link are all the real ones.
#
# LOADER_INC must put loaderhost first so its logit.h wins, and include
# include/abi for the real event ABI + tests/unit for the fixture-site header.
LOADER_INC := -Itests/unit/loaderhost -Itests/unit -Iinclude/abi
LOADER_SRC := tests/unit/loader_test.c tests/unit/loader_fakebfetch.c \
              c/apps/browser/browser.c c/apps/browser/browser_paint.c \
              c/apps/browser/layout.c c/apps/browser/css_engine.c \
              c/apps/browser/css_vars.c c/apps/browser/css_extra.c \
              c/apps/browser/js_dom.c c/apps/browser/js_page.c \
              c/apps/browser/js_webapi.c c/apps/browser/js_module.c \
              c/apps/browser/tabs.c \
              c/net/http/url.c c/net/http/http1.c c/net/http/cookies.c \
              tests/unit/rust_host_shim.c $(GFX_SRC) $(HTML_PARSER_SRC)
test-loader: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O2 -w $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	    -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -o $(BUILD)/loader_test \
	    $(LOADER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/loader_test

# The NEGATIVE CONTROL. Same test, same fixtures, one thing removed: the loader
# is built with a redirect budget of ZERO, which is exactly what it did before
# -- take the record js_webapi.c left and throw it away. It must FAIL, and it
# must fail on the five checks the fix is about (the FACT ... FIXED ones and the
# navigation itself) while every other check still passes. If it ever passes,
# test-loader is measuring something other than this change.
test-loader-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O2 -w $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	    -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -DNAV_MAX_HOPS=0 \
	    -o $(BUILD)/loader_negctl $(LOADER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/loader_negctl > $(BUILD)/loader_negctl.log 2>&1; then \
	    echo "FAIL: the negative control PASSED -- test-loader does not measure the fix"; \
	    exit 1; fi
	@grep -q 'FAIL: location.replace() NAVIGATED' $(BUILD)/loader_negctl.log || \
	    { echo "FAIL: the control failed, but not on the navigation"; exit 1; }
	@grep -q 'FAIL: FACT 1 FIXED' $(BUILD)/loader_negctl.log || \
	    { echo "FAIL: the control failed, but the sub-resources still arrived"; exit 1; }
	@grep -q 'FAIL: FACT 2 FIXED' $(BUILD)/loader_negctl.log || \
	    { echo "FAIL: the control failed, but the text still reached the painter"; exit 1; }
	@grep -q 'ok: CONTROL: the real document' $(BUILD)/loader_negctl.log || \
	    { echo "FAIL: the control broke the pipeline itself, not just the loader"; exit 1; }
	@echo "ok: the negative control fails, and on the right checks"

# --- test-tabs-negctl: the negative control for TABS -----------------------
# Same test, same fixtures, one thing removed: -DTABS_NO_RETAIN builds a browser
# whose tabs keep NOTHING -- no document bytes, no stylesheet, no sub-resources,
# and no session written. That is exactly what a naive tab implementation is,
# and it is what part 3 of loader_test.c has to be able to tell apart from the
# real one.
#
# It must FAIL, and it must fail on the two checks the design is about:
#   - switching back to a tab costs ZERO network requests (without retention it
#     re-fetches the whole page, so N tabs multiply the handshakes that the
#     connection pool exists to remove);
#   - a restart restores the tabs (without a session file there is nothing to
#     restore).
# And it must still PASS "tab 1 rendered" -- if the control broke rendering, the
# failures above would prove nothing about retention.
test-tabs-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O2 -w $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	    -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -DTABS_NO_RETAIN \
	    -o $(BUILD)/tabs_negctl $(LOADER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/tabs_negctl > $(BUILD)/tabs_negctl.log 2>&1; then \
	    echo "FAIL: the tab negative control PASSED -- part 3 does not measure retention"; \
	    exit 1; fi
	@grep -q 'FAIL: and cost ZERO network requests' $(BUILD)/tabs_negctl.log || \
	    { echo "FAIL: the control failed, but not on the network cost of a tab switch"; exit 1; }
	@grep -q 'FAIL: SESSION RESTORE' $(BUILD)/tabs_negctl.log || \
	    { echo "FAIL: the control failed, but the session still restored"; exit 1; }
	@grep -q 'ok: tab 1 rendered' $(BUILD)/tabs_negctl.log || \
	    { echo "FAIL: the control broke rendering, not just retention"; exit 1; }
	@grep -q 'ok: location.replace() NAVIGATED' $(BUILD)/tabs_negctl.log || \
	    { echo "FAIL: the control broke the loader as well"; exit 1; }
	@echo "ok: the tab negative control fails, and on the right checks"

# Tabs under ASan+UBSan. A switch frees a document, a runtime and a display list
# and then rebuilds all three from bytes the tab still owns -- ownership across
# that boundary is exactly where a use-after-free would be silent for hours.
test-tabs-asan: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	    -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -o $(BUILD)/tabs_asan \
	    $(LOADER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/tabs_asan

# Same under ASan+UBSan: the redirect chain frees a whole document and opens a
# new runtime per hop, which is precisely the shape where an order-of-teardown
# mistake is silent until much later.
test-loader-asan: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	    -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -o $(BUILD)/loader_asan \
	    $(LOADER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/loader_asan

# --- test-script-nav: the same claim ON THE MACHINE, in the pixels ---------
# test-loader proves the loader follows location.replace() against a fake
# network. This boots the OS, serves baidu's own stub (retargeted at the host
# server) and requires the destination's TEXT to appear in a screendump, its
# sub-resources in the server's request log, and a self-navigating page to be
# stopped rather than spun. See the docstring in tests/qmp/qmp_script_nav.py.
test-script-nav: $(ISO) $(DISK)
	python3 tests/qmp/qmp_script_nav.py $(ISO) $(DISK)

# --- test-tabs: two real pages open at once, ON THE MACHINE ----------------
# The host test proves the model against a fake network. This boots the OS,
# serves two pages whose only visible colour comes from an EXTERNAL stylesheet,
# opens one in each tab and switches between them -- asserting the pixels from a
# screendump AND that the switch cost the fixture server zero requests. Then it
# closes the window and relaunches, and requires both tabs back.
# See the docstring in tests/qmp/qmp_tabs.py.
test-tabs: $(ISO) $(DISK)
	python3 tests/qmp/qmp_tabs.py $(ISO) $(DISK)

