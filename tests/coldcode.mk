# tests/coldcode.mk -- CODE THAT CANNOT RUN, and the per-change gate that is
# the point of it.
#
# tools/check-test-liveness.py finds tests that cannot fail.  This is its twin:
# it finds code that never ran.  CLAUDE.md keeps a HAND-WRITTEN list of that
# under "(b) Built with no real consumer" -- fs_prefix with zero enforcement
# points, .lpk signatures the loader never checks, 921 lines of panic/KASSERT
# whose only call sites are its own self-test, layout_text.c's 1,352 lines of
# UAX #14 that no page has ever reached, fontcolor.c, gfx_fill_clipped, the
# whole h264/h265 stack, subs.c, VP8 inter frames.  A hand-written list is the
# wrong shape for this: it rots, it is incomplete by construction, and it
# cannot catch the instance somebody adds tomorrow.  Six were added in the
# twenty-four hours before this fragment was written.
#
# THE PRODUCT IS `make cold-what-ran`, NOT THE WHOLE-TREE REPORT.  A whole-tree
# report is read once and is wallpaper by Friday.  The question a person
# actually has is "I just wrote 400 lines -- did any of it run", and every one
# of those six failures would have answered it at the moment it was written.
#
# NOT ON A ci-host: LINE, DELIBERATELY.  A corpus run boots no QEMU but it does
# link two 13 MB instrumented binaries and drive hundreds of pages through
# them, and with COLD_FULL=1 it drives 221 jsfb implementations and 9,181 WPT
# files.  That is neither fast nor perfectly deterministic (the WPT runner
# forks and the fixtures fetch subresources), and CLAUDE.md is explicit that a
# gate which fails for reasons unrelated to the code under test is noise that
# trains people to ignore red.  This is a target somebody runs DELIBERATELY,
# before a commit.
#
# The one thing here that IS a gate is `test-coldcode-control`: it builds the
# probe, runs a single fixture, and asserts that the instrument can tell a line
# that ran from a line that did not INSIDE THE SAME FUNCTION.  That is fast and
# deterministic and it is the only claim this fragment makes that can be
# checked without a corpus.

# Everything lands under build-* so `make clean-scratch` reaches it, and OUT is
# a subdirectory make never writes to -- CLAUDE.md's sixth rule for this line:
# "a full-corpus run died at implementation #1 because a background make
# replaced the binary mid-run."  make writes $(COLD_BUILD)/webapi_probe; the
# copy this instrument measures lives in $(COLD_BUILD)/probe/.
COLD_BUILD  ?= build-cold
COLD_OUT    ?= $(COLD_BUILD)/probe
COLD_WORK   ?= $(COLD_BUILD)/work
COLD_PROF   ?= $(COLD_WORK)/all.profdata
COLD_OBJS    = --object $(COLD_OUT)/webapi_probe --object $(COLD_OUT)/wpt_test
COLD_CC      = clang -fprofile-instr-generate -fcoverage-mapping

# What `cold-what-ran` measures your change against.  Default is the working
# tree against HEAD, which is the "I just wrote this" case.  Override with
# REV=<rev>, DIFF=<file>, or FILES="a.c b.c".
COLD_REV    ?= HEAD

.PHONY: cold-probe cold-corpus cold-what-ran cold-report test-coldcode-control

# ---------------------------------------------------------------- the probe
# build.sh asks `make -n -B` for the real link line rather than restating it.
# CLAUDE.md rule 2 (join the continuations first) and rule 4 (hand-copied
# source lists): CANVAS_SRC, PROBE_SRC, H2MUX_SRC and MSE_INC are all copies of
# a TU list the tree kept growing and every one has drifted, and a cold-code
# report built from a stale copy would call every function in the missing TU
# cold -- this instrument's own failure mode wearing the answer's clothes.
#
# THE BUILD IS SEPARATED FROM THE GATE BECAUSE THREE WORKFLOWS ARE LIVE IN
# c/apps/browser AND third_party/.  This target compiles whatever is in the
# tree at the moment it runs, and while it was being written it twice caught
# somebody else's half-saved edit -- a libcss archive missing four
# border-radius accessors, and a quickjs.c with two call sites updated and
# their prototypes not yet.  Neither is a fact about the cold-code control, and
# a gate that goes red for them is the noise CLAUDE.md says trains people to
# ignore red.  So the compile failure says so IN THOSE WORDS.
cold-probe:
	@$(MAKE) BUILD=$(COLD_BUILD) CC="$(COLD_CC)" \
	        $(COLD_BUILD)/webapi_probe $(COLD_BUILD)/wpt_test \
	 || { echo ""; \
	      echo "cold-probe: the TREE did not compile. This is not a cold-code"; \
	      echo "  finding and says nothing about the control -- the probe is an"; \
	      echo "  ordinary host build of the browser, so it fails whenever the"; \
	      echo "  host build fails. Three workflows are live in c/apps/browser"; \
	      echo "  and third_party/; a half-saved edit looks exactly like this."; \
	      echo "  Check with:  make BUILD=$(COLD_BUILD) $(COLD_BUILD)/webapi_probe"; \
	      exit 1; }
	@COLD_BUILD=$(COLD_BUILD) tools/coldcode/build.sh $(COLD_OUT)

# ---------------------------------------------------------------- the corpus
# QUICK by default (the three captured-site corpora, ~5 min).  COLD_FULL=1 adds
# jsfb's 221 implementations and WPT's 9,181 harness files, ~40 min.  The
# manifest run.sh writes names which half ran, so a NEVER-RAN finding is always
# read against the corpus that produced it -- never against an assumed one.
cold-corpus: cold-probe
	@tools/coldcode/run.sh $(COLD_OUT) $(COLD_WORK)

# ------------------------------------------------- THE PRODUCT: one change
# "Of the 412 lines you changed in js_webapi.c, 47 never ran; they are these
# functions."  Not a percentage -- a coverage percentage over a browser is
# meaningless, error handling alone would sink it, and a number that is always
# bad is a number nobody acts on.
#
# It REFUSES on source drift rather than warning, and that refusal is the most
# load-bearing line in this fragment.  You just edited the file; if the profile
# predates the edit the line numbers are not stale, they are MISALIGNED, and
# the tool would confidently name the wrong function.  So this depends on
# cold-corpus: the probe is rebuilt from your source before it is asked.
cold-what-ran: cold-corpus
	@if [ -n "$(DIFF)" ]; then \
	   python3 tools/coldcode/whatran.py $(COLD_OBJS) --profdata $(COLD_PROF) \
	       --corpus-note $(COLD_WORK)/corpus.txt --diff "$(DIFF)"; \
	 elif [ -n "$(FILES)" ]; then \
	   python3 tools/coldcode/whatran.py $(COLD_OBJS) --profdata $(COLD_PROF) \
	       --corpus-note $(COLD_WORK)/corpus.txt \
	       $(foreach f,$(FILES),--file $(f)); \
	 else \
	   python3 tools/coldcode/whatran.py $(COLD_OBJS) --profdata $(COLD_PROF) \
	       --corpus-note $(COLD_WORK)/corpus.txt --rev "$(COLD_REV)"; \
	 fi

# ------------------------------------------------------------ the whole tree
# The once-a-month view.  Ranked by cold lines per translation unit.  It labels
# nothing category 1 / 2 / 3 -- that is a judgement, and a judgement printed
# beside a measurement gets quoted as one.
cold-report: cold-corpus
	@python3 tools/coldcode/report.py $(COLD_OBJS) --profdata $(COLD_PROF) \
	    --json $(COLD_WORK)/coldcode.json --top 60

# ----------------------------------------------------------------- THE GATE
# CLAUDE.md rule 5: a control that cannot be watched failing is worse than no
# control, because it reads like one.  This one is watched BOTH WAYS and at
# BOTH GRANULARITIES, on the same binary, in the same run, by the same reader
# of the same profile:
#
#   coldctl_hot_marker         a constructor -- must read RAN
#   coldctl_cold_unreachable   only call site behind a macro nothing defines
#                              (build.sh greps the tree and fails if anybody
#                              ever defines it) -- must read NEVER RAN
#   coldctl_line_marker        RUNS, and contains a branch on a volatile that
#                              cannot be taken.  Its first statement must read
#                              RAN and the three lines inside the branch must
#                              read NEVER RAN -- IN THE SAME FUNCTION.
#
# That third one is the whole reason this gate exists rather than reusing the
# report's control.  A reader that hands a function's entry count to every line
# in its body passes the first two and calls every never-taken branch in the
# tree covered.  Such a reader would have reported the two singleton iterations
# that publish Screen and Crypto as covered AND everything around them as
# covered -- and would have found none of the six failures of 2026-08-29.
#
# One fixture, one process.  Seconds, no network, no fork, no QEMU.
test-coldcode-control: cold-probe
	@set -e; \
	 d=$(COLD_WORK)/ctl; rm -rf $$d; mkdir -p $$d; \
	 fix=$$(ls -d tests/fixtures/webapi/*/ 2>/dev/null | head -1); \
	 if [ -z "$$fix" ]; then \
	   echo "test-coldcode-control: SKIP -- tests/fixtures/webapi is empty, so"; \
	   echo "  the probe has nothing to run and the HOT half of the control"; \
	   echo "  cannot be exercised. This gate will not pass on a run that never"; \
	   echo "  happened."; exit 0; fi; \
	 LLVM_PROFILE_FILE=$$d/c-%c-%m.profraw $(COLD_OUT)/webapi_probe --errors $$fix \
	   > $$d/probe.log 2>&1 || true; \
	 if ! ls $$d/*.profraw >/dev/null 2>&1; then \
	   echo "test-coldcode-control: FAIL -- the probe wrote no profile at all."; \
	   echo "  Everything would read NEVER RAN, which is the loudest possible"; \
	   echo "  wrong answer. See $$d/probe.log"; exit 1; fi; \
	 pd=$$( (xcrun -f llvm-profdata 2>/dev/null) || echo /opt/homebrew/opt/llvm/bin/llvm-profdata ); \
	 if [ ! -x "$$pd" ]; then \
	   echo "test-coldcode-control: SKIP -- llvm-profdata is not on this host."; \
	   echo "  brew keeps llvm KEG-ONLY, so it is not on PATH even when installed:"; \
	   echo "  look in /opt/homebrew/opt/llvm/bin, or install Xcode. Settle it with:"; \
	   echo "      xcrun -f llvm-profdata"; exit 0; fi; \
	 "$$pd" merge -sparse $$d/*.profraw -o $$d/ctl.profdata; \
	 python3 tools/coldcode/whatran.py $(COLD_OBJS) --profdata $$d/ctl.profdata \
	     --self-test

-include tests/coldcode-negctl.mk
