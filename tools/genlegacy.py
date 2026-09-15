#!/usr/bin/env python3
# Generate the legacy-codec host-test matrix for Cinepak, MS Video 1, RPZA
# and QuickTime Animation (RLE): source patterns -> ffmpeg-encoded AVI/MOV
# samples (where an ffmpeg ENCODER exists) or hand-authored bitstreams
# (where none does) -> ffmpeg-DECODED reference pixels, plus one corrupted
# fixture per codec for the negative control.
#
# Usage: genlegacy.py <outdir>
#
# For every case, writes:
#   <outdir>/<case>.manifest   key=value lines the C harness parses
#   <outdir>/<case>.src        the container file (AVI/MOV) frames are sliced from
#   <outdir>/<case>.ref        the reference raw pixels (ffmpeg's own decode)
#   <outdir>/<case>.corrupt    (negative-control cases only) one standalone
#                               corrupted frame chunk, no container needed
#
# WHY "-pix_fmt pal8" IS THE ORACLE FOR EVERY PALETTE CASE: ffmpeg's rawvideo
# muxer dumps a PAL8 frame as width*height raw INDEX bytes immediately
# followed by the 1024-byte AVPALETTE (256 x 4-byte 0xAARRGGBB, native order)
# -- confirmed empirically (ref size == 3 * (w*h + 1024) for a 3-frame case).
# That means the byte-exact gate for MS Video 1's 8-bit mode and QTRLE's
# 1/2/4/8bpp modes never needs to reproduce ffmpeg's PALETTE at all, only the
# INDEX plane this decoder actually produces -- which is exactly what
# legacy.h's output convention already promises callers (see its header
# comment). This sidesteps a whole class of risk (replicating ffmpeg's
# default-palette / greyscale-ramp logic in qtpalette.c) that has nothing to
# do with whether the RLE/VQ decode itself is correct, which is the only
# thing this phase owns.
import os, sys, struct, subprocess, random, zlib

def die(msg):
    sys.stderr.write("genlegacy.py: " + msg + "\n")
    sys.exit(1)

def stable_seed(name):
    """A per-case RNG seed that is the SAME across processes and machines.
    Python's built-in hash() on a str is randomised per interpreter run
    (PYTHONHASHSEED, on by default since 3.3) -- found the hard way here:
    the ORIGINAL code at every synth_frames()/synth_gray() call site below
    read `seed=hash(case) & 0xffff`, which silently regenerated a DIFFERENT
    corpus every invocation of this script, which is exactly the kind of thing
    CLAUDE.md's own
    apparatus warnings are about -- the byte-exact gate never noticed
    because ffmpeg's own decoder was always the oracle for whatever content
    got generated, but a case whose PASS/FAIL depends on encoder heuristics
    tied to content (e.g. how many strips Cinepak's encoder chooses) is not
    reproducible run to run, and the CINEPAK_CONTROL_ABS_Y1 negative
    control's reddened count is exactly such a case (2/6 one run, 0/6 the
    next, same script, same machine). zlib.crc32 is stable by construction:
    same bytes in, same 32-bit value out, every process, every machine."""
    return zlib.crc32(name.encode("utf-8")) & 0xffff

def run(args, **kw):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)
    if r.returncode != 0:
        die("command failed: " + " ".join(args) + "\n" + r.stderr.decode("latin1", "replace"))
    return r.stdout

# ---------------------------------------------------------------- patterns
# Deterministic RGB24 source frames. Every pattern leaves SOME region
# identical between consecutive frames (so an encoder has a real reason to
# emit skip/delta blocks) and changes another region (so it has a real
# reason not to just skip everything) -- a codec's inter-frame path is only
# exercised by content shaped like that.
def synth_frames(w, h, n, seed):
    rnd = random.Random(seed)
    base = [[(0, 0, 0)] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            base[y][x] = ((x * 255) // max(1, w - 1), (y * 255) // max(1, h - 1),
                          ((x + y) * 128 // max(1, w + h - 2)) & 255)
    out = []
    for f in range(n):
        buf = bytearray(w * h * 3)
        # Left half: static gradient (identical every frame -> skip/delta
        # blocks). Right half: a moving stripe + per-frame noise speckle
        # (changes every frame -> real coded blocks every time).
        moving_x = (f * max(1, w // 4)) % w
        for y in range(h):
            for x in range(w):
                if x < w // 2:
                    r, g, b = base[y][x]
                else:
                    r, g, b = base[y][x]
                    if (x - moving_x) % w < max(2, w // 8):
                        r, g, b = (255 - r) & 255, (g + 64) & 255, (255 - b) & 255
                    if rnd.random() < 0.03:
                        r, g, b = rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)
                o = (y * w + x) * 3
                buf[o] = r; buf[o + 1] = g; buf[o + 2] = b
        out.append(bytes(buf))
    return out

def write_rgb24(path, frames):
    with open(path, "wb") as f:
        for fr in frames:
            f.write(fr)

# ----------------------------------------------------------- packet slicing
def probe_packets(path):
    # ffprobe's csv writer prints fields in ITS OWN internal order, not the
    # order named in -show_entries (verified empirically: asking for
    # "pos,size" still prints size before pos) -- so this asks for
    # "key=value" lines instead of trusting column position at all.
    txt = run(["ffprobe", "-v", "error", "-select_streams", "v",
               "-show_entries", "packet=pos,size", "-of",
               "default=noprint_wrappers=1:nokey=0", path]).decode()
    pkts = []
    cur = {}
    for line in txt.splitlines():
        line = line.strip()
        if not line:
            continue
        k, v = line.split("=", 1)
        cur[k] = int(v)
        if "pos" in cur and "size" in cur:
            pkts.append((cur["pos"], cur["size"]))
            cur = {}
    return pkts

def decode_ref(path, pixfmt, outraw):
    run(["ffmpeg", "-v", "error", "-y", "-i", path, "-pix_fmt", pixfmt, "-f", "rawvideo", outraw])

# --------------------------------------------------------------- manifests
def write_manifest(outdir, case, **kv):
    with open(os.path.join(outdir, case + ".manifest"), "w") as f:
        for k, v in kv.items():
            f.write("%s=%s\n" % (k, v))

def add_packets(kv, pkts):
    kv["frames"] = len(pkts)
    for i, (pos, size) in enumerate(pkts):
        kv["frame%d_pos" % i] = pos
        kv["frame%d_size" % i] = size
    return kv

# =================================================== minimal container writers
# Used only for the HAND-AUTHORED bitstreams (no ffmpeg encoder reaches
# these depths/modes) -- everything ffmpeg's own encoder can produce goes
# through ffmpeg's own (real, tested) muxer instead. These write just enough
# structure for ffmpeg's DEMUXER to accept the file as the real oracle.

def _riff_chunk(fourcc, payload):
    assert len(fourcc) == 4
    out = fourcc + struct.pack("<I", len(payload)) + payload
    if len(payload) & 1:
        out += b"\x00"
    return out

def _riff_list(listtype, payload_chunks):
    assert len(listtype) == 4
    body = listtype + b"".join(payload_chunks)
    return _riff_chunk(b"LIST", body)

def build_avi_msvc(width, height, bitcount, palette_bgr0, frames):
    """palette_bgr0: list of 256 (B,G,R) tuples, or None for bitcount==16."""
    fps_num, fps_den = 10, 1
    avih = struct.pack("<IIIIIIIIII", 1000000 // fps_num, 0, 0, 0x10, len(frames), 0,
                        1, 0, width, height) + b"\x00" * 16
    strh = (b"vids" + b"MSVC" +
            struct.pack("<IHHIIIIIIII", 0, 0, 0, 0, fps_den, fps_num, 0, len(frames), 0,
                        0xFFFFFFFF, 0) +
            struct.pack("<hhhh", 0, 0, width, height))
    # BITMAPINFOHEADER: biCompression is 4 literal fourcc bytes in the file,
    # not a packed little-endian dword of the ASCII value -- ff_get_bmp_header
    # reads it with avio_rl32 and callers then compare it against a fourcc
    # table, so the byte order matters and "just append the ASCII bytes" is
    # the correct (and only) way to get that comparison to match.
    strf = (struct.pack("<I", 40) + struct.pack("<i", width) + struct.pack("<i", height) +
            struct.pack("<H", 1) + struct.pack("<H", bitcount) + b"MSVC" +
            struct.pack("<IiiII", 0, 0, 0, 256 if bitcount == 8 else 0, 0))
    if bitcount == 8:
        assert palette_bgr0 is not None and len(palette_bgr0) == 256
        for (b, g, r) in palette_bgr0:
            strf += struct.pack("<BBBB", b, g, r, 0)

    strl = _riff_list(b"strl", [_riff_chunk(b"strh", strh), _riff_chunk(b"strf", strf)])
    hdrl = _riff_list(b"hdrl", [_riff_chunk(b"avih", avih), strl])

    movi_chunks = [_riff_chunk(b"00dc", fr) for fr in frames]
    movi = _riff_list(b"movi", movi_chunks)

    # idx1: offsets are relative to the first byte after the 'movi' fourcc.
    idx = b""
    off = 4  # past the 'movi' fourcc itself
    for i, fr in enumerate(frames):
        flags = 0x10 if i == 0 else 0
        idx += struct.pack("<4sIII", b"00dc", flags, off, len(fr))
        off += 8 + len(fr) + (len(fr) & 1)
    idx1 = _riff_chunk(b"idx1", idx)

    riff_body = b"AVI " + hdrl + movi + idx1
    return _riff_chunk(b"RIFF", riff_body)

def _mov_atom(fourcc, payload):
    assert len(fourcc) == 4
    return struct.pack(">I", 8 + len(payload)) + fourcc + payload

def build_mov_qtrle(width, height, depth, ctab_rgb, frames):
    """ctab_rgb: list of (r,g,b) 8-bit tuples (length 2**depth), or None for
    depth in (16,24,32). Builds an in-place QuickDraw ColorTable exactly as
    ff_get_qtpalette's "color table ID is 0" branch reads it: colorStart(4)
    colorFlags(2) colorEnd(2), then per entry index(2,ignored) R16 G16 B16
    (only the HIGH byte of each 16-bit component is read)."""
    n = len(frames)
    timescale = 600
    ftyp = _mov_atom(b"ftyp", b"qt  " + struct.pack(">I", 0x20050300) + b"qt  ")

    mdat_payload = b"".join(frames)
    mdat = _mov_atom(b"mdat", mdat_payload)
    mdat_data_start = len(ftyp) + 8  # offset of mdat's payload within the file

    offsets = []
    o = mdat_data_start
    for fr in frames:
        offsets.append(o)
        o += len(fr)

    mvhd_payload = (struct.pack(">I", 0) + struct.pack(">II", 0, 0) +
                     struct.pack(">I", timescale) + struct.pack(">I", n) +
                     struct.pack(">I", 0x00010000) + struct.pack(">H", 0x0100) +
                     b"\x00" * 10 +
                     struct.pack(">9i", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000) +
                     b"\x00" * 24 + struct.pack(">I", 2))
    mvhd = _mov_atom(b"mvhd", mvhd_payload)

    tkhd = (struct.pack(">I", 0x00000007) + struct.pack(">II", 0, 0) +
            struct.pack(">I", 1) + b"\x00\x00\x00\x00" + struct.pack(">I", n) +
            b"\x00" * 8 + struct.pack(">h", 0) + struct.pack(">h", 0) +
            struct.pack(">H", 0) + b"\x00\x00" +
            struct.pack(">9i", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000) +
            struct.pack(">I", width << 16) + struct.pack(">I", height << 16))
    tkhd = _mov_atom(b"tkhd", tkhd)

    mdhd = (struct.pack(">I", 0) + struct.pack(">II", 0, 0) +
            struct.pack(">I", timescale) + struct.pack(">I", n) +
            struct.pack(">H", 0x55c4) + struct.pack(">H", 0))
    mdhd = _mov_atom(b"mdhd", mdhd)

    hdlr = (struct.pack(">I", 0) + b"\x00\x00\x00\x00" + b"vide" + b"\x00" * 12 + b"")
    hdlr = _mov_atom(b"hdlr", hdlr)

    vmhd = _mov_atom(b"vmhd", struct.pack(">I", 1) + struct.pack(">H", 0) + b"\x00" * 6)

    url = _mov_atom(b"url ", struct.pack(">I", 1))
    dref = _mov_atom(b"dref", struct.pack(">I", 0) + struct.pack(">I", 1) + url)
    dinf = _mov_atom(b"dinf", dref)

    ext = (struct.pack(">H", 0) + struct.pack(">H", 0) + b"FFMP" +
           struct.pack(">I", 0) + struct.pack(">I", 0) +
           struct.pack(">H", width) + struct.pack(">H", height) +
           struct.pack(">I", 0x00480000) + struct.pack(">I", 0x00480000) +
           struct.pack(">I", 0) + struct.pack(">H", 1) +
           b"\x00" * 32 +
           struct.pack(">H", depth))
    if ctab_rgb is None:
        ext += struct.pack(">H", 0xFFFF)
    else:
        n_colors = len(ctab_rgb)
        ext += struct.pack(">H", 0)  # colorTableID 0: table follows inline
        ext += struct.pack(">I", 0)          # colorStart
        ext += struct.pack(">H", 0)          # colorFlags
        ext += struct.pack(">H", n_colors - 1)  # colorEnd
        for i, (r, g, b) in enumerate(ctab_rgb):
            ext += struct.pack(">H", i)
            ext += struct.pack(">H", (r << 8) | r)
            ext += struct.pack(">H", (g << 8) | g)
            ext += struct.pack(">H", (b << 8) | b)
    entry_payload = b"\x00" * 6 + struct.pack(">H", 1) + ext
    entry = _mov_atom(b"rle ", entry_payload)
    stsd = _mov_atom(b"stsd", struct.pack(">I", 0) + struct.pack(">I", 1) + entry)

    stts = _mov_atom(b"stts", struct.pack(">I", 0) + struct.pack(">I", 1) +
                      struct.pack(">II", n, 1))
    stsc_entries = struct.pack(">III", 1, 1, 1)
    stsc = _mov_atom(b"stsc", struct.pack(">I", 0) + struct.pack(">I", 1) + stsc_entries)
    stsz = _mov_atom(b"stsz", struct.pack(">I", 0) + struct.pack(">I", 0) +
                      struct.pack(">I", n) + b"".join(struct.pack(">I", len(fr)) for fr in frames))
    stco = _mov_atom(b"stco", struct.pack(">I", 0) + struct.pack(">I", n) +
                      b"".join(struct.pack(">I", off) for off in offsets))
    stbl = _mov_atom(b"stbl", stsd + stts + stsc + stsz + stco)
    minf = _mov_atom(b"minf", vmhd + dinf + stbl)
    mdia = _mov_atom(b"mdia", mdhd + hdlr + minf)
    trak = _mov_atom(b"trak", tkhd + mdia)
    moov = _mov_atom(b"moov", mvhd + trak)

    return ftyp + mdat + moov

def build_mov_rpza(width, height, frames):
    """Minimal MOV wrapper for a hand-authored 'rpza' sample stream: same
    atom skeleton as build_mov_qtrle, with no palette extension -- RPZA is
    fixed 15-bit RGB555 (see legacy_rpza.c's own header, "there is no
    bit-depth axis to this format")."""
    n = len(frames)
    timescale = 600
    ftyp = _mov_atom(b"ftyp", b"qt  " + struct.pack(">I", 0x20050300) + b"qt  ")
    mdat_payload = b"".join(frames)
    mdat = _mov_atom(b"mdat", mdat_payload)
    mdat_data_start = len(ftyp) + 8
    offsets = []
    o = mdat_data_start
    for fr in frames:
        offsets.append(o)
        o += len(fr)
    mvhd_payload = (struct.pack(">I", 0) + struct.pack(">II", 0, 0) +
                     struct.pack(">I", timescale) + struct.pack(">I", n) +
                     struct.pack(">I", 0x00010000) + struct.pack(">H", 0x0100) +
                     b"\x00" * 10 +
                     struct.pack(">9i", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000) +
                     b"\x00" * 24 + struct.pack(">I", 2))
    mvhd = _mov_atom(b"mvhd", mvhd_payload)
    tkhd = (struct.pack(">I", 0x00000007) + struct.pack(">II", 0, 0) +
            struct.pack(">I", 1) + b"\x00\x00\x00\x00" + struct.pack(">I", n) +
            b"\x00" * 8 + struct.pack(">h", 0) + struct.pack(">h", 0) +
            struct.pack(">H", 0) + b"\x00\x00" +
            struct.pack(">9i", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000) +
            struct.pack(">I", width << 16) + struct.pack(">I", height << 16))
    tkhd = _mov_atom(b"tkhd", tkhd)
    mdhd = (struct.pack(">I", 0) + struct.pack(">II", 0, 0) +
            struct.pack(">I", timescale) + struct.pack(">I", n) +
            struct.pack(">H", 0x55c4) + struct.pack(">H", 0))
    mdhd = _mov_atom(b"mdhd", mdhd)
    hdlr = _mov_atom(b"hdlr", struct.pack(">I", 0) + b"\x00\x00\x00\x00" + b"vide" + b"\x00" * 12)
    vmhd = _mov_atom(b"vmhd", struct.pack(">I", 1) + struct.pack(">H", 0) + b"\x00" * 6)
    url = _mov_atom(b"url ", struct.pack(">I", 1))
    dref = _mov_atom(b"dref", struct.pack(">I", 0) + struct.pack(">I", 1) + url)
    dinf = _mov_atom(b"dinf", dref)
    ext = (struct.pack(">H", 0) + struct.pack(">H", 0) + b"FFMP" +
           struct.pack(">I", 0) + struct.pack(">I", 0) +
           struct.pack(">H", width) + struct.pack(">H", height) +
           struct.pack(">I", 0x00480000) + struct.pack(">I", 0x00480000) +
           struct.pack(">I", 0) + struct.pack(">H", 1) +
           b"\x00" * 32 + struct.pack(">H", 16) + struct.pack(">H", 0xFFFF))
    entry_payload = b"\x00" * 6 + struct.pack(">H", 1) + ext
    entry = _mov_atom(b"rpza", entry_payload)
    stsd = _mov_atom(b"stsd", struct.pack(">I", 0) + struct.pack(">I", 1) + entry)
    stts = _mov_atom(b"stts", struct.pack(">I", 0) + struct.pack(">I", 1) + struct.pack(">II", n, 1))
    stsc = _mov_atom(b"stsc", struct.pack(">I", 0) + struct.pack(">I", 1) + struct.pack(">III", 1, 1, 1))
    stsz = _mov_atom(b"stsz", struct.pack(">I", 0) + struct.pack(">I", 0) + struct.pack(">I", n) +
                      b"".join(struct.pack(">I", len(fr)) for fr in frames))
    stco = _mov_atom(b"stco", struct.pack(">I", 0) + struct.pack(">I", n) +
                      b"".join(struct.pack(">I", off) for off in offsets))
    stbl = _mov_atom(b"stbl", stsd + stts + stsc + stsz + stco)
    minf = _mov_atom(b"minf", vmhd + dinf + stbl)
    mdia = _mov_atom(b"mdia", mdhd + hdlr + minf)
    trak = _mov_atom(b"trak", tkhd + mdia)
    moov = _mov_atom(b"moov", mvhd + trak)
    return ftyp + mdat + moov

# ============================================ hand-authored MS Video 1 8bpp
# Direct encoders for the three MS Video 1 8-bit block types, each the exact
# inverse of legacy_msvideo1.c's decode_8bit -- built from the bit formulas
# in that file, not guessed. Used because ffmpeg has no msvideo1 8bpp
# ENCODER (`ffmpeg -h encoder=msvideo1` lists only rgb555le) but its
# DECODER handles both modes, so encoding a bitstream by hand and checking
# it against ffmpeg's decode is a real differential, just with a
# hand-authored input on this one side.
def _msvc_1color(idx):
    return bytes([idx & 0xFF, 0x80])  # b=0x80 is always valid 1-color, never the 0x84-0x87 skip range

def _msvc_skip(n):
    val = n - 1
    b = 0x84 + (val >> 8)
    a = val & 0xFF
    assert 0x84 <= b <= 0x87 and 0 <= a <= 0xFF
    return bytes([a, b])

def _msvc_2color(pat, c0, c1):
    """pat[py][px] in {0,1} selects c0/c1. Always satisfiable with b<0x80
    by construction: bit15 (py=3,px=3) is 0 exactly when pat[3][3]==1, so
    the caller is expected to have already put whichever value belongs at
    the bottom-right pixel into c1."""
    flags = 0
    for py in range(4):
        for px in range(4):
            bit = pat[py][px] ^ 1
            flags |= bit << (py * 4 + px)
    a, b = flags & 0xFF, (flags >> 8) & 0xFF
    assert b < 0x80, "pat[3][3] must be 1 (selects c1) for a valid 2-color block"
    return bytes([a, b, c0 & 0xFF, c1 & 0xFF])

def _msvc_8color(pat, colors8):
    """pat[py][px] in {qbase,qbase+1} where qbase=((py&2)<<1)+(px&2) in
    {0,2,4,6} -- the quadrant this pixel belongs to. Unlike 2-color, b>=0x90
    is NOT always achievable by a fixed rule from one pixel alone (it needs
    enough of flags' top bits set), so this tries the two choices at each
    of the bottom row's four pixels until the constraint is met -- a small
    and always-terminating search (2**4 = 16 candidates at most, and 0xff
    trivially satisfies it), not a guess."""
    import itertools
    py3 = pat[3][:]
    for combo in itertools.product((0, 1), repeat=4):
        trial = [row[:] for row in pat]
        ok = True
        for px in range(4):
            qbase = ((3 & 2) << 1) + (px & 2)
            v = qbase + combo[px]
            if v not in (qbase, qbase + 1):
                ok = False
            trial[3][px] = v
        if not ok:
            continue
        flags = 0
        for py in range(4):
            for px in range(4):
                qbase = ((py & 2) << 1) + (px & 2)
                rel = trial[py][px] - qbase
                bit = rel ^ 1
                flags |= bit << (py * 4 + px)
        b = (flags >> 8) & 0xFF
        if b >= 0x90:
            a = flags & 0xFF
            return bytes([a, b] + [c & 0xFF for c in colors8])
    raise AssertionError("no bottom-row assignment reached b>=0x90 (should be impossible)")

def gen_msvideo1(outdir, cases_log):
    sizes = [(16, 16), (32, 32), (48, 32)]
    for (w, h) in sizes:
        case = "msvideo1-16bit-%dx%d" % (w, h)
        frames = synth_frames(w, h, 3, seed=stable_seed(case))
        src = os.path.join(outdir, case + ".src.rgb")
        write_rgb24(src, frames)
        container = os.path.join(outdir, case + ".src")
        run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
             "-s", "%dx%d" % (w, h), "-i", src, "-c:v", "msvideo1", "-pix_fmt", "rgb555le",
             "-f", "avi", container])
        ref = os.path.join(outdir, case + ".ref")
        decode_ref(container, "rgb555le", ref)
        pkts = probe_packets(container)
        kv = dict(codec="msvideo1", variant="16bit", width=w, height=h,
                  container=os.path.basename(container), ref=os.path.basename(ref),
                  ref_kind="plain")
        add_packets(kv, pkts)
        write_manifest(outdir, case, **kv)
        cases_log.append(case)
        os.remove(src)

    # --- hand-authored 8-bit case: 16x16 = 4x4 blocks, three frames -------
    w, h = 16, 16
    bw, bh = w // 4, h // 4  # 4,4
    palette = [(i * 3 % 256, (i * 7) % 256, (i * 11) % 256) for i in range(256)]

    def block_bytes_frame0(br, bc):
        if br == 0:
            return _msvc_1color(10 + bc)
        if br == 1:
            pat = [[(x + y) & 1 for x in range(4)] for y in range(4)]
            lo, hi = 20 + bc, 21 + bc
            c1 = hi if pat[3][3] == 1 else lo
            c0 = lo if c1 == hi else hi
            # pat2 must be 1 at [3][3] (see _msvc_2color's own docstring on
            # that constraint) -- which value that is depends on whether
            # pat[3][3] is itself 1 or 0, so pat2 selects "belongs to the
            # SAME group as the bottom-right pixel", not "equals 1"
            # literally. The latter was a real bug (found running this
            # generator for the first time, since __main__ never called
            # gen_msvideo1 before now): pat[3][3] == (3+3)&1 == 0 here, so
            # the old `v == 1` mapping put a 0 at [3][3] and
            # _msvc_2color's own assertion caught it immediately.
            pat2 = [[1 if v == pat[3][3] else 0 for v in row] for row in pat]
            return _msvc_2color(pat2, c0, c1)
        if br == 2:
            pat = [[((py & 2) << 1) + (px & 2) + ((px + py) & 1) for px in range(4)] for py in range(4)]
            colors8 = [30 + bc * 8 + k for k in range(8)]
            return _msvc_8color(pat, colors8)
        return _msvc_1color(110 + bc)

    def block_bytes_frame_delta(br, bc, prev_kind):
        # Frame1: change br 0,1,2; skip br 3 (unchanged from frame0).
        # Frame2: change br 0,1; skip br 2,3 (unchanged from frame1).
        if br == 3:
            return _msvc_skip(1)
        if br == 2 and prev_kind == 2:
            return _msvc_skip(1)
        if br == 0:
            return _msvc_1color(50 + bc)
        if br == 1:
            pat = [[(x != y) for x in range(4)] for y in range(4)]
            lo, hi = 60 + bc, 61 + bc
            c1 = hi if pat[3][3] else lo
            c0 = lo if c1 == hi else hi
            # Same fix as block_bytes_frame0's br==1 case above: map by
            # "same group as [3][3]", not by literal truthiness.
            pat2 = [[1 if bool(v) == bool(pat[3][3]) else 0 for v in row] for row in pat]
            return _msvc_2color(pat2, c0, c1)
        pat = [[((py & 2) << 1) + (px & 2) + ((px) & 1) for px in range(4)] for py in range(4)]
        colors8 = [70 + bc * 8 + k for k in range(8)]
        return _msvc_8color(pat, colors8)

    def assemble(block_fn):
        # Processing order: block rows bottom-to-top (br = bh-1 .. 0), each
        # row left-to-right (bc = 0 .. bw-1) -- see decode_8bit's own loop.
        out = bytearray()
        for br in range(bh - 1, -1, -1):
            for bc in range(bw):
                out += block_fn(br, bc)
        return bytes(out)

    f0 = assemble(lambda br, bc: block_bytes_frame0(br, bc))
    f1 = assemble(lambda br, bc: block_bytes_frame_delta(br, bc, prev_kind=None))
    f2 = assemble(lambda br, bc: block_bytes_frame_delta(br, bc, prev_kind=2))
    frames8 = [f0, f1, f2]

    avi = build_avi_msvc(w, h, 8, [(b, g, r) for (r, g, b) in palette], frames8)
    case = "msvideo1-8bit-16x16"
    container = os.path.join(outdir, case + ".src")
    open(container, "wb").write(avi)
    ref = os.path.join(outdir, case + ".ref")
    decode_ref(container, "pal8", ref)  # index plane + AVPALETTE trailer per frame
    pkts = probe_packets(container)
    kv = dict(codec="msvideo1", variant="8bit", width=w, height=h,
              container=os.path.basename(container), ref=os.path.basename(ref),
              ref_kind="pal8trailer", hand_authored=1)
    add_packets(kv, pkts)
    write_manifest(outdir, case, **kv)
    cases_log.append(case)

    # Negative control: truncate frame0 to 3 bytes -- CHECK(2) for the first
    # block's (byte_a,byte_b) succeeds, but a 2-color block then needs 2
    # more bytes CHECK(2) does not have.
    corrupt = f0[:3]
    open(os.path.join(outdir, "msvideo1-negctl.corrupt"), "wb").write(corrupt)
    write_manifest(outdir, "msvideo1-negctl", codec="msvideo1", variant="8bit",
                    width=w, height=h, corrupt="msvideo1-negctl.corrupt")
    cases_log.append("msvideo1-negctl")

# ============================================================ Cinepak (AVI)
def gen_cinepak(outdir, cases_log):
    sizes = [(16, 16), (32, 32), (48, 32)]
    for (w, h) in sizes:
        for variant, pixfmt in (("rgb24", "rgb24"), ("gray", "gray")):
            case = "cinepak-%s-%dx%d" % (variant, w, h)
            frames = synth_frames(w, h, 3, seed=stable_seed(case))
            src = os.path.join(outdir, case + ".src.rgb")
            write_rgb24(src, frames)
            container = os.path.join(outdir, case + ".src")
            run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                 "-s", "%dx%d" % (w, h), "-i", src, "-c:v", "cinepak", "-pix_fmt", pixfmt,
                 "-f", "avi", container])
            ref = os.path.join(outdir, case + ".ref")
            decode_ref(container, "rgb24" if variant == "rgb24" else "gray", ref)
            pkts = probe_packets(container)
            kv = dict(codec="cinepak", variant=variant, width=w, height=h,
                      container=os.path.basename(container), ref=os.path.basename(ref),
                      ref_kind="plain")
            add_packets(kv, pkts)
            write_manifest(outdir, case, **kv)
            cases_log.append(case)
            os.remove(src)

    # Negative control: corrupt the first codebook chunk's size field of a
    # real intra frame so it claims a chunk far larger than the strip that
    # contains it -- decode_strip's own `chunk_size<0` / eod-clamp logic
    # must refuse this, not paint from garbage.
    case = "cinepak-rgb24-32x32"
    container = os.path.join(outdir, case + ".src")
    pkts = probe_packets(container)
    pos, size = pkts[0]
    data = bytearray(open(container, "rb").read()[pos:pos + size])
    # data[10:] is the first strip header (12 bytes); data[10+12] is the
    # first chunk's id byte, data[10+13:10+16] its 24-bit size (big-endian,
    # includes the 4-byte chunk header itself per the format). Zeroing it
    # makes chunk_size = 0 - 4 = -4, which decode_strip's own
    # `if (chunk_size < 0) return LEGACY_ERR_CORRUPT;` must catch -- a large
    # claimed size is NOT a good trigger here, because the eod-clamp two
    # lines later is a deliberate, spec-correct tolerance (ffmpeg's own
    # cinepak.c does the same clamp) and would just silently truncate the
    # chunk instead of erroring.
    off = 10 + 12 + 1
    data[off] = 0x00
    data[off + 1] = 0x00
    data[off + 2] = 0x00
    open(os.path.join(outdir, "cinepak-negctl.corrupt"), "wb").write(bytes(data))
    write_manifest(outdir, "cinepak-negctl", codec="cinepak", variant="rgb24",
                    width=32, height=32, corrupt="cinepak-negctl.corrupt")
    cases_log.append("cinepak-negctl")

# ================================================================== RPZA (MOV)
def gen_rpza(outdir, cases_log):
    sizes = [(16, 16), (32, 32), (48, 32)]
    for (w, h) in sizes:
        case = "rpza-%dx%d" % (w, h)
        frames = synth_frames(w, h, 3, seed=stable_seed(case))
        src = os.path.join(outdir, case + ".src.rgb")
        write_rgb24(src, frames)
        container = os.path.join(outdir, case + ".src")
        run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
             "-s", "%dx%d" % (w, h), "-i", src, "-c:v", "rpza", "-pix_fmt", "rgb555le",
             "-f", "mov", container])
        ref = os.path.join(outdir, case + ".ref")
        decode_ref(container, "rgb555le", ref)
        pkts = probe_packets(container)
        kv = dict(codec="rpza", variant="rgb555", width=w, height=h,
                  container=os.path.basename(container), ref=os.path.basename(ref),
                  ref_kind="plain")
        add_packets(kv, pkts)
        write_manifest(outdir, case, **kv)
        cases_log.append(case)
        os.remove(src)

    # ffmpeg's rpza ENCODER never chose the 4-color opcode (0x20/0xc0) for
    # any of the three sizes above -- measured, not assumed: a first attempt
    # at this control read 0/3 cases reddened for RPZA_CONTROL_SWAPPED_BLEND
    # even after steepening the synth gradient, because rpzaenc.c's
    # ONE-COLOR CHECK (a per-block avg/min/max threshold) or its own SIXTEEN
    # COLOR fallback (when the least-squares fit error is too high) absorbed
    # every block before the four-color path was reached. So this one case
    # is hand-authored -- a single opcode 0xC0 block (explicit colorA, then
    # colorB, then four rows of 2-bit indices) -- the same "no real encoder
    # reaches this, a real decoder still verifies it" shape gen_qtrle uses
    # for QTRLE's un-encodable depths and gen_msvideo1 uses for 8-bit MSVC.
    case = "rpza-4color-4x4"
    def _rpza_chunk(opcode_body):
        total = 4 + len(opcode_body)
        return bytes([0xe1]) + struct.pack(">I", total)[1:] + opcode_body
    colorA, colorB = 0x0421, 0x3DEF  # arbitrary 15-bit RGB555, bit15=0
    rows_f0 = [0b00011011, 0b11100100, 0b01001110, 0b10110001]
    rows_f1 = [0b11001001, 0b00110110, 0b10011100, 0b01100011]
    frame0 = _rpza_chunk(bytes([0xC0]) + struct.pack(">H", colorA) +
                         struct.pack(">H", colorB) + bytes(rows_f0))
    frame1 = _rpza_chunk(bytes([0xC0]) + struct.pack(">H", colorA) +
                         struct.pack(">H", colorB) + bytes(rows_f1))
    mov = build_mov_rpza(4, 4, [frame0, frame1])
    container = os.path.join(outdir, case + ".src")
    open(container, "wb").write(mov)
    ref = os.path.join(outdir, case + ".ref")
    decode_ref(container, "rgb555le", ref)
    pkts = probe_packets(container)
    kv = dict(codec="rpza", variant="rgb555", width=4, height=4,
              container=os.path.basename(container), ref=os.path.basename(ref),
              ref_kind="plain", hand_authored=1)
    add_packets(kv, pkts)
    write_manifest(outdir, case, **kv)
    cases_log.append(case)

    case = "rpza-32x32"
    container = os.path.join(outdir, case + ".src")
    pkts = probe_packets(container)
    pos, size = pkts[0]
    data = bytearray(open(container, "rb").read()[pos:pos + size])
    # Byte 4 is the first opcode (after the 4-byte 0xe1-prefixed chunk-size
    # word). Any byte with its top bit clear (0x00-0x7f) never reaches the
    # opcode switch as itself -- rpza_decode_stream folds it into colorA and
    # re-dispatches on the FOLLOWING byte instead (see the "MSbit is 0"
    # branch), so 0x00-0x7f can only ever produce case 0x00 or case 0x20.
    # The one genuinely unhandled range is 0xe0-0xff: opcode&0xe0 there is
    # 0xe0, which matches none of 0x00/0x20/0x80/0xa0/0xc0 -- the decoder's
    # `default:` must fire.
    data[4] = 0xf0
    open(os.path.join(outdir, "rpza-negctl.corrupt"), "wb").write(bytes(data))
    write_manifest(outdir, "rpza-negctl", codec="rpza", variant="rgb555",
                    width=32, height=32, corrupt="rpza-negctl.corrupt")
    cases_log.append("rpza-negctl")

# ================================================================== QTRLE (MOV)
# legacy_qtrle.c's own header comment names the shape this function has to
# have: ffmpeg's qtrle ENCODER (`ffmpeg -h encoder=qtrle`) reaches rgb24,
# rgb555be, argb and gray -- 24/16/32bpp and grayscale-8bpp (bits_per_coded_
# sample 40) -- and reaches NEITHER an arbitrary (non-grayscale) 8bpp palette
# NOR 1/2/4bpp at all. So depths 16/24/32/40 go through a real ffmpeg-encode
# + ffmpeg-decode differential exactly like the other three codecs in this
# file; depths 1/2/4/8 are hand-authored bitstreams built as the exact
# inverse of legacy_qtrle.c's own decode routines (verified against
# independently-simulated unpacking of every group-packing formula before
# this function was written -- see the sanity check run separately), wrapped
# in build_mov_qtrle() with a real in-place QuickTime color table, and
# checked against ffmpeg's own (real, independent) qtrle DECODER -- the same
# "hand-authored input, real oracle" shape genlegacy.sh's own header comment
# already promised and gen_msvideo1's 8-bit case already uses.
#
# GROUP SIZE. legacy_qtrle.c's four palette routines each count their `rle`
# field in a fixed number of PIXELS per unit, not one -- 16 for 1bpp and
# 2bpp, 8 for 4bpp, 4 for 8bpp -- and it is the SAME size for both the
# "repeat" (rle<0, one group's bytes read once and replayed) and "copy"
# (rle>0, raw un-repeated bytes) opcodes, because both walk the identical
# per-group byte packing. QTRLE_GROUP records that size once so the encoder
# below never has to re-derive it per opcode.
QTRLE_GROUP = {1: 16, 2: 16, 4: 8, 8: 4}


def _qtrle_pack_group(vals, bpp):
    """The exact inverse of legacy_qtrle.c's per-group unpacking for every
    palette depth -- verified bit-for-bit against a standalone transcription
    of dec_1bpp's / dec_2n4bpp's / dec_8bpp's own unpack loops before this
    function was written into the generator (round-tripped 1/2/4/8bpp
    randomised groups). bpp=1: 16 values -> 2 bytes, MSB-first, 8 values per
    byte. bpp=2: 16 values -> 4 bytes, 4 pixels/byte, 2 bits each, MSB-first.
    bpp=4: 8 values -> 4 bytes, 2 pixels/byte, MSB-first nibble. bpp=8: 4
    values -> 4 raw bytes (no packing -- 8bpp is already byte-per-pixel)."""
    if bpp == 1:
        assert len(vals) == 16
        b0 = 0
        for k in range(8):
            b0 = (b0 << 1) | (vals[k] & 1)
        b1 = 0
        for k in range(8):
            b1 = (b1 << 1) | (vals[8 + k] & 1)
        return bytes([b0, b1])
    if bpp == 2:
        assert len(vals) == 16
        out = bytearray()
        for i in range(0, 16, 4):
            out.append(((vals[i] & 3) << 6) | ((vals[i + 1] & 3) << 4) |
                       ((vals[i + 2] & 3) << 2) | (vals[i + 3] & 3))
        return bytes(out)
    if bpp == 4:
        assert len(vals) == 8
        out = bytearray()
        for i in range(0, 8, 2):
            out.append(((vals[i] & 0xf) << 4) | (vals[i + 1] & 0xf))
        return bytes(out)
    if bpp == 8:
        assert len(vals) == 4
        return bytes(v & 0xff for v in vals)
    raise ValueError("unsupported qtrle bpp %r" % bpp)


def _s8(v):
    """A signed rle byte in [-128,127] as the unsigned byte value the
    bitstream carries."""
    assert -128 <= v <= 127
    return v & 0xff


def _qtrle_row_ops(pixels, bpp):
    """Cover one whole row of `pixels` (length a multiple of
    QTRLE_GROUP[bpp]) with a greedy sequence of repeat/copy ops: a run of
    >=2 IDENTICAL consecutive groups becomes one "repeat" op (rle<0, the
    group's bytes written once); everything else becomes a "copy" op
    (rle>0, every group's bytes written out in full). Returns a list of
    (rle_byte, payload_bytes) tuples with no leading start-byte and no
    terminator -- the two per-format outer wrappers below add those,
    because 1bpp's per-line framing differs from 2/4/8bpp's."""
    g = QTRLE_GROUP[bpp]
    assert len(pixels) % g == 0
    groups = [tuple(pixels[i:i + g]) for i in range(0, len(pixels), g)]
    ops = []
    i = 0
    while i < len(groups):
        j = i
        while j < len(groups) and groups[j] == groups[i]:
            j += 1
        run = j - i
        if run >= 2:
            assert run <= 127
            ops.append((_s8(-run), _qtrle_pack_group(list(groups[i]), bpp)))
            i = j
        else:
            k = i
            while k < len(groups) and not (k + 1 < len(groups) and groups[k] == groups[k + 1]):
                k += 1
            lit = groups[i:k]
            assert 1 <= len(lit) <= 127
            payload = b"".join(_qtrle_pack_group(list(gr), bpp) for gr in lit)
            ops.append((len(lit), payload))
            i = k
    return ops


def _qtrle_frame_1bpp(rows):
    """dec_1bpp's framing: one GLOBAL lines counter, no per-row terminator.
    Only the FIRST op of a row sets skip's 0x80 bit (advance to the next
    line, decrementing the shared counter); later ops in the same row use
    skip=0 (`pixel_ptr += 16*0`, i.e. carry straight on from where the
    previous op left off). Ends with the global rle=0 terminator."""
    out = bytearray()
    for row in rows:
        first = True
        for (rle, payload) in _qtrle_row_ops(row, 1):
            out.append(0x80 if first else 0x00)
            first = False
            out.append(rle)
            out += payload
    out += bytes([0x00, 0x00])
    return bytes(out)


def _qtrle_frame_rowmajor(rows, bpp):
    """dec_2n4bpp's / dec_8bpp's framing: a start byte per row (pixel_ptr
    offset within the row, in groups -- 1 always means offset 0), then a
    sequence of ops, then a per-row rle=-1 terminator."""
    out = bytearray()
    for row in rows:
        out.append(1)
        for (rle, payload) in _qtrle_row_ops(row, bpp):
            out.append(_s8(rle) if rle < 0 else rle)
            out += payload
        out.append(_s8(-1))
    return bytes(out)


def _qtrle_wrap_chunk(header_extra, body):
    """Prepend the 4-byte BE chunk-size word (the sample's OWN total length,
    matching what a real encoder writes and what ffmpeg's decode-side
    `discard_damaged_percentage` check compares against) and the 2-byte
    header word."""
    payload = header_extra + body
    total = 4 + 2 + len(payload)
    return struct.pack(">I", total) + struct.pack(">H", len(header_extra) and 0x0008 or 0x0000) + payload


def _synth_indices(w, h, bpp, f):
    """Deterministic palette-index frames with the same shape every codec's
    synth pattern here has: the right half of every row holds ONE value that
    only changes slowly across rows/frames (long, clean repeat runs -- the
    "repeat" opcode's real reason to exist), the left half varies every
    pixel (the "copy" opcode's)."""
    maxval = (1 << bpp) - 1
    rows = []
    for y in range(h):
        rightval = (y + f) % (maxval + 1)
        row = []
        for x in range(w):
            if x < w // 2:
                row.append((x + 3 * y + 5 * f) % (maxval + 1))
            else:
                row.append(rightval)
        rows.append(row)
    return rows


def _synth_gray(w, h, n, seed):
    frames = synth_frames(w, h, n, seed)
    out = []
    for fr in frames:
        g = bytearray(w * h)
        for i in range(w * h):
            g[i] = (fr[i * 3] + fr[i * 3 + 1] + fr[i * 3 + 2]) // 3
        out.append(bytes(g))
    return out


def gen_qtrle(outdir, cases_log):
    sizes = [(16, 16), (32, 32), (48, 32)]

    # --- ffmpeg-encoder-reachable depths: real encode + real decode -------
    for (w, h) in sizes:
        case = "qtrle-16bit-%dx%d" % (w, h)
        frames = synth_frames(w, h, 3, seed=stable_seed(case))
        src = os.path.join(outdir, case + ".src.rgb")
        write_rgb24(src, frames)
        container = os.path.join(outdir, case + ".src")
        run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
             "-s", "%dx%d" % (w, h), "-i", src, "-c:v", "qtrle", "-pix_fmt", "rgb555be",
             "-f", "mov", container])
        ref = os.path.join(outdir, case + ".ref")
        decode_ref(container, "rgb555le", ref)
        pkts = probe_packets(container)
        kv = dict(codec="qtrle", variant="16bit", width=w, height=h, depth=16,
                  container=os.path.basename(container), ref=os.path.basename(ref),
                  ref_kind="plain")
        add_packets(kv, pkts)
        write_manifest(outdir, case, **kv)
        cases_log.append(case)
        os.remove(src)

        case = "qtrle-24bit-%dx%d" % (w, h)
        frames = synth_frames(w, h, 3, seed=stable_seed(case))
        src = os.path.join(outdir, case + ".src.rgb")
        write_rgb24(src, frames)
        container = os.path.join(outdir, case + ".src")
        run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
             "-s", "%dx%d" % (w, h), "-i", src, "-c:v", "qtrle", "-pix_fmt", "rgb24",
             "-f", "mov", container])
        ref = os.path.join(outdir, case + ".ref")
        decode_ref(container, "rgb24", ref)
        pkts = probe_packets(container)
        kv = dict(codec="qtrle", variant="24bit", width=w, height=h, depth=24,
                  container=os.path.basename(container), ref=os.path.basename(ref),
                  ref_kind="plain")
        add_packets(kv, pkts)
        write_manifest(outdir, case, **kv)
        cases_log.append(case)
        os.remove(src)

        case = "qtrle-32bit-%dx%d" % (w, h)
        frames = synth_frames(w, h, 3, seed=stable_seed(case))
        src = os.path.join(outdir, case + ".src.rgb")
        write_rgb24(src, frames)
        container = os.path.join(outdir, case + ".src")
        run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
             "-s", "%dx%d" % (w, h), "-i", src, "-c:v", "qtrle", "-pix_fmt", "argb",
             "-f", "mov", container])
        ref = os.path.join(outdir, case + ".ref")
        decode_ref(container, "argb", ref)
        pkts = probe_packets(container)
        kv = dict(codec="qtrle", variant="32bit", width=w, height=h, depth=32,
                  container=os.path.basename(container), ref=os.path.basename(ref),
                  ref_kind="plain")
        add_packets(kv, pkts)
        write_manifest(outdir, case, **kv)
        cases_log.append(case)
        os.remove(src)

        # bits_per_coded_sample 40: GRAY8's own "greyscale" depth, decoded
        # through the SAME dec_8bpp routine as an arbitrary 8bpp palette
        # (qtrle_route masks 40 & 0x1f == 8) -- so, like MS Video 1's 8-bit
        # mode and the hand-authored depths below, the byte-exact gate is
        # the raw INDEX plane (ffmpeg's own encoder XORs each sample byte
        # with 0xff for GRAY8, an inverted ramp neither decoder interprets;
        # both just pass the byte through), never a resolved gray8 value.
        case = "qtrle-gray8-%dx%d" % (w, h)
        gframes = _synth_gray(w, h, 3, seed=stable_seed(case))
        src = os.path.join(outdir, case + ".src.gray")
        with open(src, "wb") as f:
            for g in gframes:
                f.write(g)
        container = os.path.join(outdir, case + ".src")
        run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "gray",
             "-s", "%dx%d" % (w, h), "-i", src, "-c:v", "qtrle", "-pix_fmt", "gray",
             "-f", "mov", container])
        ref = os.path.join(outdir, case + ".ref")
        decode_ref(container, "pal8", ref)
        pkts = probe_packets(container)
        kv = dict(codec="qtrle", variant="gray8", width=w, height=h, depth=40,
                  container=os.path.basename(container), ref=os.path.basename(ref),
                  ref_kind="pal8trailer")
        add_packets(kv, pkts)
        write_manifest(outdir, case, **kv)
        cases_log.append(case)
        os.remove(src)

    # --- hand-authored depths: no ffmpeg qtrle ENCODER reaches these -------
    # Every case is TWO frames: frame 0 a full-picture update (plain 2-byte
    # header, every row), frame 1 a PARTIAL update of the middle third of
    # the rows only, through the extended header (start_line/lines) --
    # exercising inter-frame persistence (the untouched rows must still read
    # back as frame 0's content, proving the decoder left them alone) the
    # same way every other codec in this file is stateful by construction.
    for bpp in (1, 2, 4, 8):
        for (w, h) in sizes:
            if w % QTRLE_GROUP[bpp] != 0:
                continue  # never happens for {16,32,48} against {16,16,8,4}, kept as a guard
            case = "qtrle-%dbit-%dx%d" % (bpp, w, h)
            rows0 = _synth_indices(w, h, bpp, 0)
            rows1_full = _synth_indices(w, h, bpp, 1)
            band0, band1 = h // 3, (2 * h) // 3
            body0 = (_qtrle_frame_1bpp(rows0) if bpp == 1
                     else _qtrle_frame_rowmajor(rows0, bpp))
            frame0 = _qtrle_wrap_chunk(b"", body0)

            mid_rows = rows1_full[band0:band1]
            body1 = (_qtrle_frame_1bpp(mid_rows) if bpp == 1
                     else _qtrle_frame_rowmajor(mid_rows, bpp))
            ext_header = struct.pack(">H", band0) + b"\x00\x00" + \
                         struct.pack(">H", band1 - band0) + b"\x00\x00"
            frame1 = _qtrle_wrap_chunk(ext_header, body1)

            maxval = (1 << bpp) - 1
            ctab = [((i * 37) % 256, (i * 53) % 256, (i * 97) % 256) for i in range(maxval + 1)]
            mov = build_mov_qtrle(w, h, bpp, ctab, [frame0, frame1])
            container = os.path.join(outdir, case + ".src")
            open(container, "wb").write(mov)
            ref = os.path.join(outdir, case + ".ref")
            decode_ref(container, "pal8", ref)
            pkts = probe_packets(container)
            kv = dict(codec="qtrle", variant="%dbit" % bpp, width=w, height=h, depth=bpp,
                      container=os.path.basename(container), ref=os.path.basename(ref),
                      ref_kind="pal8trailer", hand_authored=1)
            add_packets(kv, pkts)
            write_manifest(outdir, case, **kv)
            cases_log.append(case)

    # --- negative control ---------------------------------------------------
    # A truncated hand-authored 8bpp frame: a repeat op's rle byte (say -3)
    # promises a 4-byte group payload (NEED(4) in dec_8bpp) but the chunk
    # ends 1 byte short of it -- the same "declared length exceeds what is
    # actually there" shape as msvideo1-negctl and cinepak-negctl, applied to
    # QTRLE's own opcode grammar rather than a mutilation of working bytes.
    w, h = 16, 16
    rows = _synth_indices(w, h, 8, 0)
    body = _qtrle_frame_rowmajor(rows, 8)
    frame = bytearray(_qtrle_wrap_chunk(b"", body))
    # frame layout: [4B size][2B header=0][1B start-byte=1][1B rle][payload...]
    # Byte 7 is the first rle byte; force it to a repeat op needing 4 more
    # bytes, then cut the chunk 1 byte short of the end of the payload it
    # promises.
    frame[7] = _s8(-2)  # "repeat this group twice", needs 4 payload bytes
    truncated = bytes(frame[:7 + 1 + 3])  # rle byte + 3 of the 4 needed bytes
    open(os.path.join(outdir, "qtrle-negctl.corrupt"), "wb").write(truncated)
    write_manifest(outdir, "qtrle-negctl", codec="qtrle", variant="8bit",
                    width=w, height=h, depth=8, corrupt="qtrle-negctl.corrupt")
    cases_log.append("qtrle-negctl")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        die("usage: genlegacy.py <outdir>")
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    cases = []
    gen_cinepak(outdir, cases)
    # gen_msvideo1 and gen_rpza were fully implemented (real ffmpeg
    # differentials + a hand-authored 8-bit case each, complete with their
    # own negative controls) but __main__ never called them -- confirmed by
    # grep before this fix: only gen_cinepak(outdir, cases) appeared here.
    # So every run of this script before today produced ONLY cinepak cases;
    # msvideo1, rpza and qtrle produced nothing at all, silently.
    gen_msvideo1(outdir, cases)
    gen_rpza(outdir, cases)
    gen_qtrle(outdir, cases)
    with open(os.path.join(outdir, "CASES"), "w") as f:
        f.write("\n".join(cases) + "\n")
    print("generated %d cases (cinepak + msvideo1 + rpza + qtrle) in %s" % (len(cases), outdir))
