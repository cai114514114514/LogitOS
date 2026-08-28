# tests/imelearn.mk -- the pinyin USER-WEIGHT STORE (c/kernel/gui/ime_learn.c).
#
# Separate from tests/ime.mk, and the split is the same one the code makes
# rather than a filing convenience. tests/ime.mk measures c/lib/ime/pinyin.c:
# an engine that is freestanding by construction and whose ranking, with no
# store installed, is byte-for-byte the dictionary's own frequency order. This
# fragment measures the thing that makes that order MOVE. Two fragments because
# two lines own them: pinyin.c belongs to the engine line and ime_learn.c to
# the desktop line, and a shared fragment is a shared file two agents edit at
# once -- which is the failure tests/ime.mk's own header describes.
#
# The Makefile pulls this in with one line, in the pre-wired block beside the
# other feature fragments:
#
#     -include tests/imelearn.mk
#
# ---------------------------------------------------------------------------
# WHAT THIS GATE CAN SEE, AND WHAT IT STRUCTURALLY CANNOT
# ---------------------------------------------------------------------------
# ime_learn.c is split by -DIME_LEARN_HOST (its own header argues the split):
# the table, the ageing sweep, the serialiser and the parser are pure memory
# work over static arrays and compile here with no kernel header at all. The
# other half -- kmalloc, vfs_write, ktimer, work_queue -- does not exist on a
# host and is NOT measured here. So this gate answers "is the right thing
# learned, and does the file say it" and does not answer "did the bytes reach
# the disk". The second question is a device question and this fragment does
# not pretend otherwise; see the report that shipped this line for the boot
# check that settles it.
#
# It links the REAL engine and loads the REAL shipped dictionary, exactly as
# tests/ime.mk does: every rank the harness prints is a rank the shipped
# machine would show.
.PHONY: test-imelearn test-imelearn-negctl

IMELEARN_CF  := -O2 -g -Wall -Wextra -Werror -DIME_LEARN_HOST \
                -Ic/lib/ime -Ic/kernel/gui -Iinclude/abi
IMELEARN_SRC := tests/unit/ime_learn_test.c c/lib/ime/pinyin.c c/kernel/gui/ime_learn.c
IMELEARN_DEP := c/lib/ime/pinyin.h c/lib/ime/pinyin_fmt.h c/lib/ime/pinyin_syllables.inc \
                c/kernel/gui/ime_learn.h

$(BUILD)/ime_learn_test: $(IMELEARN_SRC) $(IMELEARN_DEP)
	@mkdir -p $(BUILD)
	$(CC) $(IMELEARN_CF) -o $@ $(IMELEARN_SRC)

# THE CONTROL IS A PREREQUISITE, NOT A NOTE. CLAUDE.md rule 5 counts 61
# "stranded" controls in this tree -- controls listed on a ci-host: line, which
# satisfies the audit and still runs them never. `test-imelearn:
# test-imelearn-negctl` is the one-line fix, and it is here rather than below.
test-imelearn: $(BUILD)/ime_learn_test fsroot/ime/pinyin.dat test-imelearn-negctl
	$(BUILD)/ime_learn_test fsroot/ime/pinyin.dat

# ---------------------------------------------------------------------------
# THREE CONTROLS, AND EACH IS A POLICY SOMEBODY COULD GENUINELY PROPOSE
# ---------------------------------------------------------------------------
# The assertions in the harness are IDENTICAL in all four builds. Nothing is
# #ifdef'd to match a control, because a control that flips the expectation to
# match itself PASSES, and rule 5's whole point is that a passing control reads
# like a check. Each build below must go RED, and must go red at the named
# check -- the grep is what makes "red" mean "red for the predicted reason"
# rather than "red because the binary would not link".
#
#   LEARN_CTL_DEAF     ime_learn_note() is never called on a commit. The
#                      machine is told what the user chose and does not write
#                      it down. This is the wiring bug ime_ui.c's emit() is: it
#                      would ship with a store that loads, saves, and learns
#                      nothing, and every other check in the harness passes.
#   LEARN_CTL_NOHOOK   everything is learned and everything is written; the
#                      ranking hook is simply never installed. THIS IS THE ONE
#                      A ROUND-TRIP TEST CANNOT SEE -- the file on disk is
#                      byte-identical to the correct build's. It is exactly the
#                      failure ime_ui.c's st_reset() exists to prevent (five
#                      ime_reset() sites, each of which clears the hook), and
#                      four-of-five-correct is the shape it would really take.
#   LEARN_CTL_KEEPTAIL the parser accepts an unterminated final line. The more
#                      forgiving parser, and it reads a torn write as data.
#                      settings.h's rule -- "a final line with no terminating
#                      newline is DISCARDED" -- is what makes the harness's
#                      truncate-at-every-byte sweep safe BY CONSTRUCTION, and
#                      this is that sweep being watched failing: 14 of 344 cuts
#                      disagree, the first at offset 329.
IMELEARN_CTLS := DEAF NOHOOK KEEPTAIL
IMELEARN_WANT_DEAF     := trajectory is 8 6 5 2 1 1 0 0 0
IMELEARN_WANT_NOHOOK   := trajectory is 8 6 5 2 1 1 0 0 0
IMELEARN_WANT_KEEPTAIL := every prefix parses to exactly its whole lines

test-imelearn-negctl: $(IMELEARN_SRC) $(IMELEARN_DEP) fsroot/ime/pinyin.dat
	@mkdir -p $(BUILD)
	@set -e; \
	for c in $(IMELEARN_CTLS); do \
	  $(CC) $(IMELEARN_CF) -DLEARN_CTL_$$c -o $(BUILD)/ime_learn_nc_$$c $(IMELEARN_SRC); \
	  if $(BUILD)/ime_learn_nc_$$c fsroot/ime/pinyin.dat > $(BUILD)/ime_learn_nc_$$c.log 2>&1; then \
	    echo "NEGCTL FAILED: LEARN_CTL_$$c passed -- the check it breaks is not checking"; \
	    exit 1; \
	  fi; \
	  want=""; \
	  case $$c in \
	    DEAF)     want='$(IMELEARN_WANT_DEAF)';; \
	    NOHOOK)   want='$(IMELEARN_WANT_NOHOOK)';; \
	    KEEPTAIL) want='$(IMELEARN_WANT_KEEPTAIL)';; \
	  esac; \
	  if ! grep -q "FAIL $$want" $(BUILD)/ime_learn_nc_$$c.log; then \
	    echo "NEGCTL FAILED: LEARN_CTL_$$c went red, but NOT at \"$$want\""; \
	    echo "  it failed at:"; grep '^  FAIL' $(BUILD)/ime_learn_nc_$$c.log || true; \
	    exit 1; \
	  fi; \
	  echo "  negctl LEARN_CTL_$$c: red at \"$$want\" -- ok"; \
	done; \
	echo "ime-learn negative controls ok ($(words $(IMELEARN_CTLS)) of $(words $(IMELEARN_CTLS)))"

ci-host: test-imelearn
