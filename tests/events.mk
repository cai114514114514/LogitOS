# tests/events.mk -- the DOM event layer (c/apps/browser/js_events.c).
#
#   make test-events            the ordering suite: must pass          (the gate)
#   make test-events-negctl     the same suite against a stubbed
#                               dispatcher: must FAIL                  (the control)
#   make test-wpt-events        WPT dom/events with the layer linked
#   make wpt-events-baseline    rewrite tests/unit/wpt_events_fail.txt
#   make test-wpt-events-all    the full WPT corpus with the layer linked
#
# WHY THIS IS A SEPARATE FRAGMENT AND NOT LINES IN tests/wpt.mk.
# Two reasons. The one that matters day to day is the one wpt.mk gives for
# itself -- several lines are editing this tree at once and a shared file gets
# overwritten wholesale. The other is ownership: wpt.mk belongs to the WPT
# line, and the stock `make test-wpt` must keep measuring the stock build.
# js_events_install is declared WEAK in js_page.c, so a runner built without
# this TU links cleanly and simply has no event layer -- which is exactly what
# makes `make test-wpt` and `make test-wpt-events` two different measurements
# of the same corpus rather than one measurement with a moving definition.
#
# PARSE ORDER IS NOT ASSUMED, and that is deliberate. This fragment reuses
# wpt.mk's runner source list, but wpt.mk's own `-include` line is not in HEAD
# yet -- it is another line's in-flight edit -- so which of the two fragments
# make reads first is not something this file can know. Every reference to
# WPT_TEST_SRC / WPT_CF / HTML_PARSER_SRC / QJS_SRC therefore lives in a RECIPE
# body, which make expands when the rule runs (by which time the whole makefile
# has been read), never in a prerequisite list, which make expands as it parses.
# Putting $(WPT_TEST_SRC) on a prerequisite line would silently expand to
# nothing whenever this file happened to be read first, and the runner would be
# linked from a source list missing thirteen of its fourteen files.
#
# The recipes check for it and say so rather than emitting a confusing link
# error, because "wpt.mk was not read" is the only way it can be empty.

.PHONY: test-events test-events-negctl test-wpt-events test-wpt-events-all
.PHONY: wpt-events-baseline events-root

EVENTS_SRC      := c/apps/browser/js_events.c
EVENTS_BASELINE := tests/unit/wpt_events_fail.txt
EVENTS_ROOT     := $(BUILD)/events_root

EVENTS_NEED = @if [ -z "$(strip $(WPT_TEST_SRC))" ]; then \
	    echo "$@: WPT_TEST_SRC is empty -- tests/wpt.mk was not read."; \
	    echo "  This fragment builds the WPT runner plus js_events.c and needs"; \
	    echo "  that fragment's source list. Check the -include lines."; exit 1; fi

# The prerequisites still have to be RIGHT -- a runner that does not relink
# when wpt_test.c or js_dom.c changes is worse than a slow one. Secondary
# expansion is what gets both: `$$(WPT_TEST_SRC)` survives the parse pass
# untouched and is expanded in a second pass, after every fragment has been
# read, so the full source list lands in the prerequisite list whatever order
# make happened to read the two files in.
.SECONDEXPANSION:

$(BUILD)/wpt_events: $$(WPT_TEST_SRC) $$(HTML_PARSER_SRC) $(EVENTS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	$(EVENTS_NEED)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WPT_CF) -o $@ $(filter-out $(EVENTS_SRC),$(WPT_TEST_SRC)) \
	    $(EVENTS_SRC) $(HTML_PARSER_SRC) $(GFX_SRC) \
	    $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# The same runner with the propagation walk removed. See the JS_EVENTS_NEGCTL
# block in js_events.c for what "removed" means precisely.
$(BUILD)/wpt_events_negctl: $$(WPT_TEST_SRC) $$(HTML_PARSER_SRC) $(EVENTS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	$(EVENTS_NEED)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(WPT_CF) -DJS_EVENTS_NEGCTL -o $@ \
	    $(filter-out $(EVENTS_SRC),$(WPT_TEST_SRC)) $(EVENTS_SRC) \
	    $(HTML_PARSER_SRC) $(GFX_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

# The ordering suite is served out of a staged root rather than a committed
# one: the runner resolves /resources/testharness.js relative to --root, and
# the alternative is a committed symlink into third_party/wpt, which on a
# core.autocrlf=true checkout is exactly the kind of thing that arrives broken.
events-root:
	@mkdir -p $(EVENTS_ROOT)/order
	@cp -f tests/events/*.html $(EVENTS_ROOT)/order/
	@rm -rf $(EVENTS_ROOT)/resources
	@cp -r $(WPT_ROOT)/resources $(EVENTS_ROOT)/resources 2>/dev/null || \
	    { echo "test-events: no WPT corpus at $(WPT_ROOT) -- run 'make wpt-fetch'"; exit 0; }

# The gate. An empty baseline means every failure is a new failure, so --strict
# exits non-zero on any of them: this target is green only at 100%.
test-events: $(BUILD)/wpt_events events-root
	@if [ ! -d $(EVENTS_ROOT)/resources ]; then \
	    echo "test-events: SKIPPED (no corpus)"; exit 0; fi
	@$(BUILD)/wpt_events --root $(EVENTS_ROOT) --subset order -b /dev/null --strict -v 20

# The control. An assertion nobody has watched fail is not a known-failing
# assertion: this build keeps the listeners, the options decode, the event
# objects and preventDefault, and removes ONLY the capture/target/bubble walk.
# If the ordering suite still passes, it is not measuring the walk.
test-events-negctl: $(BUILD)/wpt_events_negctl events-root
	@if [ ! -d $(EVENTS_ROOT)/resources ]; then \
	    echo "test-events-negctl: SKIPPED (no corpus)"; exit 0; fi
	@if $(BUILD)/wpt_events_negctl --root $(EVENTS_ROOT) --subset order \
	        -b /dev/null --strict > $(BUILD)/events_negctl.log 2>&1; then \
	    echo "test-events-negctl: FAILED -- the ordering suite PASSED with the"; \
	    echo "  propagation walk stubbed out, so it is not measuring ordering."; \
	    exit 1; \
	 else \
	    echo "test-events-negctl: ok -- the suite fails without the three-phase walk:"; \
	    grep 'NEW FAILURE' $(BUILD)/events_negctl.log | head -8; \
	    grep -E '^WPT:' $(BUILD)/events_negctl.log; \
	 fi

# dom/events only -- the scoreboard for this line, and the only thing the
# ratchet covers. The baseline is written with the SAME --only scope it is
# checked with; a baseline whose scope does not match the run it is compared
# against reports every file outside the scope as "newly passing", which is
# noise that hides the one line that matters.
test-wpt-events: $(BUILD)/wpt_events
	@$(BUILD)/wpt_events --root $(WPT_ROOT) --only dom/events -b $(EVENTS_BASELINE) \
	    $(if $(V),-v $(V),) $(if $(STRICT),--strict,)

wpt-events-baseline: $(BUILD)/wpt_events
	@$(BUILD)/wpt_events --root $(WPT_ROOT) --only dom/events -b $(EVENTS_BASELINE) \
	    --write-baseline

# The whole corpus with the layer linked. A RAW MEASUREMENT, not a ratchet:
# the event constructors are used well outside dom/events, so a regression in
# html/ or css/ would never show up in the number above -- but comparing the
# whole corpus against a dom/events baseline would drown that signal, so this
# one deliberately carries no baseline. Run it before and after a change and
# diff the two totals.
test-wpt-events-all: $(BUILD)/wpt_events
	@$(BUILD)/wpt_events --root $(WPT_ROOT) -b /dev/null $(if $(V),-v $(V),) \
	    $(if $(ONLY),--only $(ONLY),) $(if $(SUBSET),--subset $(SUBSET),)
