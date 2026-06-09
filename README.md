# Aether OS

A real operating system built from the lowest level up — a from-scratch
x86_64 kernel that boots under GRUB/Multiboot2 into a macOS-style graphical
desktop. Not a simulation, not a Linux distribution: an actual kernel, its own
drivers, its own filesystem, its own network stack and crypto, its own window
system and applications — and its own programming language.

It boots to a frosted-glass desktop, opens **real HTTPS websites** (Wikipedia
included) through a from-scratch TLS 1.3 stack, runs a POSIX-ish shell with
`fork`/`exec`/pipes, and ships a language of its own — **AetherScript** — with a
standard library and a small IDE. The long-term north star is for Aether to
become a genuine *AI system* that owns its inference end to end; the strategy is
to build a solid, real OS first and reach the AI capstone from there.

~23,000 lines of from-scratch C and assembly across 219 commits, plus three
ported engines (QuickJS, NetSurf LibCSS, musl libm) used only where re-deriving
them adds nothing.

## What it can do

- **Boots to a macOS-style desktop** — gradient wallpaper, a frosted menu bar and
  Dock, draggable/focusable windows with traffic-light controls, a compositor with
  alpha blending, a PS/2 mouse cursor, and a real wall clock from the CMOS RTC.
- **Browses the real web** — `http://` and **`https://`** pages over a from-scratch
  TCP → HTTP(S) → TLS 1.3 stack with strict X.509 verification against a **130-root**
  trust store, an HTML/DOM + CSS-cascade + layout + paint pipeline, decoded PNG/GIF
  images, and inline `<script>` execution. Renders `en.wikipedia.org` and
  `zh.wikipedia.org`.
- **Anti-aliased Unicode text** — a from-scratch TrueType parser + integer-only AA
  rasterizer (the kernel is `-mno-sse` in places) with a glyph cache, font fallback,
  and CJK support; the whole UI is anti-aliased.
- **Runs real processes** — a POSIX-ish model (`fork`/`execve`/`waitpid`, file
  descriptors, pipes, a TTY) with `/bin/sh` (pipes, redirects, job control) and
  coreutils (`ls cat echo wc head mkdir rm …`). The GUI Terminal is a real terminal
  emulator over the same shell.
- **Its own language** — **AetherScript** (`/bin/as`, `.as`): a from-scratch
  Python-flavored language (indentation blocks, closures, classes, exceptions, a
  mark-sweep GC) compiled to bytecode and run on a stack VM, with raw memory + direct
  syscall access for systems programming. Ships with the **LibAether** standard
  library and **Code Studio**, a syntax-highlighting IDE that edits, runs, and
  jumps-to-error on `.as` files.
- **Runs JavaScript** — a ported QuickJS as a ring-3 app, on a from-scratch
  freestanding mini-libc + musl libm; the browser runs page scripts with DOM bindings.
- **Multi-core** — an SMP scheduler: AP bring-up (ACPI + LAPIC + trampoline), a
  big-kernel-lock model with ring-3 parallelism, and parallel framebuffer present.
- **Modern devices** — a virtio 1.0 PCI stack (virtio-blk disk, virtio-gpu display
  with DMA present), plus legacy ATA / e1000 / VGA fallbacks, and an NVMe driver.

## Status — the road so far

The original M1–M8 roadmap (boot → desktop) is complete, and the project has gone
far beyond it.

**Foundation & desktop (M1–M8):** boot + long mode + serial/VGA · interrupts +
PS/2 keyboard · PMM + kernel heap · preemptive scheduler · ATA + **AetherFS** +
VFS · GDT/TSS + ring 3 + `int 0x80` + ELF loader · framebuffer VMM + macOS-style
desktop · font + double-buffered compositor + mouse + draggable windows.

**The browser arc (M9–M14):** PCI + e1000 + ARP/IPv4/ICMP/UDP/DNS · **TCP** ·
**HTTP** + Browser app · **TLS 1.3** (from-scratch SHA/HMAC/HKDF, ChaCha20-Poly1305,
AES-GCM, X25519, P-256/384 ECDSA, **RSA** + a 130-root CA bundle) · **HTML/CSS**
layout → paint with images · **Unicode + from-scratch anti-aliased TrueType** text.

**Becoming a real platform (M15–M19):** SSE2/FPU at boot · **JavaScript** (QuickJS)
as a ring-3 app on a from-scratch mini-libc + libm · **LibCSS** + the whole render
pipeline moved into the browser app, with JS↔DOM bindings · **real processes**
(`fork`/`exec`/pipes/shell/coreutils) · **virtio** (modern paravirtual disk + GPU).

**Its own language (M20–M22):** **AetherScript** — lexer (INDENT/DEDENT) + single-pass
compiler + stack VM; strings/lists/dicts, `for`/`range`, modules + `import`, closures,
classes (inheritance + `super`), exceptions (`raise`/`try`/`except`), a mark-sweep GC,
and raw-memory + syscall indirection. Plus the LibAether stdlib and the Code Studio IDE.

**Scaling & hardening (M25–M26 + ongoing):** a preemptive **SMP** scheduler · TCP
out-of-order **reassembly** (large TLS handshake flights arrive reliably) · NVMe ·
the **Aqua → Aether** rename · a system-wide security/bug-hunt pass.

**Post-roadmap:** per-process address spaces (each app its own PML4, CR3 switched on
schedule) and ring-3 fault containment — an app fault kills only that app; the
kernel and desktop survive.

Each milestone follows **spec → plan → implement**; specs live in
`docs/superpowers/specs/`.

## Build & run

Host: macOS / Apple Silicon (arm64). Target: x86_64 (clang cross-compiles natively).
Requires (Homebrew): `clang`, `nasm`, `lld`, `qemu`, `xorriso`, `i686-elf-grub`.

```sh
make                 # build build/aether.iso
make run             # boot in QEMU (-smp 4, virtio GPU/disk + e1000); desktop window + serial here
make build/disk.img  # rebuild the disk image after changing apps / fonts / fsroot
make debug           # boot frozen with a gdb stub on localhost:1234
make clean
```

Tests (headless, asserted over serial unless noted):

```sh
make test            # boot smoke test (asserts the kernel prints AETHER_BOOT_OK)
make test-shell      # fork/exec + pipes + coreutils via /bin/sh
make test-as         # AetherScript language core (host unit tests, no QEMU)
make test-as-os      # AetherScript on Aether: runs the examples incl. the LibAether stdlib
make test-tcp-host   # TCP reassembly unit tests (host)
make test-smp        # boots -smp 4 and asserts genuine cross-core parallelism
make test-nvme       # NVMe driver
```

Boot the desktop with `make run`: drag windows, launch from the Dock, open a `.as`
file in Code Studio, or open the Browser and load `https://en.wikipedia.org`.

## Source layout

All source lives under `c/`, with headers colocated next to their `.c` (header
names are unique, so every `#include "foo.h"` resolves via the Makefile's `INCDIRS`).
`include/` holds only the cross-cutting kernel↔user ABI.

```text
c/boot/                                       Multiboot2 + long-mode entry, ISR stubs, ring-3 entry (nasm)
c/kernel/{core,cpu,mm,sched,exec,gui,pci}/    kernel by subsystem (incl. wm = window manager + GUI syscalls, SMP)
c/drivers/{char,timer,block,net,virtio}/      device drivers (PS/2, PIT/RTC, ATA/NVMe/virtio-blk, e1000, virtio-gpu)
c/fs/                                          VFS + AetherFS (hierarchical read-write inode FS)
c/net/{link,ip,transport,core,dns,http,tls}/  network stack: eth/arp/ip/icmp/udp/dns/tcp/http + TLS 1.3 + x509
c/crypto/{hash,aead,pubkey,trust}/            from-scratch crypto (SHA, HMAC/HKDF, ChaCha20-Poly1305, AES-GCM, X25519, ECDSA, RSA, roots)
c/lib/{image,text}/                            shared libs (inflate/png/gif, utf8, TrueType raster)
c/apps/                                        ring-3 apps + shared aether.h / clib.h / crt0
c/apps/gui/                                    windowed apps: Finder, Terminal, TextEdit, Clock, Monitor, Code Studio + the aui toolkit
c/apps/coreutils/                              /bin/sh + coreutils
c/apps/as/                                     AetherScript: /bin/as (lexer, compiler, VM)
c/apps/browser/                                browser + render engine (dom, layout, css_engine, paint, js_dom) + QuickJS
c/apps/libc/                                   from-scratch freestanding mini-libc
third_party/                                     ported, not from-scratch: QuickJS, NetSurf LibCSS, musl libm
include/abi/aether_abi.h                         the app/syscall ABI (kernel ↔ userland)
tools/                                           mkfs.py, mkaex.py, mkfont.py, genroots.py, QMP screenshot/CI drivers
fsroot/                                          files packed into the disk image (incl. /usr/as LibAether stdlib + examples)
docs/superpowers/specs/                          design specs (spec → plan → implement)
```

## Toolchain notes

- **Compile:** `clang --target=x86_64-elf -ffreestanding` (clang cross-compiles natively).
- **Link:** `ld.lld` — Apple's `ld` only emits Mach-O, so the LLVM linker is required.
- **Assemble:** `nasm -f elf64`. **ISO:** `i686-elf-grub-mkrescue` + `xorriso`.
- **Run:** `qemu-system-x86_64` (full x86_64 emulation on the arm64 host).
- The kernel loads at 1 MiB; `linker.ld` forces the Multiboot2 header first. SSE is
  enabled at boot, so kernel and userland use hardware floating point.

## AetherScript

A from-scratch language — not a port — in `c/apps/as/` (~3,400 lines of C +
assembly): a clox-lineage single-pass compiler that emits flat bytecode for a
computed-goto stack VM. Python-ish (indentation blocks, dynamic types), with `def`,
`if`/`elif`/`else`, `while`, `for … in range()`/lists, lists + dicts + strings,
closures and lambdas, classes (single inheritance + `super`), exceptions
(`raise`/`try`/`except`), and a mark-sweep GC.

What sets it apart from a sandbox scripting language is **indirection**: integers are
64-bit machine words, so a program can `peek`/`poke` raw memory, build typed pointers,
and make `int 0x80` syscalls directly — systems programming in a language that reads
like Python. It ships as `/bin/as`, runs scripts or a REPL, pipes through the shell,
and compiles modules to `.la` bytecode.

The **LibAether** standard library (`/usr/as/lib`, ~1,450 lines of AetherScript)
provides `seq`, `dicts`, `strings`, `sets`, `stats`, `random`, `paths`, `bits`,
`math`, and `mathx`, with a `test` assertion module. **Code Studio**
(`c/apps/gui/studio.c`) is its IDE: a syntax-highlighting editor that saves, runs
`/bin/as` over a pipe into an output pane, and jumps the caret to the reported error
line.

## A note on honesty

"From scratch" here means the kernel, drivers, filesystem, network stack, crypto,
window system, applications, and the AetherScript language are all original. Three
large pieces are **ported** and labelled as such: the QuickJS JavaScript engine,
NetSurf's LibCSS, and a double-only subset of musl's libm — used because the goal is a
working web/JS platform, not re-deriving a CSS parser or `pow()` by hand.
