#!/usr/bin/env python3
"""qwen_verify.py -- is the model this machine runs really Qwen3?

Drives tools/qwen_oracle.py and build/lm_host over several prompts and EVERY
position of each, and reports the worst disagreement against a bound that is
argued rather than fitted.

WHY EVERY POSITION AND NOT JUST THE LAST. Position 0 cannot see a rope bug at
all -- the angle is pos * inv_freq, so at pos 0 every angle is zero, cos is 1
and sin is 0 whatever the frequency table holds. That is not a hypothetical:
during this work an oracle running on an UNINITIALISED inv_freq buffer agreed
with us to 1.5e-06 of scale at position 0 and was wrong at every position
after it. A check that samples one position can be the one position where the
bug is invisible, so the sweep walks k = 1..len(ids) and compares the last row
of each prefix -- which is every position exactly once.

THE TWO COMPARISONS ARE DIFFERENT QUESTIONS AND GET DIFFERENT BOUNDS:

  --lm build/qwen3_f32.lm   ARCHITECTURE. Both sides hold bit-identical
                            weights (the same Q8_0 dequantisation), so the only
                            difference is f32 accumulation order against a f64
                            reference. Bounded, and the bound is tight.

  --lm build/qwen3.lm       QUANTISATION. Our weights are Q4_AFFINE, block 64
                            (c/lib/nn/quant4.h). The difference is dominated by
                            that and is REPORTED rather than bounded -- putting
                            a pass/fail on it would make one number answer two
                            questions, and the honest output is the measurement.

Runs on the WINDOWS python (torch); calls the WSL build for our side.
"""
import sys, os, subprocess, argparse, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# The prompts, with their token ids PRECOMPUTED so this file does not depend on
# a tokenizer at all. Each was produced by tools/lmtok.py and cross-checked
# against transformers' AutoTokenizer -- two implementations agreeing, which is
# what tools/lmtok.py's own gate is for.
PROMPTS = [
    ("The capital of France is",   [785, 6722, 315, 9625, 374]),
    ("def add(a, b):",             [750, 912, 2877, 11, 293, 1648]),
    ("Once upon a time",           [12522, 5193, 264, 882]),
    ("1 + 1 =",                    [16, 488, 220, 16, 284]),
    ("<chinese: rengongzhineng>",  [104455, 20412]),
]


def run(cmd, **kw):
    # errors="replace": wsl.exe prints a UTF-16 proxy banner on stderr that is
    # not decodable in the host console's codepage, and letting that raise
    # would report a Unicode error where the real result is a logit file.
    return subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", **kw)


def ours(lm, ids, out, wsl):
    a = ",".join(str(i) for i in ids)
    if wsl:
        cmd = ["wsl.exe", "-e", "bash", "-lc",
               "cd /mnt/d/ststem && ./build/lm_host -m %s --ids '%s' -n 0 "
               "--greedy --budget 0 --dump-logits %s" % (lm, a, out)]
    else:
        cmd = ["./build/lm_host", "-m", lm, "--ids", a, "-n", "0",
               "--greedy", "--budget", "0", "--dump-logits", out]
    r = run(cmd)
    if not os.path.exists(out) or os.path.getsize(out) == 0:
        raise SystemExit("qwen_verify: lm_host wrote no logits.\n%s\n%s"
                         % ((r.stdout or "")[-2000:], (r.stderr or "")[-2000:]))
    return np.fromfile(out, dtype="<f4").astype(np.float64)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--lm", required=True)
    ap.add_argument("--gguf", default="build/qwen/Qwen3-0.6B-Q8_0.gguf")
    ap.add_argument("--dir",  default="build/qwen")
    ap.add_argument("--bound", type=float, default=None,
                    help="fail if max|d| exceeds this multiple of the logit "
                         "scale; omit to report only")
    ap.add_argument("--wsl", action="store_true")
    a = ap.parse_args(argv)

    for f in (a.lm.replace("build/", "build/"), a.gguf):
        if not os.path.exists(f):
            print("qwen_verify: SKIP -- %s is absent.\n"
                  "  The weights are 610 MB and are not a repo artifact; this "
                  "is a skip and not a pass." % f)
            return 77

    import torch
    import qwen_oracle
    g, W = qwen_oracle.dequant_all(a.gguf)
    cfg, model = qwen_oracle.build_torch(a.dir, W, torch.float64, "cpu")

    # INSIDE THE REPO, not the system temp: the run below crosses into WSL,
    # where a Windows %TEMP% path does not exist. .agtmp is visible from both
    # sides as the same bytes, which is the whole requirement.
    tmp = ".agtmp"
    os.makedirs(tmp, exist_ok=True)
    worst_all = 0.0
    worst_rel = 0.0
    where = ""
    ndis = 0
    ntot = 0
    print()
    print("%-28s %4s %-12s %-11s %-9s %s"
          % ("prompt", "pos", "max|d|", "of scale", "greedy", ""))
    for name, ids in PROMPTS:
        with torch.no_grad():
            out = model(torch.tensor([ids])).logits[0].to(torch.float64).numpy()
        for k in range(1, len(ids) + 1):
            o = ours(a.lm, ids[:k], tmp + "/qv_ours.bin", a.wsl)
            r = out[k - 1]
            d = float(np.abs(o - r).max())
            sc = float(max(np.abs(r).max(), 1e-30))
            same = int(o.argmax()) == int(r.argmax())
            ntot += 1
            if not same:
                ndis += 1
            if d / sc > worst_rel:
                worst_rel, worst_all = d / sc, d
                where = "%s pos %d" % (name, k - 1)
            print("%-28s %4d %-12.5g %-11.4g %-9s %s"
                  % (name if k == 1 else "", k - 1, d, d / sc,
                     "SAME" if same else "DIFFER",
                     "" if same else "  <-- argmax moved"))
    print()
    print("worst        %.6g  (%.4g of scale) at %s" % (worst_all, worst_rel, where))
    print("greedy       %d/%d positions agree" % (ntot - ndis, ntot))
    if a.bound is None:
        print("bound        none given -- reported, not judged")
        return 0
    ok = worst_rel <= a.bound and ndis == 0
    print("bound        %.3g of scale -> %s" % (a.bound, "PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
