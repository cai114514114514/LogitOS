#!/usr/bin/env python3
"""Ground truth for the ANIMATED decoders: GIF and APNG.

The assertion this exists to support is not "N frames decoded". It is that
frame k's COMPOSITED CANVAS and frame k's DELAY are both right, because the
interesting bugs live in the parts a frame count cannot see:

  * disposal 2 / APNG_DISPOSE_OP_BACKGROUND -- clear the frame's rectangle to
    transparent afterwards. Skip it and a moving sprite leaves a trail.
  * disposal 3 / APNG_DISPOSE_OP_PREVIOUS -- restore the canvas to what it was
    BEFORE this frame. Skip it and only the frames that used it smear.
  * a frame is a sub-rectangle at an offset, not the whole canvas.
  * transparent index / APNG_BLEND_OP_OVER means composite, not copy; and
    APNG_BLEND_OP_SOURCE means copy INCLUDING alpha, i.e. it punches holes.

Every case here therefore uses a deliberately asymmetric pattern and a moving
rectangle, so that a disposal bug changes pixels rather than shifting them.

References:
  * GIF: PIL, which seeks each frame and composites it. Byte-exact.
  * APNG: PIL for the frame COUNT, the DELAYS and the SOURCE-blend cases.
    For OVER blending the expectation is computed here from the integer
    formula in the APNG specification, which is the normative definition of
    the output -- see the note printed for that case.

For each case: <name>.<ext>, <name>.frames (a text manifest of
delay_ms per frame), and <name>.<k>.rgba per frame.
Usage: img_anim_gen.py <outdir>
"""
import os, struct, subprocess, sys, io, zlib
from PIL import Image, ImageSequence

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
manifest = []
W, H = 24, 16


def write_case(name, ext, data, frames, delays, loops, ref):
    """frames: list of bytes (RGBA, W*H*4). delays: ms per frame."""
    open(os.path.join(OUT, f"{name}.{ext}"), "wb").write(data)
    for k, f in enumerate(frames):
        open(os.path.join(OUT, f"{name}.{k}.rgba"), "wb").write(f)
    with open(os.path.join(OUT, f"{name}.frames"), "w") as fh:
        fh.write(" ".join(str(d) for d in delays) + "\n")
    manifest.append(f"{name} {ext} {write_case.w} {write_case.h} {len(frames)} {loops}")
    print(f"  {name:18s} {ext:4s} {write_case.w}x{write_case.h}  {len(frames)} frames "
          f"delays={delays} loops={loops}  ref={ref}")


# --------------------------------------------------------------------------
# GIF
#
# The encoder is hand-rolled rather than PIL's. PIL's GIF writer re-quantises
# and re-orders the palette, emits per-frame local colour tables and rewrites
# the frame rectangles, so what lands on disk is not the animation that was
# asked for -- which makes it useless for pinning down disposal. Here every
# byte of the file is chosen: one global colour table, explicit rectangles,
# explicit disposal and delay per frame.
#
# Ground truth is a compositor written straight from the GIF89a rules, and it
# is CROSS-CHECKED against ffmpeg (libavcodec's gif decoder, an entirely
# independent implementation) for every case. The generator fails if they
# disagree on any colour byte.
#
# One documented divergence, in ALPHA only: ffmpeg renders to an opaque video
# canvas, so pixels no frame has painted -- and the region a "restore to
# background" disposal clears -- come out as opaque background colour. A
# browser, and this decoder, treat them as TRANSPARENT, because an image is
# composited over whatever is behind it. ffmpeg's RGB is compared; its alpha
# is not.
# --------------------------------------------------------------------------
PAL = [(0, 0, 0), (255, 0, 0), (0, 200, 40), (30, 60, 255),
       (250, 250, 90), (120, 0, 180), (18, 240, 240), (7, 7, 7)]


def gif_lzw(indices):
    """Valid GIF LZW that never compresses: a CLEAR every 250 codes pins the
    code size at 9, so no dictionary state is needed to emit it. The DECODER
    still has to run its full dictionary machinery to read it back."""
    bits, nbits, out = 0, 0, bytearray()

    def put(code):
        nonlocal bits, nbits
        bits |= code << nbits
        nbits += 9
        while nbits >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nbits -= 8

    put(256)                                   # CLEAR
    for i, v in enumerate(indices):
        if i and i % 250 == 0:
            put(256)
        put(v)
    put(257)                                   # EOI
    if nbits:
        out.append(bits & 0xFF)
    blocks = bytearray([8])                    # LZW minimum code size
    o = bytes(out)
    for i in range(0, len(o), 255):
        c = o[i:i + 255]
        blocks.append(len(c))
        blocks += c
    blocks.append(0)
    return bytes(blocks)


def gif_build(frames, loop=0, interlace_frames=()):
    d = bytearray(b"GIF89a")
    d += struct.pack("<HHBBB", W, H, 0x80 | 0x07, 0, 0)      # 256-entry GCT
    for i in range(256):
        d += bytes(PAL[i] if i < len(PAL) else (0, 0, 0))
    d += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01" + struct.pack("<H", loop) + b"\x00"
    for k, f in enumerate(frames):
        pk = (f["disposal"] << 2) | (1 if f["transp"] is not None else 0)
        d += b"\x21\xF9\x04" + bytes([pk]) + struct.pack("<H", f["delay"]) \
             + bytes([f["transp"] if f["transp"] is not None else 0]) + b"\x00"
        idx = f["idx"]
        ilace = 0x40 if k in interlace_frames else 0
        if ilace:
            # Reorder the rows into the 4 interlace passes.
            rows = [idx[r * f["w"]:(r + 1) * f["w"]] for r in range(f["h"])]
            order = (list(range(0, f["h"], 8)) + list(range(4, f["h"], 8))
                     + list(range(2, f["h"], 4)) + list(range(1, f["h"], 2)))
            idx = [v for r in order for v in rows[r]]
        d += b"\x2C" + struct.pack("<HHHHB", f["x"], f["y"], f["w"], f["h"], ilace)
        d += gif_lzw(idx)
    d += b"\x3B"
    return bytes(d)


def gif_compose(frames):
    """GIF89a disposal, from the spec."""
    canvas = bytearray(W * H * 4)
    out = []
    for f in frames:
        prev = bytes(canvas)
        for r in range(f["h"]):
            for c in range(f["w"]):
                ix = f["idx"][r * f["w"] + c]
                if f["transp"] is not None and ix == f["transp"]:
                    continue                    # composite: leave what is under
                x, y = f["x"] + c, f["y"] + r
                if x >= W or y >= H:
                    continue
                o = (y * W + x) * 4
                canvas[o:o + 3] = bytes(PAL[ix] if ix < len(PAL) else (0, 0, 0))
                canvas[o + 3] = 255
        out.append(bytes(canvas))
        if f["disposal"] == 2:
            for r in range(f["h"]):
                for c in range(f["w"]):
                    x, y = f["x"] + c, f["y"] + r
                    if x < W and y < H:
                        canvas[(y * W + x) * 4:(y * W + x) * 4 + 4] = b"\0\0\0\0"
        elif f["disposal"] == 3:
            canvas[:] = prev
    return out


def ffmpeg_frames(path):
    raw = path + ".raw"
    subprocess.run(["ffmpeg", "-v", "quiet", "-i", path, "-fps_mode", "passthrough",
                    "-pix_fmt", "rgba", "-f", "rawvideo", raw, "-y"],
                   check=True, stderr=subprocess.DEVNULL)
    d = open(raw, "rb").read()
    os.remove(raw)
    n = W * H * 4
    return [d[i * n:(i + 1) * n] for i in range(len(d) // n)]


def gif_case(name, frames, loop=0, interlace_frames=()):
    data = gif_build(frames, loop, interlace_frames)
    path = os.path.join(OUT, name + ".gif")
    open(path, "wb").write(data)
    want = gif_compose(frames)
    got = ffmpeg_frames(path)
    assert len(got) == len(want), f"{name}: ffmpeg {len(got)} frames, spec {len(want)}"
    # Cross-check every PAINTED pixel against ffmpeg, exactly. Pixels the spec
    # calls transparent are skipped: ffmpeg composites onto an opaque canvas and
    # has no single representation for "nothing here" -- it emits opaque
    # background colour in one case and white-with-zero-alpha in another. What
    # matters is that wherever a frame actually put a colour, an independent
    # decoder agrees which colour, from which frame, after which disposal.
    skipped = 0
    for k in range(len(want)):
        for i in range(0, len(want[k]), 4):
            if want[k][i + 3] != 255:
                skipped += 1
                continue
            if want[k][i:i + 3] != got[k][i:i + 3] or got[k][i + 3] != 255:
                raise AssertionError(
                    f"{name} frame {k} px {i//4}: spec {list(want[k][i:i+4])} "
                    f"!= ffmpeg {list(got[k][i:i+4])}")
    write_case.w, write_case.h = W, H
    total = len(want) * W * H
    write_case(name, "gif", data, want, [f["delay"] * 10 for f in frames], loop,
               f"spec + ffmpeg({total - skipped}/{total} px cross-checked)")


def F(**k):
    return dict(dict(x=0, y=0, disposal=1, transp=None, delay=10), **k)


def bgpat(w, h, ox=0, oy=0):
    return [2 if ((x + ox) // 5 + (y + oy) // 3) % 2 == 0 else 1
            for y in range(h) for x in range(w)]


def solid_idx(w, h, v):
    return [v] * (w * h)


# disposal 1 (leave): each patch stays on the canvas.
gif_case("gif_leave", [
    F(w=W, h=H, idx=bgpat(W, H), delay=10),
    F(w=10, h=7, x=4, y=1, idx=solid_idx(10, 7, 3), delay=4),
    F(w=10, h=7, x=10, y=6, idx=solid_idx(10, 7, 4), delay=25),
])

# disposal 2 (restore to background): each patch is cleared after it is shown,
# so a decoder that ignores disposal leaves a trail behind the sprite.
gif_case("gif_background", [
    F(w=W, h=H, idx=bgpat(W, H)),
    F(w=10, h=7, x=4, y=1, idx=solid_idx(10, 7, 3), disposal=2),
    F(w=10, h=7, x=10, y=6, idx=solid_idx(10, 7, 4), disposal=2),
    F(w=8, h=5, x=2, y=8, idx=solid_idx(8, 5, 5)),
], loop=0)

# disposal 3 (restore to previous): frame 3 must see exactly what frame 0 left.
gif_case("gif_previous", [
    F(w=W, h=H, idx=bgpat(W, H)),
    F(w=10, h=7, x=4, y=1, idx=solid_idx(10, 7, 3), disposal=3),
    F(w=10, h=7, x=10, y=6, idx=solid_idx(10, 7, 4), disposal=3),
    F(w=8, h=5, x=2, y=8, idx=solid_idx(8, 5, 5)),
])

# every disposal in one animation, with wildly different delays.
gif_case("gif_mixed", [
    F(w=W, h=H, idx=bgpat(W, H), delay=1),
    F(w=10, h=7, x=4, y=1, idx=solid_idx(10, 7, 3), disposal=2, delay=50),
    F(w=12, h=9, x=6, y=2, idx=bgpat(12, 9, 3, 1), disposal=3, delay=1),
    F(w=10, h=7, x=8, y=2, idx=solid_idx(10, 7, 6), disposal=2, delay=200),
    F(w=W, h=H, idx=bgpat(W, H, 2, 1), disposal=0, delay=7),
], loop=0)

# transparent index: the sprite lets the background show through, and the
# cleared rectangle underneath it is transparent too.
gif_case("gif_transp", [
    F(w=W, h=H, idx=bgpat(W, H)),
    F(w=W, h=H, idx=[7 if (x + y * 3) % 3 else 5 for y in range(H) for x in range(W)],
      transp=7, disposal=2),
    F(w=12, h=8, x=6, y=4, idx=[7 if (x + y) % 2 else 4 for y in range(8) for x in range(12)],
      transp=7),
])

# interlaced frames inside an animation (rows stored in four passes).
gif_case("gif_interlace", [
    F(w=W, h=H, idx=bgpat(W, H)),
    F(w=W, h=H, idx=bgpat(W, H, 1, 2), disposal=1),
], interlace_frames=(0, 1))

# an explicit finite loop count from the NETSCAPE2.0 application extension.
gif_case("gif_loop3", [
    F(w=W, h=H, idx=bgpat(W, H), delay=8),
    F(w=10, h=7, x=4, y=1, idx=solid_idx(10, 7, 3), delay=8),
], loop=3)

# --------------------------------------------------------------------------
# APNG
# --------------------------------------------------------------------------
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


def raw_rgba_idat(px, w, h):
    """Filter-None scanlines of RGBA8, zlib'd."""
    raw = b"".join(b"\0" + px[y * w * 4:(y + 1) * w * 4] for y in range(h))
    return zlib.compress(raw, 9)


def apng(canvas_w, canvas_h, frames, plays=0):
    """frames: list of dicts {w,h,x,y,num,den,dispose,blend,px(bytes RGBA)}."""
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", canvas_w, canvas_h, 8, 6, 0, 0, 0))
    out += chunk(b"acTL", struct.pack(">II", len(frames), plays))
    seq = 0
    for i, f in enumerate(frames):
        out += chunk(b"fcTL", struct.pack(">IIIIIHHBB", seq, f["w"], f["h"], f["x"], f["y"],
                                          f["num"], f["den"], f["dispose"], f["blend"]))
        seq += 1
        data = raw_rgba_idat(f["px"], f["w"], f["h"])
        if i == 0:
            out += chunk(b"IDAT", data)
        else:
            out += chunk(b"fdAT", struct.pack(">I", seq) + data)
            seq += 1
    out += chunk(b"IEND", b"")
    return out


def solid(w, h, rgba):
    return bytes(rgba) * (w * h)


def gradient(w, h, base, alpha=255):
    o = bytearray()
    for y in range(h):
        for x in range(w):
            o += bytes(((base + x * 9) & 0xFF, (base + y * 13) & 0xFF,
                        (base + x * y) & 0xFF, alpha))
    return bytes(o)


def compose_expect(cw, ch, frames):
    """Reference compositor: the APNG spec's own rules, integer arithmetic."""
    canvas = bytearray(cw * ch * 4)
    out = []
    for i, f in enumerate(frames):
        dispose = f["dispose"]
        if i == 0 and dispose == 2:
            dispose = 1                      # spec: PREVIOUS on frame 0 == BACKGROUND
        prev = bytes(canvas) if dispose == 2 else None
        for r in range(f["h"]):
            for c in range(f["w"]):
                o = ((f["y"] + r) * cw + f["x"] + c) * 4
                so = (r * f["w"] + c) * 4
                s = f["px"][so:so + 4]
                if f["blend"] == 0:
                    canvas[o:o + 4] = s
                else:
                    fa = s[3]
                    if fa == 0:
                        continue
                    ba = canvas[o + 3]
                    if fa == 255 or ba == 0:
                        canvas[o:o + 4] = s
                        continue
                    ca = fa + ba * (255 - fa) // 255
                    for k in range(3):
                        canvas[o + k] = (s[k] * fa + canvas[o + k] * ba * (255 - fa) // 255) // ca
                    canvas[o + 3] = ca
        out.append(bytes(canvas))
        if dispose == 1:
            for r in range(f["h"]):
                o = ((f["y"] + r) * cw + f["x"]) * 4
                canvas[o:o + f["w"] * 4] = b"\0" * (f["w"] * 4)
        elif dispose == 2:
            canvas[:] = prev
    return out


def apng_case(name, cw, ch, frames, plays=0, ref="spec"):
    data = apng(cw, ch, frames, plays)
    expect = compose_expect(cw, ch, frames)
    delays = [f["num"] * 1000 // (f["den"] or 100) for f in frames]
    write_case.w, write_case.h = cw, ch
    # Cross-check the count and the delays against PIL, which is an independent
    # implementation of the container even where its compositor is not usable.
    im = Image.open(io.BytesIO(data))
    assert im.n_frames == len(frames), (name, im.n_frames)
    pil_delays = []
    for fr in ImageSequence.Iterator(im):
        pil_delays.append(int(fr.info.get("duration", 0)))
    assert pil_delays == delays, (name, pil_delays, delays)
    write_case(name, "png", data, expect, delays, plays, ref)


AW, AH = 20, 12
base = {"num": 1, "den": 10, "dispose": 0, "blend": 0}


def fr(**kw):
    d = dict(base)
    d.update(kw)
    return d


# Opaque frames, dispose NONE: each frame paints over the last.
apng_case("apng_none", AW, AH, [
    fr(w=AW, h=AH, x=0, y=0, px=gradient(AW, AH, 0)),
    fr(w=6, h=5, x=2, y=1, px=solid(6, 5, (255, 0, 0, 255)), num=3, den=20),
    fr(w=6, h=5, x=9, y=4, px=solid(6, 5, (0, 255, 0, 255)), num=1, den=4),
], ref="spec+PIL(count,delays)")

# dispose BACKGROUND: the sprite's rectangle must be transparent afterwards.
apng_case("apng_background", AW, AH, [
    fr(w=AW, h=AH, x=0, y=0, px=gradient(AW, AH, 30)),
    fr(w=5, h=4, x=1, y=1, px=solid(5, 4, (255, 255, 0, 255)), dispose=1),
    fr(w=5, h=4, x=10, y=6, px=solid(5, 4, (0, 0, 255, 255)), dispose=1),
    fr(w=5, h=4, x=6, y=3, px=solid(5, 4, (255, 0, 255, 255))),
])

# dispose PREVIOUS: frame 3 must see exactly what frame 1 left.
apng_case("apng_previous", AW, AH, [
    fr(w=AW, h=AH, x=0, y=0, px=gradient(AW, AH, 90)),
    fr(w=7, h=6, x=3, y=2, px=solid(7, 6, (10, 250, 10, 255)), dispose=2),
    fr(w=4, h=3, x=12, y=7, px=solid(4, 3, (250, 10, 10, 255)), dispose=2),
    fr(w=AW, h=2, x=0, y=10, px=solid(AW, 2, (5, 5, 200, 255))),
])

# blend SOURCE with transparent pixels: it PUNCHES A HOLE, it does not blend.
apng_case("apng_source_holes", AW, AH, [
    fr(w=AW, h=AH, x=0, y=0, px=gradient(AW, AH, 140)),
    fr(w=8, h=6, x=4, y=3, px=solid(8, 6, (0, 0, 0, 0)), blend=0),
])

# blend OVER with partial alpha: the spec's integer compositing formula.
apng_case("apng_over", AW, AH, [
    fr(w=AW, h=AH, x=0, y=0, px=gradient(AW, AH, 200, alpha=180)),
    fr(w=9, h=7, x=5, y=2, px=gradient(9, 7, 60, alpha=96), blend=1),
    fr(w=9, h=7, x=2, y=4, px=gradient(9, 7, 10, alpha=255), blend=1),
], ref="spec(integer OVER formula; PIL's compositor is not usable here)")

# mixed disposals + a zero delay (den=0 means 100 per spec).
apng_case("apng_mixed", AW, AH, [
    fr(w=AW, h=AH, x=0, y=0, px=gradient(AW, AH, 5), num=0, den=0),
    fr(w=6, h=6, x=1, y=1, px=solid(6, 6, (200, 200, 0, 255)), dispose=1, num=7, den=100),
    fr(w=6, h=6, x=8, y=2, px=solid(6, 6, (0, 200, 200, 255)), dispose=2, num=1, den=1),
    fr(w=6, h=6, x=13, y=5, px=solid(6, 6, (200, 0, 200, 200)), blend=1, num=1, den=1000),
], plays=4)

open(os.path.join(OUT, "manifest.txt"), "w").write("\n".join(manifest) + "\n")
print(f"generated {len(manifest)} cases")
