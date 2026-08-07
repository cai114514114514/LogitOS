# Text shaping and bidirectional text test targets.
#
# Kept in its own fragment (like tests/nic.mk, tests/audio.mk, tests/usb.mk) so
# it could be added while several agents were editing the Makefile.
#
# Sources of truth, both external and both differential -- neither of these
# suites contains a case we invented:
#   test-bidi   the Unicode Character Database's own BidiTest.txt and
#               BidiCharacterTest.txt conformance corpora
#   test-shape  HarfBuzz, shaping the same strings with the same font, compared
#               glyph id by glyph id and position by position

UCD    ?= /usr/share/unicode
HBPY   ?= /tmp/hbvenv/bin/python3
SHAPEFONT ?= /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf

TEXTLIB := c/lib/text/bidi.c c/lib/text/script.c c/lib/text/shape.c \
           c/lib/text/otlayout.c c/lib/text/ttf.c c/lib/text/cff.c

.PHONY: test-bidi test-bidi-negctl test-shape test-shape-negctl \
        test-shape-hb test-text regen-bidi-tables shape-preflight

# --- bidi: UAX #9 against the Unicode conformance corpora -------------------
test-bidi:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/bidi_test tests/unit/bidi_test.c \
	    c/lib/text/bidi.c -Ic/lib/text
	@$(BUILD)/bidi_test $(UCD)

# The negative control for the bidi half: replace the resolver with what
# LogitOS did before this line existed (every level 0, visual order = logical
# order) and require the corpus to reject it. An "it rendered" assertion cannot
# tell those apart; 800k conformance cases can.
test-bidi-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DBIDI_NEGATIVE_CONTROL -o $(BUILD)/bidi_test_negctl \
	    tests/unit/bidi_test.c c/lib/text/bidi.c -Ic/lib/text
	@if $(BUILD)/bidi_test_negctl $(UCD) >$(BUILD)/bidi_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the corpus passes without any bidi resolution"; \
	    exit 1; \
	 else \
	    echo "negative control ok: without bidi resolution the corpus reports"; \
	    grep -E '^  (levels|reorder)' $(BUILD)/bidi_negctl.log | sed 's/^/      /'; \
	 fi

# --- shaping: differential against HarfBuzz ---------------------------------
# tests/unit/shape_hb_gen.py drives HarfBuzz over a corpus of strings and emits
# the expected glyph/position sequences as a C header; shape_test.c then shapes
# the same strings with our shaper and compares. The header is generated into
# $(BUILD) rather than committed, so a HarfBuzz upgrade cannot silently become
# "our expected output".
#
# NONE OF THAT EXISTS YET. The line building it landed the bidi half -- which is
# real and passes 861,948 UCD conformance cases -- and was cut off before
# writing the shaper. These four files are named by the rules below and are
# absent from the tree:
#
#     c/lib/text/script.c        c/lib/text/shape.c
#     tests/unit/shape_test.c    tests/unit/shape_hb_gen.py
#
# The guard below says so, because plain make says "No rule to make target
# 'c/lib/text/script.c'", which reads like a build system fault rather than
# unfinished work. The rules are kept rather than deleted: the design is sound
# and the differential-against-HarfBuzz shape is the right one to finish into.
SHAPE_MISSING := $(strip $(foreach f,$(TEXTLIB) tests/unit/shape_test.c \
                                     tests/unit/shape_hb_gen.py,\
                           $(if $(wildcard $(f)),,$(f))))

shape-preflight:
	@if [ -n "$(SHAPE_MISSING)" ]; then \
	    echo "test-shape: the shaper is NOT IMPLEMENTED -- these are missing:"; \
	    for f in $(SHAPE_MISSING); do echo "    $$f"; done; \
	    echo "  What DOES exist and is verified: c/lib/text/bidi.c -- run 'make test-bidi'"; \
	    echo "  (861948/861948 UCD conformance cases, levels and reordering)."; \
	    echo "  Arabic and Hebrew therefore come out in the right ORDER but the"; \
	    echo "  wrong GLYPHS: no contextual forms, no ligatures, no kerning."; \
	    exit 1; \
	 fi
$(BUILD)/shape_expect.h: tests/unit/shape_hb_gen.py tests/unit/shape_corpus.txt
	@mkdir -p $(BUILD)
	@$(HBPY) tests/unit/shape_hb_gen.py --font $(SHAPEFONT) \
	    --corpus tests/unit/shape_corpus.txt --out $@

# The generated header is built by a sub-make from inside the recipe rather
# than named as a prerequisite. make resolves prerequisites before it runs
# anything, so a missing source aborts with its own message before
# shape-preflight ever gets to explain what is actually going on.
test-shape: shape-preflight
	@$(MAKE) --no-print-directory $(BUILD)/shape_expect.h
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/shape_test tests/unit/shape_test.c \
	    $(TEXTLIB) -Ic/lib/text -I$(BUILD)
	@$(BUILD)/shape_test $(SHAPEFONT)

# Negative control for the shaping half: SHAPE_NEGATIVE_CONTROL makes the
# shaper do what text.c did before -- one glyph per code point straight out of
# cmap, advances summed, no GSUB and no GPOS. Arabic then comes out as
# disconnected isolated letters and every kerned pair is a pixel or two wide,
# which is exactly what the HarfBuzz comparison must catch.
test-shape-negctl: shape-preflight
	@$(MAKE) --no-print-directory $(BUILD)/shape_expect.h
	@$(CC) -O2 -w -DSHAPE_NEGATIVE_CONTROL -o $(BUILD)/shape_test_negctl \
	    tests/unit/shape_test.c $(TEXTLIB) -Ic/lib/text -I$(BUILD)
	@if $(BUILD)/shape_test_negctl $(SHAPEFONT) >$(BUILD)/shape_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: HarfBuzz agrees with an unshaped cmap walk"; \
	    exit 1; \
	 else \
	    echo "negative control ok: without GSUB/GPOS the HarfBuzz differential reports"; \
	    grep -E '^(shape_test:|  )' $(BUILD)/shape_negctl.log | tail -6 | sed 's/^/      /'; \
	 fi

test-text: test-bidi test-shape
	@echo "test-text: ALL PASS"

# Rebuild the Unicode property tables from the host UCD. Not part of a normal
# build: the .inc files are committed, exactly like roots_bundle.inc.
regen-bidi-tables:
	@$(HBPY) tests/unit/bidi_gen.py --ucd $(UCD) --out c/lib/text
