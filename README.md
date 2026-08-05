# LogitOS OS

LogitOS OS is an experimental, AI-assisted x86_64 operating-system project. It
contains a standalone kernel that boots through GRUB/Multiboot2 into a graphical
desktop; it is not a Linux distribution or a renamed Linux kernel. The kernel,
drivers, AetherFS, network stack, window system, and applications are maintained
in this repository, alongside explicitly identified ports and adapted code.

In its tested QEMU configuration it boots to a frosted-glass desktop, can fetch
and render selected public HTTP and HTTPS pages, runs a POSIX-inspired shell with
`fork`/`exec`/pipes, and ships **AetherScript**, a project language with a standard
library and a small IDE. It is a research and learning system, not a production
OS, security product, or standards-complete web browser.

Development has been heavily assisted by Claude and other coding agents. That
does not change the license, but it is part of the project's provenance. See
[Project Transparency](TRANSPARENCY.md), [Third-Party Software and Data](THIRD_PARTY.md),
and the [Security Policy](SECURITY.md).

## What it can do

- **Boots to a macOS-style desktop** — gradient wallpaper, a frosted menu bar and
  Dock, draggable/focusable windows with traffic-light controls, a compositor with
  alpha blending, a PS/2 mouse cursor, and a real wall clock from the CMOS RTC.
- **Browses selected real sites** — `http://` and **`https://`** pages over project
  implementations of TCP, HTTP, TLS 1.3, and a limited X.509 verifier backed by a
  **130-root** trust store, plus an HTML/DOM + CSS-cascade + layout + paint pipeline,
  decoded PNG/GIF images, and inline `<script>` execution. Wikipedia has been used
  as an interoperability test; this is not a general-purpose or security-hardened
  browser. The implemented and missing protocol pieces are listed in the
  [network support matrix](docs/NETWORK.md).
- **Anti-aliased Unicode text** — a project TrueType parser + integer-only AA
  rasterizer (the kernel is `-mno-sse` in places) with a glyph cache, font fallback,
  and CJK support; the whole UI is anti-aliased.
- **Runs real processes** — a POSIX-ish model (`fork`/`execve`/`waitpid`, file
  descriptors, pipes, a TTY) with `/bin/sh` (pipes, redirects, job control) and
  coreutils (`ls cat echo wc head mkdir rm …`). The GUI Terminal is a real terminal
  emulator over the same shell.
- **Its own language** — **AetherScript** (`/bin/as`, `.as`): a Python-flavored,
  clox-lineage language with substantial project-specific syntax, runtime features,
  bytecode, libraries, and OS integration. It has raw-memory and direct-syscall
  facilities and is not a sandbox. See [Third-Party Software and Data](THIRD_PARTY.md)
  for the lineage and upstream notice.
- **Runs JavaScript** — a ported QuickJS as a ring-3 app, on a project
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
**HTTP** + Browser app · **TLS 1.3** (project SHA/HMAC/HKDF, ChaCha20-Poly1305,
AES-GCM, X25519, P-256/384 ECDSA, **RSA** + a 130-root CA bundle) · **HTML/CSS**
layout → paint with images · **Unicode + project anti-aliased TrueType** text.

**Becoming a real platform (M15–M19):** SSE2/FPU at boot · **JavaScript** (QuickJS)
as a ring-3 app on a project mini-libc + ported libm · **LibCSS** + the whole render
pipeline moved into the browser app, with JS↔DOM bindings · **real processes**
(`fork`/`exec`/pipes/shell/coreutils) · **virtio** (modern paravirtual disk + GPU).

**Its own language (M20–M22):** **AetherScript** — lexer (INDENT/DEDENT) + single-pass
compiler + stack VM; strings/lists/dicts, `for`/`range`, modules + `import`, closures,
classes (inheritance + `super`), exceptions (`raise`/`try`/`except`), a mark-sweep GC,
and raw-memory + syscall indirection. Plus the LibAether stdlib and the Code Studio IDE.

**Scaling & hardening (M25–M26 + ongoing):** a preemptive **SMP** scheduler · TCP
out-of-order **reassembly** (large TLS handshake flights arrive reliably) · NVMe ·
the **Aqua → LogitOS** rename · a system-wide security/bug-hunt pass.

**Post-roadmap:** per-process address spaces (each app its own PML4, CR3 switched on
schedule) and ring-3 fault containment — an app fault kills only that app; the
kernel and desktop survive.

Each milestone follows **spec → plan → implement**; specs live in
`docs/superpowers/specs/`.

## Build & run

Primary development host: macOS / Apple Silicon (arm64). The remediation snapshot
documented in `docs/CODE_AUDIT.md` was also built under WSL/Ubuntu 26.04. The
Makefile expects a POSIX shell and Unix utilities; direct PowerShell builds are not
documented. Target: x86_64.

Required tools include LLVM/Clang + LLD, NASM, Rustup with the
`x86_64-unknown-none` target, Python 3, GNU Make and Unix utilities, QEMU, xorriso,
and GRUB's `grub-mkrescue` (named `i686-elf-grub-mkrescue` by the Makefile).

LogitOS now vendors OFL-licensed Noto Sans SC and Noto Sans Mono source fonts and
checked-in, distinctly named runtime subsets. A normal build does not read host
fonts or use the network. Run `make regen-fonts` only when intentionally rebuilding
the subsets; see `fsroot/fonts/README.md` and [THIRD_PARTY.md](THIRD_PARTY.md).

The current local wallpaper is a separate, unresolved Apple-derived asset and is
not cleared for public binary distribution. Replace it before publishing an image.

```sh
make                 # build build/aether.iso
make run             # boot in QEMU (-smp 4, virtio GPU/disk + e1000); desktop window + serial here
make build/disk.img  # rebuild the disk image after changing apps / fonts / fsroot
make verify-fonts    # verify vendored OFL sources and generated subset hashes
make debug           # boot frozen with a gdb stub on localhost:1234
make clean
```

Tests (headless, asserted over serial unless noted):

```sh
make test            # boot smoke test (asserts the kernel prints AETHER_BOOT_OK)
make test-shell      # fork/exec + pipes + coreutils via /bin/sh
make test-as         # AetherScript language core (host unit tests, no QEMU)
make test-as-os      # AetherScript on LogitOS: runs the examples incl. the LibAether stdlib
make test-net        # TCP + IPv4/UDP/ICMP protocol unit tests (host)
make test-net-os     # QEMU: guest fetches a 32 KiB file from a host-local HTTP server
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
c/crypto/{hash,aead,pubkey,trust}/            project crypto code (SHA, HMAC/HKDF, ChaCha20-Poly1305, AES-GCM, X25519, ECDSA, RSA, roots)
c/lib/{image,text}/                            shared libs (inflate/png/gif, utf8, TrueType raster)
c/apps/                                        ring-3 apps + shared aether.h / clib.h / crt0
c/apps/gui/                                    windowed apps: Finder, Terminal, TextEdit, Clock, Monitor, Code Studio + the aui toolkit
c/apps/coreutils/                              /bin/sh + coreutils
c/apps/as/                                     AetherScript: /bin/as (lexer, compiler, VM)
c/apps/browser/                                browser + render engine (dom, layout, css_engine, paint, js_dom) + QuickJS
c/apps/libc/                                   project freestanding mini-libc
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

A project language in `c/apps/as/`: a materially adapted clox-lineage single-pass
compiler that emits flat bytecode for a computed-goto stack VM. Its surface syntax,
bytecode format, OS integration, and many runtime features are LogitOS-specific.
Python-ish (indentation blocks, dynamic types), with `def`,
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

"Project implementation" means that the implementation is maintained here rather
than taken from Linux or another OS. It does not mean standards completeness,
production readiness, absence of AI assistance, or absence of conceptual influence.
Vendored and adapted components are listed in [THIRD_PARTY.md](THIRD_PARTY.md), and
the authorship, testing, and capability boundaries are recorded in
[TRANSPARENCY.md](TRANSPARENCY.md). Binary distributors should also follow
[RELEASING.md](RELEASING.md).

## License

LogitOS is a multi-license project. The project-authored operating-system core —
kernel, boot code, drivers, filesystems, network stack, AetherTLS, cryptography,
and kernel-only support libraries — is licensed under `GPL-3.0-or-later`.
Project-authored user applications, the public userspace ABI, shared Rust parser
code, build tools, tests, documentation, and examples are licensed under `MIT`.

See [LICENSE](LICENSE) and [LICENSING.md](LICENSING.md) for the exact path
boundary. Code and data listed in [THIRD_PARTY.md](THIRD_PARTY.md) retain their
upstream licenses. The bootable ISO is consequently a multi-license aggregate,
not an entirely MIT or entirely GPL artifact.
