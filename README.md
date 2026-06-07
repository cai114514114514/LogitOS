# Aqua OS

A real operating system built from the lowest level up — a from-scratch
x86_64 kernel that boots under GRUB/Multiboot2 — with the long-term goal of a
macOS-style ("Aqua") graphical desktop.

This is not a simulation. It is an actual kernel that boots and runs.

## Status

**Milestones 1–7 complete.**
- **M1 Boot & Hello:** GRUB → Multiboot2 → 32-bit entry → paging + long mode →
  C `kernel_main`; VGA text (color, scrolling) + COM1 serial console.
- **M2 Interrupts & Input:** 64-bit IDT, CPU exception handlers, 8259 PIC remap,
  100 Hz PIT timer, and a PS/2 keyboard that echoes keystrokes — interactive.
- **M3 Memory management:** Multiboot2 memory-map parsing, a bitmap physical
  frame allocator, and a kernel heap (`kmalloc`/`kfree`).
- **M4 Multitasking:** kernel threads, an assembly context switch, and a
  preemptive round-robin scheduler driven by the timer.
- **M5 Storage & filesystem:** a polled ATA PIO disk driver, the custom AquaFS
  on-disk filesystem, a VFS layer, and a host `mkfs.py` that builds the disk.
- **M6 Userland:** a GDT + TSS, ring 3, `int 0x80` system calls, an ELF64
  loader, and user paging — runs an unprivileged program loaded off the disk.
- **M7 Graphics:** a 4-level VMM page-mapper, a Multiboot2 linear framebuffer,
  drawing primitives with alpha blending, and a from-scratch **macOS-style
  desktop** — gradient wallpaper, frosted menu bar, a window with traffic-light
  controls, and a frosted Dock.
- **M8 Window system:** an 8×16 bitmap font + text, a double-buffered
  compositor, a PS/2 mouse with an arrow cursor, and **focusable, draggable
  windows** — a genuinely interactive desktop.

The full roadmap (M1–M8) is complete. On top of it sits a **real application
platform**: programs are `.aex` executables on disk, each launched as a genuine
**ring-3 process** scheduled by M4, drawing its window through GUI system calls.

- **AEX format** — a native executable (header + ELF) built by `tools/mkaex.py`.
- **Apps** (in `user/`): **Clock** (live RTC time), **TextEdit** (opens `.txt`),
  **Terminal** (a shell: `ls`/`cat`/`mem`/`ps`/…), **Activity Monitor** (the
  real process list).
- **Open/close** — the Dock launches apps; the Finder opens a file with the app
  registered for its extension (click `readme.txt` → TextEdit); the red title-bar
  button closes an app and its process is reaped.
- **Wall clock** — the menu bar and Clock read the real date/time from the CMOS
  RTC.

See the [design spec](docs/superpowers/specs/2026-05-30-aqua-os-m1-design.md).
Run `make run`: drag windows, click Dock icons, open a `.txt`, type in the Terminal.

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
boot/     boot path, Multiboot2 header, ISR stubs, context switch, ring-3 entry (nasm)
kernel/   kernel_main + core services: idt, gdt, pmm, vmm, kheap, sched, fb,
          wm (window manager + GUI syscalls), elf, aex (loader), syscall
drivers/  VGA text, COM1 serial, PIC, PIT, PS/2 keyboard + mouse, ATA disk, RTC
fs/        VFS layer + AquaFS filesystem
lib/      freestanding helpers (memset/memcpy/…)
user/      ring-3 apps (clock, textedit, terminal, monitor) + aqua.h + crt0
tools/     mkfs.py (disk image), mkaex.py (executables), genfont.py (font)
fsroot/    files packed into the disk image
include/  public headers (incl. aqua_abi.h — the app/syscall ABI)
linker.ld kernel link script (loads at 1 MiB)
grub.cfg  GRUB Multiboot2 menu entry
scripts/  build/test helpers
docs/     design specs
```

## Toolchain notes

The host is Apple Silicon (arm64); the target is x86_64. `clang` cross-compiles
natively, but Apple's linker only emits Mach-O, so linking uses `ld.lld`
(`brew install lld`). QEMU fully emulates x86_64 on the arm64 host.

## Aqua Script

we use 2300 lines to build the script