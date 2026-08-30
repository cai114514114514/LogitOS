# webaccel gates -- owned by the web-accelerator wave-1 agent (2026-08-30).
#
# WHAT THIS PACKAGE IS: the open-time work. The complaint was "page-open speed
# is far too slow"; the deliverable is (1) a guest-side open-time timeline
# (tests/qmp/qmp_webaccel.py + the [wa] stamps in browser_rt.c), and (2) the
# biggest lever that timeline exposed: a cross-navigation HTTP cache
# (c/apps/browser/http_cache.{h,c}, #included by browser_rt.c), which is what
# these gates are about.
#
# FOUR TARGETS, TWO HOST AND TWO GUEST, each negative control a PREREQUISITE
# of its positive (an audit line that names it but never depends on it runs it
# never -- the tree has been bitten by exactly that):
#
#   test-webaccel            host: the cache POLICY (freshness rules, refusals,
#                            validators, eviction) against a fake clock
#   test-webaccel-negctl     host: the same test with -DWACACHE_OFF -- the
#                            cache compiled to nothing MUST redden it
#   test-webaccel-os         guest: the same-boot revisit gate -- visit 2 of
#                            heavy.html must serve entirely from cache
#                            (0 dials, hits == requests) and land under the
#                            ratio bound
#   test-webaccel-os-negctl  guest: the same gate on a WACACHE_OFF browser --
#                            it MUST go red on the cache assertions
#
# SAME-BOOT COMPARABILITY (the rule the ratio gate is built on): five sibling
# agents run QEMU on this host, so an absolute millisecond ratchet would
# ratchet on host contention, not on the browser. Every number the guest gate
# divides comes from the guest's own monotonic clock, taken inside ONE boot
# (visit 1 and visit 2 back to back). Cross-boot comparisons -- including
# these negctl boots against the positive boots -- are only trusted for the
# CATEGORICAL counters (dials, hits), never for milliseconds.

.PHONY: test-webaccel test-webaccel-negctl test-webaccel-os test-webaccel-os-negctl

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
test-webaccel-os: test-webaccel-os-negctl $(ISO) $(DISK)
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
