# tests/semantics.mk -- the HTML element interfaces (c/apps/browser/js_semantics.c).
#
#   make test-semantics          the interfaces, host-side, seconds
#   make test-semantics-negctl   the negative control: the SAME binary built
#                                with static collections, which must FAIL
#   make semantics-rank          the ranked cause table for html/semantics,
#                                per directory -- the work order this file
#                                was written from
#
# Own fragment rather than lines in the Makefile, for the reason every other
# fragment gives: several agents edit that file at once and a whole-file write
# loses somebody's work. Two lines is the whole footprint.
#
# ---------------------------------------------------------------------------
# WHERE THIS CAME FROM -- the ranking, not a hunch
# ---------------------------------------------------------------------------
# `make test-wpt SUBSET=html/semantics` is the largest subset in the corpus and
# had had one line touch one corner of it (forms). Running it a directory at a
# time (which is what `semantics-rank` below does, and why it exists as a
# target rather than as a paragraph) produced this, on 2026-08-09, before any
# of the code this fragment tests:
#
#     forms                  690/4269      popovers                 1/2733
#     scripting-1            364/1128      interactive-elements    28/492
#     the-button-element       9/355       document-metadata       16/226
#     links                  135/203       tabular-data            17/157
#     selectors               38/115       sections                 0/107
#     menu                     2/82        grouping-content        47/47
#     text-level-semantics    17/38        permission-element       6/16
#     disabled-elements        3/11        edits                    0/2
#
# and then, grouping each directory's failures by the message they produce, ONE
# mechanism over the top three:
#
#     2100  popover / popovertarget IDL absent   (of popovers' 2732 failures)
#      346  command / commandfor absent          (of the-button-element's 346)
#      466  <dialog> has no show/showModal/close  (interactive-elements)
#      140  <table> has no rows/insertRow/...     (tabular-data)
#       81  ':heading' is an unknown pseudo-class (sections -- ONE line)
#
# Not 3,000 bugs: three APIs and a selector. That is what js_semantics.c is.
#
# ---------------------------------------------------------------------------
# WHY A SEPARATE TEST FROM test-wpt
# ---------------------------------------------------------------------------
# test-wpt is minutes and its unit is "did the corpus pass", which is the right
# question and the wrong feedback loop. This is seconds and its unit is "does
# THIS mechanism hold" -- and, crucially, it asserts the properties the corpus
# is bad at isolating. Liveness is the example: WPT has hundreds of tests that
# would fail against a static collection, but every one of them fails for its
# own stated reason, so the corpus can never tell you "your collections are
# snapshots". A test that inserts a row and re-reads `rows.length` can.

.PHONY: test-semantics test-semantics-negctl semantics-rank

# The link is the SHIPPING browser's JS side, wildcarded, for the reason
# tests/wpt.mk gives at length: a hand-kept list drifted twice in one night and
# the suite went on reporting bugs that were already fixed. browser.c is out --
# it is the app shell and draws through `int 0x80`.
SEM_JS_SRC := $(filter-out c/apps/browser/browser.c,$(sort $(wildcard c/apps/browser/js_*.c)))
SEM_SRC := tests/unit/semantics_test.c $(SEM_JS_SRC) \
           c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
           c/apps/browser/css_extra.c c/apps/browser/layout.c \
           c/apps/browser/forms.c c/apps/browser/focus.c \
           c/net/http/http1.c c/net/http/url.c c/net/http/cookies.c \
           c/lib/image/img.c c/lib/image/gif.c c/lib/image/jpeg.c \
           c/lib/image/svg.c c/lib/image/exif.c \
           $(wildcard c/lib/media/*.c) $(wildcard c/lib/video/*.c) \
           $(wildcard c/lib/audio/*.c) \
           tests/unit/rust_host_shim.c
# -Ic/apps because js_platform.c includes "logit.h" unconditionally; the media
# include dirs because js_media.c drags the demuxer in. Same set as tests/wpt.mk.
SEM_CF := $(BTEST_INC) -Ic/apps -Ic/kernel/mm -Ic/lib/media -Ic/lib/audio \
          -Ic/lib/video $(CSS_INC) $(JS_INC) -Iinclude/abi \
          -DCONFIG_VERSION='"host"' -DWEBAPI_HOST

$(BUILD)/semantics_test: $(SEM_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(SEM_CF) -o $@ $(SEM_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-semantics: $(BUILD)/semantics_test
	@$(BUILD)/semantics_test

# --- the negative control ---------------------------------------------------
# -DSEMANTICS_STATIC_COLLECTIONS keeps every collection, every method and every
# reflected property, and changes ONE thing: a collection's contents are
# computed on first read and cached. That is the plausible wrong
# implementation, not a broken one -- a page that never mutates behaves
# identically, `table.rows.length` is right, `rows[0]` is right, and the bug
# only appears the moment something is inserted. It is deliberately not "delete
# the collections", which would prove only that the test calls them.
#
# The suite's LIVENESS assertions must go red in this build. They are counted
# separately in the test's own output so that a control failing for some
# unrelated reason is visible as such.
test-semantics-negctl: $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(SEM_CF) -DSEMANTICS_STATIC_COLLECTIONS \
	    -o $(BUILD)/semantics_negctl $(SEM_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
	@if $(BUILD)/semantics_negctl > $(BUILD)/semantics_negctl.log 2>&1; then \
	    echo "test-semantics-negctl: FAILED -- the suite PASSED with every"; \
	    echo "  collection frozen at its first read, so nothing in it is"; \
	    echo "  measuring liveness and a snapshot implementation would ship."; \
	    exit 1; \
	 else \
	    echo "test-semantics-negctl: ok -- static collections break the suite:"; \
	    grep '^FAIL' $(BUILD)/semantics_negctl.log | head -6; \
	    tail -2 $(BUILD)/semantics_negctl.log; \
	 fi

# --- semantics-rank: reproduce the table at the top of this file ------------
# Not named test-* on purpose: it measures and cannot fail, which is exactly
# what tools/audit_tests.py looks for. It runs the WPT runner one directory at
# a time because a whole-subset run is long enough that a crash anywhere in it
# costs the entire measurement -- and one did, twice, on the night this was
# written.
SEM_WPT_ROOT ?= third_party/wpt
semantics-rank: $(BUILD)/wpt_test
	@mkdir -p $(BUILD)/sem
	@for d in $(SEM_WPT_ROOT)/html/semantics/*/; do \
	    n=$$(basename $$d); \
	    WEBAPI_FILE_ROOT=$(SEM_WPT_ROOT) $(BUILD)/wpt_test --root $(SEM_WPT_ROOT) \
	        --only html/semantics/$$n/ -b /dev/null \
	        --report $(BUILD)/sem/$$n.tsv > $(BUILD)/sem/$$n.log 2>&1 || true; \
	    printf '  %-24s %s\n' "$$n" \
	        "$$(grep -h 'subtests passed' $(BUILD)/sem/$$n.log | head -1 | sed 's/^WPT: //')"; \
	 done
	@echo ""
	@echo "  top causes, by the message the failure produces:"
	@cat $(BUILD)/sem/*.tsv 2>/dev/null | awk -F'\t' \
	    '$$1!="PASS" && $$1!="#status" {print $$4}' \
	  | sed -E 's/"[^"]*"/"S"/g; s/[0-9]+/N/g' | cut -c1-70 \
	  | sort | uniq -c | sort -rn | head -20 | sed 's/^/    /'
