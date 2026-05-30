# Aqua OS — notes for Claude

A from-scratch x86_64 OS kernel (C + nasm), booted via GRUB/Multiboot2, aiming
toward a macOS-style desktop. Real kernel, not a simulation.

## Build / run / test

```sh
make        # -> build/aqua.iso
make run    # QEMU: VGA window + serial on terminal
make test   # headless; asserts kernel prints AQUA_BOOT_OK on serial
make debug  # QEMU frozen with gdb stub on :1234
```

## Toolchain (macOS / Apple Silicon host, x86_64 target)

- Compile: `clang --target=x86_64-elf -ffreestanding` (clang cross-compiles natively)
- Link: **`ld.lld`** — Apple `ld` only emits Mach-O, so the LLVM linker is required (`brew install lld`)
- Assemble: `nasm -f elf64` (32-bit boot code lives in elf64 objects via `bits 32`)
- ISO: `i686-elf-grub-mkrescue` + `xorriso`
- Run: `qemu-system-x86_64` (full emulation on arm64)

## Conventions

- Kernel loads at **1 MiB**; `linker.ld` forces the Multiboot2 header first.
- Boot path: `boot/boot.asm` (32-bit: checks → identity page tables → long mode) → `boot/long.asm` (64-bit) → `kernel_main`.
- `kprintf` fans output to **both** VGA and serial; serial is also the test channel.
- IDE diagnostics about inline-asm constraints / missing headers are false
  positives unless `.clangd` is being honored — the real build uses `-Iinclude`
  and the x86_64 target.

## Roadmap

M1 Boot & Hello ✅ · M2 interrupts + keyboard ✅ · M3 memory (PMM + heap) ✅ ·
M4 multitasking (preemptive scheduler) ✅ · M7 graphics (framebuffer + VMM +
Aqua desktop) ✅ · M5 fs (deferred) · M6 userland (deferred) ·
M8 window system + live desktop.

Graphics notes: `vmm_map_page` does a real 4-level walk and maps the high-MMIO
framebuffer (GRUB sets 1024x768x32 via the multiboot2 framebuffer tag). Drawing
primitives + alpha blending in `kernel/fb.c`; the scene is in `kernel/desktop.c`.
Next up M8: a bitmap font + text rendering, PS/2 mouse + cursor, draggable
windows, double-buffered compositor.
Each milestone: spec → plan → implement. Specs in `docs/superpowers/specs/`.
