# tests/csstyle.mk -- the computed-style FLUSH.
#
# Own fragment for the reason every other tests/*.mk gives: several agents
# share this tree, and a whole-file Makefile snapshot written minutes after it
# was read has silently deleted other people's targets more than once.
# tests/wpt.mk and tests/cssom.mk belong to other lines and are not edited
# from here.
#
#   make test-csstyle          the gate: computed values on a document that no
#                              embedder ever cascaded
#   make test-csstyle-negctl   the two negative controls -- the suite MUST fail
#                              against both
#
# WHAT THIS LINE FOUND, since the number is the useful part whether or not the
# code is. The premise handed to it was that getComputedStyle answers for 63 of
# LibCSS's 239 computed accessors, so ~176 properties have no getter. The
# corpus disagreed. Of the 10,196 css/ subtests failing with the signature
# `but got ""`, the ones whose property can be named from the subtest title
# split 4,680 / 446 -- and the 4,680 read a property this engine ALREADY
# resolves. A probe settled it:
#
#     display= color= float= fs=       ('float' in gCS == true, length == 62)
#
# `display` on a plain <div> is about as wired as a property gets. It answered
# "" because node->computed was NULL for every node in the document: css_apply
# is called from browser.c's render loop and from nowhere else, so any embedder
# that is not that loop -- the host WPT runner is one -- reads back a document
# that was never styled. ONE missing call, not 176 missing getters, and
# building the 176 would have gained nothing.

.PHONY: test-csstyle test-csstyle-negctl

CSSTYLE_DIR := $(BUILD)/csstyle

# The same link cssom.mk uses, and layout.c is in it for the same reason it is
# there: js_cssom.c's geometry reads the display list, and leaving layout out
# would make those accessors answer 0 rather than link-fail -- a silent hole
# rather than a build error. Nothing in THIS suite asserts on geometry; the
# link is here so the binary is the same shape as the one cssom.mk measures.
CSSTYLE_SRC := tests/unit/csstyle_test.c \
               c/apps/browser/js_page.c c/apps/browser/js_dom.c \
               c/apps/browser/js_cssom.c \
               c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
               c/apps/browser/css_extra.c \
               c/apps/browser/layout.c \
               c/apps/browser/js_select.c \
               c/apps/browser/js_tokenlist.c

# $1 = output binary, $2 = extra -D flags
define CSSTYLE_BUILD
	@mkdir -p $(CSSTYLE_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	    -DCONFIG_VERSION='"host"' $(2) -o $(1) $(CSSTYLE_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
endef

$(CSSTYLE_DIR)/csstyle_test: $(CSSTYLE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call CSSTYLE_BUILD,$@,)

$(CSSTYLE_DIR)/csstyle_negctl_ser: $(CSSTYLE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call CSSTYLE_BUILD,$@,-DCSSOM_NEGCTL_SERIALIZE)

$(CSSTYLE_DIR)/csstyle_negctl_noflush: $(CSSTYLE_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call CSSTYLE_BUILD,$@,-DCSS_NEGCTL_NOFLUSH)

test-csstyle: $(CSSTYLE_DIR)/csstyle_test
	@$(CSSTYLE_DIR)/csstyle_test

# THE CONTROLS. Two, because the two ways this line can be wrong fail
# differently and one control would only cover one of them.
#
#   SERIALIZE  is the one that matters, and it is deliberately NOT "remove the
#              getters". The flush is intact, the cascade runs, every property
#              answers a non-empty string and nothing throws -- colours come
#              back as `#090807` instead of `rgb(9, 8, 7)` and lengths as `20`
#              instead of `20px`. That is what a careful-looking implementation
#              gets wrong, and bytes are all WPT compares. A suite that only
#              checked "not empty" would sail straight past it, which is
#              exactly why this target exists.
#   NOFLUSH    pins WHICH mechanism produced the values: css_ensure_styled
#              returns immediately and every read is "" again. The weaker
#              control, kept because without it a suite could be passing on
#              values some other code path happened to leave behind.
#
# The target SUCCEEDS when the suite FAILS against both. tools/audit_tests.py
# counts harnesses that print FAIL and exit 0; this must not become the 23rd.
test-csstyle-negctl: $(CSSTYLE_DIR)/csstyle_negctl_ser $(CSSTYLE_DIR)/csstyle_negctl_noflush
	@fail=0; \
	for c in csstyle_negctl_ser csstyle_negctl_noflush; do \
	    if $(CSSTYLE_DIR)/$$c > $(CSSTYLE_DIR)/$$c.log 2>&1; then \
	        echo "test-csstyle-negctl: FAIL -- the suite PASSED against $$c."; \
	        echo "  The control is supposed to break it. Either the sabotage no"; \
	        echo "  longer reaches the code under test, or the suite is not"; \
	        echo "  asserting on what it claims to assert on."; \
	        fail=1; \
	    else \
	        echo "  ok   the suite fails against $$c"; \
	        grep -c '^  FAIL' $(CSSTYLE_DIR)/$$c.log \
	            | sed 's/^/       broken checks: /'; \
	    fi; \
	done; \
	if [ $$fail -ne 0 ]; then exit 1; fi; \
	echo "test-csstyle-negctl: ok -- the suite detects both sabotages"
