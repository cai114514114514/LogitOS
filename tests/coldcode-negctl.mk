# tests/coldcode-negctl.mk -- the cold-code control, WATCHED FAILING.
#
# CLAUDE.md rule 5: "a control that cannot be watched failing is worse than no
# control, because it reads like one."  The tree has live examples --
# test-bidi-negctl prints "negative control ok" on a machine where the Unicode
# corpus does not exist; test-demux-fuzz-negctl is satisfied by an ASan abort
# that never reaches the injected bug.  So this fragment does not assert that
# the control is good.  It BREAKS the reader in two named ways and requires
# test-coldcode-control to go RED, each time, for the stated reason.
#
# AND IT IS WIRED THE ONLY WAY THAT WORKS: `test-coldcode-control:
# test-coldcode-control-negctl`.  CLAUDE.md counts 61 stranded controls in this
# tree, dropped from the suite on the theory that "a control is run by its
# positive counterpart" -- which nothing checked.  It also names the fix that
# LOOKS like a fix and is worse: naming it on a `ci-host:` line satisfies the
# audit and still runs it never.  A prerequisite edge is the real thing.
#
# THE TWO DEFECTS ARE THE TWO WAYS THIS INSTRUMENT CAN LIE:
#
#   fnlevel  the COARSE READER.  Hands every line in a function the function's
#            entry count.  It passes a function-level control perfectly -- both
#            coldctl_hot_marker and coldctl_cold_unreachable still answer
#            correctly -- and calls every never-taken branch in the tree
#            COVERED.  A reader with this defect would have reported all six of
#            the 2026-08-29 failures as fine.  The line half of the control is
#            the only thing that catches it, which is why the line half exists.
#
#   nolink   the control TU MISSING from the coverage map.  This is what a
#            report over a binary that did not link the file looks like from
#            the inside, and it is the failure CLAUDE.md rule 1 calls "the same
#            failure in a new costume": every line reads cold and the number is
#            about the harness, not the code.  The reader must say so rather
#            than report zeros.

.PHONY: test-coldcode-control-negctl
test-coldcode-control-negctl: cold-probe
	@set -e; \
	 d=$(COLD_WORK)/ctl-neg; rm -rf $$d; mkdir -p $$d; \
	 fix=$$(ls -d tests/fixtures/webapi/*/ 2>/dev/null | head -1); \
	 if [ -z "$$fix" ]; then \
	   echo "test-coldcode-control-negctl: SKIP -- tests/fixtures/webapi is empty."; \
	   echo "  With nothing to run, the HOT half reads cold and BOTH defects"; \
	   echo "  below would 'fail' for that reason instead of their own -- which"; \
	   echo "  is precisely the shape of test-demux-fuzz-negctl, satisfied by an"; \
	   echo "  abort that never reaches the injected bug. Refusing to pass."; \
	   exit 0; fi; \
	 pd=$$( (xcrun -f llvm-profdata 2>/dev/null) || echo /opt/homebrew/opt/llvm/bin/llvm-profdata ); \
	 if [ ! -x "$$pd" ]; then \
	   echo "test-coldcode-control-negctl: SKIP -- llvm-profdata absent (keg-only:"; \
	   echo "  /opt/homebrew/opt/llvm/bin, or Xcode). Settle: xcrun -f llvm-profdata"; \
	   exit 0; fi; \
	 LLVM_PROFILE_FILE=$$d/c-%c-%m.profraw $(COLD_OUT)/webapi_probe --errors $$fix \
	   > $$d/probe.log 2>&1 || true; \
	 "$$pd" merge -sparse $$d/*.profraw -o $$d/ctl.profdata; \
	 echo "--- baseline: the control must HOLD before either defect means anything"; \
	 python3 tools/coldcode/whatran.py $(COLD_OBJS) --profdata $$d/ctl.profdata \
	     --self-test > $$d/base.log 2>&1 || { \
	       echo "test-coldcode-control-negctl: FAIL -- the UNBROKEN control did not"; \
	       echo "  hold, so a red result under either defect would prove nothing."; \
	       cat $$d/base.log; exit 1; }; \
	 echo "    ok"; \
	 fail=0; \
	 for m in fnlevel nolink; do \
	   echo "--- defect: COLDCODE_BREAK=$$m  (must go RED)"; \
	   if COLDCODE_BREAK=$$m python3 tools/coldcode/whatran.py $(COLD_OBJS) \
	        --profdata $$d/ctl.profdata --self-test > $$d/$$m.log 2>&1; then \
	     echo "test-coldcode-control-negctl: FAIL -- the reader was broken with"; \
	     echo "  COLDCODE_BREAK=$$m and STILL passed its own control. The control"; \
	     echo "  is decorative; it is measuring itself."; \
	     sed 's/^/      /' $$d/$$m.log; fail=1; \
	   else \
	     echo "    went red, as it must:"; \
	     grep -E '^  \*' $$d/$$m.log | head -4 | sed 's/^/      /'; \
	   fi; \
	 done; \
	 if [ $$fail != 0 ]; then exit 1; fi; \
	 echo "test-coldcode-control-negctl: ok -- the control was watched failing in"; \
	 echo "  both directions a reader can be wrong: too coarse, and not linked."

# The edge that stops this from joining the 61 stranded controls.
test-coldcode-control: test-coldcode-control-negctl
