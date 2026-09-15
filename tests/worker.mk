# tests/worker.mk -- dedicated Worker (c/apps/browser/js_worker.c).
#
#   make test-worker                    the coherent-subset suite, PLUS the
#                                        termination-bar quiescence check:
#                                        every Worker this run produced must
#                                        have reached a terminal state within
#                                        a bounded pump, AND the worker task
#                                        queue (js_worker_pending()) must be
#                                        EMPTY -- not just "every visible
#                                        effect eventually happened" but "the
#                                        scheduler actually drained".
#   make test-worker-negctl-terminate   the SAME quiescence check, linked
#                                        against -DJS_WORKER_NO_TERMINATE
#                                        (js_worker.c's own control:
#                                        mark_dead_ex() skips
#                                        cancel_worker_tasks(), so a queued
#                                        task survives terminate() instead of
#                                        being dropped immediately) -- must
#                                        FAIL, specifically on the terminate()
#                                        control check and nothing upstream
#                                        of it.
#   make test-worker-negctl-silent      the SAME file, linked against
#                                        -DJS_WORKER_SILENT_ERROR
#                                        (deliver_error_to_parent() becomes a
#                                        no-op) -- must FAIL, on every
#                                        error-delivery scenario at once.
#
# js_worker.c landed 2026-08-28 fully wired into js_page.c (weak symbols, so
# a build without this file is unchanged) but with NO gate anywhere: no
# make target, no host test, and its own two negative controls existed only
# as #ifdefs nothing built. That is CLAUDE.md rule 4 (a gate nobody runs is a
# gate that rots) and rule 5 (a control that cannot be watched failing is
# worse than no control) landing on brand-new code the day it was written.
#
# WORKER_JS_OUT mirrors tests/idb.mk's IDB_JS_OUT -- a HAND-COPIED EXCLUSION
# LIST over BROWSER_JS_SRC's wildcard, on purpose and for the same reason
# stated there: a positive list would silently drift the other way. This one
# excludes what Worker does not need (media/forms/module/canvas/semantics/
# reflect/urlbind/websocket) and, unlike IDB_JS_OUT, does NOT exclude
# js_worker.c itself -- that file is what this fragment measures.
WORKER_JS_OUT := c/apps/browser/browser.c \
                 c/apps/browser/js_media.c c/apps/browser/js_media_src.c \
                 c/apps/browser/js_forms.c c/apps/browser/js_module.c \
                 c/apps/browser/js_canvas.c c/apps/browser/js_semantics.c \
                 c/apps/browser/js_reflect.c c/apps/browser/js_urlbind.c \
                 c/apps/browser/js_websocket.c

# tests/unit/loader_fakebfetch.c is bfetch.h implemented against an in-memory
# site -- js_worker.c's own startup fetch (bfetch_sync) and importScripts
# (bfetch_resolve + bfetch_sync) both go through it, synchronously, which is
# what lets this fragment drive real cross-origin / 404 / importScripts
# scenarios without a socket.
WORKER_TEST_SRC := tests/unit/worker_test.c tests/unit/loader_fakebfetch.c \
                    $(filter-out $(WORKER_JS_OUT),$(BROWSER_JS_SRC))
WORKER_TEST_SRC += c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_interp.c
WORKER_TEST_SRC += c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c
WORKER_TEST_SRC += tests/unit/rust_host_shim.c
WORKER_CF := $(BTEST_INC) $(CSS_INC) $(JS_INC) -Itests/unit -Iinclude/abi -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

.PHONY: test-worker test-worker-negctl-terminate test-worker-negctl-silent

$(BUILD)/worker_test: $(WORKER_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WORKER_CF) -o $@ $(WORKER_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# test-worker-negctl-* are PREREQUISITES, not siblings -- NOT_CI drops every
# `test-*-negctl` from the suite listing on the ground that a control is "run
# by its positive counterpart", and tools/audit_tests.py flags one that is
# not a prerequisite of anything as a NEW stranded control. A control that
# runs never reads exactly like a control that passes. tests/license.mk and
# tests/logreporter.mk are the worked examples; tests/idb.mk is the worked
# example for THIS shape specifically.
test-worker: test-worker-negctl-terminate test-worker-negctl-silent $(BUILD)/worker_test
	@$(BUILD)/worker_test

# -DJS_WORKER_NO_TERMINATE: see worker_test.c's own "THE CONTROL" block. Must
# fail on exactly that check (and the quiescence check downstream of it,
# since the dropped-immediately assertion is what quiescence also depends
# on) -- every OTHER scenario (message roundtrip, 404, cross-origin, throw,
# nested worker, importScripts, transferables) must still pass, because
# -DJS_WORKER_NO_TERMINATE touches only cancel_worker_tasks().
$(BUILD)/worker_test_negctl_terminate: $(WORKER_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WORKER_CF) -DJS_WORKER_NO_TERMINATE -o $@ $(WORKER_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-worker-negctl-terminate: $(BUILD)/worker_test_negctl_terminate
	@if $(BUILD)/worker_test_negctl_terminate > $(BUILD)/worker_negctl_terminate.log 2>&1; then \
	    echo "test-worker-negctl-terminate: FAILED -- the quiescence check PASSED against a"; \
	    echo "  build where terminate() does not drop queued tasks, so it is not measuring"; \
	    echo "  the property it claims to."; \
	    exit 1; \
	 else \
	    echo "test-worker-negctl-terminate: ok -- the control catches it:"; \
	    grep -E 'FAIL' $(BUILD)/worker_negctl_terminate.log | head -8; \
	 fi

# -DJS_WORKER_SILENT_ERROR: deliver_error_to_parent() becomes a no-op, so
# EVERY error-delivery scenario (404, cross-origin, uncaught throw) is
# expected to fail -- unlike the terminate control, this one is not isolated
# to one check, and the log is expected to show several.
$(BUILD)/worker_test_negctl_silent: $(WORKER_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WORKER_CF) -DJS_WORKER_SILENT_ERROR -o $@ $(WORKER_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-worker-negctl-silent: $(BUILD)/worker_test_negctl_silent
	@if $(BUILD)/worker_test_negctl_silent > $(BUILD)/worker_negctl_silent.log 2>&1; then \
	    echo "test-worker-negctl-silent: FAILED -- the quiescence check PASSED against a"; \
	    echo "  build where errors are never delivered to the parent, so it is not"; \
	    echo "  measuring the property it claims to."; \
	    exit 1; \
	 else \
	    echo "test-worker-negctl-silent: ok -- the control catches it:"; \
	    grep -E 'FAIL' $(BUILD)/worker_negctl_silent.log | head -8; \
	 fi

# --- test-wpt-worker: the WPT subset, js_worker.c linked (it rides
# WPT_TEST_SRC's own $(wildcard c/apps/browser/js_*.c) automatically -- no
# source list to keep in sync). A REAL RATCHET, not a raw-count measurement:
# workers/ is not one of the 6 directories tests/unit/wpt_expected_fail.txt
# covers (that baseline was taken over build/wpt, not build/wpt-full), so
# before this file existed nothing in this tree would go red on a Worker
# regression -- js_worker.c could break silently and every OTHER gate would
# stay green. wpt-worker-baseline writes tests/unit/wpt_worker_fail.txt
# against build/wpt-full; test-wpt-worker checks against it and --strict
# fails on either a NEW failure or a passing subtest going away. Read FILES
# REVIVED beside the percentage, not instead of it -- workers/modules staying
# at 0/N here is CORRECT (module workers are refused by name), so a nonzero
# there would mean the refusal quietly turned into a half-implementation.
WORKER_BASELINE := tests/unit/wpt_worker_fail.txt
WORKER_WPT_ROOT := $(if $(wildcard build/wpt-full/workers),build/wpt-full,$(WPT_ROOT))

test-wpt-worker: $(BUILD)/wpt_test
	@if [ ! -d $(WORKER_WPT_ROOT)/workers ]; then \
	    echo "test-wpt-worker: SKIPPED (no workers/ under $(WORKER_WPT_ROOT) -- run 'make wpt-fetch' or fetch build/wpt-full)"; exit 0; fi
	@WEBAPI_FILE_ROOT=$(WORKER_WPT_ROOT) $(BUILD)/wpt_test --root $(WORKER_WPT_ROOT) --subset workers -b $(WORKER_BASELINE) \
	    $(if $(V),-v $(V),) $(if $(STRICT),--strict,)

wpt-worker-baseline: $(BUILD)/wpt_test
	@if [ ! -d $(WORKER_WPT_ROOT)/workers ]; then \
	    echo "wpt-worker-baseline: SKIPPED (no workers/ under $(WORKER_WPT_ROOT))"; exit 0; fi
	@WEBAPI_FILE_ROOT=$(WORKER_WPT_ROOT) $(BUILD)/wpt_test --root $(WORKER_WPT_ROOT) --subset workers -b $(WORKER_BASELINE) --write-baseline

# Same reasoning as tests/idb.mk's closing comment: both host-only, both skip
# loudly (not falsely-green) when their optional input is absent, and both
# wired onto ci-host so tools/ci.sh's recipe-derived host/boot split picks
# them up without a hand-maintained suite list to forget.
ci-host: test-worker test-wpt-worker

WORKER_CONTEXT_SRC = $(filter-out tests/unit/worker_test.c,$(WORKER_TEST_SRC)) tests/unit/worker_context_test.c tests/unit/worker_fetch_net.c
WORKER_CONTEXT_DEPS = $(WORKER_CONTEXT_SRC) tests/unit/worker_test.c tests/unit/worker_fetch_net.h tests/unit/stream_net.h tests/worker.mk $(wildcard c/apps/browser/*.h c/apps/browser/*.inc) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(BUILD)/worker-context/current: $(WORKER_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@$(CC) -O2 -w $(WORKER_CF) -o $@ $(WORKER_CONTEXT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/worker-context/shared: $(WORKER_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@sed 's/worker_owner=s?s:\&worker_default;/worker_owner=\&worker_default;/' c/apps/browser/js_worker.c > $@.c
	@! cmp -s c/apps/browser/js_worker.c $@.c
	@$(CC) -O2 -w $(WORKER_CF) -o $@ $(filter-out c/apps/browser/js_worker.c,$(WORKER_CONTEXT_SRC)) $@.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-worker-context-negctl test-worker-context test-worker-context-san
$(BUILD)/worker-context/unbalanced: $(WORKER_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@sed 's/if(LOGIT_HAVE(js_page_slice_end))js_page_slice_end();/(void)0;/' c/apps/browser/js_worker.c > $@.c
	@! cmp -s c/apps/browser/js_worker.c $@.c
	@$(CC) -O2 -w $(WORKER_CF) -o $@ $(filter-out c/apps/browser/js_worker.c,$(WORKER_CONTEXT_SRC)) $@.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/worker-context/wrong-site: $(WORKER_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@sed 's/worker_owner->policy.site_url:w->creator_origin/w->creator_origin:w->creator_origin/' c/apps/browser/js_worker.c > $@.c
	@! cmp -s c/apps/browser/js_worker.c $@.c
	@$(CC) -O2 -w $(WORKER_CF) -o $@ $(filter-out c/apps/browser/js_worker.c,$(WORKER_CONTEXT_SRC)) $@.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(BUILD)/worker-context/creator-policy: $(WORKER_CONTEXT_DEPS)
	@mkdir -p $(dir $@)
	@sed 's/return !w->policy.allow||w->policy.allow(w->policy.opaque,op,url);/return worker_allowed(op,url);/' c/apps/browser/js_worker.c > $@.c
	@! cmp -s c/apps/browser/js_worker.c $@.c
	@$(CC) -O2 -w $(WORKER_CF) -o $@ $(filter-out c/apps/browser/js_worker.c,$(WORKER_CONTEXT_SRC)) $@.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-worker-context-negctl: $(BUILD)/worker-context/shared $(BUILD)/worker-context/unbalanced $(BUILD)/worker-context/wrong-site $(BUILD)/worker-context/creator-policy
	@rc=0; $(BUILD)/worker-context/shared > $(BUILD)/worker-context/shared.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q 'FAIL.*child pump does not advance parent Worker queue' $(BUILD)/worker-context/shared.log
	@rc=0; $(BUILD)/worker-context/unbalanced > $(BUILD)/worker-context/unbalanced.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q 'FAIL.*context switch after worker callbacks succeeds' $(BUILD)/worker-context/unbalanced.log
	@rc=0; $(BUILD)/worker-context/wrong-site > $(BUILD)/worker-context/wrong-site.log 2>&1 || rc=$$?; test $$rc -eq 1 && test "$$(grep -c '^FAIL:' $(BUILD)/worker-context/wrong-site.log)" -eq 2 && grep -q 'FAIL.*cross-site ancestor prevents Worker SameSite cookie leakage' $(BUILD)/worker-context/wrong-site.log && grep -q 'FAIL.*network Worker retains opaque ancestor cookie site' $(BUILD)/worker-context/wrong-site.log
	@rc=0; $(BUILD)/worker-context/creator-policy > $(BUILD)/worker-context/creator-policy.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q 'FAIL.*network Worker uses response policy and final URL for imports and fetch' $(BUILD)/worker-context/creator-policy.log
test-worker-context: test-worker-context-negctl $(BUILD)/worker-context/current
	@$(BUILD)/worker-context/current
test-worker-context-san: test-worker-context
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(WORKER_CF) -o $(BUILD)/worker-context/san $(WORKER_CONTEXT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/worker-context/san
ci-host: test-worker-context

# Standalone native port ownership needs no page fixture: two actual runtimes
# and the production serializer are sufficient to exercise transfer fences.
NATIVE_PORTS_SRC = tests/unit/native_ports_test.c c/apps/browser/js_ports.c
NATIVE_PORTS_DEP = $(NATIVE_PORTS_SRC) c/apps/browser/js_ports.h $(QJS_SRC) tests/worker.mk
$(BUILD)/native-ports/current: $(NATIVE_PORTS_DEP)
	@mkdir -p $(dir $@)
	@$(CC) -O1 -g -w $(WORKER_CF) -o $@ $(NATIVE_PORTS_SRC) $(QJS_SRC) -lm
$(BUILD)/native-ports/not-detached: $(NATIVE_PORTS_DEP)
	@mkdir -p $(dir $@)
	@$(CC) -O1 -g -w $(WORKER_CF) -DPORT_TEST_NO_DETACH -o $@ $(NATIVE_PORTS_SRC) $(QJS_SRC) -lm
$(BUILD)/native-ports/not-closed: $(NATIVE_PORTS_DEP)
	@mkdir -p $(dir $@)
	@sed 's/if(op==2){close_endpoint(e);return JS_UNDEFINED;}/if(op==2){return JS_UNDEFINED;}/' c/apps/browser/js_ports.c > $@.c
	@! cmp -s c/apps/browser/js_ports.c $@.c
	@$(CC) -O1 -g -w $(WORKER_CF) -o $@ tests/unit/native_ports_test.c $@.c $(QJS_SRC) -lm
.PHONY: test-native-ports test-native-ports-negctl test-native-ports-san
test-native-ports-negctl: $(BUILD)/native-ports/not-detached $(BUILD)/native-ports/not-closed
	@rc=0; $(BUILD)/native-ports/not-detached > $(BUILD)/native-ports/not-detached.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q 'FAIL.*transferred endpoint attaches only in destination runtime' $(BUILD)/native-ports/not-detached.log
	@rc=0; $(BUILD)/native-ports/not-closed > $(BUILD)/native-ports/not-closed.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q 'FAIL.*closed port drops its queued callbacks' $(BUILD)/native-ports/not-closed.log
test-native-ports: test-native-ports-negctl $(BUILD)/native-ports/current
	@$(BUILD)/native-ports/current
test-native-ports-san: test-native-ports
	@$(CC) -O1 -g -w $(WORKER_CF) -fsanitize=address,undefined -fno-omit-frame-pointer -o $(BUILD)/native-ports/san $(NATIVE_PORTS_SRC) $(QJS_SRC) -lm
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/native-ports/san
ci-host: test-native-ports
