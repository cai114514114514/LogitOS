#!/usr/bin/env python3
"""lmcmp.py -- compare two raw f32 logit rows and say whether they agree.

Reads two files of `vocab` little-endian f32 (what `lm --dump-logits` and
tools/qwen_oracle.py --out both write) and reports the worst absolute
difference, the logit scale it should be read against, and whether GREEDY picks
the same token.

THE BOUND IS ARGUED, NOT FITTED, and which bound applies depends on which
comparison is being made:

  f32 ours vs f64 reference
      The only difference between the two sides is FLOATING-POINT
      ACCUMULATION ORDER. Every weight is bit-identical (both come from the
      same Q8_0 dequantisation) and every constant is the file's. A logit is a
      dot product of 1024 terms at the end of 28 layers, each of which is a
      dot product of 1024 or 3072 terms. f32 carries ~1.2e-7 relative, and
      error through k sequential dependent accumulations grows like sqrt(k)
      at best and k at worst; with the residual stream re-normalised at every
      layer the growth is bounded rather than compounding. A residual of
      1e-4 * scale is what that predicts; 1e-2 * scale would mean something
      structural, not rounding.

  q4 ours vs f64 reference
      Now the weights DIFFER: Q4_AFFINE, block 64, f32 scale + f32 min
      (c/lib/nn/quant4.h). That is a much larger and entirely expected error,
      and the honest thing to report is the number rather than a verdict --
      so --bound is not given for that comparison and the tool prints what it
      measured.

WHAT MAKES THE VERDICT MEAN ANYTHING IS THE GREEDY LINE, not the max-abs. Two
logit rows can differ by a lot in the tail and still produce identical text;
they can also differ by very little and cross at the top. Both are printed
because they answer different questions, and the argmax is the one the
generation loop actually consumes.
"""
import sys, argparse
import numpy as np


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("ref")
    ap.add_argument("--bound", type=float, default=None,
                    help="fail if max|d| exceeds this MULTIPLE of the logit "
                         "scale. Omitted = report only, exit 0.")
    ap.add_argument("--label", default="")
    a = ap.parse_args(argv)

    x = np.fromfile(a.ours, dtype="<f4").astype(np.float64)
    y = np.fromfile(a.ref,  dtype="<f4").astype(np.float64)
    if x.size == 0 or y.size == 0:
        raise SystemExit("lmcmp: %s is empty -- a comparison against a file "
                         "that was never written is the failure this check "
                         "exists to prevent, so it is refused rather than "
                         "reported as agreement"
                         % (a.ours if x.size == 0 else a.ref))
    if x.shape != y.shape:
        raise SystemExit("lmcmp: %d vs %d logits -- different vocabularies"
                         % (x.size, y.size))

    d = np.abs(x - y)
    scale = float(max(np.abs(x).max(), np.abs(y).max()))
    worst = float(d.max())
    at = int(d.argmax())
    gx, gy = int(x.argmax()), int(y.argmax())

    # The top-k overlap is reported because it degrades GRADUALLY where the
    # argmax is a step function: a model that is slightly wrong keeps the same
    # argmax for a while and loses the ordering below it first.
    kx = set(np.argsort(-x)[:10].tolist())
    ky = set(np.argsort(-y)[:10].tolist())

    print("%s%d logits, scale %.4f" % (a.label and a.label + ": ", x.size, scale))
    print("  max|d|        %.6g  (%.3g of scale) at id %d" % (worst, worst / scale, at))
    print("  mean|d|       %.6g" % float(d.mean()))
    print("  greedy        ours %d   ref %d   %s"
          % (gx, gy, "SAME" if gx == gy else "*** DIFFER ***"))
    print("  top-10 overlap %d/10" % len(kx & ky))
    print("  ours top5     %s" % [(int(i), round(float(x[i]), 4)) for i in np.argsort(-x)[:5]])
    print("  ref  top5     %s" % [(int(i), round(float(y[i]), 4)) for i in np.argsort(-y)[:5]])

    if a.bound is None:
        return 0
    ok = (worst <= a.bound * scale) and (gx == gy)
    print("  bound         %.3g of scale = %.6g -> %s"
          % (a.bound, a.bound * scale, "PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
