# tests/forms.mk -- the focus model and the form controls.
#
# Own fragment rather than lines in the Makefile, for the reason every other
# fragment in this directory gives: concurrent sessions overwrite that file
# wholesale, and a test target is not worth losing someone else's work over.
#
# THREE TARGETS AND THEY MEASURE DIFFERENT THINGS. Read this before trusting
# any one of them on its own:
#
#   test-forms          host. The state machine: typing, the caret, the dirty
#                       value flag, Tab order, the submission payload, and the
#                       EVENT ORDER those produce. Fast, exhaustive, and unable
#                       to prove that a human can type into anything.
#
#   test-forms-device   QEMU. Types into a real text field on a real page over
#                       QMP and reads the characters back out of a SCREENDUMP.
#                       This is the one that measures the bug that was actually
#                       reported -- "no web page on this machine can accept a
#                       keystroke" is a device-level statement and a host test
#                       cannot contradict it.
#
#   test-forms-negctl   THE NEGATIVE CONTROL, and it is free, because the
#                       control is the behaviour that shipped yesterday: a
#                       browser built with -DBROWSER_NO_FOCUS routes keys to
#                       <body> exactly as it did before this change. The device
#                       test must FAIL against it. If it passes, the device test
#                       is not measuring the focus model.
#
# THERE IS NO test-wpt-forms, AND THERE MUST NOT BE. This fragment used to
# build a second WPT runner with forms.c/focus.c/js_forms.c added, on the
# premise that tests/wpt.mk's runner did not link them. It does: WPT_JS_SRC is
# a wildcard over c/apps/browser/js_*.c and WPT_TEST_SRC names forms.c and
# focus.c outright. So the second runner measured the same browser twice and
# could not even link -- tests/unit/wpt_test.c already defines text_measure,
# which is the one symbol the deleted shim supplied.
#
# The measurement is therefore the shipping one, which is also what the brief
# asks for (both sides of an A/B pinned to the same binary):
#
#     make wpt ONLY=html/semantics/forms
#
# Measured 2026-08-08: 569/4258 subtests (13.4%) over 414 harness files.
#
# Note for whoever runs it next: that runner's wildcard sweeps up UNTRACKED
# js_*.c from other lines, and a half-written one fails the build for reasons
# that have nothing to do with what is being measured. Pin it to the committed
# set when that happens:
#
#     make wpt ONLY=... WPT_JS_SRC="$(git ls-files c/apps/browser/js_*.c)"

.PHONY: test-forms test-forms-asan test-forms-device test-forms-negctl

FORMS_SRC := tests/unit/forms_test.c \
             c/apps/browser/forms.c c/apps/browser/focus.c \
             c/apps/browser/layout.c c/apps/browser/css_engine.c \
             c/apps/browser/css_vars.c $(HTML_PARSER_SRC)

test-forms: $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/forms_test \
	    $(FORMS_SRC) $(BUILD)/libcss_host.a
	@$(BUILD)/forms_test

# ASan/UBSan over the same test. forms.c allocates per control and frees the
# lot on navigation, and a use-after-free of a control's value would otherwise
# only show as the wrong text on screen.
test-forms-asan: $(BUILD)/libcss_host.a
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(BTEST_INC) $(CSS_INC) -o $(BUILD)/forms_test_asan \
	    $(FORMS_SRC) $(BUILD)/libcss_host.a
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/forms_test_asan

# --- the device test --------------------------------------------------------
test-forms-device: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_forms.py $(ISO) $(DISK)

# --- the negative control ---------------------------------------------------
# A browser.aex built with the focus routing compiled OUT: keys go to <body>,
# which is precisely what the tree did before this change. Packed onto its own
# disk image (BROWSER_AEX is already overridable for exactly this -- see the
# note above BROWSER_AEX in the Makefile) and driven by the same harness, which
# must then FAIL.
#
# Same shape as test-webapi-page-control: one object rebuilt, everything else
# reused. browser.o is compiled with -DBROWSER_NO_FOCUS (the routing compiled
# out, keys back to <body>) and js_forms.o is dropped, so the control is exactly
# "the tree as it was" and not an approximation of it.
NOFOCUS_JS_OBJ := $(filter-out $(BUILD)/jsobj/c/apps/browser/browser.o \
                               $(BUILD)/jsobj/c/apps/browser/js_forms.o, \
                               $(BROWSER_JS_OBJ)) \
                  $(BUILD)/nofocus/browser.o

$(BUILD)/nofocus/browser.o: c/apps/browser/browser.c
	@mkdir -p $(dir $@)
	$(CC) $(JS_CF) -DBROWSER_NO_FOCUS -c $< -o $@

$(BUILD)/browser-nofocus.elf: $(ENGINE_OBJ) $(NOFOCUS_JS_OBJ) $(BROWSER_OBJ) $(CSS_OBJ) \
                              $(GFX_OBJ) $(RUST_LIB) $(BUILD)/apps/crt0.o \
                              $(BUILD)/browserobj/malloc_big.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ --start-group \
	    $(BUILD)/apps/crt0.o $(ENGINE_OBJ) $(NOFOCUS_JS_OBJ) $(BROWSER_OBJ) \
	    $(CSS_OBJ) $(GFX_OBJ) $(RUST_LIB) $(BUILD)/browserobj/malloc_big.o \
	    --end-group

$(BUILD)/browser-nofocus.aex: $(BUILD)/browser-nofocus.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/browser-nofocus.elf $@ Browser - 'B' 120 130 240

test-forms-negctl: $(ISO) $(BUILD)/browser-nofocus.aex
	@$(MAKE) DISK=$(BUILD)/disk-nofocus.img BROWSER_AEX=$(BUILD)/browser-nofocus.aex $(BUILD)/disk-nofocus.img
	python3 tests/qmp/qmp_forms.py $(ISO) $(BUILD)/disk-nofocus.img --expect-no-focus

# --- WPT: see the header. The shipping runner already links these files. ------
