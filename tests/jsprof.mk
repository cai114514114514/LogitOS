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
# a rule here that compiled a c/apps/gui/browser source with raw $(JS_CF) would
# revive the `hidden` macro mismatch tests/currentscript.mk's comment
# explains in full. This is another rule in the tree that has to obey it.
$(BUILD)/noslice/js_dom.o: c/apps/gui/browser/js/dom/dom.c
	@mkdir -p $(dir $@)
	$(CC) $(BROWSER_JS_CF) -DJS_DOM_NO_SLICE_BRACKET -c $< -o $@

NOSLICE_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/gui/browser/js/dom/dom.o,$(BROWSER_JS_OBJ)) \
                  $(BUILD)/noslice/js_dom.o

$(BUILD)/browser-noslice.elf: $(ENGINE_OBJ) $(NOSLICE_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NOSLICE_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-noslice.aex: $(BUILD)/browser-noslice.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-noslice.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048


# Profile one captured site fixture. An instrument, not a test: it asserts
# nothing and exists so "where did the slice go" has a command.
probe-jsprof: $(JSPROF_PROBE)
	@$(JSPROF_PROBE) --prof $(WEBAPI_FIXTURES) 2>&1 | grep -a jsprof

DOMSCALE_REPS ?= 3


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

.PHONY: test-jsprof probe-jsprof probe-jsctl
