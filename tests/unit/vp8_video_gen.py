#!/usr/bin/env python3
"""Generate VP8 INTER-frame (video) corpus + reference YUV for test-vp8-video.

THE ORACLE IS ffmpeg'S OWN vp8 DECODER, on the identical IVF bytes -- the same
"decode the identical bytes with the real thing" shape webp_vp8_gen.py uses for
the key-frame decoder (dwebp -nofancy). VP8 reconstruction is exactly specified
integer arithmetic, so the bar is byte equality, not a tolerance.

Cases are chosen to hit the three places an inter decoder usually breaks first
(see rust/src/vp8_inter.rs's own module doc and CLAUDE.md's TLS/H.264-style
"traps" sections for the general shape of this argument):

  basic_p      moderate motion, single-pass, NO golden/altref refresh beyond
               the ordinary per-frame refresh_last -- proves plain P-frame
               decode (MV read, motion compensation, coefficient decode)
               before anything reference-related is layered on.
  motion_p     more motion (more/larger MVs, more split-MV), still no hidden
               altref frames, larger frame -- more macroblocks, more edge
               cases in find_near_mvs' neighbour census.
  altref_hidden  two-pass with auto-alt-ref + lag-in-frames, which is the ONLY
               reliable way found (by trial) to make libvpx actually emit an
               INVISIBLE alt-ref frame (show_frame=0) rather than merely using
               golden/altref bookkeeping between ordinarily-shown frames. This
               is the case that actually exercises decode_frame's "shown"
               plumbing and the copy-before-refresh reference ordering
               (RFC 6386 9.7/9.8) -- the two things golden/altref decoders get
               wrong first.
  odd_dims     a size that is not a multiple of 16 (macroblock edge padding +
               the width/height crop on the inter path, not just the keyframe
               path already covered by test-webp-vp8).

A CRITICAL GOTCHA, hit while building this gate and recorded so the next
person does not lose a day to it: the FIRST run of this generator decoded the
altref_hidden reference with plain `ffmpeg -i x.ivf ... rawvideo`, no
`-vsync 0`. ffmpeg's default CFR muxing duplicated 4 frames on that stream --
the one with real hidden alt-ref packets -- to pad the output back up to the
container's nominal frame rate, and that produced 70 OF 72 SHOWN FRAMES
"MISMATCHING" against the decoder under test. Every one of those 70 was a
harness artifact: the reference file's frame N had stopped being decoded frame
N the moment ffmpeg inserted the first duplicate, so everything downstream
compared against the wrong picture. It read exactly like a real golden/altref
bug -- small, consistent, non-resyncing differences starting a couple of
frames after the first hidden frame, which is the shape a genuine reference-
bookkeeping bug would have. Re-decoding the SAME ivf with `-vsync 0`
(passthrough: one output frame per decoded frame, in bitstream order, no
duplication) made the reference's frame count equal the true shown-frame
count and dropped the mismatch count to 0 of 72. The decoder was never wrong;
the oracle was miscounting. `-vsync 0` below is not a style preference.
"""
import os
import shutil
import struct
import subprocess
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "build/vp8video"
os.makedirs(OUT, exist_ok=True)

FFMPEG = shutil.which("ffmpeg")
if not FFMPEG:
    sys.stderr.write(
        "vp8_video_gen: ffmpeg not found. It is the reference encoder AND\n"
        "decoder for this corpus; without it there is no oracle and the gate\n"
        "would be asserting our decoder against itself.  apt-get install ffmpeg\n")
    sys.exit(2)


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        sys.stderr.write("command failed: %s\n%s\n" % (" ".join(cmd), r.stdout.decode("utf8", "replace")))
        sys.exit(1)


def encode_singlepass(name, lavfi, size, duration, rate, extra):
    # -t on the OUTPUT, not duration= on the filter: testsrc/testsrc2 accept a
    # duration= filter option but mandelbrot does not ("Could not set
    # non-existent option 'duration'", found by running this generator) -- -t
    # works uniformly across every lavfi source instead of special-casing one.
    ivf = os.path.join(OUT, name + ".ivf")
    run([FFMPEG, "-y", "-f", "lavfi", "-i", "%s=size=%s:rate=%d" % (lavfi, size, rate),
         "-t", str(duration), "-c:v", "libvpx"] + extra + ["-f", "ivf", ivf])
    return ivf


def encode_twopass(name, lavfi, size, duration, rate, extra):
    ivf = os.path.join(OUT, name + ".ivf")
    log = os.path.join(OUT, name + ".vp8log")
    src = "%s=size=%s:rate=%d" % (lavfi, size, rate)
    run([FFMPEG, "-y", "-f", "lavfi", "-i", src, "-t", str(duration), "-c:v", "libvpx"] + extra +
        ["-pass", "1", "-passlogfile", log, "-f", "null", os.devnull])
    run([FFMPEG, "-y", "-f", "lavfi", "-i", src, "-t", str(duration), "-c:v", "libvpx"] + extra +
        ["-pass", "2", "-passlogfile", log, "-f", "ivf", ivf])
    return ivf


def count_hidden(ivf):
    data = open(ivf, "rb").read()
    hdrlen = struct.unpack("<H", data[6:8])[0]
    off = hdrlen
    total = hidden = 0
    while off + 12 <= len(data):
        sz = struct.unpack("<I", data[off:off + 4])[0]
        off += 12
        tag = data[off] | (data[off + 1] << 8) | (data[off + 2] << 16)
        if ((tag >> 4) & 1) == 0:
            hidden += 1
        off += sz
        total += 1
    return total, hidden


def make_ref(ivf, yuv):
    # -vsync 0: see the module doc above -- this is load-bearing, not a
    # cosmetic flag. Without it a stream with any hidden alt-ref frame
    # produces a reference file whose frame N is not decoded frame N.
    run([FFMPEG, "-y", "-vsync", "0", "-i", ivf, "-pix_fmt", "yuv420p", "-f", "rawvideo", yuv])


manifest = []

cases = [
    ("basic_p", lambda: encode_singlepass(
        "basic_p", "testsrc", "64x48", "1", 10,
        ["-b:v", "400k", "-g", "100", "-auto-alt-ref", "0"])),
    ("motion_p", lambda: encode_singlepass(
        "motion_p", "mandelbrot", "80x64", "2", 15,
        ["-b:v", "600k", "-g", "300", "-auto-alt-ref", "1", "-lag-in-frames", "16",
         "-arnr-maxframes", "7", "-arnr-strength", "4"])),
    ("altref_hidden", lambda: encode_twopass(
        "altref_hidden", "testsrc2", "96x64", "3", 24,
        ["-b:v", "800k", "-auto-alt-ref", "1", "-lag-in-frames", "25", "-g", "240"])),
    ("odd_dims", lambda: encode_singlepass(
        "odd_dims", "testsrc", "70x50", "1", 12,
        ["-b:v", "350k", "-g", "60", "-auto-alt-ref", "0"])),
]

for name, fn in cases:
    ivf = fn()
    yuv = os.path.join(OUT, name + ".yuv")
    make_ref(ivf, yuv)
    total, hidden = count_hidden(ivf)
    shown = total - hidden
    ysz = os.path.getsize(yuv)
    manifest.append(name)
    print("  %-14s %3d packets  %2d hidden  %2d shown  ref %d B" % (name, total, hidden, shown, ysz))

with open(os.path.join(OUT, "manifest.txt"), "w") as f:
    f.write("\n".join(manifest) + "\n")

print("generated %d VP8 inter-frame cases" % len(manifest))
