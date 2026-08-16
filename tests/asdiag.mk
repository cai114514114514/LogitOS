# tests/asdiag.mk -- AetherScript diagnostics parity (unit DG).
#
# tests/ascross.mk proves the two compilers emit identical bytecode for every
# .as file in the tree. Every file in that corpus COMPILES, so the crosscheck
# has never looked at the other half of a compiler: what it says when the
# program is WRONG. This one does, over a corpus of 48 deliberately broken
# programs, and it exists because /bin/as is about to stop being the C
# compiler -- after which a user who forgets a colon reads asc.as's message
# instead of compiler.c's, and a worse message would be a downgrade the
# crosscheck cannot see (a broken program produces no bytecode to diff).
#
# The full argument, the scoring rules and the stated limits are in the header
# of tests/unit/run-as-diag.sh; the corpus and what each class declares are in
# tests/unit/asdiag/CLASSES.txt. Prerequisites are the same as every other
# test-as* target: check-asops guards the hand-copied opcode table, check-abi
# the shared constants.

.PHONY: test-as-diag test-as-diag-negctl-guard test-as-diag-negctl-line

test-as-diag: check-asops check-abi $(BUILD)/asc
	@bash tests/unit/run-as-diag.sh $(BUILD)/asc

# ---------------------------------------------------------------------------
# NEGATIVE CONTROLS. Two, because this gate has two independent arms and a gate
# nobody has watched fail is a gate nobody knows the polarity of. Both perturb a
# SCRATCH COPY of the self-hosted compiler in $(BUILD) -- never the tree -- so a
# Ctrl-C here cannot leave a drifted compiler behind.

# ARM 1, the hard one: (a) "did it diagnose at all". The perturbation removes
# the expression-depth guard in asc.as -- which is not a hypothetical edit, it
# is the state this compiler was actually in until this unit measured it. The
# parser recurses through real VM calls, so without the guard ~83 nested
# parentheses exhaust the C VM's 256-frame stack and the user gets "as: call
# depth exceeded" from inside asc.grouping: no line, no mention of their
# program. The gate must call that a CRASH and fail.
test-as-diag-negctl-guard: check-asops check-abi $(BUILD)/asc
	@rm -rf $(BUILD)/asdiag-perturb
	@mkdir -p $(BUILD)/asdiag-perturb
	@cp fsroot/as/lib/*.as $(BUILD)/asdiag-perturb/
	@sed -i.bak 's/if len(self.expr_starts) >= 64:/if len(self.expr_starts) >= 100000:/' \
	    $(BUILD)/asdiag-perturb/asc.as && rm -f $(BUILD)/asdiag-perturb/asc.as.bak
	@grep -q 'if len(self.expr_starts) >= 100000:' $(BUILD)/asdiag-perturb/asc.as || \
	    { echo "negctl SETUP FAILED: could not disable the depth guard in the scratch copy (did asc.as change spelling?)"; \
	      rm -rf $(BUILD)/asdiag-perturb; exit 1; }
	@echo "negctl-guard: expression-depth guard disabled in $(BUILD)/asdiag-perturb; the gate MUST fail"
	@bash tests/unit/run-as-diag.sh $(BUILD)/asc $(BUILD)/asdiag-perturb > $(BUILD)/asdiag-negctl-guard.log 2>&1; \
	  rc=$$?; \
	  grep -E 'HARD FAIL|as-diag:' $(BUILD)/asdiag-negctl-guard.log | head -12; \
	  rm -rf $(BUILD)/asdiag-perturb; \
	  if [ $$rc -eq 0 ]; then \
	      echo "test-as-diag-negctl-guard: FAIL -- the gate PASSED on a compiler that crashes on deep input"; \
	      exit 1; \
	  fi; \
	  grep -q 'HARD FAIL \[expression-nested-too-deep\]: self-hosted compiler CRASHED' $(BUILD)/asdiag-negctl-guard.log || \
	      { echo "test-as-diag-negctl-guard: FAIL -- gate failed, but not by naming the crash (wrong reason)"; exit 1; }; \
	  grep -q 'innermost frame: asc.grouping' $(BUILD)/asdiag-negctl-guard.log || \
	      { echo "test-as-diag-negctl-guard: FAIL -- crash reported without naming the frame it came from; the message is not actionable"; exit 1; }; \
	  echo "test-as-diag-negctl-guard: PASS (gate failed, named the class, the frame and the message)"

# ARM 2, the scored one: (b) "does it name the line". The perturbation deletes
# the "(line N)" suffix from every raise site in the self-hosted compiler --
# messages stay word-for-word identical, only the location goes. Nothing about
# (a) changes, so this arm proves the baseline comparison is load-bearing on its
# own and not carried by the hard arm.
test-as-diag-negctl-line: check-asops check-abi $(BUILD)/asc
	@rm -rf $(BUILD)/asdiag-perturb2
	@mkdir -p $(BUILD)/asdiag-perturb2
	@cp fsroot/as/lib/*.as $(BUILD)/asdiag-perturb2/
	@sed -i.bak 's/ (line {[^}]*})//g' $(BUILD)/asdiag-perturb2/asc.as $(BUILD)/asdiag-perturb2/aslex.as
	@rm -f $(BUILD)/asdiag-perturb2/*.bak
	@grep -q '(line {' $(BUILD)/asdiag-perturb2/asc.as && \
	    { echo "negctl SETUP FAILED: a '(line ...)' remains in the scratch asc.as -- the sed did not take"; \
	      rm -rf $(BUILD)/asdiag-perturb2; exit 1; } || true
	@echo "negctl-line: line numbers stripped from every raise in $(BUILD)/asdiag-perturb2; the gate MUST fail"
	@bash tests/unit/run-as-diag.sh $(BUILD)/asc $(BUILD)/asdiag-perturb2 > $(BUILD)/asdiag-negctl-line.log 2>&1; \
	  rc=$$?; \
	  grep -E 'REGRESSION|as-diag:' $(BUILD)/asdiag-negctl-line.log | head -8; \
	  echo "..."; \
	  grep -c 'REGRESSION' $(BUILD)/asdiag-negctl-line.log | sed 's/^/regressed classes: /'; \
	  rm -rf $(BUILD)/asdiag-perturb2; \
	  if [ $$rc -eq 0 ]; then \
	      echo "test-as-diag-negctl-line: FAIL -- the gate PASSED on a compiler that names no line anywhere"; \
	      exit 1; \
	  fi; \
	  grep -q 'REGRESSION' $(BUILD)/asdiag-negctl-line.log || \
	      { echo "test-as-diag-negctl-line: FAIL -- gate failed, but not against the baseline (wrong reason)"; exit 1; }; \
	  echo "test-as-diag-negctl-line: PASS (gate failed, per-class, against the committed baseline)"
