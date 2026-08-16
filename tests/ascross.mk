# tests/ascross.mk -- AetherScript compiler crosscheck (unit DX).
#
# The two compilers for this language check each other. See the header of
# tests/unit/run-as-crosscheck.sh for why the three existing selfhost gates do
# not cover this: they compare tokens, runtime output of nine programs, and the
# bytecode of exactly one input (asc.as). This one diffs the .la BYTES of every
# .as in the tree, compiled by both.
#
# Prerequisites are the same as every other test-as* target: check-asops guards
# the hand-copied opcode table in asc.as, check-abi the shared constants. They
# matter more here than elsewhere -- a drifted opcode is precisely the failure
# this gate is built to see, and running with a stale asc.as would blame the
# wrong thing.

.PHONY: test-as-crosscheck test-as-crosscheck-negctl

test-as-crosscheck: check-asops check-abi $(BUILD)/asc
	@bash tests/unit/run-as-crosscheck.sh $(BUILD)/asc

# NEGATIVE CONTROL. A gate nobody has watched fail is a gate nobody knows the
# polarity of. This one perturbs the SELF-HOSTED compiler in a scratch copy --
# never the tree -- and requires the gate to fail.
#
# The perturbation is OP_RET, from 27 to 99, and it is chosen rather than
# invented: CLAUDE.md's SELF-HOSTING TAX paragraph names exactly this edit as
# the SILENT MISCOMPILE that `test-as` and `test-as-gcstress` stay fully green
# through, catchable today only by `make check-asops`. So this target also
# records what the crosscheck buys: a second, independent detector for the
# tree's own documented worst case, one that works on the emitted bytes instead
# of on a table comparison.
#
# The scratch copy lives in $(BUILD) and is deleted before and after; nothing
# under fsroot/ is touched, so a Ctrl-C here cannot leave a drifted compiler
# behind.
test-as-crosscheck-negctl: check-asops check-abi $(BUILD)/asc
	@rm -rf $(BUILD)/ascross-perturb
	@mkdir -p $(BUILD)/ascross-perturb
	@cp fsroot/as/lib/*.as $(BUILD)/ascross-perturb/
	@sed -i.bak 's/^OP_RET = 27$$/OP_RET = 99/' $(BUILD)/ascross-perturb/asc.as && rm -f $(BUILD)/ascross-perturb/asc.as.bak
	@grep -q '^OP_RET = 99$$' $(BUILD)/ascross-perturb/asc.as || \
	    { echo "negctl SETUP FAILED: could not perturb OP_RET in the scratch copy (did asc.as change spelling?)"; \
	      rm -rf $(BUILD)/ascross-perturb; exit 1; }
	@echo "negctl: self-hosted compiler perturbed (OP_RET 27 -> 99) in $(BUILD)/ascross-perturb; the gate MUST fail"
	@bash tests/unit/run-as-crosscheck.sh $(BUILD)/asc $(BUILD)/ascross-perturb > $(BUILD)/ascross-negctl.log 2>&1; \
	  rc=$$?; \
	  head -20 $(BUILD)/ascross-negctl.log; \
	  echo "..."; tail -2 $(BUILD)/ascross-negctl.log; \
	  rm -rf $(BUILD)/ascross-perturb; \
	  if [ $$rc -eq 0 ]; then \
	      echo "test-as-crosscheck-negctl: FAIL -- the gate PASSED on a perturbed compiler; it is not checking anything"; \
	      exit 1; \
	  fi; \
	  grep -q 'DIVERGENCE (BYTECODE-DIFFERS)' $(BUILD)/ascross-negctl.log || \
	      { echo "test-as-crosscheck-negctl: FAIL -- gate failed, but not with a bytecode divergence (wrong reason)"; exit 1; }; \
	  grep -q 'first differing byte: offset' $(BUILD)/ascross-negctl.log || \
	      { echo "test-as-crosscheck-negctl: FAIL -- divergence reported without a byte offset; the message is not actionable"; exit 1; }; \
	  echo "test-as-crosscheck-negctl: PASS (gate failed, named the file, the offset and both sides' bytes)"
