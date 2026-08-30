# --- test-crashfix: the site-JS crash cluster, fixed at the general defect ----
#
# In its own fragment for the reason every other tests/*.mk here gives: several
# agents edit the top-level Makefile at once and a fragment is the only way to
# add targets without a commit sweeping up somebody else's half-finished work.
# This one is owned by the crashfix wave-1 agent (2026-08-30).
#
#   make test-crashfix          the guest fixture gate (boots QEMU)
#   make test-crashfix-negctl   the same driver against a browser built with
#                               the type gate compiled out -- the defect on a
#                               switch -- required to go red for the RIGHT
#                               reasons. A PREREQUISITE of the positive gate,
#                               not a name on a ci- line (that satisfies the
#                               audit and runs it never; see currentscript.mk).
#   make test-crashfix-caller   host-side: Function.prototype.caller against
#                               the CALLER-BASELINE ratchet (see below)
#
# WHAT THE GATE GATES, AND WHERE THE DEFECTS ACTUALLY WERE
#
# Four site specimens, four first symptoms. Diagnosis first, then the fix list,
# because two of the four were already fixed by the roadmap arc (9e96e8035)
# and are verified here rather than fixed here:
#
#   google-search "postMessage target-origin SyntaxError" -- Web IDL overload
#       resolution: an object second argument is WindowPostMessageOptions,
#       not a USVString "[object Object]". Fixed in the arc (9e96e8035,
#       js_platform.c's postMessage prelude) -- the first draft of this
#       comment said "by that file's owner", which was wrong: js_platform.c's
#       only uncommitted edits are docwrite's, and none touch postMessage.
#       Guest before/after: 2 exceptions (scoreboard wtype-live1, commit
#       9ba575e93) -> 0 postMessage-shaped exceptions in this gate's replay.
#   kimi           "caller of undefined" -- Function.prototype.caller returns
#       undefined for a non-strict function in stock QuickJS where browsers
#       return the calling function. NOT FIXED: third_party/quickjs is not
#       this agent's file; the precise patch is in the wave report and the
#       gap is ratcheted by test-crashfix-caller so landing it flips a gate
#       rather than going unnoticed.
#   douyin         "getAttribute of null" -- document.currentScript was null
#       for every dynamically inserted script (js_page_eval was handed a
#       filename, not the node). Fixed in the arc (js_page_eval takes the
#       node). Verified by the secsdk-shape specimen replay, before/after.
#   stripe + douyin "SyntaxError: expecting ';'" / "'%'" at <page-url>:1 --
#       THE ONE THIS FRAGMENT GATES. Dynamically inserted data-block <script>s
#       (ld+json, %-templates) executed because the insertion path applied no
#       type whitelist and no fragment "already started" flag. Fixed in
#       js_dom.c (offer_scripts) + js_dom.c/js_dom_iface.inc (fragment scripts
#       stamped, HTMLScriptElement.text). The fixture page
#       tests/fixtures/crashfix/inserted-datablock.html reduces the defect
#       with no site in it, and asserts the controls as hard as the fix: an
#       EXECUTABLE inserted script must still run, or the gate has bought
#       silence by breaking the feature ("absent beats present-and-wrong"
#       cuts both ways).
#
# NOT on ci-boot lines: two QEMU boots per full pass (positive + control) and
# a second mkfs for the control disk. Run it across a change to js_dom.c,
# js_dom_iface.inc, js_module.c or browser.c's script drain.

# --- the guest gate ---------------------------------------------------------
.PHONY: test-crashfix test-crashfix-negctl test-crashfix-caller

# caller ratchet FIRST: it is host-side and instant once built, so a red
# ratchet answers before either QEMU boot is paid for.
test-crashfix: test-crashfix-caller test-crashfix-negctl
test-crashfix: $(ISO) $(DISK)
	python3 tests/qmp/qmp_crashfix_page.py --iso $(ISO) --disk $(DISK) --out $(BUILD)/crashfix.json

# THE CONTROL BUILD: js_dom.c compiled with the type gate switched off, which
# is byte-for-byte the behaviour the defect shipped with. $(BROWSER_JS_CF),
# not raw $(JS_CF), for the reason currentscript.mk states: the root Makefile's
# one target-specific variable override exists so a fragment rule cannot
# revive the `hidden` macro disagreement that cost this tree a day.
$(BUILD)/notold/js_dom.o: c/apps/browser/js_dom.c
	@mkdir -p $(dir $@)
	$(CC) $(BROWSER_JS_CF) -DCRASHFIX_DATABLOCK_NOTOLD -c $< -o $@

NOTOLD_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_dom.o,$(BROWSER_JS_OBJ)) \
                 $(BUILD)/notold/js_dom.o

$(BUILD)/browser-notold.elf: $(ENGINE_OBJ) $(NOTOLD_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group \
	    $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NOTOLD_JS_OBJ) $(BROWSER_OBJ) \
	    $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-notold.aex: $(BUILD)/browser-notold.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-notold.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

test-crashfix-negctl: $(ISO) $(BUILD)/browser-notold.aex
	@$(MAKE) DISK=$(BUILD)/disk-notold.img BROWSER_AEX=$(BUILD)/browser-notold.aex $(BUILD)/disk-notold.img
	python3 tests/qmp/qmp_crashfix_page.py --iso $(ISO) --disk $(BUILD)/disk-notold.img \
	    --expect-broken --only fixture --out $(BUILD)/crashfix-negctl.json

# --- the caller ratchet (host, no QEMU) --------------------------------------
#
# Function.prototype.caller is the kimi specimen's root cause and the fix is
# NOT here (third_party/quickjs). What IS here is the ratchet: the vendored
# engine's actual behaviour, asserted against CALLER-BASELINE. The baseline's
# current entry is the known gap; when the quickjs patch lands, the assertion
# goes red until the baseline is updated, so the patch cannot land invisible --
# and if a quickjs upgrade ever silently changes .caller semantics, this goes
# red too, in the other direction.
$(BUILD)/crashfix_caller_probe: tests/fixtures/crashfix/caller_probe.c $(QJS_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -w $(JS_INC) -DCONFIG_VERSION='"host"' -o $@ \
	    tests/fixtures/crashfix/caller_probe.c $(QJS_SRC) -lm

test-crashfix-caller: $(BUILD)/crashfix_caller_probe
	@$(BUILD)/crashfix_caller_probe tests/fixtures/crashfix/CALLER-BASELINE
