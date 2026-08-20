#!/usr/bin/env python3
"""qwen_oracle.py -- the PyTorch reference this line is judged against.

WHY THIS FILE EXISTS, stated once. Everything before it established that the
BYTES are Qwen's: tools/gguf.c reads the Q8_0 blocks, gguf_check.py dequantises
the same elements with numpy and demands EXACT equality, --matvec checks one
matrix product against float64, and the name map refuses an unmapped tensor, a
shape disagreement and an unclaimed GGUF tensor alike. All of that is about
whether the right numbers landed in the right places.

NONE OF IT IS ABOUT WHETHER THE MODEL RUNS. A file can hold every weight
correctly and still be evaluated with the wrong position encoding, the wrong
norm epsilon, the QK-norm on the wrong side of the rotation, or the query and
key heads paired wrongly under GQA -- and EVERY ONE of those produces finite
logits, unchanged throughput, and fluent, confident, wrong text. There is no
sample you can read that distinguishes them. So the comparison is made on the
LOGITS, against an implementation this repository did not write.

THE ORACLE IS transformers' OWN Qwen3ForCausalLM AND NOT A HAND-WRITTEN
REFERENCE, and that choice is the point rather than a convenience. A reference
written here would encode this session's understanding of Qwen3 -- which is
exactly the thing under test. If I mis-ordered QK-norm and RoPE in c/lib/nn and
mis-ordered them again in the oracle, the two would agree beautifully and the
number would mean nothing. transformers' implementation was written by other
people from the model card; it is the definition of what Qwen3 means.

BOTH SIDES START FROM THE SAME f32. The GGUF's Q8_0 blocks are dequantised
ONCE, here, and the identical arrays are (a) loaded into the torch model and
(b) already in build/qwen3_f32.lm, which tools/lmshape.c wrote from the same
blocks through the same arithmetic. So the residual between the two sides is
OUR error and nothing else -- no bf16-vs-Q8_0 gap is folded into it, because
neither side ever sees the bf16 original.

WHAT THIS DOES NOT ESTABLISH is written at the top of the final report and is
worth repeating here: this is Qwen3-0.6B AS QUANTISED TO Q8_0 BY WHOEVER BUILT
THE GGUF. The bf16 release is upstream of both sides and is not on this disk,
so the Q8_0 step is unmeasured by construction -- an error introduced there is
shared by the oracle and the implementation and cannot appear in any number
below.

Runs on the WINDOWS python (torch 2.13.0+cu132), not in WSL: the build runs in
WSL and torch is not installed there. It is not in the build graph for that
reason -- it is an instrument, like tools/gguf_check.py.
"""
import sys, os, json, argparse
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import gguf_check


def dequant_all(path, verbose=True):
    """Every GGUF tensor as f32 numpy, keyed by GGUF name.

    Reuses gguf_check.Reader rather than re-deriving Q8_0 here. That reader is
    the ORACLE half of test-gguf: it is checked element-for-element against
    tools/gguf.c with an EXACT equality, so a second dequantiser in this file
    would be a third opinion nothing gates -- and the one place a constant
    factor of 1.0001 could hide unnoticed.
    """
    g = gguf_check.Reader(path)
    out = {}
    for name in g.order:
        out[name] = g.full(name)
    if verbose:
        print("dequantised %d tensors from %s" % (len(out), os.path.basename(path)))
    return g, out


def build_torch(model_dir, W, dtype, device):
    import torch
    from transformers import AutoConfig, Qwen3ForCausalLM

    cfg = AutoConfig.from_pretrained(model_dir)
    # THE CONFIG IS THE SHIPPED ONE, READ, NOT RECONSTRUCTED FROM THE GGUF.
    # That makes rope_theta and rms_norm_eps a SECOND independent source for
    # the two constants c/lib/nn now carries in the header -- if the GGUF's
    # metadata and the config.json disagreed, the assertion below would say so
    # instead of both sides quietly using the GGUF's value.
    with torch.device("meta"):
        model = Qwen3ForCausalLM(cfg)
    model = model.to_empty(device=device)

    nl = cfg.num_hidden_layers
    m = {"model.embed_tokens.weight": "token_embd.weight",
         "model.norm.weight":         "output_norm.weight"}
    per = {"input_layernorm":            "attn_norm",
           "self_attn.q_proj":           "attn_q",
           "self_attn.k_proj":           "attn_k",
           "self_attn.v_proj":           "attn_v",
           "self_attn.q_norm":           "attn_q_norm",
           "self_attn.k_norm":           "attn_k_norm",
           "self_attn.o_proj":           "attn_output",
           "post_attention_layernorm":   "ffn_norm",
           "mlp.gate_proj":              "ffn_gate",
           "mlp.up_proj":                "ffn_up",
           "mlp.down_proj":              "ffn_down"}
    for i in range(nl):
        for k, v in per.items():
            m["model.layers.%d.%s.weight" % (i, k)] = "blk.%d.%s.weight" % (i, v)

    sd = {}
    for tname, gname in m.items():
        a = W[gname]
        # NO TRANSPOSE ANYWHERE, and it is asserted rather than assumed. GGUF
        # dims are [ne0, ne1] with ne0 contiguous, so Reader.full() returns
        # [ne1, ne0] = [out_features, in_features] -- which is torch's Linear
        # layout already. The assert is what makes that a check instead of a
        # belief: four of these tensors are ASYMMETRIC (q_proj 2048x1024 and
        # o_proj 1024x2048 are each other's transpose), so a convention error
        # cannot satisfy both.
        want = tuple(model.state_dict()[tname].shape)
        # A 1-D GGUF tensor (every norm weight) comes back as [1, n] because
        # Reader.rows() always returns a 2-D block. Reshaped, never
        # transposed: the two are the same operation on a vector and
        # DIFFERENT on a matrix, so the squeeze is restricted to the case
        # where it cannot hide an orientation bug.
        if a.ndim == 2 and a.shape[0] == 1 and len(want) == 1:
            a = a.reshape(-1)
        if a.shape != want:
            raise SystemExit("qwen_oracle: %s is %s and torch wants %s -- a "
                             "transpose here would be silent in the output"
                             % (tname, a.shape, want))
        sd[tname] = torch.from_numpy(np.ascontiguousarray(a)).to(dtype)

    missing, unexpected = model.load_state_dict(sd, strict=False, assign=True)

    # ---------------------------------------------------------------------
    # to_empty() LEAVES EVERY NON-PERSISTENT BUFFER AS UNINITIALISED MEMORY,
    # AND inv_freq IS ONE. THIS COST HOURS AND IS THE MOST INSTRUCTIVE FAILURE
    # IN THIS WHOLE TASK, SO IT IS WRITTEN OUT RATHER THAN QUIETLY FIXED.
    #
    # Qwen3RotaryEmbedding registers `inv_freq` as a NON-PERSISTENT buffer:
    # it is computed in __init__ from config.rope_theta, and it is deliberately
    # absent from the state dict (it is derived, not learned). Building under
    # `torch.device("meta")` never runs that computation, `to_empty()` then
    # allocates the buffer WITHOUT INITIALISING IT, and `load_state_dict` has
    # nothing to put there because no checkpoint carries it. The result is a
    # model that loads cleanly, reports no missing keys, produces finite
    # logits, and rotates every position by an angle read out of whatever was
    # in that page -- i.e. the exact failure mode this file's opening
    # paragraph warns about, in the ORACLE rather than in the implementation.
    #
    # HOW IT SURVIVED THE OBVIOUS CHECK. The first comparison run against it
    # was at position 0, which agreed to 1.5e-06 of scale and was read as
    # "embeddings, norms, projections and the head are all correct". They are
    # -- but position 0 cannot see this bug AT ALL: the angle is pos *
    # inv_freq, so at pos 0 every angle is zero, cos is 1 and sin is 0 NO
    # MATTER WHAT inv_freq HOLDS. The one position chosen to isolate the
    # arithmetic from the rope is the one position at which a corrupt rope
    # table is invisible.
    #
    # It was not even reproducible: two runs get two different pages, so the
    # "reference" moved between invocations while looking perfectly stable in
    # any single run's output.
    #
    # The fix is to rebuild the rotary module for real. The ASSERT is the part
    # that matters -- inv_freq is recomputed here from the config by hand and
    # compared against what the module holds, so a future refactor that breaks
    # the re-init again fails loudly instead of returning to a silent wrong
    # answer.
    import math
    for mod in model.modules():
        if not hasattr(mod, "inv_freq"):
            continue
        theta = float(cfg.rope_parameters["rope_theta"])
        hdim = int(cfg.head_dim)
        want = torch.tensor(
            [1.0 / (theta ** ((2 * i) / hdim)) for i in range(hdim // 2)],
            dtype=torch.float32, device=device)
        for nm in ("inv_freq", "original_inv_freq"):
            if hasattr(mod, nm):
                setattr(mod, nm, want.clone())
        got = mod.inv_freq.detach().to(torch.float64).cpu().numpy()
        ref = want.to(torch.float64).cpu().numpy()
        err = float(np.abs(got - ref).max())
        if not (err == 0.0):
            raise SystemExit("qwen_oracle: rotary inv_freq is not the table "
                             "config.rope_theta implies (max|d| %g)" % err)
        print("rotary         inv_freq re-initialised from rope_theta=%g "
              "(to_empty() leaves it uninitialised; see the comment)" % theta)

    # Any OTHER non-persistent buffer would have exactly the same problem, so
    # the survivors are listed rather than assumed harmless. A buffer that is
    # all zeros or contains a subnormal is the signature of the bug above.
    for n, b in model.named_buffers():
        if "inv_freq" in n:
            continue
        print("buffer         %s %s -- NOT re-initialised, check it is derived"
              % (n, tuple(b.shape)))
    # lm_head is TIED, so it is legitimately absent from `sd`; anything else
    # missing is a weight torch would then run with whatever to_empty() left
    # in memory -- uninitialised, finite, and catastrophic in silence.
    missing = [k for k in missing if k != "lm_head.weight"]
    if missing or unexpected:
        raise SystemExit("qwen_oracle: missing=%s unexpected=%s" % (missing, unexpected))
    model.tie_weights()
    model.eval()
    return cfg, model


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--gguf",  default="build/qwen/Qwen3-0.6B-Q8_0.gguf")
    ap.add_argument("--dir",   default="build/qwen")
    ap.add_argument("--ids",   required=True,
                    help="comma-separated token ids -- the same list fed to lm")
    ap.add_argument("--out",   required=True, help="write the f32 logit row here")
    ap.add_argument("--fp64",  action="store_true",
                    help="run the reference in float64. The bound this whole "
                         "comparison is read against is OUR f32 accumulation "
                         "error, so a f64 reference removes the oracle's own "
                         "rounding from the residual instead of adding to it.")
    a = ap.parse_args(argv)

    import torch
    ids = [int(x) for x in a.ids.replace(" ", "").split(",") if x != ""]

    g, W = dequant_all(a.gguf)

    # THE TWO CONSTANTS, CROSS-CHECKED BETWEEN TWO INDEPENDENT SOURCES. The
    # GGUF metadata and the shipped config.json are written by different tools
    # from different inputs; agreement here is what makes "rope base 1000000"
    # a fact about the model rather than a fact about one file.
    cj = json.load(open(os.path.join(a.dir, "config.json")))
    gg_rope = g.kv["qwen3.rope.freq_base"]
    gg_eps  = g.kv["qwen3.attention.layer_norm_rms_epsilon"]
    print("rope_theta   gguf %r   config.json %r   %s"
          % (gg_rope, cj["rope_theta"],
             "AGREE" if float(gg_rope) == float(cj["rope_theta"]) else "*** DISAGREE ***"))
    print("rms_norm_eps gguf %r   config.json %r   %s"
          % (gg_eps, cj["rms_norm_eps"],
             "AGREE" if abs(float(gg_eps) - float(cj["rms_norm_eps"])) < 1e-12
             else "*** DISAGREE ***"))
    if float(gg_rope) != float(cj["rope_theta"]):
        raise SystemExit("qwen_oracle: the two sources disagree about the rope "
                         "base. Refused -- this is the number whose corruption "
                         "has no symptom.")

    dtype = torch.float64 if a.fp64 else torch.float32
    dev = "cpu"    # f64 has no CUDA path worth the trouble and this is 0.6B
    cfg, model = build_torch(a.dir, W, dtype, dev)

    with torch.no_grad():
        t = torch.tensor([ids], dtype=torch.long, device=dev)
        out = model(t).logits[0]           # [len(ids), vocab]

    last = out[-1].to(torch.float32).cpu().numpy()
    last.astype("<f4").tofile(a.out)
    print("ids            %d tokens, last id %d" % (len(ids), ids[-1]))
    print("logits         [%d, %d] -> wrote row %d (%s) to %s"
          % (out.shape[0], out.shape[1], out.shape[0] - 1,
             "float64 reference" if a.fp64 else "float32 reference", a.out))
    top = np.argsort(-last)[:5]
    print("reference top5 %s" % [(int(i), round(float(last[i]), 4)) for i in top])
    print("greedy         %d" % int(np.argmax(last)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
