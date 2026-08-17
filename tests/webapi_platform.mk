# Web-platform targets: the miss probe and the js_platform/js_select tests.
#
# In its own fragment, like tests/nic.mk and tests/audio.mk, for the reason
# those give: several agents edit the top-level Makefile at once, and a
# fragment is the only way to add targets without a commit sweeping up whoever
# else's half-finished work happens to be in that file. `-include` of a missing
# file is silently ignored, so losing the one line in the Makefile costs the
# targets and breaks nothing.
.PHONY: probe-webapi test-platform test-platform-control test-platform-asan
.PHONY: test-platform-page test-platform-page-control test-webapi-url-negctl
.PHONY: test-webapi-slots-negctl

# --- test-webapi-slots-negctl ----------------------------------------------
# The negative control for admitting fetches against the REAL free-slot count.
# -DWEBAPI_NO_SLOT_QUEUE restores what shipped: __fetchSlots answers "plenty",
# so admission depends only on the JS permit -- and the permit is released when
# the response HEADERS arrive while the transport slot is held until the BODY
# finishes. That gap is the bug; on the real kimi.com it rejected 144 requests
# with `TypeError: too many requests in flight` and left the page with 30 of
# its sub-resources. The /slow route in webapi_test.c opens the same gap, so
# these assertions must FAIL here.
test-webapi-slots-negctl: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' \
	    -DWEBAPI_HOST -DWEBAPI_NO_SLOT_QUEUE \
	    -o $(BUILD)/webapi_slotnegctl $(WEBAPI_TEST_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@if $(BUILD)/webapi_slotnegctl > $(BUILD)/webapi_slotnegctl.log 2>&1; then \
	    echo "test-webapi-slots-negctl: FAILED -- the suite PASSED without the fix,"; \
	    echo "  so its queue assertions are not measuring it."; exit 1; \
	 else \
	    echo "test-webapi-slots-negctl: ok -- the suite fails without the fix:"; \
	    grep '^FAIL' $(BUILD)/webapi_slotnegctl.log | head -6; \
	 fi

# --- test-webapi-url-negctl ------------------------------------------------
# The negative control for the non-special URL scheme support in js_webapi.c.
# It cannot live in test-platform-control: that build drops js_platform.o and
# js_select.o and still links js_webapi.o, so a URL assertion would pass in
# both builds and prove nothing. This one is a COMPILE-TIME control instead --
# -DWEBAPI_NO_NONSPECIAL_URL restores the old constructor exactly -- and it
# requires tests/unit/webapi_test.c to FAIL.
test-webapi-url-negctl: $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(BTEST_INC) -Iinclude/abi $(JS_INC) -DCONFIG_VERSION='"host"' \
	    -DWEBAPI_HOST -DWEBAPI_NO_NONSPECIAL_URL \
	    -o $(BUILD)/webapi_urlnegctl $(WEBAPI_TEST_SRC) $(QJS_SRC) $(RUST_LIB_HOST) -lm
	@if $(BUILD)/webapi_urlnegctl > $(BUILD)/webapi_urlnegctl.log 2>&1; then \
	    echo "test-webapi-url-negctl: FAILED -- the suite PASSED without the fix,"; \
	    echo "  so its URL assertions are not measuring it."; exit 1; \
	 else \
	    echo "test-webapi-url-negctl: ok -- the suite fails without the fix:"; \
	    grep '^FAIL' $(BUILD)/webapi_urlnegctl.log | head -6; \
	 fi

# --- probe-webapi: WHICH globals do real pages miss? -----------------------
# Not a test -- an INSTRUMENT. It parses the committed corpus in
# tests/fixtures/webapi/, runs each page's scripts under a Proxy that records
# every global lookup, and every platform-object property, the runtime cannot
# answer, then prints the result ranked by how many pages need each name. The
# Web API surface is extended down that ranking rather than from a remembered
# list of Web APIs -- see the header of tests/unit/webapi_probe.c for the three
# channels and for the one distortion channel 1 has.
#   make probe-webapi                        the table
#   make probe-webapi PROBE="--errors"       ... plus every script's real exception
#   make probe-webapi PROBE="--deep"         ... plus what a page would ask for next
PROBE ?=
WEBAPI_FIXTURES := $(sort $(dir $(wildcard tests/fixtures/webapi/*/index.html)))
PROBE_SRC := tests/unit/webapi_probe.c c/apps/browser/js_page.c c/apps/browser/js_dom.c
PROBE_SRC += c/apps/browser/js_webapi.c c/apps/browser/js_platform.c c/apps/browser/js_select.c c/apps/browser/js_intl.c
# js_module.c is the REAL module loader, linked in rather than reimplemented:
# the probe supplies only the bfetch under it (served from the committed
# fixture), so the normalizer, the loader, the linker and the evaluator being
# measured are the ones the browser ships. Until this line existed the probe
# skipped every <script type=module>, which is most of the modern web.
PROBE_SRC += c/apps/browser/js_module.c
PROBE_SRC += c/apps/browser/css_engine.c c/apps/browser/css_vars.c
PROBE_SRC += c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c tests/unit/rust_host_shim.c
PROBE_CF  := $(BTEST_INC) $(CSS_INC) $(JS_INC) -Iinclude/abi -Ic/kernel/mm -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
# webapi_probe.c DEFINES printf so it can capture js_module.c's diagnostics.
# gcc rewrites printf("%s\n", x) into puts/fputs, and a rewritten call goes
# straight to libc and never reaches that definition -- so the tee would
# silently drop exactly the module exceptions it exists to collect.
PROBE_CF  += -fno-builtin-printf
$(BUILD)/webapi_probe: $(PROBE_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(PROBE_CF) -o $@ $(PROBE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

probe-webapi: $(BUILD)/webapi_probe
	@$(BUILD)/webapi_probe $(PROBE) $(WEBAPI_FIXTURES)

# --- test-platform: js_platform.c + js_select.c, host-side -----------------
# Timing, the document lifecycle, task/message queues, DOMException, Storage
# named properties, crypto, structuredClone, Blob/FormData, the observers, and
# the selector queries. Runs against a REAL parsed document through
# js_page_open, which is the same call the browser makes.
PLATFORM_TEST_SRC := tests/unit/webapi_platform_test.c c/apps/browser/js_page.c
PLATFORM_TEST_SRC += c/apps/browser/js_dom.c c/apps/browser/css_engine.c c/apps/browser/css_vars.c
# js_webapi.c comes along because half of what this file fills in is a GAP in
# what that file publishes -- localStorage's named properties, URL.createObjectURL
# -- and a test that stubbed those would be testing the stub.
PLATFORM_TEST_SRC += c/apps/browser/js_webapi.c c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c
PLATFORM_TEST_SRC += tests/unit/rust_host_shim.c
PLATFORM_MOD := c/apps/browser/js_platform.c c/apps/browser/js_select.c c/apps/browser/js_intl.c
PLATFORM_CF  := $(BTEST_INC) $(CSS_INC) $(JS_INC) -Iinclude/abi -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
test-platform: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(PLATFORM_CF) -o $(BUILD)/platform_test $(PLATFORM_TEST_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/platform_test

# The negative control. The SAME test file, linked WITHOUT js_platform.o and
# js_select.o -- which links cleanly because js_page.c declares both entry
# points weak -- and every check inverted: each one must fail. If this ever
# passes the positive checks, test-platform is measuring something other than
# this change.
test-platform-control: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(PLATFORM_CF) -o $(BUILD)/platform_control $(PLATFORM_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/platform_control --control

# ASan/UBSan with the leak checker: the prelude holds JSValues (the rejection
# hook, the observer registry) across page close, and the failure mode for that
# is a leak or a use-after-free at JS_FreeRuntime, not a wrong answer.
test-platform-asan: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w $(PLATFORM_CF) -o $(BUILD)/platform_asan $(PLATFORM_TEST_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/platform_asan

# --- test-platform-page: the on-device proof, in PIXELS --------------------
# The host test above does not go through the .aex loader, the ring-3 heap, the
# browser's event loop, layout or the framebuffer. This does. The fixture page
# is built so the text on screen can only appear if a CHAIN of the new APIs all
# worked -- mark/measure, queueMicrotask, a MessagePort macrotask, a named
# Storage read and querySelectorAll -- so the before/after screendump measures
# the chain rather than a typeof.
test-platform-page: $(ISO) $(DISK)
	python3 tests/qmp/qmp_platform_page.py $(ISO) $(DISK)

# The device negative control: the same harness against a browser.aex linked
# without js_platform.o and js_select.o. Both entry points are weak in
# js_page.c, so that build links cleanly and simply comes up with none of it.
#
# THE CONTROL MUST DIFFER FROM THE REAL BROWSER IN EXACTLY ONE WAY, and these
# two lines are a hand-copy of Makefile:702-703 and 730, so they drift. Both had:
#
#   * $(GFX_OBJ) missing. Harmless until the G3 migration pointed svg.c's
#     paint_shape at the engine, at which point the control stopped LINKING --
#     twenty undefined gfx_* symbols. Loud, and the cheap one.
#
#   * --stack-pages 2048 missing, which is the dangerous one. It is not a link
#     error and never will be; it silently gives the control a fraction of the
#     real browser's stack. This harness asserts the control comes up with NONE
#     of the platform APIs, and a control that crashed on a deep render stack
#     satisfies that assertion perfectly. A negative control passing for the
#     wrong reason is worse than no control, because it is counted as evidence.
#
# Anything added to the browser's link or packaging has to be added here too.
NOPLAT_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_platform.o $(BUILD)/jsobj/c/apps/browser/js_select.o $(BUILD)/jsobj/c/apps/browser/js_intl.o,$(BROWSER_JS_OBJ))
$(BUILD)/browser-noplat.elf: $(ENGINE_OBJ) $(NOPLAT_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NOPLAT_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group
$(BUILD)/browser-noplat.aex: $(BUILD)/browser-noplat.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-noplat.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

test-platform-page-control: $(ISO) $(BUILD)/browser-noplat.aex
	@$(MAKE) DISK=$(BUILD)/disk-noplat.img BROWSER_AEX=$(BUILD)/browser-noplat.aex $(BUILD)/disk-noplat.img
	python3 tests/qmp/qmp_platform_page.py $(ISO) $(BUILD)/disk-noplat.img --expect-none

# --- test-bing: the acceptance case, from the page's own bytes -------------
# bing.com is what the bug report was about. This serves the CAPTURED bing
# document and its four scripts (tests/fixtures/webapi/bing, the same bytes the
# probe measures) at the paths the document names, loads it on the machine, and
# reports every JS exception the page produced plus a screendump. It is a
# REPORT, not a pass/fail gate on the page rendering: bing's own <inline 2>
# references `_w` 37 KiB before the script that defines it and throws in a real
# browser too, so a green light there would be a lie. What it does assert is
# that the number of scripts that die is the number the host probe predicts.
test-bing: $(ISO) $(DISK)
	python3 tests/qmp/qmp_bing.py $(ISO) $(DISK)

