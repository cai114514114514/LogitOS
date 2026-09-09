# --- test-jsmodpf: module-graph prefetch, ON THE MACHINE --------------------
#
# In its own fragment for the reason every other tests/*.mk here gives:
# several agents edit the top-level Makefile at once, and a fragment is the
# only way to add targets without a commit sweeping up somebody else's
# half-finished work.
#
#   make test-jsmodpf          the gate (boots QEMU; runs its own control)
#   make test-jsmodpf-negctl   the control alone
#
# WHAT THIS PROVES, and why it has to be a boot harness. js_module.c's
# mod_compile_and_prefetch() turns a module's own static import list into one
# concurrent wave of bfetch_prefetch() calls instead of a chain of blocking
# bfetch_sync() round trips -- see that file's own comment for the mechanism
# (it needed one addition to third_party/quickjs/quickjs.c,
# JS_EVAL_FLAG_COMPILE_NO_RESOLVE, because QuickJS's own JS_Eval has no seam
# to stop it resolving a module's children before returning). Whether that
# actually overlaps requests on the wire is not something a host unit test can
# see: the thing under test is real sockets, a real connection pool
# (hpool_config's 6-total/2-per-origin caps) and real network latency, and
# QuickJS's module loader is only ever driven for real inside browser.aex.
#
# THE FIXTURE: a root module statically importing 18 leaf modules split across
# THREE local HTTP origins (three ports), 6 leaves per origin -- lined up with
# hpool's 2-per-origin cap so a full prefetch wave uses the whole 6-connection
# pool at once. Every fixture response sleeps 200ms on the HOST before
# answering (tests/qmp/qmp_modprefetch.py), which is what makes 18 sequential
# round trips (>= 3.6s) and 3 concurrent waves (~0.6-1.5s) far enough apart to
# tell apart under TCG's own jitter.
#
# THE NUMBER IS READ OFF THE GUEST'S OWN CLOCK, per AGENTS.md's "never measure
# wall clock on the host": browser_rt.c already stamps monotonic_ms() around
# exactly this window ("[wa] ... nav" when the navigation is armed, "[wa] ...
# loadend ..." once the whole load -- module graph included -- has finished),
# and the driver's only measurement is the guest-clock difference between
# those two lines. The 200ms delay lives on the host because something has to
# make a round trip slow; the DURATION under test does not.
#
# THE CONTROL IS THE PRE-PREFETCH CODE PATH ON A SWITCH: js_module.c compiled
# with -DJS_MODULE_NO_PREFETCH takes mod_compile_and_prefetch() back to one
# JS_Eval call with QuickJS's own unconditional resolve -- byte-identical to
# this file before 2026-09-02. The same driver against that build, passed
# --expect-slow, requires the guest-clock elapsed time to be ABOVE the bound
# the prefetch build must stay below; if both builds print the same number,
# prefetch is not happening. It is a PREREQUISITE of the positive target
# rather than a name on a ci- line, for the reason tests/currentscript.mk
# states: audit_tests.py's NOT_CI drops every test-*-negctl from what ci.sh
# runs, and a control named-but-never-run looks fixed while being worse than
# stranded.
#
# NOT on ci-boot:. One whole QEMU boot per side, two boots per run, and the
# control's disk image is a second full mkfs. Run it across a change to
# js_module.c, browser_rt.c's bfetch_prefetch*/res_fetch, or
# third_party/quickjs/quickjs.c's module resolution.
.PHONY: test-jsmodpf test-jsmodpf-negctl

test-jsmodpf: test-jsmodpf-negctl
test-jsmodpf: $(ISO) $(DISK)
	python3 tests/qmp/qmp_modprefetch.py $(ISO) $(DISK)

# $(BROWSER_JS_CF), not $(JS_CF): the root Makefile's ONE target-specific
# variable override strips `-include features.h` from the browser's own TUs;
# a rule here that compiled a c/apps/browser source with raw $(JS_CF) would
# revive the `hidden` macro mismatch tests/currentscript.mk's comment
# explains in full. This is the third rule in the tree that has to obey it.
$(BUILD)/nomodpf/js_module.o: c/apps/browser/js_module.c
	@mkdir -p $(dir $@)
	$(CC) $(BROWSER_JS_CF) -DJS_MODULE_NO_PREFETCH -c $< -o $@

NOMODPF_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_module.o,$(BROWSER_JS_OBJ)) \
                  $(BUILD)/nomodpf/js_module.o

$(BUILD)/browser-nomodpf.elf: $(ENGINE_OBJ) $(NOMODPF_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NOMODPF_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-nomodpf.aex: $(BUILD)/browser-nomodpf.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-nomodpf.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

test-jsmodpf-negctl: $(ISO) $(BUILD)/browser-nomodpf.aex
	@$(MAKE) DISK=$(BUILD)/disk-nomodpf.img BROWSER_AEX=$(BUILD)/browser-nomodpf.aex $(BUILD)/disk-nomodpf.img
	python3 tests/qmp/qmp_modprefetch.py $(ISO) $(BUILD)/disk-nomodpf.img --expect-slow
