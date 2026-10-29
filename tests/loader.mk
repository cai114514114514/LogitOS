# Targets for the browser loader test. Included from the top-level Makefile.
#
# test-browser gains these as PREREQUISITES rather than as recipe lines, so
# the hook lives in here too -- nothing about this test can then be lost by an
# overwrite of the Makefile, which is exactly how it was lost once already.

.PHONY: test-loader test-loader-negctl \
        test-tabs test-tabs-negctl test-tabs-asan test-media-negctl

test-browser: test-loader test-loader-negctl test-tabs-negctl test-media-negctl

# The source set includes the actual loader/painter and all existing runtime
# seams. The transport alone gains a caller for the registered progress tick.
PROGRESS_PAINT_SRC = $(filter-out tests/unit/runtime_scroll_test.c tests/unit/loader_fakebfetch.c,$(RUNTIME_SCROLL_SRC)) tests/unit/progress_paint_test.c tests/unit/progress_paint_fake.c
PROGRESS_PAINT_DIR = $(BUILD)/site-general/progress-paint
# runtime_scroll.mk is parsed later: its deferred source variable works in a
# recipe, NOT in a prerequisite expanded now. Use concrete browser/header
# dependencies here or editing browser.c silently keeps yesterday's binary.
PROGRESS_PAINT_DEPS = tests/unit/progress_paint_test.c tests/unit/progress_paint_fake.c tests/unit/loader_test.c tests/unit/loader_fakebfetch.c $(wildcard $(BROWSER_SOURCES) $(BROWSER_HEADERS) $(BROWSER_INCLUDES)) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(PROGRESS_PAINT_DIR)/current: $(PROGRESS_PAINT_DEPS)
	@mkdir -p $(PROGRESS_PAINT_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -o $@ $(PROGRESS_PAINT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(PROGRESS_PAINT_DIR)/old: $(PROGRESS_PAINT_DEPS)
	@mkdir -p $(PROGRESS_PAINT_DIR)
	$(CC) $(RUNTIME_SCROLL_CF) -DBROWSER_PROGRESS_STALE_PAINT -o $@ $(PROGRESS_PAINT_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-progress-paint test-progress-paint-negctl
test-progress-paint-negctl: $(PROGRESS_PAINT_DIR)/old
	@rc=0; $< > $(PROGRESS_PAINT_DIR)/old.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: progress callback does not paint an unsettled DOM' $(PROGRESS_PAINT_DIR)/old.log && grep -F 'ok: committed replacement is painted after settling' $(PROGRESS_PAINT_DIR)/old.log
test-progress-paint: test-progress-paint-negctl $(PROGRESS_PAINT_DIR)/current
	@$(PROGRESS_PAINT_DIR)/current
ci-host: test-progress-paint

# --- test-loader: the REAL browser.c load path, host-side ------------------
# Every other host test in test-browser links a piece of the pipeline. This one
# links the LOADER -- c/apps/gui/browser/app/main.c itself -- because the bug it
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
              c/apps/gui/browser/app/main.c c/apps/gui/browser/paint/renderer.c \
              c/apps/gui/browser/layout/block.c c/apps/gui/browser/layout/text.c $(BROWSER_FORM_SRC) c/apps/gui/browser/dom/focus.c \
              c/apps/gui/browser/css/engine.c \
              c/apps/gui/browser/css/vars.c c/apps/gui/browser/css/extra.c \
              c/apps/gui/browser/js/dom/dom.c c/apps/gui/browser/js/runtime/page.c \
              $(BROWSER_WEBAPI_SRC) c/apps/gui/browser/js/runtime/module.c \
              c/apps/gui/browser/app/tabs.c \
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

# --- test-media-negctl: the negative control for <link media>/<style media> ---
# Same test, same fixtures, one thing removed: -DMEDIA_ATTR_IGNORE makes
# media_needs_wrap() always answer "no" -- exactly what shipped before this
# change, on BOTH doors (collect_style's <style media> and collect_css_links'
# <link media>).
#
# It must FAIL, and specifically on the two checks that only distinguish
# "was the attribute honoured" from "did source order alone decide it": the
# media=print <link> is BOTH later in source order AND wrongly unconditional
# under the old behaviour, so with the attribute ignored it wins outright,
# displacing the screen sheet. (The THIRD check -- the narrow inline <style>
# -- is order-only-coincidentally still correct under the old behaviour too:
# collect_style() runs before collect_css_links(), so the inline block lands
# EARLIEST in author_css regardless of its media attribute, and both external
# sheets already outrank it on source order alone. That check is not asserted
# to fail here for exactly that reason -- asserting it would be asserting
# something this control does not isolate.)
# Correction 2026-09-22, retained beside that old ordering claim: the loader
# now preserves DOM order. The fixture's narrow inline style is LAST, so when
# MEDIA_ATTR_IGNORE removes both guards it wins over screen AND print. The
# unchanged pre-move source already has this behavior. Require screen to lose
# and the narrow style to win; expecting print to win rejected the correct
# control. This combined mutant does not separately isolate the print link.
test-media-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@$(CC) -O2 -w $(LOADER_INC) $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	    -DCONFIG_VERSION='"host"' -DWEBAPI_HOST -DMEDIA_ATTR_IGNORE \
	    -o $(BUILD)/media_negctl $(LOADER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/media_negctl > $(BUILD)/media_negctl.log 2>&1; then \
	    echo "FAIL: the media negative control PASSED -- part 4 does not measure the fix"; \
	    exit 1; fi
	@grep -q 'FAIL: the UNCONDITIONAL screen sheet' $(BUILD)/media_negctl.log || \
	    { echo "FAIL: the control failed, but not on which sheet won"; exit 1; }
	@grep -Fq 'FAIL: the (max-width: 1px) inline <style> did NOT win' $(BUILD)/media_negctl.log || \
	    { echo "FAIL: the control failed, but the last narrow style did not win"; exit 1; }
	@grep -q 'ok: both list items produced boxes' $(BUILD)/media_negctl.log || \
	    { echo "FAIL: the control broke box generation itself, not just cascade order"; exit 1; }
	@grep -q 'loader_test: FAIL' $(BUILD)/media_negctl.log || \
	    { echo "FAIL: the harness did not report an overall failure"; exit 1; }
	@echo "ok: the media negative control fails, and on the right checks"

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




# --- test-tabs: two real pages open at once, ON THE MACHINE ----------------
# The host test proves the model against a fake network. This boots the OS,
# serves two pages whose only visible colour comes from an EXTERNAL stylesheet,
# opens one in each tab and switches between them -- asserting the pixels from a
# screendump AND that the switch cost the fixture server zero requests. Then it
# closes the window and relaunches, and requires both tabs back.
# See the docstring in tests/qmp/qmp_tabs.py.
test-tabs: $(ISO) $(DISK)
	python3 tests/qmp/qmp_tabs.py $(ISO) $(DISK)

# ci-boot takes the DEVICE twin: the host tabs test (below, test-tabs-asan's
# neighbours) proves the model against a fake network, and only this gate
# proves two real documents coexist in one process on the machine. It was in
# tests/audit-unwired.baseline from the day it landed -- reachable by nobody,
# reading like coverage.
ci-boot: test-tabs
