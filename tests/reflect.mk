# tests/reflect.mk -- IDL attribute reflection: the coercions, and the table.
#
# What the code under test is: c/apps/browser/js_reflect.c turns `el.title` into
# a view of the `title` content attribute, and does that for the ~380
# (element, IDL name, content name, type) triples in js_reflect.inc, which
# tools/gen_reflect.py generates from the WPT corpus's own reflection tables.
#
# WHY THERE ARE THREE TARGETS AND NOT ONE. There are three distinct things that
# can be wrong here and they fail in different places:
#
#   test-reflect         the COERCIONS. Seconds, no corpus needed. Every
#                        assertion is one a plain string pass-through gets
#                        wrong -- see the header of tests/unit/reflect_test.c.
#   test-reflect-negctl  proves that suite can go red, by building the
#                        pass-through and requiring it to.
#   test-reflect-table   the TABLE still matches the corpus it was generated
#                        from. A hand-edit here is invisible to the other two:
#                        they test the machinery, and a wrong row is data.
#
# Own fragment rather than lines in the Makefile, for the reason tests/wpt.mk
# and tests/ci.mk give: a whole-file Makefile write from a concurrent line
# deletes targets added straight into it, and that happened to nine targets in
# one night.
#
# The MEASUREMENT is tests/wpt.mk's, not this file's: `make wpt ONLY=html/dom/
# reflection` is the number, and duplicating it here would be a second scoreboard
# for one thing -- which the forms line has already had to delete once
# (af5f085, "delete test-wpt-forms, which measured the same browser twice").

.PHONY: test-reflect test-reflect-negctl test-reflect-table reflect-table

# The link is js_dom_test's plus js_reflect.c. Deliberately NOT the whole
# browser: this test wants the reflection layer over the real DOM and nothing
# else, so a failure here names the coercion rather than the page runtime.
REFLECT_SRC := tests/unit/reflect_test.c c/apps/browser/js_dom.c \
               c/apps/browser/js_reflect.c c/apps/browser/css_engine.c \
               c/apps/browser/css_vars.c

REFLECT_CF := -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"'

$(BUILD)/reflect_test: $(REFLECT_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) $(REFLECT_CF) -o $@ $(REFLECT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a -lm

test-reflect: $(BUILD)/reflect_test
	@$(BUILD)/reflect_test

# --- the negative control ---------------------------------------------------
# NOT "remove reflection". Deleting the file proves a missing feature is
# missing, which nobody doubted. The failure this code can really have is that
# reflection WORKS and is untyped -- every property present, every prototype
# right, every attribute round-tripping, a real page rendering, and the
# enumerated invalid-value defaults, the integer parsing rules, the range
# clamping, the throwing setters and the URL resolution all gone. A reviewer
# cannot see that and a smoke test cannot either.
#
# So -DREFLECT_NEGCTL leaves every line of the plumbing intact and makes exactly
# one substitution: every reflected attribute becomes a plain DOMString
# pass-through (js_reflect.c's eff_type). test-reflect must go red on it, and
# this target succeeds only when it does.
test-reflect-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) $(REFLECT_CF) -DREFLECT_NEGCTL -o $(BUILD)/reflect_negctl \
	    $(REFLECT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@if $(BUILD)/reflect_negctl > $(BUILD)/reflect_negctl.log 2>&1; then \
	    echo "test-reflect-negctl: FAILED -- reflect_test passed with every"; \
	    echo "  attribute made a plain DOMString pass-through, so it is not"; \
	    echo "  measuring the coercions and test-reflect proves nothing."; \
	    exit 1; \
	 else \
	    n=$$(grep -c '^FAIL' $(BUILD)/reflect_negctl.log); \
	    echo "test-reflect-negctl: ok -- the suite catches an untyped reflection"; \
	    echo "  ($$n assertions went red). The first few:"; \
	    grep '^FAIL' $(BUILD)/reflect_negctl.log | head -4 | sed 's/^/    /'; \
	 fi

# --- the table still describes the corpus -----------------------------------
# js_reflect.inc is generated, and a generated file that someone edits by hand
# is a fork nobody knows about. This regenerates into $(BUILD) and diffs.
#
# A MISSING CORPUS IS NOT A FAILURE, and that is tests/wpt.mk's rule, adopted
# here for the same reason it gives: the corpus is optional and the capability
# is not, so the vendored data can be deleted later without deleting the check.
# It says so out loud rather than passing quietly.
test-reflect-table:
	@if [ ! -d $(WPT_ROOT)/html/dom ]; then \
	    echo "test-reflect-table: SKIP -- no corpus at $(WPT_ROOT)/html/dom (make wpt-fetch)."; \
	    echo "  The committed js_reflect.inc stands; \`make wpt-fetch\` brings the"; \
	    echo "  data back. A missing corpus is not a regression in the table."; \
	 else \
	    mkdir -p $(BUILD); \
	    python3 tools/gen_reflect.py -o $(BUILD)/js_reflect.inc.new || exit 1; \
	    if diff -u c/apps/browser/js_reflect.inc $(BUILD)/js_reflect.inc.new \
	           > $(BUILD)/js_reflect.inc.diff 2>&1; then \
	        echo "test-reflect-table: ok -- the table matches the corpus it came from"; \
	    else \
	        echo "test-reflect-table: FAIL -- js_reflect.inc and the corpus disagree."; \
	        echo "  Either the corpus moved (run \`make reflect-table\` and read the"; \
	        echo "  diff before committing it) or the file was edited by hand."; \
	        head -40 $(BUILD)/js_reflect.inc.diff; \
	        exit 1; \
	    fi; \
	 fi

# Regenerate in place. Not named test-* on purpose: it changes the tree.
reflect-table:
	@python3 tools/gen_reflect.py

# ---------------------------------------------------------------------------
# The CSSOM NAMED-PROPERTY SET, which is a different question from reflection
# and shares this fragment only because both are IDL attributes over js_dom.c.
#
# A property with no named accessor cannot be set from script, so its parser is
# unreachable and every test of it fails on "property should be set" without the
# parser running. js_dom.c took that set from css.h's CSSP_* enum -- the ~60
# properties the CASCADE resolves -- when the right set is every property the
# PARSER knows. The CSS line implemented position-area in full, 2,598 checks,
# and gained zero for exactly this reason.
#
# The control restores the enum-sourced set and nothing else, so a red run names
# the source of the list rather than a broken accessor.
.PHONY: test-cssprops test-cssprops-negctl

CSSPROPS_SRC := tests/unit/cssprops_test.c c/apps/browser/js_dom.c                 c/apps/browser/js_reflect.c c/apps/browser/css_engine.c                 c/apps/browser/css_vars.c

$(BUILD)/cssprops_test: $(CSSPROPS_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) $(REFLECT_CF) -o $@ $(CSSPROPS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) 	    $(BUILD)/libcss_host.a -lm

test-cssprops: $(BUILD)/cssprops_test
	@$(BUILD)/cssprops_test

test-cssprops-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(BUILD)
	@$(CC) $(REFLECT_CF) -DCSSD_PROPS_FROM_ENUM -o $(BUILD)/cssprops_negctl 	    $(CSSPROPS_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
	@if $(BUILD)/cssprops_negctl > $(BUILD)/cssprops_negctl.log 2>&1; then 	    echo "test-cssprops-negctl: FAILED -- cssprops_test passed with the"; 	    echo "  named-property set taken from the cascade enum again, so it is"; 	    echo "  not measuring which properties are settable."; 	    exit 1; 	 else 	    n=$$(grep -c '^FAIL' $(BUILD)/cssprops_negctl.log); 	    echo "test-cssprops-negctl: ok -- the suite catches the enum-sourced set"; 	    echo "  ($$n assertions went red). The first few:"; 	    grep '^FAIL' $(BUILD)/cssprops_negctl.log | head -4 | sed 's/^/    /'; 	 fi
