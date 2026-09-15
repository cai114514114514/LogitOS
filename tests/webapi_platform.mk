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
.PHONY: test-platform-timing-negctl test-platform-livecollection-negctl
.PHONY: test-platform-observer-negctl
.PHONY: webapi-link-check

# ===========================================================================
# THE TWO SOURCE LISTS IN THIS FILE WERE HAND COPIES OF THE BROWSER'S, AND
# BOTH DRIFTED. Measured 2026-08-28.
# ===========================================================================
# Fourteen js_*.c translation units landed in c/apps/browser after PROBE_SRC
# and PLATFORM_TEST_SRC were written; neither list followed. Both links died
# with fifteen undefined symbols (js_anim_install, js_canvas_install,
# js_cssom_install/_close, js_domparser_install, js_events_install,
# js_forms_install, js_media_install/_close, js_reflect_install,
# js_semantics_install, js_tokenlist_install, js_url_install, plus
# ci_transform_parse from css_engine.c and layout_count/layout_items from
# js_dom.c).
#
# EVERY ONE OF THOSE FIFTEEN IS AN __attribute__((__weak__)) DECLARATION, and
# that is the finding, not the link error. On ELF the link SUCCEEDS: the
# missing installers resolve to NULL, js_page.c's `if (js_events_install)`
# guards step over them, and the harness runs a browser with seven of its
# twenty js TUs missing while reporting itself green.
#
# For probe-webapi that is not a degraded test, it is a FALSE MEASUREMENT.
# The probe exists to answer "which globals do real pages miss?" -- so a name
# js_events.c or js_cssom.c or js_url.c publishes was reported MISSING, by the
# one instrument the Web API surface is extended from. Measured after the fix,
# against tests/fixtures/frameworks: SVGElement undefined -> ctor,
# document.currentScript null -> the real script, template.content undefined
# -> fragment, document.baseURI undefined -> the page URL. svelte, vue and
# webpack go from `#app=0` to a mounted, interactive app -- exactly the three
# rows CLAUDE.md already records as FIXED, which this probe had never seen.
#
# So both lists are now SUBTRACTIONS from the Makefile's own
# $(BROWSER_JS_SRC) (Makefile:828 -- `browser.c` plus
# `$(wildcard c/apps/browser/js_*.c)`, the variable $(BUILD)/browser.elf is
# built from). A file added to the browser is on these link lines the same
# minute; a file that must NOT be on one is named below with its reason.
ifeq ($(strip $(BROWSER_JS_SRC)),)
WEBAPI_LINK_ERR := tests/webapi_platform.mk: BROWSER_JS_SRC is empty -- this fragment must be -included from the Makefile, AFTER it. Refusing to build a probe or a platform test from a partial source list.
endif

# Out of BOTH lists, because neither test file can supply what these need:
#   browser.c        the ring-3 app shell -- window management and a ring-3
#                    `main`. It is what $(BROWSER_JS_SRC) adds on top of the
#                    js_*.c wildcard.
#   js_media.c       includes c/apps/logit.h; gui_blit/snd_write/monotonic_ns
#                    are `int 0x80` syscalls.
#   js_media_src.c   the engine half -- media.h/h264.h/h265.h/aac.h/mp3.h, so
#                    c/lib/media + c/lib/video + c/lib/audio come with it.
#                    tests/wpt.mk pays that price deliberately and says so.
#   js_forms.c       calls forms.c's fc_*, and forms.c calls text_measure, a
#                    ring-3 syscall each host harness defines for itself.
#                    Neither tests/unit/webapi_probe.c nor
#                    tests/unit/webapi_platform_test.c does (forms_test.c,
#                    layout_test.c and wpt_test.c each do), so forms.c cannot
#                    be linked and js_forms.c cannot be linked without it.
WEBAPI_JS_OUT := c/apps/browser/browser.c \
                 c/apps/browser/js_media.c c/apps/browser/js_media_src.c \
                 c/apps/browser/js_forms.c

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

# --- webapi-link-check: the drift check, as a target rather than a habit ----
# The subtraction makes the dangerous direction impossible -- a new js_*.c is
# linked into both binaries without anyone remembering -- so only the other
# direction is left: a name in an OUT list that the browser no longer links
# excludes nothing and has quietly stopped being a decision. A RENAMED file is
# the bad case, because its old name sits in the list reading like a live
# exclusion while its new one is silently linked.
#
# It does NOT cover $(BROWSER_PIPE), and that is a stated limit. Excluding
# layout.c / forms.c / browser_rt.c from a host harness is forced by
# text_measure and bfetch, so those names would be constraints of the test
# files rather than decisions about the browser. The wildcard is the list that
# grows -- fourteen files in one stretch -- and it is the one covered.
webapi-link-check:
	@if [ -n "$(WEBAPI_LINK_ERR)" ]; then echo "$(WEBAPI_LINK_ERR)"; exit 1; fi
	@stale=""; for f in $(sort $(WEBAPI_JS_OUT) $(PLATFORM_JS_OUT)); do \
	    case " $(BROWSER_JS_SRC) " in *" $$f "*) ;; *) stale="$$stale $$f";; esac; \
	  done; \
	  if [ -n "$$stale" ]; then \
	    echo "webapi-link-check: FAIL -- an OUT list names files the browser no"; \
	    echo "  longer links, so the exclusion is a lie rather than a decision:"; \
	    for f in $$stale; do echo "    $$f"; done; \
	    echo "  If one was RENAMED, its new name is being linked silently."; exit 1; \
	  fi

# THE PROBE LINKS THE WHOLE js_*.c SET, MINUS $(WEBAPI_JS_OUT) AND NOTHING
# ELSE. It is the instrument the Web API surface is ranked from, so any name
# it cannot answer must be a name the BROWSER cannot answer -- a TU missing
# here does not weaken the measurement, it inverts it.
#
# js_module.c is IN, and only here: it is the REAL module loader rather than a
# reimplementation, and the probe supplies the bfetch under it itself
# (webapi_probe.c:392 and :452, served from the committed fixture) so the
# normalizer, the loader, the linker and the evaluator being measured are the
# ones the browser ships. Until that line existed the probe skipped every
# <script type=module>, which is most of the modern web. The canvas and
# platform harnesses have no bfetch, which is why they name it OUT.
PROBE_SRC := tests/unit/webapi_probe.c \
             $(filter-out $(WEBAPI_JS_OUT),$(BROWSER_JS_SRC))
PROBE_SRC += c/apps/browser/css_engine.c c/apps/browser/css_vars.c
# css_interp.c is the `ci_transform_parse` in the drift above: css_engine.c
# grew a call to it (css_supports_decl, for `@supports (transform: ...)`) and
# this list did not follow.
PROBE_SRC += c/apps/browser/css_interp.c
# js_canvas.c's cost, paid on purpose. It reaches gfx_fill/gfx_paint_* and
# img_css_color(), so the engine and svg.c come with it -- and NOT img.c,
# which would then want gif_register/jpeg_register/exif_apply for a probe that
# decodes no image. [CORRECTED 2026-08-30, and the old claim kept beside the
# correction: js_canvas.c's cv_drawImage now references _img_decode directly,
# so the probe DOES need the decoders -- the list below stopped being
# complete the day drawImage landed, found by the frameworks agent. The
# hand-copy is replaced by $(IMG_HOST_SRC), the Makefile's one authoritative
# host image list ("there is no narrower place to add it that would not
# silently miss one" -- its own comment), minus rust_host_shim.c which the
# ws.c rider below already adds; listing a .c twice in one link is duplicate
# symbols.] Leaving js_canvas.c out would have been cheaper and would
# have kept `getContext` at the top of this probe's own ranking forever: it
# was 33 occurrences and the #1 finding, the context was written, and the
# instrument that ordered the work could not see its own result land.
# $(IMG_HOST_SRC) itself CANNOT be used here: it is defined in the root
# Makefile at :3796, AFTER the -include of this fragment (~:3400), and
# PROBE_SRC is := so the reference expanded EMPTY -- measured: the link line
# carried no image .c at all and failed on _img_decode regardless. The
# wildcard is the door instead: c/lib/image/ held exactly the five decoders
# (checked 2026-08-30), and a sixth that lands there joins every probe link
# without an edit -- the property the IMG_HOST_SRC comment asks for.
PROBE_SRC += $(wildcard c/lib/image/*.c) $(GFX_SRC)
# c/net/http/ws.c: js_websocket.c (in BROWSER_JS_SRC, landed 2026-08-28 21:18,
# after this list was last touched 14:39) calls ws_accept_matches/
# ws_frame_write/ws_make_key/ws_parser_*/ws_utf8_valid, all defined only in
# ws.c -- the exact hand-copied-source-list drift CLAUDE.md rule 4 names.
# Measured: without this line, build-*/webapi_probe fails 8 undefined symbols.
# ws.c itself calls ocsp_sha1 (c/crypto/hash/sha1.c) and includes base64.h
# (c/net/ssh) -- same two riders tests/wpt.mk already carries for this file.
PROBE_SRC += c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c c/net/http/ws.c c/net/ssh/base64.c c/crypto/hash/sha1.c tests/unit/rust_host_shim.c
PROBE_CF  := $(BTEST_INC) $(CSS_INC) $(JS_INC) -Iinclude/abi $(KMM_INC) -Ic/net/ssh -Ic/crypto -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
# webapi_probe.c DEFINES printf so it can capture js_module.c's diagnostics.
# gcc rewrites printf("%s\n", x) into puts/fputs, and a rewritten call goes
# straight to libc and never reaches that definition -- so the tee would
# silently drop exactly the module exceptions it exists to collect.
PROBE_CF  += -fno-builtin-printf
# webapi-link-check is NOT a prerequisite of this file target on purpose: a
# .PHONY prerequisite makes a file rule unconditionally out of date, and this
# is a two-minute link that probe-frameworks and test-frameworks both hang
# off. It hangs off the phony entry points instead.
# $(HTML_PARSER_SRC) AND $(QJS_SRC) ARE PREREQUISITES BECAUSE THE RECIPE
# COMPILES THEM. They were on the command line below and NOT in this list, so
# `make $(BUILD)/webapi_probe` answered "up to date" after an edit to dom.c,
# html_tree.c, html_tokenizer.c, dom_serialize.c or quickjs.c, and the next
# measurement was taken with the OLD engine and the NEW source on disk. Found
# 2026-08-30 by editing dom.c and being told there was nothing to do. It is the
# hand-copied-source-list shape CLAUDE.md rule 4 names, in its quietest form:
# not a link error, not a build failure -- a green instrument reporting on code
# that is no longer in the tree. It matters today in particular because the
# wrong-type line is editing quickjs.c and ranking its work with this binary.
$(BUILD)/webapi_probe: $(PROBE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
                       $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(PROBE_CF) -o $@ $(PROBE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

probe-webapi: webapi-link-check $(BUILD)/webapi_probe
	@$(BUILD)/webapi_probe $(PROBE) $(WEBAPI_FIXTURES)

# --- test-platform: js_platform.c + js_select.c, host-side -----------------
# Timing, the document lifecycle, task/message queues, DOMException, Storage
# named properties, crypto, structuredClone, Blob/FormData, the observers, and
# the selector queries. Runs against a REAL parsed document through
# js_page_open, which is the same call the browser makes.

# THIS LIST HAS SIX MORE EXCLUSIONS THAN THE PROBE. One (js_module.c) is
# about what will link. THE OTHER FIVE ARE NOT -- all five compile and link
# here fine, and were measured doing so. They are named because
# tests/unit/webapi_platform_test.c ASSERTS THEIR ABSENCE, and that file
# belongs to another line. Each is a work order for whoever owns it:
#
#   js_canvas.c     :186-194 -- "canvas.getContext is a REAL context now
#                   (js_canvas.c), WHICH THIS BUILD DOES NOT LINK", and then
#                   asserts `typeof ....getContext === 'undefined'`. The
#                   composition is the assertion; linking it fails the gate.
#                   The real surface is make test-canvas, 46 checks.
#   js_semantics.c  :628 asserts `!('closedBy' in HTMLDialogElement.prototype)`
#                   -- "answers a feature test (falsely, correctly)".
#                   js_semantics.c:679 implements closedBy for real, so with
#                   it linked the answer is true and the check fails.
#   js_events.c     supplies PromiseRejectionEvent, and js_reflect.c /
#   js_reflect.c    js_urlbind.c supply the four `script.src is ABSOLUTE`
#   js_urlbind.c    checks (:809-815) and `anchor.href is absolute too`
#                   (:816). test-platform-control links this file WITHOUT
#                   js_platform.o/js_select.o and requires EVERY check to
#                   fail; those six pass without either, so linking the three
#                   takes the control from 1 failure to 7. The control's
#                   premise -- every check in that file is about js_platform.c
#                   or js_select.c -- is what has drifted, and repairing it is
#                   an edit to the test source, not to this list.
#
# js_module.c is out for the reason the canvas fragment gives: only
# webapi_probe.c supplies bfetch_resolve/bfetch_sync. js_worker.c (landed
# 2026-08-28, after this list was last touched) calls the same two functions
# from _js__workerCreate/_js__wImportScripts/js_worker_run_due, so it is out
# for the identical reason -- webapi_platform_test.c has no bfetch either.
# js_websocket.c stays IN: it needs only c/net/http/ws.c, which
# PLATFORM_TEST_SRC now supplies below, same as PROBE_SRC does.
PLATFORM_JS_OUT := $(WEBAPI_JS_OUT) c/apps/browser/js_module.c \
                   c/apps/browser/js_worker.c \
                   c/apps/browser/js_canvas.c c/apps/browser/js_semantics.c \
                   c/apps/browser/js_events.c c/apps/browser/js_reflect.c \
                   c/apps/browser/js_urlbind.c
# PLATFORM_MOD is the three the control drops, so they are subtracted here and
# re-added on the positive link line -- one file cannot be on both.
PLATFORM_MOD := c/apps/browser/js_platform.c c/apps/browser/js_select.c c/apps/browser/js_intl.c
PLATFORM_TEST_SRC := tests/unit/webapi_platform_test.c \
                     $(filter-out $(PLATFORM_JS_OUT) $(PLATFORM_MOD),$(BROWSER_JS_SRC))
PLATFORM_TEST_SRC += c/apps/browser/css_engine.c c/apps/browser/css_vars.c
# css_interp.c: css_engine.c's ci_transform_parse, the one non-js drift.
PLATFORM_TEST_SRC += c/apps/browser/css_interp.c
# js_webapi.c comes along (through the subtraction now) because half of what
# this file fills in is a GAP in what that file publishes -- localStorage's
# named properties, URL.createObjectURL -- and a test that stubbed those would
# be testing the stub.
# ws.c riders (base64.h, ocsp_sha1) -- same reasoning as PROBE_SRC above.
PLATFORM_TEST_SRC += c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c c/net/http/ws.c c/net/ssh/base64.c c/crypto/hash/sha1.c
PLATFORM_TEST_SRC += tests/unit/rust_host_shim.c
PLATFORM_CF  := $(BTEST_INC) $(CSS_INC) $(JS_INC) -Iinclude/abi -Ic/net/ssh -Ic/crypto -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
# test-platform-livecollection-negctl joins its two siblings on this line:
# same rule (NOT_CI drops test-*-negctl from CI; a prerequisite line is the
# only thing that makes the positive actually run it), same suite, and it
# had drifted to stranded-but-never-recorded -- worse than stranded, because
# the audit had no line to compare against.
test-platform: webapi-link-check test-platform-timing-negctl test-platform-observer-negctl test-platform-livecollection-negctl $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
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

# --- test-platform-timing-negctl -------------------------------------------
# The negative control for User Timing's SECOND lookup. It cannot be
# test-platform-control: that build drops js_platform.o entirely, so
# performance.measure does not exist and EVERY performance check fails for the
# same uninformative reason. -DPLATFORM_NO_TIMING_MARKS restores exactly what
# shipped -- markTime searching only user marks -- and leaves the rest of the
# platform intact, so the checks that redden are the ones about this rule and
# nothing else.
#
# EXACTLY 2, and the count is asserted rather than eyeballed. The third check
# of the three added with the fix -- that a non-numeric timing member is still
# a SyntaxError -- must keep PASSING here: it is the half that stops "make
# markTime never throw" from satisfying the other two.
test-platform-timing-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(PLATFORM_CF) -DPLATFORM_NO_TIMING_MARKS \
	    -o $(BUILD)/platform_tmneg $(PLATFORM_TEST_SRC) $(PLATFORM_MOD) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/platform_tmneg > $(BUILD)/platform_tmneg.log 2>&1; \
	 n=`grep -c '^FAIL: ' $(BUILD)/platform_tmneg.log`; \
	 if [ "$$n" != "2" ]; then \
	   echo "test-platform-timing-negctl: FAILED -- expected exactly 2 FAILs, got $$n:"; \
	   grep '^FAIL: ' $(BUILD)/platform_tmneg.log; exit 1; \
	 else \
	   echo "test-platform-timing-negctl: ok -- the suite fails without the fix:"; \
	   grep '^FAIL: ' $(BUILD)/platform_tmneg.log; \
	 fi

# --- test-platform-livecollection-negctl ------------------------------------
# js_dom.c's comment above live_list_cid promises this target exists ("See
# ll_own_prop below and CLAUDE.md rule 5: test-live-collection-negctl is the
# control that stays able to fail") -- it did not, until now. Per rule 5, a
# control that is claimed in a comment but not wired is worse than admitting
# there is none: the comment reads as evidence the fix is guarded when nothing
# runs it.
#
# -DPLATFORM_NO_LIVE_COLLECTIONS (js_dom.c's child_array) restores the exact
# Array-snapshot behaviour this file shipped before 2026-08-28. The six
# "children / childNodes are LIVE" checks in webapi_platform_test.c must
# answer differently there: five depend on the collection observing a mutation
# made AFTER it was captured, which a snapshot cannot do by construction; the
# sixth ("keep their real interface identity") does NOT -- child_array's
# negative-control branch still calls iface_tag_list(), the same call the live
# branch makes, so `instanceof HTMLCollection/NodeList` holds either way. A
# target that asserted all six would be asserting something the code was never
# meant to break, and the day it silently stopped breaking five-of-six instead
# of six-of-six nobody would notice which one.
#
# Matched by DESCRIPTION rather than a blind total-FAIL count on purpose: this
# file's other negctl (test-platform-timing-negctl, immediately above) counts
# every FAIL line, and while this target was being written that count read 4
# instead of its asserted 2 -- two unrelated checks (indexedDB, crypto.subtle)
# were failing because PLATFORM_MOD/PLATFORM_TEST_SRC do not yet carry every
# js_*.c TU another workflow is mid-landing (see the file header on why this
# fragment's source lists drift). A blind count here would go red for the same
# reason and blame the wrong fix. Matching the five checks' own text is immune
# to that collateral noise: it goes red only when ITS OWN five checks stop
# failing, which is the one thing this target exists to watch.
test-platform-livecollection-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(PLATFORM_CF) -DPLATFORM_NO_LIVE_COLLECTIONS \
	    -o $(BUILD)/platform_llneg $(PLATFORM_TEST_SRC) $(PLATFORM_MOD) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/platform_llneg > $(BUILD)/platform_llneg.log 2>&1; \
	 n=`grep -cE '^FAIL: (children\.length reflects|children\[i\] sees|children re-indexes|childNodes\.length is live|children supports Array-generic)' $(BUILD)/platform_llneg.log`; \
	 idok=`grep -c '^ok  : children/childNodes keep their real interface identity' $(BUILD)/platform_llneg.log`; \
	 if [ "$$n" != "5" ]; then \
	   echo "test-platform-livecollection-negctl: FAILED -- expected exactly 5 of the live-collection checks to FAIL, got $$n:"; \
	   grep -E '^(FAIL|ok  ): (children|childNodes)' $(BUILD)/platform_llneg.log; exit 1; \
	 elif [ "$$idok" != "1" ]; then \
	   echo "test-platform-livecollection-negctl: FAILED -- the interface-identity check must still PASS (it does not depend on liveness) but did not"; \
	   grep -E '^(FAIL|ok  ): children/childNodes keep' $(BUILD)/platform_llneg.log; exit 1; \
	 else \
	   echo "test-platform-livecollection-negctl: ok -- the five liveness checks fail without the fix, identity still passes:"; \
	   grep -E '^(FAIL|ok  ): (children|childNodes)' $(BUILD)/platform_llneg.log; \
	 fi

# --- test-platform-observer-negctl -------------------------------------------
# The negative control for PerformanceObserver (js_platform.c). MEASURED IN
# THE GUEST: bing.com constructs one un-guarded and threw a ReferenceError that
# took the rest of its IIFE with it -- see js_platform.c's comment above
# SUPPORTED_TYPES for the call site and tests/scoreboard/full-corpus/bing.json
# for the recorded exception.
#
# -DPLATFORM_NO_PERFORMANCE_OBSERVER restores exactly what shipped before this
# change: `if (!G.PerformanceObserver) def(...)` is skipped, so the global
# stays absent and every check about it must fail.
#
# MATCHED BY DESCRIPTION, not a blind FAIL count, for the reason
# test-platform-livecollection-negctl above gives verbatim: this file's source
# lists (PLATFORM_TEST_SRC) are mid-drift from other workflows landing in
# c/apps/browser at the same time, and at the moment this target was written
# that collateral noise was three UNRELATED failures (indexedDB, crypto.subtle,
# Element.attachShadow) that have nothing to do with this change and must not
# make this control flap. A blind count would go red the next time that drift
# count changes and blame the wrong fix.
#
# EXACTLY 11, not 14: three of the fourteen PerformanceObserver checks assert
# an ABSENCE of a side effect ("has not fired synchronously", "an unsupported
# type never fires", "disconnect() removes the observer before its callback
# ever ran") and are true VACUOUSLY when the whole feature is missing -- there
# is no callback to have fired either way. Each of those three is paired with
# a positive check right next to it (the batched-delivery count, the
# instanceof check, takeRecords) that DOES fail here, so the feature is still
# proven; counting the vacuous three as evidence would be the exact "control
# passes for the wrong reason" shape CLAUDE.md rule 5 names.
test-platform-observer-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(PLATFORM_CF) -DPLATFORM_NO_PERFORMANCE_OBSERVER \
	    -o $(BUILD)/platform_poneg $(PLATFORM_TEST_SRC) $(PLATFORM_MOD) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/platform_poneg > $(BUILD)/platform_poneg.log 2>&1; \
	 n=`grep -cE '^FAIL: (PerformanceObserver exists|supportedEntryTypes |PerformanceObserver requires|three entries made|the batched callback|the delivered list|delivered entries are instanceof|buffered:true replays|takeRecords )' $(BUILD)/platform_poneg.log`; \
	 if [ "$$n" != "11" ]; then \
	   echo "test-platform-observer-negctl: FAILED -- expected exactly 11 PerformanceObserver checks to FAIL, got $$n:"; \
	   grep -E '^(FAIL|ok  ): (PerformanceObserver|supportedEntryTypes|three entries|the batched|the delivered|delivered entries|buffered:true|takeRecords)' $(BUILD)/platform_poneg.log; exit 1; \
	 else \
	   echo "test-platform-observer-negctl: ok -- the observer's real-delivery checks fail without the fix:"; \
	   grep -E '^FAIL: (PerformanceObserver|supportedEntryTypes|three entries|the batched|the delivered|delivered entries|buffered:true|takeRecords)' $(BUILD)/platform_poneg.log; \
	 fi
