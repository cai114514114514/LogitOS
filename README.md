<p align="center">
  <img src="docs/assets/logitos-mark.svg" width="72" alt="">
</p>

<h1 align="center">LogitOS</h1>

<p align="center">
  <b>An experimental x86_64 operating system, written layer by layer —<br>
  from its own boot sector to a desktop where an AI agent works inside native apps.</b>
</p>

<p align="center">
  <a href="https://github.com/cai114514114514/LogitOS/releases/tag/v0.1.0-alpha">Release v0.1.0-alpha</a> ·
  <a href="#build--run">Build &amp; run</a> ·
  <a href="#what-it-can-do">What it can do</a> ·
  <a href="#what-does-not-work">What does not work</a> ·
  <a href="TRANSPARENCY.md">Transparency</a>
</p>

---

LogitOS is a standalone operating system for x86_64. It is **not** a Linux distribution, a Linux fork or a
skin over a host OS: it boots through **its own BIOS and UEFI loaders** and its own boot protocol, and the
kernel, drivers, filesystem, network stack, TLS, graphics stack, window system, browser engine, programming
language and applications are maintained in this repository — alongside clearly identified ports such as
QuickJS and NetSurf LibCSS (see [THIRD_PARTY.md](THIRD_PARTY.md)).

The name is the point. A *logit* is the raw score a model produces before it becomes a choice. LogitOS asks
what an operating system looks like when those scores have to land on a real machine: an agent service
(`agentd`) runs as an ordinary ring-3 process, sees only the files you hand it, and returns its work to native
applications as a reviewable proposal — never as a silent edit.

**How it is made.** LogitOS is human-directed and its first-party production code is written by AI coding
agents, as a matter of policy. Humans set goals, design the architecture, review, test, verify and integrate.
Every test is expected to carry a *negative control* — a deliberately broken build that the test must catch —
and user-interface work is checked screen by screen in QEMU against the previous image. See
[CONTRIBUTING.md](CONTRIBUTING.md) and [TRANSPARENCY.md](TRANSPARENCY.md).

> **Status: experimental.** LogitOS is a research and learning system. There is no security support and no SLA.
> Do not use it to protect real credentials, personal data, keys or production workloads, and do not treat its
> browser, TLS, isolation, filesystem or drivers as a security boundary. See [SECURITY.md](SECURITY.md).

## At a glance

Lines of code in git-tracked files on `main` (2026-10-29, comments and blank lines included):

| Layer | Where | Lines |
|---|---|---:|
| Boot (BIOS + UEFI loaders, long-mode entry) | `c/boot/` | 3,948 |
| Kernel (window compositor, processes & loader, memory, CPU/ACPI, scheduler, …) | `c/kernel/` | 66,547 |
| Drivers (USB, GPU, NICs, block, audio, virtio, power, timers) | `c/drivers/` | 35,200 |
| Filesystem (LogitFS, VFS, procfs, ramfs, caches) | `c/fs/` | 9,366 |
| Network and cryptography (IP, TCP, DNS, TLS, HTTP/1.1 + HTTP/2, SSH) | `c/net/`, `c/crypto/` | 55,975 |
| Graphics, text and media libraries | `c/lib/` | 84,942 |
| Browser engine | `c/apps/gui/browser/` | 131,650 |
| AetherScript language | `c/apps/as/` | 23,649 |
| C library | `c/apps/libc/` | 19,545 |

In total `git ls-files` tracks about 1.14 million lines of code, across more than 1,690 commits since 2026-05-30.

## What it can do

### Boot, kernel and memory

- **Its own boot path, BIOS and UEFI.** `tools/mkiso.py` writes the ISO, `c/boot/bios/` brings the machine up
  from the El Torito boot sector and `c/boot/efi/loader.c` does the same under UEFI. A real boot under QEMU
  reports, in order: loader → long mode → CPU and protection setup → memory accounting → interrupts and devices
  → LogitFS mounted → TLS trust store → network → desktop → SMP bring-up → `LOGIT_BOOT_OK` → the first ring-3
  apps.
- **SMP.** Application processors are brought up through ACPI, the LAPIC and a trampoline (`[smp] 4/4 CPUs
  online`). The big kernel lock is gone: synchronisation follows object ownership
  ([docs/BKL_REMOVAL_2026-09-10.md](docs/BKL_REMOVAL_2026-09-10.md)).
- **A weighted scheduler.** On every 100 Hz tick each core runs the runnable thread with the smallest weighted
  virtual time, where `weight = round(1024 · 2^(−nice/10))` — a decade of nice is exactly a factor of two, and the
  table spans 4096:274. A waking thread is placed at the run ring's virtual "now", so it runs soon but banks no
  credit for having slept (`c/kernel/sched/sched.c`).
- **Memory.** Per-process address spaces, a physical map backed by 2 MiB pages, per-frame metadata (bitmap,
  poison map, 2-byte reference counts), reverse mapping, page reclaim, swap and a page cache. On a deliberately
  small 192 MiB machine running the desktop plus a program mapping more memory than exists, thousands of frames
  are evicted per run with no allocation failures.
- **A boundary between ring 3 and the kernel.** NX, SMEP and W^X are enabled at boot; every pointer crossing the
  syscall boundary is validated with the user bit required at every level of the page walk, and `make test-sec`
  runs one attack per process from `/bin/secprobe`. SMAP is not enabled yet.

### Storage, devices and networking

- **LogitFS** — a journalled (write-ahead log, `data=ordered`) read-write filesystem with record checksums,
  barriers that actually reach the device, and a read-only `fsck` at every mount. `make test-durability` boots one
  image five times **without `-snapshot`**, and 1,744 host checks cut power at every single device write.
- **Devices** — virtio 1.0 (blk, gpu, net), AHCI and NVMe, xHCI with USB HID and storage, Intel HDA audio,
  Intel e1000 and Realtek RTL8169/8139, plus ATA, PS/2 and VGA fallbacks.
- **Networking** — project implementations of Ethernet/ARP, IPv4 and IPv6 with neighbour discovery, DHCP, TCP,
  UDP, DNS, HTTP/1.1 and **HTTP/2** (frames and HPACK), a connection pool and a cookie jar. **TLS 1.3 and 1.2**
  with x25519 and P-256/384/521, HelloRetryRequest and PSK resumption (`psk_dhe_ke` only), a limited X.509
  verifier and a 130-root trust store; interop is checked against a real `openssl s_server` across 48 cases. See
  the [network support matrix](docs/NETWORK.md).

### Graphics, text and the desktop

- **A graphics stack of its own** (`c/lib/gfx`, `c/lib/gfx3d`): vector paths and strokes, arcs and squircles,
  linear, radial and conic gradients, Porter–Duff blending, filtered image blits, clip stacks and opacity groups,
  analytic shadows, four tiers of glass material with backdrop blur and adaptive tint, easing curves and
  fixed-point springs — and a software 3D pipeline with raster state, depth, culling, mipmapped textures, and a
  shading language (LSL) compiled to a small IR and VM. Applications reach the compositor through a validated
  GUI ABI v2 (`SYS_GUI_PATH`, `GLASS2`, `SHADOW2`, `BLIT2`, `BACKDROP`, `TEXT_RUN2`).
- **Text** — TrueType and CFF outlines, colour fonts, OpenType shaping, bidi, UAX #14 line breaking, paragraph
  layout with head/middle/tail ellipsis, medium and semibold UI faces, and quarter-pixel glyph placement. A pinyin
  input method is built in (Shift+Space).
- **The desktop** — a compositor with damage tracking, a menu bar, a Dock, Spotlight, notifications, zoomed
  windows and a toolkit (`aui`) shared by the native apps: Finder, TextEdit, Terminal, Code Studio, Clock,
  Activity Monitor, Preview, Settings, the Browser and more. A new interface language, 「澄」 *Clarity*, is being
  rolled out; its first three waves (tokens, the widget toolkit, the window-manager split, generated app icons)
  are merged.

### The browser

- **A spec HTML5 parser** — tokenizer **7032/7032** and tree construction **1723/1818** on the shared
  html5lib corpus (data only; the runner and the parser are ours), including the adoption agency algorithm and
  foster parenting.
- **Live pages** — a persistent QuickJS runtime with timers, `requestAnimationFrame`, a drained microtask queue,
  full capture/target/bubble event dispatch and real default actions; a CSSOM with scoped invalidation; `fetch`
  with streaming bodies, `XMLHttpRequest`, storage, observers, `structuredClone`, `Intl`, ES modules and more.
- **Media** — H.264 High profile and H.265 Main/Main 10 decoders held to **byte identity with ffmpeg** (a real
  1080p stream decodes byte for byte across 254 frames); MPEG-1/2, MPEG-4 Part 2 and MJPEG; WAV, FLAC, MP3, AAC,
  Vorbis and Opus; MP4/fMP4 and Matroska/WebM demuxing; PNG, JPEG, GIF (animated), WebP, APNG, ICO, BMP and EXIF.

### The agent, inside the system

- `agentd` is a ring-3 broker. A task sees only the files you select; its output comes back to the app that owns
  the document as a **candidate**, shown in TextEdit's work panel next to the text. Nothing changes until you
  apply it, a pending review survives a reboot, stale decisions are refused, and you can always keep the current
  version.
- Models are reached through a host gateway (`tools/agent_gateway.py`) so provider credentials never enter the
  guest; the guest gets a one-time capability for a single bounded endpoint. A local inference path for GGUF
  models lives in `c/lib/nn`.
- A real run on 2026-10-29 (DeepSeek, BIOS, 512 MiB): **21 checks passed in 72 seconds**, six model calls, all
  HTTP 200 — drafting a cited report, preserving a hand-written sentence, holding the proposal for review across
  a power cycle, applying and saving it, and keeping the document unchanged when asked.

### AetherScript

A project language in `c/apps/as/` (`/bin/as`, `.as` files) with Python-flavoured syntax. Its single-pass
bytecode compiler and stack VM have clox (*Crafting Interpreters*) lineage; AetherScript 3 adds static types and
ahead-of-time native compilation of a supported subset through LLVM. Integers are machine words, so programs can `peek`/`poke` memory and make syscalls
directly — systems programming that reads like Python. It ships with the LibLogit standard library and the Code
Studio editor. See [docs/AETHERSCRIPT_LANGUAGE_3.md](docs/AETHERSCRIPT_LANGUAGE_3.md).

## What does not work

Kept deliberately, and kept specific.

- **No Media Source Extensions**, so a `<video>` on a real streaming site does not play: the decoders exist, but a
  page cannot feed them separate fMP4 segments.
- **Large single-page applications do not load acceptably.** QuickJS compiles eagerly with no pre-parse; a site
  shipping 12.77 MB of JavaScript across 134 files fits in memory, but its compile cost has not been made
  acceptable.
- **HEVC B slices are not bit-exact**, at either bit depth, and HEVC scaling lists are not bit-exact either. Treat
  "HEVC works" as a claim about I/P material.
- **SMAP is off.** The sites that must route through `user_copy_*` first are enumerated, not yet converted.
- **AetherScript depends on the host LLVM** and is not fully self-hosted.
- **QEMU is the primary verification environment.** Real-hardware bring-up exists for specific machines (see the
  dated reports in `docs/`), but hardware support is limited.
- **The browser has no JavaScript security sandbox** and must not be used with hostile content or real
  credentials.

## Build & run

The quickest way to try LogitOS is the [v0.1.0-alpha release](https://github.com/cai114514114514/LogitOS/releases/tag/v0.1.0-alpha):

```sh
gunzip logitos-v0.1.0-alpha-release.img.gz
qemu-system-x86_64 -cdrom logitos-v0.1.0-alpha.iso \
  -drive file=logitos-v0.1.0-alpha-release.img,format=raw,if=none,id=hd0 \
  -device virtio-blk-pci,drive=hd0 -boot d -m 1G -smp 4 -cpu max \
  -vga none -device virtio-gpu-pci,xres=1280,yres=800 -serial stdio
```

(Add `-accel tcg,thread=multi` on Apple Silicon, or `-accel kvm` on an x86_64 Linux host. The release ISO boots on
BIOS; UEFI uses the separate ESP image built by `make run-uefi`.)

### From source

The Makefile needs a POSIX shell and Unix utilities; on Windows, build inside WSL. Required tools: LLVM/Clang
and LLD, NASM, Rust (rustup with the `x86_64-unknown-none` target), Python 3, GNU Make and QEMU. GRUB and
`xorriso` are no longer needed — the ISO is written by `tools/mkiso.py`. A normal build reads no host fonts and
uses no network.

```sh
make                 # build build/logit.iso
make run             # boot in QEMU (-smp 4, virtio GPU/disk + e1000); desktop window, serial on stdio
make build/disk.img  # rebuild the development disk image after changing apps / fonts / fsroot
make release-disk    # build/release.img: the release disk, without test paths
make run-uefi        # boot through the UEFI loader
make shot            # headless boot + QMP screendump to build/desktop.png
make debug           # boot frozen with a gdb stub on localhost:1234
make clean
```

For parallel builds, compiler caching and incremental-build checks see [docs/BUILDING.md](docs/BUILDING.md).
GNU Make 4.4+ parallelises ordinary product builds automatically; older versions accept `-j12`.

### Tests

Tests are headless and asserted over the serial line unless noted. A selection:

```sh
make ci                # clean-clone build of HEAD, run the suites, assert the artifacts exist
make test-audit        # fail on any harness that cannot fail
make test              # boot smoke test (LOGIT_BOOT_OK)
make test-shell        # fork/exec, pipes and coreutils through /bin/sh
make test-smp          # -smp 4, genuine cross-core parallelism
make test-sec          # /bin/secprobe: one attack per process
make test-durability   # five real boots, no -snapshot, files verified byte for byte
make test-swap         # reclaim and swap on a deliberately small machine
make test-net          # TCP/IP protocol unit tests (host)
make test-tls-interop  # the TLS client against a real openssl s_server
make test-browser      # DOM, CSS, layout, JS and paint, host-side
make test-html5lib     # HTML tree construction against the shared corpus (a ratcheted rate)
make test-h264         # byte identity with ffmpeg, including a real 1080p stream
make test-video        # on device: the guest's CRC must equal the host's
make test-aui-host     # the widget toolkit rendered on the host against golden images
make test-repaint      # compositor repaint and damage behaviour
make test-agent        # agent broker, state, storage and IPC
make test-textedit-work  # the native TextEdit review workflow (deterministic model)
```

Many tests have a negative control — the same test against a build with the fix compiled out, which is **required
to fail**. An assertion nobody has watched fail is not a known-failing assertion.

**Running the agent with a real model.** Put a DeepSeek key in `.env` (`deepseek_api=…`; it never leaves the
host), start the gateway and point the workflow driver at its state directory:

```sh
python3 tools/agent_gateway.py --env .env --check          # reports whether a key is present; prints no values
python3 tools/agent_gateway.py --env .env --state /tmp/gw &
python3 tests/boot/run-textedit-work.py --build build --gateway /tmp/gw --out /tmp/real-run
```

## Source layout

```text
c/boot/bios/, c/boot/efi/     our BIOS and UEFI loaders; c/boot/ holds long-mode entry, ISR stubs, ring-3 entry
c/kernel/                     core, cpu (ACPI, SMP), mm (physmap, pmm, reclaim, swap), sched, exec, gui (compositor
                              and GUI syscalls), pci, audio, diag
c/drivers/                    block, char, net, usb, gpu, virtio, audio, timer, power, platform
c/fs/                         VFS, LogitFS (journalled), procfs, ramfs, caches, fsck
c/net/, c/crypto/             link/ip/transport/dns/http/tls/ssh · hashes, AEAD, public-key, post-quantum, trust store
c/lib/                        gfx, gfx3d, text, image, video, audio, media, nn (inference), ime, wasm
c/apps/                       ring-3 apps and services; gui/ (Finder, TextEdit, Terminal, Studio, Browser, …, and the
                              aui toolkit), as/ (AetherScript), libc/, coreutils/, agent/, services/
include/abi/                  the app/syscall ABI between the kernel and userland
third_party/                  vendored and adapted code: QuickJS, NetSurf LibCSS, musl libm, TCC, uACPI, fonts
tools/                        mkiso, mkfs, mkaex, font and asset generators, the agent gateway, QMP drivers, audits
fsroot/                       files packed into the disk image
tests/                        host unit tests, QEMU/QMP drivers, fixtures and the make fragments that wire them
docs/                         design notes, references and dated engineering reports
```

See [docs/SOURCE_LAYOUT.md](docs/SOURCE_LAYOUT.md) for the full map.

## Toolchain notes

- **Compile:** `clang --target=x86_64-elf -ffreestanding`. **Link:** `ld.lld` (Apple's `ld` only emits Mach-O).
- **Assemble:** `nasm -f elf64` for the kernel, `nasm -f bin` for the BIOS loader.
- The kernel loads at 32 MiB and is entered in 64-bit long mode at `logit_native_start`. SSE is enabled at boot,
  so the kernel and userland use hardware floating point.

## Documentation

- [Building](docs/BUILDING.md) · [Source layout](docs/SOURCE_LAYOUT.md) · [Network support](docs/NETWORK.md)
- [AEX v3 executable format](docs/AEX_V3.md) · [App capabilities](docs/AEX_APP_CAPABILITIES.md)
- [AetherScript 3](docs/AETHERSCRIPT_LANGUAGE_3.md) · [OpenLogit SDK](docs/OPENLOGIT_SDK_IMPLEMENTATION.md)
- [Contributing](CONTRIBUTING.md) · [Releasing](RELEASING.md) · [Security](SECURITY.md) ·
  [Transparency](TRANSPARENCY.md) · [Third-party software](THIRD_PARTY.md)

A reorganised, subsystem-by-subsystem documentation set is being written.

## A note on claims

"Project implementation" means the implementation is maintained here rather than taken from Linux or another OS.
It does not mean standards completeness, production readiness, absence of AI authorship, or absence of influence
from standards, books and prior systems. Passing a test demonstrates a behaviour in the named environment; it does
not establish full conformance, hardware support or security. The definitions behind each claim are in
[TRANSPARENCY.md](TRANSPARENCY.md); binary distributors should follow [RELEASING.md](RELEASING.md).

## License

LogitOS is a multi-license project. The project-authored operating-system core — kernel, boot code, drivers,
filesystems, network stack, TLS, cryptography and kernel-only support libraries — is `GPL-3.0-or-later`.
Project-authored applications, the public userspace ABI, shared Rust parser code, build tools, tests,
documentation and examples are `MIT`. Code and data listed in [THIRD_PARTY.md](THIRD_PARTY.md) keep their upstream
licenses, so the bootable ISO is a multi-license aggregate. See [LICENSE](LICENSE) and [LICENSING.md](LICENSING.md)
for the exact boundaries.
