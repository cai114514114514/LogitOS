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
#   make test-grid-negctl   the spanning-item suite with the spec's space
#                           distribution replaced by an even split; it MUST
#                           fail, and the target succeeds when it does
# ============================================================================
.PHONY: test-grid test-grid-parse test-grid-place test-grid-negctl

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

test-grid: test-grid-parse test-grid-place
