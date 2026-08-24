# tests/ime.mk -- the Pinyin IME engine (c/lib/ime/pinyin.c).
#
# In its own fragment for the reason tests/virtio_rng.mk and tests/canvas.mk
# both give: several agents edit the top-level Makefile at once, and a
# fragment is the only way to add targets without a commit sweeping up
# somebody else's half-finished work. The Makefile pulls this in with one
# line (report it, do not add it -- CLAUDE.md reserves the Makefile itself):
#
#     -include tests/ime.mk
#
# HOST ONLY. pinyin.c has no kernel/vfs/wm dependency at all (freestanding,
# like c/lib/gfx) -- tests/unit/ime_test.c links it directly and loads the
# real shipped fsroot/ime/pinyin.dat off disk exactly as tests/unit/ttf_test.c
# loads the real shipped font. No QEMU boot is needed to test this file, so
# there is no `-os` counterpart here yet.
.PHONY: test-ime test-ime-negctl

IME_CF := -O2 -g -Wall -Wextra -Ic/lib/ime -Iinclude/abi

IME_SRC := tests/unit/ime_test.c c/lib/ime/pinyin.c

$(BUILD)/ime_test: $(IME_SRC) c/lib/ime/pinyin.h c/lib/ime/pinyin_fmt.h \
                   c/lib/ime/pinyin_syllables.inc
	@mkdir -p $(BUILD)
	$(CC) $(IME_CF) -o $@ $(IME_SRC)

test-ime: $(BUILD)/ime_test fsroot/ime/pinyin.dat test-ime-negctl
	$(BUILD)/ime_test

# THE NEGATIVE CONTROL: -DIME_NO_BACKTRACK compiles the segmenter down to
# greedy longest-syllable-first with NO backtracking (see the #ifdef inside
# seg_dfs() in pinyin.c) -- the first (longest) syllable that matches at each
# position is committed to permanently, so a dead end below it is never
# revisited.
#
# "xian" itself does NOT redden under this control, and that is a finding
# about the DICTIONARY, not a weak control: tools/mkpinyin.py concatenates a
# phrase's pinyin with no syllable separator, so the single-syllable reading
# ("xian" -> 先) and the two-syllable phrase ("xi"+"an" -> 西安) already live
# under the SAME dictionary key, both reachable via tier 0 (the whole-buffer
# lookup) with no segmentation involved at all. Removing backtracking cannot
# touch tier 0, so both candidates survive in the negctl build too --
# verified below, not assumed (the build must still find both).
#
# FOUR witnesses actually require backtracking, and the fourth was found by
# running this very control rather than designed in advance -- worth keeping
# for that reason. Three were found by exhaustive search over the shipped
# dictionary and the engine's own 414-syllable table (every pair A,B of
# dictionary-keyed syllables where greedy longest-first dead-ends on A+B,
# backtracking recovers exactly [A,B], and A+B is itself NOT a dictionary key
# -- so no tier-0 shortcut can rescue it):
#   angong (an+gong, greedy commits "ang" then cannot parse "ong"/"ng")
#   jini   (ji+ni,   greedy commits "jin" then cannot parse "i")
#   xier   (xi+er,   greedy commits "xie" then cannot parse "r")
# The fourth is "nihao" itself: greedy's FIRST run at position 2 ("hao",
# length 3) already reaches the end of the buffer successfully, so greedy
# never backtracks to try the shorter "ha" there -- which means the SECOND,
# lower-ranked tier-1 candidate ni+ha+o (你哈哦) never gets generated at all,
# even though the primary candidate (你好, reached via tier 0) is completely
# unaffected. First run of this control did not expect a 4th failure; it is
# real (verified: 你哈哦 is present in the default build's first page and
# absent here), so the count below is 4, not the 3 originally designed for --
# re-measured rather than forced to match a guess.
# Each of the four produces ZERO matching candidates under IME_NO_BACKTRACK
# where the default build produces >= 1 -- exactly 4 assertions redden.
test-ime-negctl: $(IME_SRC) c/lib/ime/pinyin.h c/lib/ime/pinyin_fmt.h \
                 c/lib/ime/pinyin_syllables.inc fsroot/ime/pinyin.dat
	@mkdir -p $(BUILD)
	@$(CC) $(IME_CF) -DIME_NO_BACKTRACK -o $(BUILD)/ime_negctl $(IME_SRC)
	@echo "--- negative control: the SAME assertions with backtracking compiled out ---"
	@if $(BUILD)/ime_negctl > $(BUILD)/ime_negctl.log 2>&1; then \
	    echo "test-ime-negctl: FAILED -- the greedy-only build PASSED everything,"; \
	    echo "  so nothing in the suite is measuring backtracking."; \
	    exit 1; \
	else \
	    n=`grep -c '^FAIL:' $(BUILD)/ime_negctl.log`; \
	    if [ "$$n" != "4" ]; then \
	        echo "test-ime-negctl: FAILED -- reddened $$n assertion(s), expected exactly 4."; \
	        echo "  Re-measure it; never adjust the number to whatever the run printed."; \
	        grep '^FAIL:' $(BUILD)/ime_negctl.log; \
	        exit 1; \
	    fi; \
	    echo "test-ime-negctl: ok -- exactly 4 assertion(s) redden without backtracking:"; \
	    grep '^FAIL:' $(BUILD)/ime_negctl.log | sed 's/^/    /'; \
	fi

# Named on the suite so it runs, and its control is a prerequisite of the
# positive above so the control runs too -- the two halves of not being in
# tests/audit-stranded.baseline.
ci-host: test-ime
