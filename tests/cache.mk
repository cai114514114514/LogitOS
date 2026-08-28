# tests/cache.mk -- CacheStorage (c/apps/browser/js_cache.c) and
# navigator.serviceWorker (c/apps/browser/js_swreg.c), the two parts of the
# ServiceWorker workflow item that ship. Part C -- executing a service
# worker script and intercepting fetch -- is refused by name in both files'
# own headers, not built here or anywhere.
#
#   make test-cache                    the coherent-subset suite for BOTH
#                                       files (CacheStorage/Cache AND
#                                       navigator.serviceWorker), PLUS the
#                                       termination-bar quiescence check:
#                                       every promise this run created must
#                                       have reached a terminal state
#                                       (resolved OR rejected, with the
#                                       RIGHT outcome) within a bounded pump.
#   make test-cache-negctl-put         SAME file, linked against
#                                       -DJS_CACHE_NO_SETTLE (js_cache.c's
#                                       own control: Cache.prototype.put
#                                       becomes a Promise executor that never
#                                       calls resolve/reject -- the exact
#                                       js_platform.h:66-70 shape) -- must
#                                       FAIL, on exactly the put()-shaped
#                                       checks and nothing upstream of them.
#   make test-cache-negctl-register    SAME file, linked against
#                                       -DJS_SWREG_PENDING_REGISTER
#                                       (js_swreg.c's own control:
#                                       register() becomes a Promise executor
#                                       that never settles) -- must FAIL, on
#                                       exactly the register()-shaped checks.
#   make test-cache-quota              a SEPARATE host binary (prerequisite
#                                       of test-cache, like the two negctls),
#                                       compiled with -DJS_CACHE_QUOTA_BYTES=200
#                                       on BOTH js_cache.c and cache_test.c,
#                                       so the QuotaExceededError path is
#                                       reachable without allocating the real
#                                       64 MiB production cap in a host test.
#   make test-wpt-cachestorage         the WPT service-workers/ subset, a
#                                       REAL RATCHET -- service-workers/ is
#                                       in NONE of the 6 directories
#                                       tests/unit/wpt_expected_fail.txt
#                                       covers, so before this file there was
#                                       nothing that would go red on a
#                                       CacheStorage or ServiceWorkerContainer
#                                       regression at all.
#
# CACHE_JS_OUT is a HAND-COPIED EXCLUSION LIST over BROWSER_JS_SRC's own
# wildcard, the same shape tests/idb.mk and tests/worker.mk use and for the
# same reason: js_worker.c and js_websocket.c both need bfetch_resolve/
# bfetch_sync in a shape this fixture (a synchronous, always-succeeding fake
# site) does not have to match, and js_module.c/js_canvas.c/js_semantics.c/
# js_reflect.c/js_urlbind.c are other lines' work this feature has no
# dependency on. If this gate ever fails to LINK on a symbol you do not
# recognise, the answer is almost certainly a new c/apps/browser/js_*.c that
# belongs on this exclusion list, not a bug in js_cache.c/js_swreg.c.
CACHE_JS_OUT := c/apps/browser/browser.c \
                c/apps/browser/js_media.c c/apps/browser/js_media_src.c \
                c/apps/browser/js_forms.c c/apps/browser/js_module.c \
                c/apps/browser/js_canvas.c c/apps/browser/js_semantics.c \
                c/apps/browser/js_reflect.c c/apps/browser/js_urlbind.c \
                c/apps/browser/js_websocket.c c/apps/browser/js_worker.c

CACHE_TEST_SRC := tests/unit/cache_test.c tests/unit/loader_fakebfetch.c \
                   $(filter-out $(CACHE_JS_OUT),$(BROWSER_JS_SRC))
CACHE_TEST_SRC += c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_interp.c
CACHE_TEST_SRC += c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c
CACHE_TEST_SRC += tests/unit/rust_host_shim.c
CACHE_CF := $(BTEST_INC) $(CSS_INC) $(JS_INC) -Itests/unit -Iinclude/abi -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

.PHONY: test-cache test-cache-negctl-put test-cache-negctl-register test-cache-quota \
        test-wpt-cachestorage wpt-cachestorage-baseline

$(BUILD)/cache_test: $(CACHE_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CACHE_CF) -o $@ $(CACHE_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# test-cache-negctl-* are PREREQUISITES, not siblings -- CLAUDE.md rule 5 and
# tests/idb.mk/tests/worker.mk's own comment on the exact same shape: a
# control that is not a prerequisite of anything reads exactly like a
# control that passes, and NOT_CI drops every test-*-negctl from the suite
# listing on the (unverified-per-fragment) ground that a control runs by its
# positive counterpart.
test-cache: test-cache-negctl-put test-cache-negctl-register test-cache-quota $(BUILD)/cache_test
	@$(BUILD)/cache_test

# -DJS_CACHE_NO_SETTLE: js_cache.c's own stub, replacing Cache.prototype.put
# with a Promise executor that never calls resolve or reject. Every put()-
# shaped check in cache_test.c (put, match of what put stored, the four
# put() refusal checks, the Vary puts, both ignoreSearch/ignoreMethod puts,
# add()/addAll() -- everything downstream of a put() this build's fetch
# machinery would otherwise settle) must FAIL or hang inside the bounded
# pump; everything that never calls put() (open/has/keys/delete of an EMPTY
# cache, register(), ready, getRegistration/getRegistrations) must still
# pass, proving the control is isolated to the property it claims to test.
$(BUILD)/cache_test_negctl_put: $(CACHE_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CACHE_CF) -DJS_CACHE_NO_SETTLE -o $@ $(CACHE_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-cache-negctl-put: $(BUILD)/cache_test_negctl_put
	@if $(BUILD)/cache_test_negctl_put > $(BUILD)/cache_negctl_put.log 2>&1; then \
	    echo "test-cache-negctl-put: FAILED -- the quiescence check PASSED against a build"; \
	    echo "  where Cache.prototype.put() never settles, so it is not measuring termination."; \
	    exit 1; \
	 else \
	    echo "test-cache-negctl-put: ok -- the control catches it:"; \
	    grep -E 'STUCK|FAIL' $(BUILD)/cache_negctl_put.log | head -10; \
	 fi

# -DJS_SWREG_PENDING_REGISTER: js_swreg.c's own stub, replacing register()
# with a Promise executor that never settles. Every register()-shaped check
# (register, register-syntax, register-cross-origin, register-scope,
# register-module) must FAIL or hang; every check that never calls
# register() (all of CacheStorage/Cache, ready, getRegistration/
# getRegistrations -- ready and getRegistration do NOT call register()) must
# still pass.
$(BUILD)/cache_test_negctl_register: $(CACHE_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CACHE_CF) -DJS_SWREG_PENDING_REGISTER -o $@ $(CACHE_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-cache-negctl-register: $(BUILD)/cache_test_negctl_register
	@if $(BUILD)/cache_test_negctl_register > $(BUILD)/cache_negctl_register.log 2>&1; then \
	    echo "test-cache-negctl-register: FAILED -- the quiescence check PASSED against a build"; \
	    echo "  where register() never settles, so it is not measuring termination."; \
	    exit 1; \
	 else \
	    echo "test-cache-negctl-register: ok -- the control catches it:"; \
	    grep -E 'STUCK|FAIL' $(BUILD)/cache_negctl_register.log | head -10; \
	 fi

# test-cache-quota: a separate binary. -DJS_CACHE_QUOTA_BYTES=200 must reach
# BOTH js_cache.c (the actual cap) and cache_test.c (the #ifdef'd extra
# check) -- see cache_test.c's CACHE_TEST_QUOTA block. 200 bytes is smaller
# than two ~100-byte responses put in sequence, so the SECOND put() must
# reject QuotaExceededError while the first one (alone, under the cap)
# succeeds -- proving the accounting is a running total, not a per-call check.
$(BUILD)/cache_test_quota: $(CACHE_TEST_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(CACHE_CF) -DJS_CACHE_QUOTA_BYTES=200 -DCACHE_TEST_QUOTA \
	    -o $@ $(CACHE_TEST_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-cache-quota: $(BUILD)/cache_test_quota
	@$(BUILD)/cache_test_quota

# --- test-wpt-cachestorage: the WPT service-workers/ subset, js_cache.c and
# js_swreg.c linked (they ride WPT_TEST_SRC's own $(wildcard
# c/apps/browser/js_*.c) automatically -- no source list to keep in sync).
# WEBAPI_FILE_ROOT is set explicitly, matching tests/worker.mk: omitting it
# changed a measurement 17x during this workflow (407/497 without vs
# 6636/8587 with, same 32 files) -- see the workflow's own scope note.
# workers/modules-style "correctly zero" numbers apply here too:
# service-workers/service-worker/ subtests that assert a worker actually
# RUNS stay failing -- that is this file's refusal working, not a bug.
CACHE_BASELINE := tests/unit/wpt_cachestorage_fail.txt
CACHE_WPT_ROOT := $(if $(wildcard build/wpt-full/service-workers),build/wpt-full,$(WPT_ROOT))

test-wpt-cachestorage: $(BUILD)/wpt_test
	@if [ ! -d $(CACHE_WPT_ROOT)/service-workers ]; then \
	    echo "test-wpt-cachestorage: SKIPPED (no service-workers/ under $(CACHE_WPT_ROOT) -- run 'make wpt-fetch' or fetch build/wpt-full)"; exit 0; fi
	@WEBAPI_FILE_ROOT=$(CACHE_WPT_ROOT) $(BUILD)/wpt_test --root $(CACHE_WPT_ROOT) --subset service-workers -b $(CACHE_BASELINE) \
	    $(if $(V),-v $(V),) $(if $(STRICT),--strict,)

wpt-cachestorage-baseline: $(BUILD)/wpt_test
	@if [ ! -d $(CACHE_WPT_ROOT)/service-workers ]; then \
	    echo "wpt-cachestorage-baseline: SKIPPED (no service-workers/ under $(CACHE_WPT_ROOT))"; exit 0; fi
	@WEBAPI_FILE_ROOT=$(CACHE_WPT_ROOT) $(BUILD)/wpt_test --root $(CACHE_WPT_ROOT) --subset service-workers -b $(CACHE_BASELINE) --write-baseline

# Both host-only, both skip loudly (not falsely-green) when their optional
# input is absent, both wired onto ci-host so tools/ci.sh's recipe-derived
# host/boot split picks them up without a hand-maintained suite list to
# forget. test-cache-quota is a prerequisite of test-cache (like the two
# negctls above), not a separate ci-host line -- CLAUDE.md rule 4 applies to
# it exactly as it does to a negctl: a gate nobody runs is a gate that rots.
ci-host: test-cache test-wpt-cachestorage
