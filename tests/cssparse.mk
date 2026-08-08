# tests/cssparse.mk -- the CSS specified-value parser and its canonical
# serialization (third_party/css/libcss/src/parse/canon.c).
#
#   make test-cssparse         the suite: parse, reject, serialize, round-trip
#   make test-cssparse-asan    the same under ASan+UBSan -- this is the one the
#                              fuzz table is FOR; without it "did not crash"
#                              means "did not crash noisily"
#   make test-cssparse-negctl  THE NEGATIVE CONTROL. A build that parses every
#                              value correctly and spells it plausibly rather
#                              than canonically. Required to FAIL.
#
# WHY THIS IS NOT JUST test-wpt.
#
# The corpus reaches this grammar only through `el.style.foo = x`, and that is
# still a verbatim text store in c/apps/browser/js_dom.c -- so every number
# test-wpt prints about the css/*-parse-valid files today is a number about a
# STORE, not about a parser. This suite asks the parser directly, which is what
# makes it able to go red on a parser bug on the day the bug lands rather than
# on the day the CSSOM is wired to it.
#
# THE COMPILE LINE IS DELIBERATELY MINIMAL, and that is an assertion rather
# than tidiness: canon.c has no dependency on anything in LibCSS -- not the
# lexer, not parserutils, not libwapcaplet -- and the only -I here is its own
# public header. If someone gives it one, this target stops building, which is
# the point at which to argue about it. It is also what keeps the suite at a
# second flat instead of linking the 319-file engine.
#
# Own fragment rather than lines in the Makefile, for the reason every other
# fragment here gives: several lines share this tree and a whole-file Makefile
# write from a stale working copy has silently deleted other people's targets
# more than once. The only token this needs in the shared Makefile is its
# `-include`.

.PHONY: test-cssparse test-cssparse-asan test-cssparse-negctl

CSSPARSE_SRC := tests/unit/cssparse_test.c \
                third_party/css/libcss/src/parse/canon.c
CSSPARSE_INC := -Ithird_party/css/libcss/include

test-cssparse:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-comment \
	    $(CSSPARSE_INC) -o $(BUILD)/cssparse_test $(CSSPARSE_SRC) -lm
	@$(BUILD)/cssparse_test

# The fuzz table in the suite feeds this parser unterminated strings, lone
# backslashes, truncated UTF-8 and 200-deep nesting. Run without a sanitizer
# that is a test that the process survived, which is a much weaker claim than
# the one being made -- the first ASan run found a heap overflow on a trailing
# `\` that the un-sanitized build passed cleanly. Kept as its own target
# because the plain build is what CI runs a hundred times and this is what
# makes the fuzz table mean something.
test-cssparse-asan:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -Wall -Wno-comment $(CSSPARSE_INC) \
	    -o $(BUILD)/cssparse_asan $(CSSPARSE_SRC) -lm
	@$(BUILD)/cssparse_asan
	@echo "test-cssparse-asan: ok -- clean under ASan+UBSan"

# --- THE NEGATIVE CONTROL ---------------------------------------------------
#
# Not "remove the property". Deleting the anchor grammar would make the suite
# go red for the least interesting reason available: values stop being
# accepted, everything throws, and a completely broken harness would "catch" it
# just as well.
#
# -DCANON_NEGCTL instead attacks SERIALIZATION while leaving parsing intact.
# The build accepts exactly the same values, rejects exactly the same invalid
# ones, and then spells the result plausibly rather than canonically. FIVE
# sabotages, each of which reads as a defensible choice in isolation:
#
#   - the anchor operands come out in the order the author wrote them (the
#     obvious choice, and the wrong one -- the spec fixes name-first)
#   - a comma separator loses its space, `a,b` rather than `a, b`
#   - the typed zero stays untyped, `0` rather than `0px`
#   - an sRGB channel is TRUNCATED rather than rounded half-up. Worth reading
#     twice: it is wrong only when a channel lands exactly on .5, the corpus
#     contains such a case on purpose (hwb(320deg 30% 40%) has a blue channel
#     of precisely 127.5), and every other colour comes out identical. A
#     serializer with this bug renders every page correctly.
#   - `shorter hue` is kept in a color-mix() rather than dropped as the
#     default. Also invisible: the value means the same thing either way.
#
# Nothing throws and nothing is missing.
#
# That is the exact failure a careful-looking implementation ships with, and
# the only thing that catches it is comparing BYTES -- which is what WPT does
# and therefore what this suite has to do. A suite that only asked "was it
# accepted" or "does it round-trip through itself" would stay green on this
# build; both of those hold under the control.
test-cssparse-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DCANON_NEGCTL $(CSSPARSE_INC) \
	    -o $(BUILD)/cssparse_negctl $(CSSPARSE_SRC) -lm
	@if $(BUILD)/cssparse_negctl > $(BUILD)/cssparse_negctl.log 2>&1; then \
	    echo "test-cssparse-negctl: FAILED -- the suite passed against a build"; \
	    echo "  that serializes anchor operands in source order, drops the space"; \
	    echo "  after a comma and leaves the typed zero untyped. It is therefore"; \
	    echo "  not comparing serializations at all, and every green run of"; \
	    echo "  test-cssparse means nothing."; exit 1; \
	 else \
	    echo "test-cssparse-negctl: ok -- the suite catches a plausible but"; \
	    echo "  non-canonical serializer. First findings:"; \
	    grep -A2 'FAIL' $(BUILD)/cssparse_negctl.log | head -9 | sed 's/^/    /'; \
	    grep -E '^cssparse: [0-9]+ passed' $(BUILD)/cssparse_negctl.log \
	        | sed 's/^/    /'; \
	 fi

# THE ROUNDING SABOTAGE ON ITS OWN, because a compound control proves only
# that the compound is caught -- and the compound above changes commas, which
# every colour in the suite contains, so it would go red even if the digits
# were never compared. -DCANON_NEGCTL_ROUND changes ONE character of
# behaviour, floor() instead of floor(+0.5) on an sRGB channel, and leaves
# every comma, keyword order and typed zero exactly as the real build spells
# them. If this target ever goes green, the suite has stopped checking the
# digits and is only checking the punctuation.
.PHONY: test-cssparse-round-negctl
test-cssparse-round-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DCANON_NEGCTL_ROUND $(CSSPARSE_INC) \
	    -o $(BUILD)/cssparse_rneg $(CSSPARSE_SRC) -lm
	@if $(BUILD)/cssparse_rneg > $(BUILD)/cssparse_rneg.log 2>&1; then \
	    echo "test-cssparse-round-negctl: FAILED -- the suite passed against a"; \
	    echo "  build that truncates an sRGB channel instead of rounding it."; \
	    echo "  Every colour whose channel lands on .5 is then off by one and"; \
	    echo "  nothing notices."; exit 1; \
	 else \
	    echo "test-cssparse-round-negctl: ok -- caught by:"; \
	    grep -A2 'FAIL' $(BUILD)/cssparse_rneg.log | head -6 | sed 's/^/    /'; \
	 fi

# Wired into the aggregate the audit reads, the same way tests/url.mk does it.
# tools/audit_tests.py calls a test- target "unwired" unless it is reachable as
# a PREREQUISITE from one of the named suites, and separately derives the CI
# host list from each recipe -- so this line covers the audit and the
# classification covers `make ci`. 217 targets were once orphans for want of
# exactly this.
ci-host: test-cssparse test-cssparse-asan test-cssparse-negctl \
         test-cssparse-round-negctl
