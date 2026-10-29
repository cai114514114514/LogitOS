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

# ---- HOST CAPABILITY: are the two external oracles here at all? ------------
#
# Both suites are differential against something this repository does not
# ship, on purpose (see the header). On a host without them -- every macOS
# checkout, the host CLAUDE.md documents -- they used to FAIL: test-bidi on
# "corpus too small (2)", test-shape on `/tmp/hbvenv/bin/python3: No such file`.
# A gate that fails for a reason unrelated to the code under test is noise
# (CLAUDE.md rule 5), and test-bidi-negctl was worse: its binary exits nonzero
# because it cannot OPEN BidiTest.txt, and the recipe read that as "negative
# control ok" -- a control passing for the wrong reason.
#
# $(wildcard) rather than a $(shell) probe: this fragment is parsed on every
# make, and existence is all these can check without running the oracle.
BIDI_ORACLE_OK  := $(and $(wildcard $(UCD)/BidiTest.txt),$(wildcard $(UCD)/BidiCharacterTest.txt))
SHAPE_ORACLE_OK := $(and $(wildcard $(HBPY)),$(wildcard $(SHAPEFONT)))
BIDI_SKIP_TEXT  = $@ SKIPPED: no Unicode conformance corpus at UCD=$(UCD).\n\
  bidi resolution is UNMEASURED on this host, not passing.\n\
  Needs BidiTest.txt and BidiCharacterTest.txt from the SAME UCD release\n\
  c/lib/text/bidi_data.inc was generated from (that file does not record\n\
  which), e.g. https://www.unicode.org/Public/UCD/latest/ucd/ -- then\n\
      make $@ UCD=<directory holding both files>\n
SHAPE_SKIP_TEXT = $@ SKIPPED: the HarfBuzz oracle is not here\n\
  (HBPY=$(HBPY), SHAPEFONT=$(SHAPEFONT)).\n\
  Shaping is UNMEASURED on this host, not passing. To settle it:\n\
      python3 -m venv /tmp/hbvenv && /tmp/hbvenv/bin/pip install uharfbuzz\n\
      make $@ SHAPEFONT=third_party/fonts/DejaVuSans.ttf\n\
  (any font works if both sides use it: the expected header is generated\n\
  from the same SHAPEFONT the shaper is run against).\n

.PHONY: test-bidi test-bidi-negctl test-shape test-shape-negctl \
        test-shape-device test-text regen-bidi-tables

# --- bidi: UAX #9 against the Unicode conformance corpora -------------------
test-bidi:
ifneq ($(BIDI_ORACLE_OK),)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/bidi_test tests/unit/bidi_test.c \
	    c/lib/text/bidi.c -Ic/lib/text
	@$(BUILD)/bidi_test $(UCD)
else
	@printf '$(BIDI_SKIP_TEXT)'
endif

# The negative control for the bidi half: replace the resolver with what
# LogitOS did before this line existed (every level 0, visual order = logical
# order) and require the corpus to reject it. An "it rendered" assertion cannot
# tell those apart; 800k conformance cases can.
test-bidi-negctl:
ifneq ($(BIDI_ORACLE_OK),)
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
else
	@printf '$(BIDI_SKIP_TEXT)'
endif

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
	    --corpus tests/unit/shape_corpus.txt --out $@ \
	    --nogpos $(BUILD)/shape_nogpos.ttf

test-shape: $(if $(SHAPE_ORACLE_OK),$(BUILD)/shape_expect.h)
ifneq ($(SHAPE_ORACLE_OK),)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/shape_test tests/unit/shape_test.c \
	    $(TEXTLIB) -Ic/lib/text -I$(BUILD)
	@$(BUILD)/shape_test $(SHAPEFONT)
else
	@printf '$(SHAPE_SKIP_TEXT)'
endif

# Negative control for the shaping half: SHAPE_NEGATIVE_CONTROL makes the
# shaper do what text.c did before -- one glyph per code point straight out of
# cmap, advances summed, no GSUB and no GPOS. Arabic then comes out as
# disconnected isolated letters and every kerned pair is a pixel or two wide,
# which is exactly what the HarfBuzz comparison must catch.
test-shape-negctl: $(if $(SHAPE_ORACLE_OK),$(BUILD)/shape_expect.h)
ifneq ($(SHAPE_ORACLE_OK),)
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
else
	@printf '$(SHAPE_SKIP_TEXT)'
endif

# --- shaping on the device --------------------------------------------------
# The host differential proves the glyph ids are right. This proves the pixels
# reach the screen: it boots the ISO, opens /shaping.txt in TextEdit, and
# MEASURES the screenshot -- Arabic letters that join project onto the x axis as
# a handful of connected ink runs, isolated letters as one run per letter.
#
# Its negative control is a kernel build, not a second binary, so it is a
# command rather than a target (CFLAGS changes need a rebuild, and a target that
# quietly rebuilt the tree with different flags would leave it that way):
#
#     make clean && make NOSHAPE=1 && make test-shape-device
#
# which reports 7/10/11/6 ink groups where shaped text gives 3/4/4/2, and every
# Arabic word about half again as wide. Put the tree back with
# `make clean && make` afterwards.
test-shape-device: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_shape.py $(ISO) $(DISK) $(BUILD)/shape_device.ppm

test-text: test-bidi test-bidi-negctl test-shape test-shape-negctl
	@if [ -n "$(BIDI_ORACLE_OK)" ] && [ -n "$(SHAPE_ORACLE_OK)" ]; then \
	    echo "test-text: ALL PASS"; \
	 else \
	    echo "test-text: NOT all pass --$(if $(BIDI_ORACLE_OK),, bidi SKIPPED)$(if $(SHAPE_ORACLE_OK),, shape SKIPPED) (see above); what ran passed"; \
	 fi

# Rebuild the Unicode property tables from the host UCD. Not part of a normal
# build: the .inc files are committed, exactly like roots_bundle.inc.
regen-bidi-tables:
	@$(HBPY) tests/unit/bidi_gen.py --ucd $(UCD) --out c/lib/text
