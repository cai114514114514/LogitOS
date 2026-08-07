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
           c/lib/text/otlayout.c c/lib/text/ttf.c c/lib/text/cff.c \
           c/lib/text/utf8.c

.PHONY: test-bidi test-bidi-negctl test-shape test-shape-negctl \
        test-text regen-bidi-tables

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
# The corpus also states, by hand, the segmentation each line must produce:
# script, direction, start and length, in VISUAL order. The generator refuses a
# corpus whose runs do not tile the text exactly, and shape_test checks our
# bidi + segmentation against that statement before it looks at a single glyph.
# That matters because a mixed LTR/RTL line can have every glyph right and
# still be in the wrong order.
$(BUILD)/shape_expect.h: tests/unit/shape_hb_gen.py tests/unit/shape_corpus.txt
	@mkdir -p $(BUILD)
	@$(HBPY) tests/unit/shape_hb_gen.py --font $(SHAPEFONT) \
	    --corpus tests/unit/shape_corpus.txt --out $@

test-shape: $(BUILD)/shape_expect.h
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/shape_test tests/unit/shape_test.c \
	    $(TEXTLIB) -Ic/lib/text -I$(BUILD)
	@$(BUILD)/shape_test $(SHAPEFONT)

# Negative control for the shaping half: SHAPE_NEGATIVE_CONTROL makes the
# shaper do what text.c did before -- one glyph per code point straight out of
# cmap, advances summed, no GSUB and no GPOS. Arabic then comes out as
# disconnected isolated letters and every kerned pair is a pixel or two wide,
# which is exactly what the HarfBuzz comparison must catch.
test-shape-negctl: $(BUILD)/shape_expect.h
	@$(CC) -O2 -w -DSHAPE_NEGATIVE_CONTROL -o $(BUILD)/shape_test_negctl \
	    tests/unit/shape_test.c $(TEXTLIB) -Ic/lib/text -I$(BUILD)
	@if $(BUILD)/shape_test_negctl $(SHAPEFONT) >$(BUILD)/shape_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: HarfBuzz agrees with an unshaped cmap walk"; \
	    exit 1; \
	 else \
	    echo "negative control ok: without GSUB/GPOS the HarfBuzz differential reports"; \
	    grep -E '^(shape_test:|  FAIL|    [0-9]+ of)' $(BUILD)/shape_negctl.log \
	        | tail -8 | sed 's/^/      /'; \
	 fi

test-text: test-bidi test-bidi-negctl test-shape test-shape-negctl
	@echo "test-text: ALL PASS"

# Rebuild the Unicode property tables from the host UCD. Not part of a normal
# build: the .inc files are committed, exactly like roots_bundle.inc.
regen-bidi-tables:
	@$(HBPY) tests/unit/bidi_gen.py --ucd $(UCD) --out c/lib/text
