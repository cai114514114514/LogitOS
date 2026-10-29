# Subtitles -- WebVTT and SRT, against an independently written oracle.
#
# THE ORACLE SHARES NO CODE WITH THE PARSER. tests/unit/subs_oracle.py is a
# second implementation, in another language, and tests/unit/subs_diff.py
# compares the two field by field for every cue of every fixture -- the same
# relationship demux_diff.py has to ffprobe. The question is not "does it
# run", it is "do two independently written parsers agree on every field".
#
# Numeric fields carry a tolerance and that is not slack: a WebVTT `line`
# value can legitimately be Number.MAX_VALUE, and string-diffing two
# languages' float formatting compares formatting, not parsing.
#
# THE CORPUS IS 48 .vtt FILES EXTRACTED FROM web-platform-tests, 116 KB,
# committed -- not fetched. That is deliberate and does NOT contradict the
# WPT un-vendoring (tools/wpt_fetch.sh at a pinned revision): this is a small
# extract that a parser gate needs in order to run at all, with its own
# README recording the upstream pin and BSD-3-Clause, and a PROVENANCE.md
# classifying every file. 116 KB is not 90 MB.

SUBS_SRC := c/lib/media/subs.c
SUBS_INC := -Ic/lib/media
SUBS_FX  := tests/fixtures/subs

$(BUILD)/subs_test: tests/unit/subs_test.c $(SUBS_SRC) c/lib/media/subs.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/subs_test.c $(SUBS_SRC) $(SUBS_INC)

# The control build: -DSUBS_STRICT aborts the whole parse on the FIRST cue it
# cannot fully understand, instead of skipping it and carrying on.
$(BUILD)/subs_test_strict: tests/unit/subs_test.c $(SUBS_SRC) c/lib/media/subs.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DSUBS_STRICT -o $@ tests/unit/subs_test.c $(SUBS_SRC) $(SUBS_INC)

.PHONY: test-subs test-subs-negctl test-subs-fuzz

# THE CONTROL IS A PREREQUISITE, NOT A SECOND NAME ON A ci-host: LINE.
# tools/audit_tests.py's NOT_CI drops every `test-*-negctl` from what
# tools/ci.sh runs, on the assumption that a control is "RUN BY its positive
# counterpart" -- nothing checked that, and this one was invoked by nobody from
# the day it landed until the audit's STRANDED CONTROLS category named it
# (2026-08-28). It reuses the positive's own binary and fixtures, so the only
# new cost is the -DSUBS_STRICT build beside it.
test-subs: test-subs-negctl

test-subs: $(BUILD)/subs_test
	@$(BUILD)/subs_test units
	@python3 tests/unit/subs_diff.py $(BUILD)/subs_test $(SUBS_FX)/wpt
	@python3 tests/unit/subs_srt_roundtrip.py $(BUILD)/subs_test \
	    $(SUBS_FX)/sample.vtt $(SUBS_FX)/sample.ffmpeg.srt

# The control asserts BOTH halves, and the second is the one that has teeth:
# a fixture the plain build reports skipped==0 for must produce the
# BYTE-IDENTICAL dump under -DSUBS_STRICT. "It also succeeds" would be
# satisfied by a strict build that quietly parsed something else.
test-subs-negctl: $(BUILD)/subs_test $(BUILD)/subs_test_strict
	@python3 tests/unit/subs_negctl.py $(BUILD)/subs_test \
	    $(BUILD)/subs_test_strict $(SUBS_FX)/wpt

# A subtitle file comes off the network -- a <track src>, a .srt downloaded
# next to a video -- and unlike a container there is no magic number gating
# the parse, so every length in it is a stranger's claim.
$(BUILD)/subs_fuzz: tests/unit/subs_fuzz.c $(SUBS_SRC) c/lib/media/subs.h
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $@ tests/unit/subs_fuzz.c $(SUBS_SRC) $(SUBS_INC)

test-subs-fuzz: $(BUILD)/subs_fuzz
	@$(BUILD)/subs_fuzz $(SUBS_FX)/wpt
