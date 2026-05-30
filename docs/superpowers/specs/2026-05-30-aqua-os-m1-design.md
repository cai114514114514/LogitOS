# Aqua OS — Design Spec

**Date:** 2026-05-30
**Status:** M1–M8 implemented & verified — the full roadmap is complete

## Vision

Build a *real* operating system from the lowest level up, culminating in a
macOS-style ("Aqua") graphical desktop. Not a simulation — an actual kernel
that boots on bare-metal-equivalent hardware (QEMU full emulation).

## Locked technology decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| System form | Real bootable OS kernel | Most literal reading of "从底层开始" |
| Kernel language | C (+ asm for boot) | Classic OSDev path, richest references, close to real kernels |
| CPU architecture | x86_64 | Best-documented, best QEMU support, well-trodden boot path |
| Boot method | GRUB + Multiboot2 | Skips 16-bit real-mode; focus effort on the kernel itself |
| Host toolchain | clang `--target=x86_64-elf` + `ld.lld` + `nasm` | clang cross-compiles natively (host is Apple Silicon); `ld.lld` links ELF (Apple `ld` is Mach-O only) |
| Emulator | qemu-system-x86_64 | Full emulation of x86_64 on the arm64 host |
| ISO tooling | `i686-elf-grub-mkrescue` + `xorriso` | Produce a BIOS-bootable Multiboot2 ISO |

## Roadmap (each milestone is its own spec → plan → implement cycle)

| # | Milestone | Deliverable |
|---|-----------|-------------|
| **M1** | Boot & Hello | Bootable 64-bit kernel, VGA text + serial, build + test harness ✅ |
| **M2** | Interrupts & input | IDT, ISR/IRQ stubs, PIC remap, exceptions, PIT timer, PS/2 keyboard ✅ |
| **M3** | Memory management | Multiboot2 map parse, bitmap frame allocator, kernel heap ✅ |
| **M4** | Multitasking | Kernel threads, asm context switch, preemptive round-robin scheduler ✅ |
| **M5** | Storage & FS | ATA PIO driver, AquaFS (custom on-disk FS), VFS layer, host mkfs tool ✅ |
| **M6** | Userland | GDT/TSS, ring 3, int 0x80 syscalls, ELF64 loader, user paging ✅ |
| **M7** | Graphics | Multiboot2 linear framebuffer, VMM page-mapper, drawing primitives, Aqua desktop scene ✅ |
| **M8** | Window system | Bitmap font + text, double-buffered compositor, PS/2 mouse + cursor, draggable/focusable windows ✅ |

## Milestone 1 — "Boot & Hello"

### Goal
`make run` → GRUB boots our Multiboot2 ELF64 kernel → CPU enters 64-bit long
mode → `kernel_main()` runs in C → a banner appears on both the VGA text screen
and the serial console; the kernel then halts.

### Boot flow
```
GRUB --multiboot2--> boot.asm [bits 32]
                       verify magic; set up stack
                       verify CPUID + long-mode support
                       build identity page tables (first 1 GiB, 2 MiB pages)
                       enable PAE -> set EFER.LME -> enable paging
                       load 64-bit GDT; far jump
                     long.asm [bits 64] --> kernel_main() (C)
                                              |-> vga.c     (0xB8000, 80x25)
                                              |-> serial.c  (COM1)
                                              `-> kprintf.c (fans out to both)
```

### Components
| File | Responsibility |
|------|----------------|
| `boot/multiboot2.asm` | Multiboot2 header (GRUB discovery) |
| `boot/boot.asm` | 32-bit entry: checks → page tables → long mode → far jump |
| `boot/long.asm` | 64-bit entry: load segments, call `kernel_main` |
| `kernel/kmain.c` | `kernel_main()` — init + banner |
| `kernel/kprintf.c` | minimal printf (`%s %c %d %u %x %p %%`) → VGA + serial |
| `drivers/vga.c` | VGA text: clear / putc / puts / color / scroll |
| `drivers/serial.c` | COM1 init + output (debug + test transport) |
| `lib/string.c` | `memset`/`memcpy`/`memmove`/`memcmp` (freestanding) |
| `linker.ld` | header first, load at 1 MiB, section layout |
| `grub.cfg` | `multiboot2 /boot/kernel.elf` |
| `Makefile` | compile / assemble / link / ISO / run / debug / test |
| `scripts/run-test.sh` | headless boot smoke test |

### Error handling
Pre-paging failures (bad multiboot magic, no CPUID, no long mode) print
`ERR: <code>` directly to VGA memory and halt — there is no interrupt or
console infrastructure yet, so this is the appropriate mechanism.

### Test strategy (reused by all later milestones)
`make test` boots headless (`-serial file:… -display none`), and a portable
shell harness polls the serial log for the `AQUA_BOOT_OK` marker the kernel
prints once it reaches 64-bit C. Found → pass; timeout → fail (dumps serial).

### Success criteria — all met
1. `make` produces `kernel.elf` + `aqua.iso` with no errors. ✅
2. `make run` shows the banner on VGA and serial. ✅
3. `make test` passes automatically. ✅

## Milestone 2 — "Interrupts & Input"

### Goal
Turn the static kernel into an interactive one: install a 64-bit IDT, handle
CPU exceptions, remap the 8259 PIC, drive a periodic timer (PIT), and echo
keystrokes from a PS/2 keyboard.

### Flow
```
boot/isr.asm   48 stubs (vec 0-31 exceptions, 32-47 IRQs); uniform error-code
               + vector push -> save GP regs -> interrupt_handler() -> iretq
kernel/idt.c   build/load IDT (gates -> stub_table, selector 0x08, type 0x8E)
drivers/pic.c  remap IRQs to 32-47; unmask IRQ0/IRQ1; EOI
drivers/pit.c  channel-0 square wave @ 100 Hz -> IRQ0 -> tick counter
drivers/keyboard.c  IRQ1 -> read 0x60 -> scancode set 1 -> echo via kprintf
```

### Components added
| File | Responsibility |
|------|----------------|
| `boot/isr.asm` | per-vector stubs + common save/restore/dispatch path |
| `kernel/idt.c` | IDT entries + `lidt` |
| `kernel/interrupts.c` | C dispatcher: exception panic vs IRQ routing + EOI |
| `drivers/pic.c` | 8259 remap + end-of-interrupt |
| `drivers/pit.c` | timer programming + tick count |
| `drivers/keyboard.c` | scancode → ASCII echo |
| `include/{interrupts,idt,pic,pit,keyboard}.h` | interfaces |

### Notes
- Reuses the boot GDT's 64-bit code selector (0x08); no new GDT/TSS needed yet.
- Stack stays 16-byte aligned for the C call because the CPU 16-aligns RSP on
  interrupt entry; the stub's pushes preserve the ABI requirement.
- Exceptions print `*** EXCEPTION: <name> ***` (white-on-red) with rip/error
  and halt — adequate until we have richer fault handling.

### Success criteria — all met
1. Clean build with the new sources. ✅
2. `make test` passes only after IRQ0 has ticked ≥10× (proves the interrupt
   path works end to end). ✅
3. Injected keystrokes echo on screen ("hello aqua os"). ✅

## Milestone 3 — "Memory management"

### Goal
Discover physical RAM and provide dynamic allocation: parse the Multiboot2
memory map, run a physical frame allocator, and offer a kernel heap.

### Design
- **Multiboot2 plumbing:** `boot.asm` stashes GRUB's info pointer (`ebx`) in
  `edi`; `long.asm` zero-extends it into `rdi` so `kernel_main(uint64_t)`
  receives it as its first argument.
- **PMM (`kernel/pmm.c`):** bitmap allocator, 1 bit / 4 KiB frame. Bitmap lives
  just past `_kernel_end` (a linker symbol). Init marks all frames used, frees
  firmware-`available` regions, then re-reserves low memory + kernel + bitmap +
  the info block. Provides `pmm_alloc`, `pmm_free`, `pmm_alloc_contig`.
- **Heap (`kernel/kheap.c`):** bump-allocate within a contiguous arena obtained
  from `pmm_alloc_contig` (identity-mapped, so phys == virt), plus a first-fit
  free list of returned blocks. `kmalloc`/`kfree`. Grows by grabbing new arenas.

### Notes
- All QEMU RAM is below the identity-mapped first 1 GiB, so a physical frame
  address doubles as its virtual address — no new page mappings needed yet. A
  real 4 KiB-granular VMM mapper is deferred to when non-identity mappings are
  required (higher-half kernel / MMIO / user address spaces).

### Success criteria — all met
1. Memory map parsed; reports 127 MiB usable on a 128 MiB guest. ✅
2. Frame allocator returns distinct, page-aligned frames. ✅
3. Heap round-trips data and recycles freed blocks (`kmalloc==freed ptr`). ✅
4. `make test` passes (marker gated on both self-tests). ✅

## Milestone 4 — "Multitasking"

### Goal
Run multiple kernel threads concurrently on one core, preempted by the timer.

### Design
- **`boot/switch.asm` — `context_switch(uint64_t *old_rsp, uint64_t new_rsp)`:**
  pushes callee-saved registers + RFLAGS, stores RSP into the outgoing thread,
  loads the incoming RSP, restores, and `ret`s.
- **`kernel/sched.c`:** a circular ready ring of `struct thread { rsp; next;
  stack; … }`. `sched_init` adopts the boot context as `main`. `thread_create`
  allocates a 16 KiB stack and hand-builds an initial frame
  (`[rflags=0x202][r15..rbp=0][entry]`) so the first switch "returns" into the
  thread with interrupts enabled. `schedule()` advances to `current->next`.
- **Preemption:** the IRQ0 handler sends EOI then calls `schedule()`. Because
  the CPU clears IF on entry through an interrupt gate and `iretq` restores it,
  and new threads start with IF=1, time-slicing is safe and re-entrant.

### Notes
- A switch only saves callee-saved registers; the caller-saved set and the
  interrupted thread's full GP state are preserved by `isr_common` on the
  thread's own stack and restored at `iretq`.
- Stacks come from `kmalloc` (identity-mapped), 16 KiB each.

### Success criteria — all met
1. Three worker threads that never yield all make forward progress purely via
   timer preemption. ✅
2. Counters advance fairly (within ~2% of each other across samples). ✅
3. `make test` marker gated on all three counters being non-zero. ✅

## Milestone 7 — "Graphics" (jumped here from M4 by request)

### Goal
Leave VGA text mode for a true linear framebuffer and render a macOS-style
("Aqua") desktop from scratch.

### Design
- **Framebuffer request:** `boot/multiboot2.asm` adds a type-5 framebuffer tag
  asking GRUB for 1024×768×32. GRUB sets the VBE mode and returns a type-8 info
  tag with the framebuffer address, pitch, and RGB field positions.
- **VMM (`kernel/vmm.c`):** the framebuffer is high MMIO (≈0xFD000000), outside
  the identity-mapped first 1 GiB, so a real 4-level page-table walker
  (`vmm_map_page`/`vmm_map_range`) maps it, allocating intermediate tables from
  the PMM. Mapped write-through + cache-disabled (`PWT|PCD`).
- **Drawing (`kernel/fb.c`):** `fb_init` parses the tag and maps the surface.
  Primitives: `fb_put`, `fb_fill_rect`, `fb_fill_circle`, `fb_round_rect`, and
  alpha-blended `fb_blend_rect`/`fb_blend_round_rect` for frosted-glass effects.
  Colors are packed via the firmware-reported field positions.
- **Scene (`kernel/desktop.c`):** gradient wallpaper, frosted menu bar (logo +
  battery + status), a window (drop shadow, rounded title bar, red/yellow/green
  traffic lights, Finder sidebar, skeleton content), and a frosted Dock with
  seven glossy rounded-square icons.

### Notes
- Static, single-buffered draw (one pass). Double buffering + a real compositor
  come with M8.
- VGA text driver stays compiled; `kprintf` still logs to serial (the test
  channel) — its writes to 0xB8000 are simply not displayed in graphics mode.

### Success criteria — all met
1. GRUB supplies a 32-bpp linear framebuffer; `fb_init` maps it with no fault. ✅
2. `make test` marker prints after the desktop is composited. ✅
3. Screenshot shows a recognizable macOS-style desktop. ✅

## Milestone 5 — "Storage & filesystem"

### Goal
Read files from a real (emulated) disk through a layered storage stack.

### Design
- **`drivers/ata.c`:** polled ATA PIO (primary bus, master, LBA28). `ata_read`
  selects the drive, issues READ SECTORS, and PIO-reads each sector.
- **AquaFS (`fs/aquafs.c`):** a tiny custom on-disk format — superblock (magic
  "AQUA", version, file count), a 16-entry directory (sectors 1–2), then file
  data laid out contiguously from sector 3. Read-only in the kernel.
- **VFS (`fs/vfs.c`):** a one-backend abstraction (`mount/list/size/read`) so
  callers (the ELF loader, demos) don't bind to AquaFS directly.
- **`tools/mkfs.py`:** host tool that packs a set of files into a disk image,
  invoked by the Makefile; QEMU attaches it as the primary IDE disk.

### Success criteria — all met
1. Kernel mounts the volume and lists files with sizes and LBAs. ✅
2. `readme.txt` reads back byte-for-byte and its prefix is verified. ✅

## Milestone 6 — "Userland"

### Goal
Run an unprivileged (ring 3) program loaded from disk, talking to the kernel
only through system calls.

### Design
- **GDT + TSS (`kernel/gdt.c`, `boot/gdt_flush.asm`):** a real GDT with kernel
  code/data, user code/data (DPL 3), and a 64-bit TSS whose `rsp0` is the
  kernel stack the CPU switches to on a ring 3 → ring 0 trap.
- **User paging:** `vmm` gained `VMM_USER`; intermediate table entries are made
  user-reachable (leaf PTE flags still protect kernel pages). User images link
  at 1 GiB — above the identity-mapped huge-page region — so 4 KiB user pages
  map cleanly.
- **Syscalls:** IDT vector 0x80 is a DPL-3 gate (`boot/isr.asm` stub →
  `kernel/syscall.c`). ABI: `rax`=number, `rdi/rsi/rdx`=args. `SYS_WRITE`
  (1) and `SYS_EXIT` (2).
- **ELF loader (`kernel/elf.c`):** parses an ELF64 image, maps each PT_LOAD as
  user pages (zeroed for .bss), copies file bytes, returns the entry point.
- **Entry (`boot/enter_user.asm`):** builds a ring-3 iret frame and `iretq`s in.
- **User program (`user/`):** `ustart.asm` + `user.c`, linked with `user.ld`
  at 0x40000000, stored on the AquaFS disk as `hello.elf`.

### Flow
disk → `vfs_read("hello.elf")` → `elf_load` → map user stack → `enter_user` →
ring 3 runs → `int 0x80` SYS_WRITE (kernel prints the message) → SYS_EXIT.

### Success criteria — all met
1. ELF loads from disk and runs in ring 3 (CPL 3). ✅
2. Its `int 0x80` SYS_WRITE output appears; SYS_EXIT reports code 0. ✅
3. `make test` marker gated on `fs_ok && user wrote via syscall`. ✅

## Milestone 8 — "Window system" (interactive desktop)

### Goal
Make the desktop live: real text, a moving cursor, and windows you can focus
and drag.

### Design
- **Font (`tools/genfont.py` → `include/font8x16.h`):** DejaVu Sans Mono
  (permissive) rasterized to an 8×16 1-bpp bitmap font on the host; the header
  is committed so building needs no Python. `fb_char`/`fb_text` render it.
- **Double buffering (`kernel/fb.c`):** drawing targets a RAM back buffer;
  `fb_present()` blits it to the visible framebuffer — no flicker, clean redraw.
- **PS/2 mouse (`drivers/mouse.c`):** enables the aux device + IRQ12, parses
  3-byte packets (with a resync filter that drops stray ACK/overflow bytes —
  the bug that first broke dragging), tracks an absolute clamped cursor.
- **Compositor / WM (`kernel/wm.c`):** caches the static background (wallpaper +
  menu-bar text + Dock) once, then each frame restores it, draws the clock,
  draws windows in z-order (focused on top, colored traffic lights; others
  greyed), and the arrow cursor, then presents. Mouse events raise/focus the
  window under the cursor and drag it by its title bar.
- **Handoff:** the userland `SYS_EXIT` retargets its trap frame at `wm_run`, so
  the system flows boot → userland demo → live desktop without halting.

### Success criteria — all met
1. Menu-bar text + a ticking clock render from the bitmap font. ✅
2. Cursor tracks injected PS/2 motion; redraw is flicker-free. ✅
3. Clicking a window raises/focuses it; dragging its title bar moves it
   (verified by scripting a drag over QMP — Finder raised and relocated). ✅
4. `make test` still passes end to end. ✅

## Result

All eight milestones are implemented and verified in QEMU: a from-scratch
x86_64 OS that boots via GRUB/Multiboot2, handles interrupts, manages physical
and virtual memory, preemptively multitasks, reads a real disk through its own
filesystem, runs unprivileged ELF programs in ring 3 via system calls, and
presents an interactive macOS-style ("Aqua") graphical desktop — no third-party
code, every layer written here.
