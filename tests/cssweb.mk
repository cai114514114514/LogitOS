# --- the CSS engine's measurement + fidelity targets -----------------------
#
# In its own .mk rather than in the Makefile because several lines share this
# tree and a stale Makefile snapshot has silently deleted other people's targets
# more than once today (see the "re-add ... lost to a stale Makefile snapshot"
# commits). A separate file cannot be clobbered by a whole-file overwrite.

# --- audit-css: what REAL pages need from the engine, measured -------------
# The work order for the CSS engine, produced rather than guessed. Runs the
# real pipeline over the committed corpus in tests/fixtures/cssweb (15 pages:
# news, docs, search results, product, Chinese portals, a GitHub-style app UI)
# with a reporter installed inside LibCSS's parseProperty(), and ranks every
# declaration the cascade throws away plus every layout mode the pages need,
# weighted by how much of the box tree each mode GOVERNS.
# See the header of tests/unit/css_audit.c for why it is weighted that way.
#   make audit-css                 # the whole corpus
#   make audit-css AUDIT_TOP=60
#   make audit-css AUDIT_DIRS=tests/fixtures/cssweb/github
AUDIT_DIRS ?= $(sort $(dir $(wildcard tests/fixtures/cssweb/*/index.html)))
AUDIT_TOP  ?= 30
NEGDIR := $(BUILD)/cssnegctl
$(BUILD)/css_audit: tests/unit/css_audit.c $(BUILD)/libcss_host.a \
                    c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                    c/apps/browser/css_extra.c c/apps/browser/layout.c \
                    c/apps/browser/browser_paint.c $(HTML_PARSER_SRC)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ tests/unit/css_audit.c \
	    c/apps/browser/css_engine.c c/apps/browser/css_vars.c c/apps/browser/css_extra.c \
	    c/apps/browser/layout.c c/apps/browser/browser_paint.c \
	    $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a

audit-css: $(BUILD)/css_audit
	@$(BUILD)/css_audit --top=$(AUDIT_TOP) $(AUDIT_DIRS)

# --- audit-css-before: the same corpus with this work DISABLED -------------
# The before half of the before/after, built from the SAME sources with only
# the three changes turned off -- the css_vars whole-value invariant, the
# conditional group rules, and the nested-at-rule dispatch. Building it this
# way rather than from an old checkout is what makes the two columns
# comparable: same corpus, same viewport, same binary shape, one variable.
#   make audit-css-before | head -22   # then diff against `make audit-css`
audit-css-before: $(BUILD)/libcss_host.a $(NEGDIR)/css_audit_before
	@$(NEGDIR)/css_audit_before --top=$(AUDIT_TOP) $(AUDIT_DIRS)

$(NEGDIR)/css_audit_before: tests/unit/css_audit.c c/apps/browser/css_vars.c \
                            third_party/css/libcss/src/parse/language.c \
                            third_party/css/libcss/src/parse/parse.c \
                            $(BUILD)/libcss_host.a
	@mkdir -p $(NEGDIR)/libcss/parse
	@sed -e 's/#define VVAL_MAX 8192/#define VVAL_MAX 192/' \
	     -e 's|char \*dst = (vlen < VVAL_MAX \&\& text_balanced(val, vlen))|if (vlen >= VVAL_MAX) vlen = VVAL_MAX - 1;\n    char *dst = (1)|' \
	     -e 's/^static int text_balanced(const char \*s, int n)$$/static int text_balanced(const char *s, int n) { (void) s; (void) n; return 1; }\nstatic int text_balanced_disabled(const char *s, int n)/' \
	     c/apps/browser/css_vars.c > $(NEGDIR)/css_vars.c
	@sed -e 's/^static bool at_rule_is_group(const css_token \*kw)$$/static bool at_rule_is_group(const css_token *kw) { (void) kw; return false; }\nstatic bool at_rule_is_group_disabled(const css_token *kw)/' \
	     third_party/css/libcss/src/parse/language.c > $(NEGDIR)/libcss/parse/language.c
	@sed -e 's/^static const bool css__nested_atrules = true;$$/static const bool css__nested_atrules = false;/' \
	     third_party/css/libcss/src/parse/parse.c > $(NEGDIR)/libcss/parse/parse.c
	@for f in css_vars.c libcss/parse/language.c libcss/parse/parse.c; do \
	    case $$f in css_vars.c) o=c/apps/browser/css_vars.c;; \
	                *) o=third_party/css/libcss/src/parse/$${f##*/};; esac; \
	    cmp -s $$o $(NEGDIR)/$$f && \
	      { echo "FAIL: audit-css-before patched nothing in $$o"; exit 1; } || true; \
	 done
	@$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) \
	    -c $(NEGDIR)/libcss/parse/language.c -o $(NEGDIR)/language_before.o
	@$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) \
	    -c $(NEGDIR)/libcss/parse/parse.c -o $(NEGDIR)/parse_before.o
	@cp $(BUILD)/libcss_host.a $(NEGDIR)/libcss_before.a
	@ar d $(NEGDIR)/libcss_before.a language.o parse.o
	@ar r $(NEGDIR)/libcss_before.a $(NEGDIR)/language_before.o $(NEGDIR)/parse_before.o
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ tests/unit/css_audit.c \
	    c/apps/browser/css_engine.c $(NEGDIR)/css_vars.c c/apps/browser/css_extra.c \
	    c/apps/browser/layout.c c/apps/browser/browser_paint.c \
	    $(HTML_PARSER_SRC) $(NEGDIR)/libcss_before.a

# --- test-css-modern: the constructs a 2020s stylesheet is written in -------
# @layer / @supports / @container (each of which used to take its whole block
# with it) and the CSS logical properties. See the header of
# tests/unit/css_modern_test.c for the measured reason each one is here.
CSSMODERN_SRC := tests/unit/css_modern_test.c tests/unit/css_hostmm.c c/apps/browser/css_engine.c \
                 c/apps/browser/css_vars.c c/apps/browser/css_extra.c
test-css-modern: $(BUILD)/libcss_host.a
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(BUILD)/css_modern_test \
	    $(CSSMODERN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@$(BUILD)/css_modern_test

# --- test-css-web-negctl: both new gates, disabled, must FAIL ---------------
# Two assertions in this tree would otherwise be unfalsifiable, so each is run
# again against a deliberately broken build of the file it guards:
#
#  1. css_vars.c's "a value that cannot be stored WHOLE is not stored in part".
#     The negative control restores the old fixed 192-byte field and its silent
#     truncation. css_vars_test's balance checks must then fail -- if they pass,
#     they are not testing the invariant, and the apple.com bug (its cascade saw
#     4% of its own declarations) could come back unnoticed.
#
#  2. language.c's conditional-group-rule branch. The negative control disables
#     it so @layer/@supports/@container fall back to `return CSS_INVALID` and
#     take their blocks with them. css_modern_test must then fail.
#
# Both are built from SED'd copies rather than from an #ifdef in the shipping
# file: a knob that lives in the real source is a knob that can be left on.
test-css-web-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(NEGDIR)
	@sed -e 's/#define VVAL_MAX 8192/#define VVAL_MAX 192/' \
	     -e 's|char \*dst = (vlen < VVAL_MAX \&\& text_balanced(val, vlen))|if (vlen >= VVAL_MAX) vlen = VVAL_MAX - 1;\n    char *dst = (1)|' \
	     c/apps/browser/css_vars.c > $(NEGDIR)/css_vars.c
	@cmp -s c/apps/browser/css_vars.c $(NEGDIR)/css_vars.c && \
	    { echo "FAIL: the css_vars negative control patched nothing --"; \
	      echo "      the sed no longer matches the source, so it proves nothing."; exit 1; } || true
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_vars_neg \
	    tests/unit/css_vars_test.c tests/unit/css_hostmm.c $(NEGDIR)/css_vars.c \
	    c/apps/browser/css_engine.c $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@if $(NEGDIR)/css_vars_neg > $(NEGDIR)/vars.txt 2>&1; then \
	    echo "FAIL: with value truncation restored css_vars_test still PASSED --"; \
	    echo "      the balance assertions cannot fail, so they prove nothing."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): truncation restored -> css_vars_test fails, as required"; \
	    grep FAIL $(NEGDIR)/vars.txt | sed 's/^/       /'; \
	 fi
	@mkdir -p $(NEGDIR)/libcss/parse
	@sed -e 's/^static bool at_rule_is_group(const css_token \*kw)$$/static bool at_rule_is_group(const css_token *kw) { (void) kw; return false; }\nstatic bool at_rule_is_group_disabled(const css_token *kw)/' \
	     third_party/css/libcss/src/parse/language.c > $(NEGDIR)/libcss/parse/language.c
	@cmp -s third_party/css/libcss/src/parse/language.c $(NEGDIR)/libcss/parse/language.c && \
	    { echo "FAIL: the at-rule negative control patched nothing."; exit 1; } || true
	@$(CC) -O2 -w -fcommon -D_ALIGNED= -DWITHOUT_ICONV_FILTER $(CSS_INC) \
	    -c $(NEGDIR)/libcss/parse/language.c -o $(NEGDIR)/language_neg.o
	@cp $(BUILD)/libcss_host.a $(NEGDIR)/libcss_neg.a
	@ar d $(NEGDIR)/libcss_neg.a language.o
	@ar r $(NEGDIR)/libcss_neg.a $(NEGDIR)/language_neg.o
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $(NEGDIR)/css_modern_neg \
	    $(CSSMODERN_SRC) $(HTML_PARSER_SRC) $(NEGDIR)/libcss_neg.a
	@if $(NEGDIR)/css_modern_neg > $(NEGDIR)/modern.txt 2>&1; then \
	    echo "FAIL: with the group-rule branch disabled css_modern_test still PASSED --"; \
	    echo "      @layer/@supports/@container are not actually being tested."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): group rules disabled -> css_modern_test fails, as required"; \
	    grep FAIL $(NEGDIR)/modern.txt | sed 's/^/       /'; \
	 fi

.PHONY: audit-css audit-css-before test-css-modern test-css-web-negctl
