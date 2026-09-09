# --- test-currentscript: document.currentScript, ON THE MACHINE -------------
#
# In its own fragment for the reason every other tests/*.mk here gives: several
# lines edit the top-level Makefile at once, and a fragment is the only way to
# add targets without a commit sweeping up somebody else's half-finished work.
#
#   make test-currentscript          the gate (boots QEMU; runs its own control)
#   make test-currentscript-negctl   the control alone
#
# WHY IT IS A BOOT HARNESS AND NOT A HOST GATE, and this is the whole point of
# the target rather than an implementation detail.
#
# document.currentScript had a host instrument -- tests/unit/webapi_probe.c --
# and it measured the feature green while the shipped browser returned NULL for
# every inline classic script on every page, for the whole life of the feature.
# The probe evaluates each script itself, so it called js_page_begin_script
# itself, and that function took a FILENAME and recovered the <script> node by
# matching the string. The probe passed a string chosen so the match would
# succeed. browser.c passed the page URL + "#inline-script-N" -- which an inline
# script needs as the base its `import()` resolves against -- and the inline
# test was `!strchr(filename, ':')`, so it matched nothing, ever.
#
# NO HOST TEST CAN ASK THIS QUESTION. The thing under test is the channel
# between the embedder and the runtime, and a host test IS a different embedder.
# The only way to see it was to put the question inside the guest with
# browser.c doing the calling, which is what tests/qmp/qmp_currentscript.py
# does: five <script> elements on one fixture page, every answer read off the
# guest's own serial log.
#
# THE CONTROL IS THE DEFECT ON A SWITCH. js_page.c compiled with
# -DJS_CURRENTSCRIPT_NOTOLD drops the node instead of recording it, which is
# exactly the state the browser shipped in, and the same driver with
# --expect-null requires every positive assertion to read NULL -- and ALSO
# requires the page to still render, so a control that passes because the
# browser died is refused. It is a PREREQUISITE of the positive target rather
# than a third name on a ci- line, because audit_tests.py's NOT_CI drops every
# `test-*-negctl` from what ci.sh runs: naming it there satisfies the audit and
# runs it never, which is worse than leaving it stranded because it looks
# fixed. (tests/logreporter.mk and tests/license.mk are the worked examples.)
#
# NOT on ci-boot:. One whole QEMU boot per side, two boots per run, and the
# control's disk image is a second full mkfs. Run it across a change to
# js_page.c / browser.c / js_platform.c.
.PHONY: test-currentscript test-currentscript-negctl

test-currentscript: test-currentscript-negctl
test-currentscript: $(ISO) $(DISK)
	python3 tests/qmp/qmp_currentscript.py $(ISO) $(DISK)

# $(BROWSER_JS_CF), not $(JS_CF): the root Makefile's ONE target-specific
# variable override strips `-include features.h` from the browser's own TUs,
# and a rule here that compiled a c/apps/browser source with raw $(JS_CF) would
# revive the `hidden` macro that made sizeof(struct item) disagree by 8 bytes
# across the display list. The root Makefile says so at $(BROWSER_JS_OBJ); this
# is the second rule in the tree that has to obey it.
$(BUILD)/nocs/js_page.o: c/apps/browser/js_page.c
	@mkdir -p $(dir $@)
	$(CC) $(BROWSER_JS_CF) -DJS_CURRENTSCRIPT_NOTOLD -c $< -o $@

NOCS_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/js_page.o,$(BROWSER_JS_OBJ)) \
               $(BUILD)/nocs/js_page.o

$(BUILD)/browser-nocs.elf: $(ENGINE_OBJ) $(NOCS_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NOCS_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o --end-group

$(BUILD)/browser-nocs.aex: $(BUILD)/browser-nocs.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-nocs.elf $@ Browser - 'B' 120 130 240 --stack-pages 2048

test-currentscript-negctl: $(ISO) $(BUILD)/browser-nocs.aex
	@$(MAKE) DISK=$(BUILD)/disk-nocs.img BROWSER_AEX=$(BUILD)/browser-nocs.aex $(BUILD)/disk-nocs.img
	python3 tests/qmp/qmp_currentscript.py $(ISO) $(BUILD)/disk-nocs.img --expect-null
