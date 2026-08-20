# tests/nn.mk -- c/lib/nn: tensors and the kernels an inference pass is made of.
#
# In its own fragment for the reason tests/webapi_platform.mk gives: several
# agents edit the top-level Makefile at once, and a fragment is the only way to
# add targets without a commit sweeping up somebody else's half-finished work.
.PHONY: test-nn test-nn-negctl test-lm-format test-lm-infer test-lm-infer-negctl \
        lmtrain-check

# quant4.c is in here because c/lib/nn/model.c grew a dependency on it (q4 is a
# dtype the format now carries) and THREE link lines did not follow -- so
# test-lm-format, test-lm-infer and $(BUILD)/lm_host all stopped LINKING, which
# took test-lm-os with them. test-lm-os is the only target from this whole line
# that CI can see, so the line's only CI-visible gate was unbuildable and the
# only thing that noticed was an agent running the targets by hand.
#
# CLAUDE.md records this exact failure mode -- "a source file grew a dependency
# and a link line did not follow" -- and lists five host targets it has already
# cost. It is in the shared variable rather than repeated three times so the
# next dependency has one place to be added and cannot be added to two of three.
NN_SRC = c/lib/nn/tensor.c c/lib/nn/matmul.c c/lib/nn/ops.c c/lib/nn/quant4.c
NN_CF  = -Ic/lib/nn -O2 -w

test-nn: test-nn-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(NN_CF) -o $(BUILD)/nn_test tests/unit/nn_test.c $(NN_SRC) -lm
	@$(BUILD)/nn_test

# THREE NEGATIVE CONTROLS, one per kernel whose plausible wrong version is a
# DIFFERENT CORRECT-LOOKING ALGORITHM rather than a mistake. Each is built
# separately and each must fail its OWN checks and no others -- one control
# turning on all three would not say which group of checks was measuring what.
#
#   NN_RMS_SUBTRACT_MEAN   rmsnorm becomes LayerNorm. Identical on zero-mean
#                          data, which is why the test feeds it mean-3 input.
#   NN_NO_SOFTMAX_SHIFT    the textbook softmax. Correct on every input a
#                          hand-written test would use; NaN on a real
#                          attention row.
#   NN_ROPE_SPLIT_HALF     huggingface's pairing instead of the interleaved
#                          one. STILL A ROTATION -- so the length-preserving
#                          check passes and only the reference check reddens,
#                          which is the point: it is wrong in a way no
#                          invariant catches.
test-nn-negctl:
	@mkdir -p $(BUILD)
	@fail=0; \
	 for d in NN_RMS_SUBTRACT_MEAN NN_NO_SOFTMAX_SHIFT NN_ROPE_SPLIT_HALF; do \
	   $(CC) $(NN_CF) -D$$d -o $(BUILD)/nn_neg_$$d tests/unit/nn_test.c $(NN_SRC) -lm || exit 1; \
	   if $(BUILD)/nn_neg_$$d > $(BUILD)/nn_neg_$$d.log 2>&1; then \
	     echo "test-nn-negctl: FAILED -- the suite PASSED with -D$$d,"; \
	     echo "  so nothing in it is measuring that kernel."; fail=1; \
	   else \
	     n=`grep -c '^FAIL:' $(BUILD)/nn_neg_$$d.log`; \
	     echo "test-nn-negctl: ok -- -D$$d reddens $$n check(s):"; \
	     grep '^FAIL:' $(BUILD)/nn_neg_$$d.log | sed 's/^/    /'; \
	   fi; \
	 done; \
	 exit $$fail

# --- LOGITLM format + inference: c/lib/nn/model.c and c/lib/nn/infer.c -------
#
# Same shape as test-nn above (host build, -Ic/lib/nn, link the kernels
# straight in) but each needs its own NN_SRC-plus-one-file link line rather
# than reusing $(NN_SRC), because model.c and infer.c are not part of the
# tensor/matmul/ops kernel set test-nn gates -- they are the format reader and
# the forward pass built ON TOP of it. infer.c calls into model.c (a model to
# run inference against) and both call into the NN_SRC kernels, so
# test-lm-infer's link line carries all four.
test-lm-format:
	@mkdir -p $(BUILD)
	@$(CC) $(NN_CF) -o $(BUILD)/lm_format_test tests/unit/lm_format_test.c \
	    c/lib/nn/model.c $(NN_SRC) -lm
	@$(BUILD)/lm_format_test

test-lm-infer: test-lm-infer-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(NN_CF) -o $(BUILD)/lm_infer_test tests/unit/lm_infer_test.c \
	    c/lib/nn/infer.c c/lib/nn/model.c $(NN_SRC) -lm
	@$(BUILD)/lm_infer_test

# THE NEGATIVE CONTROL FOR THE DECOUPLED head_dim.
#
# -DLM_DERIVE_HEAD_DIM restores `head_dim = dim / n_heads` in BOTH places
# infer.c uses it -- geom(), which sizes the arena, and lm_forward's shape
# invariant, which was `hd * n_heads == dim`. That is the whole of the old
# behaviour rather than a mutation of the new one, so what reddens under it is
# what the fix bought and not an artefact of how the control was written.
#
# IT IS A PREREQUISITE OF THE POSITIVE, not a separate suite member. CLAUDE.md
# measured 55 controls in this tree that are their own target, named by nobody,
# and therefore run NEVER while looking exactly like a control that is covered
# -- `test-X: test-X-negctl` is the one line that fixes that, and naming this
# on ci-host beside test-lm-infer would satisfy the audit and still run it
# never, which is the worse of the two failures because it looks fixed.
#
# TWO ASSERTIONS, and the first is the one that does not depend on a count.
# Every check that must redden carries the literal tag `[head_dim]` in its
# name, so:
#
#   (a) EVERY failing check is a head_dim check. This is what stops the control
#       from being satisfied by breaking something else -- a build that fails
#       the q8 bound or the nucleus draw under -DLM_DERIVE_HEAD_DIM has not
#       demonstrated anything about head_dim.
#   (b) There are exactly LM_NEG_N of them. Enumerated below, not observed:
#
#         t_layout                                                        5
#           the two decoupled configs in the byte-total loop (cfg 2, 3)    2
#             -- one with q_dim 12 > dim 8, one with q_dim 4 < dim 8, so
#                a max() written as either a min() or a plain q_dim fails
#           an explicit head_dim lifts the `dim % n_heads` requirement     1
#           a head geometry that does not fit an int is refused, + WHY     2
#         t_forward, the two small decoupled configs                       6
#           each: lm_forward returns NULL at pos 0, "all 4 positions ran", 3
#           and the reference check, which is FAILED rather than evaluated
#           against a `worst` no comparison produced
#         t_headdim, the Qwen3-0.6B slice                                  6
#           lm_state_bytes on the unopened header                          1
#           the loader/sizer disagreement: refused, WHY, and lm_state_new  3
#           lm_forward runs at dim 1024 / 16 heads / head_dim 128          1
#           the reference at that shape (NOT RUN)                          1
#                                                                        ---
#                                                                         17
#
# What must KEEP PASSING is as much of the point: `arena_len ==
# lm_state_bytes` stays green under the control, because it is true BY
# CONSTRUCTION (layout() runs twice) and was true while the bug shipped. A
# control that reddened it would be saying that equality was the evidence, and
# it never was -- the independent one is `lm_state_bytes == expect_bytes`.
LM_NEG_N = 17

.PHONY: test-lm-infer-negctl
test-lm-infer-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(NN_CF) -DLM_DERIVE_HEAD_DIM -o $(BUILD)/lm_infer_neg \
	    tests/unit/lm_infer_test.c c/lib/nn/infer.c c/lib/nn/model.c $(NN_SRC) -lm
	@if $(BUILD)/lm_infer_neg > $(BUILD)/lm_infer_neg.log 2>&1; then \
	   echo "test-lm-infer-negctl: FAILED -- the suite PASSED with"; \
	   echo "  -DLM_DERIVE_HEAD_DIM, so nothing in it is measuring the"; \
	   echo "  decoupled head_dim at all."; exit 1; \
	 fi; \
	 n=`grep -c '^FAIL:' $(BUILD)/lm_infer_neg.log`; \
	 t=`grep '^FAIL:' $(BUILD)/lm_infer_neg.log | grep -c '\[head_dim\]'`; \
	 if [ "$$n" != "$$t" ]; then \
	   echo "test-lm-infer-negctl: FAILED -- $$n check(s) reddened but only"; \
	   echo "  $$t of them are head_dim checks. The control broke something"; \
	   echo "  else; these are the untagged failures:"; \
	   grep '^FAIL:' $(BUILD)/lm_infer_neg.log | grep -v '\[head_dim\]' | sed 's/^/    /'; \
	   exit 1; \
	 fi; \
	 if [ "$$n" != "$(LM_NEG_N)" ]; then \
	   echo "test-lm-infer-negctl: FAILED -- $$n head_dim check(s) reddened,"; \
	   echo "  the enumeration in tests/nn.mk says $(LM_NEG_N). Re-derive it"; \
	   echo "  there rather than editing the number to match:"; \
	   grep '^FAIL:' $(BUILD)/lm_infer_neg.log | sed 's/^/    /'; \
	   exit 1; \
	 fi; \
	 echo "test-lm-infer-negctl: ok -- -DLM_DERIVE_HEAD_DIM reddens $$n check(s),"; \
	 echo "  every one of them a [head_dim] check:"; \
	 grep '^FAIL:' $(BUILD)/lm_infer_neg.log | sed 's/^/    /'

# --- tools/lmtrain.c: the HOST-ONLY trainer -----------------------------
#
# Not linked against anything that boots (tools/lmtrain.md says so in the
# first line) and not part of C_SRC, so it gets its own two-line rule rather
# than reusing any userland pattern above -- there is no crt0, no -Ttext, no
# .aex, just a host binary linking model.h/matmul.c by relative #include (see
# tools/lmtrain.md). $(BUILD)/lmtrain, not tools/lmtrain, so `make clean`'s
# existing `rm -rf $(BUILD)` already covers it.
$(BUILD)/lmtrain: tools/lmtrain.c c/lib/nn/model.h c/lib/nn/matmul.c c/lib/nn/nn.h
	@mkdir -p $(BUILD)
	cc -O2 -o $@ tools/lmtrain.c -lm

# The gate lmtrain.md says to run before believing any loss curve: every
# analytic gradient against a central finite difference, in double, three
# configs (tied/untied/GQA) so a wiring bug that only shows up with unequal
# n_heads/n_kv_heads has somewhere to hide from and be found. ~18ms; not
# wired into ci-host because it builds+runs a whole extra host binary for one
# fast check that test-lm-format/test-lm-infer above don't exercise (the
# trainer, not the reader/inference path) -- run by name.
lmtrain-check: $(BUILD)/lmtrain
	@$(BUILD)/lmtrain --gradcheck

# --- the host build of /bin/lm, from the SAME source the device runs --------
#
# c/apps/lm/lm.c compiles unchanged on the host: it uses stdio/string/time and
# nothing else, and c/lib/nn is plain freestanding-compatible C. So this is not
# a host REIMPLEMENTATION of the device program -- there is exactly one lm.c,
# and test-lm-os below compares two builds of it rather than two programs. That
# distinction is the whole value of the comparison: a second implementation
# that agreed would prove nothing about the first, and one that disagreed would
# not say which was wrong.
$(BUILD)/lm_host: c/apps/lm/lm.c $(wildcard c/lib/nn/*.c) $(wildcard c/lib/nn/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(NN_CF) -o $@ c/apps/lm/lm.c \
	    c/lib/nn/infer.c c/lib/nn/model.c $(NN_SRC) -lm

# --- test-lm-os: the same weights, the same source, both machines -----------
#
# SKIPPED, LOUDLY, WHEN THERE IS NO MODEL. build/model.lm is not in the build
# graph (Makefile:1144: it comes from running the host trainer by hand) and is
# not committed -- 3.2 MB of weights is not a repo artifact -- so on a fresh
# clone there is nothing on the disk image at /model.lm and nothing for this
# gate to compare. That is the same rule test-wpt already applies to an absent
# WPT_ROOT, and for the same reason: a missing corpus is not a regression in
# the code under test. It is a skip and not a stub because the skip prints the
# command that removes it and cannot be mistaken for a pass.
.PHONY: test-lm-os
test-lm-os: $(ISO) $(DISK) $(BUILD)/lm_host
	@if [ ! -f $(BUILD)/model.lm ]; then \
	   echo "test-lm-os: SKIP -- no $(BUILD)/model.lm, so the disk image has"; \
	   echo "  no /model.lm and there are no weights to compare. Train one:"; \
	   echo "    make $(BUILD)/lmtrain && $(BUILD)/lmtrain --gradcheck"; \
	   echo "    $(BUILD)/lmtrain --corpus CLAUDE.md --out $(BUILD)/model.lm --steps 3000"; \
	   echo "    make $(DISK)   # repack, so /model.lm is on the image"; \
	   exit 0; \
	 fi; \
	 bash tests/boot/run-lm-test.sh $(ISO) $(DISK) $(BUILD)/lm_host $(BUILD)/model.lm

# Named on the suite so it runs, with its control a prerequisite of the
# positive -- the two halves of not landing in tests/audit-stranded.baseline.
ci-host: test-nn test-lm-format test-lm-infer
ci-boot: test-lm-os
