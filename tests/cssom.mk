# tests/cssom.mk -- the CSSOM line's measurement.
#
# Own fragment for the reason every other tests/*.mk gives: several agents
# share this tree and a stale whole-file Makefile snapshot has silently deleted
# other people's targets more than once. tests/wpt.mk belongs to the WPT line
# and is not edited from here; this file builds its own runner variant instead.
#
#   make test-cssom            host unit tests for the CSSOM  (the gate)
#   make test-cssom-negctl     the negative control -- MUST fail without the fix
#   make wpt-cssom             the WPT run WITH js_cssom.c linked
#   make wpt-cssom-base        the same run WITHOUT it (the before column)
#   make wpt-cssom-rank        the ranked table of why css/ files die
#
# THE HEADLINE NUMBER IS NOT THE PASS RATE. 1,926 files under css/ never
# complete the harness, so every subtest inside them is outside the
# denominator entirely. A file that starts completing ADDS failing subtests to
# the count, so the raw pass rate can fall while the result is strictly
# better. `make wpt-cssom` prints both columns and the harness-death delta,
# and the delta is the number this line is judged on.

.PHONY: test-cssom test-cssom-negctl wpt-cssom wpt-cssom-base wpt-cssom-rank
.PHONY: cssom-compare

CSSOM_DIR  := $(BUILD)/cssom
CSSOM_SRC  := c/apps/browser/js_cssom.c

# --- the host unit tests ----------------------------------------------------
# Links layout.c so the GEOMETRY is real: a CSSOM-View test against a build
# with no layout would assert that zero equals zero, and would pass over an
# implementation that returns a constant. That is the whole reason this target
# exists separately from the WPT runner, which links no layout (see the note on
# wpt-cssom below). browser_paint.c is deliberately NOT linked: the display
# list is what the geometry reads, and PAINTING it would additionally need the
# recorder in tests/unit/painthost and a canvas this test has no use for.
CSSOM_TEST_SRC := tests/unit/cssom_test.c \
                  c/apps/browser/js_page.c c/apps/browser/js_dom.c \
                  c/apps/browser/js_cssom.c \
                  c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                  c/apps/browser/css_extra.c \
                  c/apps/browser/layout.c \
                  c/apps/browser/js_select.c \
                  c/apps/browser/js_tokenlist.c

$(CSSOM_DIR)/cssom_test: $(CSSOM_TEST_SRC) $(HTML_PARSER_SRC) \
                         $(BUILD)/libcss_host.a
	@mkdir -p $(CSSOM_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	    -DCONFIG_VERSION='"host"' -o $@ $(CSSOM_TEST_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm

test-cssom: $(CSSOM_DIR)/cssom_test
	@$(CSSOM_DIR)/cssom_test

# --- the negative control ---------------------------------------------------
# An assertion nobody has watched fail is not an assertion. Two independent
# sabotages, because the two halves of this line fail in different ways and one
# control would only cover one of them:
#
#   -DCSSOM_NEGCTL_SERIALIZE   the colour serialiser emits `#0000ff` instead
#                              of `rgb(0, 0, 255)` and lengths lose their
#                              `px`. This is the plausible-looking wrong
#                              CSSOM: every property is present, every call
#                              returns a string, nothing throws, and only the
#                              BYTES are wrong -- which is all WPT compares.
#   -DCSSOM_NEGCTL_NOGEOM      the geometry accessors are installed but never
#                              flush layout, so they answer 0 -- which is
#                              exactly what a build with layout unlinked
#                              answers, i.e. the state the WPT runner is in.
#   -DCSSOM_NEGCTL_INKUNION    every box is the SUBTREE INK UNION, borrowed
#                              from the nearest inked ancestor when the
#                              element painted nothing. This is the plausible
#                              geometry: nothing is 0, nothing throws, every
#                              accessor answers a rectangle of the right
#                              order, and every box with children in it is
#                              wrong by their overflow. NOGEOM cannot cover
#                              it -- NOGEOM is the wrong answer that LOOKS
#                              wrong, this is the one that does not.
#
# The target succeeds when the tests FAIL against all three.
CSSOM_NEGS := SERIALIZE NOGEOM INKUNION

test-cssom-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(CSSOM_DIR)
	@bad=0; \
	 for n in $(CSSOM_NEGS); do \
	   $(CC) -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) \
	       -DCONFIG_VERSION='"host"' -DCSSOM_NEGCTL_$$n \
	       -o $(CSSOM_DIR)/negctl_$$n $(CSSOM_TEST_SRC) \
	       $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm || exit 1; \
	   if $(CSSOM_DIR)/negctl_$$n > $(CSSOM_DIR)/negctl_$$n.log 2>&1; then \
	     echo "test-cssom-negctl: FAILED -- the suite PASSED with $$n sabotaged,"; \
	     echo "  so its assertions are not measuring that half."; bad=1; \
	   else \
	     echo "test-cssom-negctl: ok -- $$n sabotaged, suite fails:"; \
	     grep -m3 '^  FAIL' $(CSSOM_DIR)/negctl_$$n.log || true; \
	   fi; \
	 done; \
	 exit $$bad

# --- the WPT runner, with js_cssom.c ----------------------------------------
# The same source list tests/wpt.mk builds, plus this line's file. Built here
# rather than by editing wpt.mk because that file belongs to the WPT line.
#
# IT DOES NOT LINK layout.c, deliberately, and this is the limit on every
# number below. layout.c needs the font backend and a canvas, and the WPT
# runner has neither; with it unlinked every box is 0x0, so a revived
# check-layout file COMPLETES and then FAILS. That is strictly better than
# dying -- a failing subtest is in the denominator and a dead file is not --
# and it is not the same as passing. `make test-cssom` is where the geometry
# is actually measured.
# Deferred (`=`, not `:=`): WPT_TEST_SRC and WPT_CF come from tests/wpt.mk,
# and make reads the fragments in whatever order the Makefile lists them. An
# immediate assignment here captured them EMPTY when wpt.mk had not been read
# yet, and the failure was a compile with no include path at all -- which
# reads as a missing header rather than as a missing variable.
#
# DEDUPED, and the `filter-out` is not defensive tidying. tests/wpt.mk globs
# c/apps/browser/js_*.c into WPT_TEST_SRC, so js_cssom.c is in it now and
# listing it again put the same TU on the link line twice: five "multiple
# definition of js_cssom_install" errors and no runner. The `after` column and
# `make test-wpt` are therefore now the SAME binary's numbers -- which is the
# right outcome and worth saying, because this fragment was written when they
# were not.
CSSOM_WPT_SRC = $(WPT_TEST_SRC) $(filter-out $(WPT_TEST_SRC),$(CSSOM_SRC))
CSSOM_WPT_CF  = $(WPT_CF)

$(CSSOM_DIR)/wpt_cssom: $(CSSOM_WPT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(CSSOM_DIR)
	@$(CC) -O2 -w $(CSSOM_WPT_CF) -o $@ $(CSSOM_WPT_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

CSSOM_ONLY ?=
CSSOM_ARGS  = --root $(WPT_ROOT) -b /dev/null --subset css \
              $(if $(CSSOM_ONLY),--only $(CSSOM_ONLY),)

# The BEFORE column: tests/wpt.mk's own runner, unchanged. Its baseline is
# written to a path of ours -- tests/unit/wpt_expected_fail.txt is the WPT
# line's file and has a known defect they are fixing.
wpt-cssom-base: $(BUILD)/wpt_test
	@mkdir -p $(CSSOM_DIR)
	@$(BUILD)/wpt_test $(CSSOM_ARGS) --report $(CSSOM_DIR)/before.tsv \
	    > $(CSSOM_DIR)/before.log 2>&1 || true
	@tail -3 $(CSSOM_DIR)/before.log | head -1

wpt-cssom: $(CSSOM_DIR)/wpt_cssom
	@mkdir -p $(CSSOM_DIR)
	@$(CSSOM_DIR)/wpt_cssom $(CSSOM_ARGS) --report $(CSSOM_DIR)/after.tsv \
	    > $(CSSOM_DIR)/after.log 2>&1 || true
	@tail -3 $(CSSOM_DIR)/after.log | head -1
	@if [ -f $(CSSOM_DIR)/before.tsv ]; then \
	    python3 tools/cssom_compare.py $(CSSOM_DIR)/before.tsv $(CSSOM_DIR)/after.tsv; \
	 else echo "(run 'make wpt-cssom-base' first for the before column)"; fi

# --- the ranked cause table -------------------------------------------------
# The thing this line was asked for first and the thing that stays useful
# whether or not the code lands: WHY the 1,926 files die, with counts, grouped
# by cause rather than by file.
wpt-cssom-rank: $(CSSOM_DIR)/wpt_cssom
	@mkdir -p $(CSSOM_DIR)
	@test -f $(CSSOM_DIR)/before.tsv || $(MAKE) --no-print-directory wpt-cssom-base
	@python3 tools/cssom_rank.py $(CSSOM_DIR)/before.tsv

# --- the ABI gate: one display list, two flag sets --------------------------
#
# The display list is a raw array of `struct item` shared ACROSS A COMPILATION
# BOUNDARY. layout.c writes it under one set of flags ($(UCFLAGS) $(CSS_INC));
# browser.c, js_dom.c and this line's js_cssom.c read it under another
# ($(BROWSER_JS_CF)). Both sides include the same layout.h, which makes it very
# easy to assume both sides see the same struct.
#
# They did not, and it was a ring-3 page fault on SEVEN of the sixteen
# scoreboard sites on 2026-08-09. The Makefile force-included musl's internal
# features.h into the reading group; features.h defines a lowercase `hidden`;
# `struct item` has a field called `hidden`; so `int hidden;` expanded to
# `int __attribute__((__visibility__("hidden")));`, an anonymous declaration
# that declares no member at all. sizeof(struct item) was 224 on the reading
# side and 232 on the writing side, every it[i] past i == 0 was a misaligned
# slice of its neighbours, and it[i].node came back as 0x14 -- not NULL, so the
# null check in border_box() could not catch it. clang says nothing about any
# of this: both translation units are individually valid C.
#
# The assertion is therefore the property itself -- the two flag sets must
# agree about every struct they share -- and it is taken by COMPILING rather
# than by reading the source, because reading the source is exactly what could
# not see this. `char abi_x[sizeof(struct x)];` becomes `.size abi_x, N` in the
# assembly: no binutils, and no host execution, which matters because these
# objects are cross-compiled for x86_64-elf and can never be asked their
# opinion at run time.
#
# THE NEGATIVE CONTROL RUNS INSIDE THIS TARGET rather than beside it. A control
# that fires only when somebody types its name is precisely the shape
# tools/audit_tests.py exists to find, and `test-.*-negctl$$` is excluded from
# the derived CI suite list by name. The sabotage is the exact flag that caused
# the bug, and the target FAILS if adding it back does not change the answer --
# which would mean this check had quietly stopped being able to see the thing
# it was written for.
#
# Deferred (`=`, not `:=`) for the reason CSSOM_WPT_SRC above is: BROWSER_JS_CF
# is defined in the Makefile and make reads the fragments in whatever order the
# Makefile lists them.
.PHONY: test-cssom-abi

CSSOM_ABI_DIR      := $(BUILD)/cssom-abi
CSSOM_ABI_STRUCTS  := item node image
CSSOM_ABI_WRITER    = $(UCFLAGS) $(CSS_INC)        # layout.c, browser_paint.c
CSSOM_ABI_READER    = $(BROWSER_JS_CF)             # browser.c, js_dom.c, js_cssom.c
CSSOM_ABI_SABOTAGE  = -include features.h          # the flag that did it

# $(1) = extra flags, $(2) = output file. Empty output or a short count is a
# FAILURE, never a pass: two empty files compare equal, and a target that
# reports ok because it measured nothing is the one thing worse than no target.
define cssom_abi_sizes
	$(CC) $(CSSOM_ABI_READER) $(1) -w -S -o - $(CSSOM_ABI_DIR)/probe.c 2>$(CSSOM_ABI_DIR)/cc.log \
	  | sed -n 's/^[[:space:]]*\.size[[:space:]][[:space:]]*abi_\([A-Za-z_]*\),[[:space:]]*\([0-9][0-9]*\).*/\1 \2/p' \
	  | sort > $(2)
endef

test-cssom-abi:
	@mkdir -p $(CSSOM_ABI_DIR)
	@{ echo '#include "layout.h"'; \
	   for s in $(CSSOM_ABI_STRUCTS); do echo "char abi_$$s[sizeof(struct $$s)];"; done; \
	 } > $(CSSOM_ABI_DIR)/probe.c
	@$(CC) $(CSSOM_ABI_WRITER) -w -S -o - $(CSSOM_ABI_DIR)/probe.c 2>$(CSSOM_ABI_DIR)/cc.log \
	  | sed -n 's/^[[:space:]]*\.size[[:space:]][[:space:]]*abi_\([A-Za-z_]*\),[[:space:]]*\([0-9][0-9]*\).*/\1 \2/p' \
	  | sort > $(CSSOM_ABI_DIR)/writer.txt
	@$(call cssom_abi_sizes,,$(CSSOM_ABI_DIR)/reader.txt)
	@$(call cssom_abi_sizes,$(CSSOM_ABI_SABOTAGE),$(CSSOM_ABI_DIR)/sabotaged.txt)
	@n=`echo $(CSSOM_ABI_STRUCTS) | wc -w`; bad=0; \
	 for f in writer reader sabotaged; do \
	   got=`wc -l < $(CSSOM_ABI_DIR)/$$f.txt`; \
	   if [ "$$got" -ne "$$n" ]; then \
	     echo "test-cssom-abi: FAILED -- the $$f compile produced $$got of $$n sizes,"; \
	     echo "  so this target measured nothing. Either it does not compile (see"; \
	     echo "  $(CSSOM_ABI_DIR)/cc.log) or clang stopped emitting '.size abi_x, N'"; \
	     echo "  and the sed above needs updating."; bad=1; \
	   fi; \
	 done; \
	 if [ $$bad -ne 0 ]; then exit 1; fi; \
	 if ! diff -u $(CSSOM_ABI_DIR)/writer.txt $(CSSOM_ABI_DIR)/reader.txt; then \
	   echo "test-cssom-abi: FAILED -- layout.c and the browser's JS/DOM TUs do not"; \
	   echo "  agree about the size of a struct they share through the display list."; \
	   echo "  Left is \$$(UCFLAGS) \$$(CSS_INC) (the writer), right is \$$(BROWSER_JS_CF)"; \
	   echo "  (the readers). A field whose name is also a macro in a force-included"; \
	   echo "  header is how this happened last time -- see the note above."; \
	   exit 1; \
	 fi; \
	 if diff -q $(CSSOM_ABI_DIR)/writer.txt $(CSSOM_ABI_DIR)/sabotaged.txt >/dev/null; then \
	   echo "test-cssom-abi: FAILED -- the built-in control did not fire. Adding"; \
	   echo "  \`$(CSSOM_ABI_SABOTAGE)\` back to the reader changed nothing, so this"; \
	   echo "  target can no longer see the defect it exists for."; \
	   exit 1; \
	 fi; \
	 echo "test-cssom-abi: ok -- `tr '\n' ' ' < $(CSSOM_ABI_DIR)/writer.txt`(writer == readers);"; \
	 echo "  control fired: with $(CSSOM_ABI_SABOTAGE) the readers say `tr '\n' ' ' < $(CSSOM_ABI_DIR)/sabotaged.txt`"

# ---------------------------------------------------------------------------
# THE SETTABLE-PROPERTY SET FROM canon.c, AND THE SETTER THAT REFUSES
#
#   make test-cssd-canon          the gate
#   make test-cssd-canon-negctl   the two controls -- the suite MUST fail
#
# Appended to this fragment rather than given one of its own because the
# question is the CSSOM's: WHICH properties a CSSStyleDeclaration lets a script
# assign to, and what it does with a value none of the parsers can take. A new
# fragment would have needed a line in the Makefile, and that file is the one
# place in this tree where a stale whole-file snapshot has silently deleted
# other lines' work.
#
# LINKS js_dom.c AND NOT js_cssom.c, deliberately. js_cssom.c's setProperty
# wrapper refuses an INVALID declaration as well, so linking it would let this
# suite pass with js_dom.c's own setter doing nothing at all -- and js_dom.c's
# setter is the one the corpus reaches, because the wrapper's named accessors
# cover only the properties LibCSS knows and every property tested here is one
# LibCSS has never heard of.
.PHONY: test-cssd-canon test-cssd-canon-negctl

CSSDCANON_SRC := tests/unit/cssdcanon_test.c c/apps/browser/js_dom.c \
                 c/apps/browser/js_reflect.c c/apps/browser/css_engine.c \
                 c/apps/browser/css_vars.c c/apps/browser/css_interp.c
CSSDCANON_CF   = -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"'

$(CSSOM_DIR)/cssdcanon_test: $(CSSDCANON_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	@mkdir -p $(CSSOM_DIR)
	@$(CC) $(CSSDCANON_CF) -o $@ $(CSSDCANON_SRC) \
	    $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm

test-cssd-canon: $(CSSOM_DIR)/cssdcanon_test
	@$(CSSOM_DIR)/cssdcanon_test

# Three sabotages, because this landed as three changes and one control would
# only cover one of them:
#
#   CSSD_NO_CANON_REFUSE   THE HALF-IMPLEMENTATION, and it is not a straw man:
#                          adopt canon.c's enumeration and leave the setter
#                          alone. That is the obvious way to do this and it is
#                          what the numbers were measured against -- on
#                          css/css-grid/parsing it is +128 valid subtests and
#                          -115 refusals handed back, because a property LibCSS
#                          never heard of had no validity step anywhere and the
#                          "-invalid" files were passing vacuously.
#   CSSD_PROPS_FROM_ENUM   the set taken from the cascade's CSSP_* enum, i.e.
#                          the state before any of this. Shared with
#                          tests/reflect.mk's test-cssprops-negctl, which is
#                          the same switch asking a narrower question.
#   CSSSUP_LIBCSS_ONLY     css_supports_decl answers from LibCSS alone, as it
#                          did before -- so CSS.supports() denies every
#                          property LibCSS never gained, which additionally
#                          makes it unsettable, because the CSSOM setter asks
#                          the same function.
#
# The target succeeds when the suite FAILS against all three, and reports how
# many assertions went red so a control that fires for an unrelated reason (a
# link error, an empty run) is visible rather than counted as success.
CSSDCANON_NEGS := CSSD_NO_CANON_REFUSE CSSD_PROPS_FROM_ENUM CSSSUP_LIBCSS_ONLY

test-cssd-canon-negctl: $(BUILD)/libcss_host.a
	@mkdir -p $(CSSOM_DIR)
	@bad=0; \
	 for n in $(CSSDCANON_NEGS); do \
	   $(CC) $(CSSDCANON_CF) -D$$n -o $(CSSOM_DIR)/cssdcanon_$$n \
	       $(CSSDCANON_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	       $(BUILD)/libcss_host.a -lm || exit 1; \
	   if $(CSSOM_DIR)/cssdcanon_$$n > $(CSSOM_DIR)/cssdcanon_$$n.log 2>&1; then \
	     echo "test-cssd-canon-negctl: FAILED -- the suite PASSED with $$n set,"; \
	     echo "  so its assertions are not measuring that half of the change."; \
	     bad=1; \
	   else \
	     n_red=`grep -c '^  FAIL' $(CSSOM_DIR)/cssdcanon_$$n.log`; \
	     echo "test-cssd-canon-negctl: ok -- $$n set, $$n_red assertions red:"; \
	     grep -m3 '^  FAIL' $(CSSOM_DIR)/cssdcanon_$$n.log | sed 's/^/  /'; \
	   fi; \
	 done; \
	 exit $$bad
