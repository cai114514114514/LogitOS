# tests/canvas.mk -- CanvasRenderingContext2D over c/lib/gfx.
#
# In its own fragment for the reason tests/webapi_platform.mk gives: several
# agents edit the top-level Makefile at once, and a fragment is the only way to
# add targets without a commit sweeping up somebody else's half-finished work.
#
# The link is the platform test's set plus js_canvas.c, the engine (GFX_SRC)
# and svg.c -- the last because img_css_color() lives there. That is not a
# convenience: a canvas fillStyle and an SVG fill attribute ask the SAME
# question, and css.h is emphatic that two evaluators for one question do not
# fail by being approximate, they fail by DISAGREEING.
.PHONY: test-canvas test-canvas-negctl

# DEFERRED (=) not immediate (:=) on purpose: this fragment is -included at
# Makefile:2864 and IMG_HOST_SRC/GFX_SRC are defined at :3271. With := these
# would capture the EMPTY value and the link would fail on forty gfx symbols
# with nothing in the error naming the ordering. Anything here that references
# a variable from further down the Makefile has to be deferred.
CANVAS_SRC = tests/unit/canvas_test.c c/apps/browser/js_canvas.c \
              c/apps/browser/js_page.c c/apps/browser/js_dom.c \
              c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
              c/apps/browser/js_platform.c c/apps/browser/js_select.c \
              c/apps/browser/js_intl.c \
              c/apps/browser/js_webapi.c c/net/http/http1.c c/net/http/url.c \
              c/net/http/cookies.c $(IMG_HOST_SRC) \
              $(HTML_PARSER_SRC)
CANVAS_CF  = $(BTEST_INC) $(CSS_INC) $(JS_INC) $(IMG_HOST_INC) -Iinclude/abi \
              -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

test-canvas: $(BUILD)/libcss_host.a $(RUST_LIB_HOST) test-canvas-negctl
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
test-canvas-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
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
