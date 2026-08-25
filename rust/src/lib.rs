//! Logit kernel — Rust modules (`#![no_std]`, target `x86_64-unknown-none`).
//!
//! Hybrid C+Rust: Rust owns the **memory-safety-critical, untrusted-input parsers**
//! (the bug class the security audit kept finding); the C kernel keeps the core,
//! drivers, and scheduler. This crate compiles to a `staticlib` and links with the
//! C objects via ld.lld. C-facing functions are `#[no_mangle] extern "C"`; calls
//! into C are `unsafe`.

#![no_std]

use core::panic::PanicInfo;

mod bmp; // BMP/DIB decoder (also the DIB half of an .ico)
mod ico; // ICO/CUR container -> png or bmp (favicons)
mod imgbuf; // shared FFI types + an owning kmalloc buffer for the decoders
mod inflate; // RFC 1951/1950 DEFLATE/zlib, ported to safe Rust (replaces inflate.c)
mod png; // PNG decoder, ported to safe Rust (replaces png.c); calls inflate
mod vp8; // VP8 bool decoder, quantisers, uncompressed frame header
mod vp8_dec; // VP8 planes, transforms, header state
mod vp8_frame; // VP8 key-frame driver: macroblock loop, loop filter, YUV->RGBA
#[cfg(feature = "vp8-interframe")]
mod vp8_inter; // VP8 INTER-frame driver (video): gated OFF the kernel build --
               // see the module doc comment for why, and `nm` on RUST_LIB for
               // the proof this cfg keeps every symbol in it out of ring 0.
mod vp8_rec; // VP8 intra prediction, coefficient tokens, filter kernels
mod vp8_tables; // VP8 probability tables, generated from RFC 6386
mod webp; // WebP: RIFF container, VP8L lossless, VP8 lossy key frames, ALPH

// FFI discipline: this staticlib links into BOTH the C kernel AND the ring-3
// browser (which has its own image pipeline and its own address space), so the
// crate only ever calls symbols that exist in BOTH domains -- kmalloc/kfree
// (real in the kernel; shimmed onto malloc/free by browser_rt.c) and img_register.
// It must NOT touch kernel-only symbols (e.g. serial_puts), or the browser link
// breaks. Every parser is written to never panic (errors return -1), so this
// handler is a never-firing lang-item: just halt.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // Never-firing lang item (every parser returns -1 on error). If it ever DID
    // fire, spin with a pause hint instead of burning the core at full tilt;
    // `hlt` is not an option -- this staticlib also links into the ring-3 browser.
    loop {
        core::hint::spin_loop();
    }
}