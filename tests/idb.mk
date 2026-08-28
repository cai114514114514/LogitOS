# tests/idb.mk -- IndexedDB (c/apps/browser/js_idb.c).
#
#   make test-idb              a coherent-subset suite: open/upgrade/put/get/
#                               cursor/index/abort-rollback, PLUS the
#                               termination-bar quiescence check: every
#                               IDBRequest and IDBTransaction this run
#                               produced must have reached a terminal state
#                               within a bounded pump of the event loop.
#   make test-idb-negctl       the SAME quiescence check, linked against a
#                               build where IDBObjectStore.prototype.get is
#                               replaced with a request that is never handed
#                               a settle-task -- must FAIL, and specifically
#                               on the one stuck request.
#   make test-wpt-idb          WPT IndexedDB/ with js_idb.c linked, raw count
#                               (see the workflow scope note: reviving 226
#                               files that fail at `indexedDB is not defined`
#                               can lower the RATE while the engine gets
#                               strictly better -- read files-revived, not %).
#
# WHY A SEPARATE FRAGMENT. Same reason tests/events.mk gives for itself:
# several lines edit c/apps/browser/*.mk-adjacent fragments at once, and this
# line's own gate (a bounded pump asserting every request terminates) is not
# something any existing fragment measures.
#
# THE SOURCE LIST is the SAME composition test-platform already proves
# builds and runs against a real js_page_open document (js_platform.c +
# js_select.c + js_intl.c + the rest of BROWSER_JS_SRC, host-side), with two
# changes: js_events.c is ADDED BACK IN (test-platform excludes it because
# ANOTHER line's control asserts its absence -- js_idb.c needs the real,
# constructible EventTarget it publishes and cannot use the placeholder), and
# js_idb.c rides in automatically because BROWSER_JS_SRC is
# `$(wildcard c/apps/browser/js_*.c)`. js_canvas.c/js_semantics.c/js_module.c/
# js_reflect.c/js_urlbind.c/js_websocket.c/js_worker.c stay OUT, matching
# tests/webapi_platform.mk's own reasoning for each (js_module.c and
# js_worker.c both need bfetch_resolve/bfetch_sync this runner does not
# supply; the rest are other lines' work and not a dependency IndexedDB has).
#
# IDB_JS_OUT IS A HAND-COPIED EXCLUSION LIST OVER A WILDCARD, which is the
# shape CLAUDE.md's host-reality table names as taking down whole gates: the
# wildcard grows, this list does not, and the gate fails on a symbol that has
# nothing to do with what it measures. It has already happened once --
# js_worker.c landed on 2026-08-28 and test-idb went red on `undefined
# symbol: bfetch_resolve / bfetch_sync`, i.e. IndexedDB's gate reporting the
# Worker line's dependency. The subtraction is deliberate anyway (a positive
# list would drift the OTHER way, silently dropping a TU IndexedDB needs and
# testing less than it claims), so the rule for the next person is: if this
# gate fails to LINK on a symbol you do not recognise, the answer is almost
# certainly a new c/apps/browser/js_*.c that belongs on this line, not a bug
# in js_idb.c.

.PHONY: test-idb test-idb-negctl test-wpt-idb wpt-idb-baseline

IDB_JS_OUT := c/apps/browser/browser.c \
              c/apps/browser/js_media.c c/apps/browser/js_media_src.c \
              c/apps/browser/js_forms.c c/apps/browser/js_module.c \
              c/apps/browser/js_canvas.c c/apps/browser/js_semantics.c \
              c/apps/browser/js_reflect.c c/apps/browser/js_urlbind.c \
              c/apps/browser/js_websocket.c c/apps/browser/js_worker.c
IDB_TEST_SRC := tests/unit/webapi_idb_test.c $(filter-out $(IDB_JS_OUT),$(BROWSER_JS_SRC))
IDB_TEST_SRC += c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_interp.c
IDB_TEST_SRC += c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c
IDB_TEST_SRC += tests/unit/rust_host_shim.c
IDB_CF := $(BTEST_INC) $(CSS_INC) $(JS_INC) -Iinclude/abi -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

$(BUILD)/idb_test: $(IDB_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(IDB_CF) -o $@ $(IDB_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# test-idb-negctl is a PREREQUISITE, not a sibling. NOT_CI drops every
# `test-*-negctl` from the suite listing on the ground that a control is "run
# by its positive counterpart" -- and tools/audit_tests.py flagged this one as
# a NEW stranded control precisely because nothing made that true. A control
# that runs never reads exactly like a control that passes, which is the
# failure mode this file's whole termination argument depends on NOT having.
# tests/license.mk and tests/logreporter.mk are the worked examples.
test-idb: test-idb-negctl $(BUILD)/idb_test
	@$(BUILD)/idb_test

# The control: -DJS_IDB_NEGCTL links js_idb.c's own stub (see js_idb.c) that
# replaces IDBObjectStore.prototype.get with a request nobody ever schedules
# a settle-task for -- the exact shape js_platform.h used to warn about. The
# SAME test file, unmodified, must report that request (and only that one)
# stuck at readyState 'pending' after the bounded pump.
$(BUILD)/idb_test_negctl: $(IDB_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(IDB_CF) -DJS_IDB_NEGCTL -o $@ $(IDB_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-idb-negctl: $(BUILD)/idb_test_negctl
	@if $(BUILD)/idb_test_negctl > $(BUILD)/idb_negctl.log 2>&1; then \
	    echo "test-idb-negctl: FAILED -- the quiescence check PASSED against a"; \
	    echo "  build with an unscheduled request, so it is not measuring termination."; \
	    exit 1; \
	 else \
	    echo "test-idb-negctl: ok -- the quiescence check catches the stuck request:"; \
	    grep -E 'STUCK|FAIL' $(BUILD)/idb_negctl.log | head -8; \
	 fi

# --- test-wpt-idb: the WPT subset, js_idb.c linked (it rides WPT_TEST_SRC's
# own $(wildcard c/apps/browser/js_*.c) automatically -- no source list to
# keep in sync). A REAL RATCHET now, not the raw-count measurement it started
# as -- IndexedDB/ is not one of the 6 directories tests/unit/wpt_expected_fail.txt
# covers (that baseline was taken over build/wpt, not build/wpt-full; see the
# scope note this fragment's header records), so before this file there was
# nothing that would go red on an IndexedDB regression. wpt-idb-baseline
# writes tests/unit/wpt_idb_fail.txt against build/wpt-full (the corpus this
# directory actually lives in); test-wpt-idb checks against it and --strict
# fails on either a NEW failure or the file dropping below what it already
# passes. Read FILES REVIVED beside the percentage, not instead of it: a file
# that goes from "dies at statement 1" to "runs and mostly fails" adds many
# subtests to the denominator at once (tools/cssom_compare.py's warning).
IDB_BASELINE := tests/unit/wpt_idb_fail.txt
IDB_WPT_ROOT := $(if $(wildcard build/wpt-full/IndexedDB),build/wpt-full,$(WPT_ROOT))

test-wpt-idb: $(BUILD)/wpt_test
	@if [ ! -d $(IDB_WPT_ROOT)/IndexedDB ]; then \
	    echo "test-wpt-idb: SKIPPED (no IndexedDB/ under $(IDB_WPT_ROOT) -- run 'make wpt-fetch' or fetch build/wpt-full)"; exit 0; fi
	@$(BUILD)/wpt_test --root $(IDB_WPT_ROOT) --subset IndexedDB -b $(IDB_BASELINE) \
	    $(if $(V),-v $(V),) $(if $(STRICT),--strict,)

wpt-idb-baseline: $(BUILD)/wpt_test
	@if [ ! -d $(IDB_WPT_ROOT)/IndexedDB ]; then \
	    echo "wpt-idb-baseline: SKIPPED (no IndexedDB/ under $(IDB_WPT_ROOT))"; exit 0; fi
	@$(BUILD)/wpt_test --root $(IDB_WPT_ROOT) --subset IndexedDB -b $(IDB_BASELINE) --write-baseline

# Both host-only, both skip loudly rather than failing when their optional
# input is absent (test-idb needs nothing beyond mini-libc-free host compile;
# test-wpt-idb's own recipe above prints SKIPPED and exits 0 when build/wpt-full
# has no IndexedDB/). Audit was listing both UNWIRED -- reachable from neither
# ci-host nor ci-boot, which is a real gap even though tools/ci.sh itself
# derives host-vs-boot from the recipe rather than from this line (see
# CLAUDE.md's "UNWIRED does not mean CI does not run it") -- a control's
# suite-listing membership is still worth being honest about, and test-idb's
# own negctl prerequisite is exactly the shape rule 5 warns a hand-written
# list can drop silently if nobody adds the line.
ci-host: test-idb test-wpt-idb
