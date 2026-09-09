# js_prof -- where a page's JavaScript slice actually goes, and the two gates
# that make its numbers worth quoting.
#
# WHY THERE IS A FRAGMENT AT ALL. The profiler and its selftest were written,
# run by hand, and named in no Makefile. CLAUDE.md rule 4 is that a gate nobody
# runs is a gate that rots silently, and rule 1's worked examples are four
# instruments that looked like instruments and were not -- including a kprof
# parser that printed "0 samples over 0 sites" underneath a header saying 8,471
# samples were taken. An unwired profiler is the next entry on that list.
#
# test-jsprof is the HOST gate and it is the one that must stay green: it checks
# the profiler against answers known from somewhere else before any number from
# it is believed. Eight checks, and the two that matter most are negative --
# "pure interpretation reports no native gap" (a gap detector that fired on
# ordinary interpretation would attribute a whole page load to native calls and
# read exactly like a finding) and "profiling changes the work by exactly
# nothing", which compares the WATCHDOG's fuel counter profiled against
# unprofiled. That second one was a fake until 2026-08-30: it ran js_prof's own
# counter twice with the profiler on both times, so it measured run-to-run
# determinism and printed the words "the observer effect" over the top.
#
# test-jsslice is the DEVICE gate and it is deliberately NOT in ci-boot: it
# spends 52 s of guest wall clock doing nothing, on purpose, because that is the
# thing being measured. Run it by hand or from a slice-specific sweep.
JSPROF_PROBE := $(BUILD)/webapi_probe

test-jsprof: $(JSPROF_PROBE)
	@$(JSPROF_PROBE) --prof-selftest

# The device half. Boots once, loads a page with no timers (a page WITH a timer
# re-arms the deadline every callback and is immune, which is why the fixture
# has none), clicks it three times, each with a synchronous nested "scroll"
# dispatch inside the click handler (window.scrollTo -- see the driver's
# NESTED DISPATCH section), and requires the FIXED-BUILD shape: all three
# handlers run, none watchdog-bitten -- including click 2, 52+ s after load,
# which is the case that was bitten before js_dom_dispatch() started bracketing
# every outermost dispatch with js_page_slice_begin()/_end() (c/apps/browser/
# js_dom.c). BEFORE THAT FIX, on a device boot with the bracket removed
# (test-jsslice-negctl's browser-noslice.aex), the field-measured shape was
# click 2 killed on the wall-time rail at fuel=13 of a 2,000,000 budget
# (0.00065%) and click 3 immediately after running clean, because the bite
# clears g_slice_armed ("one interrupt per slice, not a storm") -- see
# qmp_jsslice.py's header for the full arithmetic and why click 1 and click 3
# alone would not distinguish this from a browser that simply dropped clicks.
#
# test-jsslice-negctl IS A PREREQUISITE, not a name on a ci- line: NOT_CI drops
# every test-*-negctl from what ci.sh runs (tests/currentscript.mk's comment
# has the full argument, paid for three times already in this tree), and a
# control that is merely named somewhere runs never while looking wired.
test-jsslice: test-jsslice-negctl
test-jsslice: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_jsslice.py --iso $(ISO) --disk $(DISK) \
	    --out $(BUILD)/jsslice.json | tee $(BUILD)/jsslice.log
	@grep -q '"verdict": "FIXED"' $(BUILD)/jsslice.log || \
	    { echo "FAIL: the shipped browser did not show the fixed slice shape -- read $(BUILD)/jsslice.json"; exit 1; }

# THE NEGATIVE CONTROL: js_dom.c compiled with -DJS_DOM_NO_SLICE_BRACKET
# removes BOTH js_page_slice_begin() and js_page_slice_end() calls from
# js_dom_dispatch() (see the #ifndef there), which is byte-for-byte the
# pre-fix state -- js_dom_dispatch brackets nothing, so every event handler
# inherits whatever deadline the last <script>/timer left armed. The SAME
# driver, pointed at this build's own disk image, must report the BUG-PRESENT
# shape (click 2 bitten, click 3 clean) rather than FIXED; if it reports
# FIXED against a build with the fix compiled OUT, the driver -- not the
# browser -- is broken, e.g. the fixture no longer waits long enough, or a
# second unbracketed re-arm site elsewhere is quietly covering for this one.
#
# $(BROWSER_JS_CF), not $(JS_CF): the root Makefile's ONE target-specific
# variable override strips `-include features.h` from the browser's own TUs;
# a rule here that compiled a c/apps/browser source with raw $(JS_CF) would
# revive the `hidden` macro mismatch tests/currentscript.mk's comment
# explains in full. This is another rule in the tree that has to obey it.
$(BUILD)/noslice/js_dom.o: c/apps/browser/js_dom.c
	@mkdir -p $(dir $@)
	$(CC) $(BROWSER_JS_CF) -DJS_DOM_NO_SLICE_BRACKET -c $< -o $@

NOSLICE_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_dom.o,$(BROWSER_JS_OBJ)) \
                  $(BUILD)/noslice/js_dom.o

$(BUILD)/browser-noslice.elf: $(ENGINE_OBJ) $(NOSLICE_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NOSLICE_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-noslice.aex: $(BUILD)/browser-noslice.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-noslice.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

test-jsslice-negctl: $(ISO) $(BUILD)/browser-noslice.aex
	@$(MAKE) DISK=$(BUILD)/disk-noslice.img BROWSER_AEX=$(BUILD)/browser-noslice.aex $(BUILD)/disk-noslice.img
	@python3 tests/qmp/qmp_jsslice.py --iso $(ISO) --disk $(BUILD)/disk-noslice.img \
	    --label noslice --out $(BUILD)/jsslice-negctl.json | tee $(BUILD)/jsslice-negctl.log
	@grep -q '"verdict": "BUG-PRESENT"' $(BUILD)/jsslice-negctl.log || \
	    { echo "FAIL: the noslice build did not reproduce the known bug shape -- read $(BUILD)/jsslice-negctl.json"; exit 1; }
	@echo "negctl red as expected: bug shape reproduced against -DJS_DOM_NO_SLICE_BRACKET"

# Profile one captured site fixture. An instrument, not a test: it asserts
# nothing and exists so "where did the slice go" has a command.
probe-jsprof: $(JSPROF_PROBE)
	@$(JSPROF_PROBE) --prof $(WEBAPI_FIXTURES) 2>&1 | grep -a jsprof

# --- the scaling table: COST PLOTTED AGAINST INPUT SIZE ---------------------
# The one thing a total cannot show. A quadratic operation and a linear one are
# indistinguishable at one input size and differ by three orders of magnitude
# at a thousand, so the only way to see it is to run the same operation at 1,
# 10, 100, 1000 and 4000 and divide.
#
# test-domscale IS A TEST rather than an instrument, and it is a test of ONE
# thing: its own control. Row 0 is a pure-JS loop that never touches the DOM
# and must stay flat as the document grows around it; if it rises, the rise is
# GC pressure or the allocator and every other row is contaminated. That is the
# assertion. It deliberately does NOT assert that any particular row is
# quadratic -- a gate that fails when somebody FIXES querySelector is a gate
# that punishes the work it exists to order. The shapes are printed; the
# control is enforced.
#
# It is not on ci-host: at 3 repeats it takes about eight minutes, which is
# most of a host suite. Run it when the DOM or the selector engine changes.
test-domscale: $(JSPROF_PROBE)
	@$(JSPROF_PROBE) --domscale $(DOMSCALE_REPS)
DOMSCALE_REPS ?= 3

# --- the guest half of the same question ------------------------------------
# The host binary is arm64/darwin and does not link layout.c; this runs the
# same seven operations in browser.aex, x86_64 under TCG, with the layout
# engine present. See the file header for why the row list is deliberately
# smaller and hand-copied rather than shared, and for what that costs.
#
# Manual, not ci-boot, for the same reason test-jsslice is manual: it boots
# QEMU and spends minutes inside it.
test-domscale-os: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_jsdomscale.py --iso $(ISO) --disk $(DISK) \
	    --out $(BUILD)/domscale-guest.json | tee $(BUILD)/domscale-guest.log
	@grep -qE '"verdict": "(CONFIRMED|PARTIAL)"' $(BUILD)/domscale-guest.log || \
	    { echo "FAIL: the device scaling run produced no table -- read $(BUILD)/domscale-guest.json"; exit 1; }

# --- probe-jsctl: the CONTROLS, and they are the reason to believe the rest --
# A profile in which everything is expensive is a profile of the instrument.
# tests/fixtures/jsctl/ holds two pages whose behaviour is known from the site
# scoreboard -- control-example (19/19/19 text runs, no scripts at all) and
# control-wikipedia (50 -> 207 painted runs on this engine) -- and three built
# from the committed real-site bundles in tests/fixtures/jsperf/. The controls
# must come back CHEAP and nowhere near the slice; if they do not, no number
# taken from a real page means anything.
#
# THE STATED LIMIT: these are captured DOCUMENTS, so a <script src> pointing at
# the live site is not in the corpus and does not run. The probe prints each
# one as "NOT IN FIXTURE" and control-wikipedia has one. So this measures the
# inline execution of those pages, which is a floor on their cost and not their
# whole cost.
JSCTL_FIXTURES := $(sort $(dir $(wildcard tests/fixtures/jsctl/*/index.html)))
probe-jsctl: $(JSPROF_PROBE)
	@$(JSPROF_PROBE) --prof $(JSCTL_FIXTURES) 2>&1 | grep -aE 'jsprof|scripts \(|NOT IN FIXTURE'

ci-host: test-jsprof

.PHONY: test-jsprof test-jsslice test-jsslice-negctl probe-jsprof test-domscale test-domscale-os probe-jsctl
