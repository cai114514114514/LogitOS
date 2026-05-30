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
M4 multitasking (preemptive scheduler) ✅ · M5 storage (ATA + AquaFS + VFS) ✅ ·
M6 userland (GDT/TSS + ring3 + int 0x80 + ELF loader) ✅ · M7 graphics
(framebuffer + VMM + Aqua desktop) ✅ · M8 window system (font + double-buffer +
PS/2 mouse + draggable windows) ✅ — full roadmap complete.

Key notes:
- `vmm_map_page` does a real 4-level walk; maps the high-MMIO framebuffer and
  user pages. Intermediate table entries carry USER; leaf PTE flags protect
  kernel pages. User images link at 1 GiB (above the identity huge-page region).
- Disk: QEMU `-drive ...,if=ide` primary master; `drivers/ata.c` (PIO) +
  `fs/aquafs.c` + `fs/vfs.c`. Build the image with `tools/mkfs.py` (Makefile
  `$(DISK)` target packs `fsroot/*` + `build/user.elf` as `hello.elf`).
- Userland: `kernel/gdt.c` (TSS rsp0), `boot/enter_user.asm`, syscalls via
  int 0x80 in `kernel/syscall.c`; `user/` builds the ring-3 ELF.
- M8 window system: `tools/genfont.py` rasterizes DejaVu Sans Mono to
  `include/font8x16.h` (committed — building needs no PIL). `kernel/fb.c` has a
  RAM back buffer + `fb_present()`; `kernel/wm.c` is the compositor (cached
  background, z-ordered focusable windows, arrow cursor); `drivers/mouse.c` is
  the PS/2 mouse (IRQ12). The userland SYS_EXIT retargets its iret frame at
  `wm_run` so boot flows into the live desktop.
- `tools/qmp_drag.py` scripts a mouse drag over QEMU QMP (for screenshots/CI of
  interaction); real use is `make run` + your mouse.
- Subsystem integration: each WM window is backed by a real subsystem — Finder
  reads the AquaFS dir (vfs_count/ent_name/ent_size), Console shows captured
  ring-3 output (syscall_console()), Activity Monitor shows PIT uptime + PMM
  memory + three M4 worker-thread counters, Notes receives keyboard via wm_key.
  wm_run runs as the scheduler "main" thread; workers + WM round-robin via
  schedule() (plus timer preemption). Keyboard IRQ -> keyboard_handle -> wm_key.
Each milestone: spec → plan → implement. Specs in `docs/superpowers/specs/`.
