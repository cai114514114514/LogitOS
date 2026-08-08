# tests/html5lib_negctl.mk -- the negative control for the HTML tree builder.
#
# test-html5lib prints 94.8% and test-html5lib-tok prints 100.00%. Neither
# number, on its own, establishes that the corpus would NOTICE the parser being
# wrong. An expected-failure list is a ratchet against regression; it is not
# evidence that the assertions behind it are load-bearing. The only way to know
# a test would fail is to watch it fail.
#
# So: rebuild the parser with ONE thing replaced -- the adoption agency
# algorithm, swapped for the naive "close the most recent matching tag" (see
# HTML_AAA_NAIVE in c/apps/browser/html_tree.c) -- and REQUIRE the suite to go
# red. If it stays green, test-html5lib is not measuring tree construction and
# the rate it prints means less than it appears to.
#
# Why that stub and not some other damage. It is the specific wrong parser that
# is hardest to notice: it agrees with the real algorithm on every well-nested
# document, diverges only on misnesting, and when it diverges it produces a
# plausible tree rather than a broken one. Deleting a function or corrupting a
# pointer would fail the suite for reasons that prove nothing -- a crash is not
# a conformance signal. This fails, if it fails, on the tree.
#
#   make test-html5lib-negctl        the control: PASSES when the stub FAILS
#   make test-html5lib-negctl V=20   ... and show what the stub got wrong
#
# Own fragment rather than lines in the Makefile, for the reason the other
# fragments give: concurrent agents overwrite that file wholesale.

.PHONY: test-html5lib-negctl

H5NEG_SRC := build/negctl/html_tree.c c/apps/browser/dom.c \
             c/apps/browser/html_tokenizer.c c/apps/browser/dom_serialize.c

# The stub is compiled from a COPY, not by adding a define to the normal build,
# so a stale object from the real build can never be linked in by accident --
# the control failing open is exactly the outcome it exists to rule out.
build/negctl/html_tree.c: c/apps/browser/html_tree.c
	@mkdir -p build/negctl
	@cp $< $@

test-html5lib-negctl: $(BUILD)/libcss_host.a build/negctl/html_tree.c
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DHTML_AAA_NAIVE $(BTEST_INC) $(CSS_INC) \
	    -o $(BUILD)/html5lib_negctl tests/unit/html5lib_test.c \
	    $(H5NEG_SRC) $(BUILD)/libcss_host.a
	@if $(BUILD)/html5lib_negctl third_party/html5lib-tests/tree-construction \
	       -b tests/unit/html5lib_expected_fail.txt --strict \
	       > $(BUILD)/html5lib_negctl.log 2>&1; then \
	    echo "test-html5lib-negctl: FAILED -- the corpus PASSED with the adoption"; \
	    echo "  agency algorithm replaced by 'close the most recent matching tag'."; \
	    echo "  test-html5lib is therefore not measuring tree construction."; \
	    exit 1; \
	 else \
	    echo "test-html5lib-negctl: ok -- the corpus fails without the real algorithm:"; \
	    grep 'NEW FAILURE' $(BUILD)/html5lib_negctl.log | head -$(if $(V),$(V),6); \
	    grep -E 'html5lib tree-construction:' $(BUILD)/html5lib_negctl.log; \
	 fi
