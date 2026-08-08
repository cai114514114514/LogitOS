# LogitOS

LogitOS OS is an experimental, AI-assisted x86_64 operating-system project. It
contains a standalone kernel that boots through GRUB/Multiboot2 into a graphical
desktop; it is not a Linux distribution or a renamed Linux kernel. The kernel,
drivers, LogitFS, network stack, window system, and applications are maintained
in this repository, alongside explicitly identified ports and adapted code.

In its tested QEMU configuration it boots to a frosted-glass desktop, fetches and
renders selected public HTTP and HTTPS pages through a from-scratch HTML5 parser
with **live** pages (event handlers, timers, promises), decodes H.264 and H.265
video and MP3/FLAC/AAC/Vorbis audio out of MP4 and Matroska containers, runs a
POSIX-inspired shell with `fork`/`exec`/pipes, and ships **AetherScript**, a
project language with a standard library and a small IDE. It is a research and
learning system, not a production OS, security product, or standards-complete web
browser.

Where a claim here can be measured, it is, and the number is the claim: the HTML
tokenizer passes **7032/7032** of the shared html5lib corpus and tree construction
**1723/1818**; the H.264 decoder reproduces a real 1080p bilibili stream
**byte for byte across 254 frames**; the H.265 decoder matches the ITU conformance
package's own MD5 on a Main 10 stream; the AAC decoder scores inside the MPEG
full-accuracy bound on **30 of 41** ISO conformance bitstreams. Where something
does not work, this file says so rather than omitting it — see
[What does not work](#what-does-not-work).

Development has been heavily assisted by Claude and other coding agents. That
does not change the license, but it is part of the project's provenance. See
[Project Transparency](TRANSPARENCY.md), [Third-Party Software and Data](THIRD_PARTY.md),
and the [Security Policy](SECURITY.md).

## What it can do

- **Boots to a macOS-style desktop** — gradient wallpaper, a frosted menu bar and
  Dock, draggable/focusable windows with traffic-light controls, a compositor with
  alpha blending, a PS/2 mouse cursor, and a real wall clock from the CMOS RTC.
- **Browses selected real sites** — `http://` and **`https://`** pages over project
  implementations of TCP, HTTP/1.1 **and HTTP/2** (frames + HPACK), a connection
  pool, a cookie jar, **TLS 1.3 and TLS 1.2**, and a limited X.509 verifier backed
  by a **130-root** trust store. The TLS client negotiates x25519, P-256, P-384 and
  **P-521**, handles **HelloRetryRequest**, and does **session resumption** (PSK +
  tickets, `psk_dhe_ke` only — never bare `psk_ke`, which would trade forward secrecy
  for a scalar multiplication). Interop is checked against a real `openssl s_server`
  across **48 cases**. **IPv6** with neighbour discovery, DHCP, and a link layer with
  RFC 4861-shaped ARP are present. The implemented and missing protocol pieces are
  listed in the [network support matrix](docs/NETWORK.md).
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
- **Decodes real video** — from-scratch **H.264 High profile** (CABAC, B slices with
  a 4-deep pyramid, 8×8 transform, implicit weighted bi-prediction, scaling matrices)
  and **H.265 Main / Main 10** (CTU quadtree, 35 intra modes, merge/AMVP, SAO, tiles,
  wavefronts). Held to byte-identity with ffmpeg rather than to a tolerance, because
  reconstruction is exactly specified integer arithmetic and any mismatch is a bug and
  not a rounding difference: a real 1080p bilibili stream decodes **byte for byte
  across 254 frames**, and an ITU Main 10 conformance stream matches the package's own
  MD5 over 256 pictures. Both run in ring 3, not the kernel — a video is decoded thirty
  times a second and holds megabytes of reference frames. **HEVC B slices are not yet
  bit-exact; see [What does not work](#what-does-not-work).**
- **Decodes real audio** — WAV, FLAC (bit-exact), MP3, **AAC-LC** and **Vorbis**, with
  MP4/fMP4 and Matroska/WebM demuxing and an A/V sync clock. AAC is scored against the
  **ISO conformance package's own reference waveforms**, not against another decoder.
- **Reads the image formats the web uses** — PNG, JPEG, GIF including **animation with
  real frame timing and disposal**, WebP (VP8L), APNG, ICO/CUR, BMP, and **EXIF
  orientation** — each verified byte-exact against an independent reference.
- **Anti-aliased Unicode text** — a project TrueType parser and integer-only AA
  rasterizer with a glyph cache, font fallback, CJK support, plus CFF outlines,
  colour fonts, script detection, **bidi** and OpenType shaping; the whole UI is
  anti-aliased.
- **Keeps what you give it** — LogitFS gained a **write-ahead log** (`data=ordered`),
  barriers actually issued to the device and counted by the kernel, record checksums,
  and an `fsck` that runs read-only at every mount. Proven rather than asserted: five
  real boots against one image **with no `-snapshot`**, three files verified byte for
  byte, a 4.4 MB double-indirect file surviving a clean reboot, and **1744 host checks
  that cut power at every single device write**.
- **Runs real processes** — a POSIX-ish model (`fork`/`execve`/`waitpid`, file
  descriptors, pipes, a TTY) with `/bin/sh` (pipes, redirects, job control) and
  coreutils (`ls cat echo wc head mkdir rm …`). The GUI Terminal is a real terminal
  emulator over the same shell.
- **Its own language** — **AetherScript** (`/bin/as`, `.as`): a Python-flavored,
  clox-lineage language with substantial project-specific syntax, runtime features,
  bytecode, libraries, and OS integration. It has raw-memory and direct-syscall
  facilities and is not a sandbox. See [Third-Party Software and Data](THIRD_PARTY.md)
  for the lineage and upstream notice.
- **Runs JavaScript, with a platform under it** — a ported QuickJS as a ring-3 app on
  a project freestanding mini-libc + musl libm, now with `fetch` (streaming response
  bodies, binary `Uint8Array` request bodies), `XMLHttpRequest`, `localStorage` and
  `sessionStorage`, a selector engine, `performance`, `queueMicrotask`,
  `MessageChannel`, `requestIdleCallback`, `DOMException`, the three observers,
  `structuredClone`, `TextEncoder`, `Intl`, ES modules with dynamic `import()`, and
  `data:` URLs. The platform is measured by an instrument, not guessed at: a probe
  records every global lookup that misses across a corpus of real pages, and
  `test-platform` asserts that things which **must stay absent** (`ActiveXObject`,
  `documentMode`, …) remain absent, so a future "reduce the miss list" change fails a
  test instead of a page. A modern single-page app still does not run here — see
  [What does not work](#what-does-not-work).
- **Multi-core** — an SMP scheduler: AP bring-up (ACPI + LAPIC + trampoline), a
  big-kernel-lock model with ring-3 parallelism, and parallel framebuffer present.
  The scheduler and the kernel heap have been **peeled out from under the BKL**, and
  the proof required is wall-clock speedup, not absence of corruption: N children must
  finish in well under N×T1, because a syscall still holding the lock would serialise
  them.
- **Memory that survives being full** — per-frame **reverse mapping**, page reclaim
  with a clock hand, pinning enforced structurally rather than by a list, and **swap**.
  On a deliberately small (192 MiB) machine running the real desktop plus a program
  mapping more memory than exists, 14,218–38,400 frames are evicted per run with zero
  allocation failures, where previously the process simply died.
- **Modern devices** — a virtio 1.0 PCI stack (virtio-blk, virtio-gpu with DMA
  present, virtio-net), **AHCI** and **NVMe** storage, **xHCI with USB HID**, Intel
  **HDA audio**, Realtek **RTL8169**/8139 and Intel e1000 networking, plus legacy
  ATA / PS/2 / VGA fallbacks.
- **A boundary between ring 3 and the kernel** — **SMEP** enabled, **W^X** enforced
  (ELF segments map as the file asks; a W+X segment is refused outright), POSIX file
  permissions checked on every VFS path, and every pointer crossing `int 0x80`
  validated with the USER bit required at *every* level of the page walk. Verified by
  an exploit suite, not by assertions: `/bin/secprobe` runs one attack per process and
  `make test-sec` fails on the parent commit and passes now.

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

**Beyond the roadmap:** IPv6 + neighbour discovery, DHCP, HTTP/2 with HPACK, TLS 1.2
and session resumption · a journalled filesystem with `fsck` and a crash-tested
recovery path · AHCI, NVMe, xHCI/USB-HID, Intel HDA audio, virtio-net and RTL8169 ·
H.264 High profile and H.265 Main 10 · AAC and Vorbis · MP4/Matroska demuxing with
A/V sync · WebP, APNG, ICO, BMP, animated GIF and EXIF · text shaping and bidi ·
reverse mapping, page reclaim and swap · SMEP and W^X · damage-tracked compositing
(a dock hover recomposites 7% of the screen instead of 100%) · a widget toolkit with
real layout, focus and keyboard navigation · and a CI that can tell the difference
between a passing test and a test that cannot fail.

**Video:** from-scratch H.264 and H.265 decoders in `c/lib/video/`, held to
byte-identity with ffmpeg rather than to a tolerance — reconstruction is exactly
specified integer arithmetic, so any mismatch is a bug and not a rounding difference.
H.264 began as Baseline and now covers **High profile**, which is what real sites
actually serve: bilibili's ladder advertises `avc1.640033`, and both its 640×360 and
1920×1080 renditions decode byte for byte. `make test-video` boots LogitOS and
requires the CRC32 the guest computes to equal the host's, so the decoder is held to
the same bar on the target as on the host.

One number is worth keeping for its own sake: on that real 1080p stream, **53 of 104
frames come out of decode order.** Pairing timestamps with a decode-order queue — which
is exactly what the player did before B-frames existed — would have silently mis-timed
half of them.

**The browser rewritten around a real HTML5 parser:** the 279-line tag-soup scanner
was replaced by a spec tokenizer and tree construction, and the DOM data model with
it — a chunk arena instead of a malloc per node, which on the target's first-fit
allocator took a 120k-node parse from **110 seconds to 29 milliseconds**. On top of
that: a persistent JS runtime with timers and real event dispatch, a CSSOM with
scoped invalidation, roughly 45 CSS properties read from LibCSS instead of 19 (the
rest were being computed and thrown away), floats, overflow clipping and true alpha
compositing.

**Networking, out of the kernel:** the fetch that used to run all of DNS + TCP + TLS
+ HTTP synchronously inside a ring-0 syscall — freezing the UI, one request ever in
flight — is gone. HTTP/1.1 and HTTP/2 now run in ring 3 over an async socket layer,
with a header list, a cookie jar, a connection pool and content encoding. The pooling
is measured rather than claimed: one real page loads 15 requests with **13 reused
connections**.

**Media, and what "correct" means here:** every codec in `c/lib/{video,audio,image}`
is held to the strictest bar its format actually defines, and the difference is stated
rather than blurred. H.264 and H.265 reconstruction is exactly specified, so the bar is
**byte-identity with ffmpeg**. AAC and MP3 are defined by a conformance *tolerance*, so
the bar is ISO's own reference waveforms and the published bound — and the README does
not call that "bit-exact". Vorbis defines no numeric bound and Xiph publishes no
conformance suite, so it gets a differential and an explicit note that **there is
nothing to pass**.

**Things that could not fail, made able to fail:** an audit of the test suite found
**26 dead harnesses, 15 that printed a verdict and exited 0 either way, and 217
`test-` targets wired into no suite at all** — including the five-boot durability
proof, which belonged to nothing and had apparently never run. `make ci` now
clean-clone-builds HEAD and asserts the artifacts exist rather than trusting make's
exit status, and `make test-audit` fails the build when a harness cannot fail. Its
suite list is **derived from the Makefile**, because a hand-written list is precisely
what rotted into 217 orphans.

Each milestone follows **spec → plan → implement**; specs live in
`docs/superpowers/specs/`.

## What does not work

Kept deliberately, and kept specific.

- **No Media Source Extensions**, so a `<video>` on a real site does not play. Every
  decoder above exists and a web page cannot reach any of them: sites serve separate
  audio and video fMP4 segments, and feeding those to one element is MSE or nothing.
- **A large single-page application still does not load.** kimi.com is **12.77 MB of
  JavaScript across 134 files**; QuickJS compiles eagerly with no pre-parse, and one
  runtime holding all of it measures **32 MB resident for 9 MB of source**. It fits,
  but the compile cost has not been made acceptable.
- **HEVC B slices are not bit-exact**, at either bit depth. The failure is localized to
  the residual path of one AMP CU in a B slice, with a two-picture reproduction; real
  streaming content is B-heavy, so treat "HEVC works" as a claim about I/P material.
  HEVC scaling lists are likewise not bit-exact.
- **NX is written, tested and inert.** `c/kernel/mm` converts PTEs to frames with a
  mask that clears the low flag bits and keeps bit 63, so an NX page reaches the frame
  allocator as a corrupt physical address. Until ~26 masks change, a ring-3 program can
  execute any data it can write. **SMAP** is likewise off, with the 38 sites that would
  have to route through `user_copy_*` first enumerated rather than estimated.
- **File permissions do not survive a reboot** — contents are durable, the mode bits
  are in RAM.
- **No AV1, no VP8/VP9, no Opus**, no progressive JPEG, no lossy or animated WebP.
- **No input method**, so CJK text can be displayed but not typed.
- **No window resize, no maximize, no cross-application clipboard, no notifications,
  and nothing is remembered between boots.** The desktop's whole verb vocabulary is
  drag and close.
- **1080p video does not fit on the device** — the decoder's peak working set is about
  62 MB against mini-libc's arena bound.
- **There is one big kernel lock.** SMP is real and two subsystems are out from under
  it; everything else still serialises.

## Build & run

The Makefile expects a POSIX shell and Unix utilities; direct PowerShell builds are
not supported. Target: x86_64. The project has been built on macOS / Apple Silicon
(arm64) and under WSL/Ubuntu — on Windows, **build inside WSL**, and note that Git
Bash's `/tmp` and WSL's `/tmp` are different directories, so a clean-clone check that
clones on one side and builds on the other verifies nothing.

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
make ci              # clean-clone-build HEAD and run the suites; asserts the artifacts exist
make test-audit      # fails the build on a harness that cannot fail (no target, no non-zero exit)

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

make test-h264         # byte-identity vs ffmpeg, incl. a real 1080p bilibili stream
make test-h265         # the same bar for HEVC; test-h265-diff prints per-case wrong-byte totals
make test-aac-conformance  # ISO's own bitstreams against ISO's own reference waveforms
make test-demux-diff   # container sample boundaries, timestamps and extradata vs ffmpeg
make test-img-anim     # animated GIF/APNG: composited canvas, per-frame delay, loop count
make test-durability   # five real boots, NO -snapshot, three files verified byte for byte
make test-fscrash      # SIGKILL mid-write: the victim is whole or absent, never torn
make test-barrier      # barriers actually reach the device, counted by the kernel
make test-sec          # /bin/secprobe: one attack per process; fails on the parent commit
make test-swap         # reclaim + swap on a deliberately small machine
```

Several of these carry a **negative control** — the same test against a build with the
fix compiled out, which is *required to fail*. `make test-js-syntax-control`,
`test-loader-negctl`, `test-aui-negctl`, `test-img-fuzz-negctl`, `test-p521-control`,
`test-arena` and others exist for one reason: an assertion nobody has watched fail is
not a known-failing assertion.

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
c/kernel/{core,cpu,mm,sched,exec,gui,pci,audio}/  kernel by subsystem (wm = window manager + GUI syscalls, SMP, reclaim/swap/rmap)
c/drivers/{char,timer,block,net,virtio,usb,audio,core}/  PS/2, PIT/RTC, ATA/AHCI/NVMe/virtio-blk, e1000/rtl8169/virtio-net, virtio-gpu, xHCI+HID, Intel HDA
c/fs/                                          VFS + LogitFS (journalled read-write inode FS) + fsck, bcache, ramfs, credentials
c/net/{link,ip,transport,core,dns,http,tls}/  eth/arp/ip4/ip6+ND/icmp/udp/dns/tcp/dhcp + HTTP/1.1 + HTTP/2 + TLS 1.3/1.2 + x509
c/crypto/{hash,aead,pubkey,trust}/            project crypto code (SHA, HMAC/HKDF, ChaCha20-Poly1305, AES-GCM, X25519, ECDSA, RSA, roots)
c/lib/{image,text,video,audio,media}/          shared ring-3 libs: png/jpeg/gif/webp/bmp/ico/exif · utf8/TrueType/CFF/shaping/bidi ·
                                                 H.264 + H.265 · WAV/FLAC/MP3/AAC/Vorbis · MP4/MKV demux + A/V clock
c/apps/                                        ring-3 apps + shared logit.h / clib.h / crt0
c/apps/gui/                                    windowed apps: Finder, Terminal, TextEdit, Clock, Monitor, Preview, Gallery,
                                                 Network, Code Studio + the aui widget toolkit
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
