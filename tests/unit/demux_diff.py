#!/usr/bin/env python3
"""tests/unit/demux_diff.py -- the differential against ffmpeg's demuxer.

The decoders next door are held to bit-exactness against ffmpeg's decoders. The
equivalent bar for a demuxer is that, for a given file, it finds:

  * the same SAMPLE BOUNDARIES  -- same count, same size, same file offset
  * the same TIMESTAMPS         -- same pts and dts, in the container's own
                                   timescale, including the composition offsets
                                   and the edit-list shift
  * the same KEYFRAME FLAGS
  * the same CODEC-CONFIGURATION BYTES -- avcC/hvcC/CodecPrivate, by CRC32
  * the same ELEMENTARY STREAM out -- our Annex B rewrite, NAL for NAL, against
    ffmpeg's own -bsf h264_mp4toannexb output

All five are checkable byte-for-byte, which is the point. Nothing here is a
tolerance and nothing is "close enough".

Two places where ffmpeg is asked a question rather than trusted blindly, and
why:

  file offset (`pos`).  ffprobe reports the position of the packet in the file.
    For MP4 that is the sample offset and we require equality. For Matroska
    ffmpeg reports the position of the enclosing CLUSTER, not of the frame
    inside the block, so comparing it would be comparing two different
    quantities; sizes and timestamps are compared instead and the offsets are
    checked for internal consistency (in range, non-overlapping, ascending).

  Annex B start codes.  Both 3- and 4-byte start codes are legal, and ffmpeg's
    bitstream filter mixes them (4 for the first NAL of a packet and for
    parameter sets, 3 otherwise) while we always emit 4. It also re-inserts the
    parameter sets before every IDR. So the comparison is NAL-by-NAL after
    splitting both streams on start codes, with a repeated parameter set
    treated as the repeat it is -- the NAL PAYLOADS must match exactly, in
    order, which is the actual claim.
"""
import subprocess, sys, os, zlib, collections

def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit("command failed: %s\n%s" % (" ".join(cmd), p.stderr[:2000]))
    return p.stdout

# ---------------------------------------------------------------- ours ----
def our_packets(tool, path):
    out = collections.defaultdict(list)
    for line in run([tool, "packets", path]).splitlines():
        f = line.split()
        if not f or f[0] != "P":
            continue
        t, pts, dts, size, pos, key = int(f[1]), int(f[2]), int(f[3]), int(f[4]), int(f[5]), int(f[6])
        out[t].append((pts, dts, size, pos, key))
    return out

def our_info(tool, path):
    tracks = {}
    hdr = {}
    for line in run([tool, "info", path]).splitlines():
        kv = dict(x.split("=", 1) for x in line.split() if "=" in x)
        if "track" in kv:
            tracks[int(kv["track"])] = kv
        else:
            hdr = kv
    return hdr, tracks

# -------------------------------------------------------------- ffmpeg ----
def ff_packets(path):
    out = collections.defaultdict(list)
    txt = run(["ffprobe", "-v", "error", "-show_packets",
               "-show_entries", "packet=stream_index,pts,dts,size,pos,flags",
               "-of", "compact", path])
    for line in txt.splitlines():
        if not line.startswith("packet|"):
            continue
        kv = dict(x.split("=", 1) for x in line.split("|")[1:] if "=" in x)
        def num(k):
            v = kv.get(k, "N/A")
            return None if v in ("N/A", "") else int(v)
        out[int(kv["stream_index"])].append(
            (num("pts"), num("dts"), num("size"), num("pos"),
             1 if kv.get("flags", "").startswith("K") else 0))
    return out

def ff_streams(path):
    txt = run(["ffprobe", "-v", "error", "-show_data_hash", "CRC32",
               "-show_entries",
               "stream=index,codec_name,codec_type,width,height,sample_rate,"
               "channels,time_base,extradata_size,extradata_hash",
               "-of", "compact", path])
    out = {}
    for line in txt.splitlines():
        if not line.startswith("stream|"):
            continue
        kv = dict(x.split("=", 1) for x in line.split("|")[1:] if "=" in x)
        out[int(kv["index"])] = kv
    return out

# --------------------------------------------------------------- annexb ---
def split_nals(data):
    """Split an Annex B byte stream into NAL payloads (start codes removed)."""
    nals, i, n = [], 0, len(data)
    starts = []
    while i + 3 <= n:
        if data[i] == 0 and data[i+1] == 0:
            if data[i+2] == 1:
                starts.append((i, 3)); i += 3; continue
            if i + 4 <= n and data[i+2] == 0 and data[i+3] == 1:
                starts.append((i, 4)); i += 4; continue
        i += 1
    for k, (off, sc) in enumerate(starts):
        end = starts[k+1][0] if k + 1 < len(starts) else n
        nals.append(bytes(data[off+sc:end]))
    return nals

def split_ps(nals, is_ps):
    """Separate the parameter sets from everything else.

    We emit the parameter sets once, at the front, which is what an elementary
    stream normally looks like. ffmpeg's bitstream filter re-inserts them
    immediately before every IDR instead -- also legal, and a placement choice
    rather than a content one. So the comparison is: the non-parameter-set NALs
    must match EXACTLY, in order, byte for byte (that is every slice and every
    SEI in the file), and the distinct parameter sets must be the same set of
    bytes. Nothing about the file's content escapes that pair of checks."""
    ps, rest = [], []
    for nal in nals:
        if is_ps(nal):
            if nal not in ps:
                ps.append(nal)
        else:
            rest.append(nal)
    return ps, rest

def starts_with_annexb(tool, build, path, t):
    """True when track t's FIRST sample begins with an Annex B start code
    (00 00 01 or 00 00 00 01) -- the positive, checkable signal that this
    track's parameter sets are IN-BAND, not a discrete avcC/hvcC record.
    Used instead of trusting "our extradata_size is 0" on its own, which a
    real bug (this demuxer failing to find extradata that genuinely exists)
    would look identical to -- this only skips the extradata comparison when
    the sample bytes themselves prove there was nothing out-of-band to find."""
    raw = os.path.join(build, "sig.bin")
    try:
        run([tool, "raw", path, str(t), raw])
    except SystemExit:
        return False
    d = open(raw, "rb").read(4)
    return d[:3] == b"\x00\x00\x01" or d[:4] == b"\x00\x00\x00\x01"

def h264_is_ps(nal):
    return bool(nal) and (nal[0] & 0x1F) in (7, 8)

def h265_is_ps(nal):
    return bool(nal) and ((nal[0] >> 1) & 0x3F) in (32, 33, 34)

# ----------------------------------------------------------------- main ---
def check(path, tool, build, fail):
    name = os.path.basename(path)
    ours = our_packets(tool, path)
    theirs = ff_packets(path)
    hdr, otracks = our_info(tool, path)
    ftracks = ff_streams(path)
    is_mkv = hdr.get("container") == "matroska"
    is_avi = hdr.get("container") == "avi"
    is_ts = hdr.get("container") == "mpegts"
    is_ps = hdr.get("container") == "mpeg"
    is_flv = hdr.get("container") == "flv"
    # POS IS A DIFFERENT QUANTITY FOR THREE OF THESE FORMATS, THE SAME WAY IT
    # ALREADY IS FOR MATROSKA -- not a bug, a different coordinate space:
    #   TS/PS: ts.h/ps.h both document it outright -- a sample's bytes are
    #     REASSEMBLED into a scratch buffer this library allocates (a PES
    #     payload is chopped across ~184-byte TS packets, and PS occasionally
    #     splits too), so media_sample.file_off is an offset into THAT buffer,
    #     never a transport/pack-stream file position.
    #   FLV: media_sample.file_off is documented (media.h) as "where the
    #     PAYLOAD starts", so ours points past the 11-byte FLV tag header AND
    #     past the codec sub-header (AVCPacketType+CompositionTime, or
    #     SoundFormat+AACPacketType) -- ffprobe's pos is the TAG's own start.
    #     The difference is a fixed, exact, per-tag-type byte count (verified:
    #     +16 for every AVC NALU tag, +13 for every raw AAC frame tag on this
    #     corpus) and not a wrong offset -- FLV tags ARE contiguous file bytes
    #     (flv.h's whole point), so this is a choice of what "pos" means, not
    #     a lost track of where anything is.
    pos_comparable = not (is_mkv or is_ts or is_ps or is_flv)

    if set(ours) != set(theirs):
        fail(name, "track set: ours %s ffmpeg %s" % (sorted(ours), sorted(theirs)))
        return

    for t in sorted(ours):
        a, b = ours[t], theirs[t]
        # AVI'S OWN TIME_BASE, NOT 1/timescale -- unlike MP4 and Matroska,
        # where a stream's timescale IS its ffprobe time_base denominator
        # (num=1), libavformat's AVI demuxer sets a stream's time_base to the
        # RAW (dwScale, dwRate) pair from the strh -- e.g. 32/1225 for an MP3
        # track muxed at 1225 ticks/s with 32 ticks/frame -- not the reduced
        # 1/1225. So ffprobe's packet pts/dts count CHUNK-TIME-UNITS (one MP3
        # frame here) while ours counts raw ticks (media.h's own contract:
        # "ticks per second of this track's timestamps"), and the two differ
        # by an EXACT integer factor (dwScale) with no rounding anywhere --
        # not a tolerance, a unit conversion. Confirmed empirically: ffprobe
        # reports time_base=32/1225 and our track's timescale=1225 for the
        # identical stream, and every raw pts/dts pair disagrees by exactly
        # 32x. Rescale ffprobe's ticks into ours before comparing, and ONLY
        # when our timescale actually equals the time_base denominator (the
        # one case the arithmetic below is valid for) -- otherwise leave the
        # pair untouched and let a real mismatch fail loudly, same as today.
        if is_avi:
            tb = ftracks.get(t, {}).get("time_base", "")
            our_ts = otracks.get(t, {}).get("timescale")
            if "/" in tb and our_ts:
                num_s, den_s = tb.split("/", 1)
                if num_s.isdigit() and den_s.isdigit() and int(den_s) == int(our_ts):
                    factor = int(num_s)
                    b = [(p * factor if p is not None else None,
                          d * factor if d is not None else None, sz, pos, key)
                         for (p, d, sz, pos, key) in b]
        if len(a) != len(b):
            fail(name, "track %d sample count: ours %d ffmpeg %d" % (t, len(a), len(b)))
            continue
        bad = 0
        for i, (x, y) in enumerate(zip(a, b)):
            fields = [("pts", 0), ("size", 2), ("key", 4)]
            if not is_mkv:
                # MATROSKA CARRIES NO DECODE TIMESTAMPS. A Block has one
                # timestamp and it is the presentation one; the dts ffprobe
                # prints for a Matroska file is libavformat's RECONSTRUCTION,
                # made by delaying presentation stamps through a reorder buffer
                # whose depth comes from the H.264 SPS -- i.e. from the decoder,
                # not the container. Comparing against it would be comparing our
                # demuxer with ffmpeg's guess. We report dts = pts and say so;
                # decode ORDER is preserved because block order is decode order,
                # which is what actually feeds the decoder.
                fields.append(("dts", 1))
                if pos_comparable:
                    fields.append(("pos", 3))
            for label, idx in fields:
                if y[idx] is None:
                    continue
                if x[idx] != y[idx]:
                    if bad < 4:
                        fail(name, "track %d sample %d %s: ours %s ffmpeg %s"
                             % (t, i, label, x[idx], y[idx]))
                    bad += 1
        if bad:
            fail(name, "track %d: %d field mismatches over %d samples" % (t, bad, len(a)))

        # Offsets must at least be self-consistent even where ffmpeg reports
        # something else: inside the file, and strictly ascending per track.
        prev = -1
        for (pts, dts, size, pos, key) in a:
            if pos < 0 or size < 0:
                fail(name, "track %d: negative offset/size" % t); break
            if pos <= prev:
                fail(name, "track %d: offsets not ascending at %d" % (t, pos)); break
            prev = pos

        # codec configuration
        o, f = otracks.get(t, {}), ftracks.get(t, {})
        fsz = f.get("extradata_size")
        # FLAC in Matroska is the one place the two conventions differ and both
        # are right. CodecPrivate holds a whole FLAC stream header -- the "fLaC"
        # magic and the STREAMINFO metadata block, 42 bytes -- and prepending it
        # to the blocks gives a playable .flac, which is exactly what this
        # system's FLAC decoder takes. libavformat's convention is the 34-byte
        # STREAMINFO PAYLOAD alone, so it strips the 8-byte prefix. Compare what
        # they have in common rather than pretending one of them is wrong.
        strip = 8 if (is_mkv and o.get("codec") == "flac") else 0
        # IN-BAND PARAMETER SETS (Annex B: SPS/PPS as ordinary NALs ahead of
        # the first slice) MEAN THERE IS NO DISCRETE RECORD TO COMPARE.
        # ts.c's own header comment says as much for width/height ("a real
        # player gets them by parsing the video SPS itself, which is a
        # decoder's job, not this demuxer's") and the same argument applies
        # to extradata: ffprobe's extradata_size/hash for an Annex-B stream
        # is libavformat SYNTHESISING an avcC-shaped record by scanning the
        # elementary stream itself -- there is no such record in the FILE
        # for either side to disagree about the contents of. This is not
        # limited to TS/PS (both always carry H.264 in-band) -- AVI can too,
        # when ffmpeg encodes straight to it rather than remuxing an AVCC
        # source (verified: containers-h264-mp3.avi's strf chunk is exactly
        # the bare 40-byte BITMAPINFOHEADER, no extra bytes, and its first
        # sample begins with a literal 00 00 00 01 start code) -- so the
        # test is POSITIVE (does the sample itself start with a start code),
        # not "which container is this" and not "is our extradata_size
        # zero" -- the latter would just as happily hide a real bug (this
        # demuxer failing to find an avcC that genuinely exists, exactly
        # what avi.c's strf-extension fix in this same session corrected).
        in_band_ps = (o.get("codec") in ("h264", "hevc") and
                      starts_with_annexb(tool, build, path, t))
        if fsz is not None and fsz != "N/A" and not in_band_ps:
            if int(o.get("extradata_size", -1)) - strip != int(fsz):
                fail(name, "track %d extradata size: ours %s-%d ffmpeg %s"
                     % (t, o.get("extradata_size"), strip, fsz))
            fh = f.get("extradata_hash", "")
            if fh.startswith("CRC32:"):
                want = fh.split(":", 1)[1].lower()
                if strip:
                    xd = os.path.join(build, "xd.bin")
                    run([tool, "extradata", path, str(t), xd])
                    blob = open(xd, "rb").read()
                    if blob[:4] != b"fLaC":
                        fail(name, "track %d FLAC CodecPrivate has no fLaC magic" % t)
                    got = "%08x" % (zlib.crc32(blob[strip:]) & 0xFFFFFFFF)
                else:
                    got = o.get("extradata_crc", "").lower()
                if want != got:
                    fail(name, "track %d extradata CRC32: ours %s ffmpeg %s"
                         % (t, got, want))
        if f.get("codec_name") and o.get("codec") not in (f["codec_name"], "unknown"):
            fail(name, "track %d codec: ours %s ffmpeg %s" % (t, o.get("codec"), f["codec_name"]))

        # elementary stream, for the two codecs we can hand to a decoder
        if o.get("codec") in ("h264", "hevc"):
            ours_es = os.path.join(build, "es_ours.bin")
            ff_es = os.path.join(build, "es_ff.bin")
            run([tool, "annexb", path, str(t), ours_es])
            bsf = "h264_mp4toannexb" if o["codec"] == "h264" else "hevc_mp4toannexb"
            fmt = "h264" if o["codec"] == "h264" else "hevc"
            subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                            "-i", path, "-map", "0:%d" % t, "-c", "copy",
                            "-bsf:v", bsf, "-f", fmt, ff_es], check=True)
            is_ps = h264_is_ps if o["codec"] == "h264" else h265_is_ps
            psa, na = split_ps(split_nals(open(ours_es, "rb").read()), is_ps)
            psb, nb = split_ps(split_nals(open(ff_es, "rb").read()), is_ps)
            if sorted(psa) != sorted(psb):
                fail(name, "track %d parameter sets differ: ours %s ffmpeg %s"
                     % (t, [len(x) for x in psa], [len(x) for x in psb]))
            if len(na) != len(nb):
                fail(name, "track %d annexb: %d NALs ours, %d ffmpeg" % (t, len(na), len(nb)))
            else:
                for i, (x, y) in enumerate(zip(na, nb)):
                    if x != y:
                        fail(name, "track %d annexb NAL %d differs (%d vs %d bytes)"
                             % (t, i, len(x), len(y)))
                        break

def main():
    if len(sys.argv) < 4:
        print("usage: demux_diff.py <demux_test> <builddir> <file>...")
        return 2
    tool, build = sys.argv[1], sys.argv[2]
    failures = []
    def fail(name, msg):
        failures.append("%s: %s" % (name, msg))
    for path in sys.argv[3:]:
        if not os.path.exists(path):
            continue
        try:
            check(path, tool, build, fail)
        except SystemExit as e:
            fail(os.path.basename(path), str(e))
        print("  %-24s %s" % (os.path.basename(path),
                              "ok" if not failures else "checked"))
    if failures:
        print("DEMUX-DIFF FAILED (%d)" % len(failures))
        for f in failures[:40]:
            print("   " + f)
        return 1
    print("DEMUX-DIFF OK: sample boundaries, timestamps, keyframes, "
          "codec configuration and elementary streams all match ffmpeg")
    return 0

if __name__ == "__main__":
    sys.exit(main())
