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

test-lm-infer: test-lm-infer-negctl test-lm-ropebase-negctl
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
ci-host: test-nn test-lm-format test-lm-infer test-lm-units
ci-boot: test-lm-os

# --- build/lmshape: the model writer ----------------------------------------
#
# WHY IT IS A RULE AND WAS NOT. tools/lmshape.c had no target anywhere in the
# tree -- every agent that used it retyped the link line from the file's own
# header comment. That header records that its link line was WRONG for as long
# as the file existed (it named neither infer.c nor quant4.c, so the documented
# command did not build), which is exactly what an unrun command line does. A
# rule cannot rot that way: it is the only copy and it is exercised.
#
# It reuses $(NN_SRC) and $(NN_CF) rather than restating flags, and adds
# model.c + infer.c the same way $(BUILD)/lm_host does one target above --
# --verify needs the loader, --forward needs lm_forward.
#
# tools/gguf.c joins it rather than getting a binary of its own: --weights is a
# mode of this tool, not a second converter (the seam is elem(); see the file
# header). -Itools is for gguf.h, which is colocated with its .c the way every
# header in this tree is.
$(BUILD)/lmshape: tools/lmshape.c tools/gguf.c tools/gguf.h \
                  $(wildcard c/lib/nn/*.c) $(wildcard c/lib/nn/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(NN_CF) -Itools -o $@ tools/lmshape.c tools/gguf.c \
	    c/lib/nn/model.c c/lib/nn/infer.c $(NN_SRC) -lm

# --- tools/gguf.c: real weights into LOGITLM --------------------------------
#
# $(BUILD)/lmshape above gains tools/gguf.c and -Itools. Both go in the ONE
# rule rather than a second lmshape-with-gguf binary, for the reason that rule
# already gives: a second link line is the thing that rots, and this fragment's
# own header records three targets that stopped LINKING because a source grew a
# dependency and only some of the link lines followed.
#
# WHERE THE MODEL COMES FROM. build/qwen/*.gguf is NOT in the build graph and
# is not committed -- 610 MB of weights is not a repo artifact -- so every
# target below SKIPS LOUDLY when it is absent. Same rule as test-lm-os one
# screen up and as test-wpt's absent WPT_ROOT: a missing corpus is not a
# regression in the code under test. The skip prints the exact fetch, so it
# cannot be mistaken for a pass.
GGUF ?= build/qwen/Qwen3-0.6B-Q8_0.gguf

# numpy is the ORACLE and it is not optional to the MEANING of these gates,
# only to their availability: gguf_check.py exits 77 when numpy is missing and
# the recipes below report that as a SKIP by name rather than as a pass.
PY ?= python3

.PHONY: test-gguf test-gguf-negctl test-lm-ropebase-negctl test-qwen-logits test-qwen-logits-negctl

# --- test-gguf-arch-drift: RETIRED, and the reason is worth keeping ---------
#
# This target grepped LM_ROPE_BASE and LM_RMS_EPS out of c/lib/nn/infer.c and
# LMS_INFER_* out of tools/lmshape.c and required the two COPIES to agree. It
# was a good gate over a bad situation: the constants were file-local #defines
# in a .c, lmshape could not include them, and it needed them in order to
# REFUSE a GGUF whose values differed from the ones the runtime was compiled
# with. The copy was the only thing making that refusal correct.
#
# Its own comment recorded a real bug found by running its control: every regex
# ends in [[:space:]] because `^#define LM_RMS_EPS` is a PREFIX match, so
# renaming the constant left the gate reporting `ok` while reading the renamed
# line. That lesson is preserved here rather than deleted with the recipe.
#
# All of it is gone because the constants moved into struct lm_header, so
# lmshape STORES them instead of refusing, there is one definition, and there
# is no copy to drift. Deleting a gate is worth this much text because a gate
# that quietly disappears looks the same as one that was never written.
#
# test-lm-ropebase-negctl replaces it and checks the stronger property: not
# that two copies of a number agree, but that the number in the file reaches
# the arithmetic.

# --- test-gguf: the positive ------------------------------------------------
#
# FOUR CHECKS, and they measure four different things. Enumerated because a
# gate whose members are not named is a gate whose members can quietly stop
# running:
#
#   1. the NAME MAP closes. Every LOGITLM tensor found a GGUF tensor, every
#      shape agreed, and NO GGUF tensor went unclaimed. The third is the one a
#      converter normally omits: an unclaimed `output.weight` would mean the
#      model is not tied and the file just written has no classifier.
#   2. --verify on the produced file: lm_open, lm_expected_size == st_size,
#      and first+last of every tensor PLUS $(LM_PROBES) random elements each,
#      re-derived FROM THE GGUF through the same elem() the writer used and
#      compared within their own q4 half-step bounds. The random group is
#      tagged FAIL[random] and is the only one that can see an orientation
#      bug -- see test-gguf-negctl below for the derivation.
#   3. the DEQUANTISER against numpy on the raw bytes, EXACTLY. This is the one
#      that catches a constant factor, which every other check in this tree
#      passes over -- see tools/gguf_check.py's header.
#   4. the ORIENTATION, by a matvec through the real nn_matvec_f32 against an
#      independent float64 load of the same tensor. An argument about ne0 being
#      the contiguous dimension is not a proof; this is.
#
# q4 rather than q8 for the written file, because 355.5 MiB is the figure the
# 512 MiB machine's budget was written against and q8's 570.5 MiB is not.
test-gguf: test-lm-ropebase-negctl test-gguf-negctl $(BUILD)/lmshape
	@if [ ! -f "$(GGUF)" ]; then \
	   echo "test-gguf: SKIP -- no $(GGUF), so there are no real weights to"; \
	   echo "  convert. It is a 610 MB download and not a repo artifact:"; \
	   echo "    huggingface-cli download Qwen/Qwen3-0.6B-GGUF \\"; \
	   echo "      Qwen3-0.6B-Q8_0.gguf --local-dir build/qwen"; \
	   echo "  Override the path with: make test-gguf GGUF=/path/to.gguf"; \
	   exit 0; \
	 fi; \
	 set -e; \
	 echo "--- 1+2: convert (q4) and verify against the GGUF ---"; \
	 $(BUILD)/lmshape --weights $(GGUF) --dtype q4 --accept-arch-mismatch \
	     --out $(BUILD)/qwen3.lm > $(BUILD)/gguf_write.log 2>&1; \
	 cat $(BUILD)/gguf_write.log; \
	 grep -q '0 refusal(s)' $(BUILD)/gguf_write.log; \
	 $(BUILD)/lmshape --weights $(GGUF) --dtype q4 --accept-arch-mismatch \
	     --probes $(LM_PROBES) --verify $(BUILD)/qwen3.lm > $(BUILD)/gguf_verify.log 2>&1; \
	 cat $(BUILD)/gguf_verify.log; \
	 grep -q 'VERIFY OK' $(BUILD)/gguf_verify.log; \
	 echo "--- 3: the dequantiser against numpy, exactly ---"; \
	 $(BUILD)/lmshape --weights $(GGUF) --accept-arch-mismatch \
	     --dump-elems $(BUILD)/gguf_elems.csv --samples 4096; \
	 set +e; \
	 $(PY) tools/gguf_check.py --dequant $(GGUF) --elems $(BUILD)/gguf_elems.csv \
	     > $(BUILD)/gguf_deq.log 2>&1; rc=$$?; set -e; \
	 cat $(BUILD)/gguf_deq.log; \
	 if [ $$rc = 77 ]; then echo "test-gguf: SKIP at check 3 -- no numpy"; exit 0; fi; \
	 [ $$rc = 0 ]; \
	 echo "--- 4: the orientation, by matvec ---"; \
	 $(BUILD)/lmshape --weights $(GGUF) --accept-arch-mismatch \
	     --matvec blk.0.attn_k.weight --xy $(BUILD)/gguf_xy.bin; \
	 $(PY) tools/gguf_check.py --matvec $(GGUF) --tensor blk.0.attn_k.weight \
	     --xy $(BUILD)/gguf_xy.bin; \
	 echo "test-gguf: ok -- 4 of 4"

# NAMED ON THE SUITE, and the two controls come with it as prerequisites --
# the same two lines test-lm-infer/test-lm-infer-negctl use above, for the
# reason argued there: a control that is its own target and is named by nobody
# runs NEVER while looking exactly like one that is covered.
#
# WIRED RATHER THAN LEFT IN tests/audit-unwired.baseline, which is where the
# other corpus-dependent targets (test-wpt, test-reftest) sit. Two reasons, and
# the second is the deciding one:
#
#   - without the model it is a SKIP that costs one 5-second link, and that
#     link is exactly the thing this fragment's own header records rotting
#     three times.
#   - test-lm-ropebase-negctl needs NO model at all and guards a silent failure in
#     c/lib/nn: if infer.c's LM_ROPE_BASE or LM_RMS_EPS changes, lmshape starts
#     refusing the wrong files, in both directions, and the dangerous direction
#     is the one that stops refusing.
#
# THE COST, said rather than discovered: with the model present the three full
# conversions take about 9 minutes on this machine -- but that is 9 minutes of
# 9P round trips to /mnt/d, at 3.3 s of user time each. On a local filesystem
# it is well under two. To skip it deliberately: make ci-host GGUF=/nonexistent
ci-host: test-gguf

# --- test-gguf-negctl -------------------------------------------------------
#
# THE CONTROL IS THE PLAUSIBLE WRONG CONVERTER, not an absent one: --weights
# reads the tensor the other way round. GGUF's dim list reads [in, out] while
# torch calls the identical bytes [out, in], so "it must need transposing" is
# exactly what a careful reader concludes -- and the resulting model runs,
# produces finite logits at the same tok/s, and is nonsense.
#
# TWO TENSORS -- wk [1024,1024] and w1 [3072,1024], one square and one not --
# and running BOTH is what corrected the prediction this comment used to carry.
# It said wk would leave --verify's element check green because it is SQUARE,
# and that w1 would redden `last` because it is not. MEASURED: both reddened
# ZERO, and the real property is stronger than the guess. The first and last
# flat index of a tensor are fixed points of (r,c)->(c,r) at ANY shape:
#
#     idx = 0           -> (0,0)            -> c*rows+r = 0            = idx
#     idx = rows*cols-1 -> (rows-1, cols-1) -> (cols-1)*rows + rows-1
#                                            = rows*cols - 1           = idx
#
# So {first,last} is structurally blind to an orientation bug, square or not.
# That is not a defect -- it is what those two probes are FOR (extent: a tensor
# placed one row early cannot have the right last element) -- but it means a
# converter needs a second kind of probe, and this control is what established
# that. `--probes N` adds N RANDOM elements per tensor to --verify, compared
# against the SAME per-element half-step bound, and they are reported and
# tagged separately: FAIL[extent] and FAIL[random].
#
# THE ASSERTION IS ON BOTH TAGS, and that is what makes this a control rather
# than a demonstration:
#
#   FAIL[extent] must be exactly LM_TRANSPOSE_VERIFY_N (0, DERIVED above). A
#     nonzero count means somebody changed which elements --verify probes --
#     which would be an improvement -- so RE-DERIVE the number there rather
#     than editing it here.
#   FAIL[random] must be GREATER THAN ZERO. Without this half, "the control
#     reddens nothing" would satisfy the extent assertion perfectly.
#
# Three things must NOT move under either control, and the recipe asserts each
# rather than assuming it: lm_open, lm_expected_size == the file size, and the
# name map. All three are shape-only and a transposed READ changes no shape --
# which is precisely what makes this bug dangerous.
LM_TRANSPOSE_VERIFY_N = 0
# 13 per tensor x 310 tensors = 4030, the same budget the element dump uses, so
# the two checks probe the same positions from opposite sides: the dump asks
# "did the converter READ the right element", --verify asks "did it WRITE it
# where the loader will look".
LM_PROBES = 13
test-gguf-negctl: test-lm-ropebase-negctl $(BUILD)/lmshape
	@if [ ! -f "$(GGUF)" ]; then \
	   echo "test-gguf-negctl: SKIP -- no $(GGUF) (see test-gguf)"; exit 0; \
	 fi; \
	 fail=0; \
	 for T in wk w1; do \
	   echo "--- control: --neg-transpose $$T ---"; \
	   $(BUILD)/lmshape --weights $(GGUF) --dtype q4 --accept-arch-mismatch \
	       --neg-transpose $$T --out $(BUILD)/qwen3_neg.lm \
	       > $(BUILD)/gguf_neg_$$T.write.log 2>&1 || \
	       { echo "  the control's own write failed"; fail=1; continue; }; \
	   if ! grep -q '0 refusal(s)' $(BUILD)/gguf_neg_$$T.write.log; then \
	     echo "test-gguf-negctl: FAILED -- the name map moved under the control."; \
	     echo "  A transposed READ changes no shape, so if the map reddens the"; \
	     echo "  control is doing something other than transposing."; fail=1; \
	   fi; \
	   $(BUILD)/lmshape --weights $(GGUF) --dtype q4 --accept-arch-mismatch \
	       --probes $(LM_PROBES) --verify $(BUILD)/qwen3_neg.lm > $(BUILD)/gguf_neg_$$T.verify.log 2>&1 || true; \
	   if ! grep -q 'lm_open         0 ok' $(BUILD)/gguf_neg_$$T.verify.log || \
	      ! grep -q '== file size' $(BUILD)/gguf_neg_$$T.verify.log; then \
	     echo "test-gguf-negctl: FAILED -- lm_open or the byte total moved under"; \
	     echo "  the control. Those are the HEADER checks and they must not."; fail=1; \
	   fi; \
	   v=`grep -c 'FAIL\[extent\]:' $(BUILD)/gguf_neg_$$T.verify.log || true`; \
	   r=`grep -c 'FAIL\[random\]:' $(BUILD)/gguf_neg_$$T.verify.log || true`; \
	   if [ "$$v" != "$(LM_TRANSPOSE_VERIFY_N)" ]; then \
	     echo "test-gguf-negctl: FAILED -- FAIL[extent] reddened $$v time(s) with"; \
	     echo "  $$T transposed; the derivation above says $(LM_TRANSPOSE_VERIFY_N)."; \
	     echo "  Both endpoints of any [rows,cols] tensor are fixed points of"; \
	     echo "  (r,c)->(c,r), so 0 is what the geometry gives. A nonzero count"; \
	     echo "  means --verify's element choice changed -- which would be an"; \
	     echo "  improvement. RE-DERIVE the number there, do not edit it here."; \
	     fail=1; \
	   elif [ "$$r" -lt 1 ]; then \
	     echo "test-gguf-negctl: FAILED -- FAIL[random] reddened $$r time(s), so"; \
	     echo "  --verify saw NOTHING with $$T transposed. The extent assertion"; \
	     echo "  above is satisfied by a control that does nothing at all, which"; \
	     echo "  is why this half exists."; fail=1; \
	   else \
	     echo "  --verify: FAIL[extent] $$v (derived: first and last are fixed"; \
	     echo "    points of the transposition), FAIL[random] $$r of $(LM_PROBES)x310"; \
	   fi; \
	   $(BUILD)/lmshape --weights $(GGUF) --accept-arch-mismatch \
	       --neg-transpose $$T --dump-elems $(BUILD)/gguf_neg_$$T.csv --samples 4096 \
	       > /dev/null 2>&1 || { echo "  the control's dump failed"; fail=1; continue; }; \
	   $(PY) tools/gguf_check.py --dequant $(GGUF) --elems $(BUILD)/gguf_neg_$$T.csv \
	       > $(BUILD)/gguf_neg_$$T.deq.log 2>&1; rc=$$?; \
	   if [ $$rc = 77 ]; then echo "test-gguf-negctl: SKIP -- no numpy"; exit 0; fi; \
	   n=`grep -oE '[0-9]+ differ' $(BUILD)/gguf_neg_$$T.deq.log | awk '{print $$1}'`; \
	   if [ $$rc = 0 ]; then \
	     echo "test-gguf-negctl: FAILED -- the spot check PASSED with $$T read"; \
	     echo "  transposed, so nothing in it is measuring the orientation."; fail=1; \
	   else \
	     echo "test-gguf-negctl: ok -- $$T transposed reddens $$n of 4096 spot"; \
	     echo "  elements; lm_open, the byte total and the name map unchanged."; \
	   fi; \
	 done; \
	 echo "--- control: --matvec with the tensor read transposed ---"; \
	 $(BUILD)/lmshape --weights $(GGUF) --accept-arch-mismatch --neg-transpose wk \
	     --matvec blk.0.attn_k.weight --xy $(BUILD)/gguf_xy_neg.bin > /dev/null 2>&1; \
	 if $(PY) tools/gguf_check.py --matvec $(GGUF) --tensor blk.0.attn_k.weight \
	      --xy $(BUILD)/gguf_xy_neg.bin > $(BUILD)/gguf_mv_neg.log 2>&1; then \
	   echo "test-gguf-negctl: FAILED -- the matvec AGREED with a transposed W."; \
	   fail=1; \
	 else \
	   sed 's/^/    /' $(BUILD)/gguf_mv_neg.log; \
	   echo "test-gguf-negctl: ok -- the matvec reddens under the transposition"; \
	 fi; \
	 exit $$fail

# --- test-lm-ropebase-negctl ------------------------------------------------
#
# THE REPLACEMENT FOR test-gguf-arch-drift, which measured the wrong thing for
# a good reason. That gate grepped LM_ROPE_BASE and LM_RMS_EPS out of both
# c/lib/nn/infer.c and tools/lmshape.c and required the two COPIES to agree,
# because lmshape had to know what infer.c had been compiled with in order to
# refuse a GGUF that disagreed. Both the copy and the refusal are gone: the
# constants moved into struct lm_header (model.h argues why, and why zero is a
# safe sentinel for these two where it was not for LogitFS's mode), so there is
# one definition and nothing to drift.
#
# What the copy was ever standing in for is the property gated here: THE NUMBER
# IN THE FILE MUST REACH THE ARITHMETIC. It is the only claim in this subsystem
# whose violation has no symptom -- a 100x rope base is a different position
# encoding, and the model still runs at the same speed and still emits grammar.
#
# The control is -DLM_IGNORE_HEADER_ARCH, which is the PLAUSIBLE wrong build
# and not an absurd one: it is precisely what infer.c did before the fields
# existed, and what any build that missed the change still does.
#
# FOUR COUNTS, NOT ONE. The control must redden EXACTLY the two pos>0 checks --
# the rope BASE and the rope PAIRING, which ride in different header fields (a
# value in what used to be `reserved`, a bit in `flags`) and fail
# independently, so a control covering one would certify the other. And it must
# leave BOTH pos-0 checks PASSING: position 0 rotates by zero under every base
# and every pairing, so reddening those would be the control reporting that it
# broke the model rather than that it broke the position encoding.
test-lm-ropebase-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(NN_CF) -DLM_IGNORE_HEADER_ARCH -o $(BUILD)/lm_ropebase_negctl \
	    tests/unit/lm_infer_test.c c/lib/nn/infer.c c/lib/nn/model.c $(NN_SRC) -lm
	@$(BUILD)/lm_ropebase_negctl > $(BUILD)/lm_ropebase_negctl.log 2>&1 || true
	@moved=`grep -cE '^FAIL: rope(base|pairing): .* reaches the arithmetic' $(BUILD)/lm_ropebase_negctl.log || true`; \
	 pos0=`grep -cE '^ok  : rope(base|pairing): pos 0 is BIT-IDENTICAL' $(BUILD)/lm_ropebase_negctl.log || true`; \
	 if [ "$$moved" != "2" ] || [ "$$pos0" != "2" ]; then \
	   echo "test-lm-ropebase-negctl: FAILED"; \
	   echo "  wanted BOTH pos>0 checks to redden (got $$moved, want 2) and BOTH"; \
	   echo "  pos-0 checks to keep passing (got $$pos0, want 2)."; \
	   grep -E 'ropebase|ropepairing|rope base|interleaved vs' $(BUILD)/lm_ropebase_negctl.log || true; \
	   exit 1; \
	 fi; \
	 echo "test-lm-ropebase-negctl: ok -- ignoring the header's rope base AND its"; \
	 echo "  pairing flag reddens exactly the two pos>0 checks (2) and leaves"; \
	 echo "  both pos-0 checks passing (2)"; \
	 grep -E '      (rope base 10000|interleaved vs NEOX)' $(BUILD)/lm_ropebase_negctl.log || true

# --- test-qwen-logits: is the model this machine runs REALLY Qwen3? ---------
#
# EVERY OTHER GATE IN THIS FILE IS ABOUT THE BYTES. test-gguf checks that the
# right numbers landed in the right places: the name map refuses an unmapped
# tensor, a shape disagreement and an unclaimed GGUF tensor alike; --verify
# re-derives elements straight out of the source; --dump-elems is diffed
# against numpy for EXACT equality; --matvec checks one product against
# float64. All of that can pass on a file that is then EVALUATED WRONG.
#
# There are at least four ways to do that and every one of them is silent:
# the wrong rope base, the wrong rope PAIRING, the wrong rmsnorm epsilon, the
# QK-norm on the wrong side of the rotation. Each produces finite logits,
# unchanged throughput, and fluent, confident, wrong text. Two of the four
# were real here -- Qwen3 wants base 1e6 where this format compiled in 1e4,
# and the NEOX pairing where this format only had interleaved -- and NEITHER
# was visible in any output before this gate existed.
#
# So the comparison is against transformers' OWN Qwen3ForCausalLM, on the
# LOGITS, at EVERY POSITION. Not a hand-written reference: a reference written
# here would encode this tree's understanding of Qwen3, which is the thing
# under test, and a shared misunderstanding would agree beautifully.
#
# IT IS NOT WIRED INTO ci-host AND THAT IS DELIBERATE. It needs torch, which
# lives on the WINDOWS python in this environment and not in the WSL build
# (CLAUDE.md's own note), plus a 610 MB GGUF and a 2.2 GB f32 conversion. The
# skip is loud and names what is missing, the same rule test-gguf and test-wpt
# follow: a missing corpus is not a regression in the code under test.
#
# THE BOUND IS ARGUED, NOT FITTED. Both sides hold bit-identical weights (one
# Q8_0 dequantisation, handed to both), so the only difference is f32
# accumulation order against a float64 reference. 1e-4 of the logit scale is
# what that predicts; measured worst over 22 positions and 5 prompts is
# 8.91e-06, more than a decade inside it. A structural error is not a near
# miss -- the interleaved/NEOX pairing alone reads 0.59 of scale.
# THE ORACLE'S PYTHON IS NOT THE BUILD'S PYTHON, and on this machine it cannot
# be. The build runs in WSL; torch is installed on the WINDOWS interpreter and
# not in WSL (CLAUDE.md's environment note says so, and `python3 -c "import
# torch"` in WSL is a ModuleNotFoundError). WSL's binfmt interop makes the
# Windows one reachable as python.exe, which is what this default uses -- so
# `make test-qwen-logits` works from the WSL build like every other target,
# rather than being a thing you have to remember to run from another shell.
#
# It is a variable so the same recipe works anywhere torch is on $PATH under
# a different name: make test-qwen-logits QWEN_PY=python3
#
# THE PATHS CROSS A SHELL BOUNDARY, so every file these recipes hand to
# QWEN_PY has to be one BOTH sides can open. They are all repo-relative and
# the working directory is the repo, which is the whole requirement -- an
# absolute /mnt/d path would be correct in WSL and meaningless to python.exe.
QWEN_PY ?= python.exe
QWEN_F32 ?= build/qwen3_f32.lm
test-qwen-logits: test-qwen-logits-negctl
	@if [ ! -f "$(QWEN_F32)" ] || [ ! -f "$(GGUF)" ]; then \
	   echo "test-qwen-logits: SKIP -- need both:"; \
	   echo "    $(GGUF)   (610 MB, not a repo artifact)"; \
	   echo "    $(QWEN_F32)  (2.2 GB, build it with:"; \
	   echo "      build/lmshape --weights $(GGUF) --dtype f32 --no-qemb \\"; \
	   echo "        --seq 512 --out $(QWEN_F32))"; \
	   echo "  A missing corpus is not a regression in the code under test."; \
	   exit 0; \
	 fi; \
	 $(QWEN_PY) tools/qwen_verify.py --lm $(QWEN_F32) --gguf $(GGUF) --bound 1e-4 --wsl

# --- test-qwen-logits-negctl -----------------------------------------------
#
# THE CONTROL FOR THE GATE ABOVE, and it is the PLAUSIBLE wrong build rather
# than an absurd one: -DLM_IGNORE_HEADER_ARCH is exactly what c/lib/nn/infer.c
# did before the header could carry the rope base and the rope pairing, and it
# is what any build that missed that change still does. It reads the same file,
# loads every weight correctly, runs at the same speed, and prints the same
# `LOGITLM ... rope=neox/1e+06 eps=1e-06` describe line -- because
# lm_describe() reads the HEADER, which is right, while the arithmetic ignores
# it. THE FILE SAYS THE CORRECT THING AND THE RUN DOES THE WRONG THING, with
# nothing in between to notice. That is the whole failure mode this line of
# work is arranged against, reproduced on a switch.
#
# MEASURED, both directions, same prompt and same reference:
#     shipped build     max|d| 3.29e-05  = 1.89e-06 of scale, greedy " Paris"
#     control           max|d| 10.9      = 0.626    of scale, greedy " the"
# Five orders of magnitude. A structural error in this arithmetic is never a
# near miss, which is what makes the 1e-4 bound above safe to be loose.
#
# It compares ONE position rather than driving the whole sweep, because the
# sweep's cost is 22 torch forward passes and the property here is not subtle.
test-qwen-logits-negctl:
	@if [ ! -f "$(QWEN_F32)" ] || [ ! -f "$(GGUF)" ]; then \
	   echo "test-qwen-logits-negctl: SKIP -- same corpus as test-qwen-logits."; \
	   exit 0; \
	 fi; \
	 set -e; mkdir -p $(BUILD); \
	 $(CC) $(NN_CF) -DLM_IGNORE_HEADER_ARCH -o $(BUILD)/lm_host_negarch \
	     c/apps/lm/lm.c c/lib/nn/infer.c c/lib/nn/model.c $(NN_SRC) -lm; \
	 $(BUILD)/lm_host_negarch -m $(QWEN_F32) --ids 785,6722,315,9625,374 \
	     -n 0 --greedy --budget 0 --dump-logits $(BUILD)/negarch.bin > /dev/null; \
	 $(QWEN_PY) tools/qwen_oracle.py --gguf $(GGUF) --ids 785,6722,315,9625,374 \
	     --out $(BUILD)/negarch_ref.bin --fp64 > /dev/null; \
	 set +e; \
	 $(QWEN_PY) tools/lmcmp.py $(BUILD)/negarch.bin $(BUILD)/negarch_ref.bin \
	     --bound 1e-4 --label "negctl" > $(BUILD)/negarch.log 2>&1; rc=$$?; \
	 set -e; cat $(BUILD)/negarch.log; \
	 if [ "$$rc" -eq 0 ]; then \
	   echo "test-qwen-logits-negctl: FAILED -- the control PASSED the gate."; \
	   echo "  Ignoring the header rope base and pairing must move the logits"; \
	   echo "  far outside the bound. That it did not means the gate is not"; \
	   echo "  measuring the position encoding at all."; \
	   exit 1; \
	 fi; \
	 echo "test-qwen-logits-negctl: ok -- the control fails the 1e-4 bound"; \
	 echo "  (see the max|d| and the moved greedy token above)"

# --- three harnesses that existed and were RUN BY NOBODY -------------------
#
# tests/unit/{lmshape,kvbudget,quant4}_test.c are 139 checks that no target in
# any tests/*.mk named. They are exactly the shape CLAUDE.md counts and warns
# about: a harness that compiles, looks like coverage, and executes never.
#
# WHAT THAT COST, concretely, because it is not hypothetical. lmshape_test.c
# carried an assertion that lm_forward REFUSES a decoupled head_dim -- a
# placeholder from when the format could express the shape and the forward pass
# could not. infer.c implemented it; the assertion then said the feature does
# not work, and stayed green-looking because nothing ran it. It was found only
# because a signature change forced a rebuild, and it is corrected in place
# with the history written into the file.
#
# They are one rule rather than three because they share a compile line
# exactly, and naming them on ci-host WITHOUT a rule to run them is the
# failure mode CLAUDE.md names: it satisfies the audit and still runs nothing.
.PHONY: test-lm-units
test-lm-units:
	@mkdir -p $(BUILD); rc=0; \
	 for t in lmshape kvbudget quant4; do \
	   $(CC) $(NN_CF) -o $(BUILD)/lm_$${t}_test tests/unit/$${t}_test.c \
	       c/lib/nn/model.c c/lib/nn/infer.c c/lib/nn/kvcache.c $(NN_SRC) -lm \
       || { echo "test-lm-units: $${t}_test FAILED TO BUILD"; rc=1; continue; }; \
	   $(BUILD)/lm_$${t}_test || rc=1; \
	 done; exit $$rc

