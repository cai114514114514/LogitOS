//! Aether kernel — Rust modules (`#![no_std]`, target `x86_64-unknown-none`).
//!
//! Hybrid C+Rust: Rust owns the **memory-safety-critical, untrusted-input parsers**
//! (the bug class the security audit kept finding); the C kernel keeps the core,
//! drivers, and scheduler. This crate compiles to a `staticlib` and links with the
//! C objects via ld.lld. C-facing functions are `#[no_mangle] extern "C"`; calls
//! into C are `unsafe`.

#![no_std]

use core::panic::PanicInfo;

mod inflate; // RFC 1951/1950 DEFLATE/zlib, ported to safe Rust (replaces inflate.c)

// The crate is deliberately FREE of any kernel FFI so the same staticlib links
// into BOTH the C kernel AND the ring-3 browser (which has its own image pipeline
// and cannot reach kernel symbols). inflate is written to never panic (every error
// path returns -1), so this handler is a never-firing lang-item: just halt.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
