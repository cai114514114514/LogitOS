#!/usr/bin/env python3
"""Generate the lossy-WebP (VP8 key-frame) conformance corpus.

THE ORACLE IS libwebp'S OWN DECODER, on the identical bytes: `dwebp -nofancy`.
VP8 reconstruction is exactly specified integer arithmetic -- as H.264's is,
and for the same reason -- so the bar is BYTE EQUALITY, not a tolerance. There
is no "close enough" here; a single wrong sample means a rule was misread.

`-nofancy` selects box chroma upsampling, which is what our decoder does and
what jpeg.c does. libwebp's default is a 4-tap "fancy" upsample; matching that
too is a separate piece of work and would confuse two questions into one, so
the flag pins the comparison to the decoder rather than the resampler.

WHAT EACH KNOB REACHES, because a corpus of pretty pictures tests one path:

  -q          the quantiser index, hence every dequantisation table row, and
              at low q the "everything is DC" macroblock the token decoder
              exits early on
  -segments   the per-segment quantiser and filter-level machinery, plus the
              segment tree in the macroblock header. -segments 1 turns it off
              entirely, which is a DIFFERENT header path, not a milder one
  -f / -sharpness  the loop filter's level and its interior limit; -f 0
              disables the filter and so tests reconstruction alone
  -strong / -nostrong  the normal (6-tap, chroma too) filter against the
              simple (4-tap, luma only) one -- two separate kernels
  -m          the encoder's search effort, which is what actually decides how
              many macroblocks come out B_PRED. -m 6 on sharp content is the
              only reliable way to get the ten 4x4 predictors exercised
  odd sizes   a frame that is not a whole number of macroblocks, so the
              rightmost/bottom partial macroblocks and the crop are real

Usage: webp_vp8_gen.py <outdir>
Writes <name>.webp + <name>.ref (raw RGBA from dwebp) + manifest.txt.
"""
import os
import shutil
import subprocess
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "build/webpvp8"
os.makedirs(OUT, exist_ok=True)

CWEBP = shutil.which("cwebp")
DWEBP = shutil.which("dwebp")
if not CWEBP or not DWEBP:
    sys.stderr.write(
        "webp_vp8_gen: cwebp/dwebp not found. They are the reference encoder and\n"
        "decoder for this format; without them there is no oracle and the gate\n"
        "would be asserting our decoder against itself.  apt-get install webp\n")
    sys.exit(2)

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("webp_vp8_gen: PIL is required to synthesise the sources\n")
    sys.exit(2)


def src_smooth(w, h):
    """Gradients: mostly-DC macroblocks, low AC energy."""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 255) // max(w - 1, 1),
                        (y * 255) // max(h - 1, 1),
                        ((x + y) * 255) // max(w + h - 2, 1))
    return im


def src_sharp(w, h):
    """Hard edges and thin lines: the content that makes an encoder choose
    B_PRED and spend real coefficients, which is what exercises the ten 4x4
    predictors and the deep token categories."""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            checker = 255 if ((x // 3) ^ (y // 5)) & 1 else 20
            diag = 250 if (x + y) % 11 < 2 else 0
            ring = 200 if abs((x - w // 2) ** 2 + (y - h // 2) ** 2 - (min(w, h) // 3) ** 2) \
                < min(w, h) * 4 else 0
            px[x, y] = (checker, max(diag, ring), (x * 7 + y * 13) & 0xFF)
    return im


def src_noise(w, h):
    """A deterministic pseudo-random field: maximum entropy, so the encoder
    cannot be clever and the token decoder walks the long path everywhere."""
    im = Image.new("RGB", (w, h))
    px = im.load()
    s = 12345
    for y in range(h):
        for x in range(w):
            vals = []
            for _ in range(3):
                s = (1103515245 * s + 12345) & 0x7FFFFFFF
                vals.append((s >> 16) & 0xFF)
            px[x, y] = tuple(vals)
    return im


def src_alpha(w, h):
    """RGBA. A lossy WebP has no alpha of its own -- VP8 does not have the
    channel -- so transparency arrives as a SEPARATE ALPH chunk holding an
    8-bit plane, compressed as a headerless VP8L stream (alpha in the green
    channel) and spatially filtered. Both the ramp and the hard cut-out are
    here because the filters predict very differently on each."""
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            ramp = (x * 255) // max(w - 1, 1)
            hole = 0 if (x - w // 2) ** 2 + (y - h // 2) ** 2 < (min(w, h) // 4) ** 2 else 255
            px[x, y] = ((x * 5) & 0xFF, (y * 7) & 0xFF, 200, min(ramp, hole))
    return im


SOURCES = {"smooth": src_smooth, "sharp": src_sharp, "noise": src_noise,
           "alpha": src_alpha}

# (name, source, size, extra cwebp args)
CASES = [
    # quality sweep on smooth content -- the quantiser range
    ("q0_smooth", "smooth", (64, 48), ["-q", "0"]),
    ("q25_smooth", "smooth", (64, 48), ["-q", "25"]),
    ("q75_smooth", "smooth", (64, 48), ["-q", "75"]),
    ("q100_smooth", "smooth", (64, 48), ["-q", "100"]),
    # sharp content at high effort: B_PRED and the deep token categories
    ("sharp_m6", "sharp", (64, 48), ["-q", "90", "-m", "6"]),
    ("sharp_m0", "sharp", (64, 48), ["-q", "90", "-m", "0"]),
    ("sharp_q100", "sharp", (48, 32), ["-q", "100", "-m", "6"]),
    ("noise_q90", "noise", (64, 48), ["-q", "90", "-m", "6"]),
    # odd dimensions: partial macroblocks at the right and bottom edge
    ("odd_23x17", "sharp", (23, 17), ["-q", "80", "-m", "6"]),
    ("odd_1x1", "smooth", (1, 1), ["-q", "80"]),
    ("odd_17x1", "sharp", (17, 1), ["-q", "80"]),
    ("odd_1x17", "sharp", (1, 17), ["-q", "80"]),
    ("odd_65x33", "sharp", (65, 33), ["-q", "70", "-m", "6"]),
    # the loop filter: off, simple, normal, and the sharpness ladder
    ("filt_off", "sharp", (64, 48), ["-q", "80", "-f", "0"]),
    ("filt_simple", "sharp", (64, 48), ["-q", "80", "-f", "60", "-nostrong"]),
    ("filt_strong", "sharp", (64, 48), ["-q", "80", "-f", "60", "-strong"]),
    ("filt_max", "sharp", (64, 48), ["-q", "50", "-f", "100", "-strong"]),
    ("sharp0", "sharp", (64, 48), ["-q", "60", "-f", "60", "-sharpness", "0"]),
    ("sharp4", "sharp", (64, 48), ["-q", "60", "-f", "60", "-sharpness", "4"]),
    ("sharp7", "sharp", (64, 48), ["-q", "60", "-f", "60", "-sharpness", "7"]),
    # segmentation: 1 is a different header path from 4, not a weaker one
    ("seg1", "sharp", (80, 64), ["-q", "70", "-segments", "1", "-sns", "0"]),
    ("seg2", "sharp", (80, 64), ["-q", "70", "-segments", "2", "-sns", "80"]),
    ("seg4", "sharp", (80, 64), ["-q", "70", "-segments", "4", "-sns", "100"]),
    # bigger, so several token partitions and a long macroblock run
    ("big_noise", "noise", (256, 192), ["-q", "85", "-m", "5"]),
    ("big_sharp", "sharp", (240, 176), ["-q", "75", "-m", "6"]),
    # lossy + ALPH: the alpha plane, its two compression methods and its three
    # spatial filters. -alpha_method 0 stores the plane raw; 1 compresses it as
    # a headerless VP8L stream.
    ("alpha_raw", "alpha", (48, 32), ["-q", "80", "-alpha_method", "0"]),
    ("alpha_lossless", "alpha", (48, 32), ["-q", "80", "-alpha_method", "1"]),
    ("alpha_f_none", "alpha", (64, 48), ["-q", "80", "-alpha_filter", "none"]),
    ("alpha_f_fast", "alpha", (64, 48), ["-q", "80", "-alpha_filter", "fast"]),
    ("alpha_f_best", "alpha", (64, 48), ["-q", "80", "-alpha_filter", "best"]),
    ("alpha_odd", "alpha", (23, 17), ["-q", "90", "-m", "6"]),
]


def read_pam(path):
    d = open(path, "rb").read()
    i = d.index(b"ENDHDR\n") + 7
    return d[i:]


manifest = []
for name, src, (w, h), extra in CASES:
    png = os.path.join(OUT, name + ".png")
    webp = os.path.join(OUT, name + ".webp")
    pam = os.path.join(OUT, name + ".pam")
    SOURCES[src](w, h).save(png)
    subprocess.run([CWEBP, "-quiet", png] + extra + ["-o", webp], check=True)
    subprocess.run([DWEBP, "-quiet", "-nofancy", "-pam", webp, "-o", pam], check=True)
    ref = read_pam(pam)
    if len(ref) != w * h * 4:
        raise SystemExit("webp_vp8_gen: %s reference is %d bytes, want %d"
                         % (name, len(ref), w * h * 4))
    open(os.path.join(OUT, name + ".ref"), "wb").write(ref)
    os.remove(pam)
    os.remove(png)
    manifest.append("%s %d %d" % (name, w, h))
    print("  %-14s %4dx%-4d %6d B  %s"
          % (name, w, h, os.path.getsize(webp), " ".join(extra)))

open(os.path.join(OUT, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
print("generated %d lossy-WebP cases" % len(manifest))
