# LogitOS

LogitOS OS is an experimental, AI-assisted x86_64 operating-system project. It
contains a standalone kernel that boots through GRUB/Multiboot2 into a graphical
desktop; it is not a Linux distribution or a renamed Linux kernel. The kernel,
drivers, LogitFS, network stack, window system, and applications are maintained
in this repository, alongside explicitly identified ports and adapted code.

In its tested QEMU configuration it boots to a frosted-glass desktop, fetches and
renders selected public HTTP and HTTPS pages through a from-scratch HTML5 parser
with **live** pages (event handlers, timers, promises), plays H.264 video, runs a
POSIX-inspired shell with `fork`/`exec`/pipes, and ships **AetherScript**, a
project language with a standard library and a small IDE. It is a research and
learning system, not a production OS, security product, or standards-complete web
browser.

Where a claim here can be measured, it is, and the number is the claim: the HTML
tokenizer passes **7032/7032** of the shared html5lib corpus and tree construction
**1723/1818**; the H.264 decoder is byte-identical to ffmpeg across eleven streams.
Where something does not work, this file says so rather than omitting it.

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
  **130-root** trust store. The TLS client negotiates x25519, P-256 and P-384 and
  handles **HelloRetryRequest**, so a server that wants a curve we did not offer
  first is reachable rather than impossible. It is **TLS 1.3 only** — servers that
  speak only 1.2 (sectigo.com among them) cannot be opened yet. The implemented and
  missing protocol pieces are listed in the [network support matrix](docs/NETWORK.md).
- **A from-scratch HTML5 parser** — a spec tokenizer and tree construction,
  including the **adoption agency algorithm** and foster parenting, which is what
  makes real-world malformed markup produce the right tree instead of a quietly
  wrong one. Measured against the corpus every browser is measured against
  (`third_party/html5lib-tests`, data only — the runner and the parser are ours):
  **tokenizer 7032/7032, tree construction 1723/1818**, with the remaining failures
  categorised rather than left as a number.
- **Pages are alive, not just rendered** — a persistent QuickJS runtime that
  outlives page load, `setTimeout`/`setInterval`/`requestAnimationFrame`, a drained
  microtask queue (so promises and `await` actually resolve), full capture → target
  → bubble event dispatch, and link navigation as a real **default action**, so
  `preventDefault()` prevents it. Plus a **CSSOM**: `element.style`,
  `getComputedStyle`, and scoped invalidation — a class toggle on a leaf re-styles
  2 elements instead of the whole document.
- **Plays H.264 video** — a from-scratch baseline decoder (CAVLC, I/P slices,
  multiple references, weighted prediction, deblocking) that is **byte-identical to
  ffmpeg** across eleven test streams. It runs in ring 3, not the kernel: a video is
  decoded thirty times a second and holds megabytes of reference frames.
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
  freestanding mini-libc + musl libm. What is missing is the platform, not the
  language: there is no `fetch`, no `XMLHttpRequest`, no `localStorage`, and
  `querySelectorAll` does not exist (`querySelector` handles only `#id`, `.class`
  and a bare tag). A modern single-page app does not run here.
- **Multi-core** — an SMP scheduler: AP bring-up (ACPI + LAPIC + trampoline), a
  big-kernel-lock model with ring-3 parallelism, and parallel framebuffer present.
- **Modern devices** — a virtio 1.0 PCI stack (virtio-blk disk, virtio-gpu display
  with DMA present), plus legacy ATA / e1000 / VGA fallbacks, and an NVMe driver.

## Status — the road so far

The original M1–M8 roadmap (boot → desktop) is complete, and the project has gone
far beyond it.

**Foundation & desktop (M1–M8):** boot + long mode + serial/VGA · interrupts +
PS/2 keyboard · PMM + kernel heap · preemptive scheduler · ATA + **LogitFS** +
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
and raw-memory + syscall indirection. Plus the LibLogit stdlib and the Code Studio IDE.

**Scaling & hardening (M25–M26 + ongoing):** a preemptive **SMP** scheduler · TCP
out-of-order **reassembly** (large TLS handshake flights arrive reliably) · NVMe ·
the **Aqua → LogitOS** rename · a system-wide security/bug-hunt pass.

**Post-roadmap:** per-process address spaces (each app its own PML4, CR3 switched on
schedule) and ring-3 fault containment — an app fault kills only that app; the
kernel and desktop survive.

**H.264 video:** a from-scratch baseline decoder in `c/lib/video/`, held to
byte-identity with ffmpeg rather than to a tolerance — H.264 reconstruction is
exactly specified integer arithmetic, so any mismatch is a bug and not a rounding
difference. `make test-video` boots LogitOS and requires the CRC32 the guest
computes to equal the host's.

**The browser rewritten around a real HTML5 parser:** the 279-line tag-soup scanner
was replaced by a spec tokenizer and tree construction, and the DOM data model with
it — a chunk arena instead of a malloc per node, which on the target's first-fit
allocator took a 120k-node parse from **110 seconds to 29 milliseconds**. On top of
that: a persistent JS runtime with timers and real event dispatch, a CSSOM with
scoped invalidation, roughly 45 CSS properties read from LibCSS instead of 19 (the
rest were being computed and thrown away), floats, overflow clipping and true alpha
compositing.

**Networking, in progress:** HTTP is moving out of the kernel. Today a fetch runs
the whole of DNS + TCP + TLS + HTTP synchronously inside a ring-0 syscall, so the
UI freezes for the duration and only one request can ever be in flight. A ring-3
HTTP/1.1 client with a real header list, a cookie jar, a connection pool and gzip is
written and tested; the async socket layer beneath it is in progress.

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
make                 # build build/logit.iso
make run             # boot in QEMU (-smp 4, virtio GPU/disk + e1000); desktop window + serial here
make build/disk.img  # rebuild the disk image after changing apps / fonts / fsroot
make verify-fonts    # verify vendored OFL sources and generated subset hashes
make debug           # boot frozen with a gdb stub on localhost:1234
make clean
```

Tests (headless, asserted over serial unless noted):

```sh
make test            # boot smoke test (asserts the kernel prints LOGIT_BOOT_OK)
make test-shell      # fork/exec + pipes + coreutils via /bin/sh
make test-as         # AetherScript language core (host unit tests, no QEMU)
make test-as-os      # AetherScript on LogitOS: runs the examples incl. the LibLogit stdlib
make test-net        # TCP + IPv4/UDP/ICMP protocol unit tests (host)
make test-net-os     # QEMU: guest fetches a 32 KiB file from a host-local HTTP server
make test-smp        # boots -smp 4 and asserts genuine cross-core parallelism
make test-nvme       # NVMe driver

make test-html5lib     # HTML tree construction vs the shared corpus (a rate, ratcheted)
make test-html5lib-tok # HTML tokenizer vs the same corpus, plus a byte-at-a-time cross-check
make test-browser      # the whole render pipeline, host-side: DOM, CSS, layout, JS, paint
make test-live-page    # on device: a click handler, a timer and preventDefault reach real pixels
make test-css-fidelity # on device: border-box, <pre>, and a wrapping flex row, measured in pixels
make test-video        # on device: decodes H.264 and requires the guest CRC to match the host
make test-tls-interop  # the real TLS client against a real openssl s_server, with a throwaway CA
make test-https-smoke  # live network: per-site handshake and reachability
make test-http-fuzz    # the ring-3 HTTP parser under ASan/UBSan, ~1.4M iterations
```

Two of these deserve a note, because both encode a lesson that cost real time:

`make test-html5lib` reports a **rate**, not pass/fail, and diffs against a
committed expected-failure list. A gate that is red on every run for weeks only
teaches people to stop reading the build; it becomes a gate when there is a rate
worth defending.

`make shot` boots headless, screendumps over QMP and writes `build/desktop.png`. It
exists to answer one question: when `make run` shows a QEMU window with nothing in
it, is the guest not drawing, or is the host not painting? Those look identical from
the outside and have completely different causes. If the image is correct, stop
looking at this repository.

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
c/fs/                                          VFS + LogitFS (hierarchical read-write inode FS)
c/net/{link,ip,transport,core,dns,http,tls}/  network stack: eth/arp/ip/icmp/udp/dns/tcp/http + TLS 1.3 + x509
c/crypto/{hash,aead,pubkey,trust}/            project crypto code (SHA, HMAC/HKDF, ChaCha20-Poly1305, AES-GCM, X25519, ECDSA, RSA, roots)
c/lib/{image,text}/                            shared libs (inflate/png/gif, utf8, TrueType raster)
c/apps/                                        ring-3 apps + shared logit.h / clib.h / crt0
c/apps/gui/                                    windowed apps: Finder, Terminal, TextEdit, Clock, Monitor, Code Studio + the aui toolkit
c/apps/coreutils/                              /bin/sh + coreutils
c/apps/as/                                     AetherScript: /bin/as (lexer, compiler, VM)
c/apps/browser/                                browser + render engine (dom, layout, css_engine, paint, js_dom) + QuickJS
c/apps/libc/                                   project freestanding mini-libc
third_party/                                     ported, not from-scratch: QuickJS, NetSurf LibCSS, musl libm
include/abi/logit_abi.h                         the app/syscall ABI (kernel ↔ userland)
tools/                                           mkfs.py, mkaex.py, mkfont.py, genroots.py, QMP screenshot/CI drivers
fsroot/                                          files packed into the disk image (incl. /usr/as LibLogit stdlib + examples)
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

The **LibLogit** standard library (`/usr/as/lib`, ~1,450 lines of AetherScript)
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
kernel, boot code, drivers, filesystems, network stack, LogitTLS, cryptography,
and kernel-only support libraries — is licensed under `GPL-3.0-or-later`.
Project-authored user applications, the public userspace ABI, shared Rust parser
code, build tools, tests, documentation, and examples are licensed under `MIT`.

See [LICENSE](LICENSE) and [LICENSING.md](LICENSING.md) for the exact path
boundary. Code and data listed in [THIRD_PARTY.md](THIRD_PARTY.md) retain their
upstream licenses. The bootable ISO is consequently a multi-license aggregate,
not an entirely MIT or entirely GPL artifact.
