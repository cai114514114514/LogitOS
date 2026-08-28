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
# THE ENGINE IS HOST-TESTABLE; THE FEATURE IS NOT. pinyin.c has no kernel/vfs/wm
# dependency at all (freestanding, like c/lib/gfx) -- tests/unit/ime_test.c links
# it directly and loads the real shipped fsroot/ime/pinyin.dat exactly as
# tests/unit/ttf_test.c loads the real shipped font.
#
# This paragraph used to end "so there is no `-os` counterpart here yet", and
# that was wrong for as long as it stood: tests/boot/run-ime-test.sh and
# tests/boot/ime_type.py were written the day the feature landed and no Makefile
# target has ever named either of them. `make test-audit` listed both under DEAD
# and nobody read the list. So the whole-path assertion -- scancode to
# keyboard.c to wm_key to ime_ui.c to EV_KEY to the app's UTF-8 encoder to
# SYS_WRITE_FILE to LogitFS to media -- ran exactly twice, both times by hand.
# It is test-ime-os below.
#
# ---------------------------------------------------------------------------
# WHAT THE POSITIVE GATE ASSERTS, in one paragraph, because a control is only
# sharp if the contract it defends is written down.
#
# The engine assembles candidates in four CLASSES and the class order is the
# contract, not a preference:
#
#   T0  IME_TIER_KEY   the dictionary key equal to the WHOLE buffer
#   T1  IME_TIER_SEG   segmentation-composed, >= 2 syllables
#   T2  IME_TIER_PRE   prefix extensions: keys strictly extending the buffer
#   T3  IME_TIER_ABBR  the initials bucket equal to the buffer  ("nh" -> 你好)
#
# Within a class: score descending; equal scores keep arrival order, which is
# a total and reproducible order because the file's own order is total. Score
# is the u32 jieba frequency stored per candidate by pinyin.dat v2, plus
# whatever a user-weight store returns, CLAMPED to the ceiling that store
# declared. Two live assertions depend on T0 < T1 < T2 being strict (西安 at
# index 8 of "xian"; 及你 at index 0 of "jini") -- see the TIER_BLIND control.
#
# ---------------------------------------------------------------------------
# SIX CONTROLS, and every one of them is a PREREQUISITE of test-ime.
#
# Not a sibling on a `ci-host:` line: naming a control beside its positive
# satisfies tools/audit_tests.py's stranded check and still runs it never,
# which is worse than being stranded because it looks fixed. tests/license.mk
# and tests/logreporter.mk are the worked examples; this fragment follows them.
#
# Each control is the SAME 108 assertions with exactly one thing changed, and
# each pins the exact number that must redden. The numbers below were measured
# one control at a time on 2026-08-28, twice each, against the shipped
# dictionary -- never adjusted to match a run.
#
#   target                        one change                        redden
#   ---------------------------------------------------------------------
#   test-ime-negctl               -DIME_NO_BACKTRACK                   9
#   test-ime-flatfreq-negctl      -DIME_CTL_FLAT_FREQ                 10
#   test-ime-bucketorder-negctl   -DIME_CTL_BUCKET_KEYORDER            5
#   test-ime-tierblind-negctl     -DIME_CTL_TIER_BLIND                12
#   test-ime-promote-negctl       -DIME_CTL_PROMOTE                    3
#   test-ime-ceiling-negctl       -DIME_CTL_HONEST_CEILING             1
#
# Five of the six change the TEST, not the engine (a dictionary mutation on
# this process's own copy of the bytes, a rival user-weight policy, or a
# reader that re-ranks the engine's own output). Only IME_NO_BACKTRACK edits
# the code under test. That split is deliberate and is explained at the top of
# tests/unit/ime_test.c: a mutation control can ask "which stored FACT is this
# assertion reading?", which a code #ifdef cannot ask at all.
#
# There is a seventh gate that is not a control -- test-ime-dat, which proves
# the 967,663 bytes every one of those 108 assertions is pinned against are
# what tools/mkpinyin.py actually produces. It has its own control.
.PHONY: test-ime test-ime-negctl test-ime-flatfreq-negctl \
        test-ime-bucketorder-negctl test-ime-tierblind-negctl \
        test-ime-promote-negctl test-ime-ceiling-negctl \
        test-ime-dat test-ime-dat-negctl test-ime-os test-ime-os-negctl

# -DIME_STATS compiles in recompute()'s per-key work counters (pinyin.h:385).
# The cost group in ime_test.c asserts against them and, without the define,
# fails with its own instruction -- "tests/ime.mk's IME_CF must define it". It
# was written believing this line already carried it and this line did not: the
# counters, the test that reads them and the flag that turns them on were three
# places that had to agree, and one of them was never written. Defined here so
# the assertion has something to assert about; it costs the shipped kernel
# nothing, because c/lib/ime/pinyin.c compiles into it WITHOUT this flag.
IME_CF  := -O2 -g -Wall -Wextra -DIME_STATS -Ic/lib/ime -Iinclude/abi

IME_SRC := tests/unit/ime_test.c c/lib/ime/pinyin.c
IME_DEP := c/lib/ime/pinyin.h c/lib/ime/pinyin_fmt.h c/lib/ime/pinyin_syllables.inc \
           fsroot/ime/pinyin.dat

# ONE definition site for the assertion count, because it is checked in seven
# places (the positive and each of the six controls) and CLAUDE.md rule 3 is
# about exactly this: a constant spelled twice agrees on the wrong value about
# as often as the right one. It is pinned EXACTLY, not as a floor: the six
# control counts below are only meaningful as a fraction of a known total, and
# a suite that quietly lost thirty assertions would otherwise still be green
# in the positive AND in every control whose reddening assertions survived.
# Adding an assertion is one edit here plus a re-measure of the six -- which is
# the friction this pin exists to charge.
IME_CHECKS := 108

# The shared verdict for a control run. Written once and passed the binary,
# the -D that made it, and the count it must produce -- so the six recipes
# below differ ONLY in those three tokens and in the $(CC) line, which is the
# level at which tools/negctl_drift.py compares a control to its positive.
#
# `sh -c '...' ime-verdict` so $$1/$$2/$$3 are the arguments and $$0 is a name
# that shows up in an error message. grep -cE, not `grep -c "a\|b"`: BSD grep's
# BRE has no \| and would silently count zero (CLAUDE.md's fourth host shape --
# the coreutil difference that makes a gate answer confidently and wrongly).
IME_CTL_VERDICT = sh -c 'l="$$1.log"; \
	if "$$1" > "$$l" 2>&1; then \
	    echo "    FAILED: the $$2 build PASSED all $(IME_CHECKS) assertions."; \
	    echo "    Nothing in the suite measures what that flag turns off."; \
	    exit 1; \
	fi; \
	n=`grep -c "^FAIL:" "$$l"`; t=`grep -cE "^(ok|FAIL):" "$$l"`; \
	if [ "$$t" != "$(IME_CHECKS)" ]; then \
	    echo "    FAILED: $$2 ran $$t assertions, IME_CHECKS pins $(IME_CHECKS)."; \
	    echo "    The suite changed size; re-measure all six controls."; \
	    exit 1; \
	fi; \
	if [ "$$n" != "$$3" ]; then \
	    echo "    FAILED: $$2 reddened $$n of $$t, expected exactly $$3."; \
	    echo "    Re-measure it; never edit the number to whatever the run printed."; \
	    grep "^FAIL:" "$$l" | sed "s/^/      /"; \
	    exit 1; \
	fi; \
	echo "    ok -- exactly $$n of $$t redden under $$2:"; \
	grep "^FAIL:" "$$l" | sed "s/^/      /"' ime-verdict

$(BUILD)/ime_test: $(IME_SRC) $(IME_DEP)
	@mkdir -p $(BUILD)
	$(CC) $(IME_CF) -o $@ $(IME_SRC)

test-ime: $(BUILD)/ime_test $(IME_DEP) \
          test-ime-negctl test-ime-flatfreq-negctl test-ime-bucketorder-negctl \
          test-ime-tierblind-negctl test-ime-promote-negctl \
          test-ime-ceiling-negctl test-ime-dat
	@$(BUILD)/ime_test > $(BUILD)/ime_test.log 2>&1; rc=$$?; \
	 cat $(BUILD)/ime_test.log; \
	 t=`grep -cE "^(ok|FAIL):" $(BUILD)/ime_test.log`; \
	 if [ $$rc -ne 0 ]; then exit 1; fi; \
	 if [ "$$t" != "$(IME_CHECKS)" ]; then \
	     echo "test-ime: FAILED -- $$t assertions ran, tests/ime.mk pins IME_CHECKS=$(IME_CHECKS)."; \
	     echo "  A green run of a shrunken suite is the failure this pin exists for."; \
	     exit 1; \
	 fi

# ---- control 1/6: the segmenter has no backtracking -------------------------
#
# -DIME_NO_BACKTRACK compiles seg_dfs() down to greedy longest-syllable-first:
# the first (longest) syllable that matches at each position is committed to
# permanently, so a dead end below it is never revisited. This is the only one
# of the six that edits the code under test.
#
# "xian" itself does NOT redden under this control, and that is a finding
# about the DICTIONARY, not a weak control: tools/mkpinyin.py concatenates a
# phrase's pinyin with no syllable separator, so the single-syllable reading
# ("xian" -> 先) and the two-syllable phrase ("xi"+"an" -> 西安) already live
# under the SAME dictionary key, both reachable via T0 (the whole-buffer
# lookup) with no segmentation involved at all. Removing backtracking cannot
# touch T0, so both candidates survive in the negctl build too -- verified,
# not assumed (the build must still find both, and does).
#
# FOUR witnesses actually require backtracking, and the fourth was found by
# running this very control rather than designed in advance -- worth keeping
# for that reason. Three were found by exhaustive search over the shipped
# dictionary and the engine's own 414-syllable table (every pair A,B of
# dictionary-keyed syllables where greedy longest-first dead-ends on A+B,
# backtracking recovers exactly [A,B], and A+B is itself NOT a dictionary key
# -- so no T0 shortcut can rescue it):
#   angong (an+gong, greedy commits "ang" then cannot parse "ong"/"ng")
#   jini   (ji+ni,   greedy commits "jin" then cannot parse "i")
#   xier   (xi+er,   greedy commits "xie" then cannot parse "r")
# The fourth is "nihao" itself: greedy's FIRST run at position 2 ("hao",
# length 3) already reaches the end of the buffer successfully, so greedy
# never backtracks to try the shorter "ha" there -- which means the SECOND,
# lower-ranked T1 candidate ni+ha+o (你哈哦) never gets generated at all,
# even though the primary candidate (你好, reached via T0) is completely
# unaffected. First run of this control did not expect a 4th failure; it is
# real (verified: 你哈哦 is present in the default build's first page and
# absent here), so the count is 4 witnesses, not the 3 originally designed for
# -- re-measured rather than forced to match a guess.
#
# IT IS NINE ASSERTIONS NOW, AND THE FIVE NEW ONES ARE NOT NEW WITNESSES --
# they are new ASSERTIONS standing on the same four. Re-measured 2026-08-28
# after the abbreviation/prefix/phrase work, one at a time, rather than by
# editing the count to match a run:
#   5. the incremental sweep `ncand across n/ni/nih/niha/nihao is exactly
#      { 96 96 2 2 2 }` -- the tail of that sequence is T1 output, so it
#      moves when segmentation loses a parse.
#   6. `"nihao" is exactly 你好 你哈哦` -- witness 4 again, now asserted as a
#      SET rather than as a presence.
#   7. `"jini" leads with 及你, then the prefix extensions` -- witness 2 again:
#      without backtracking 及你 does not exist, so the head of the ranked list
#      is a prefix extension instead.
#   8. `ignoring the classes would push 西安 from index 8 to 59` -- the ranking
#      assertion reads a list that T1 helped build.
#   9. `jini is NOT a witness for a score merge` -- a META-assertion about why
#      jini cannot demonstrate a score merge; it reasons about 及你's score and
#      cannot when 及你 is absent.
# So the count is 9 because the suite grew from 32 assertions to 108 and five
# of the new ones happen to depend on candidates only backtracking produces --
# NOT because backtracking became load-bearing in five new places. Anyone
# raising this number again should be able to say which of those two it is.
test-ime-negctl: $(IME_SRC) $(IME_DEP)
	@mkdir -p $(BUILD)
	@$(CC) $(IME_CF) -DIME_NO_BACKTRACK -o $(BUILD)/ime_ctl_backtrack $(IME_SRC)
	@echo "--- control: the segmenter cannot backtrack (IME_NO_BACKTRACK) ---"
	@$(IME_CTL_VERDICT) $(BUILD)/ime_ctl_backtrack IME_NO_BACKTRACK 9

# ---- control 2/6: the dictionary stores an ORDER and no NUMBER --------------
#
# -DIME_CTL_FLAT_FREQ rewrites every candidate's u32 frequency to 1 in this
# process's own copy of the loaded bytes, before ime_open(). The shipped file
# is never touched. That is EXACTLY v1's information content -- the file keeps
# its order and loses its number -- so this is the control for pinyin_fmt.h's
# whole thesis, and for the format bump the v2 header cost.
#
# It is 1 and not 0 on purpose: at 0 every score ties the selector's initial
# floor of 0, offer_key()'s `score_bound(base) <= s->floor` fires on the first
# record of every key, and the engine returns an empty list everywhere. A
# control that empties the machine measures nothing.
#
# TEN REDDEN, AND WHICH TEN IS THE FINDING. Everything that compares
# candidates WITHIN one key or WITHIN one initials bucket survives untouched:
# "xian" page 0 is still 先 县 现 线 显 仙 弦 献 西安, the whole 32-name "nh"
# vector is still in order with 你好 at index 8, "bj" still leads with 北京,
# "zz" and "sj" page 0 are unchanged. That is not the control failing -- it is
# the measurement that motivated v2 in the first place, restated from the
# other side: v1's stored order is CORRECT inside one key and one bucket, and
# an order simply cannot be merged ACROSS keys.
#
# So what reddens is exactly the cross-key merges and the user-weight
# comparisons: rank(你好) under prefix "ni" (49 -> absent, a 168-key merge),
# "n" page 0, "beijing" (two keyed readings + one composed + three prefix
# extensions in one list), the nih-nested-in-ni check, the 13-count 西安/鲜
# margin, the class-vs-score displacement, and all four learned-weight
# assertions -- because against a base of 1 any bonus at all is decisive, so
# the store stops being a tie-breaker and becomes a personaliser.
test-ime-flatfreq-negctl: $(IME_SRC) $(IME_DEP)
	@mkdir -p $(BUILD)
	@$(CC) $(IME_CF) -DIME_CTL_FLAT_FREQ -o $(BUILD)/ime_ctl_flatfreq $(IME_SRC)
	@echo "--- control: every candidate frequency := 1, i.e. v1's information (IME_CTL_FLAT_FREQ) ---"
	@$(IME_CTL_VERDICT) $(BUILD)/ime_ctl_flatfreq IME_CTL_FLAT_FREQ 10

# ---- control 3/6: the ref buckets are not pre-sorted ------------------------
#
# -DIME_CTL_BUCKET_KEYORDER sorts each initials bucket's refs ascending by
# byte offset -- which, because the key section is itself sorted by key, is
# DICTIONARY KEY ORDER. That is what a generator emits if it appends refs as
# it walks the keys and never sorts: the natural implementation that
# pinyin_fmt.h's "each bucket is pre-sorted by DESCENDING FREQUENCY at build
# time" exists to rule out. The file still passes every one of ime_open()'s
# structural checks -- refs in range, buckets tiling the array with no gap and
# no overlap -- because none of them is about order. Nothing in the tree
# checked this before; the property lived entirely in the generator.
#
# FIVE REDDEN, IN TWO DIFFERENT SHAPES, and the second one is the reason this
# control is worth more than a list-order diff:
#   - "zz" (248 refs) and "sj" (165) come back with the wrong page 0.
#   - "zz", "sj" AND "bj" (108) redden the COST group, on `lists identical:
#     NO`. That assertion runs the same buffer twice, once with a store that
#     returns 0 for everything but declares a huge ceiling (which defeats the
#     prune) and once without, and demands the two lists agree. The prune's
#     exactness is a THEOREM about descending input; on key-ordered input it
#     silently becomes a heuristic that drops real candidates.
#
# "nh" does NOT redden, and that is a fact about the dictionary rather than a
# weak control: the abbreviation selector's floor stays 0 until it fills, and
# a fresh T3 sweep has the whole IME_MAX_CAND=96 budget, so a 32-ref bucket is
# re-sorted correctly by the selector no matter what order it arrives in. Only
# a bucket LARGER than the remaining budget can demonstrate the property at
# all, and the shipped dictionary has five (zz 248, ss 244, zs 200, sz 199,
# jz 190). Two of them are asserted; adding a third would not add a shape.
test-ime-bucketorder-negctl: $(IME_SRC) $(IME_DEP)
	@mkdir -p $(BUILD)
	@$(CC) $(IME_CF) -DIME_CTL_BUCKET_KEYORDER -o $(BUILD)/ime_ctl_bucketorder $(IME_SRC)
	@echo "--- control: initials buckets in key order, not frequency order (IME_CTL_BUCKET_KEYORDER) ---"
	@$(IME_CTL_VERDICT) $(BUILD)/ime_ctl_bucketorder IME_CTL_BUCKET_KEYORDER 5

# ---- control 4/6: the four classes are merged into one ranked list ----------
#
# -DIME_CTL_TIER_BLIND changes the test's READER, not the engine:
# cand_snapshot() re-sorts the engine's own emitted list by score alone. That
# is exactly what "somebody deleted the class order and kept the scores" looks
# like from outside, and it is the plausible simplification -- one list, one
# comparator, fewer moving parts, and it produces a completely reasonable-
# looking candidate list for most buffers.
#
# TWELVE REDDEN and they are the whole reason T0 < T1 < T2 is written as a
# contract: "xian" leads with 向 instead of 先 and 西安 falls from index 8
# (last slot of page 0, one keypress) to 59 (six pages down); "nihao" leads
# with the composed 你哈哦 instead of 你好; "ni" page 0 leads with 年, a
# prefix extension, instead of the key's own 你. The user-visible cost of
# merging is that a rarer word from a LONGER key outranks the exact thing the
# user typed, which is the failure people describe as "the IME is stupid".
test-ime-tierblind-negctl: $(IME_SRC) $(IME_DEP)
	@mkdir -p $(BUILD)
	@$(CC) $(IME_CF) -DIME_CTL_TIER_BLIND -o $(BUILD)/ime_ctl_tierblind $(IME_SRC)
	@echo "--- control: rank by score alone, classes ignored (IME_CTL_TIER_BLIND) ---"
	@$(IME_CTL_VERDICT) $(BUILD)/ime_ctl_tierblind IME_CTL_TIER_BLIND 12

# ---- control 5/6: the learned store promotes to front ----------------------
#
# -DIME_CTL_PROMOTE swaps the test's user-weight store from an accumulator
# (bonus = commits * 256) to promote-to-front (return the whole budget the
# moment the count is non-zero). The store is the TEST'S, not the engine's --
# pinyin.h keeps the mutable table outside struct ime_dict precisely so it
# never lands in the object every window shares unlocked -- so a rival ranking
# policy is a two-line change here and needs no engine edit at all.
#
# This is the plausible wrong implementation, not a broken one: it "works" on
# every single-commit demo, and it is what a reader who has only seen 你好
# jump to the top after one commit would write. THREE REDDEN, and all three
# are about SHAPE rather than endpoint: the trajectory 8 6 5 2 1 1 0 0 0
# (promote-to-front reads 8 0 0 0 0 0 0 0 0), the PLATEAU inside it -- two
# commits buying no rank and the third buying one -- and "ONE commit moves
# 你好 by two places, not to the front".
#
# The fourth learned-weight assertion (500 commits of 泥 still do not pass 你)
# does NOT redden here, and that is correct: promote-to-front returns
# 0xFFFFFFFF, which the engine CLAMPS to the ceiling the store declared, and
# the declared ceiling is the same 2048 in both builds. Clamping is what keeps
# a runaway store from being able to reorder the dictionary -- and it is the
# subject of control 6, which is why the two are separate targets.
test-ime-promote-negctl: $(IME_SRC) $(IME_DEP)
	@mkdir -p $(BUILD)
	@$(CC) $(IME_CF) -DIME_CTL_PROMOTE -o $(BUILD)/ime_ctl_promote $(IME_SRC)
	@echo "--- control: the learned store promotes to front instead of accumulating (IME_CTL_PROMOTE) ---"
	@$(IME_CTL_VERDICT) $(BUILD)/ime_ctl_promote IME_CTL_PROMOTE 3

# ---- control 6/6: the declared ceiling is not load-bearing ------------------
#
# The engine does not trust what a user-weight store RETURNS; it clamps to the
# (max_mul_q8, max_add) that store declared to ime_set_user_weight(). The
# declaration is what keeps the prune sound: score_bound() has to be an upper
# bound on every score the store can produce, or offer_key()'s early stop
# starts dropping candidates the user chose most.
#
# The positive asserts that with a store returning a real 1536-count bonus but
# declaring (256, 0), the candidate list is BYTE-IDENTICAL to no store at all
# -- same texts, same scores, same order. -DIME_CTL_HONEST_CEILING makes that
# same store declare its real ceiling instead of zero, so the bonus is allowed
# through and 你好 moves from index 8 to 0.
#
# EXACTLY ONE REDDENS, and one is the right number: this control changes a
# single declaration on a single ime_state, and only one assertion in the
# suite is about that declaration. A control that reddened more would mean the
# declaration was leaking into buffers that never installed a store. Its
# sibling assertion -- "the SAME store declaring (256, 2048) takes 你好 to
# rank 0" -- deliberately survives, because it is the honest half of the pair
# and this control makes the under-declared half honest too; the pair is what
# separates "the bonus moved it" from "the declaration moved it".
test-ime-ceiling-negctl: $(IME_SRC) $(IME_DEP)
	@mkdir -p $(BUILD)
	@$(CC) $(IME_CF) -DIME_CTL_HONEST_CEILING -o $(BUILD)/ime_ctl_ceiling $(IME_SRC)
	@echo "--- control: the 'declares nothing' store declares its real ceiling (IME_CTL_HONEST_CEILING) ---"
	@$(IME_CTL_VERDICT) $(BUILD)/ime_ctl_ceiling IME_CTL_HONEST_CEILING 1

# ---- the seventh gate: the dictionary is REPRODUCIBLE -----------------------
#
# Every one of the 108 assertions above is pinned against fsroot/ime/pinyin.dat
# -- 967,663 bytes, 25,945 keys, 35,374 candidates, 5,850 initials rows, 31,112
# refs. Nothing in this tree proved those bytes were derivable from anything.
# A committed binary that no generator reproduces is a fixture, and a suite
# pinned to a fixture measures the fixture.
#
# It is also the second door of CLAUDE.md rule 3: c/lib/ime/pinyin_fmt.h
# defines the format and tools/mkpinyin.py writes it. mkpinyin.py's own
# docstring says the header wins and it is the bug -- this is the line that
# makes that checkable rather than aspirational. The previous version of that
# docstring described a 28-byte header with a fixed-stride syllable table;
# write_binary() had never written one, and nobody noticed for as long as it
# stood.
#
# 0.4 s. --check builds from the committed key section and compares against
# the shipped file WITHOUT writing it. It needs jieba (for the frequencies)
# and mkpinyin.py exits 0 with a loud SKIP line if that is absent, which is
# this tree's rule for a host capability that is missing rather than broken --
# so the recipe below refuses a run that printed NEITHER "ok --" NOR "SKIP:",
# because a generator that exits 0 in silence is the shape rule 5 is about.
test-ime-dat: fsroot/ime/pinyin.dat tools/mkpinyin.py c/lib/ime/pinyin_fmt.h \
              c/lib/ime/pinyin_syllables.inc test-ime-dat-negctl
	@mkdir -p $(BUILD)
	@echo "--- the shipped dictionary is what tools/mkpinyin.py produces ---"
	@python3 tools/mkpinyin.py --check > $(BUILD)/ime_dat.log 2>&1; rc=$$?; \
	 sed 's/^/    /' $(BUILD)/ime_dat.log; \
	 if [ $$rc -ne 0 ]; then \
	     echo "test-ime-dat: FAILED -- fsroot/ime/pinyin.dat is not what the generator emits."; \
	     echo "  This is a tools/mkpinyin.py or pinyin_fmt.h finding, not a pinyin.c one:"; \
	     echo "  every assertion in test-ime is pinned against those bytes."; \
	     exit 1; \
	 fi; \
	 if ! grep -q "mkpinyin --check: ok" $(BUILD)/ime_dat.log && \
	    ! grep -q "^SKIP:" $(BUILD)/ime_dat.log; then \
	     echo "test-ime-dat: FAILED -- --check exited 0 and said neither ok nor SKIP."; \
	     exit 1; \
	 fi

# THE CONTROL for it, and it is byte-for-byte rather than a length check --
# which on this tree is a rule, not a preference: a comparison that only looks
# at length passes for a file that is exactly the right size and holds
# somebody else's data.
#
# The mutation is `LC_ALL=C tr a b` over the shipped file: every ASCII 'a'
# becomes 'b', the key section is full of them, and the length cannot change.
# LC_ALL=C is load-bearing and was found by running it without -- BSD tr in a
# UTF-8 locale exits 1 with "Illegal byte sequence" after writing 13 bytes, so
# the control would have "fired" on a truncated file for a reason that has
# nothing to do with the comparison. The recipe therefore proves the mutation
# did what it claims (same length, different bytes) BEFORE asking --check
# about it; the run that settles it prints both lengths as 967663 and says
# "they differ".
#
# TWO THINGS ABOUT THE LAST THREE LINES OF THE RECIPE, both learned by running
# them rather than by reading them:
#
#   The grep is NOT piped into sed. The first version was
#   `if ! grep ... | sed 's/^/    /'; then FAILED`, and it could not fail -- a
#   pipeline's exit status is the LAST command's, so sed's 0 satisfied the test
#   no matter what grep found. Found by pointing it at a pattern that matches
#   nothing and watching it print "ok": rule 5 happening inside a control whose
#   whole job is rule 5. Test first, then display.
#
#   And the explanation you are reading is HERE, above the rule, rather than in
#   the middle of the recipe where it was first written. make is happy either
#   way (a `#` line does not end a recipe), but tools/negctl_drift.py stops
#   collecting at it -- the last three lines went invisible and the tool
#   reported a drift that did not exist. CLAUDE.md rule 2's family: six tools
#   parse make in this tree, and a comment is not a free action inside a rule.
test-ime-dat-negctl: fsroot/ime/pinyin.dat tools/mkpinyin.py
	@mkdir -p $(BUILD)
	@LC_ALL=C tr 'a' 'b' < fsroot/ime/pinyin.dat > $(BUILD)/ime_dat_flip.dat
	@a=`wc -c < fsroot/ime/pinyin.dat | tr -d ' '`; \
	 b=`wc -c < $(BUILD)/ime_dat_flip.dat | tr -d ' '`; \
	 if [ "$$a" != "$$b" ]; then \
	     echo "test-ime-dat-negctl: FAILED -- the mutation changed the LENGTH ($$a -> $$b),"; \
	     echo "  so a --check that only compared lengths would satisfy this control."; \
	     exit 1; \
	 fi; \
	 if cmp -s fsroot/ime/pinyin.dat $(BUILD)/ime_dat_flip.dat; then \
	     echo "test-ime-dat-negctl: FAILED -- the mutation changed nothing at all."; \
	     exit 1; \
	 fi; \
	 echo "--- control: the same $$a bytes with every ASCII 'a' turned into 'b' ---"
	@if python3 tools/mkpinyin.py --check --output $(BUILD)/ime_dat_flip.dat \
	        --no-crosscheck > $(BUILD)/ime_dat_negctl.log 2>&1; then \
	    echo "    FAILED: --check ACCEPTED a dictionary with different bytes."; \
	    exit 1; \
	 fi
	@if ! grep -q "mkpinyin --check: FAILED" $(BUILD)/ime_dat_negctl.log; then \
	    echo "    FAILED: --check exited non-zero without saying why."; \
	    echo "    A control that fires for an unrelated reason (a missing module,"; \
	    echo "    an unreadable path) is indistinguishable from one that worked."; \
	    sed 's/^/      /' $(BUILD)/ime_dat_negctl.log; \
	    exit 1; \
	 fi
	@grep "mkpinyin --check: FAILED" $(BUILD)/ime_dat_negctl.log | sed 's/^/    /'
	@echo "    ok -- --check refuses it, and names both lengths while doing so."

# ---- the device gate: does a PERSON's keystroke become a Han character -------
#
# WHAT IT PROVES: `nihao` and a space typed into TextEdit, saved with Ctrl+S,
# put E4 BD A0 E5 A5 BD (U+4F60 U+597D) on the disk. The bytes, not a
# screenshot -- a screenshot of CJK proves the FONT works, which it has since
# M14; only the bytes say a codepoint survived the whole path.
#
# WHAT IT CANNOT PROVE, and this is why it is worth writing down rather than
# just wiring. QMP injects scancodes BENEATH the host keyboard, so every link
# from a person's fingers to QEMU is bypassed by construction. On 2026-08-28
# this gate passed green -- the run is in the record -- while the feature was
# unusable, because the chord was Ctrl+Space and macOS, the host this machine
# is developed on, consumes Ctrl+Space itself as "select the previous input
# source". The owner reported the IME "only types English"; the engine was
# never the problem and no test here could have said so. The chord is
# Shift+Space now (c/kernel/gui/ime_ui.h, IME_TOGGLE_NAME, defined once).
#
# Green here means the guest works. It has never meant a person can type.
test-ime-os: $(ISO) $(DISK) tests/boot/run-ime-test.sh tests/boot/ime_type.py \
             test-ime-os-negctl
	@bash tests/boot/run-ime-test.sh $(ISO) $(DISK)

# THE CONTROL: the same run with the toggle chord never sent. The file must then
# hold `nihao ` (6e 69 68 61 6f 20) -- which proves the keys arrived, TextEdit
# received them, the save worked and the extractor can read what was written. So
# when the positive produces two Han characters instead, the TOGGLE is the only
# thing that differs and therefore the only thing that can have caused it.
# Without it the positive is equally consistent with "TextEdit has a Chinese
# mode". A PREREQUISITE of the positive, not a sibling on a ci- line: naming it
# beside the positive satisfies the UNWIRED audit and still runs it never, which
# is worse because it looks fixed.
test-ime-os-negctl: $(ISO) $(DISK) tests/boot/run-ime-test.sh tests/boot/ime_type.py
	@bash tests/boot/run-ime-test.sh $(ISO) $(DISK) --negctl

# Named on the suite so it runs, and its seven gates are prerequisites of the
# positive above so all seven run too -- the two halves of not being in
# tests/audit-stranded.baseline.
ci-host: test-ime
ci-boot: test-ime-os
