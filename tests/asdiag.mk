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
# test-as* target: check-abi guards the shared constants. check-asops used to
# stand beside it and guarded the A2 opcode table; it went with the bytecode.

.PHONY: test-as-diag test-as-diag-negctl-guard test-as-diag-negctl-line

test-as-diag: check-abi $(BUILD)/asc
	@bash tests/unit/run-as-diag.sh $(BUILD)/asc

# The host exercises the same engine objects without any GUI/Logit syscalls.
$(BUILD)/asc: c/apps/as/common/diagnostic.h
STUDIO_HOST_SRC := $(wildcard c/apps/studio/*.c) c/apps/as/editor/completion.c c/apps/as/common/version.c c/lib/agent/json.c c/lib/agent/task.c c/drivers/block/crc32.c
.PHONY: test-studio-core test-studio-core-negctl test-as-check
$(BUILD)/studio-core-test: tests/unit/studio_core_test.c $(STUDIO_HOST_SRC) $(wildcard c/apps/studio/*.h)
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-function -Wno-misleading-indentation -O1 -g -fsanitize=address,undefined tests/unit/studio_core_test.c $(STUDIO_HOST_SRC) -o $@
test-studio-core-negctl: $(BUILD)/studio-core-test $(BUILD)/asc as-toolchain
	@python3 tests/unit/studio_core_gate.py --negative $(BUILD)/studio-core-test $(BUILD)/asc
test-studio-core: test-studio-core-negctl $(BUILD)/studio-core-test $(BUILD)/asc
	@python3 tests/unit/studio_core_gate.py $(BUILD)/studio-core-test $(BUILD)/asc
test-as-check: check-abi $(BUILD)/asc
	@python3 tests/unit/as_check_test.py $(BUILD)/asc

# Completion consumes the same snapshot and module declarations as checking.
# Run the privacy mutation before its positive target so the gate cannot rot.
.PHONY: test-as-completion test-as-completion-negctl
test-as-completion-negctl: $(BUILD)/asc
	@python3 tests/unit/as_semantic_completion_test.py --negative $(BUILD)/asc
	@python3 tests/unit/as_object_completion_test.py --negative $(BUILD)/asc
test-as-completion: test-as-completion-negctl $(BUILD)/asc-numeric-debug
	@python3 tests/unit/as_semantic_completion_test.py $(BUILD)/asc-numeric-debug
	@python3 tests/unit/as_object_completion_test.py $(BUILD)/asc-numeric-debug
test-as-typed test-studio-core: test-as-completion

.PHONY: test-as-recovery test-as-recovery-negctl
$(BUILD)/as-lex-recovery-test: tests/unit/as_lex_recovery_test.c c/apps/as/frontend/lexer.c c/apps/as/frontend/lexer.h
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Ic/apps/as tests/unit/as_lex_recovery_test.c c/apps/as/frontend/lexer.c -o $@
test-as-recovery-negctl: $(BUILD)/as-lex-recovery-test
	@python3 tests/unit/as_lex_recovery_gate.py --negative $<
test-as-recovery: test-as-recovery-negctl $(BUILD)/asc $(BUILD)/asc-numeric-debug
	@python3 tests/unit/as_recovery_test.py $(BUILD)/asc
	@python3 tests/unit/as_recovery_test.py $(BUILD)/asc-numeric-debug
test-as-typed test-as-completion: test-as-recovery

# The same language contract runs optimized and unoptimized under sanitizers.
# Controls perturb private compiler sources, never the working tree.
.PHONY: test-as-numeric test-as-numeric-negctl
$(BUILD)/asc: c/apps/as/common/numeric.h
$(BUILD)/asc-numeric-debug: $(AS_CORE) c/apps/as/cli/main.c c/apps/as/common/numeric.h c/apps/as/common/diagnostic.h c/apps/as/sema/abi_layout.inc
	@mkdir -p $(BUILD)
	$(CC) -O0 -g -fsanitize=address,undefined -o $@ c/apps/as/cli/main.c $(AS_CORE) -Ic/apps/as -Iinclude/abi
$(BUILD)/asc-numeric-debug: $(AS_HDRS)
test-as-numeric-negctl: check-abi
	@python3 tests/unit/as_numeric_negative.py
test-as-numeric: test-as-numeric-negctl $(BUILD)/asc $(BUILD)/asc-numeric-debug
	@python3 tests/unit/as_numeric_test.py $(BUILD)/asc
	@python3 tests/unit/as_numeric_test.py $(BUILD)/asc-numeric-debug

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
test-as-diag-negctl-guard: check-abi $(BUILD)/asc
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
test-as-diag-negctl-line: check-abi $(BUILD)/asc
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

# Recording primitives execute the actual Studio renderer, not a copied model.
.PHONY: test-studio-render test-studio-render-negctl
$(BUILD)/studio-render-test: tests/unit/studio_render_test.c c/apps/studio/studio_render.inc c/apps/studio/studio_code.inc c/apps/studio/studio_completion.inc $(STUDIO_HOST_SRC) $(wildcard c/apps/studio/*.h)
	$(CC) -std=c11 -D_DEFAULT_SOURCE -O1 -g -fsanitize=address,undefined -Wno-unused-function tests/unit/studio_render_test.c $(STUDIO_HOST_SRC) -o $@
test-studio-render-negctl: $(BUILD)/studio-render-test
	@python3 tests/unit/studio_render_gate.py --negative
test-studio-render: test-studio-render-negctl $(BUILD)/studio-render-test
	@$(BUILD)/studio-render-test

# The guest gate builds a PRIVATE disk; never overwrite a running desktop.
# Base contains the kernel/fonts' core CLI peers; Studio/AS come from BUILD.
STUDIO_GUEST_BASE ?= build
.PHONY: test-studio-completion
test-studio-completion: test-studio-render test-studio-core $(BUILD)/studio.aex $(BUILD)/as.aex
	@python3 tests/boot/run-studio-completion.py --build $(BUILD) --base $(STUDIO_GUEST_BASE) --out $(BUILD)/studio-completion-$$$$
.PHONY: test-studio-ui
test-studio-ui: test-studio-render test-studio-core $(BUILD)/studio.aex $(BUILD)/as.aex
	@python3 tests/boot/run-studio-ui.py --build $(BUILD) --base $(STUDIO_GUEST_BASE) --out $(BUILD)/studio-ui-$$$$

.PHONY: test-studio-persistence-os
test-studio-persistence-os: test-studio-persistence test-studio-render test-studio-core $(BUILD)/studio.aex $(BUILD)/as.aex
	@python3 tests/boot/run-studio-ui.py --build $(BUILD) --base $(STUDIO_GUEST_BASE) --rebuild --out $(BUILD)/studio-rebuild-$$$$
