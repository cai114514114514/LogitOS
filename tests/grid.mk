# ============================================================================
# CSS Grid Layout -- c/apps/browser/layout_grid.{c,h}. Host tests, seconds.
#
# THE ORACLE IS THE SPEC, NOT A SCREENSHOT. CSS Grid is specified as an
# algorithm with exact numeric outcomes: the track sizing algorithm's steps
# produce specific base sizes and growth limits, placement produces specific
# line indices, free-space distribution produces specific pixel values. So
# "given this template, these items and this container width, the columns
# resolve to [10, 90]" is a complete, checkable assertion that needs no
# renderer, no DOM and no font -- which is why these tests exist and pass
# before the reftest harness does. When reftests arrive they ask a DIFFERENT
# question (do the numbers reach the screen), not this one.
#
#   make test-grid          every host suite below
#   make test-grid-parse    value parsers + auto-fill/auto-fit repetition count
#   make test-grid-place    placement: line-based, named, spans, sparse/dense
#   make test-grid-size     the track sizing algorithm, s12.3-s12.8
#   make test-grid-negctl   the spanning-item suite with the spec's space
#                           distribution replaced by an even split; it MUST
#                           fail, and the target succeeds when it does
# ============================================================================
.PHONY: test-grid test-grid-parse test-grid-place test-grid-size test-grid-negctl

GRID_SRC  := c/apps/browser/layout_grid.c
GRID_CF   := -O1 -g -Wall -Wextra -Ic/apps/browser

$(BUILD)/grid_parse_test: tests/unit/grid_parse_test.c $(GRID_SRC) c/apps/browser/layout_grid.h
	@mkdir -p $(BUILD)
	$(CC) $(GRID_CF) -o $@ tests/unit/grid_parse_test.c $(GRID_SRC)

test-grid-parse: $(BUILD)/grid_parse_test
	$(BUILD)/grid_parse_test

$(BUILD)/grid_place_test: tests/unit/grid_place_test.c $(GRID_SRC) c/apps/browser/layout_grid.h
	@mkdir -p $(BUILD)
	$(CC) $(GRID_CF) -o $@ tests/unit/grid_place_test.c $(GRID_SRC)

test-grid-place: $(BUILD)/grid_place_test
	$(BUILD)/grid_place_test

$(BUILD)/grid_size_test: tests/unit/grid_size_test.c $(GRID_SRC) c/apps/browser/layout_grid.h
	@mkdir -p $(BUILD)
	$(CC) $(GRID_CF) -o $@ tests/unit/grid_size_test.c $(GRID_SRC)

test-grid-size: $(BUILD)/grid_size_test
	$(BUILD)/grid_size_test

# The NEGATIVE CONTROL, and it is meant to fail.
#
# It does NOT delete grid -- a control that fails everything proves nothing
# except that the tests run. It attacks the one step a plausible wrong
# implementation gets wrong: -DGRID_SPAN_EVEN_SPLIT replaces s12.5.1's
# "distribute extra space across spanned tracks" with an EVEN SPLIT of a
# spanning item's leftover contribution across the tracks it spans, ignoring
# which tracks are affected, ignoring growth limits and ignoring the
# beyond-limits priority rules.
#
# That build still produces sensible-looking track sizes with the right totals.
# The spec's own worked example resolves to [55, 45] instead of [10, 90] -- same
# 100px container, same sum, wrong tracks. Most of the suite stays green; only
# the assertions that actually depend on the procedure go red. The target
# succeeds when the test fails, and reports how many.
test-grid-negctl:
	@mkdir -p $(BUILD)
	$(CC) $(GRID_CF) -DGRID_SPAN_EVEN_SPLIT -o $(BUILD)/grid_size_negctl \
	    tests/unit/grid_size_test.c $(GRID_SRC)
	@echo "--- negative control: spanning items split evenly instead of per s12.5.1 ---"
	@if $(BUILD)/grid_size_negctl > $(BUILD)/grid_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the even-split build satisfied the spec assertions"; \
	    exit 1; \
	else \
	    grep -c '^FAIL' $(BUILD)/grid_negctl.log | \
	        xargs -I{} echo "negative control ok: {} assertion(s) fail on an even split"; \
	    grep -c '^FAIL' $(BUILD)/grid_negctl.log | \
	        awk '{ if ($$1 < 1) { print "but none of them were spanning cases"; exit 1 } }'; \
	fi

test-grid: test-grid-parse test-grid-place test-grid-size
