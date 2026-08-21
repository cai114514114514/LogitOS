# tests/layoutbox.mk -- the box table in c/apps/browser/layout.c, measured.
#
#   make test-layout-box          the gate: host unit tests, seconds
#   make test-layout-box-negctl   the four sabotages, each of which the suite
#                                 MUST fail against
#   make layout-box-survey        the corpus survey: over the five WPT
#                                 directories js_cssom.h's ask was measured on,
#                                 how many elements have a box the display list
#                                 can answer, and how many the table can
#
# WHY A FRAGMENT AND NOT LINES IN THE MAKEFILE: the reason every other
# tests/*.mk gives -- several agents share this tree and a whole-file Makefile
# write from a concurrent line has silently deleted other people's targets more
# than once.
#
# THE NEGATIVE CONTROL THAT MATTERS IS THE FIRST ONE, and it is the shape of
# "the line silently not done": populate the box table only for elements that
# were ALREADY emitting an IT_RECT. Every geometry assertion that passed before
# still passes, the table is populated, `layout_node_box` returns 1 for plenty
# of elements -- and the 2,583 NOBOX cases the table exists to close are
# unchanged. A suite that cannot tell those two builds apart is measuring
# nothing, so this target requires it to fail against that build by name.

.PHONY: test-layout-box test-layout-box-negctl layout-box-survey

LBOX_DIR := $(BUILD)/layoutbox
LBOX_SRC := tests/unit/layout_box_test.c c/apps/browser/layout.c \
            c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
            c/apps/browser/css_extra.c

$(LBOX_DIR)/layout_box_test: $(LBOX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a \
                             c/apps/browser/layout.h
	@mkdir -p $(LBOX_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(LBOX_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm

test-layout-box: $(LBOX_DIR)/layout_box_test
	@$(LBOX_DIR)/layout_box_test

# Each sabotage is a real previous behaviour or a plausible wrong one, never a
# `return 0` -- an assertion only proves something about the failure it can
# actually distinguish.
#
#   BOX_INK_ONLY        the table is the display list with extra steps
#   BODY_NOPAD          <body>'s padding and border take no space (the bug)
#   ABS_PARENT          position:absolute anchors at its PARENT, whatever the
#                       parent's position is (the other bug)
#   SCROLL_IS_CLIENT    the scrollable overflow area is the padding box, so
#                       scrollWidth always equals clientWidth. The plausible
#                       wrong overflow: nothing is 0, nothing throws, and a
#                       scroller never reports anything to scroll.
LBOX_NEGS := BOX_INK_ONLY BODY_NOPAD ABS_PARENT SCROLL_IS_CLIENT

test-layout-box-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(LBOX_DIR)
	@bad=0; \
	 for n in $(LBOX_NEGS); do \
	   $(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_NEGCTL_$$n \
	       -o $(LBOX_DIR)/negctl_$$n $(LBOX_SRC) \
	       $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm || exit 1; \
	   if $(LBOX_DIR)/negctl_$$n > $(LBOX_DIR)/negctl_$$n.log 2>&1; then \
	     echo "test-layout-box-negctl: FAILED -- the suite PASSED with $$n"; \
	     echo "  sabotaged, so nothing in it is measuring that."; bad=1; \
	   else \
	     echo "test-layout-box-negctl: ok -- $$n sabotaged, suite fails:"; \
	     grep -m2 'FAIL' $(LBOX_DIR)/negctl_$$n.log | sed 's/^/    /'; \
	   fi; \
	 done; \
	 exit $$bad

# --- the corpus survey ------------------------------------------------------
# js_cssom.h's ask is stated as a measurement over five WPT directories, so the
# answer is measured the same way. For every element of every file under them,
# this classifies the box exactly as js_cssom.c's border_box() does -- EXACT
# (its own IT_RECT), INKUNION (some ink in its subtree), NOBOX (nothing) -- and
# then reports whether layout_node_box() answers.
#
# THE HONEST CAVEAT, stated here rather than in a report: border_box() is
# called on the elements a TEST asks about, and this walks every element in the
# document, so the two populations are not the same one. What it can settle,
# and what it is for, is whether the table answers the NOBOX class at all --
# a per-call weighting cannot turn "answers all of them" into less.
LBOX_WPT ?= build/wpt
LBOX_DIRS ?= css/css-align css/css-sizing css/css-flexbox css/css-grid css/cssom-view

layout-box-survey: $(LBOX_DIR)/layout_box_survey
	@$(LBOX_DIR)/layout_box_survey $(LBOX_WPT) $(LBOX_DIRS)

LBOX_SURVEY_SRC := tests/unit/layout_box_survey.c c/apps/browser/layout.c \
                   c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                   c/apps/browser/css_extra.c

$(LBOX_DIR)/layout_box_survey: $(LBOX_SURVEY_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(LBOX_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(LBOX_SURVEY_SRC) \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
