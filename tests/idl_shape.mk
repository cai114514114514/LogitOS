# tests/idl_shape.mk -- WHERE the platform's members live, host-side.
#
#   make test-idl-shape          the gate: the shape pass
#                                (c/apps/gui/browser/js/runtime/idl_shape.c)
#                                over the browser's own install order, its
#                                must-hold assertions, and a per-category
#                                ratchet against Chrome 153's dump
#   make test-idl-shape-negctl   THE CONTROL: the same binary with the pass
#                                compiled to a no-op (-DIDL_SHAPE_NEGCTL_SKIP).
#                                The [shape] assertions must fail, by name; the
#                                [behaviour] ones must NOT
#   make idl-shape-baseline      rewrite the ratchet from the current numbers
#                                (say why in the commit that does it)
#
# WHAT IS COMPARED, AND AGAINST WHAT. tests/fixtures/webidl_shape/shape.js is
# the dump script; chrome-153.json is its output in Chrome 153; the guest runs
# the same script from shape.html (see PROVENANCE.md). This fragment runs it in
# a host link of the browser and diffs the result with the SAME differ the
# guest dump goes through (tests/qmp/webidl_shape_diff.py). The ratchet holds
# every category's count at or below tests/unit/idl_shape_baseline.json, so a
# change that moves one member back onto `document` goes red here in seconds
# rather than on a Cloudflare page in QEMU.
#
# THE LINK IS tests/wpt.mk's RUNNER LIST, minus two files, each named:
#   tests/unit/wpt_test.c                 that runner's main(); ours replaces it
#   c/apps/gui/browser/frame/passive.c    network iframes. It calls the app
#       shell's transport (bfetch_start_embedded/bfetch_body/... and
#       browser_hittest_node_scroll, in app/runtime.c and app/main.c, which
#       WPT_BROWSER_OUT excludes) and css_import_expand_alloc (css/import.c,
#       which the runner list never had), so WPT_TEST_SRC does not link AS IS
#       at the commit this landed on: `make build/wpt_test` stops on those
#       thirteen undefined symbols -- and so, identically at the parent commit,
#       do test-wpt*, test-events, test-eventloop, test-iframe, test-encoding
#       and test-frame-wired. Every consumer reaches passive.c through weak
#       declarations (frame/passive.h), so leaving it out is "no network
#       frames", which a page with no <iframe> never asks for.
# Reusing the list rather than copying it is the point: a hand-copied source
# list is how five host gates stopped BUILDING (CLAUDE.md, host-reality table).
#
# PARSE ORDER. tests/wpt.mk is -included long after this file, so WPT_TEST_SRC
# is empty while this is parsed. The LINK uses it only where make expands late
# (`=` variables and recipe bodies). The PREREQUISITES are expanded as they are
# read, so they name the lists the root Makefile has already defined by then --
# BROWSER_PIPE + BROWSER_JS_SRC, a superset of what is linked, so an edit to
# any linked TU relinks and an edit to an excluded one merely relinks too.
# Deliberately NOT .SECONDEXPANSION: that switch is global from the line that
# declares it, and here it would turn on second expansion for every rule in
# the ~1200 Makefile lines between this -include and the first fragment that
# already declares it.

.PHONY: test-idl-shape test-idl-shape-negctl idl-shape-baseline

IDL_SHAPE_OUT      = tests/unit/wpt_test.c c/apps/gui/browser/frame/passive.c
IDL_SHAPE_SRC      = tests/unit/idl_shape_test.c $(filter-out $(IDL_SHAPE_OUT),$(WPT_TEST_SRC))
IDL_SHAPE_REF      := tests/fixtures/webidl_shape/chrome-153.json
IDL_SHAPE_JS       := tests/fixtures/webidl_shape/shape.js
IDL_SHAPE_BASELINE := tests/unit/idl_shape_baseline.json
IDL_SHAPE_DIR      := $(BUILD)/idl_shape

IDL_SHAPE_NEED = @if [ -z "$(strip $(WPT_TEST_SRC))" ]; then \
	    echo "$@: WPT_TEST_SRC is empty -- tests/wpt.mk was not read, and this"; \
	    echo "  gate links that fragment's runner list. Check the -include lines."; exit 1; fi

$(BUILD)/idl_shape_test $(BUILD)/idl_shape_negctl: tests/unit/idl_shape_test.c \
    $(sort $(BROWSER_PIPE) $(BROWSER_JS_SRC)) $(HTML_PARSER_SRC) \
    $(BROWSER_INTERFACE_DEPS) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)

$(BUILD)/idl_shape_test:
	$(IDL_SHAPE_NEED)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WPT_CF) -o $@ $(IDL_SHAPE_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(GFX_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# The control: ONE flag, compiled into the pass's own TU, which returns before
# doing anything. Same link, same page, same checks, same dump script.
$(BUILD)/idl_shape_negctl:
	$(IDL_SHAPE_NEED)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WPT_CF) -DIDL_SHAPE_NEGCTL_SKIP -o $@ $(IDL_SHAPE_SRC) $(HTML_PARSER_SRC) \
	    $(QJS_SRC) $(GFX_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# THE CONTROL. With the pass compiled out, the binary must fail its [shape]
# checks -- these named ones, each a different property -- and must fail NO
# [behaviour] check: a behaviour failure here means the control broke the page
# rather than the shape, and a control that fails for the wrong reason is
# worse than none (CLAUDE.md rule 5). Then the dump-side assertions must ALL
# fail (idl_shape_ratchet.py --control), so the script the guest dump is held
# to is shown able to go red too.
IDL_SHAPE_CONTROL_MUST_FAIL := \
    'document owns exactly [location]' \
    'navigator owns nothing' \
    "Object.prototype.toString.call(navigator) === '[object Navigator]'" \
    'Performance.prototype chains to EventTarget.prototype' \
    "brand: reading Navigator.prototype.userAgent throws 'Illegal invocation'" \
    'brand: Document.prototype.cookie on a <div> / on Document.prototype throws' \
    "createHTMLDocument: cookie '' and cookie-averse, never the page jar" \
    'HTMLBodyElement.prototype carries onload .. onbeforeprint as get/set' \
    'after DOMContentLoaded and load, the singletons still own nothing new' \
    'a second pass moves a late own activeElement and re-wraps nothing'

test-idl-shape-negctl: $(BUILD)/idl_shape_negctl
	@mkdir -p $(IDL_SHAPE_DIR)
	@$(BUILD)/idl_shape_negctl $(IDL_SHAPE_JS) $(IDL_SHAPE_DIR)/negctl.json \
	    > $(IDL_SHAPE_DIR)/negctl.log 2>&1 || true
	@L=$(IDL_SHAPE_DIR)/negctl.log; \
	 grep -q '^shape dump -> ' $$L || { cat $$L; \
	    echo "test-idl-shape-negctl: the control wrote no dump -- it failed for some other reason"; exit 1; }; \
	 for c in $(IDL_SHAPE_CONTROL_MUST_FAIL); do \
	    grep -qF "FAIL [shape] $$c" $$L || { \
	        echo "test-idl-shape-negctl: CONTROL BROKEN -- without the pass, this still passes:"; \
	        echo "    $$c"; exit 1; }; done; \
	 if grep -q '^FAIL \[behaviour\]' $$L; then \
	    grep -A2 '^FAIL \[behaviour\]' $$L; \
	    echo "test-idl-shape-negctl: CONTROL BROKEN -- it failed a [behaviour] check, so it"; \
	    echo "  broke the page, not the shape"; exit 1; fi; \
	 echo "test-idl-shape-negctl: ok -- without the pass $$(grep -c '^FAIL \[shape\]' $$L) [shape] checks fail, 0 [behaviour]"
	@python3 tests/unit/idl_shape_ratchet.py $(IDL_SHAPE_REF) $(IDL_SHAPE_DIR)/negctl.json \
	    $(IDL_SHAPE_BASELINE) --control

test-idl-shape: test-idl-shape-negctl $(BUILD)/idl_shape_test
	@mkdir -p $(IDL_SHAPE_DIR)
	@rc=0; $(BUILD)/idl_shape_test $(IDL_SHAPE_JS) $(IDL_SHAPE_DIR)/ours.json \
	    > $(IDL_SHAPE_DIR)/test.log 2>&1 || rc=$$?; \
	 grep -E '^(FAIL|     |idl-shape:|\[idl-shape\])' $(IDL_SHAPE_DIR)/test.log; \
	 [ $$rc -eq 0 ] || { echo "test-idl-shape: FAILED (log: $(IDL_SHAPE_DIR)/test.log)"; exit 1; }
	@python3 tests/unit/idl_shape_ratchet.py $(IDL_SHAPE_REF) $(IDL_SHAPE_DIR)/ours.json \
	    $(IDL_SHAPE_BASELINE)

idl-shape-baseline: $(BUILD)/idl_shape_test
	@mkdir -p $(IDL_SHAPE_DIR)
	@$(BUILD)/idl_shape_test $(IDL_SHAPE_JS) $(IDL_SHAPE_DIR)/ours.json > $(IDL_SHAPE_DIR)/test.log 2>&1 || true
	@python3 tests/unit/idl_shape_ratchet.py $(IDL_SHAPE_REF) $(IDL_SHAPE_DIR)/ours.json \
	    $(IDL_SHAPE_BASELINE) --write

ci-host: test-idl-shape
