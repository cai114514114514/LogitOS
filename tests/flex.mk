# tests/flex.mk -- CSS Flexbox layout, measured.
#
# Own fragment for the reason every other tests/*.mk gives: several lines share
# this worktree and a whole-file Makefile overwrite from a concurrent session
# has silently deleted other people's targets more than once. The only token
# this feature needs in the shared Makefile is its `-include`.
#
#   make test-flex          the gate. c/apps/browser/layout_flex.c against the
#                           numbers CSS Flexbox § 9 names, plus fifteen values
#                           transcribed from web-platform-tests.
#   make test-flex-negctl   THE NEGATIVE CONTROLS. Three sabotages, each of
#                           which lays pages out perfectly plausibly and gets
#                           the individual numbers wrong. All three are
#                           REQUIRED TO FAIL.
#
# WHAT THIS MEASURES AND WHAT IT DOES NOT. layout_flex.c is the flex sizing
# algorithm as a pure function over numbers -- no DOM, no display list, no font
# backend, no canvas. So this target settles whether the algorithm is right,
# with arithmetic and no renderer. It says nothing about whether those numbers
# reach the screen; that is the reftest harness's question, and it is a
# different one. Both are needed. Only one of them exists today.
#
# NOT WIRED INTO THE BROWSER YET. layout_flex.c is not in any app's source list
# -- integration into c/apps/browser/layout.c is a separate coordinated change,
# because layout.c belongs to another line. Nothing here depends on that
# landing, which is the point of the module having no dependency on layout.c.

.PHONY: test-flex test-flex-negctl

FLEX_DIR  := $(BUILD)/flex
FLEX_SRC  := c/apps/browser/layout_flex.c
FLEX_TEST := tests/unit/flex_test.c
# Self-sufficient rather than borrowing BTEST_INC: the module includes exactly
# one header of ours (css.h, for `struct cstyle`), and a test that needs no
# image/text/net include path should not silently acquire one.
FLEX_INC  := -Ic/apps/browser
FLEX_CF   := -O1 -g -std=c11 -Wall -Wextra -Wno-unused-parameter

$(FLEX_DIR)/flex_test: $(FLEX_TEST) $(FLEX_SRC) c/apps/browser/layout_flex.h \
                       c/apps/browser/css.h
	@mkdir -p $(FLEX_DIR)
	@$(CC) $(FLEX_CF) $(FLEX_INC) -o $@ $(FLEX_TEST) $(FLEX_SRC)

test-flex: $(FLEX_DIR)/flex_test
	@$(FLEX_DIR)/flex_test

# --- the negative controls ---------------------------------------------------
#
# An assertion nobody has watched fail is not an assertion. Each sabotage below
# is a WRONG IMPLEMENTATION THAT WORKS: it produces reasonable-looking layouts
# with correct totals, and only the individual item sizes are wrong. None of
# them is "delete flexbox".
#
#   FLEX_UNSCALED_SHRINK   distribute negative free space by the raw
#                          `flex-shrink` value instead of by flex-shrink times
#                          the item's flex base size. § 9.7 step 4b calls the
#                          weighted one the SCALED FLEX SHRINK FACTOR, and it
#                          is the single most commonly botched detail in the
#                          spec -- precisely because the unweighted reading is
#                          what a careful recollection produces, the totals
#                          still add up, and the result still looks like a
#                          layout. This is the control the line was asked for.
#
#   FLEX_NEGCTL_ONEPASS    resolve the flexible lengths in ONE pass and clamp,
#                          instead of freezing the items that violated their
#                          min/max and redistributing what they gave back.
#                          Not a straw man: this is exactly the approximation
#                          c/apps/browser/layout.c's own flex_resolve() makes
#                          today, by its own admission. It also breaks a
#                          browser-verified WPT value, which is the useful part
#                          -- it says the shipped approximation would fail a
#                          real reftest, not merely differ from this file.
#
#   FLEX_NEGCTL_NOAUTOMIN  make min-width:auto compute to 0 the way min-width
#                          does everywhere else in CSS. Every page still lays
#                          out. Content that should have pushed a flex row
#                          wider gets squeezed instead, which is the single
#                          most common way a real page comes out wrong.
#
# The target succeeds when the suite FAILS against all three, and it prints the
# first few assertions each one breaks so the failure is visible rather than
# merely counted.
FLEX_NEGS := FLEX_UNSCALED_SHRINK FLEX_NEGCTL_ONEPASS FLEX_NEGCTL_NOAUTOMIN

test-flex-negctl:
	@mkdir -p $(FLEX_DIR)
	@bad=0; \
	 for n in $(FLEX_NEGS); do \
	   $(CC) $(FLEX_CF) -w $(FLEX_INC) -D$$n -o $(FLEX_DIR)/neg_$$n \
	       $(FLEX_TEST) $(FLEX_SRC) || exit 1; \
	   if $(FLEX_DIR)/neg_$$n > $(FLEX_DIR)/neg_$$n.log 2>&1; then \
	     echo "test-flex-negctl: FAILED -- the suite PASSED with $$n in place,"; \
	     echo "  so nothing in it is measuring that rule."; bad=1; \
	   else \
	     echo "negative control ok: $$n -- suite fails:"; \
	     grep -m4 '^  FAIL' $(FLEX_DIR)/neg_$$n.log | sed 's/^/    /'; \
	   fi; \
	 done; \
	 exit $$bad
