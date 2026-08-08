# Web-platform targets: the miss probe and the js_platform/js_select tests.
#
# In its own fragment, like tests/nic.mk and tests/audio.mk, for the reason
# those give: several agents edit the top-level Makefile at once, and a
# fragment is the only way to add targets without a commit sweeping up whoever
# else's half-finished work happens to be in that file. `-include` of a missing
# file is silently ignored, so losing the one line in the Makefile costs the
# targets and breaks nothing.
.PHONY: probe-webapi test-platform test-platform-control test-platform-asan
.PHONY: test-platform-page test-platform-page-control

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
PROBE_SRC += c/apps/browser/js_webapi.c c/apps/browser/js_platform.c c/apps/browser/js_select.c
PROBE_SRC += c/apps/browser/css_engine.c c/apps/browser/css_vars.c
PROBE_SRC += c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c tests/unit/rust_host_shim.c
PROBE_CF  := $(BTEST_INC) $(CSS_INC) $(JS_INC) -Iinclude/abi -Ic/kernel/mm -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
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
PLATFORM_MOD := c/apps/browser/js_platform.c c/apps/browser/js_select.c
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

