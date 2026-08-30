# tests/canvas.mk -- CanvasRenderingContext2D over c/lib/gfx.
#
# In its own fragment for the reason tests/webapi_platform.mk gives: several
# agents edit the top-level Makefile at once, and a fragment is the only way to
# add targets without a commit sweeping up somebody else's half-finished work.
#
# The link is the browser's own js_*.c set (minus the five named below) plus
# the engine (GFX_SRC) and svg.c -- the last because img_css_color() lives
# there. That is not a convenience: a canvas fillStyle and an SVG fill
# attribute ask the SAME question, and css.h is emphatic that two evaluators
# for one question do not fail by being approximate, they fail by DISAGREEING.
.PHONY: test-canvas test-canvas-negctl test-canvas-readback-negctl canvas-link-check
.PHONY: test-canvas-refuse-negctl test-canvas-text-negctl

# ---------------------------------------------------------------------------
# THE SOURCE LIST, AS A SUBTRACTION -- it was a hand copy and it drifted
# ---------------------------------------------------------------------------
# Measured 2026-08-28. Fourteen js_*.c translation units landed in
# c/apps/browser after this list was written and not one of them reached it,
# so `make test-canvas` died at the link with fifteen undefined symbols:
# js_anim_install, js_cssom_install, js_cssom_close, js_domparser_install,
# js_events_install, js_forms_install, js_media_install, js_media_close,
# js_reflect_install, js_semantics_install, js_tokenlist_install,
# js_url_install, ci_transform_parse (css_engine.c -> css_interp.c) and
# layout_count / layout_items (js_dom.c). `ci-host: test-canvas` at the bottom
# of this file means CI was carrying a target that could not build, and
# CLAUDE.md went on quoting "46 checks" the whole time. It is 46 again, run.
#
# THE WORSE HALF, and it is why "the link is broken" was the wrong summary:
# every one of those fifteen is a WEAK declaration. On any host with ELF's
# weak-undefined semantics the link SUCCEEDED, the installers resolved to
# NULL, js_page.c's `if (js_events_install)` guards stepped over them, and
# this gate reported itself green while measuring a browser with seven of its
# twenty js TUs missing -- the same failure tests/wpt.mk records three times
# in its own header ("the runner was measuring a browser that does not
# exist"). That is the defect this fragment had; the Mach-O link error was
# only the noise that made it visible.
#
# NINE weak references SURVIVE the fix and are meant to: js_forms_install,
# js_media_install/_close (the three TUs named OUT below) and layout_count /
# layout_items / layout_page / layout_height / layout_node_box /
# layout_node_scroll from js_dom.c and js_cssom.c. This harness cannot link
# layout.c -- see the js_forms.c note -- so it RELIES on the weak mechanism
# working, i.e. on include/weaksym.h. It is not a list that can shrink to
# nothing here.
#
# So the js side is a SUBTRACTION from the Makefile's own $(BROWSER_JS_SRC)
# (Makefile:828 -- `browser.c` plus `$(wildcard c/apps/browser/js_*.c)`, the
# same variable $(BUILD)/browser.elf is built from). A file added to the
# browser is on this link line the same minute; a file that must NOT be here
# is named below, once, with its reason. canvas-link-check then only has to
# ask whether any of those names has gone stale.
ifeq ($(strip $(BROWSER_JS_SRC)),)
CANVAS_LINK_ERR := tests/canvas.mk: BROWSER_JS_SRC is empty -- this fragment must be -included from the Makefile, AFTER it. Refusing to link a canvas test from a partial source list.
endif

# WHAT IS DELIBERATELY OUT. Each line is a claim, so each gets a reason:
#   browser.c        the ring-3 app shell; it is what $(BROWSER_JS_SRC) adds
#                    on top of the js_*.c wildcard. Window management and a
#                    ring-3 `main`, neither of which a host binary can use.
#   js_media.c       includes c/apps/logit.h and calls gui_blit/snd_write/
#                    monotonic_ns as `int 0x80` syscalls.
#   js_media_src.c   the engine half: media.h, h264.h, h265.h, aac.h, mp3.h --
#                    it drags c/lib/media, c/lib/video and c/lib/audio in
#                    behind it. tests/wpt.mk pays that price on purpose and
#                    says so; this gate is about pixels a 2D context wrote.
#   js_forms.c       calls forms.c's fc_*, and forms.c calls text_measure --
#                    a ring-3 syscall every host harness defines for itself.
#                    tests/unit/canvas_test.c does not (forms_test.c,
#                    layout_test.c and wpt_test.c each do), so forms.c cannot
#                    be linked here and js_forms.c cannot be linked without it.
#   js_module.c      its loader calls bfetch_resolve/bfetch_sync from
#                    browser_rt.c. Only tests/unit/webapi_probe.c supplies
#                    those (:392 and :452); linking browser_rt.c here would
#                    bring the whole GUI/socket shell with it.
#   js_worker.c      SAME REASON, and it is the subtraction working exactly as
#                    designed rather than a new problem. js_worker.c landed
#                    after this list was written, was linked here the same
#                    minute without anyone remembering -- which is the point --
#                    and brought bfetch_resolve/bfetch_sync with it
#                    (js_worker.c:427 and :455 call them to load a worker's
#                    script). Measured 2026-08-29: `make test-canvas` died at
#                    the link with those two undefined plus eight ws_* from
#                    js_websocket.c. The ws_* half was NOT excluded -- see
#                    CANVAS_SRC below -- because c/net/http/ws.c is a plain
#                    host-linkable TU (tests/unit/ws_test.c already links it),
#                    so the right answer there was to supply the dependency
#                    rather than drop the file. A name only goes on this list
#                    when supplying what it needs would drag ring 3 in.
CANVAS_JS_OUT = c/apps/browser/browser.c \
                c/apps/browser/js_media.c c/apps/browser/js_media_src.c \
                c/apps/browser/js_forms.c c/apps/browser/js_module.c \
                c/apps/browser/js_worker.c
CANVAS_JS_SRC = $(filter-out $(CANVAS_JS_OUT),$(BROWSER_JS_SRC))

# DEFERRED (=) not immediate (:=) on purpose: this fragment is -included at
# Makefile:2935 and IMG_HOST_SRC/GFX_SRC are defined at :3343. With := these
# would capture the EMPTY value and the link would fail on forty gfx symbols
# with nothing in the error naming the ordering. Anything here that references
# a variable from further down the Makefile has to be deferred.
#
# css_engine.c, css_vars.c and css_interp.c are named rather than derived:
# they are not in $(BROWSER_JS_SRC) (css_engine.c rides in $(CSS_OBJ), the
# other two in $(BROWSER_PIPE)) and canvas-link-check does not cover
# $(BROWSER_PIPE) -- see the note on that target. css_interp.c is the
# ci_transform_parse in the list above: css_engine.c grew a call to it and
# this line did not follow.
CANVAS_SRC = tests/unit/canvas_test.c $(CANVAS_JS_SRC) \
              c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
              c/apps/browser/css_interp.c \
              c/net/http/http1.c c/net/http/url.c \
              c/net/http/cookies.c $(CANVAS_WS_SRC) $(IMG_HOST_SRC) \
              $(HTML_PARSER_SRC)
# js_websocket.c's dependency, named the same way tests/ws.mk names it (that
# fragment's WS_SRC is the authority on what ws.c needs: the framer, plus
# base64 and sha1 for the Sec-WebSocket-Accept handshake). All three are plain
# host-linkable TUs, which is why supplying them beat excluding js_websocket.c.
CANVAS_WS_SRC = c/net/http/ws.c c/net/ssh/base64.c c/crypto/hash/sha1.c
CANVAS_CF  = $(BTEST_INC) $(CSS_INC) $(JS_INC) $(IMG_HOST_INC) -Iinclude/abi \
              -Ic/net/http -Ic/net/ssh -Ic/crypto \
              -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

# The drift check, as a target rather than a habit. The subtraction makes the
# dangerous direction impossible -- a new js_*.c is linked without anyone
# remembering -- so only the other direction is left: a name in CANVAS_JS_OUT
# that the browser no longer links excludes nothing, and has quietly stopped
# being a decision. A RENAMED file is the bad case: its old name sits here
# reading like a live exclusion while its new one is silently linked.
#
# It does NOT cover $(BROWSER_PIPE), and that is a stated limit rather than an
# oversight: excluding layout.c/forms.c/browser_rt.c from a host harness is
# forced by text_measure and bfetch (above), so the ten names it would need
# are constraints of this test file, not decisions about the browser. The
# wildcard is the list that grows -- fourteen files in one stretch -- and it
# is the one covered.
canvas-link-check:
	@if [ -n "$(CANVAS_LINK_ERR)" ]; then echo "$(CANVAS_LINK_ERR)"; exit 1; fi
	@stale=""; for f in $(CANVAS_JS_OUT); do \
	    case " $(BROWSER_JS_SRC) " in *" $$f "*) ;; *) stale="$$stale $$f";; esac; \
	  done; \
	  if [ -n "$$stale" ]; then \
	    echo "canvas-link-check: FAIL -- CANVAS_JS_OUT names files the browser no"; \
	    echo "  longer links, so the exclusion is a lie rather than a decision:"; \
	    for f in $$stale; do echo "    $$f"; done; \
	    echo "  If one was RENAMED, its new name is being linked silently."; exit 1; \
	  fi

test-canvas: canvas-link-check $(BUILD)/libcss_host.a $(RUST_LIB_HOST) \
             test-canvas-negctl test-canvas-readback-negctl test-canvas-b64-negctl \
             test-canvas-refuse-negctl test-canvas-text-negctl
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CANVAS_CF) -o $(BUILD)/canvas_test \
	    $(CANVAS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@rm -f $(BUILD)/canvas_urls.txt
	@CANVAS_URL_DUMP=$(BUILD)/canvas_urls.txt $(BUILD)/canvas_test
	@python3 tests/unit/canvas_b64_ext_test.py $(BUILD)/canvas_urls.txt

# THE NEGATIVE CONTROL. -DCANVAS_IGNORE_CTM drops the CTM from point
# transformation -- every point goes to the device unchanged. It is the single
# most plausible wrong implementation of this file (the engine's own path
# object carries a matrix, so "the path will handle it" is the natural
# mistake), and it draws a perfectly good picture in the wrong place: every
# fill, every colour, every ImageData round-trip still passes. Only the
# translate/scale/compose checks redden, which is what makes them the ones
# actually measuring the transform.
test-canvas-negctl: canvas-link-check $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CANVAS_CF) -DCANVAS_IGNORE_CTM -o $(BUILD)/canvas_negctl \
	    $(CANVAS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/canvas_negctl > $(BUILD)/canvas_negctl.log 2>&1; then \
	   echo "test-canvas-negctl: FAILED -- the suite PASSED with the CTM ignored,"; \
	   echo "  so nothing in it is measuring the transform."; exit 1; \
	 else \
	   echo "test-canvas-negctl: ok -- ignoring the CTM fails the gate:"; \
	   grep '^FAIL:' $(BUILD)/canvas_negctl.log | head -8; \
	 fi

# THE SECOND NEGATIVE CONTROL, and it exists because the first one cannot see
# the readback at all. -DCANVAS_READBACK_BLANK makes toDataURL/toBlob ignore the
# backing store and encode a correctly-sized PNG of transparent black.
#
# It is a control about ONE sentence, the one js_canvas.c's header used to close
# with: "a fabricated data URL is the single most load-bearing lie a canvas can
# tell ... a wrong one is believed rather than detected". With this flag on, the
# output is a real PNG, at the right size, with a valid signature, valid chunk
# CRCs, a valid zlib stream and the right IHDR -- and the wrong picture. Every
# assertion about the SHAPE of the file still passes. Only the ones that name a
# PIXEL redden.
#
# So this target asserts two things and the second is the interesting one:
# the suite must FAIL, and the failures must be the pixel checks. A gate that
# reddened here on "the URL does not start with data:image/png" would be
# telling us its readback assertions are structural, which is the same as not
# having them.
test-canvas-readback-negctl: canvas-link-check $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CANVAS_CF) -DCANVAS_READBACK_BLANK -o $(BUILD)/canvas_blank \
	    $(CANVAS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/canvas_blank > $(BUILD)/canvas_blank.log 2>&1; then \
	   echo "test-canvas-readback-negctl: FAILED -- the suite PASSED with the canvas"; \
	   echo "  readback replaced by a well-formed PNG of transparent black. Nothing"; \
	   echo "  in it is measuring the PICTURE, only the shape of the file -- which"; \
	   echo "  is exactly the fabrication the toDataURL refusal existed to prevent."; \
	   exit 1; \
	 fi
	@if grep -q '^FAIL: pixel (0,0)' $(BUILD)/canvas_blank.log; then \
	   echo "test-canvas-readback-negctl: ok -- a valid PNG of the wrong pixels"; \
	   echo "  reddens the gate, and reddens it on the PIXELS:"; \
	   grep '^FAIL:' $(BUILD)/canvas_blank.log | head -5; \
	 else \
	   echo "test-canvas-readback-negctl: FAILED -- the suite went red, but NOT on a"; \
	   echo "  pixel assertion. Whatever it caught, it was not the wrong picture:"; \
	   grep '^FAIL:' $(BUILD)/canvas_blank.log | head -5; exit 1; \
	 fi

# THE THIRD NEGATIVE CONTROL, and its shape is the argument for having a second
# oracle at all -- it is the one control here that the C suite must NOT catch.
#
# -DCANVAS_B64_NOPAD drops the '=' from b64_encode. That is a real defect: the
# data: URL it produces is refused by python, by libpng's callers, and by the
# URL parser in every other browser. tests/unit/canvas_test.c does not notice,
# and cannot, because it reads every URL back through js_platform.c's atob --
# whose second statement is `if (s.length % 4 === 0) s = s.replace(/==?$/, '')`
# and which refuses only `length % 4 === 1`. Unpadded input decodes there
# perfectly, so all 68 checks stay green.
#
# The two implementations really are independent -- a C table against a JS shim
# -- which makes the round-trip a genuine differential for the alphabet and the
# bit packing. It is simply not one for the padding, because a lenient decoder
# agrees with a sloppy encoder. That is tests/pngenc.mk's finding one layer up:
# "a wrong CRC polynomial that both sides compute the same way round-trips
# perfectly and is rejected by every other program on earth."
#
# So this target asserts BOTH halves, and fails if either stops holding:
#   1. the C suite must still PASS. If it ever starts catching this, the
#      sentence above has gone stale and the comment must be rewritten before
#      somebody trusts it.
#   2. the python oracle must FAIL. If it stops, the external leg has stopped
#      being external -- and it is the only thing standing between this browser
#      and a data URL that only this browser can read.
.PHONY: test-canvas-b64-negctl
test-canvas-b64-negctl: canvas-link-check $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CANVAS_CF) -DCANVAS_B64_NOPAD -o $(BUILD)/canvas_nopad \
	    $(CANVAS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@rm -f $(BUILD)/canvas_nopad_urls.txt
	@if CANVAS_URL_DUMP=$(BUILD)/canvas_nopad_urls.txt $(BUILD)/canvas_nopad \
	      > $(BUILD)/canvas_nopad.log 2>&1; then \
	   echo "test-canvas-b64-negctl: the C suite passes unpadded base64, as"; \
	   echo "  documented -- atob strips padding before decoding, so the"; \
	   echo "  round-trip cannot see this. That is why there is a second oracle."; \
	 else \
	   echo "test-canvas-b64-negctl: FAILED -- the C suite CAUGHT missing padding."; \
	   echo "  That is good news and a stale comment: js_canvas.c and this file"; \
	   echo "  both claim atob is blind to it. Re-read atob and rewrite them."; \
	   grep '^FAIL:' $(BUILD)/canvas_nopad.log | head -5; exit 1; \
	 fi
	@if python3 tests/unit/canvas_b64_ext_test.py $(BUILD)/canvas_nopad_urls.txt \
	      > $(BUILD)/canvas_nopad_ext.log 2>&1; then \
	   echo "test-canvas-b64-negctl: FAILED -- the EXTERNAL oracle passed unpadded"; \
	   echo "  base64 too. Both oracles are now blind to the same defect, which"; \
	   echo "  means the second one is no longer independent of the first."; \
	   exit 1; \
	 else \
	   echo "test-canvas-b64-negctl: ok -- python catches what atob cannot:"; \
	   grep -E '^(FAIL|canvas_b64)' $(BUILD)/canvas_nopad_ext.log | head -5; \
	 fi

# THE FOURTH CONTROL, and it is the one that exists so a SENTENCE can be
# measured rather than inherited.
#
# -DCANVAS_READBACK_REFUSE restores the behaviour that shipped until 2026-08-29:
# toDataURL and toBlob throw. It is the BEFORE picture as a build flag, and it
# was added because the claim downstream of this whole change -- "the page now
# gets further" -- is a COMPARISON, and every run before this flag existed was an
# after-run. The before half was quoted from the complaint that started the work.
# CLAUDE.md rule 1 is exactly that shape: the measurement is right and the
# sentence around it sends the reader somewhere else.
#
# Its guest use is the point (build browser.aex with it, load the same real page
# on the same disk image, diff the two records -- and be willing to report "no
# difference", which is a finding and not a failure). This host target exists so
# the flag cannot rot: a #ifdef nothing ever compiles stops compiling silently,
# and this is a tree with five documented instances of exactly that.
#
# What it asserts is deliberately WEAKER than test-canvas-readback-negctl, and
# the difference is worth stating. BLANK proves the pixel checks measure the
# PICTURE. REFUSE only proves the suite reaches the readback at all -- if it
# passed with both entry points throwing, nothing in it would be exercising
# them. Both are true statements about different things.
.PHONY: test-canvas-refuse-negctl
test-canvas-refuse-negctl: canvas-link-check $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CANVAS_CF) -DCANVAS_READBACK_REFUSE -o $(BUILD)/canvas_refuse \
	    $(CANVAS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/canvas_refuse > $(BUILD)/canvas_refuse.log 2>&1; then \
	   echo "test-canvas-refuse-negctl: FAILED -- the suite PASSED with toDataURL"; \
	   echo "  and toBlob restored to throwing, so nothing in it reaches the"; \
	   echo "  readback and the before/after comparison has no second term."; \
	   exit 1; \
	 else \
	   echo "test-canvas-refuse-negctl: ok -- the pre-2026-08-29 refusal reddens"; \
	   echo "  the gate, so the flag still compiles and still means what it says:"; \
	   grep '^FAIL:' $(BUILD)/canvas_refuse.log | head -4; \
	 fi

# THE FIFTH CONTROL (2026-08-30), and the one that exists because of how the
# text/image half of this file actually died.
#
# -DCANVAS_TEXT_ABSENT compiles js_canvas.c with the fillText/strokeText/
# measureText/drawImage registrations LEFT OUT -- every function body still
# compiles, none is reachable from JS. That is not a hypothetical wrong
# implementation, it is the PRE-2026-08-30 build as a flag: the dead canvas
# agent of that day wrote all the bodies and never reached cv_proto_funcs, so
# the tree compiled clean and `typeof ctx.fillText` read "undefined" with a
# thousand lines of working text code one table away. "Implemented but
# unregistered" is invisible to a compile and to a link, which is precisely
# the "linking a translation unit is not running it" failure tests/wpt.mk
# records three times, one layer down.
#
# As a prerequisite of test-canvas (the audit rule: named on a ci- line it
# would satisfy the wiring check and run never), and it asserts BOTH halves:
# the suite must FAIL, and the failures must be the text/drawImage checks --
# a build that reddened only on some unrelated check would be proving the
# flag changes SOMETHING, not that these checks measure this feature.
.PHONY: test-canvas-text-negctl
test-canvas-text-negctl: canvas-link-check $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CANVAS_CF) -DCANVAS_TEXT_ABSENT -o $(BUILD)/canvas_text_absent \
	    $(CANVAS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/canvas_text_absent > $(BUILD)/canvas_text_absent.log 2>&1; then \
	   echo "test-canvas-text-negctl: FAILED -- the suite PASSED with the text and"; \
	   echo "  image methods unregistered, so nothing in it measures them. This is"; \
	   echo "  the dead-agent state this flag exists to catch."; exit 1; \
	 fi
	@if grep -q '^FAIL: measureText' $(BUILD)/canvas_text_absent.log; then \
	   echo "test-canvas-text-negctl: ok -- the pre-2026-08-30 build (bodies present,"; \
	   echo "  registrations absent) reddens the gate, and on the text checks:"; \
	   grep '^FAIL:' $(BUILD)/canvas_text_absent.log | head -6; \
	 else \
	   echo "test-canvas-text-negctl: FAILED -- the suite went red, but NOT on a"; \
	   echo "  text check. The flag is proving something other than what it says:"; \
	   grep '^FAIL:' $(BUILD)/canvas_text_absent.log | head -6; exit 1; \
	 fi

# --- test-canvas-readback-os: the same claim, IN THE GUEST ------------------
#
# CLAUDE.md rule 1, and it cost this session a wrong answer before this file was
# touched: "MEASURE IN THE GUEST -- tests/unit/webapi_probe.c does not link
# every TU the browser links, and reported a whole subsystem absent that the
# browser has." test-canvas above links js_canvas.c, the Rust staticlib and
# QuickJS on arm64; it does NOT link browser_rt.c, whose kmalloc shim is what
# the encoder allocates through in ring 3, and the browser runs x86_64 under
# TCG. A green host gate is not evidence about the binary the owner runs.
#
# It also asserts the one thing no host gate can: that the SCRIPT KEPT RUNNING
# past the fingerprint, visible as a colour on the screen that appears in no
# stylesheet on the page. The failure being closed was never "the check is
# refused", it was "the check never appeared".
#
# Boot target, minutes, so it is not a prerequisite of test-canvas -- but it IS
# named on ci-boot, because an encoder nobody has watched run on the machine is
# the "linking a translation unit is not running it" failure this tree records
# three times in tests/wpt.mk's header.
.PHONY: test-canvas-readback-os
test-canvas-readback-os: $(ISO) $(DISK)
	python3 tests/qmp/qmp_canvas_readback.py $(ISO) $(DISK)

ci-boot: test-canvas-readback-os

# Named on the suite so it runs, and its controls are prerequisites of the
# positive above so the controls run too -- the two halves of not being in
# tests/audit-stranded.baseline. `ci-host:` accepts prerequisites from any
# fragment, so membership is this one line in the file that owns the target.
ci-host: test-canvas

# --- TWO INSTRUMENTS, AND WHY THEY ARE `probe-` AND NOT `test-` -------------
#
# Both measure and neither asserts, so calling either a test would put a target
# in the tree that CANNOT FAIL -- rule 5, and tools/check-test-liveness.py
# exists to find exactly that. probe-canvas-fingerprint is EXPECTED to report a
# throw today (fillText), and a gate whose passing condition is "it threw where
# we predicted" would go green forever the day somebody implemented fillText.
# They get targets rather than living as loose scripts because that is the
# defect CLAUDE.md's performance section records about qmp_repaint.py: "the
# instrument built for exactly this question, and NO MAKE TARGET HAD EVER RUN
# IT". A script with no target is not reachable, and a reader concludes the
# measurement was never made.
#
# Neither is on ci-boot. They boot QEMU, they answer questions rather than
# holding a line, and putting a non-asserting target on a suite trains people
# to read green as evidence.
.PHONY: probe-canvas-fingerprint probe-regex-turnstile
probe-canvas-fingerprint: $(ISO) $(DISK)
	python3 tests/qmp/qmp_canvas_fingerprint.py $(ISO) $(DISK)

# The wall AFTER the canvas one, pinned to a construct rather than to a file.
# See the script's header: nowsecure.nl now fetches and executes Cloudflare's
# Turnstile loader, which dies compiling a regex that V8 accepts.
probe-regex-turnstile: $(ISO) $(DISK)
	python3 tests/qmp/qmp_regex_turnstile.py $(ISO) $(DISK)
