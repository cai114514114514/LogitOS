# --- test-docwrite: document.write/writeln, ON THE MACHINE -------------------
#
# In its own fragment for the reason every other tests/*.mk here gives: several
# agents edit the top-level Makefile at once, and a fragment is the only way to
# add targets without a commit sweeping up somebody else's half-finished work.
# The -include line is already in the Makefile (pre-wired at HEAD); this file
# is the content it promised.
#
#   make test-docwrite                   the gate (boots QEMU twice: control, then positive)
#   make test-docwrite-negctl            the compiled-out control alone
#   make test-docwrite-afterload-red     the one-shot falsifier (see below)
#
# WHAT THE GATE PROVES, and why it is a BOOT harness and not a host gate:
# document.write's whole observable is WHERE the written markup lands in the
# document, that it reaches layout and paint like source markup, that a
# <script> IN the written markup executes (written input goes through the
# MAIN parser in a real browser -- the fragment algorithm an innerHTML-based
# parse uses would leave it inert, so js_platform.c's installer rebuilds
# written scripts fresh before insertion), and that a write after load
# changes NOTHING. All four are guest questions -- a host probe would have
# to invent its own script-execution order and its own document, and
# tests/currentscript.mk records exactly how that instrument once measured a
# feature green that the shipped browser could not produce.
# tests/qmp/qmp_docwrite.py loads tests/fixtures/docwrite/docwrite.html over
# the network, reads the DOM order from the page's own serial markers, and
# the block order + colours from a screendump.
#
# WATCHED RED, all three halves, before any of them was allowed to go green
# (rule 5 -- a green test that has never been shown to fail is not evidence).
# All re-watched 2026-08-30 (post-resume session) against the CURRENT fixture
# with the revived-written-script shape, driver exit 1 each time:
#
#   mid-parse writes, against the PRE-FEATURE build (first recorded by the
#   first docwrite agent, re-watched against the ndw disk): the driver died
#     FAIL: script 1 ran to completion -- its document.write calls returned
#   with the serial showing
#     [browser] JS exception: TypeError: write is not a function (it is undefined)
#   and "DW-DOM-ORDER before,after,last" -- every write threw and killed its
#   script.
#
#   the written-<script> revival, against a build with revive(scratch)
#   locally removed (the fragment stamp left in place -- writes work, only
#   written scripts go inert): the first FOUR assertions pass (script 1
#   completes, DOM order, w1 read-back, writeln newline) and the run dies at
#     FAIL: a <script> WRITTEN by document.write EXECUTED (main-parser
#     semantics -- a fragment parse alone would leave it inert)
#   -- which is what proves the gate measures the revival specifically and
#   not the write machinery around it. This control has no shipped target:
#   it was a local one-shot removal, watched once, restored, and re-watched
#   green; a permanent -D build would be a third boot per CI run for
#   evidence already in this log.
#
#   the after-load refusal, against a build with the gate open
#   (JS_DOCWRITE_AFTERLOAD, the tempting append-at-body-end implementation,
#   target below): everything passes through the timer firing, then
#     FAIL: the DOM order after the after-load write is the pre-drain order
#     PLUS w4 ... and NOTHING else -- #late never entered the tree
#   with the page's own line reading
#     DW-DOM-ORDER2 fromhead,before,w1,w2,w4,after,w3,last,late
#   (the ",late" suffix is the refusal failing). That variant is NOT on
#   ci-boot -- it exists to falsify the refusal assertion, its redness is
#   recorded, and two extra QEMU boots per CI run would buy a re-run of
#   evidence already in the log.
#
# THE CONTROL AS PREREQUISITE, not a name on a ci- line: audit_tests.py's
# NOT_CI drops every test-*-negctl from what tools/ci.sh runs, so naming it
# beside the positive would satisfy the audit and run it never
# (tests/coredump.mk and the Makefile's own test-sigint say this at length).
# test-docwrite-negctl compiles js_platform.c with -DJS_DOCWRITE_NO_INSTALL,
# which reproduces the pre-feature build exactly (document.write absent,
# pages still render), and runs the same driver with --expect-off: order line
# "before,after,last", w1 reads null, no written blocks on screen.
#
# On ci-boot:. One boot per side, two per run. Run it across a change to
# js_platform.c / js_page.c / js_dom.c / browser.c.
.PHONY: test-docwrite test-docwrite-negctl test-docwrite-afterload-red

test-docwrite: test-docwrite-negctl
test-docwrite: $(ISO) $(DISK)
	python3 tests/qmp/qmp_docwrite.py $(ISO) $(DISK)

ci-boot: test-docwrite

# $(BROWSER_JS_CF), not $(JS_CF): the root Makefile's ONE target-specific
# variable override strips `-include features.h` from the browser's own TUs,
# and a rule here that compiled a c/apps/browser source with raw $(JS_CF)
# would revive the `hidden` macro that made sizeof(struct item) disagree by 8
# bytes across the display list. tests/currentscript.mk states this for
# js_page.o; this rule obeys it for js_platform.o.
$(BUILD)/ndw/c/apps/browser/js_platform.o: c/apps/browser/js_platform.c
	@mkdir -p $(dir $@)
	$(CC) $(BROWSER_JS_CF) -DJS_DOCWRITE_NO_INSTALL -c $< -o $@

NDW_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_platform.o,$(BROWSER_JS_OBJ)) \
              $(BUILD)/ndw/c/apps/browser/js_platform.o

$(BUILD)/browser-ndw.elf: $(ENGINE_OBJ) $(NDW_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NDW_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-ndw.aex: $(BUILD)/browser-ndw.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-ndw.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

test-docwrite-negctl: $(ISO) $(BUILD)/browser-ndw.aex
	@$(MAKE) DISK=$(BUILD)/disk-ndw.img BROWSER_AEX=$(BUILD)/browser-ndw.aex $(BUILD)/disk-ndw.img
	python3 tests/qmp/qmp_docwrite.py $(ISO) $(BUILD)/disk-ndw.img --expect-off

# The one-shot falsifier for the after-load refusal: js_platform.c compiled
# with -DJS_DOCWRITE_AFTERLOAD makes a write with no currentScript APPEND AT
# BODY END instead of contributing nothing -- the exact wrong implementation
# the gate's DW-DOM-ORDER2/no-orange assertions exist to catch. Run it once
# across a change to the refusal, watch it go red, record it; it is not on
# ci-boot (two boots per run for already-recorded evidence).
$(BUILD)/adw/c/apps/browser/js_platform.o: c/apps/browser/js_platform.c
	@mkdir -p $(dir $@)
	$(CC) $(BROWSER_JS_CF) -DJS_DOCWRITE_AFTERLOAD -c $< -o $@

ADW_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_platform.o,$(BROWSER_JS_OBJ)) \
              $(BUILD)/adw/c/apps/browser/js_platform.o

$(BUILD)/browser-adw.elf: $(ENGINE_OBJ) $(ADW_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(ADW_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-adw.aex: $(BUILD)/browser-adw.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-adw.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

test-docwrite-afterload-red: $(ISO) $(BUILD)/browser-adw.aex
	@$(MAKE) DISK=$(BUILD)/disk-adw.img BROWSER_AEX=$(BUILD)/browser-adw.aex $(BUILD)/disk-adw.img
	python3 tests/qmp/qmp_docwrite.py $(ISO) $(BUILD)/disk-adw.img
