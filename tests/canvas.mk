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
.PHONY: test-canvas test-canvas-negctl canvas-link-check

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
CANVAS_JS_OUT = c/apps/browser/browser.c \
                c/apps/browser/js_media.c c/apps/browser/js_media_src.c \
                c/apps/browser/js_forms.c c/apps/browser/js_module.c
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
              c/net/http/cookies.c $(IMG_HOST_SRC) \
              $(HTML_PARSER_SRC)
CANVAS_CF  = $(BTEST_INC) $(CSS_INC) $(JS_INC) $(IMG_HOST_INC) -Iinclude/abi \
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

test-canvas: canvas-link-check $(BUILD)/libcss_host.a $(RUST_LIB_HOST) test-canvas-negctl
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CANVAS_CF) -o $(BUILD)/canvas_test \
	    $(CANVAS_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/canvas_test

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

# Named on the suite so it runs, and its control is a prerequisite of the
# positive above so the control runs too -- the two halves of not being in
# tests/audit-stranded.baseline. `ci-host:` accepts prerequisites from any
# fragment, so membership is this one line in the file that owns the target.
ci-host: test-canvas
