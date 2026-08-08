//! Shared plumbing for the Rust image decoders: the C `struct image`, the three
//! C symbols they are allowed to call, and an owning `kmalloc` buffer.
//!
//! `Buf` exists because the decoders below allocate several scratch blocks and
//! bail out from a dozen places. The hand-written C equivalent of that is a
//! `goto fail:` ladder, and the bug it hides is the one path that returns before
//! reaching the ladder. `Buf` frees on drop, including on the early returns, so
//! there is no ladder to get wrong; handing the block to C is the explicit
//! `into_raw()`, which is the only way to opt out of the free.

use core::slice;

/// Mirrors C `struct image { int w, h; uint8_t *rgba; }`.
#[repr(C)]
pub struct Image {
    pub w: i32,
    pub h: i32,
    pub rgba: *mut u8,
}

/// Mirrors C `struct img_frame { int delay_ms; uint8_t *rgba; }`.
#[repr(C)]
pub struct ImgFrame {
    pub delay_ms: i32,
    pub rgba: *mut u8,
}

/// Mirrors C `struct img_anim`.
#[repr(C)]
pub struct ImgAnim {
    pub w: i32,
    pub h: i32,
    pub nframes: i32,
    pub loops: i32,
    pub frames: *mut ImgFrame,
}

pub type DetectFn = extern "C" fn(*const u8, i32) -> i32;
pub type DecodeFn = extern "C" fn(*const u8, i32, *mut Image) -> i32;
pub type AnimFn = extern "C" fn(*const u8, i32, *mut ImgAnim) -> i32;

extern "C" {
    pub fn kmalloc(n: usize) -> *mut u8;
    pub fn kfree(p: *mut u8);
    pub fn img_register(detect: DetectFn, decode: DecodeFn);
    pub fn img_register_anim(detect: DetectFn, decode: DecodeFn, anim: AnimFn);
}

/// No decoder in this crate will allocate a canvas bigger than this. It is the
/// same 8192 cap png.rs already applies to width and height, expressed as an
/// area so that 8192x8192 (256 MiB of RGBA) cannot be requested by a 30-byte
/// header -- the classic "decompression bomb" shape.
pub const MAX_DIM: i32 = 8192;
pub const MAX_PIXELS: usize = 64 << 20; // 64 Mpx == 256 MiB RGBA ceiling

/// An owning `kmalloc` block. Freed on drop unless `into_raw` is called.
pub struct Buf {
    p: *mut u8,
    n: usize,
}

impl Buf {
    /// Allocate `n` bytes (uninitialised). `None` on OOM or a zero request.
    pub fn new(n: usize) -> Option<Buf> {
        if n == 0 {
            return None;
        }
        let p = unsafe { kmalloc(n) };
        if p.is_null() {
            None
        } else {
            Some(Buf { p, n })
        }
    }

    /// Allocate `n` bytes and zero them.
    pub fn zeroed(n: usize) -> Option<Buf> {
        let mut b = Buf::new(n)?;
        b.as_mut().fill(0);
        Some(b)
    }

    pub fn as_mut(&mut self) -> &mut [u8] {
        unsafe { slice::from_raw_parts_mut(self.p, self.n) }
    }

    pub fn as_ref(&self) -> &[u8] {
        unsafe { slice::from_raw_parts(self.p, self.n) }
    }

    pub fn len(&self) -> usize {
        self.n
    }

    /// Adopt a block that some other decoder already `kmalloc`'d (the ICO path
    /// wraps the buffer `png_decode` returns). Caller must not free it too.
    ///
    /// # Safety
    /// `p` must be a live `kmalloc` block of at least `n` bytes, unowned.
    pub unsafe fn from_raw(p: *mut u8, n: usize) -> Buf {
        Buf { p, n }
    }

    /// Hand the block to C (which frees it with `kfree`). Cancels the drop.
    pub fn into_raw(self) -> *mut u8 {
        let p = self.p;
        core::mem::forget(self);
        p
    }
}

impl Drop for Buf {
    fn drop(&mut self) {
        if !self.p.is_null() {
            unsafe { kfree(self.p) };
        }
    }
}

/// A growing C array of `struct img_frame`, for the animated decoders.
///
/// Both allocators this crate links against return 16-byte-aligned payloads
/// (kheap.c `ALIGN16`, mini-libc malloc.c `align16`), so a block cast to
/// `*mut ImgFrame` (align 8) is correctly aligned. Dropping the array frees
/// every frame it still owns, so an error two thirds of the way through an
/// animation does not leak the two thirds already decoded.
pub struct FrameArray {
    p: *mut ImgFrame,
    cap: usize,
    n: usize,
}

impl FrameArray {
    pub fn new(cap: usize) -> Option<FrameArray> {
        if cap == 0 {
            return None;
        }
        let bytes = cap.checked_mul(core::mem::size_of::<ImgFrame>())?;
        let p = unsafe { kmalloc(bytes) } as *mut ImgFrame;
        if p.is_null() {
            return None;
        }
        for i in 0..cap {
            unsafe { p.add(i).write(ImgFrame { delay_ms: 0, rgba: core::ptr::null_mut() }) };
        }
        Some(FrameArray { p, cap, n: 0 })
    }

    pub fn len(&self) -> usize {
        self.n
    }

    pub fn is_full(&self) -> bool {
        self.n >= self.cap
    }

    /// Append a frame, taking ownership of its pixels. False if full.
    pub fn push(&mut self, delay_ms: i32, rgba: Buf) -> bool {
        if self.n >= self.cap {
            return false;
        }
        unsafe { self.p.add(self.n).write(ImgFrame { delay_ms, rgba: rgba.into_raw() }) };
        self.n += 1;
        true
    }

    /// Publish into a C `struct img_anim`; ownership moves to the caller.
    pub fn publish(self, w: i32, h: i32, loops: i32, out: *mut ImgAnim) {
        unsafe {
            (*out).w = w;
            (*out).h = h;
            (*out).nframes = self.n as i32;
            (*out).loops = loops;
            (*out).frames = self.p;
        }
        core::mem::forget(self);
    }
}

impl Drop for FrameArray {
    fn drop(&mut self) {
        unsafe {
            for i in 0..self.n {
                let f = self.p.add(i).read();
                if !f.rgba.is_null() {
                    kfree(f.rgba);
                }
            }
            kfree(self.p as *mut u8);
        }
    }
}

/// A `kmalloc`'d array of u16, addressed by index. Stored as raw bytes and read
/// two at a time rather than transmuted, so no alignment assumption is made
/// about what `kmalloc` returns (the kernel heap and mini-libc's arena are two
/// different allocators and this crate links into both).
pub struct U16Buf {
    b: Buf,
    n: usize,
}

impl U16Buf {
    pub fn zeroed(n: usize) -> Option<U16Buf> {
        Some(U16Buf { b: Buf::zeroed(n.checked_mul(2)?)?, n })
    }
    #[inline]
    pub fn get(&self, i: usize) -> u16 {
        if i >= self.n {
            return 0;
        }
        let d = self.b.as_ref();
        d[i * 2] as u16 | (d[i * 2 + 1] as u16) << 8
    }
    #[inline]
    pub fn set(&mut self, i: usize, v: u16) {
        if i >= self.n {
            return;
        }
        let d = self.b.as_mut();
        d[i * 2] = v as u8;
        d[i * 2 + 1] = (v >> 8) as u8;
    }
    pub fn len(&self) -> usize {
        self.n
    }
}

/// Same, for u32 (the ARGB pixel planes and the per-code symbol offsets).
pub struct U32Buf {
    b: Buf,
    n: usize,
}

impl U32Buf {
    pub fn zeroed(n: usize) -> Option<U32Buf> {
        Some(U32Buf { b: Buf::zeroed(n.checked_mul(4)?)?, n })
    }
    #[inline]
    pub fn get(&self, i: usize) -> u32 {
        if i >= self.n {
            return 0;
        }
        let d = self.b.as_ref();
        d[i * 4] as u32
            | (d[i * 4 + 1] as u32) << 8
            | (d[i * 4 + 2] as u32) << 16
            | (d[i * 4 + 3] as u32) << 24
    }
    #[inline]
    pub fn set(&mut self, i: usize, v: u32) {
        if i >= self.n {
            return;
        }
        let d = self.b.as_mut();
        d[i * 4] = v as u8;
        d[i * 4 + 1] = (v >> 8) as u8;
        d[i * 4 + 2] = (v >> 16) as u8;
        d[i * 4 + 3] = (v >> 24) as u8;
    }
    pub fn len(&self) -> usize {
        self.n
    }
}

// --- little/big-endian readers that never index out of bounds --------------
// Every one takes the whole slice plus an offset and returns Option, so a
// truncated file is a `?` rather than a bounds check somebody forgot.

pub fn le16(d: &[u8], o: usize) -> Option<u32> {
    Some(*d.get(o)? as u32 | (*d.get(o + 1)? as u32) << 8)
}

pub fn le32(d: &[u8], o: usize) -> Option<u32> {
    Some(le16(d, o)? | le16(d, o + 2)? << 16)
}

pub fn be16(d: &[u8], o: usize) -> Option<u32> {
    Some((*d.get(o)? as u32) << 8 | *d.get(o + 1)? as u32)
}

pub fn be32(d: &[u8], o: usize) -> Option<u32> {
    Some(be16(d, o)? << 16 | be16(d, o + 2)?)
}

/// Total RGBA byte count for w*h, or None if it would exceed the ceilings.
pub fn rgba_size(w: i32, h: i32) -> Option<usize> {
    if w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM {
        return None;
    }
    let px = w as usize * h as usize;
    if px > MAX_PIXELS {
        return None;
    }
    Some(px * 4)
}
