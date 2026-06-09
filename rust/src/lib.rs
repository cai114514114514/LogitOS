//! Aether kernel — Rust modules (`#![no_std]`, target `x86_64-unknown-none`).
//!
//! Hybrid C+Rust strategy: Rust owns the **memory-safety-critical, untrusted-input
//! parsers** (where the security audit found whole bug classes); the C kernel keeps
//! the core, drivers, and scheduler (the FFI boundary cost is only worth paying for
//! modules with a narrow `(ptr,len) -> result` interface and bounds-heavy internals).
//!
//! This crate compiles to a `staticlib` and links with the C objects via ld.lld.
//! Functions exposed to C are `#[no_mangle] extern "C"`; calls into C are `unsafe`.

#![no_std]

use core::panic::PanicInfo;

mod inflate;   /* RFC1951/1950 inflate, ported to safe Rust -- replaces inflate.c */

extern "C" {
    fn serial_puts(s: *const u8); /* C: void serial_puts(const char *) */
}

/// Boot self-test: proves the C->Rust call, the Rust->C call, and the no_std link
/// all work. Called once from kernel_main.
#[no_mangle]
pub extern "C" fn rust_selftest() {
    unsafe {
        serial_puts(b"[rust] hello -- no_std rust linked into the C kernel (x86_64-unknown-none)\n\0".as_ptr());
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe { serial_puts(b"[rust] PANIC\n\0".as_ptr()); }
    loop {}
}
