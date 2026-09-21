# --- test-as-eval: the interpreter behind Studio's Run button ----------------
#
# In its own fragment for the reason tests/dock.mk gives at its top: several
# agents edit the root Makefile at once, and a fragment is the only way to add
# targets without a commit sweeping up somebody else's half-finished work.
#
#   make test-as-eval        the evaluator's printed output, case by case
#   make test-as-eval-asan   the same battery under ASan + UBSan
#
# WHAT IS BEING GATED, AND WHY IT HAD NO GATE. c/apps/as/cli/eval.c is what
# `Run` in Code Studio executes: the machine has no LLVM, so commands.c routes
# a freestanding build (`#if !__STDC_HOSTED__`) to as_typed_eval() and walks
# the checked tree. Every other AetherScript gate in this tree drives the LLVM
# backend, which on this path is exactly the code that does NOT run -- so the
# one execution engine a person reaches by clicking a button was measured by
# nothing at all. What that cost: fsroot/docs/demo.as, the file shipped to
# /docs to be run from that button, could not run, and neither could
#
#     a = 1
#     b = 2
#     print(a / b)
#
# which is the shortest program anybody types. Both were reported from the
# desktop, not by a test.
#
# THE BAR IS THE PRINTED BYTES. This evaluator shares no arithmetic with the
# LLVM backend, so its failure mode is a WRONG ANSWER rather than a refusal --
# an operator with the wrong signedness or a missing overflow check prints a
# plausible number. Every case states the exact stdout.
#
# THE TWO CONTROLS ARE PREREQUISITES OF THE POSITIVE GATE, which is CLAUDE.md's
# fix for the 61 stranded controls it found ("test-X: test-X-negctl"). Naming
# them on a ci-host: line instead would satisfy tools/audit_tests.py's wiring
# check and still run them never, which is worse because it looks fixed.
#
#   AS_EVAL_NO_UNWIND  restores the pre-change control flow, where eval_stmts()
#                      answered only "fell through" / "returned" and a loop
#                      treated any non-zero as a return. `break` then returns
#                      from the enclosing function. NOTE what makes this
#                      control bite: a break that is the LAST thing a function
#                      does is indistinguishable from a return, so the gate's
#                      break cases print a line AFTER the loop. Without that
#                      line the control passes and the gate proves nothing.
#   AS_EVAL_NO_INIT    skips the module initializers, which is what
#                      as_typed_eval() did before. A file's top-level
#                      statements are compiled into __as_initialize_module, so
#                      every program written without a `def` then runs its
#                      second half against globals nothing assigned.
#   AS_EVAL_NO_FSTR    restores the generic refusal for AN_FORMAT, which is
#                      every f-string hole (text.c desugars to `+` chains of
#                      literals and holes). Every f-string case must go red.
#   AS_EVAL_NO_STRCALL restores the refusal AT_CALL_STR fell into before F1.
#                      The two controls sit on the SAME conversion function
#                      (ev_to_str), so deleting that one jar reddens both
#                      batteries -- which is the drift the jar exists to
#                      prevent, watched from the control side.
.PHONY: test-as-eval test-as-eval-negctl test-as-eval-asan

AS_EVAL_SRC := tests/unit/as_eval_test.c
AS_EVAL_INC := -Ic/apps/as -Iinclude/abi

$(BUILD)/as_eval_test: $(AS_EVAL_SRC) $(AS_CORE)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -o $@ $(AS_EVAL_SRC) $(AS_CORE) $(AS_EVAL_INC)

test-as-eval-negctl: $(AS_EVAL_SRC) $(AS_CORE)
	@mkdir -p $(BUILD)
	@for d in AS_EVAL_NO_UNWIND AS_EVAL_NO_INIT AS_EVAL_NO_FSTR AS_EVAL_NO_STRCALL; do \
	   $(CC) -O2 -w -D$$d -o $(BUILD)/as_eval_$$d $(AS_EVAL_SRC) $(AS_CORE) $(AS_EVAL_INC) || exit 1; \
	   if $(BUILD)/as_eval_$$d >/dev/null 2>&1; then \
	     echo "as-eval-negctl: FAIL -- the battery passes with $$d, so it does not measure that claim"; \
	     exit 1; \
	   fi; \
	   echo "as-eval-negctl: $$d red as required ($$($(BUILD)/as_eval_$$d 2>/dev/null | grep -c '^FAIL') cases)"; \
	 done

test-as-eval: test-as-eval-negctl $(BUILD)/as_eval_test
	@$(BUILD)/as_eval_test

# Strings cross a frame boundary in both directions here -- an argument is a
# borrow in the caller that must be copied into the callee's slot, a result is
# built in the callee and must survive that frame being cleared -- so the
# battery is also run with the sanitizers that see those two mistakes.
# detect_leaks is off because darwin/arm64 has no leak detector and a gate that
# FAILS for a missing host capability is the noise CLAUDE.md's host-reality
# table is about.
test-as-eval-asan: $(AS_EVAL_SRC) $(AS_CORE)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer \
	   -o $(BUILD)/as_eval_asan $(AS_EVAL_SRC) $(AS_CORE) $(AS_EVAL_INC)
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/as_eval_asan

ci-host: test-as-eval test-as-eval-asan
