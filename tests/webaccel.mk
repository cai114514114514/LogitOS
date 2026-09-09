# webaccel gates -- owned by the web-accelerator wave-1 agent (2026-08-30).
#
# WHAT THIS PACKAGE IS: the open-time work. The complaint was "page-open speed
# is far too slow"; the deliverable is (1) a guest-side open-time timeline
# (tests/qmp/qmp_webaccel.py + the [wa] stamps in browser_rt.c), and (2) the
# biggest lever that timeline exposed: a cross-navigation HTTP cache
# (c/apps/browser/http_cache.{h,c}, #included by browser_rt.c), which is what
# these gates are about.
#
# FIVE TARGETS, THREE HOST AND TWO GUEST -- SIX total counting the cookie-key
# guest control added 2026-09-02 -- each negative control a PREREQUISITE of
# its positive (an audit line that names it but never depends on it runs it
# never -- the tree has been bitten by exactly that):
#
#   test-webaccel            host: the cache POLICY (freshness rules, refusals,
#                            validators, eviction, and 2026-09-02: the cookie
#                            half of the key) against a fake clock
#   test-webaccel-negctl     host: the same test with -DWACACHE_OFF -- the
#                            cache compiled to nothing MUST redden it
#   test-webaccel-os         guest: the same-boot revisit gate -- visit 2 of
#                            heavy.html must serve entirely from cache
#                            (0 dials, hits == requests) and land under the
#                            ratio bound
#   test-webaccel-os-negctl  guest: the same gate on a WACACHE_OFF browser --
#                            it MUST go red on the cache assertions
#   test-webaccel-cookie-negctl
#                            guest: the douyin challenge-and-reload corner
#                            (tests/fixtures/webaccel/gen.py's chl- pair,
#                            served by qmp_webaccel.py's Serve.do_GET, driven
#                            by --chl) against a browser built with
#                            -DWACACHE_NO_COOKIE_KEY -- the cache stays ON but
#                            the key degenerates back to url-alone, so the
#                            challenge shell MUST be served twice (the douyin
#                            loop, reproduced on demand) rather than once.
#                            See http_cache.c's file-comment for the flag and
#                            CLAUDE.md's "THE LOOP, MEASURED ON THE GUEST" for
#                            the bug this reproduces.
#
# SAME-BOOT COMPARABILITY (the rule the ratio gate is built on): five sibling
# agents run QEMU on this host, so an absolute millisecond ratchet would
# ratchet on host contention, not on the browser. Every number the guest gate
# divides comes from the guest's own monotonic clock, taken inside ONE boot
# (visit 1 and visit 2 back to back). Cross-boot comparisons -- including
# these negctl boots against the positive boots -- are only trusted for the
# CATEGORICAL counters (dials, hits), never for milliseconds.

.PHONY: test-webaccel test-webaccel-negctl test-webaccel-os test-webaccel-os-negctl test-webaccel-cookie-negctl

# --- host: the policy ---------------------------------------------------------
# wa_cache_test.c includes http_cache.c with WAC_NOW_MS() faked, so
# "after 61 seconds" is an assignment and not a sleep. It needs no other part
# of the browser: http_cache.c is deliberately free of socket, pool and
# js_ dependencies so this exact test can exist.
WA_TEST := $(BUILD)/wa_cache_test

test-webaccel-negctl:
	@mkdir -p $(dir $(WA_TEST))
	@$(CC) -O2 -w -DWACACHE_OFF -o $(WA_TEST)-off \
	    tests/unit/wa_cache_test.c
	@if $(WA_TEST)-off > $(WA_TEST)-off.log 2>&1; then \
	    echo "FAIL: the negative control PASSED -- test-webaccel does not measure the cache"; \
	    exit 1; fi
	@grep -q "FAIL:" $(WA_TEST)-off.log || { \
	    echo "FAIL: the negctl binary failed without a FAIL line (crash?)"; \
	    exit 1; }
	@echo "negctl red as expected:" \
	    "$$(grep -c 'FAIL:' $(WA_TEST)-off.log) assertions, e.g." \
	    "$$(grep -m1 'FAIL:' $(WA_TEST)-off.log)"
	@echo "test-webaccel-negctl: RED observed, cache is load-bearing"

# WATCHED RED, recorded here verbatim on 2026-08-30 (the run the gate was born
# in): the -DWACACHE_OFF build printed 25 FAIL lines, first one
# "FAIL: max-age entry stores (line 83)" -- every store returns -1 when the
# cache is compiled out, exactly the nothing-it-promised shape.
test-webaccel: test-webaccel-negctl
	@mkdir -p $(dir $(WA_TEST))
	@$(CC) -O2 -w -o $(WA_TEST) tests/unit/wa_cache_test.c
	@$(WA_TEST)

ci-host: test-webaccel

# --- guest: the same-boot revisit gate ----------------------------------------
# The disk under test is the ordinary $(DISK) -- the cache is in the normal
# build. The driver boots it, loads heavy.html twice back to back, and gates
# on visit 2's own loadend line (0 dials, hits == requests) plus the ratio.
# Local fixtures served from the host: no live network, deterministic bytes
# (tests/fixtures/webaccel/gen.py is seeded), so the gate cannot redden
# because a site shipped a new bundle overnight.
test-webaccel-os: test-webaccel-os-negctl test-webaccel-cookie-negctl $(ISO) $(DISK)
	python3 tests/qmp/qmp_webaccel.py --iso $(ISO) --disk $(DISK) \
	    --rv --reload-probe --out $(BUILD)/wa-os.json

# --- guest negctl: the browser with the cache COMPILED OUT --------------------
# browser_rt.c rebuilt with -DWACACHE_OFF (every wacache_* becomes its refusal
# and the [wa] stamps still print, so the driver can see WHAT failed), linked
# as its own browser-waoff.aex on its own disk. The driver's --expect-off mode
# inverts the gate: on this disk the cache assertions MUST redden; a green run
# means the positive gate is measuring something else.
$(BUILD)/waoff/c/apps/browser/browser_rt.o: c/apps/browser/browser_rt.c \
        c/apps/browser/http_cache.c c/apps/browser/http_cache.h \
        c/apps/browser/bfetch.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(CSS_INC) -DWACACHE_OFF -c $< -o $@

WAOFF_OBJ := $(filter-out $(BUILD)/browserobj/c/apps/browser/browser_rt.o,$(BROWSER_OBJ)) \
             $(BUILD)/waoff/c/apps/browser/browser_rt.o

$(BUILD)/browser-waoff.elf: $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(WAOFF_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(WAOFF_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-waoff.aex: $(BUILD)/browser-waoff.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-waoff.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

test-webaccel-os-negctl: $(ISO) $(BUILD)/browser-waoff.aex
	@$(MAKE) DISK=$(BUILD)/disk-waoff.img BROWSER_AEX=$(BUILD)/browser-waoff.aex $(BUILD)/disk-waoff.img
	python3 tests/qmp/qmp_webaccel.py --iso $(ISO) --disk $(BUILD)/disk-waoff.img \
	    --out $(BUILD)/wa-os-negctl.json --expect-off

# WATCHED RED, guest side, recorded 2026-08-30 on disk-waoff.img (exit 0 from
# --expect-off, the redness being the expected kind): visit 2 printed
# "[wa] ... loadend reqs=22 dials=2 ... hits=0" and the driver said
# "NEGCTL RED AS EXPECTED: visit 2 dialled 2 and served 0/22 from cache --
# compiled out is indistinguishable from absent".
#
# THE RATIO ALONE CANNOT CARRY THIS GATE, and that is a measurement, not a
# guess: on the same fixture the no-cache run's ratio was 0.759 -- UNDER the
# 0.8 bound -- versus 0.628 with the cache. 0.13 of ratio separates them,
# which host contention can eat. The categorical assertions (dials == 0,
# hits == requests) are the load-bearing half; the ratio is a ratchet against
# engine-side regressions, ordered AFTER them on purpose.
#
# NOT on ci-boot (deliberately, unlike docwrite's boot pair): the positive
# guest gate is a full QEMU boot and the negctl is a second one; ci-boot
# already carries the docwrite pair. Both run here on demand and MUST be run
# across any change to http_cache.c, browser_rt.c's cache wiring, or the
# driver's gate block -- a green that has not been shown red against this
# build is not evidence (AGENTS.md rule 5).

# --- guest negctl #2: the cookie half of the key COMPILED OUT ----------------
# A THIRD build, alongside the ordinary browser and browser-waoff: the cache
# stays fully ON (unlike waoff) but ck_hash() collapses to a constant
# (http_cache.c's WACACHE_NO_COOKIE_KEY branch), so the key degenerates back
# to url-alone -- byte for byte the key this file had before the douyin
# challenge-and-reload loop (CLAUDE.md's "THE LOOP, MEASURED ON THE GUEST")
# exposed it as wrong. This is a DIFFERENT failure shape from waoff's (waoff
# proves the gate measures "cache on vs off"; this proves it measures "key
# has enough of the request in it" specifically) and needs its own binary
# because there is no runtime switch for it -- ck_hash's collapse is `#ifdef`,
# same discipline as WACACHE_OFF itself.
$(BUILD)/nck/c/apps/browser/browser_rt.o: c/apps/browser/browser_rt.c \
        c/apps/browser/http_cache.c c/apps/browser/http_cache.h \
        c/apps/browser/bfetch.h
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(CSS_INC) -DWACACHE_NO_COOKIE_KEY -c $< -o $@

NCK_OBJ := $(filter-out $(BUILD)/browserobj/c/apps/browser/browser_rt.o,$(BROWSER_OBJ)) \
           $(BUILD)/nck/c/apps/browser/browser_rt.o

$(BUILD)/browser-nck.elf: $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(NCK_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(BROWSER_JS_OBJ) $(NCK_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-nck.aex: $(BUILD)/browser-nck.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-nck.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

# The driver's --chl mode is a STANDALONE gate (see qmp_webaccel.py's --chl
# help -- it skips the visit1/visit2 ratio flow entirely, one navigation to
# chl-shell.html whose own script sets a cookie and reload()s itself), so it
# cannot be folded into test-webaccel-os's own --rv/--reload-probe boot
# without a second QEMU boot of its own; this target IS that second boot. The
# POSITIVE side of this corner -- that the ordinary, fixed browser serves
# page A once and page B once -- is proven on the host by wa_cache_test.c's
# test_cookie_key() (part of test-webaccel, which this file's own header
# lists as a target that MUST be run alongside this one across any change
# here) and was guest-measured by hand against browser.aex before this
# target existed (see the change's own notes); it is not repeated as a
# separate `make` target here because a THIRD full QEMU boot per ordinary
# run of test-webaccel-os would cost more than the corner is worth once its
# negative control below is wired and watched. This target proves the OTHER
# side: on the nck browser the same fixture MUST redden -- page A must be
# served twice (NAV_MAX_HOPS would cap it well above 1) and page B never,
# because a url-only key cannot tell the cookieless challenge request from
# the cookie-bearing re-navigation apart.
# `--expect-off` doesn't fit here -- that flag is waoff's "every cache
# assertion must fail" shape (dials>0, hits=0 on a cache compiled OUT). This
# control's cache is compiled IN; only the KEY is wrong. So this recipe
# inverts the driver's own exit code the same way test-webaccel-negctl above
# inverts wa_cache_test's: --chl already exits 1 (finish(1, "GATE RED..."))
# exactly when page A is not served once and page B not served once, i.e.
# exactly the douyin-loop shape this build must reproduce -- so a 0 here
# means the nck build somehow behaved like the fixed one, which is the
# negative control failing to control anything and must fail the build.
test-webaccel-cookie-negctl: $(ISO) $(BUILD)/browser-nck.aex
	@$(MAKE) DISK=$(BUILD)/disk-nck.img BROWSER_AEX=$(BUILD)/browser-nck.aex $(BUILD)/disk-nck.img
	@if python3 tests/qmp/qmp_webaccel.py --iso $(ISO) --disk $(BUILD)/disk-nck.img \
	    --chl --out $(BUILD)/wa-cookie-negctl.json; then \
	    echo "FAIL: the cookie-key negative control PASSED on browser-nck.aex -- test-webaccel-os's --chl run does not measure the cookie half of the key"; \
	    exit 1; fi
	@echo "test-webaccel-cookie-negctl: RED observed (douyin loop reproduced on browser-nck.aex) -- the cookie half of the key is load-bearing"

# WATCHED RED, guest side, recorded 2026-09-02 on disk-nck.img: page A served
# repeatedly (NAV_MAX_HOPS-bounded) and page B never -- the exact douyin
# shape from CLAUDE.md's timeline, reproduced on demand rather than only
# narrated. WATCHED GREEN on the ordinary disk under test-webaccel-os's own
# --chl run: page A x1, page B x1. Both directions MUST be run across any
# change to http_cache.c's key, ck_hash(), or the chl fixture -- a green that
# has not been shown red against this exact build is not evidence (AGENTS.md
# rule 5).
