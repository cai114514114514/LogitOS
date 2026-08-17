# --- test-logreporter: the cookie surface, driven by the page that named it -
#
# In its own fragment for the reason tests/webapi_platform.mk gives at its top:
# several agents edit the top-level Makefile at once, and a fragment is the only
# way to add targets without a commit sweeping up somebody else's half-finished
# work. `-include` of a missing file is silently ignored, so losing the one line
# in the Makefile costs the targets and breaks nothing.
#
#   make test-logreporter          the tests
#   make test-logreporter-negctl   the compile-time negative control
#   make test-logreporter-asan     the same under ASan/UBSan + leak check
#
# WHY IT CANNOT LIVE IN test-cookie-cors. That target links js_webapi.c with a
# SYNTHETIC document and no js_page.c at all (tests/unit/stream_net.h:296-299),
# so it can say nothing about js_dom.c's real document, about js_page.c's real
# navigator, or about the install ORDER between them -- and the order is the
# entire subject of navigator.cookieEnabled. This one goes through js_page_open,
# the same call the browser makes.
.PHONY: test-logreporter test-logreporter-negctl test-logreporter-asan

# Wired, not blessed. These went into tests/audit-unwired.baseline when they
# landed, which is the right move for a target nobody can afford to run and
# the wrong one for host links that take seconds. The ASan build is in as
# well: it is the only one of the three that opens and closes the page twice,
# so it is where a leak in js_page_open would show.
#
# The control is a PREREQUISITE, not a third name on the ci-host line --
# audit_tests.py's NOT_CI drops `test-*-negctl` from what ci.sh runs, so
# listing it there would have excluded it and invoked it from nowhere. See
# tests/cookiegate.mk for the measurement behind that.
ci-host: test-logreporter test-logreporter-asan
test-logreporter: test-logreporter-negctl

LOGREP_FIXTURE := tests/fixtures/webapi/logreporter
LOGREP_SRC := tests/unit/logreporter_test.c c/apps/browser/js_page.c \
              c/apps/browser/js_dom.c c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
              c/apps/browser/js_webapi.c c/net/http/http1.c c/net/http/url.c \
              c/net/http/cookies.c tests/unit/rust_host_shim.c
LOGREP_CF  := $(BTEST_INC) $(CSS_INC) $(JS_INC) -Iinclude/abi -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

test-logreporter: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(LOGREP_CF) -o $(BUILD)/logreporter_test $(LOGREP_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@$(BUILD)/logreporter_test $(LOGREP_FIXTURE)

# THE CONTROL, and it is a compile-time one for a reason. Linking js_webapi.o
# out (the way test-webapi-page-control does) would take document.cookie away
# with it, so every assertion in the file would fail and the run would prove
# only that the file needs js_webapi.c. -DWEBAPI_NO_COOKIE_ENABLED removes ONE
# thing -- the navigator.cookieEnabled correction -- leaving js_page.c:610's
# `false` standing, which is exactly what shipped.
#
# EIGHT assertions fail in that build, not the two about the property, and the
# FAIL lines are printed because that cascade IS the measurement. The page asks
# once, is told no, and never reads or writes a cookie again: the id is never
# written, the write never reaches the wire, the widened domain= never happens.
# None of it throws. That is why no exception counter on the scoreboard could
# ever have shown this, and why the row for a site that answers it stays
# PAINTED while the feature is gone.
test-logreporter-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(LOGREP_CF) -DWEBAPI_NO_COOKIE_ENABLED \
	    -o $(BUILD)/logreporter_negctl $(LOGREP_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/logreporter_negctl $(LOGREP_FIXTURE) > $(BUILD)/logreporter_negctl.log 2>&1; then \
	    echo "test-logreporter-negctl: FAILED -- the suite PASSED without the fix,"; \
	    echo "  so its navigator.cookieEnabled assertions are not measuring it."; exit 1; \
	 else \
	    echo "test-logreporter-negctl: ok -- the suite fails without the fix:"; \
	    grep '^FAIL' $(BUILD)/logreporter_negctl.log; \
	 fi

# The page is opened and closed twice and holds JSValues across both; a leak or
# a use-after-free on the second js_page_open is the failure mode that a
# correct-looking pass/fail run cannot see.
test-logreporter-asan: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w $(LOGREP_CF) \
	    -o $(BUILD)/logreporter_asan $(LOGREP_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=1 $(BUILD)/logreporter_asan $(LOGREP_FIXTURE)
