# tests/frame_window.mk -- the same-origin frame's Window, driven through the
# REAL <iframe> path: js_page_open, the page's document.createElement('iframe'),
# js_platform.c's contentDocument/contentWindow getters and settle(), and
# js_frame.c's frame_install_window. tests/unit/frame_window_test.c.
#
#   make test-frame-window                 the gate
#   make test-frame-window-wire-negctl     settle() calls the OLD
#                                          `__frameAdopt(doc)` -- no element,
#                                          no URL, no navigation door. Exactly
#                                          the "[wire]" checks must fail.
#   make test-frame-window-fence-negctl    js_frame.c without the fence that
#                                          stops a discarded frame's timer
#                                          callback. Exactly "[fence]" fails.
#   make test-frame-window-cancel-negctl   js_frame.c without the release-time
#                                          cancellation of the frame's queued
#                                          timers. Exactly "[cancel]" fails.
#
# WHY A SECOND GATE BESIDE tests/frame.mk. That fragment's specimen calls
# __frameAdopt by hand, with the Window inputs settle() is supposed to pass;
# it cannot tell a js_platform.c that passes them from one that does not.
# This gate never calls __frameAdopt, so [wire] can only pass through
# js_platform.c -- the WPT runner that linked layout.c and never called it is
# this tree's standing reminder that linking a TU is not running it. It is a
# host gate (no QEMU, no corpus), so unlike test-frame-wired it cannot SKIP.
#
# Every control is sed-built from the shipping source and checked to have
# patched something (a sed that matches nothing is a green fake control), and
# each must fail EXACTLY the checks carrying its tag: fewer means the tag is
# decoration, more means the patch broke something else and proves nothing.
#
# INCLUDE-ORDER TRAP, stated because it bit tests/passive_frame.mk: this file
# is included right after tests/jsprobe.mk, i.e. BEFORE tests/domiface.mk and
# tests/select_state.mk define DOMIFACE_CF / SELECT_STATE_SRC. Recipes expand
# late and see them; prerequisites expand at parse time and would see nothing.
# So the prerequisites name every translation unit this gate is about.
FRAME_WINDOW_DIR = $(BUILD)/frame-window
# $(sort) also removes duplicates: SELECT_STATE_SRC itself now carries events.c
# and js/net/url.c, and linking either twice is 20 duplicate symbols.
FRAME_WINDOW_SRC = $(sort $(filter-out tests/unit/select_state_test.c,$(SELECT_STATE_SRC)) \
    tests/unit/frame_window_test.c c/apps/gui/browser/js/dom/domparser.c \
    c/apps/gui/browser/js/dom/frame.c c/apps/gui/browser/js/dom/events.c \
    c/apps/gui/browser/js/net/url.c)
# js/net/url.c is here for the product's WHATWG URL: without it the page's URL
# is js_webapi.c's four-field prelude, which accepts "http://[" -- and the
# "invalid URL throws SyntaxError" check then measured the parser, not the door.
FRAME_WINDOW_DEP = tests/unit/frame_window_test.c tests/frame_window.mk \
    c/apps/gui/browser/js/dom/frame.c c/apps/gui/browser/js/dom/frame.h \
    c/apps/gui/browser/js/dom/domparser.c c/apps/gui/browser/js/dom/events.c \
    c/apps/gui/browser/js/runtime/platform.c c/apps/gui/browser/js/runtime/page.c \
    c/apps/gui/browser/js/dom/dom.c c/apps/gui/browser/js/net/url.c \
    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
FRAME_WINDOW_CF = $(DOMIFACE_CF)

.PHONY: test-frame-window test-frame-window-wire-negctl test-frame-window-fence-negctl \
        test-frame-window-cancel-negctl

$(FRAME_WINDOW_DIR)/test: $(FRAME_WINDOW_DEP)
	@mkdir -p $(FRAME_WINDOW_DIR)
	@$(CC) -O2 -w $(FRAME_WINDOW_CF) -o $@ $(FRAME_WINDOW_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# One recipe for the three controls. $(1) control name, $(2) the source file
# it patches, $(3) the NAME of the variable holding the sed expression, $(4)
# the tag that must fail alone.
# The sed-built copy lives outside the source tree, so its own relative
# includes (platform.c's "rejections.inc", "../dom/native_mo.inc") are found
# through -iquote on the ORIGINAL file's directory -- quoted includes only,
# so no browser-internal header joins the angle-bracket search path. The
# flags are $$-deferred: $(call) runs at parse time, before domiface.mk has
# defined DOMIFACE_CF (the include-order trap above, in its second form).
define FRAME_WINDOW_NEGCTL
$(FRAME_WINDOW_DIR)/$(1): $(FRAME_WINDOW_DEP)
	@mkdir -p $(FRAME_WINDOW_DIR)
	@sed '$$($(3))' $(2) > $(FRAME_WINDOW_DIR)/$(1).c
	@cmp -s $(2) $(FRAME_WINDOW_DIR)/$(1).c && \
	    { echo "test-frame-window-$(1)-negctl: FAILED -- sed patched nothing in $(2)"; exit 1; } || true
	@$(CC) -O2 -w $$(FRAME_WINDOW_CF) -iquote $(dir $(2)) -o $$@ $$(filter-out $(2),$$(FRAME_WINDOW_SRC)) $(FRAME_WINDOW_DIR)/$(1).c \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-frame-window-$(1)-negctl: $(FRAME_WINDOW_DIR)/$(1)
	@log=$(FRAME_WINDOW_DIR)/$(1).log; \
	 if $(FRAME_WINDOW_DIR)/$(1) > $$$$log 2>&1; then \
	    echo "test-frame-window-$(1)-negctl: FAILED -- the gate PASSED with the control applied"; exit 1; \
	 fi; \
	 tagged=$$$$(grep '^FAIL: ' $$$$log | grep -c '\[$(4)\]'); \
	 untagged=$$$$(grep '^FAIL: ' $$$$log | grep -vc '\[$(4)\]'); \
	 passed=$$$$(grep '^PASS: ' $$$$log | grep -c '\[$(4)\]'); \
	 if [ "$$$$untagged" != "0" ] || [ "$$$$passed" != "0" ] || [ "$$$$tagged" = "0" ]; then \
	    echo "test-frame-window-$(1)-negctl: FAILED -- [$(4)] failed=$$$$tagged passed=$$$$passed, other failures=$$$$untagged"; \
	    grep -E '^(FAIL|PASS): .*\[$(4)\]|^FAIL: ' $$$$log; exit 1; \
	 fi; \
	 echo "test-frame-window-$(1)-negctl: ok -- exactly the $$$$tagged [$(4)] check(s) fail:"; \
	 grep '^FAIL: ' $$$$log
endef

# The three patches. Held in variables because two of them contain commas,
# which $(call) would read as argument separators.
# settle() handing js_frame.c only the document, as it did before 2026-09-25:
FRAME_WINDOW_SED_wire   = s/G.__frameAdopt(doc, el, r.url, r.nav)/G.__frameAdopt(doc)/
# the timer callback without its liveness check:
FRAME_WINDOW_SED_fence  = s|if (!f) return JS_UNDEFINED;   /\* FENCE|(void)0; /* FENCE|
# __frameRelease without cancelling the frame's queued timers:
FRAME_WINDOW_SED_cancel = s/if (cancel) frame_cancel_ids(owner, timers, n);/(void)cancel;/
$(eval $(call FRAME_WINDOW_NEGCTL,wire,c/apps/gui/browser/js/runtime/platform.c,FRAME_WINDOW_SED_wire,wire))
$(eval $(call FRAME_WINDOW_NEGCTL,fence,c/apps/gui/browser/js/dom/frame.c,FRAME_WINDOW_SED_fence,fence))
$(eval $(call FRAME_WINDOW_NEGCTL,cancel,c/apps/gui/browser/js/dom/frame.c,FRAME_WINDOW_SED_cancel,cancel))

test-frame-window: test-frame-window-wire-negctl test-frame-window-fence-negctl \
                   test-frame-window-cancel-negctl $(FRAME_WINDOW_DIR)/test
	@$(FRAME_WINDOW_DIR)/test

ci-host: test-frame-window
