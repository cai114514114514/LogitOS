# Aqua OS

A real operating system built from the lowest level up — a from-scratch
x86_64 kernel that boots under GRUB/Multiboot2 — with the long-term goal of a
macOS-style ("Aqua") graphical desktop.

This is not a simulation. It is an actual kernel that boots and runs.

## Status

**Milestones 1–4 + 7 complete.**
- **M1 Boot & Hello:** GRUB → Multiboot2 → 32-bit entry → paging + long mode →
  C `kernel_main`; VGA text (color, scrolling) + COM1 serial console.
- **M2 Interrupts & Input:** 64-bit IDT, CPU exception handlers, 8259 PIC remap,
  100 Hz PIT timer, and a PS/2 keyboard that echoes keystrokes — interactive.
- **M3 Memory management:** Multiboot2 memory-map parsing, a bitmap physical
  frame allocator, and a kernel heap (`kmalloc`/`kfree`).
- **M4 Multitasking:** kernel threads, an assembly context switch, and a
  preemptive round-robin scheduler driven by the timer.
- **M7 Graphics:** a 4-level VMM page-mapper, a Multiboot2 linear framebuffer,
  drawing primitives with alpha blending, and a from-scratch **macOS-style
  desktop** — gradient wallpaper, frosted menu bar, a window with traffic-light
  controls, and a frosted Dock.

Next: **M8** (bitmap font + text, PS/2 mouse cursor, draggable windows, a live
compositor). M5 (filesystem) and M6 (userland) are deferred. See the
[design spec](docs/superpowers/specs/2026-05-30-aqua-os-m1-design.md).

## Build & run

Requires (macOS / Homebrew): `clang`, `nasm`, `lld`, `qemu`, `xorriso`,
`i686-elf-grub`.

```sh
make        # build build/aqua.iso
make run    # boot in QEMU (VGA window + serial on this terminal)
make test   # headless boot smoke test (asserts the kernel reaches 64-bit C)
make debug  # boot frozen with a gdb stub on localhost:1234
make clean
```

## Layout

```
boot/     16/32→64-bit boot path and Multiboot2 header (nasm)
kernel/   kernel_main and core services (C)
drivers/  VGA text + COM1 serial (C)
lib/      freestanding helpers (memset/memcpy/…)
include/  public headers
linker.ld kernel link script (loads at 1 MiB)
grub.cfg  GRUB Multiboot2 menu entry
scripts/  build/test helpers
docs/     design specs
```

## Toolchain notes

The host is Apple Silicon (arm64); the target is x86_64. `clang` cross-compiles
natively, but Apple's linker only emits Mach-O, so linking uses `ld.lld`
(`brew install lld`). QEMU fully emulates x86_64 on the arm64 host.
