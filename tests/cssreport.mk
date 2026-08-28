# --- the stylesheet pipeline's accounting, and its control -----------------
#
# In its own .mk rather than in the Makefile because several lines share this
# tree and a stale whole-file Makefile snapshot has silently deleted other
# people's targets more than once. A separate file cannot be clobbered that way.
#
# WHAT IS GATED. c/apps/browser/css_report.c is the one record of what happened
# to a page's CSS -- linked, fetched, concatenated, parsed, and above all
# DROPPED. It exists because the browser printed twelve lines about scripts and
# zero about stylesheets, which made five different bugs (never requested /
# fetch failed / never parsed / rules discarded / layout wrong) produce one
# indistinguishable symptom.
#
# THE CONTROL IS NOT A SEPARATE TARGET, AND THAT IS DELIBERATE. CLAUDE.md's
# rule 5 and its 61 stranded controls: a `test-X-negctl` that only the docs run
# is worse than none, because it looks fixed. So the two controls live INSIDE
# css_report_test and run on every invocation -- a clean stylesheet whose drop
# counters must stay at ZERO, and the dirty stylesheet re-run with the parser
# hooks DETACHED, which must also read zero. The second is the one that proves
# the numbers come from LibCSS and not from the harness; it is the structural
# form of this line's own scar, where the WPT runner linked css_extra.c and
# layout.c and never called them.
#
# Also usable as a probe on real bytes, which is what it was built for:
#   ./build/css_report_test page.html sheet1.css sheet2.css ... '#some-id'
# prints the whole record for that page, and the computed style of any element
# named with a leading '#'. That is how "the CSS rendering cannot be used" on
# bing became "the external sheets did not reach the cascade" -- with the same
# page rendering correctly the moment they did.

CSSREPORT_SRC := tests/unit/css_report_test.c \
                 c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                 $(HTML_PARSER_SRC)

test-cssreport: $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_report_test \
	    $(CSSREPORT_SRC) $(BUILD)/libcss_host.a -lm
	@$(BUILD)/css_report_test

ci-host: test-cssreport
