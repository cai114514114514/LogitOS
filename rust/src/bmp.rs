//! BMP / DIB decoder in safe Rust. Output is straight RGBA8.
//!
//! Coverage: BITMAPCOREHEADER (12) and BITMAPINFOHEADER/V2/V3/V4/V5 (>=40);
//! 1/4/8/16/24/32 bpp; BI_RGB, BI_BITFIELDS and BI_ALPHABITFIELDS with arbitrary
//! contiguous channel masks, BI_RLE8 and BI_RLE4; bottom-up and top-down
//! (negative height); palettes with 3- or 4-byte entries.
//!
//! Two deliberate semantics, both checked against a reference in
//! `tests/unit/bmp_test.c`:
//!  * 32bpp **BI_RGB** has no alpha. The fourth byte is "reserved" in the spec
//!    and is garbage in the wild (paint programs leave it 0, which as alpha
//!    would make the whole image invisible), so it is dropped and alpha is 255.
//!    Alpha is honoured only when a mask explicitly says where it is.
//!  * Channel values scale as `v * 255 / max`, truncating -- not bit
//!    replication. The two agree at 8 bits and disagree at 5 (16 -> 131 vs 132),
//!    and 255/max is what the reference decoder does.
//!
//! Also the DIB half of an .ico: `decode_dib` takes an `is_icon` flag, which
//! halves the stored height (an icon DIB stacks the colour image over a 1-bit
//! AND mask) and applies that mask as alpha.

use crate::imgbuf::*;

const MAXPAL: usize = 256;

/// Everything the header says, normalised across the header versions.
struct Dib {
    w: i32,
    h: i32,      // real height (icon: already halved), always > 0
    topdown: bool,
    bpp: u32,
    comp: u32,
    hdrsz: usize,
    pal_off: usize,
    pal_ent: usize,   // 3 (CORE) or 4
    npal: usize,
    pix_off: usize,   // offset of pixel data, relative to the DIB start
    mask: [u32; 4],   // R, G, B, A (0 = channel absent)
}

/// (shift, max) for a channel mask, or None if the mask is 0.
fn mask_info(m: u32) -> Option<(u32, u32)> {
    if m == 0 {
        return None;
    }
    let shift = m.trailing_zeros();
    let width = (m >> shift).trailing_ones();
    // Non-contiguous masks are not a thing any encoder emits; treating the low
    // contiguous run as the channel is well-defined and cannot go out of range.
    Some((shift, (1u32 << width) - 1))
}

fn chan(v: u32, mi: Option<(u32, u32)>, absent: u8) -> u8 {
    match mi {
        None => absent,
        Some((shift, max)) => {
            if max == 0 {
                absent
            } else {
                (((v >> shift) & max) * 255 / max) as u8
            }
        }
    }
}

/// Parse the DIB header at `d[0..]`. `file_pix_off` is the pixel-data offset
/// from a BMP file header (relative to the DIB start), or None for an icon,
/// where it is computed from the header + masks + palette.
fn parse_dib(d: &[u8], is_icon: bool, file_pix_off: Option<usize>) -> Option<Dib> {
    let hdrsz = le32(d, 0)? as usize;
    let (w, mut h, bpp, comp, clrused);
    if hdrsz == 12 {
        w = le16(d, 4)? as i32;
        h = le16(d, 6)? as i16 as i32;
        if le16(d, 8)? != 1 {
            return None;
        }
        bpp = le16(d, 10)?;
        comp = 0;
        clrused = 0;
    } else if hdrsz >= 40 && hdrsz <= 1024 {
        w = le32(d, 4)? as i32;
        h = le32(d, 8)? as i32;
        if le16(d, 12)? != 1 {
            return None;
        }
        bpp = le16(d, 14)?;
        comp = le32(d, 16)?;
        clrused = le32(d, 32)? as usize;
    } else {
        return None;
    }
    let topdown = h < 0;
    if topdown {
        // i32::MIN has no positive counterpart; reject rather than wrap.
        h = h.checked_neg()?;
    }
    if is_icon {
        // An icon DIB claims twice its height: colour rows then the AND mask.
        if h % 2 != 0 {
            return None;
        }
        h /= 2;
    }
    if w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM {
        return None;
    }
    if !matches!(bpp, 1 | 4 | 8 | 16 | 24 | 32) {
        return None;
    }
    // RLE only exists for the two indexed depths it was defined for.
    if (comp == 1 && bpp != 8) || (comp == 2 && bpp != 4) {
        return None;
    }
    if comp > 6 || comp == 4 || comp == 5 {
        return None; // 4/5 = a whole JPEG/PNG in a BMP; not supported
    }
    if (comp == 3 || comp == 6) && !matches!(bpp, 16 | 32) {
        return None;
    }

    // Channel masks. For a 40-byte header they follow it; V2+ carries them in
    // the header itself at fixed offsets 40/44/48/52.
    let mut mask = [0u32; 4];
    let mut extra = 0usize;
    if comp == 3 || comp == 6 {
        let nm = if comp == 6 { 4 } else { 3 };
        if hdrsz >= 52 {
            for (i, m) in mask.iter_mut().enumerate().take(nm.max(3)) {
                *m = le32(d, 40 + i * 4).unwrap_or(0);
            }
            if hdrsz >= 56 {
                mask[3] = le32(d, 52).unwrap_or(0);
            }
        } else {
            for (i, m) in mask.iter_mut().enumerate().take(nm) {
                *m = le32(d, hdrsz + i * 4)?;
            }
            extra = nm * 4;
        }
    } else if hdrsz >= 56 && bpp == 32 {
        // A V4/V5 header with BI_RGB still declares an alpha mask; honour it
        // when it is present and non-zero (this is how a 32bpp BMP carries
        // real transparency).
        mask[0] = le32(d, 40).unwrap_or(0);
        mask[1] = le32(d, 44).unwrap_or(0);
        mask[2] = le32(d, 48).unwrap_or(0);
        mask[3] = le32(d, 52).unwrap_or(0);
        if mask[0] == 0 && mask[1] == 0 && mask[2] == 0 {
            mask = [0x00ff_0000, 0x0000_ff00, 0x0000_00ff, mask[3]];
        }
    }
    if mask[0] == 0 && mask[1] == 0 && mask[2] == 0 {
        mask = match bpp {
            16 => [0x7c00, 0x03e0, 0x001f, 0],       // X1R5G5B5
            _ => [0x00ff_0000, 0x0000_ff00, 0x0000_00ff, 0],
        };
    }
    // A 32bpp *icon* is the one place the fourth byte of BI_RGB really is
    // alpha: that is the post-XP icon convention, and dropping it renders every
    // modern favicon with hard aliased edges on an opaque square. Standalone
    // 32bpp BMPs keep the opposite default (see the module comment) -- the
    // container decides, which is why this is keyed on `is_icon` and not on bpp.
    if is_icon && bpp == 32 && mask[3] == 0 {
        mask[3] = 0xff00_0000;
    }

    let pal_ent = if hdrsz == 12 { 3 } else { 4 };
    let pal_off = hdrsz + extra;
    let npal = if bpp <= 8 {
        let n = if clrused != 0 { clrused } else { 1usize << bpp };
        if n > MAXPAL {
            return None;
        }
        n
    } else {
        0
    };
    let pix_off = match file_pix_off {
        Some(o) => o,
        None => pal_off + npal * pal_ent,
    };
    if pix_off > d.len() {
        return None;
    }
    Some(Dib { w, h, topdown, bpp, comp, hdrsz, pal_off, pal_ent, npal, pix_off, mask })
}

/// Palette as RGB triples (index i -> pal[i*3..]), zero-filled beyond `npal`.
fn read_palette(d: &[u8], dib: &Dib) -> [u8; MAXPAL * 3] {
    let mut pal = [0u8; MAXPAL * 3];
    for i in 0..dib.npal {
        let o = dib.pal_off + i * dib.pal_ent;
        if o + 3 > d.len() {
            break;
        }
        pal[i * 3] = d[o + 2];
        pal[i * 3 + 1] = d[o + 1];
        pal[i * 3 + 2] = d[o];
    }
    pal
}

/// Row `y` of the OUTPUT maps to which stored row.
fn out_row(dib: &Dib, stored: usize) -> usize {
    if dib.topdown {
        stored
    } else {
        dib.h as usize - 1 - stored
    }
}

/// Uncompressed scanlines -> rgba. Truncated pixel data leaves the remaining
/// rows at their zeroed value rather than failing: a partially received image
/// is the common case behind `res_fetch`'s 64 KiB truncation.
fn decode_raw(d: &[u8], dib: &Dib, pal: &[u8; MAXPAL * 3], rgba: &mut [u8]) {
    let w = dib.w as usize;
    let stride = ((w * dib.bpp as usize + 31) / 32) * 4;
    let (mr, mg, mb, ma) = (
        mask_info(dib.mask[0]),
        mask_info(dib.mask[1]),
        mask_info(dib.mask[2]),
        mask_info(dib.mask[3]),
    );
    for sy in 0..dib.h as usize {
        let ro = dib.pix_off + sy * stride;
        if ro + stride > d.len() {
            break;
        }
        let line = &d[ro..ro + stride];
        let oy = out_row(dib, sy);
        for x in 0..w {
            let o = (oy * w + x) * 4;
            let (r, g, b, a) = match dib.bpp {
                1 | 4 | 8 => {
                    let bits = dib.bpp as usize;
                    let bit = x * bits;
                    let idx = ((line[bit >> 3] as usize) >> (8 - bits - (bit & 7))) & ((1 << bits) - 1);
                    (pal[idx * 3], pal[idx * 3 + 1], pal[idx * 3 + 2], 255)
                }
                16 => {
                    let v = line[x * 2] as u32 | (line[x * 2 + 1] as u32) << 8;
                    (chan(v, mr, 0), chan(v, mg, 0), chan(v, mb, 0), chan(v, ma, 255))
                }
                24 => (line[x * 3 + 2], line[x * 3 + 1], line[x * 3], 255),
                _ => {
                    let v = line[x * 4] as u32
                        | (line[x * 4 + 1] as u32) << 8
                        | (line[x * 4 + 2] as u32) << 16
                        | (line[x * 4 + 3] as u32) << 24;
                    (chan(v, mr, 0), chan(v, mg, 0), chan(v, mb, 0), chan(v, ma, 255))
                }
            };
            rgba[o] = r;
            rgba[o + 1] = g;
            rgba[o + 2] = b;
            rgba[o + 3] = a;
        }
    }
}

/// BI_RLE8 / BI_RLE4. Both are the same state machine over (count, value)
/// pairs with three escapes: 0,0 = end of line; 0,1 = end of bitmap; 0,2 =
/// a (dx, dy) delta; 0,n>=3 = `n` absolute pixels, word-aligned.
fn decode_rle(d: &[u8], dib: &Dib, pal: &[u8; MAXPAL * 3], rgba: &mut [u8]) {
    let w = dib.w as usize;
    let h = dib.h as usize;
    let four = dib.comp == 2;
    let mut i = dib.pix_off;
    let (mut x, mut y) = (0usize, 0usize);
    let put = |rgba: &mut [u8], x: usize, y: usize, idx: usize| {
        if x >= w || y >= h {
            return;
        }
        let oy = if dib.topdown { y } else { h - 1 - y };
        let o = (oy * w + x) * 4;
        rgba[o] = pal[idx * 3];
        rgba[o + 1] = pal[idx * 3 + 1];
        rgba[o + 2] = pal[idx * 3 + 2];
        rgba[o + 3] = 255;
    };
    while i + 1 < d.len() {
        let cnt = d[i] as usize;
        let val = d[i + 1] as usize;
        i += 2;
        if cnt > 0 {
            // Encoded run: `cnt` pixels of one value (RLE4 alternates nibbles).
            for k in 0..cnt {
                let idx = if four {
                    if k & 1 == 0 { val >> 4 } else { val & 15 }
                } else {
                    val
                };
                put(rgba, x, y, idx);
                x += 1;
            }
            continue;
        }
        match val {
            0 => {
                x = 0;
                y += 1;
            }
            1 => break,
            2 => {
                if i + 1 >= d.len() {
                    break;
                }
                x += d[i] as usize;
                y += d[i + 1] as usize;
                i += 2;
            }
            n => {
                // Absolute mode: n literal pixels, then pad to a 16-bit boundary.
                let bytes = if four { (n + 1) / 2 } else { n };
                if i + bytes > d.len() {
                    break;
                }
                for k in 0..n {
                    let idx = if four {
                        let b = d[i + k / 2] as usize;
                        if k & 1 == 0 { b >> 4 } else { b & 15 }
                    } else {
                        d[i + k] as usize
                    };
                    put(rgba, x, y, idx);
                    x += 1;
                }
                i += bytes + (bytes & 1);
            }
        }
        if y >= h {
            break;
        }
    }
}

/// Apply an icon's trailing 1-bit AND mask as alpha (set bit = transparent).
/// Only called when the colour data carried no alpha of its own.
fn apply_and_mask(d: &[u8], dib: &Dib, rgba: &mut [u8]) {
    let w = dib.w as usize;
    let h = dib.h as usize;
    let colour_stride = ((w * dib.bpp as usize + 31) / 32) * 4;
    let mask_off = dib.pix_off + colour_stride * h;
    let mstride = ((w + 31) / 32) * 4;
    for sy in 0..h {
        let ro = mask_off + sy * mstride;
        if ro + mstride > d.len() {
            return; // no mask stored: leave everything opaque
        }
        let line = &d[ro..ro + mstride];
        let oy = out_row(dib, sy);
        for x in 0..w {
            if (line[x >> 3] >> (7 - (x & 7))) & 1 != 0 {
                rgba[(oy * w + x) * 4 + 3] = 0;
            }
        }
    }
}

/// Decode a DIB (with no BMP file header) into a fresh RGBA buffer.
/// `file_pix_off` is relative to the DIB start.
pub fn decode_dib(d: &[u8], is_icon: bool, file_pix_off: Option<usize>) -> Option<(i32, i32, Buf)> {
    let dib = parse_dib(d, is_icon, file_pix_off)?;
    let pal = read_palette(d, &dib);
    let mut buf = Buf::zeroed(rgba_size(dib.w, dib.h)?)?;
    {
        let rgba = buf.as_mut();
        if dib.comp == 1 || dib.comp == 2 {
            decode_rle(d, &dib, &pal, rgba);
        } else {
            decode_raw(d, &dib, &pal, rgba);
        }
    }
    if is_icon {
        // 32bpp icons carry alpha in the colour data; pre-Vista ones leave it
        // all zero and mean the AND mask instead. All-zero alpha would render
        // the icon invisible, so that is the tell.
        let any_alpha = dib.bpp == 32 && buf.as_ref().iter().skip(3).step_by(4).any(|&a| a != 0);
        if !any_alpha {
            if dib.bpp == 32 {
                for a in buf.as_mut().iter_mut().skip(3).step_by(4) {
                    *a = 255;
                }
            }
            apply_and_mask(d, &dib, buf.as_mut());
        }
    }
    let _ = dib.hdrsz;
    Some((dib.w, dib.h, buf))
}

fn decode_file(p: &[u8]) -> Option<(i32, i32, Buf)> {
    if p.len() < 14 {
        return None;
    }
    let off = le32(p, 10)? as usize;
    // The pixel offset is absolute in the file; the DIB slice starts at 14.
    let pix = if off >= 14 { Some(off - 14) } else { None };
    decode_dib(p.get(14..)?, false, pix)
}

// ---- C ABI ----

#[no_mangle]
pub extern "C" fn bmp_detect(p: *const u8, n: i32) -> i32 {
    if p.is_null() || n < 14 {
        return 0;
    }
    let s = unsafe { core::slice::from_raw_parts(p, 2) };
    // "BM" only. BA/CI/CP/IC/PT are OS/2 array/pointer variants nobody serves.
    i32::from(s[0] == b'B' && s[1] == b'M')
}

#[no_mangle]
pub extern "C" fn bmp_decode(p: *const u8, n: i32, out: *mut Image) -> i32 {
    if p.is_null() || out.is_null() || n < 14 {
        return -1;
    }
    let input = unsafe { core::slice::from_raw_parts(p, n as usize) };
    match decode_file(input) {
        Some((w, h, buf)) => {
            unsafe {
                (*out).w = w;
                (*out).h = h;
                (*out).rgba = buf.into_raw();
            }
            0
        }
        None => -1,
    }
}

#[no_mangle]
pub extern "C" fn bmp_register() {
    unsafe { img_register(bmp_detect, bmp_decode) };
}
