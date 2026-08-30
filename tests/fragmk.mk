# tests/fragmk.mk -- the framework corpus, mounted IN THE GUEST.
#
# In its own fragment for the reason every other tests/*.mk here gives: several
# agents edit the top-level Makefile at once, and a fragment is the only way to
# add targets without a commit sweeping up whoever else's half-finished work
# happens to be in that file. The `-include tests/fragmk.mk` line already lives
# in the Makefile (pre-wired by 1461cd173 before this agent existed).
#
#   make test-fragmk-guest        the gate: >= 5 of 7 framework apps mounted
#                                 in the shipped browser, on the machine
#   make test-fragmk-guest-negctl the control alone (modules silenced)
#
# WHY A SECOND INSTRUMENT WHEN tests/frameworks.mk EXISTS
# frameworks.mk measures the corpus with tests/unit/webapi_probe.c -- a HOST
# binary. It is a good instrument for ranking causes (it links every js_*.c TU
# and reads the committed bundles offline), and its own header says what it is:
# the probe evaluates each script ITSELF. That makes it a different embedder
# than browser.c: <script type=module> discovery in the HTML parser, the script
# fetch path, the module loader, the microtask drain -- the channel between the
# embedder and the runtime is exactly what a host probe cannot measure, and it
# is the channel document.currentScript spent months green in while the shipped
# browser returned null on every page (tests/currentscript.mk tells that story
# in full; this gate exists so the framework corpus never has to learn it the
# same way).
#
# WHAT THE GUEST RUN MEASURED (2026-08-30, first full run of this driver):
#   7 of 7 apps mounted non-blank -- react, vite, svelte, vue, webpack, next
#   AND angular; every one painted its button ("count is 0") and loaded its
#   lazy route/chunk over real HTTP. The only exception on the whole corpus is
#   angular's two console lines, diagnosed as an ENGINE defect, not a page one:
#   our unhandled-rejection reporter fires at the rejection INSTANT (QuickJS's
#   host-tracker contract) instead of at the microtask-checkpoint drain, so a
#   promise whose handler attaches one job later -- the exact shape of every
#   `async function f(){ return g(); }` where g() rejects in its resume job --
#   is reported AND dispatched as window.unhandledrejection, which zoneless
#   Angular's ErrorHandler then logs. Chrome logs nothing on the same bytes
#   (V8 defers the decision to the checkpoint). The fix belongs in
#   js_platform.c + js_dom.c; this fragment records the number it moves.
#
# THE BAR IS 5, NOT 7, ON PURPOSE. 7 is the measured state and 5 is the
# wave-1 mission bar (">=5/7 non-blank DOM paint"); the gate asserts the bar so
# a single-app regression is a REPORTED drop rather than a red gate that trains
# people to ignore red. The number above -- not the assertion -- is what says 7.
#
# THE CONTROL IS THE PRE-MODULE ERA ON A SWITCH: js_module.c compiled with
# -DFRMW_NEGCTL_NOMODULE turns js_module_eval into a no-op, which is the state
# this file replaced (qmp_module_page.py: modules evaluated as classic scripts,
# `import` at byte 0 a SyntaxError, zero JS on module-shipped sites). Five of
# the seven apps ship as modules and mount NOTHING in that build; webpack
# (defer) and next (classic async) still mount, so the control reads 2 of 7
# against the bar of 5 -- below the bar rather than at zero, because a gate
# that can only fail to zero also passes any partial-silence defect. (The
# first cut of the driver shipped its DOM reporter as a module and the control
# read 0 of 7 -- indistinguishable from a broken driver; the reporter is now a
# classic inline script for that reason. Watched red both ways, 2026-08-30.)
# The control is a PREREQUISITE of the positive target, not a name on a
# ci- line, for the reason
# tests/currentscript.mk states: audit_tests.py drops test-*-negctl from what
# ci.sh runs, and a control named-but-never-run looks fixed while being worse
# than stranded.
#
# NOT on ci-boot:. One whole QEMU boot per side (plus a second disk image for
# the control). Run it across a change to js_module.c / js_dom.c / js_page.c /
# html_tree.c -- or before believing any framework-corpus number from the host
# probe alone.
.PHONY: test-fragmk-guest test-fragmk-guest-negctl

FRMW_GUEST := tests/fixtures/frameworks/_guest/guest_mount.py
FRMW_BAR   := 5

test-fragmk-guest: test-fragmk-guest-negctl
test-fragmk-guest: $(ISO) $(DISK)
	python3 $(FRMW_GUEST) $(ISO) $(DISK) --min $(FRMW_BAR)

# $(BROWSER_JS_CF), not $(JS_CF): the root Makefile's one target-specific
# variable override strips `-include features.h` from the browser's own TUs,
# and a rule here that compiled a c/apps/browser source with raw $(JS_CF) would
# revive the `hidden` macro that made sizeof(struct item) disagree by 8 bytes
# across the display list. The root Makefile says so at $(BROWSER_JS_OBJ);
# this is the third rule in the tree that has to obey it (currentscript.mk's is
# the second and explains the history).
$(BUILD)/negmod/js_module.o: c/apps/browser/js_module.c
	@mkdir -p $(dir $@)
	$(CC) $(BROWSER_JS_CF) -DFRMW_NEGCTL_NOMODULE -c $< -o $@

FRMW_NEG_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_module.o,$(BROWSER_JS_OBJ)) \
                $(BUILD)/negmod/js_module.o

$(BUILD)/browser-negmod.elf: $(ENGINE_OBJ) $(FRMW_NEG_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(FRMW_NEG_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-negmod.aex: $(BUILD)/browser-negmod.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-negmod.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

# The verdict line, not the exit code, is what this reads: the driver exits 1
# both for "below the bar" (the designed red) and for its own die() paths, and
# a control that cannot tell those apart passes when the harness is broken.
test-fragmk-guest-negctl: $(ISO) $(BUILD)/browser-negmod.aex
	@$(MAKE) DISK=$(BUILD)/disk-negmod.img BROWSER_AEX=$(BUILD)/browser-negmod.aex $(BUILD)/disk-negmod.img
	@out=`python3 $(FRMW_GUEST) $(ISO) $(BUILD)/disk-negmod.img --settle 15 --min $(FRMW_BAR)`; \
	echo "$$out"; \
	n=`echo "$$out" | sed -n 's/.*-- \([0-9][0-9]*\) of [0-9][0-9]* settled non-blank.*/\1/p' | tail -1`; \
	if [ -z "$$n" ]; then \
	  echo "test-fragmk-guest-negctl: FAILED -- the driver produced no verdict line"; exit 1; \
	fi; \
	if [ "$$n" -ge $(FRMW_BAR) ]; then \
	  echo "test-fragmk-guest-negctl: FAILED -- the silenced-module build met the bar ($$n >= $(FRMW_BAR)); this control is measuring something other than module execution"; exit 1; \
	fi; \
	echo "test-fragmk-guest-negctl: red as designed -- $$n of 7 mounted with modules silenced, below the bar of $(FRMW_BAR)"
