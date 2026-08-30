//! PNG **encoder** — RGBA8 in, a byte-exact PNG file out. The other direction
//! from [crate::png], and the first thing in this tree that WRITES an image.
//!
//! ---------------------------------------------------------------------------
//! WHY IT EXISTS, AND WHY IT IS NOT A WEAKENING OF THE RULE IT REPLACES
//! ---------------------------------------------------------------------------
//! `js_canvas.c` refused `toDataURL`/`toBlob` by name, and the reason was
//! right: *"a fabricated data URL is the single most load-bearing lie a canvas
//! can tell — it is what every fingerprint and every does-this-browser-support-
//! webp probe reads, and a wrong one is believed rather than detected."* That
//! argument is about FABRICATION, not about the method. The way past it is not
//! to relax the rule, it is to remove its premise: encode the pixels for real
//! and the answer stops being a lie. This file is that premise removal.
//!
//! ---------------------------------------------------------------------------
//! WHY IT IS ~200 LINES AND NOT A COMPRESSION PROJECT
//! ---------------------------------------------------------------------------
//! RFC 1951 §3.2.4 defines a STORED deflate block: `BFINAL`+`BTYPE=00`, pad to
//! a byte, then `LEN` and `~LEN` little-endian, then LEN literal bytes, LEN
//! <= 65535. A zlib stream (RFC 1950) made only of stored blocks is a fully
//! conforming zlib stream, so a PNG whose IDAT holds one is a fully conforming
//! PNG. **There is no Huffman coder and no LZ77 here, and their absence costs
//! bytes rather than correctness.** A real deflate slots in behind
//! [`deflate_stored`] later without any other line of this file changing.
//!
//! The output is roughly 1.0009x the raw RGBA plus 5 bytes per 64 KiB block.
//! For the sizes canvas fingerprints actually use (a few hundred pixels wide)
//! that is tens of kilobytes, and a correct large PNG beats a clever wrong one
//! in every way that matters here.
//!
//! ---------------------------------------------------------------------------
//! THE ORACLES, AND WHY ONE OF THEM IS NOT IN THIS TREE
//! ---------------------------------------------------------------------------
//! 1. **Round-trip through [crate::png]** — an independent, already-gated
//!    implementation of the other direction that was NOT written for this. It
//!    is a real differential test, not a self-check: encode a surface, decode
//!    it, and every pixel byte must come back identical.
//!
//! 2. **python3's `zlib` + `binascii.crc32`, OUTSIDE this tree.** Required,
//!    because oracle 1 cannot see a mistake the two halves SHARE. The claim was
//!    checked rather than assumed, and it came back **half wrong, in our
//!    favour and in one specific place**:
//!
//!    * `inflate.rs:272` DOES verify the Adler-32 trailer (`if adler32(outb) !=
//!      want { return -1 }`), so a wrong modulus here is caught by oracle 1
//!      after all. The first draft of this comment said it was not.
//!    * `png.rs` does **not** check chunk CRCs at all — `grep -ic crc
//!      rust/src/png.rs` is **0**. Measured directly: a copy of this encoder's
//!      own output with every chunk CRC overwritten with four zero bytes
//!      decodes as `png_decode -> 0, 37x11`, i.e. perfectly. A CRC of literal
//!      zeroes would have scored 100% on oracle 1.
//!
//!    So the shared blind spot is real, it is exactly one field wide, and
//!    tests/unit/pngenc_ext_test.py is the only thing in the repository that
//!    looks at it.
//!
//! ---------------------------------------------------------------------------
//! WHAT IT COSTS IN MEMORY, WHICH IS NOT NOTHING
//! ---------------------------------------------------------------------------
//! Peak is **two** buffers live at once: the raw filtered stream (`w*(4h+1)`)
//! and the output (~the same again). It inherits [`crate::imgbuf::MAX_PIXELS`],
//! 64 Mpx, so the ceiling here is ~512 MiB — more than `make run` gives the
//! whole guest. That ceiling is never approached by the caller this was built
//! for: `js_canvas.c`'s `CV_MAXPX` is 4 Mpx, so a canvas readback peaks around
//! 32 MiB transient. A future caller with a bigger surface needs a streaming
//! encoder, not a bigger number, and this note is here so that is a decision
//! rather than a discovery.
//!
//! Scope, stated so it is a decision and not an omission: colour type **6**
//! (truecolour with alpha), bit depth **8**, no interlacing, filter type **0**
//! (None) on every row. That is exactly what a `gfx_surface` / `ImageData`
//! already is — straight, non-premultiplied RGBA8 — so the encoder input needs
//! no conversion layer and cannot lose a channel on the way in.

use crate::imgbuf::{kfree, Buf, MAX_DIM, MAX_PIXELS};

/// CRC-32/ISO-HDLC (reflected, poly 0xEDB88320) — what PNG §5.5 specifies.
///
/// The table is **computed, not typed**. rust/src/vp8_tables.rs earns that rule
/// the hard way (a wrong probability desynchronises an arithmetic decoder into
/// noise); here a wrong table entry produces a file every other decoder rejects
/// and ours accepts, which is the same class of undetectable-from-inside bug.
/// A nibble table is 16 entries and two lookups per byte — small enough to read
/// and check by eye, unlike the 256-entry form.
/// NEGATIVE CONTROL `pngenc-bad-crc`: CRC-32C (Castagnoli), which is a real
/// polynomial a real person could pick, and which rust/src/png.rs cannot see —
/// it checks no chunk CRC at all. The round-trip gate stays green with this on;
/// only the external oracle reddens. That asymmetry is the argument for having
/// two oracles, and this switch is how it is watched rather than asserted.
#[cfg(feature = "pngenc-bad-crc")]
const CRC_POLY: u32 = 0x82F6_3B78;
#[cfg(not(feature = "pngenc-bad-crc"))]
const CRC_POLY: u32 = 0xEDB8_8320;

const fn crc_nibbles() -> [u32; 16] {
    let mut t = [0u32; 16];
    let mut i = 0usize;
    while i < 16 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 4 {
            c = if c & 1 != 0 { CRC_POLY ^ (c >> 1) } else { c >> 1 };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}
const CRC_T: [u32; 16] = crc_nibbles();

fn crc32(data: &[u8]) -> u32 {
    let mut c = 0xFFFF_FFFFu32;
    for &b in data {
        c ^= b as u32;
        c = (c >> 4) ^ CRC_T[(c & 0xf) as usize];
        c = (c >> 4) ^ CRC_T[(c & 0xf) as usize];
    }
    !c
}

/// Adler-32 (RFC 1950 §9) over the **raw, uncompressed** stream — i.e. the
/// filtered scanlines, not the deflate output. This one IS covered by the
/// round-trip oracle: `inflate.rs:272` refuses a stream whose trailer does not
/// match. (The chunk CRC below is the field that is not — see the module doc.)
///
/// The modulus is deferred in runs of 5552, the largest n for which the
/// worst-case sums cannot overflow u32 (RFC 1950's own note).
fn adler32(data: &[u8]) -> u32 {
    let mut a: u32 = 1;
    let mut b: u32 = 0;
    for chunk in data.chunks(5552) {
        for &x in chunk {
            a += x as u32;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    (b << 16) | a
}

/// Bytes a stored-block deflate stream of `n` raw bytes occupies, INCLUDING the
/// 2-byte zlib header and the 4-byte Adler-32 trailer.
///
/// `n == 0` still costs one (empty, final) block: a zero-length stored block is
/// legal and is what a zero-byte stream must be. This encoder never produces
/// one (w,h >= 1 means at least one filter byte) but the arithmetic below is
/// shared with the writer and must not disagree with it for any n.
fn stored_len(n: usize) -> usize {
    let blocks = if n == 0 { 1 } else { n.div_ceil(65535) };
    2 + blocks * 5 + n + 4
}

/// Write `raw` as a zlib stream of stored deflate blocks into `out`, returning
/// how many bytes were written (always `stored_len(raw.len())`).
///
/// **THE BLOCK BOUNDARY IS THE BUG THIS FUNCTION IS MOST LIKELY TO HAVE**, so
/// it is written to be checkable rather than clever: `LEN` is a u16, a canvas
/// of any interesting size overruns it (a 100x100 RGBA canvas is 40,100 raw
/// bytes; 200x100 is 80,200 and needs two blocks), and the failure mode of a
/// wrong `BFINAL` is a stream that decodes to a TRUNCATED image rather than to
/// an error. tests/unit/pngenc_test.c sizes a case specifically past 65535.
fn deflate_stored(raw: &[u8], out: &mut [u8]) -> usize {
    // zlib header: CM=8 (deflate), CINFO=7 (32K window), FLEVEL=0, FDICT=0,
    // FCHECK chosen so the big-endian u16 is a multiple of 31. 0x7801 % 31 == 0.
    out[0] = 0x78;
    out[1] = 0x01;
    let mut o = 2usize;
    let mut off = 0usize;
    loop {
        let take = core::cmp::min(65535, raw.len() - off);
        let last = off + take == raw.len();
        // NEGATIVE CONTROL `pngenc-always-final`: every block claims BFINAL.
        // Identical output for anything that fits in one 65535-byte block, and
        // a silent truncation above it -- so it is the control that proves the
        // 200x100 and 256x257 cases are load-bearing.
        #[cfg(feature = "pngenc-always-final")]
        let flag = 1u8;
        #[cfg(not(feature = "pngenc-always-final"))]
        let flag = if last { 1u8 } else { 0u8 };
        out[o] = flag; // BFINAL | BTYPE=00, byte-aligned
        out[o + 1] = (take & 0xff) as u8;
        out[o + 2] = (take >> 8) as u8;
        out[o + 3] = !(take & 0xff) as u8;
        out[o + 4] = !((take >> 8) & 0xff) as u8;
        o += 5;
        out[o..o + take].copy_from_slice(&raw[off..off + take]);
        o += take;
        off += take;
        if last {
            break;
        }
    }
    let ad = adler32(raw);
    out[o] = (ad >> 24) as u8;
    out[o + 1] = (ad >> 16) as u8;
    out[o + 2] = (ad >> 8) as u8;
    out[o + 3] = ad as u8;
    o + 4
}

/// Write one PNG chunk (length, type, data, CRC) at `out[o..]`; returns the new
/// offset. The CRC covers **type + data and not the length** (PNG §5.3) — the
/// single most common way a hand-written encoder produces a file that only its
/// own author's decoder accepts.
fn chunk(out: &mut [u8], o: usize, ty: &[u8; 4], data_len: usize) -> usize {
    out[o] = (data_len >> 24) as u8;
    out[o + 1] = (data_len >> 16) as u8;
    out[o + 2] = (data_len >> 8) as u8;
    out[o + 3] = data_len as u8;
    out[o + 4..o + 8].copy_from_slice(ty);
    // data is expected to be already in place at o+8 (the caller writes it
    // there directly, so a 16 MiB IDAT is never copied twice).
    let c = crc32(&out[o + 4..o + 8 + data_len]);
    let e = o + 8 + data_len;
    out[e] = (c >> 24) as u8;
    out[e + 1] = (c >> 16) as u8;
    out[e + 2] = (c >> 8) as u8;
    out[e + 3] = c as u8;
    e + 4
}

const SIG: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];

fn encode_inner(px: &[u8], w: i32, h: i32) -> Option<Buf> {
    if w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM {
        return None;
    }
    let (wu, hu) = (w as usize, h as usize);
    if wu.checked_mul(hu)? > MAX_PIXELS {
        return None;
    }
    let stride = wu.checked_mul(4)?;
    if px.len() < stride.checked_mul(hu)? {
        return None;
    }
    // The raw (pre-deflate) stream: one filter byte + one scanline, per row.
    let raw_len = (stride + 1).checked_mul(hu)?;
    let idat_len = stored_len(raw_len);
    let total = 8 + (12 + 13) + (12 + idat_len) + 12;

    // The raw stream is built in its own block and then deflated into the
    // output. That is one extra copy of the pixels; the alternative (writing
    // filtered rows straight into the IDAT's stored-block payloads) has to
    // interleave a 5-byte header into the middle of a scanline whenever a row
    // straddles a 65535 boundary, which is the same boundary bug in a place
    // where no test can see it.
    let mut rawb = Buf::new(raw_len)?;
    {
        let r = rawb.as_mut();
        for y in 0..hu {
            let d = y * (stride + 1);
            r[d] = 0; // filter type 0 = None, on every row
            r[d + 1..d + 1 + stride].copy_from_slice(&px[y * stride..y * stride + stride]);
        }
    }

    let mut out = Buf::zeroed(total)?;
    {
        let b = out.as_mut();
        b[0..8].copy_from_slice(&SIG);
        // IHDR payload, written in place before chunk() CRCs over it.
        let ih = 8 + 8;
        b[ih] = (wu >> 24) as u8;
        b[ih + 1] = (wu >> 16) as u8;
        b[ih + 2] = (wu >> 8) as u8;
        b[ih + 3] = wu as u8;
        b[ih + 4] = (hu >> 24) as u8;
        b[ih + 5] = (hu >> 16) as u8;
        b[ih + 6] = (hu >> 8) as u8;
        b[ih + 7] = hu as u8;
        b[ih + 8] = 8; // bit depth
        b[ih + 9] = 6; // colour type 6 = truecolour with alpha
        b[ih + 10] = 0; // compression method 0 = deflate
        b[ih + 11] = 0; // filter method 0 = adaptive, five types
        b[ih + 12] = 0; // interlace method 0 = none
        let mut o = chunk(b, 8, b"IHDR", 13);

        let n = deflate_stored(rawb.as_ref(), &mut b[o + 8..o + 8 + idat_len]);
        // stored_len() and deflate_stored() are the same arithmetic written
        // twice -- "one jar, two doors", the shape CLAUDE.md records three
        // separate outages for. They agree by construction for every n
        // (including 0), but they are the size a 256 MiB buffer was allocated
        // from and the size that gets written INTO it, so the equality is
        // checked rather than asserted: a debug_assert is compiled out of the
        // release build that ships, and the failure without it is a panic in a
        // `panic = "abort"` no_std staticlib linked into ring 0.
        if n != idat_len {
            return None;
        }
        o = chunk(b, o, b"IDAT", n);
        o = chunk(b, o, b"IEND", 0);
        if o != total {
            return None;
        }
    }
    Some(out)
}

/// Encode `w * h` straight (non-premultiplied) RGBA8 bytes at `px` as a PNG.
///
/// Returns a `kmalloc`'d block and writes its length through `out_len`, or NULL
/// on a bad size or OOM. **Free it with [`png_encode_free`], not with the
/// caller's `free`** — this crate links into two link domains with two
/// different allocators (the kernel heap; mini-libc's arena via browser_rt.c's
/// shim), and pairing the alloc with its own free is the only form that is
/// right in both without the caller having to know which one it is in.
///
/// # Safety
/// `px` must point to at least `w * h * 4` readable bytes; `out_len` must be a
/// writable `int`.
#[no_mangle]
pub extern "C" fn png_encode_rgba(px: *const u8, w: i32, h: i32, out_len: *mut i32) -> *mut u8 {
    if !out_len.is_null() {
        unsafe { *out_len = 0 };
    }
    if px.is_null() || out_len.is_null() || w <= 0 || h <= 0 {
        return core::ptr::null_mut();
    }
    if w > MAX_DIM || h > MAX_DIM || (w as usize) * (h as usize) > MAX_PIXELS {
        return core::ptr::null_mut();
    }
    let n = (w as usize) * (h as usize) * 4;
    let px = unsafe { core::slice::from_raw_parts(px, n) };
    match encode_inner(px, w, h) {
        Some(b) => {
            let len = b.len();
            // A PNG this big cannot exist here: MAX_PIXELS * 4 is 256 MiB and
            // the stored-block overhead is under 0.1%, so the i32 cannot wrap.
            unsafe { *out_len = len as i32 };
            b.into_raw()
        }
        None => core::ptr::null_mut(),
    }
}

/// Free a block [`png_encode_rgba`] returned. NULL is a no-op.
///
/// # Safety
/// `p` must be a pointer this module returned, freed at most once.
#[no_mangle]
pub extern "C" fn png_encode_free(p: *mut u8) {
    if !p.is_null() {
        unsafe { kfree(p) };
    }
}

/// Boot/self-test: encode a 3x2 RGBA image, decode it back through
/// [crate::png] — a decoder written years before this and for the other
/// direction — and compare every byte. 0 = OK, -1 = FAIL.
///
/// This is the ROUND-TRIP oracle only, and it is deliberately not the whole
/// gate: it cannot see a wrong CRC polynomial, because our decoder does not
/// check chunk CRCs. tests/unit/pngenc_ext_test.py is the half that can.
#[no_mangle]
pub extern "C" fn rust_pngenc_selftest() -> i32 {
    // Deliberately not uniform: a transposed axis or a dropped channel has to
    // change some byte here.
    const W: i32 = 3;
    const H: i32 = 2;
    const PX: [u8; 24] = [
        0, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 128, //
        0, 0, 255, 64, 255, 255, 255, 255, 17, 34, 51, 0,
    ];
    let mut n: i32 = 0;
    let p = png_encode_rgba(PX.as_ptr(), W, H, &mut n);
    if p.is_null() || n <= 0 {
        return -1;
    }
    let mut img = crate::png::Image { w: 0, h: 0, rgba: core::ptr::null_mut() };
    let r = crate::png::png_decode(p, n, &mut img);
    png_encode_free(p);
    if r != 0 || img.w != W || img.h != H || img.rgba.is_null() {
        if !img.rgba.is_null() {
            unsafe { kfree(img.rgba) };
        }
        return -1;
    }
    let back = unsafe { core::slice::from_raw_parts(img.rgba, PX.len()) };
    let ok = back == &PX[..];
    unsafe { kfree(img.rgba) };
    if ok {
        0
    } else {
        -1
    }
}
