//! Logit kernel — Rust modules (`#![no_std]`, target `x86_64-unknown-none`).
//!
//! Hybrid C+Rust: Rust owns the **memory-safety-critical, untrusted-input parsers**
//! (the bug class the security audit kept finding); the C kernel keeps the core,
//! drivers, and scheduler. This crate compiles to a `staticlib` and links with the
//! C objects via ld.lld. C-facing functions are `#[no_mangle] extern "C"`; calls
//! into C are `unsafe`.

#![no_std]

use core::panic::PanicInfo;

mod inflate; // RFC 1951/1950 DEFLATE/zlib, ported to safe Rust (replaces inflate.c)
mod png; // PNG decoder, ported to safe Rust (replaces png.c); calls inflate

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