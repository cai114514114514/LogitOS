//! ICO / CUR decoder in safe Rust. An icon file is a directory of images, each
//! of which is either a whole PNG (how 256x256 icons are stored since Vista) or
//! a headerless DIB with a 1-bit AND mask stacked under it. Both paths are
//! delegated: PNG to `png::png_decode`, DIB to `bmp::decode_dib`.
//!
//! This is the format the browser requests on literally every site it visits
//! (`/favicon.ico`), and it is the one that was missing entirely.
//!
//! Which entry wins: the largest area, ties broken by the deeper bit count.
//! A favicon is drawn at whatever size the UI wants, so downscaling the best
//! available image beats upscaling a 16x16 one.

use crate::bmp;
use crate::imgbuf::*;
use crate::png;

const DIRENT: usize = 16;

struct Entry {
    off: usize,
    len: usize,
    area: u32,
    bpp: u32,
}

fn entries(p: &[u8]) -> Option<Entry> {
    let count = le16(p, 4)? as usize;
    if count == 0 || count > 4096 {
        return None;
    }
    let mut best: Option<Entry> = None;
    for i in 0..count {
        let e = 6 + i * DIRENT;
        // A directory that claims more entries than the file holds is not fatal
        // as long as at least one earlier entry was usable.
        let w = match p.get(e) {
            Some(&0) => 256u32,
            Some(&v) => v as u32,
            None => break,
        };
        let h = match p.get(e + 1) {
            Some(&0) => 256u32,
            Some(&v) => v as u32,
            None => break,
        };
        let bpp = le16(p, e + 6).unwrap_or(0);
        let len = le32(p, e + 8)? as usize;
        let off = le32(p, e + 12)? as usize;
        if off >= p.len() || len == 0 {
            continue;
        }
        let len = len.min(p.len() - off);
        let cand = Entry { off, len, area: w * h, bpp };
        let better = match &best {
            None => true,
            Some(b) => cand.area > b.area || (cand.area == b.area && cand.bpp > b.bpp),
        };
        if better {
            best = Some(cand);
        }
    }
    best
}

fn decode_entry(d: &[u8]) -> Option<(i32, i32, Buf)> {
    if d.len() >= 8 && d[..8] == [0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a] {
        // A whole PNG inside the icon. Reuse the PNG decoder rather than a
        // second one; it already returns a kmalloc'd RGBA block.
        let mut im = Image { w: 0, h: 0, rgba: core::ptr::null_mut() };
        if png::png_decode(d.as_ptr(), d.len() as i32, &mut im) != 0 {
            return None;
        }
        let n = rgba_size(im.w, im.h);
        match n {
            Some(_) if !im.rgba.is_null() => {
                // Wrap the existing block; Buf takes ownership of the kmalloc.
                let w = im.w;
                let h = im.h;
                let buf = unsafe { Buf::from_raw(im.rgba, rgba_size(w, h)?) };
                Some((w, h, buf))
            }
            _ => {
                if !im.rgba.is_null() {
                    unsafe { kfree(im.rgba) };
                }
                None
            }
        }
    } else {
        bmp::decode_dib(d, true, None)
    }
}

fn decode_ico(p: &[u8]) -> Option<(i32, i32, Buf)> {
    let e = entries(p)?;
    decode_entry(p.get(e.off..e.off + e.len)?)
}

// ---- C ABI ----

#[no_mangle]
pub extern "C" fn ico_detect(p: *const u8, n: i32) -> i32 {
    if p.is_null() || n < 6 + DIRENT as i32 {
        return 0;
    }
    let s = unsafe { core::slice::from_raw_parts(p, 6) };
    // reserved=0, type=1 (icon) or 2 (cursor), count>0. This is a weak four-byte
    // signature, so ICO is registered LAST: every stronger magic gets first
    // refusal on the buffer.
    let typ = s[2] as u32 | (s[3] as u32) << 8;
    let cnt = s[4] as u32 | (s[5] as u32) << 8;
    i32::from(s[0] == 0 && s[1] == 0 && (typ == 1 || typ == 2) && cnt > 0)
}

#[no_mangle]
pub extern "C" fn ico_decode(p: *const u8, n: i32, out: *mut Image) -> i32 {
    if p.is_null() || out.is_null() || n < 6 {
        return -1;
    }
    let input = unsafe { core::slice::from_raw_parts(p, n as usize) };
    match decode_ico(input) {
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
pub extern "C" fn ico_register() {
    unsafe { img_register(ico_detect, ico_decode) };
}
