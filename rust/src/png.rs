//! PNG decoder, ported from `c/lib/image/png.c` to **safe Rust**. Second hybrid
//! C+Rust module (after [inflate]). It decodes UNTRUSTED input (web/disk PNGs) --
//! the bug class the security audit kept finding -- so every access is a
//! bounds-checked slice index; malformed input returns -1 and NEVER panics,
//! aborts, or touches memory out of bounds.
//!
//! Same C ABI as the original (drop-in): the C png.o is excluded from both the
//! kernel build and the ring-3 browser pipeline; these `#[no_mangle]` symbols
//! (`png_register`/`png_detect`/`png_decode`) replace it in both link domains.
//! The three heap buffers use the host's `kmalloc`/`kfree` (real in the kernel,
//! shimmed onto malloc/free by the browser's browser_rt.c), so the decoded RGBA
//! pointer is freeable by `img_free`'s `kfree` exactly as before.
//!
//! Coverage matches the C: all colour types (gray/RGB/palette/gray+alpha/RGBA),
//! all bit depths (1/2/4/8/16), the five filters, Adam7 interlacing, and tRNS
//! colour-key / per-index alpha. 16-bit samples reduce to 8-bit; output is RGBA8.

use core::slice;

/// Mirrors C `struct image { int w, h; uint8_t *rgba; }`.
#[repr(C)]
pub struct Image {
    pub w: i32,
    pub h: i32,
    pub rgba: *mut u8,
}

type DetectFn = extern "C" fn(*const u8, i32) -> i32;
type DecodeFn = extern "C" fn(*const u8, i32, *mut Image) -> i32;

extern "C" {
    fn kmalloc(n: usize) -> *mut u8;
    fn kfree(p: *mut u8);
    fn img_register(detect: DetectFn, decode: DecodeFn);
}

fn be32(p: &[u8]) -> u32 {
    ((p[0] as u32) << 24) | ((p[1] as u32) << 16) | ((p[2] as u32) << 8) | p[3] as u32
}

fn paeth(a: i32, b: i32, c: i32) -> i32 {
    let p = a + b - c;
    let pa = (p - a).abs();
    let pb = (p - b).abs();
    let pc = (p - c).abs();
    if pa <= pb && pa <= pc {
        a
    } else if pb <= pc {
        b
    } else {
        c
    }
}

/// Raw sample `s` of a scanline at the given bit depth (1/2/4 -> 0..2^d-1,
/// 8 -> byte, 16 -> 0..65535, MSB-first packing for sub-byte depths).
fn sample(line: &[u8], depth: i32, s: usize) -> i32 {
    if depth == 16 {
        return ((line[s * 2] as i32) << 8) | line[s * 2 + 1] as i32;
    }
    if depth == 8 {
        return line[s] as i32;
    }
    let bit = s * depth as usize;
    let byte = bit >> 3;
    let shift = 8 - depth - (bit & 7) as i32;
    let mask = (1i32 << depth) - 1;
    ((line[byte] as i32) >> shift) & mask
}

fn to8(v: i32, depth: i32) -> u8 {
    (if depth == 16 {
        v >> 8
    } else if depth == 8 {
        v
    } else {
        v * 255 / ((1 << depth) - 1)
    }) as u8
}

/// depth allowed for each colour type?
fn depth_ok(ctype: i32, depth: i32) -> bool {
    match ctype {
        0 => depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16,
        3 => depth == 1 || depth == 2 || depth == 4 || depth == 8,
        2 | 4 | 6 => depth == 8 || depth == 16,
        _ => false,
    }
}

/// Unfilter `rows` scanlines in place; each is 1 filter byte + `stride` data
/// bytes. `bpp` = bytes per pixel for the a/c taps (ceil(channels*depth/8), >=1).
/// Absolute indexing into `buf` (rather than a `prev` cursor) keeps the borrow
/// checker happy while staying bounds-checked. Returns false on a filter type
/// byte outside 0..=4 (a spec violation the old code silently treated as None).
fn unfilter(buf: &mut [u8], rows: usize, stride: usize, bpp: usize) -> bool {
    for y in 0..rows {
        let rs = y * (stride + 1);
        let ft = buf[rs];
        if ft > 4 {
            return false;
        }
        for x in 0..stride {
            let a = if x >= bpp { buf[rs + 1 + x - bpp] as i32 } else { 0 };
            let b = if y > 0 { buf[(y - 1) * (stride + 1) + 1 + x] as i32 } else { 0 };
            let c = if y > 0 && x >= bpp {
                buf[(y - 1) * (stride + 1) + 1 + x - bpp] as i32
            } else {
                0
            };
            let mut v = buf[rs + 1 + x] as i32;
            match ft {
                1 => v += a,
                2 => v += b,
                3 => v += (a + b) / 2,
                4 => v += paeth(a, b, c),
                _ => {}
            }
            buf[rs + 1 + x] = v as u8;
        }
    }
    true
}

/// Write pixel `col` of an unfiltered scanline `line` to rgba[(y*W + x)].
#[allow(clippy::too_many_arguments)]
fn emit(
    rgba: &mut [u8],
    w: i32,
    x: i32,
    y: i32,
    line: &[u8],
    col: usize,
    ch: usize,
    depth: i32,
    ctype: i32,
    pal: &[u8],
    pala: &[u8],
    key: bool,
    kr: i32,
    kg: i32,
    kb: i32,
) {
    let o = (y as usize * w as usize + x as usize) * 4;
    let base = col * ch;
    if ctype == 3 {
        let idx = sample(line, depth, base) as usize; // depth<=8 -> idx<=255
        rgba[o] = pal[idx * 3];
        rgba[o + 1] = pal[idx * 3 + 1];
        rgba[o + 2] = pal[idx * 3 + 2];
        rgba[o + 3] = pala[idx];
    } else if ctype == 0 {
        let g = sample(line, depth, base);
        let v = to8(g, depth);
        rgba[o] = v;
        rgba[o + 1] = v;
        rgba[o + 2] = v;
        rgba[o + 3] = if key && g == kr { 0 } else { 255 };
    } else if ctype == 4 {
        let g = sample(line, depth, base);
        let a = sample(line, depth, base + 1);
        let v = to8(g, depth);
        rgba[o] = v;
        rgba[o + 1] = v;
        rgba[o + 2] = v;
        rgba[o + 3] = to8(a, depth);
    } else if ctype == 2 {
        let r = sample(line, depth, base);
        let g = sample(line, depth, base + 1);
        let b = sample(line, depth, base + 2);
        rgba[o] = to8(r, depth);
        rgba[o + 1] = to8(g, depth);
        rgba[o + 2] = to8(b, depth);
        rgba[o + 3] = if key && r == kr && g == kg && b == kb { 0 } else { 255 };
    } else {
        let r = sample(line, depth, base);
        let g = sample(line, depth, base + 1);
        let b = sample(line, depth, base + 2);
        let a = sample(line, depth, base + 3);
        rgba[o] = to8(r, depth);
        rgba[o + 1] = to8(g, depth);
        rgba[o + 2] = to8(b, depth);
        rgba[o + 3] = to8(a, depth);
    }
}

/// Decoded geometry: the per-pass column/row/stride tables + the total raw size.
struct Geom {
    pcols: [i32; 7],
    prows: [i32; 7],
    pstride: [usize; 7],
    npass: usize,
    raw_total: usize,
}

const OX: [i32; 7] = [0, 4, 0, 2, 0, 1, 0];
const OY: [i32; 7] = [0, 0, 4, 0, 2, 0, 1];
const SX: [i32; 7] = [8, 8, 4, 4, 2, 2, 1];
const SY: [i32; 7] = [8, 8, 8, 4, 4, 2, 2];

fn geometry(w: i32, h: i32, ch: usize, depth: i32, interlace: i32) -> Geom {
    let npass = if interlace != 0 { 7 } else { 1 };
    let mut g = Geom { pcols: [0; 7], prows: [0; 7], pstride: [0; 7], npass, raw_total: 0 };
    for pass in 0..npass {
        let (cols, rows) = if interlace != 0 {
            let cols = if w > OX[pass] { (w - OX[pass] + SX[pass] - 1) / SX[pass] } else { 0 };
            let rows = if h > OY[pass] { (h - OY[pass] + SY[pass] - 1) / SY[pass] } else { 0 };
            (cols, rows)
        } else {
            (w, h)
        };
        g.pcols[pass] = cols;
        g.prows[pass] = rows;
        g.pstride[pass] = (cols as usize * ch * depth as usize + 7) / 8;
        if cols > 0 && rows > 0 {
            g.raw_total += (g.pstride[pass] + 1) * rows as usize;
        }
    }
    g
}

/// Parsed header/palette/transparency from the first chunk scan.
struct Meta {
    w: i32,
    h: i32,
    depth: i32,
    ctype: i32,
    interlace: i32,
    pal: [u8; 256 * 3],
    pala: [u8; 256],
    trns: [u8; 6],
    ntrns: usize,
    idat_total: usize,
    have_ihdr: bool,
    iend: bool,
}

/// First pass: walk chunks, capture IHDR/PLTE/tRNS, sum IDAT length, find IEND.
/// Does NOT copy IDAT (a second pass does, once we've sized the buffer). The
/// `clen` overflow guard matches the hardened C: reject >= 0x80000000 and any
/// chunk that would run past the buffer (no signed wrap).
fn scan_meta(p: &[u8]) -> Meta {
    let mut m = Meta {
        w: 0,
        h: 0,
        depth: 0,
        ctype: 0,
        interlace: 0,
        pal: [0; 256 * 3],
        pala: [255; 256],
        trns: [0; 6],
        ntrns: 0,
        idat_total: 0,
        have_ihdr: false,
        iend: false,
    };
    let mut i = 8usize;
    while i + 8 <= p.len() {
        let clen = be32(&p[i..i + 4]);
        if clen > 0x7fff_ffff {
            break;
        }
        let clen = clen as usize;
        if i + 12 + clen > p.len() {
            break;
        }
        let typ = &p[i + 4..i + 8];
        let data = &p[i + 8..i + 8 + clen];
        // Deliberate divergence from the old C: a truncated IHDR (clen < 13) is
        // skipped here (-> no have_ihdr -> decode fails cleanly), whereas the C
        // read 13 bytes regardless -- a latent OOB read on a malformed file.
        // The spec requires IHDR to be the FIRST chunk (i == 8).
        if typ == b"IHDR" && clen >= 13 && i == 8 {
            m.w = be32(&data[0..4]) as i32;
            m.h = be32(&data[4..8]) as i32;
            m.depth = data[8] as i32;
            m.ctype = data[9] as i32;
            m.interlace = data[12] as i32;
            m.have_ihdr = true;
        } else if typ == b"PLTE" {
            let mut np = clen / 3;
            if np > 256 {
                np = 256;
            }
            m.pal[..np * 3].copy_from_slice(&data[..np * 3]);
        } else if typ == b"tRNS" {
            if m.ctype == 3 {
                let k = if clen > 256 { 256 } else { clen };
                m.pala[..k].copy_from_slice(&data[..k]);
            } else {
                m.ntrns = if clen > 6 { 6 } else { clen };
                m.trns[..m.ntrns].copy_from_slice(&data[..m.ntrns]);
            }
        } else if typ == b"IDAT" {
            m.idat_total += clen;
        } else if typ == b"IEND" {
            m.iend = true;
            break;
        }
        i += 12 + clen;
    }
    m
}

/// Second pass: copy every IDAT chunk's data into `dst` (exactly idat_total bytes).
fn gather_idat(p: &[u8], dst: &mut [u8]) {
    let mut off = 0usize;
    let mut i = 8usize;
    while i + 8 <= p.len() {
        let clen = be32(&p[i..i + 4]);
        if clen > 0x7fff_ffff {
            break;
        }
        let clen = clen as usize;
        if i + 12 + clen > p.len() {
            break;
        }
        let typ = &p[i + 4..i + 8];
        if typ == b"IDAT" {
            dst[off..off + clen].copy_from_slice(&p[i + 8..i + 8 + clen]);
            off += clen;
        } else if typ == b"IEND" {
            break;
        }
        i += 12 + clen;
    }
}

/// The full decode over a borrowed input slice. Returns (w, h, rgba_ptr) on
/// success; the rgba buffer is a raw `kmalloc` block the caller hands to C.
/// Returns None on any malformed input or allocation failure (all scratch freed).
fn decode_inner(p: &[u8]) -> Option<(i32, i32, *mut u8)> {
    let m = scan_meta(p);
    if !m.iend || !m.have_ihdr || m.w <= 0 || m.h <= 0 || !depth_ok(m.ctype, m.depth)
        || m.interlace > 1
    {
        return None;
    }
    if m.w > 8192 || m.h > 8192 {
        return None;
    }

    let ch: usize = match m.ctype {
        0 | 3 => 1,
        2 => 3,
        4 => 2,
        _ => 4,
    };
    let bpp = (((ch as i32 * m.depth + 7) / 8).max(1)) as usize;

    let (mut key, mut kr, mut kg, mut kb) = (false, 0i32, 0i32, 0i32);
    if m.ntrns >= 1 && m.ctype == 0 {
        key = true;
        kr = ((m.trns[0] as i32) << 8) | m.trns[1] as i32;
    } else if m.ntrns >= 6 && m.ctype == 2 {
        key = true;
        kr = ((m.trns[0] as i32) << 8) | m.trns[1] as i32;
        kg = ((m.trns[2] as i32) << 8) | m.trns[3] as i32;
        kb = ((m.trns[4] as i32) << 8) | m.trns[5] as i32;
    }

    let g = geometry(m.w, m.h, ch, m.depth, m.interlace);
    if g.raw_total == 0 || g.raw_total > i32::MAX as usize {
        return None;
    }

    // --- idat (scratch): kmalloc, fill via second pass ---
    let idat = unsafe { kmalloc(m.idat_total.max(1)) };
    if idat.is_null() {
        return None;
    }
    if m.idat_total > 0 {
        let idat_slice = unsafe { slice::from_raw_parts_mut(idat, m.idat_total) };
        gather_idat(p, idat_slice);
    }

    // --- raw (scratch): kmalloc, zlib-inflate idat into it ---
    let raw = unsafe { kmalloc(g.raw_total) };
    if raw.is_null() {
        unsafe { kfree(idat) };
        return None;
    }
    let mut rawlen: i32 = 0;
    let rc = crate::inflate::zlib_decompress(
        idat as *const u8,
        m.idat_total as i32,
        raw,
        g.raw_total as i32,
        &mut rawlen,
    );
    unsafe { kfree(idat) };
    if rc != 0 || (rawlen as usize) < g.raw_total {
        unsafe { kfree(raw) };
        return None;
    }

    // --- rgba (output): kmalloc; handed to C, freed by img_free's kfree ---
    let wh4 = m.w as usize * m.h as usize * 4;
    let rgba = unsafe { kmalloc(wh4) };
    if rgba.is_null() {
        unsafe { kfree(raw) };
        return None;
    }

    let raw_slice = unsafe { slice::from_raw_parts_mut(raw, g.raw_total) };
    let rgba_slice = unsafe { slice::from_raw_parts_mut(rgba, wh4) };

    let mut off = 0usize;
    for pass in 0..g.npass {
        let cols = g.pcols[pass];
        let rows = g.prows[pass];
        let stride = g.pstride[pass];
        if cols == 0 || rows == 0 {
            continue;
        }
        let blocklen = (stride + 1) * rows as usize;
        let block = &mut raw_slice[off..off + blocklen];
        off += blocklen;
        if !unfilter(block, rows as usize, stride, bpp) {
            unsafe { kfree(raw) };
            unsafe { kfree(rgba) };
            return None;
        }
        for r in 0..rows as usize {
            let line = &block[r * (stride + 1) + 1..r * (stride + 1) + 1 + stride];
            let y = if m.interlace != 0 { OY[pass] + r as i32 * SY[pass] } else { r as i32 };
            for c in 0..cols as usize {
                let x = if m.interlace != 0 { OX[pass] + c as i32 * SX[pass] } else { c as i32 };
                emit(
                    rgba_slice, m.w, x, y, line, c, ch, m.depth, m.ctype, &m.pal, &m.pala, key, kr,
                    kg, kb,
                );
            }
        }
    }

    unsafe { kfree(raw) };
    Some((m.w, m.h, rgba))
}

// ---- C ABI ----

#[no_mangle]
pub extern "C" fn png_detect(p: *const u8, n: i32) -> i32 {
    if p.is_null() || n < 8 {
        return 0;
    }
    let s = unsafe { slice::from_raw_parts(p, 8) };
    const SIG: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
    if *s == SIG {
        1
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn png_decode(p: *const u8, n: i32, out: *mut Image) -> i32 {
    if p.is_null() || out.is_null() || n < 8 + 25 {
        return -1;
    }
    let input = unsafe { slice::from_raw_parts(p, n as usize) };
    match decode_inner(input) {
        Some((w, h, rgba)) => {
            unsafe {
                (*out).w = w;
                (*out).h = h;
                (*out).rgba = rgba;
            }
            0
        }
        None => -1,
    }
}

#[no_mangle]
pub extern "C" fn png_register() {
    unsafe { img_register(png_detect, png_decode) };
}

/// Boot self-test: decode a baked-in 4x3 RGBA PNG (rows filtered
/// None/Sub/Up/Paeth, so the unfilter + inflate + emit paths all run) and
/// check every output byte against the known-good RGBA. 0 = OK, -1 = FAIL.
#[no_mangle]
pub extern "C" fn rust_png_selftest() -> i32 {
    // 4x3 RGBA PNG, rows filtered None/Sub/Up/Paeth (exercises unfilter).
    const PNG_VEC: [u8; 100] = [
        137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 4, 0, 0, 0, 3, 8, 6,
        0, 0, 0, 180, 244, 174, 198, 0, 0, 0, 43, 73, 68, 65, 84, 120, 218, 99, 224, 18, 97, 253,
        239, 38, 162, 252, 186, 73, 196, 241, 250, 62, 145, 248, 195, 140, 92, 41, 202, 255, 109,
        24, 228, 222, 192, 48, 19, 67, 128, 28, 3, 50, 6, 0, 147, 157, 13, 74, 61, 61, 177, 18, 0,
        0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130,
    ];
    const PNG_EXPECT: [u8; 48] = [
        10, 20, 5, 255, 70, 20, 35, 235, 130, 20, 65, 215, 190, 20, 95, 195, 10, 100, 35, 255, 70,
        100, 65, 235, 130, 100, 95, 215, 190, 100, 125, 195, 10, 180, 65, 255, 70, 180, 95, 235,
        130, 180, 125, 215, 190, 180, 155, 195,
    ];
    let mut img = Image { w: 0, h: 0, rgba: core::ptr::null_mut() };
    if png_decode(PNG_VEC.as_ptr(), PNG_VEC.len() as i32, &mut img) != 0 {
        return -1;
    }
    if img.w != 4 || img.h != 3 || img.rgba.is_null() {
        if !img.rgba.is_null() {
            unsafe { kfree(img.rgba) };
        }
        return -1;
    }
    let got = unsafe { slice::from_raw_parts(img.rgba, PNG_EXPECT.len()) };
    let ok = *got == PNG_EXPECT;
    unsafe { kfree(img.rgba) };
    if ok {
        0
    } else {
        -1
    }
}
