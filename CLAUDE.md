# LogitOS OS — notes for Claude

A from-scratch x86_64 OS kernel (C + nasm), booted via GRUB/Multiboot2, aiming
toward a macOS-style desktop. Real kernel, not a simulation.

## Build / run / test

```sh
make        # -> build/logit.iso        (the KERNEL only -- see below)
make run    # QEMU: VGA window + serial on terminal
make test   # headless; asserts kernel prints LOGIT_BOOT_OK on serial
make debug  # QEMU frozen with gdb stub on :1234
```

**`make` alone does NOT rebuild a ring-3 program.** `all: $(ISO)` and
`$(ISO): $(KERNEL) grub.cfg` -- every app is an `.aex` on `$(DISK)`, which is a
separate target that only `run` and the boot harnesses depend on. So editing
`c/apps/browser/*.c` and running `make` prints "Nothing to be done for 'all'"
and leaves a disk image with the old binary on it, which reads as "it still
builds" and is not that. `make build/disk.img` (or any `make test-*` that needs
the disk) is the check. Nothing is broken here -- the ISO genuinely does not
contain the apps -- but "I ran make and it was fine" is not evidence about
anything above the kernel.

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
  positives unless `.clangd` is being honored — the real build passes `-I` for
  every source dir (`INCDIRS` in the Makefile) and the x86_64 target.

## When a test fails, suspect the apparatus first — measured, 2026-08-17

A sweep of all 522 targets produced 45 failures. **Most were the test, not the
system**, and the expensive ones shared a shape worth naming:

> **The measurement was right and the sentence around it sent the reader
> somewhere else.**

Eight instances in one day, and they did not look alike from the outside:

| it said | it was |
|---|---|
| "isolated forms are about half again as wide as joined ones", 8 failures about Arabic | a screenshot of Preview playing an audio file — every row read `444 px` regardless of input length |
| `fresnel s=0: got 190, double says 255`, 26 failures | the oracle did not model a clamp the implementation argues for over twenty lines |
| "something is still polling instead of blocking" | ~50 passes per core against a budget of 200 that does not mention cores |
| a `TypeError` traceback in the harness | the harness had found a real bug one line earlier and then walked off the end of it |
| `undefined reference to gfx_path_ellipse` in three controls | the sentence explaining it was already a comment seven lines above one of them |
| `0 px wrong` and failing | `rel=mismatch` — zero is the WORST outcome, printed in the words of the best |
| `test-swap` green | the workload completes without swap; an optimisation shrank the desktop and the calibration went hollow |
| "there is no reftest harness here, so LAYOUT is UNMEASURED" | `test-reftest` judged 24,300 reftests that day |

**And the same thing happened to the tools used to investigate them**, which is
the part worth internalising. An `md5sum` "matched" and let a real breakage ship,
because `grep -m1` picked a different rule's first line. A shell check reported a
variable missing that was on the continuation line. Two Makefile parsers dropped
rules whose prerequisites wrap. A patch-anchor checker reported false failures
because it regex-scraped `\n` instead of evaluating the string. A syntax checker
reported an undeclared symbol because it put the unpatched header first on the
include path.

Nobody was careless in any of these. The lesson is narrower and more useful:
**before believing a failure, check that the thing reporting it is looking at
what you think it is.** Cheap tests for that: does the number change when the
input changes? Does the control fire? Does the same gate at another size or core
count agree? Is the file it read the file you edited?

## If you write anything that reads the Makefile, join the continuations first

There are five tools in this tree that parse make: `tools/audit_tests.py`,
`tools/negctl_drift.py`, `tests/boot/sweep-classify.py`, and two throwaway shell
checks that turned into decisions. **Four of the five read one PHYSICAL line at
a time, and all four were wrong**, found in a single day (2026-08-17):

- an `md5sum` of `make -n build/disk.img | grep -m1 mkfs.py` matched before and
  after a change, which is what convinced me the change was safe. It was
  comparing the first of several `mkfs` invocations, on a line the change never
  touched — and the change had made every app's host path empty. It shipped.
- a shell check reported `browser-nofocus.elf` as missing `$(GFX_OBJ)`. It has
  it, on the continuation line.
- `negctl_drift.py`'s parser lost any rule whose prerequisites wrap, because the
  second line starts with spaces, matches no target pattern and clears the
  current target — so the rule's recipe was dropped and the target never entered
  the table. It reported three browser variants where there are four, and the
  missing one was the variant that started the investigation.
- `audit_tests.py` took `line.split(":", 1)[1]` as the prerequisite list, so
  `test-fs-boot: a b c \` lost everything after the backslash. `test-fs-boot` is
  one of that file's own suite roots, so its wrapped members were reported as
  reachable by nothing. **The count the audit exists to produce was inflated by
  its own parser: 22 wired became 31, and 359 unwired became 354.**

`re.sub(r"\\\n[ \t]*", " ", text)` before anything else. Or read `make -pRrq`
instead, whose output make has already joined — verified, not assumed, which is
why `sweep-classify.py` is the one that was fine.

## Source layout

All source lives under `c/`, headers **colocated** with their `.c`. The
Makefile's `INCDIRS` is one flat list built from `find c include -type d`, so
every `#include "foo.h"` resolves without a path qualifier. `include/` keeps
only the cross-cutting kernel↔user ABI (`include/abi/logit_abi.h`).

**That flat list assumes header basenames are unique, and they are not.** The
assumption has been broken twice, both times by mini-libc growing a POSIX header
whose name the kernel already used, and both times the symptom was the same and
badly misleading: the list is sorted, `c/apps/libc/include` sorts before
`c/kernel/...`, so **kernel** files including `"foo.h"` silently got the
**userland** one and failed on undeclared kernel functions in files nobody had
edited. A clean clone was immune while the header stayed untracked, which is how
both survived a while.

- `sys/wait.h` — fixed by excluding one directory:
  `INCDIRS := $(addprefix -I,$(filter-out %/include/sys,$(sort $(shell find ...))))`
- `sched.h` — fixed by moving it to `c/apps/libc/include/uonly/`, excluding that
  directory from the shared scan, and adding `-Ic/apps/libc/include/uonly` to
  **`UCFLAGS` only**. Note the ordering trap: it must come **before**
  `$(INCDIRS)`, or the kernel's header wins anyway.

`uonly/` is the place for a userland header whose basename the kernel also uses.
Before adding a header to `c/apps/libc/include`, check its basename against
`c/kernel`, `c/drivers`, `c/net`, `c/fs` and `c/lib` — a collision does not fail
at the collision, it fails somewhere else entirely.

Tests live under **`tests/`** (moved out of `tools/` in the 2026-06-09 declutter):
`tests/unit/` = host unit/fuzz tests (`make test-tcp-host`/`test-as`/`test-png`/…)
+ `tcpstub/` + generators; `tests/boot/` = QEMU boot harnesses (`run-*.sh`, driven by
`make test`/`test-nvme`/`test-shell`/`test-libc`/`test-smp`); `tests/qmp/` = QMP
mouse/keyboard/screenshot drivers. `tools/` is now **build tools only** (mkaex,
mkfs, mkfont, genroots, gen_compile_commands, gen_libcss, mkwallpaper).

```
c/boot/                                        multiboot + long-mode entry (asm)
c/kernel/{core,cpu,mm,sched,exec,gui,pci,audio}/  kernel by subsystem
c/drivers/{char,timer,block,net,usb,virtio,audio,core}/  device drivers
c/fs/                                          vfs + logitfs
c/net/{link,ip,transport,core,dns,http,tls}/   network stack
c/crypto/{hash,aead,kdf,pubkey,trust}/         from-scratch crypto
c/lib/{image,text,gfx,audio,video,media}/ + string.c   shared libs, ring 3
c/apps/                                        shared: logit.h clib.h crt0.asm crt0_cli.asm
c/apps/gui/                                     windowed apps: clock textedit monitor terminal
                                                 files preview studio gallery settings widgets greeter
                                                 + aui.{h,c} = immediate-mode widget toolkit (linked into each)
c/apps/coreutils/                              sh + coreutils (ls cat echo wc head login httpd …)
c/apps/as/                                     AetherScript language (M20): /bin/as
c/apps/browser/                                browser + render engine (dom, layout,
                                                 css_engine, browser_paint, js_dom) — also links QuickJS
c/apps/libc/                                   mini-libc (string/stdio/malloc/setjmp…)
```

**Two things in that tree are misplaced, and they are named here rather than
moved because moving them touches a dozen build rules across the Makefile and
five `tests/*.mk` fragments — a bigger edit than the mess is worth while other
lines are live. Do not add to either.**

- **`c/apps/{audio,media,net,video}/` are not applications.** Each holds one or
  two `*check.c` programs — `audiocheck`, `demuxcheck`, `msecheck`, `h2check`,
  `vidcheck`, `vidcheck265` — whose only callers are `tests/*.mk`. They are
  **on-device test harnesses that happen to be built as `.aex`**, and they live
  in `c/apps/` for the mechanical reason that `.aex` rules did. A new one
  belongs beside them only until somebody moves the four directories under
  `tests/`; a new *application* does not belong there at all.
- **`c/lib/` is ring 3, not shared-with-the-kernel.** `c/lib/video` is filtered
  out of `C_SRC` on purpose (see the H.264 note below), `c/lib/gfx` likewise.
  The name suggests a kernel library and it is not one.

`c/apps/libc/` is `src/` + `include/` and reports as empty to anything that
looks only at `c/apps/libc/*.c`; it is 11.8k lines one level down (2026-08-17).

**File paths quoted in the Notes below are pre-reorg names** (e.g. `net/tcp.c` is
now `c/net/transport/tcp.c`, `kernel/wm.c` → `c/kernel/gui/wm.c`); basename +
subsystem are unchanged, so they're easy to find under `c/`.

## Roadmap

M1 Boot & Hello ✅ · M2 interrupts + keyboard ✅ · M3 memory (PMM + heap) ✅ ·
M4 multitasking (preemptive scheduler) ✅ · M5 storage (ATA + LogitFS + VFS) ✅ ·
M6 userland (GDT/TSS + ring3 + int 0x80 + ELF loader) ✅ · M7 graphics
(framebuffer + VMM + LogitOS desktop) ✅ · M8 window system (font + double-buffer +
PS/2 mouse + draggable windows) ✅ · M9 networking (PCI + e1000 + ARP/IPv4/
ICMP/UDP + DNS + Network app) ✅.

Browser arc: M10 TCP ✅ (`net/tcp.c`, client byte stream) · M11 HTTP + Browser
app ✅ (`net/http.c` fetch + `net/html.c` de-tag render + `user/browser.c`) ·
M12 TLS 1.3 ✅ (`crypto/*` + `net/tls.c` + `net/x509.c`; https with strict cert
verification) · M13 HTML/CSS layout ✅ (`net/dom.c` + `net/css.c` + `net/layout.c`
+ `net/paint.c` + `lib/{inflate,png,gif,img}.c`: DOM + CSS cascade → flat display
list → painted viewport with clickable links + images; `user/browser.c` renders
real pages incl. https://en.wikipedia.org).

M14 Unicode + from-scratch TrueType anti-aliased text ✅ (`lib/utf8.c` +
`lib/ttf.c` + `kernel/raster.c` — **since deleted, see Open Logit below: glyphs
are rasterized by the engine now, through `c/lib/text/glyphras.c`** — +
`kernel/text.c`): UTF-8 decode, a from-scratch
TTF parser (cmap fmt4/12, glyf simple+composite, hmtx) and an integer-only AA
rasterizer (4× vertical oversample + fractional horizontal coverage → 0–255
alpha), with a glyph cache + font fallback. `fb_text` routes through it, so the
whole UI is anti-aliased; the Terminal uses `text_draw_mono` (SYS_GUI_TEXT_MONO).
Fonts live on the LogitFS disk (`/fonts/{ui,mono}.ttf`, subset by
`tools/mkfont.py` from vendored OFL Noto Sans SC + Noto Sans Mono sources,
glyf), loaded by `text_init()` after fs mount; QEMU `-m 512M`. The CJK font is
about 2.2 MB, and LogitFS supports **double-
indirect** inodes (`fs/logitfs.c` + `tools/mkfs.py`; files >4 MB). The 8×16
bitmap font (`font8x16.h`, `genfont.py`) was removed. Chinese web pages render
(`zh.wikipedia.org`). Notes: no hinting (macOS-style), grayscale (no subpixel),
no bidi/shaping; the rasterizer is integer-only because the kernel is `-mno-sse`.

Post-roadmap: per-process address spaces (each app its own PML4; `vmm_new_space`,
`schedule()` switches CR3) and ring-3 fault containment (an app fault kills only
that app; the kernel/desktop survive — `kernel/interrupts.c`).

Key notes:
- `vmm_map_page` does a real 4-level walk; maps the high-MMIO framebuffer and
  user pages. Intermediate table entries carry USER; leaf PTE flags protect
  kernel pages. User images link at 1 GiB (above the identity huge-page region).
- Disk / filesystem: see the **Storage** section below. (This bullet used to
  describe an ATA-only, v3, no-journal LogitFS; all three are out of date.)
- Userland: `kernel/gdt.c` (TSS rsp0), `boot/enter_user.asm`, syscalls via
  int 0x80 in `kernel/syscall.c`; `user/` builds the ring-3 ELF.
- M8 window system: `tools/genfont.py` -> `include/font8x16.h` (committed, no PIL
  at build). `kernel/fb.c` draws into surfaces (`fb_target`/`fb_blit_surface`)
  with a screen back buffer + `fb_present()`. `drivers/mouse.c` = PS/2 mouse
  (IRQ12); `drivers/rtc.c` = CMOS wall clock (menu bar + Clock app).
- M9 networking: `kernel/pci.c` (0xCF8/0xCFC config) + `drivers/e1000.c` (QEMU
  e1000 8086:100E, MMIO BAR, RX/TX descriptor rings). **THE "polled -- no NIC
  IRQ" THAT USED TO END THIS LINE IS FALSE**, and it was load-bearing: on
  2026-08-17 it produced a whole diagnosis of the network's throughput as
  "gated on the compositor's frame rate", because the only steady-state caller
  of `net_poll()` is `wm.c:5411` inside the WM loop. `c/drivers/net/e1000.c:12`
  says the opposite in as many words -- *"IRQ-driven receive on RXT0 with
  net_poll() as the backstop"* -- so the WM loop is the backstop, not the pump,
  and the theory built on this sentence was dead on arrival.

  The arithmetic that went with it is worth recording as a warning too:
  `RX_DESC (64) x TIMER_HZ (100)` = 74.8 Mbit/s was offered as the cap against a
  MEASURED 269.9 Mbit/s -- 3.6x above a supposed ceiling, which should have
  killed the theory by itself. Nothing in this tree ties `e1000_rx_drain` to the
  timer tick; the two numbers never meet.
  `net/` = eth/arp/ip/icmp/udp/dns; `net_poll()` is pumped from the WM loop.
  Static IP (QEMU SLIRP: 10.0.2.15/24, gw 10.0.2.2, DNS 10.0.2.3) in
  `net_cfg` (DHCP hook later). Run/test attach `-netdev user -device e1000`
  (+ `filter-dump` pcap for `make run`). e1000 gotcha: QEMU's `set_rx_control`
  defers the RX-queue flush ~1s, so RX needs a real time base to observe.
- M10 TCP (`net/tcp.c`): client byte stream over IP proto 6 (IP_PROTO_TCP via
  ip_input's weak hook); tcp_connect/send/recv/close; single outstanding seg +
  timeout retransmit. **M26 robustness:** receive now does **out-of-order
  reassembly** (a sorted interval set over a seq-indexed 64 KiB ring, NOOO=16),
  so a reordered/lost segment mid-flight no longer discards the rest -- large TLS
  handshake flights arrive reliably (the old strict-in-order drop was the
  "sectigo fails" cause). `tcp_send` segments payloads > MSS instead of
  truncating. Still no window-scaling/congestion-control/SACK (perf, deferred).
  Reassembly is unit-tested host-side (`make test-tcp-host`, 26 checks incl. a
  32 KiB flight delivered in reverse + seq wraparound). NCONN=8; a closed
  connection's slot is freed promptly (FIN_WAIT -> CLOSED on the peer's FIN, plus
  a `tcp_poll` backstop) -- the old code leaked slots in FIN_WAIT/TIME_WAIT,
  which a multi-connection page (e.g. a redirect) exhausted.
- M11 HTTP (`net/http.c` + `net/url.c` + `net/html.c`): http_get(url) does
  DNS+TCP+GET synchronously and html_render strips tags/decodes entities/extracts
  `<a>` links; `user/browser.c` is the GUI. **Gotcha:** blocking net calls
  (http_get, dns_resolve) pump net_poll and need IF=1, but int 0x80 is an
  interrupt gate (clears IF) — so SYS_HTTP_GET re-enables interrupts around the
  fetch (see `wm_gui_syscall`), else the PIT-based timeout loop spins forever.
  http_get now branches on https (see M12), and **follows 3xx redirects** (up to
  5 hops, `Location:` via `url_resolve`) -- so https://google.com lands on
  https://www.google.com like a real browser. **Stack note:** the TLS path is
  stack-heavy (handshake + cert/RSA verify + `aead_seal`); that 16 KiB plain
  buffer is now `static`, and the boot stack + thread kstack are 32 KiB. With the
  old 16 KiB stack the deeper redirect path overflowed *into the page tables*
  (`stack_bottom` sits just above `pd_table` in boot.asm) -> silent hang.
- M12 TLS 1.3 (`crypto/*` + `net/tls.c` + `net/x509.c`): from-scratch crypto --
  SHA-256/384, HMAC/HKDF (`hmac_hkdf.c`), ChaCha20-Poly1305 + AES-128-GCM,
  X25519, EC P-256/P-384 + ECDSA-verify (`ecdsa.c`, Jacobian coords). `net/tls.c`
  is a TLS 1.3 client (X25519 KX, both AEADs, SHA-256 transcript, HKDF schedule);
  `net/x509.c` does DER/X.509 parse + **strict** chain verification to built-in
  roots (`crypto/roots.c`). http_get's https branch layers tls_connect over the
  TCP socket. Each primitive is verified against published/openssl vectors. Only
  TLS 1.3 + the two suites above; no resumption/0-RTT/client-certs. Like all
  blocking net ops, tls_connect needs IF=1 (SYS_HTTP_GET re-enables it).
- M12.5 RSA + real CA bundle (the "open most of the web" follow-up): `crypto/rsa.c`
  adds from-scratch RSA verify -- bignum modexp (double-and-add modmul, no wide
  product; 4096-bit modulus, e=65537) + **PKCS#1 v1.5** (cert-chain sigs) and
  **RSA-PSS** (TLS 1.3 CertificateVerify for RSA leaves; MGF1 + EMSA-PSS).
  `net/x509.c` now parses RSA SPKI + rsaWithSHA256/384 and dispatches EC/RSA in
  both `x509_verify_signed_by` and the new `signed_by_root` (top cert trusted if
  its issuer is a held root, not only if the root is sent in-band). Trust store
  is a **130-root bundle** (`tools/roots/*.pem` -> `tools/genroots.py` ->
  `c/crypto/trust/roots_bundle.inc`) -- a near-full mirror of the host
  Mozilla/NSS store (`/etc/ssl/cert.pem`), so the browser trusts essentially the
  whole public web. `genroots.py` globs `tools/roots/*.pem`, extracts just each
  root's SPKI (RSA n,e or EC P-256/P-384 point), and **skips** any key type the
  kernel can't verify (P-521/Ed25519); `rsa.c` modexp is exponent-generic so
  roots with e=3 / e=43147 (old GoDaddy/Starfield/NetLock) verify too. To add
  roots: drop authentic PEMs in `tools/roots/` and re-run `genroots.py`. (Each
  added pubkey is cross-checked vs `openssl` modulus / SPKI point.) `now` for validity
  comes from the **RTC** (`net/http.c now_unix`), not a hardcoded constant.
  Verified on host against 5 real chains (example/google/github/wikipedia/kernel.org,
  EC+RSA, in-band & signed-by-root, tamper/wrong-host rejected) and end-to-end in
  QEMU (Browser opens https://google.com). **Gotcha:** roots.c #includes the
  generated bundle, so the Makefile gives roots.o an explicit dep on
  roots_bundle.inc -- regenerating the bundle without that dep silently keeps the
  old roots in the kernel. No RSA *key exchange* (TLS 1.3 has none); RSA leaves
  use ECDHE so only their CertVerify is PSS.
- Algorithm coverage completed: `crypto/rsa.c` `rsa_pss_verify` recovers the salt
  length from the structure (works for any salt, not just =hLen), and full
  **SHA-512** (`crypto/sha384.c sha512*`) is wired through. `net/x509.c` now also
  parses **rsassaPss** cert signatures (hash read from the params) and
  **sha512/ecdsa-with-SHA512**, with SIG_RSA_PSS_SHA256/384/512, SIG_RSA_SHA512,
  SIG_ECDSA_SHA512. ClientHello advertises rsa_pss_rsae_sha256/384/512. D-TRUST
  Root Class 3 CA 2 2009 (a SHA-512 chain anchor) is one of the 130 roots. Verified host-side
  against 8 real chains (incl. bsi.bund.de SHA-512, sectigo SHA-384) + synthetic
  PSS/SHA-512 certs vs openssl; in QEMU bsi.bund.de (SHA-512 + RSA-PSS
  CertVerify) opens. **Former limitation, now fixed (M26):** sites with large
  multi-cert RSA flights (e.g. sectigo, 4 certs incl. a 4096-bit CA) used to fail
  with TLS_E_PROTO because the M10 TCP dropped any out-of-order segment; the M26
  TCP reassembly (see M10 note above) receives the bigger handshake flight
  reliably. (A separate cap remains: the whole flight must fit the 64 KiB window
  -- true for all real chains.)

**The AES and SHA-2 families, completed** (`c/crypto/aead/aes_modes.c` is new;
`aesgcm.c`, `sha256.c`, `sha384.c`, `hmac_hkdf.c`, `pbkdf2.c` grew). TLS needs
AES-128/256-GCM with a 96-bit IV and SHA-256/384, and that is all this tree had
-- so every family had a hole in the middle, and a hole is where a caller
discovers the primitive is missing at the moment it needs it. Now: **AES-192-GCM**
(the FIPS-197 middle key size), **GCM with any IV length 1..1024** (SP 800-38D
5.2.1.1's J0 = GHASH construction; a 96-bit IV through these is byte-identical
to the fixed path, pinned by test), **AES-CTR** and **AES-CBC/PKCS#7**,
**SHA-512/224**, **SHA-512/256**, **SHA-224**, and HMAC/HKDF/PBKDF2 at every one
of those widths (28/32/48/64). `tls12_prf` and `hkdf_expand_label` deliberately
stay at 32/48 -- TLS names no other width, and widening them would invent a
protocol.

Three things about it are worth knowing before touching it:
- **The mode never lives in a backend.** `struct aes_backend` grew a fourth
  primitive (block *decrypt*, CBC's only customer) rather than letting
  `aes_modes.c` hide an inverse cipher: "the backend does the primitive, the
  mode is written once" is the invariant the differential rests on, and an
  implementation outside the table would sit exactly where the differential
  cannot see it. `crypto_simd_selftest` now checks all four primitives at all
  three key lengths, and checks decrypt as a round-trip against *each* backend
  independently -- two backends that made the same equivalent-inverse-schedule
  mistake (the AESIMC trap) would otherwise agree while decrypting garbage.
- **CTR's counter is not GCM's.** SP 800-38A increments the whole 128-bit block
  with carry; GCM's inc32 wraps only the low four bytes. The two agree until a
  counter block ends in ffffffff, at which point GCM's rule replays a keystream
  block. The ffffffff-edge vectors exist for that one difference.
- **CBC's padding check is a constant-time accumulate over all 16 candidates**,
  not the byte-by-byte early exit, which leaks the pad length and therefore the
  plaintext length -- a real oracle when one key encrypts many records.

The gate is 140,214 differential cases against hashlib/OpenSSL
(`make test-crypto-diff`), every AES vector replayed through **both** backends,
plus 291 known answers in the fast gate (`make test-crypto`, which `make test`
runs). SHA-224 alone is 1,815 of the differential cases, and corrupting one word
of its IV fails exactly those 1,815 and no others -- which is the control, run.
One booby trap was defused on the way in: the "unsupported width is a silent
no-op" assertion used **28** as its witness, so making SHA-224 legal turned a
test that proved a refusal into a test that proved nothing. It is 20 now
(SHA-1's length -- `sha1.c` is in the tree and HMAC still does not dispatch to
it, which is the property actually being pinned).

## Application platform (on top of M8)
- Apps are `.aex` files on the LogitFS disk = real **ring-3 processes** scheduled
  by M4. `kernel/wm.c` is the window manager AND the GUI/app backend.
- Executable format: `include/aex.h` (header wrapping an ELF), built by
  `tools/mkaex.py`; loaded by `kernel/aex.c` (reuses `elf_load`). Each app links
  at a distinct base (Makefile APP_RULE: clock 0x40000000, textedit 0x41000000,
  monitor 0x42000000, terminal 0x43000000). Single instance per app.
- Process model: `thread_create_user` (sched.c) spawns a ring-3 thread with its
  own kernel stack; `schedule()` sets TSS rsp0; `thread_exit()` reaps it.
  `sched_current_data()` maps the running thread to its `struct app`.
- ABI: `include/logit_abi.h` (shared with userland `user/logit.h`). Syscalls via
  int 0x80: GUI create/clear/rect/text/flush, poll_event (key/mouse/close),
  get_arg, get_time, read_file, write_file/delete_file, mkdir,
  dir_count/dir_name (path-scoped listing), net_info/net_ping/net_dns (+ result
  pollers), http_get/http_status/http_read/http_link, yield, sysinfo,
  file_count/file_name (root), exit. read/write/delete/mkdir take paths.
  `syscall.c` routes GUI calls to `wm_gui_syscall()` in the app's context.
  Finder is a directory browser (cwd, folders, `..`); Terminal has
  cd/pwd/mkdir/ls + cat/touch/rm/echo>; TextEdit saves with Ctrl+S; Network app
  pings the gateway + resolves a host (DNS); Browser loads http:// pages
  (address bar, de-tagged text, clickable links). PS/2 keyboard supports
  Ctrl + Shift.
- WM: dynamic windows + per-window surfaces composited each frame; app registry
  scanned from *.aex; Dock launches apps; Finder opens a file with the app whose
  `ext` matches (file association); red close button -> EV_CLOSE -> app exits.
- Adding an app: write `user/<name>.c` (include "logit.h", define `app_main`),
  add an `APP_RULE` line + the name to `APPS` in the Makefile.
- `tests/qmp/qmp_*.py` drive mouse/keyboard over QEMU QMP for screenshots/CI.
M15 SSE2/FPU ✅ (JS-engine prerequisite): the kernel was integer-only because
x86-64 SysV passes `double` in XMM, so `-mno-sse` made FP unusable. Now
`boot/long.asm` enables SSE at boot (clear CR0.EM, set CR0.MP/NE + CR4.OSFXSR/
OSXMMEXCPT, `fninit` + default MXCSR), the Makefile builds kernel **and** userland
with `-msse -msse2` (kept `-mno-red-zone`), and `boot/isr.asm`'s `isr_common`
does FXSAVE/FXRSTOR around every C handler so FP survives preemption + syscalls
(context_switch needs nothing: SysV XMM are all caller-saved). No lazy-FP
(CR0.TS). Verified: kernel + ring-3 double math exact under heavy timer
preemption (0 corruptions).

M16 JavaScript (QuickJS) ✅: ported QuickJS 2024-01-13 as a **ring-3 app**
(`user/js.c`). Needed a from-scratch freestanding userland C runtime:
**mini-libc** (`user/libc/`: 24 MiB-arena malloc, string/mem, a from-scratch
`vsnprintf` incl. correct `%e/%f/%g`, stdlib `qsort`/`strtod`/`strtoll`,
`fenv`→MXCSR rounding, `time`→RTC syscall, 128-bit `__udivti3` compiler-rt) +
**musl libm** (`third_party/libm/`, double-only subset, 83 files) + the engine
(`third_party/quickjs/`: quickjs+cutils+libregexp+libunicode+libbf). Atomics are
off (`-DLOGIT_OS` guards `CONFIG_ATOMICS`; single-threaded). Builds via the
Makefile `js` rule into a ~1 MiB `.aex`; JS output goes to a window + serial
through `SYS_WRITE`. Runs fib, Array.map/arrow fns, JSON, Math.* (libm), etc.
**Gotchas:** QuickJS returns `double` so M15's SSE is mandatory (`-msse2`);
`js_dtoa` formats numbers via `snprintf("%+.*e")` so a correct `%e` is required;
`scan_apps` read each `.aex` into a 32 KiB buffer — fixed to size-to-file so the
1 MiB app registers in the Dock. **`user/browser.c` now links the engine too**
(Makefile shares `ENGINE_OBJ` between js.aex and browser.aex) and runs a page's
inline `<script>`: the kernel `collect_scripts` walks the DOM, `SYS_PAGE_SCRIPTS`
hands the concatenated source to the app, which `JS_Eval`s it; `console.log`
output shows in the status bar + serial. **No DOM bindings yet** (DOM lives in the
kernel, JS in ring-3 -- different address spaces). `dns_resolve` now accepts IP
literals ("10.0.2.2") so a `http://<ip>:port/` page works (tested via a host
http.server over SLIRP). Next: DOM bindings; CSS (`net/css.c`) may eventually move
to a third-party engine (HTML stays hand-rolled).

M17 LibCSS + render pipeline moved to ring-3 ✅: the whole HTML→DOM→CSS→layout→
paint pipeline now runs inside `browser.aex`; the kernel is just a primitive
provider (network fetch, font metrics, drawing). Four sub-steps (L1–L4):
- **L1** pipeline下放: new syscalls `SYS_HTTP_BODY` / `SYS_TEXT_MEASURE` /
  `SYS_GUI_TEXT_RUN` / `SYS_RES_FETCH` / `SYS_GUI_BLIT` / `SYS_GUI_CLIP` (36–41);
  `net/{dom,css,layout}.c` + image codecs compiled into the app, paint rewritten
  over `gui_*` (`user/browser_paint.c`), `<style>`/`<script>` collection + hit-test
  moved into the app. `SYS_HTTP_GET` is fetch-only; `SYS_PAGE_*` retired.
- **L2** NetSurf **LibCSS** replaces `net/css.c`: `third_party/css` (libwapcaplet +
  libparserutils + libcss, 319 .c) compiled into the browser; `user/css_engine.c`
  drives LibCSS off our DOM via a ~40-callback `css_select_handler` +
  `css_computed_style_compose`, reads `css_computed_*` into `struct cstyle` (so
  `net/layout.c` is unchanged). mini-libc gained `ctype.h`/`strings.h`; built with
  `-DWITHOUT_ICONV_FILTER -D_ALIGNED= -fcommon`. LibCSS codegen is **vendored**
  (autogen property parsers + `aliases.inc`; regenerate via `tools/gen_libcss.sh`).
- **L3** deeper HTML in `net/dom.c`: ~60 named entities, implied `<tbody>`, HTML5
  optional-end-tag auto-closing (li/td/th/tr/dd/dt/option/p).
- **L4** JS↔DOM bindings (`user/js_dom.c`): `document.getElementById`/`querySelector`/
  `body`, `Element.textContent`(get/set)/`innerHTML`/`tagName`/`id`/`getAttribute`/
  `setAttribute`; a mutation dirty-flag triggers re-style + re-layout + repaint.
  Works because L1 put the DOM and QuickJS in the same ring-3 address space.
  Verified host (`css_engine`/`dom`/`js_dom`/`layout`/`page` tests) + QEMU
  (example.com via LibCSS; a page's `<script>` rewriting textContent end-to-end).
  Gotchas: `<html>`'s synthetic `#document` parent must be reported as NULL to the
  select handler (else LibCSS won't treat `<html>` as root → font-size unresolved);
  host LibCSS unit tests need `-DCONFIG_BIGNUM` for libbf's decimal symbols.

M18 Real processes ✅ (the "toward a real OS" step: run software not written for
LogitOS). A POSIX-ish process model independent of the window manager:
`c/kernel/exec/{proc,file,exec}.c`. **proc.c** = a PCB table (pid/ppid/state/
cr3/fd[]/cwd); `thread->data` is a `struct proc*` (a GUI app is a proc whose
`->gui` owns a window). **fork** = `vmm_clone_user` (eager copy of the private
user subtree) + `thread_fork` building a child kstack that returns through
`fork_ret` (enter_user.asm) into ring 3 with rax=0; exit→zombie, waitpid/proc_reap
free the space (`vmm_free_space` -- also fixes a latent app-space leak). **fds**
(`file.c`): F_VFS (whole file in a kmalloc buffer + offset, flush on last close),
F_PIPE (ring buffer, EOF/EAGAIN via reader/writer refcounts, `O_NONBLOCK`), F_TTY
(serial console). **execve** (`exec.c`) replaces the user space in place + builds
a SysV argc/argv/envp stack. Userland: `crt0_cli.asm` (argc/argv→main), `clib.h`,
`/bin/sh` (pipes `|`, `< >` redirect, `&`, builtins, /bin PATH) + coreutils
(ls cat echo pwd wc head true false sleep mkdir rm touch clear uname). init:
`wm_run` proc_spawns `/bin/sh` on the serial console (fd 0/1/2 = tty); the GUI
**Terminal is now a terminal emulator** that fork+execve's the same `/bin/sh` over
two pipes. CI: `make test-shell` (`tests/boot/run-shell-test.sh`). **Gotchas:** CLI
programs link at a *common* base **0x50000000** -- it must be inside the private
user region PML4[0]/PDPT[1] (0x40000000-0x7FFFFFFF); 0x10000000 would be shared
kernel low memory. `wm_launch` gives every GUI app fd 0/1/2 = tty so an app's
`pipe()`s get fds >=3 (else they collide with dup2 targets 0/1/2). **ATA made
robust**: under `-smp` TCG the AP framebuffer-present contends the device lock and
intermittently delayed IDE PIO past the old bounded poll -> nondeterministic
"file not found"; `drivers/block/ata.c` now retries the command 8x.
**The two logitfs issues this paragraph used to list as open are both CLOSED**
(the "corrupts after repeated non-snapshot boots; use `-snapshot`" line was
still here on 2026-08-08 and was wrong by then -- see Storage below for what
fixed it and what now proves it). `tests/qmp/qmp_term.py` drives the GUI
terminal; QMP key injection must be slow (PS/2 1-byte buffer).

M19 virtio ✅ (the "VGA is too primitive" follow-up): a modern (virtio 1.0)
paravirtual device stack in `c/drivers/virtio/` replacing the legacy devices.
`virtio.c` is the virtio-pci transport (parses the vendor-0x09 PCI caps to find
the modern cfg structures in BAR4, negotiates VIRTIO_F_VERSION_1, sets up split
virtqueues, synchronous request/poll). **virtio-blk** (`virtio_blk.c` +
`drivers/block/blkdev.c`) replaces ATA PIO as the disk (logitfs bread/bwrite go
through blkdev; ATA is the fallback). **virtio-gpu** (`virtio_gpu.c`) replaces the
uncached-VGA-MMIO framebuffer: a RAM-backed 2D scanout resource, present =
TRANSFER_TO_HOST_2D + RESOURCE_FLUSH (a DMA, not a per-pixel CPU copy -- the old
lag root); `fb.c` prefers it, falls back to the multiboot LFB. Both poll with
interrupts ON but non-preemptible (`g_virtio_busy`/`ata_busy`, see interrupts.c)
since completions run on QEMU's IO thread. The multiboot2 framebuffer tag is now
optional so GRUB boots with `-vga none`; QEMU uses `-vga none -device
virtio-gpu-pci` + `virtio-blk-pci`. (Post-M18 fixes also landed: Finder list
clipping, runtime-mkdir clean dirs, the ATA IF-on/non-preempt fix, inode_trunc
double-indirect free.)

**Two more devices ride the same transport now, and neither needed a Makefile
change to build** -- `C_SRC` globs `c/drivers`, and a driver registers itself
through the device model's linker section (`DRIVER_DECLARE`), so there is no
call in `kmain` either:

- **virtio-rng** (`virtio_rng.c`, dev 4 / PCI 1af4:1005). It closes a gap this
  tree already worked around in writing: `rng_strong()` is defined as "RDSEED
  or RDRAND available" and gates whether TLS will attempt a handshake at all,
  so `Makefile:1182` passes `-cpu max` for the stated reason that the default
  qemu64 has neither and the TLS client refuses the weak rdtsc fallback. Any
  machine whose CPU model lacks them -- an explicit `-cpu qemu64`, an older
  model, a real hypervisor masking them -- has no hardware entropy at all.
  **It is NOT wired into `rng.c`** (deliberately; that file is not this
  change), and its boot self-test prints 32 bytes of the entropy stream to a
  serial log every `tests/boot` harness captures to a file -- harmless while
  nothing consumes the driver, a leaked seed the moment the requested hook
  exists. Whoever writes the hook deletes the self-test.
- **virtio-balloon** (`virtio_balloon.c`, dev 5 / PCI 1af4:1002). The pressure
  source the reclaim/swap line was written for and never had: `-m` at boot and
  `mempress.as` can only squeeze a machine that has not started yet or a
  process that asks. A balloon squeezes a RUNNING one, from the host, live. It
  lands on the right side of the reclaim invariant for free -- a balloon frame
  is `pmm_alloc()`'d, so `pmm_refcount` is 1 while `rmap_count` is 0, and
  "evict only if they are equal" makes it structurally un-evictable with no
  `pmm_pin()` and no exception list.

  `make test-virtio-rng` (4) `test-virtio-rng-negctl` ·
  `test-virtio-balloon` (12, cross-checked from BOTH sides -- the guest's
  `pmm_free_frames()` and the host's `query-balloon` must agree; one side
  alone is a driver agreeing with itself) `test-virtio-balloon-negctl`

Pre-M20 prerequisite — **mini-libc 大补**: `c/apps/libc` grew into a real
freestanding C lib. `io.c` is the single errno/syscall TU (POSIX wrappers over
int 0x80); `stdio.c` is now **fd-backed buffered FILE I/O** (fopen/fread/fgets/
fseek/…); added `fcntl.h`/`unistd.h`/`setjmp.h` (+`setjmp.asm`), `strtok`/`memmem`.
Also fixed stdio bugs (short-write loss, `%g` trailing zeros, `%*`/`.*` neg width,
round-half-to-even). Only browser/JS + `/bin/as` link mini-libc; CLI coreutils use
`logit.h` inline syscalls. IDE: `tools/gen_compile_commands.py` + a self-sufficient
`.clangd` (full INCDIRS) kill the host-SDK false-positive squiggles.

**It has kept growing well past that paragraph** — `dirent`/`stat`/`time` with a
full `strftime`+`strptime`/`signal` (real kernel delivery)/`regex`/`fnmatch`/`glob`/
`pthread`/BSD sockets/`wchar`, a real `system()` (fork+execv `/bin/sh -c`), a
segregated-free-list malloc, and as of 2026-08-14 the headers a ported program
includes on its first line: `<libgen.h>`, `<err.h>`, `<sysexits.h>`, `<paths.h>`,
`<search.h>`, `<ftw.h>`, `<iconv.h>`, `<langinfo.h>`, `<nl_types.h>`, `<getopt.h>`,
`<utime.h>`, `<sys/{uio,file,ioctl,statvfs}.h>`.

**`AS_LIBC := $(wildcard c/apps/libc/src/*.c)` (Makefile:718) feeds `LIBC_OBJS`, so
a new `.c` here needs NO build-system change.** That is why this area parallelises
and most of the tree does not.

**The gate is a diff against glibc, and that is the point**: this code is either
pure computation or a thin wrapper over a call the host also has, so "correct"
means "agrees with glibc" — which gives every function a *reference* instead of a
hand-written expectation that only records what its author already believed. Each
test source compiles twice (once against our headers with `-nostdinc`, once as an
ordinary host program) and the two stdouts are diffed byte for byte:
`make test-libc-host`, 11 gates, 2,356 lines. **Two traps live in that strategy and
are documented at the top of `tests/libc.mk`** — (1) the "ours" build still *links*
glibc, so a missing implementation TU is a **runtime fallback, not a link error**
(omitting `langinfo.c` links fine and then segfaults inside `nl_langinfo`); and (2)
`diff <(a) <(b)` starts both binaries **concurrently**, so any gate touching a fixed
scratch path races itself — the rule always writes to files and diffs the files.

**Nothing here is stubbed to success**, and that is a rule, not a coincidence:
`flock` returns `ENOSYS` because a caller that gets 0 believes it holds a lock;
`ioctl`'s tty requests return `ENOTTY`, matching `termios.c` rather than inventing a
second answer; `statvfs`/`utime` return `ENOSYS` because a fabricated `f_bsize` is
worse for a caller sizing a buffer than an error is. Each is argued in its file.

M20 AetherScript ✅: a **from-scratch language**, `as`/`.as`, in `c/apps/as/`
(clox-lineage: single-pass compiler → flat bytecode → stack VM; Python-ish
indentation). **A1** lexer (INDENT/DEDENT) + Pratt compiler + VM: nil/bool/int(i64)/
float(double), arithmetic/cmp/logic (short-circuit), if/elif/else, while, def/
return/recursion, globals + lexically-scoped locals. **A2** strings (concat/index/
len), lists (`[...]`, index get/set, `.append()` via OP_INVOKE, len), `for x in
range()/list`, builtins (print/len/range); real block scoping. **A3 indirection**
(the point — systems programming, not a sandbox): `as_ll.c` raw mem peek/poke +
inline-asm `int 0x80` syscall (portable: real on x86_64-elf, stubbed on the arm64
host) + `as_native.c` addr/peek8-64/poke8-64/i8-64ptr/syscall + `SYS_*` globals;
typed pointer `ObjPtr{addr,width,signed}` via `p[i]`. Shipped as **`/bin/as`** (own
Makefile rule: as core + mini-libc + crt0_cli @0x50000000; reads scripts via
mini-libc `fopen`) + `/usr/as/*.as`. **Modules/import**: `import NAME` / `from NAME import …`,
`mod.attr` + `mod.fn()` via the `.` operator; each ObjFn carries `->module` so
globals resolve per-module with a shared builtins fallback; loader caches +
`fopen`s `/usr/as/NAME.as` (or an in-memory registry for tests). Tests: `make
test-as` (host, 55 checks incl. fib + import) + `make test-as-os` (boots LogitOS,
runs the examples incl. an import demo over serial). Perf: fib(32) ~126ms host
(≈CPython).

**That paragraph is the record of M20 and stops there. The language is now at M27
and about 7.9 kLOC (2026-08-17)**, and the four things M20 listed as deferred — dict, closures,
GC, computed-goto — all exist. Do not plan against the M20 feature list.

- **M21 dict · M22 closures · M22.3 classes** (`class`/`super`, copy-down
  inheritance) **· M22.4 exceptions** (`try`/`except`/`raise`, with unwinding that
  release paths hook) **· M23** bitwise/shift/`**`.
- **Performance work that shapes the code**: a 16-byte tagged `Value` that is
  deliberately **not** NaN-boxed (an AetherScript int is a full int64, so there are
  no spare bits in 8 — the cost worth removing was the memory round-trip, not the
  footprint); **shapes** (hidden classes) with **property inline caches**; a
  **global-lookup cache** with generation invalidation; mark-sweep GC over a
  **contiguous object registry** rather than an intrusive `next` list; and
  **computed-goto** dispatch.
- **M27 ports** — OS endpoints as first-class values: `O_PORT`/`O_PROC`, the `|>`
  pipeline operator, `-> path` / `<- path` redirection, `with` scopes with
  deterministic release, and iteration reusing `OP_LEN` + `OP_INDEX_GET` rather
  than a new iterator protocol. Its payoff is **`fsroot/as/examples/ash.as`: the
  system shell, written in AetherScript**, with no `fork`, no `dup2`, no `waitpid`
  and no file-descriptor arithmetic anywhere in the file — against 982 lines of C
  in `c/apps/coreutils/sh.c` doing the same job.

**THE SELF-HOSTING TAX, and it is the single most important thing to know before
touching this language.** `fsroot/as/lib/asc.as` is a **second compiler for
AetherScript, written in AetherScript**, and it compiles itself to a
**byte-identical fixpoint** (`test-as-bcstable`). Opcode numbers are **hand-copied**
into it. A drift is a **SILENT MISCOMPILE** — the self-hosted compiler emits an
instruction the C VM decodes as a different one — and *nothing else catches it*:
setting `OP_RET` to 99 in `asc.as` leaves `test-as` and `test-as-gcstress` fully
green. Only `make check-asops` (`tools/gen_as_opcodes.py --check`) sees it, and it
is a prerequisite of the `test-as*` targets but **not** of `test-ash`, `all` or
`$(ISO)` — so a plain `make run` boots happily with a badly drifted `asc.as`.
Consequences: **batch opcode changes one milestone at a time**, bump
`AS_BC_VERSION` once, re-prove the fixpoint at the end, and never add an opcode
opportunistically. `gen_as_opcodes.py --write` is a stub that exits; the sync is by
hand.

**Where it is going:** `docs/superpowers/specs/2026-08-05-aetherscript-2-language-design.md`
fixes the originality criterion (*a construct earns its place only if it falls out
of a constraint specific to this OS*) and names four pillars — ports (M27, done),
**capabilities (M28)**, tasks (M29), an own IR + native `.aex` (M30).
`docs/superpowers/specs/2026-08-14-m28-capabilities.md` is M28's locked design and
supersedes the older document's M28 row; read §1 first, because the older
document's grant model does not work on this machine (every `.as` program execs the
same binary, `/bin/as`, so a grant keyed on the executed binary cannot tell two
scripts apart — and fails silently).

H.264 video ✅ (`c/lib/video/`): a from-scratch baseline-profile decoder --
Annex-B/NAL, CAVLC, I+P slices, multiple references, weighted P prediction, the
deblocking filter, cropping. Bit-exactness is the bar, not a tolerance: H.264
reconstruction is exactly specified integer arithmetic, so any mismatch with
ffmpeg is our bug. `make test-h264` decodes ten x264-generated streams plus a
committed fixture and compares every byte (11 streams x 60 frames);
`make test-h264-units` covers prediction/MC/deblocking as modules;
`make test-h264-diff` prints per-case wrong-byte totals, which is the metric to
bisect with -- "the first mismatch moved" says nothing.
**The decoder is RING-3, not kernel.** `c/lib/video` is filtered out of `C_SRC`
on purpose: it allocates with malloc/free, and unlike an image (decoded once,
hence `SYS_IMG_DECODE`) a video is decoded 30x/second with megabytes of live
reference frames -- in the kernel that holds the BKL per frame and puts a media
parser in ring 0. Consumers link `VID_OBJ` + mini-libc: `/bin/vidcheck` (prints
the decoded CRC32; `make test-video` boots LogitOS and requires it to equal the
host's) and **Preview**, which now shows both images and video and picks the
path by sniffing the Annex-B start code rather than the file name.
Gotchas worth keeping: `mbinfo` uses raster order for `mv[]` but **Z order for
`nz[]`**; mvp's A/B/C all come from the partition's TOP-LEFT corner and only C
steps right by the partition width; an INTRA neighbour is *available* with
refIdx -1 (not unavailable), and "available" additionally excludes anything not
yet decoded, including sub-partitions of the current macroblock; `ref_idx` is
coded per partition but read back per 8x8 quadrant; deblocking compares
reference **pictures**, not indices (weighted prediction puts one picture at
several indices on purpose); and a weight of 128 is inferrable though not
codable, so clamping weights to 127 silently darkens every frame.

Memory reclaim + swap ✅ (`c/kernel/mm/{rmap,reclaim,swap}.c`): the kernel can now
take a physical frame back from whoever has it, so running out of RAM stops being
fatal. **Reclaim is the mechanism; swap is only a place to put things.** Nothing
here is firefighting -- the full desktop peaks at 229 MiB of 511 and `pmm_audit()`
is clean -- so the pressure has to be MANUFACTURED to test any of it, and the
harness was built first.
- **Who maps this frame** (`rmap.c`, the hard part). pmm's refcount says how many
  leaf PTEs point at a frame, never which, and you cannot unmap a PTE you cannot
  find. So: a chain per frame (head[] + a pre-allocated node pool, 12 bytes a
  node, ~0.5% of RAM), maintained at the ONE place a leaf PTE changes
  (`vmm.c set_leaf`, which `vmm_map_page`/`vmm_map_page_in`/`vmm_map_raw_in` all
  funnel through) plus the copy-on-write copy in `fault.c`. The rule everything
  rests on: **evict only if `rmap_count(f) == pmm_refcount(f)`**, nonzero and not
  truncated -- the same number from two independently maintained structures, so a
  bug in either costs reclaimability, never correctness. `rmap_audit()` checks it.
- **That is also the pin discipline.** Page tables, kheap arenas, DMA rings and
  the rmap's own tables have refcounts but no user PTE, so their rmap count is 0
  and they fail the test structurally -- no list of exceptions to forget to
  update. `pmm_pin/unpin` (a saturating count, nesting) covers the other case: a
  real user page the kernel is touching right now.
- **Two tiers, cheapest first.** TIER 1 drops a page and re-derives it on the next
  fault, no device involved; TIER 2 writes it to a swap slot. NOTE, said plainly:
  this kernel has **no file-backed user mapping at all** -- mmap takes no fd
  (`mmsys.c`), an ELF image is read *eagerly* into anon frames by `elf_load` (so
  app text is NOT backed by its `.aex` and must be swapped, never dropped), and
  an fd read lands in a kmalloc buffer in no user page table. So the classic
  "drop a clean page-cache page" tier has no producer here; building it first
  would have been building a mechanism with nothing to feed it. The equivalent
  that does exist is the **zero page** -- an anonymous page (`VMM_PTE_ANON`, bit
  10) currently all zero, re-derivable by `do_anon`. Tier 1 tests the CONTENTS,
  not the dirty bit, so a page the process zeroed also qualifies. (It used to be
  dominated by browser.elf's ~105 MiB `.bss`; the libc line has since moved that
  96 MiB arena to `SYS_MMAP` with a commit bound and the `.bss` is now <10 MiB --
  not allocating a page beats reclaiming it. Tier 1 is smaller now and still
  real; `run-swap-test.sh` asserts BOTH tiers fire and prints the split.)
- **Clock over physical frames**, not an active/inactive LRU: with no hardware
  reference notification, a "recently used" list could only be built by the same
  accessed-bit sampling the clock's sweep already is, so lists would add
  bookkeeping without adding information. Sweeping physically (possible only
  because the rmap exists) is right because the thing reclaimed is a frame.
- **Swap PTE**: P=0, bit 1 = marker, bits 2-4 = saved W/COW/ANON, bits 12+ = slot,
  bit 63 (NX) left in place. Slot numbers start at 1 so a zeroed PTE can never
  read as a swap entry. `mm_fault_classify` checks it FIRST and independently of
  the VMA -- an ELF text page has no VMA, and a swap entry falling through to the
  anonymous case would be silently refilled with zeroes. Slots are refcounted
  (a shared page evicts once); fork inherits swap entries; munmap/exit release
  slots. Known cost: a shared page that round-trips comes back private.
- **No allocation in the write-out path**, which is where naive swap deadlocks
  under exactly the pressure it was built for: rmap nodes and the slot table are
  taken from the PMM at init, `rmap_remove` returns nodes rather than taking
  them, and the page is written straight out of its identity-mapped frame with no
  bounce buffer. Asserted, not argued -- the host test fills memory completely and
  requires reclaim to still make progress. Swap-IN does need one frame, hence
  `pmm_alloc_reserve()` over a 32-frame reserve ordinary allocations cannot touch.
- **The swap device** is chosen through blkdev.h only (no edits to `c/fs` or
  `c/drivers/block`, both live): not the root, not a partition sharing its medium,
  no LogitFS superblock, and **sector 0 blank or already ours**. Anything else is
  refused out loud. Swap is never carried across a boot.
- **Waiting**: the queue for the device uses `bkl_hlt_wait()`, which DROPS the BKL.
  The single in-flight transfer still holds it, because every block driver here is
  submit-and-poll inside one call; that cost is measured and printed
  (`swap_bkl_worst()`) rather than assumed. Closing it needs
  `submit`/`poll` on `struct blk_ops` -- an ask for the block line, not an edit.
- Tests: `make test-mm` (host, adds `mm_rmap_test` + `mm_reclaim_test`; distinct
  per-page patterns that catch wrong-slot AND wrong-offset, plus TWO negative
  controls required to FAIL: `-DRECLAIM_NO_PIN_CHECK` evicts a pinned page,
  `-DRECLAIM_NO_ZERO_CHECK` drops a page with data and it comes back zeroed).
  On device: `make test-swap` boots the same kernel on a deliberately small
  machine with a blank AHCI disk as swap and runs
  `/usr/as/examples/mempress.as`, which mmaps more than exists, writes a per-page
  pattern and reads it all back; `make test-swap-negctl` is the same with no swap
  device and MUST fail. Kernel counters are readable from ring 3 via
  `SYS_MEMINFO` with a NULL buffer (`c/kernel/mm/mmsys.c`, MMCTL_*) -- which is
  how the harness asserts reclaim actually ran rather than asserting it did not
  crash.

## Storage: the block layer and LogitFS

**Status (verified 2026-08-08 and RE-VERIFIED 2026-08-17, all 12 targets
below green both times): this machine
keeps a file across a reboot.** The old "corrupts after repeated non-snapshot
boots; use `-snapshot`" note was true of v3 and is not true now. Do not design
around it, and do not add `-snapshot` to a harness to work around a write.

**Block layer** (`c/drivers/block/`). `blkdev.c` is a multi-device table, not a
single disk: **virtio-blk** (preferred), **AHCI/SATA**, **NVMe**, and ATA PIO as
the fallback, plus `part.c` for MBR/GPT. `blk_flush()` is a real **write
barrier** on every backend that can reorder (virtio-blk `VIRTIO_BLK_T_FLUSH`,
NVMe opcode 0x00; ATA is a no-op because `ata_write` already flushes per write).
`blk_flush_count()` exposes barriers-issued-since-boot so a test can *count*
them instead of reading the source. QEMU line: `-device virtio-blk-pci`.

**LogitFS on-disk v4** (`c/fs/logitfs_fmt.h` is the single definition site;
`tools/mkfs.py` mirrors it in Python and `test-fs-format` asserts every offset
against a real image). 4 KiB blocks, 64 MiB image (16384 blocks): superblock,
free-block bitmap, inode table, **write-ahead log**, data. Inodes are 128 B with
`direct[12]` + single-indirect + **double-indirect** (so files well past the
~4.1 MiB single-indirect ceiling; `test-hugefile` drives 4.4 MB / 1075 blocks).
atime/mtime/ctime live in what used to be `reserved`, so it is a compatible
extension, not a format bump. **Block 0 is never rewritten at runtime** — which
is why the superblock needs no checksum.

**The journal is metadata-only + ordered data (ext4 `data=ordered`)** — say this
out loud, because "it has a journal" is not the same claim. Bitmap blocks, the
inode table, indirect/double-indirect pointer blocks, and **directory data
blocks** (a dirent can name an inode created in the same op) are staged into the
log and installed only after the commit record is on media. Ordinary **file data
blocks are written straight to their final location**, always before the metadata
pointing at them commits. That is sound here only because of `bfree()`: frees are
**deferred to commit**, so the allocator cannot hand a block back to the very
operation that released it and overwrite live data in place before the metadata
that would roll back. (That was a real bug, found by the crash sweep, fixed in
`d9dccbf`.) Consequence to know: an *overwrite* interrupted mid-flight can leave
that file's old content, not a mixture — but LogitFS rewrites a whole file per
write, so there is no partial-append case.

**The commit record self-verifies.** The log header carries `hcrc` (CRC-32 over
the header block — rejects a torn header, the classic truncated-final-record)
and `bcrc` (CRC-32 over exactly the *n* body blocks it describes — rejects a
**stale** header standing over a newer transaction's bodies, which was corruption
*caused by recovery* on a filesystem that never crashed). Three barriers, each
with a distinct failure mode if removed, are documented in full above
`log_commit()` in `c/fs/logitfs.c` — **read that comment before touching this
file**. B1 bodies+data before the record, B2 record before checkpoint, B3
checkpoint before the header clear. `test-barrier` observes exactly 3 per file
write, from `blk_flush_count()`.

**Buffer cache** (`bcache.c`): reads are served without a device round trip,
writes are deferred and dirty-tracked, an evicted dirty buffer is *written*, and
`bcache_sync()` (write everything dirty, then barrier) is the filesystem's only
ordering point. Correctness never comes from a write being withheld — early
writeback only ever moves a block onto media *sooner*, which every case in the
invariant already covers. Reads coalesce: a 900-block cold read costs **10**
device commands, not 902 (`f8d2ca4`; `test-bulkread` bounds it at 14 and
`test-bulkread-negctl` proves the old path cannot meet it).

**fsck** (`fsck.c`) both detects *and* repairs, but its rule is "fix only what
has ONE correct answer": a block claimed by two inodes is **refused**, whole,
because both fixes destroy a file and nothing on the disk says which. A
**read-only** fsck runs at **every mount** — so every boot harness in the tree,
including ones written for something else entirely, now asserts the bitmap agrees
with the inodes and the directory tree is a tree. A mount-time finding never
fails the mount (a damaged filesystem is still the best one available; refusing
to boot over a leaked block is a worse trade). Replay lives in `fsck.c` and is
called by both fsck and `logitfs_mount`, so there is exactly one copy of the
replay rules.

**Around it**: `vfs.c` + `vfs_path.c` (resolution as its own host-testable TU),
`vfs_meta.c` (mode/owner/links/symlink targets), `vfs_cred.c` (process
credentials, keyed by pid until `proc.c`'s owner takes them), `vfsctl.c`
(`/dev/vfsctl`, `/dev/vfsmounts`, `/dev/vfsmeta` — control as synthetic files so
an unprivileged shell can be refused for real), `ramfs.c` (second mount, no
device), `lfsro.c` (instance-aware read-only v4 reader — `logitfs.c` is a
singleton and cannot be two), `fsbench.c` (`/dev/fsbench`, the storage
stopwatch).

**THAT CAVEAT IS GONE, and this paragraph carried it four commits too long.** It
used to read "vfs_meta's records are in RAM and do NOT survive a reboot... until
logitfs implements getattr/setattr (two function pointers, no other change)".
Both pointers are implemented — `logitfs.c:1224` and `:1283`, installed in the
ops table at `:1402`. The on-disk inode carries `xmode`, `uid` and `gid`, and
`xmode` has a presence bit of its own (`LFS_MODE_SET`) for a reason worth
keeping: **mode 0 is a legal mode**, so "0 means unset" would make a file nobody
may touch unrepresentable. That distinction is carried out to ring 3 as
`LSTA_MODE_STORED` — a stat that cannot tell a chosen 0644 from a defaulted one
is a stat that lies quietly.

Gated by `test-statmeta`, `test-statmeta-negctl` (both green) and
`test-statmeta-os`.

Why this mattered enough to correct in place: modes and owners not surviving a
reboot is not a footnote, it is the premise several other things need. A trust
store on disk is only a trust store if an unprivileged process cannot rewrite
it; a read-only `/bin` is only read-only if it is still read-only after the next
boot. Reading the stale sentence, the answer to "can this machine hold a
permission?" was no. It is yes.

**Tests — and these are not decorative; all 12 ran green on 2026-08-08 and all
12 ran green again on 2026-08-17**, the second time as part of a sweep of every
target in the tree rather than as a deliberate check of this subsystem. That
distinction is the point: nine days and a good deal of unrelated work later,
including four boot targets that cost minutes each (test-durability is five
boots, test-fscrash is four SIGKILLs), nothing here had quietly rotted.
Host (`make test-fs-host`, seconds, uses a simulated device whose defining
feature is a *volatile write cache* so barriers are not no-ops):
`test-fs-cache` 29 · `test-fs-journal` 48 · `test-fs-crash` **1744** ·
`test-fsck` 167 · `test-fs-format` 25 · `test-bulkread` 34 + its negative
control. `test-fs-crash` is the one to know about: it cuts power at **every
device write** of write/mkdir/delete/rename/overwrite × 3 loss patterns, and
after every cut demands mountable, fsck-clean, bystanders byte-for-byte, victim
whole-or-absent, no block handed out twice.
Boot (minutes each, real QEMU, **no `-snapshot`** — that is the point):
`test-fsmount` (2 boots, kernel's own fsck clean both) · `test-durability`
(**5 boots, 3 files byte-for-byte, 2 rounds of churn**) · `test-fscrash`
(4 SIGKILLs; log replay witnessed in 2/4) · `test-fsreplay` (a hand-sealed
uninstalled transaction, replayed deterministically) · `test-hugefile` ·
`test-barrier`. **Byte-for-byte, never a length check** — a filesystem that
hands one block to two files produces a file of exactly the right length holding
someone else's data, which a length check cannot see.

## Open Logit: the 2D rendering engine (`c/lib/gfx`)

**It exists because there were three coverage/paint paths and every new app
started from `gui_rect`.** The kernel's M14 glyph rasterizer (`c/kernel/gui/
raster.c`, 285 lines) did glyphs; a SECOND coverage rasterizer lived
in the widget toolkit (`c/apps/gui/aui.c`); a THIRD hand-rolled paint path lived
in the browser (`c/apps/browser/browser_paint.c` -- an integer square root and a
per-row band loop). Open Logit is the one engine all three are now built on, and
**all three are deleted**, which was the point: an engine that coexists with
what it replaced is a fourth path.

**`raster.c` was the last and the largest, and it went with the glyph
migration.** `c/lib/text/glyphras.c` is what replaced it: a CONVERTER from a
font outline (`fp_path`, font units) to a `gfx_path` in device 24.8, plus one
`gfx_fill_mask_subs(..., 16)`. It rasterizes nothing. The per-point scaling is
raster.c's exact `(v*px*256)/upem` with the CTM left at identity -- a 16.16
scale matrix loses ~0.03 px at CJK upem/px combinations -- but the curve
flattening is the engine's adaptive tolerance instead of raster.c's fixed
segment count, which is where the accuracy came from. Glyphs pass `subs=16`
where a button passes 4, and `icons.c`'s eleven hand-authored vector icons went
over at the same time (`vg.h`/`vg_render_path` deleted with the file).
- **The number**, from `make test-glyph-agree`, against an oracle that is
  neither rasterizer (a 32x32 supersampled point sample of the true outline, in
  double, over 572 bitmaps and 363,650 pixels): the bridge scores **mean 0.310 /
  255, worst pixel 16**; raster.c scored **0.490 / 61** on the same set. The
  replacement is closer to the true geometry than the thing it replaced, 1.6x in
  the mean and 3.8x in the worst pixel. `tests/unit/glyph_agree_legacy.sh` is
  the build in which both existed; it cannot run from this tree because one of
  them is gone, and it says so.
- **The migration found a real defect in the engine, and it could only have been
  found this way.** `gfx_raster.c`'s `span_add` accumulated straight into the
  0..255 byte row, converting each sub-scanline's covered length on the spot
  with an integer divide -- so the truncation was paid ONCE PER SUB-SCANLINE and
  the error GREW with `subs`, backwards for the one knob that exists to buy
  accuracy. At the default 4 it cost up to 3/255 and nobody noticed; at 16 it
  cost up to 15/255 on every antialiased pixel of every glyph, always in the
  same direction, and the first measurement of the port showed the whole
  typeface coming out lighter (mean error against FreeType 0.74 -> 2.02 on
  ui.ttf). `g_acc` sums lengths exactly and converts once per pixel, through a
  16.16 reciprocal that is EXACT for every `subs` dividing 65280 (4 and 16 among
  them). Cost: 8,192 B of .bss, +3% on a whole-path fill.
- **And a latent UB**: `make test-font-fuzz` is the first ASan/UBSan build in
  the tree that reaches `c/lib/gfx` at all, and it caught `(x1-x0) << 16` --
  left-shifting a negative signed value, which every leftward edge in every
  shape produces. Fixed at all four sites (`add_edge`, `gfx_m_invert`,
  `arc_mid`); `* 65536` compiles to the same instruction.
- **Net kernel `.bss`: -29,356 B** (raster.c's 515,076 out, glyphras.c's 444,416
  + icons' own path storage 33,112 + `g_acc` 8,192 in), measured with `nm`.
- Proof the face of the machine did not change: `test-desktop-look` 16/16 with
  every recorded value identical (including the three KNOWN-BUG rows and their
  exact extents), and a before/after boot screendump differing by **mean 0.021 /
  255 over the whole screen**, 4,640 of 1,024,000 pixels touched at all, and
  mean luma identical to three decimals in every text region.

**It sits beside `c/lib/text` and `c/lib/image` and CONSUMES them** -- it
rasterizes no glyph and decodes no image; the outline comes from `c/lib/text`
and only the coverage is this engine's. (This paragraph used to open "Ring 3,
and filtered out of `C_SRC` on purpose", which was already wrong when `fb.c`
started calling `gfx_mask_corner` for the window corners: `c/lib/gfx` is NOT
filtered out of `C_SRC`, it compiles into the kernel as well as into every
ring-3 GUI binary, and since the glyph migration the kernel is its busiest
caller. The ring-3-only claim survives for `c/lib/video`, `c/lib/audio` and
`c/lib/media`, which really are filtered.)

**Phase 1** is paths (move/line/quad/cubic/close), nonzero + evenodd, a scanline
coverage rasterizer, four paints (solid / linear gradient / radial gradient /
image), Porter-Duff src-over, an **affine transform applied to PATHS** (which is
what CSS `transform` needs -- `translate(-50%,-50%)` centring is reached by 14 of
15 real pages), and a rectangle clip.

**PHASE 2 HAS LANDED** (`8e85be37b`), and this paragraph went on saying "stroke
and path clipping do not exist" for four commits after they did. That is not a
bookkeeping slip: it is why three negative controls stopped linking on
`gfx_path_ellipse` with no explanation available from the document every session
reads first. `c/lib/gfx/gfx_stroke.c` exists, and `test-gfx` is now
`test-gfx-raster test-gfx-paint test-gfx-stroke test-gfx-clip`.

Each half arrived with its own independent oracle, which is the bar this engine
is held to:
- **the stroker's** is a THIRD reference: the distance from a pixel to the
  FLATTENED source polyline, plus the miter wedge and the square-cap half-square
  in closed form. To the polyline and never to the ideal curve -- measuring
  against the curve charges the path's flattening error to the stroker and fails
  a correct one.
- **path clipping's** is the AND of two analytic predicates that already
  existed, supersampled the same 16x16 way; composition costs an oracle almost
  nothing, which is exactly why `in_sq_annulus` was worth copying. The clip mask
  is materialised by the CALLER because `raster()` is not reentrant, and the
  tests use deliberately ASYMMETRIC clip extents and origins -- a centred clip
  hides a transposed axis or a sign error, and an off-by-one in either axis is
  the single most likely defect in the feature.

The trap recorded before the work still holds and is worth keeping in view: a
stroked corner's inner ellipse shares the outer's CENTRE and differs only in
radii, and insetting the centre pinches the arc to nothing before it meets the
straight edges.

**Construction** (deliberately the glyph rasterizer's, so a shape and the type
on it are antialiased by the same rule at the same size -- and since the glyph
migration that is literally true, not merely by construction): 4 sub-scanlines
per pixel row for shapes, 16 for type, with EXACT fractional horizontal coverage
along each and the lengths summed exactly before one conversion. Horizontal
coverage is analytic and free, vertical costs a pass -- so buy accuracy where it
is cheap. Integer only, 24.8 coordinates and 16.16 matrices. **No libc and no
allocator**: six GUI apps link crt0 + aui + this and nothing else, path storage
is the caller's, and every bulk clear goes through `gfx_zero()`, whose volatile
pointer is what forbids `-O2` rewriting it into a call to `memset`.

**Three techniques carried over from the toolkit, because they are why it is
cheap enough to run a desktop on.** Masks are generated at DEVICE resolution and
blitted into a POINT rect of the same device size, so the compositor's
nearest-neighbour rescale is the identity and antialiasing survives 150%/200%.
Only what CURVES is rasterized -- a rounded rect is a 9-slice (three bands +
four `r x r` corner tiles), so **O(r^2), not O(w*h)**: 0.144 us against 6.96 us
to rasterize the same 200x40 r=8 shape whole, and zero on the second card. Masks
are cached by exact device geometry.

**The bar is a number against an INDEPENDENT reference**, because 2D coverage
has no ffmpeg to diff against but is exactly computable, so the oracle is built:
every filled shape against a 16x16 supersampled evaluation of its own analytic
predicate, every blend against Porter-Duff recomputed in double. Worst pixel
error: circles r=3..48 **0.095**, ellipses 0.091, rounded rects 0.047, triangles
0.119, corner tiles 0.078, ring tiles 0.078; src-over over 175 alpha/coverage
combinations **1/255**. (Circles and ellipses read 0.091 and 0.087 until the
glyph migration replaced the per-sub-scanline divide with `g_acc`'s exact length
sum -- see that fix above. Both are worst-SINGLE-PIXEL figures at subs=4, where
the old truncation was worth up to 3/255 in one direction, so a 0.004 move
either way is inside what that change can do; what the same change did to the
number that was actually wrong is 0.911 -> 0.310 against the glyph oracle. The
assertions in tests/unit/gfx_raster_test.c were NOT touched -- these two values
moved under unchanged bounds. Re-recorded rather than left stale, because a
number in this file is a claim somebody will diff against.) `make test-aui-mask` still passes unchanged -- it has its
own, independently written reference, so the engine is checked against TWO
oracles sharing no code.

**A real bug found by building the reference first**: `gfx_over` truncated
`da*(255-a)/255`, which at a=1, da=1 floors the destination's surviving alpha to
ZERO -- the destination colour leaves the average and the result is the source at
full strength, 17/255 off the definition. Rounding both divisions takes it to
1/255. Everything faint over something faint was wrong that way.

**Cost, measured on the machine and SPLIT**, because a frame total credits the
engine with work it does not do. `-DAUI_COST` brackets every drawing syscall and
reports the residual (`make bench-gfx-frame`, us/frame, TCG):

```
mode          clear    text  rect+blit  ENGINE   wall   engine%   tiles/frame
1280x800        153    1124       2146     677   4128     16.4%       8.0
1920x1200       391    1702       3882     755   6781     11.1%       8.2
2560x1600       715    2230       5580     585   9136      6.4%       7.6
```

The engine's share FALLS as the display grows: the compositor's fill and the
kernel's glyph work scale with pixels, the corner tiles do not. Uninstrumented
(`make bench-aui`, 1920x1200): 6.3 ms for a normal page, 10.4 ms for the
pathological all-geometry one.

**Negative control**: `-DGFX_NO_AA` drops the rasterizer to one centre sample
per row with binary horizontal coverage. Every shape still draws and still looks
broadly right -- what breaks is the agreement with the reference, 67 assertions
fail, and `make test-gfx-negctl` succeeds when the test fails. On device
`test-aui-negctl` is the same shape one layer up.

**Nothing was asked of the kernel and nothing taken** (as of phase 1 -- the
glyph migration since then deleted `raster.c` and rewrote `icons.c`).
`fb.c` and `wm.c` are untouched; the engine reaches the screen through `SYS_GUI_BLIT`, the
one existing entry point with per-pixel alpha. Consequence worth knowing:
`browser_paint.c`'s opaque rounded boxes no longer call `SYS_GUI_RRECT`, whose
corner test is the boolean `dx*dx + dy*dy <= r*r`, so **a page's `border-radius`
stopped being a staircase**.

  `make test-gfx` `test-gfx-negctl` `test-aui-mask` `bench-gfx` `bench-gfx-frame`

**Phase 2 was scheduled** in `docs/superpowers/specs/2026-08-14-open-logit-2-design.md`
(seams → stroke → path clipping → SVG onto the engine → text as paths →
groups/blend/blur). **The first four are done, and this paragraph described the
world before them for four commits** — all four of its claims were false when
measured on 2026-08-17, and each is the kind that sends somebody the wrong way,
so they are recorded here rather than quietly overwritten:

| it said | measured |
|---|---|
| stroke and path clipping "do not exist" | `c/lib/gfx/gfx_stroke.c`; `test-gfx` = raster + paint + **stroke** + **clip** |
| "a FOURTH rasterizer is still live, and it is in ring 0" | `svg.c`'s own sorted-crossing filler and its Newton `dsqrt`/`dsin` are **gone** (`e282d0f57`) |
| "`C_SRC` ... **and not `svg.c`** — so it compiles into the kernel" | `svg.c` is **not in `C_SRC`**; it no longer compiles into the kernel |
| "`grep -c stroke` on it returns **0**" | returns **30**, and svg.c makes 76 `gfx_*` calls |

So the founding argument IS finished: one rasterizer, and `svg.c` is a consumer
of it like everything else. Somebody reading the old text would have gone
looking for a fourth rasterizer that is not there, or assumed a page's stroked
icons are still missing when they render.

The way it went stale is worth more than the correction. Nothing here was
careless — the work landed, the commit message said so plainly ("gfx: G3 — the
other rasterizers are deleted, and the screen cannot tell"), and this file was
simply not the thing being edited. It is the first document every session reads,
so its staleness is not inert: three negative controls stopped linking on
`gfx_path_ellipse` this week, and nothing here explained why a symbol that
"does not exist" was being referenced.

## How big a program can this machine load? 12 MiB, measured

Asked because a Python port turns on it, and answered without CPython, without a
download and without touching the repo -- so the number stands whatever happens
to that question.

A 12 MiB `.aex` (`text 12,583,024 / bss 16` -- a REAL `PT_LOAD` of const rodata,
because `.bss` would measure nothing about the load path: `elf.c` commits
`p_memsz` and the pages would never be read) was packed at **`/bin`, not the
root**. Never the root: `wm.c:4994` `scan_apps()` kmallocs every root `.aex`
WHOLE at boot merely to read 64 header bytes, which would put a contiguous-arena
gamble on the critical boot path.

```
cold boot        [kheap] grow #5: +16384 KiB -> arena 36864 KiB, live 6059 KiB, over-allocated 0 KiB
after browser    [kheap] grow #5: +16384 KiB -> arena 36864 KiB, live 6085 KiB, over-allocated 1 KiB
                 [execve] pid 3: /bin/padbin loading
                 padbin ok
```

**The second run is the control, and it is the only one that proves anything.**
`pmm_alloc_contig` (`pmm.c:470-490`) is a linear first-fit over the frame bitmap
with NO fallback, so the question was never "does it work on a fresh machine" --
it was "does it still work once the desktop and the browser have fragmented
memory". It does, identically: same growth sequence, 26 KiB apart in `live`.

`padbin ok` prints only after touching `pad_blob[12582911]`, the LAST byte, so
this is the whole image arriving rather than a header that parsed.

**THE LIMIT IS NOT SPACE, AND IT IS NOT THE HEAP -- IT IS INODES.** The image
went 7,136 -> 10,217 of 16,384 blocks, using **19% of the free space** for 12
MiB. But `tools/mkfs.py`'s `INODE_COUNT` is **256**, and 223 are already in use.
A runtime shipping a stdlib as thousands of `.py` files exhausts the inode table
long before the disk, which is why a frozen single binary is the only shape this
filesystem permits.

## Six subsystems this file did not describe, found by counting

Not noticed -- MEASURED, on 2026-08-17, by comparing each subsystem's target
count against how often this file mentions it. Four came back with substantial
coverage and no documentation at all:

    ip6     8 targets   0 mentions        webapi  6 targets   0 mentions
    forms   7 targets   0 mentions        usb     9 targets   1 mention
    demux  10 targets   2 mentions        wpt     9 targets   1 mention

**An absent claim is worse than a stale one and much harder to see.** A wrong
sentence misdirects and can be corrected; a missing one lets a reader conclude
the subsystem does not exist or is untested, and there is nothing for a
correction to attach to. IPv6 was the sharpest case: the networking sections
above run M9 to M12 and say "net" thirty-seven times, so nobody reading them
concludes the documentation is thin -- they conclude the stack is IPv4-only.

Each section below records what that subsystem's OWN source or tests argue,
rather than a summary written from the outside. The measurement is repeatable:
compare `make -pRrq | grep -oE '^test-[a-z0-9]+'` counts against `grep -c` here.

### WPT — the headline number, and the two ways it lied

```
WPT: 149318/246542 subtests passed (60.6%) over 9181 harness files
     0 CRASHED · 554 DIED · 363 NEVER STARTED
```

This is the most quotable number in the tree, which is exactly why the suite is
built to distrust it. **The gate is a ratchet against an expected-failure list,
not the percentage**: green on a tree that changes nothing, red only when
something that worked stops working. The corpus is OPTIONAL and the capability
is not — `WPT_ROOT` points anywhere, an absent directory makes the runner say so
and exit 0, because a missing corpus is not a regression in the code under test.

**THE RUNNER MUST BE `browser.aex`, or every number is of a browser that does
not exist.** Two halves, which fail independently, and the second one is the
most instructive failure in this repository:

- **The source list** — expressed as a SUBTRACTION from the Makefile's own
  variables, so it cannot drift silently. (Three `*-negctl` link failures and
  three `browser-*.elf` variants missing `$(GFX_OBJ)` on 2026-08-17 are what
  happens where that discipline is not applied; `make test-negctl-drift` now
  gates it.)
- **The call sequence.** *LINKING A TRANSLATION UNIT IS NOT RUNNING IT.* The
  runner linked `css_extra.c` and `layout.c` and then never called
  `css_apply()`, `css_extra_apply()` or `layout_page()`. `make test-wpt
  ONLY=css/css-grid` read **531/11152 with AND without the grid
  implementation** — 11,152 subtests structurally unreachable, and the line
  shipping grid unable to tell its own work from a no-op.

And one thing the rate does NOT cover: reftests, judged by pixels against a
reference render, which this runner does not do. They are not unmeasured —
`make test-reftest` runs them out of the same `third_party/wpt` against a
17,452-entry baseline and judged 24,300 of them on 2026-08-17. The runner's
own output said "there is no reftest harness here" until that day; it now names
the gate.

### Containers, the A/V clock and MSE

`c/lib/media/`: `demux.c` (sniffing, the demuxer object, sample ordering,
seeking, the Annex B rewrite), `mp4.c`, `mkv.c`, `avclock.c`. Fifteen targets
across `test-demux*`, `test-avsync` and `test-mse*` — **fourteen green**, with
`test-mse-os` the one red (playback stalls on device; see the sweep triage).

Three things the source argues that are worth knowing before touching it:

- **Sniffing is by CONTENT, not by name.** A file called `.mp4` that is really
  Matroska opens as Matroska, and a `.bin` that is really an MP4 opens too —
  the same rule `audio_sniff()` uses. Preview already relies on this: it picks
  the image-or-video path from the Annex-B start code rather than the
  extension.
- **`demux.c` is what mp4 and mkv have in common**, and it is what a player
  talks to. Format-specific code stays in the two format files; a third
  container is a third file, not a third path through the player.
- **`avclock.c` is a POLICY, not a conversion.** A container hands out two
  streams of timestamps written by an encoder on another machine, and they mean
  nothing until something decides what "now" is. On this machine that is not
  academic — a from-scratch H.264 decoder under QEMU's TCG is not guaranteed to
  keep up, so the falling-behind policy is the design, not an error path.

The fuzz gates are worth naming because the shapes differ: `test-demux-fuzz`,
`-fuzz-deep` and `-fuzz-negctl`. A container parser reads attacker-controlled
offsets and lengths for a living, which is the same argument the image decoders'
`test-img-fuzz` makes.

### USB/xHCI — and the third producer on the input ring

`c/drivers/usb/`: `xhci.c` + `xhci_ring.c` (the controller and its rings),
`usb_core.c` (enumeration and driver binding), `usb_desc.c`, `usb_hid.c`,
`hid_report.c` — 2,317 lines, nine targets, **all nine green**.

Two things in it are load-bearing beyond USB:

- **Binding is on the INTERFACE class/subclass/protocol triple**, which is why
  `usb_core.c` is worth more than the HID driver it currently serves: a second
  device class is a `probe`/`poll` pair, not a rewrite.
- **`usb_isr` posts keys and pointer motion into the WM's input ring** through
  the same `wm_key()` / `wm_mouse_event()` the PS/2 drivers call. That makes
  USB a THIRD producer on `inq_push`, which matters to the BKL removal (see
  `docs/superpowers/specs/2026-08-17-bkl-removal.md`) and is invisible from
  either end — from the interrupt table it is "the USB vector", from `wm.c` it
  is the same two functions.

**And the tree already gates the property that fact creates.** `test-usb-both`:

> Coexistence: both stacks live at once. The requirement is not "input works"
> but "exactly once" — two producers on one queue is only safe if neither
> duplicates the other's events, and no "does input work?" test catches that.

It COUNTS rather than detects, so double delivery fails it. `test-usb-none` is
its control: the same machine with the devices unplugged, where nothing can
deliver input, so the positive assertions must not be satisfiable — *"a test
that passes with the thing under test taken away is measuring something else."*

### The interactive web platform — forms, focus, and the platform APIs

`c/apps/browser/`: `js_platform.c` (1,910), `forms.c` (2,199), `js_select.c`
(1,188), `js_forms.c` (1,015), `focus.c` (342). Thirteen targets — the
`test-webapi*` six and the `test-forms*` seven — twelve green.

**The rule this area is built on is worth more than the inventory**, and
`js_platform.c` states it:

> every entry below is either a name a page in `tests/fixtures/webapi/` actually
> reached for (with the page named in the comment) or is marked as
> requested-but-unmeasured. **Nothing in this file is here because a browser is
> "supposed to" have it.**

So the coverage is demand-driven and the evidence is per-name. That is also why
`test-frameworks` is a CHANGE DETECTOR rather than a wish list, and what makes
its acceptance criterion checkable: *when a name from channel 1 is implemented,
the channel-2 error for that page must change, and if it does not, the
implementation did not matter.*

**It has just been met, which is the concrete payoff of the whole arrangement.**
`SVGElement` (global constructor) and `document.currentScript` landed, and on
2026-08-17 **vue and webpack went from a blank page to a rendered, interactive
one** — `#app` 0 -> 106 and 0 -> 148 characters, both buttons reading
"count is 0", both frameworks' missing-cause counts falling to zero. svelte is
still blank, and its cause is named: `HTMLTemplateElement.content`.

The focus model is the other half. `-DBROWSER_NO_FOCUS` compiles the routing out
— no element takes focus from a click, Tab does not move it, a keystroke goes to
`<body>` as it did before — and `test-forms-negctl` must FAIL against that
build, or the suite is not measuring the focus model.

### Cookies — one jar, TWO doors, and the gate only knew one

`c/net/http/cookies.c` (RFC 6265 + 6265bis) is one jar reached from two places
that must classify a request the same way, and for a while did not:

- **fetch()/XHR** — `js_webapi.c`, computes cross-site from `g_loc` and passes
  it. Correct since the day it was written.
- **the transport** — `webapi_cookie_line()`, called by `browser_rt.c` for the
  navigation and for **every subresource**. It went in wired to
  `cookie_header()`, which is `cookie_header_ex(..., CK_REQ_SAME_SITE, ...)`
  with the argument fixed. Every request the browser made was declared
  same-site, so a cross-origin `<script src>` carried the target's
  Secure+HttpOnly+SameSite=Strict session and the reply was evaluated in the
  requesting page's realm.

**`test-cookie-cors` is titled "which requests carry the session, and which"
and could not see it**, because every case in it drives `fetch()`. That is the
lesson worth carrying out of this subsystem: *a gate aimed at a rule has to be
aimed at every caller of the rule, or it certifies the caller it happens to
know.* Same shape as the WPT runner that linked `layout.c` and never called
`layout_page()` — linking a TU is not running it; testing one caller is not
testing the rule.

Three things to know before touching it:

- **The request kind is three-valued** (`CK_REQ_SAME_SITE` /
  `CK_REQ_CROSS_SITE` / `CK_REQ_CROSS_SITE_NAV`) and it used to be two,
  because the two were argued from the only caller at the time: *"a fetch is
  never a top-level navigation, which is the only thing Lax relaxes for."*
  Then the transport arrived, which carries navigations. **SAME_SITE is 0 and
  permits everything**, so an uninitialised int is the DANGEROUS value here —
  which is why it is a parameter and every caller computes it.
- **`CK_HEADER_MAX` is one number because it used to be three**, disagreeing
  by 8x: 4096 for `document.cookie`, 2048 for fetch, **1024 for the navigation
  and every subresource**. A 1100-byte token (JWT / OIDC / cf_clearance) was
  readable from script while the page load sent no `Cookie` header at all, and
  a realistic bilibili-shaped set of 1039 bytes lost exactly one cookie on the
  wire — the NEWEST, because 5.4 orders by longest-path-then-earliest-created,
  i.e. the anti-bot token a WAF had just set. 8192 is what *servers* accept
  (nginx/Apache default), not what the jar could hold.
- **HttpOnly used to make a cookie the PREFERRED eviction victim** of the
  script it hides from. Three correct lines: a script read never matches an
  HttpOnly cookie; only cookies actually sent get `accessed = now`; the victim
  is the smallest `accessed`. So every `document.cookie` read renewed exactly
  what the page could see. 49 writes evicted the session, before the page's
  own cookie. `evict_lru` is two passes now — preference, not prohibition.

`cookie_header_ex` returns `CK_E_NOFIT` rather than 0 when cookies apply and
none fit; 0 now means only "the user has none". The old fold was **pinned as
correct** by `cookie_test.c`'s own buffer case.

  `make test-cookie-jar` · `test-cookie-jar-negctl` (four collapses, each
  reddening exactly its own count: 1/1/1/2) · `test-cookie-cors` ·
  `test-cookie-cors-negctl` (the shipped wiring on a `-D` switch)

### IPv6 — no `net_cfg.ip`, and ND is not ARP

Measured the same way the H.265 gap was, and it is the worse of the two: the
networking sections above run from M9 to M12 and say "net" thirty-seven times,
so a reader does not conclude the documentation is thin here — they conclude the
stack is **IPv4-only**. It is not.

`c/net/ip/`: `ip6.c` (477 lines), `nd.c` (882), `ip6_addr.c`. Eight targets,
**all eight green** in the full sweep: `test-ip6-host`, `test-nd-host`,
`test-ip6-dns`, `test-ip6-fallback`, and a negative control for each.

The two shape differences the source argues, worth knowing before touching it:

- **There is no `net_cfg.ip`.** An interface holds SEVERAL addresses at once —
  always a link-local one, usually one or more routable — each with an RFC 4862
  state and two lifetimes. Which one sources a packet is decided **per
  destination** by RFC 6724, in `ip6_addr.c`. Every "the machine's IP address"
  assumption from the IPv4 side is wrong here.
- **Neighbour Discovery is not ARP.** ARP is its own ethertype with a flat
  cache and no state; ND is ICMPv6 — it runs OVER IP, over multicast, with a
  five-state per-neighbour machine, and it carries the host's whole address
  configuration as a side effect. `nd.c` implements one protocol that does the
  job of ARP, ICMP redirects, router discovery and DHCP (RFC 4443, 4861, 4862).

`test-ip6-fallback` is the one to know by name, and its own comment says why:
it drives the dual-stack socket state machine (`c/net/core/sock.c`) against a
model TCP and asks two things -- does a preferred-but-BLACK-HOLED IPv6
destination actually end up fetching over IPv4, and does a v4-only answer behave
EXACTLY as it did before IPv6 existed (one connection, no race, same order)?
The second question is the one that matters most: it is what stops an
IPv6-capable build from making every IPv4 network slower.

### H.265/HEVC

Written down because the omission is the mirror image of a stale claim: there is
no sentence here to correct, so a reader concludes the decoder does not exist or
is not tested. Before 2026-08-17 the string "H.265" appeared in this file
exactly once, as an example of a target that fails when a sweep runs it in
parallel.

`c/lib/video/h265*.c` — nal, cabac, pred, mc, deblock — and **nine gates, eight
green** in the full sweep:

| | |
|---|---|
| `test-h265` | the bit-exact list. Anything not exact is NOT in it and is claimed nowhere |
| `test-h265-diff` | the whole matrix including the failures — "the honest picture", per its own header |
| `test-h265-m10` | **Main 10.** Not a lower bar: the 10-bit cases are gated by `test-h265` exactly like the 8-bit ones, because 10 bits is a claimed feature rather than an experiment. `H265-OK 15 pictures bit-exact (10-bit)` |
| `test-h265-scaling` | scaling lists |
| `test-h265-units`, `-asan`, `-units-control` | modules, sanitised, with a control |
| `test-h265-b` | **the one red.** B slices, declared incomplete; `got 79 want 80`, one level |

The bar is H.264's: bit-exactness against a reference, not a tolerance.

**And one thing the gates do not cover, found by the sweep**: `test-vidbench-guest`
decodes h265 at 320x240 and 640x360 and h264 at 1280x720, then returns
`decode error -3` for **h265 at 1280x720**. The gate corpus is small frames, so
a size-dependent failure in one codec sits outside every one of the nine.

## Image decoders: what decodes, and what does not

Written down because the answer is not derivable from the file list -- **PNG,
BMP, ICO, WebP and inflate are RUST** (`rust/src/*.rs`, target
`x86_64-unknown-none`, linked as `$(RUST_LIB)`), while JPEG, GIF, SVG and EXIF
are C in `c/lib/image/`. The M13 line above still says
`lib/{inflate,png,gif,img}.c`; that has not been true since the Rust port.

| format | state |
|---|---|
| PNG | complete -- every bit depth (1/2/4/8/16), all five filters, Adam7, tRNS |
| GIF | complete -- animation, per-frame sub-rects, all disposal modes |
| BMP / ICO | complete, including RLE4 and 32bpp with a real alpha mask |
| JPEG | baseline **and progressive**; byte-exact against `djpeg -nosmooth` |
| WebP | VP8L (lossless) **and VP8 (lossy) key frames, with the ALPH alpha plane**; byte-exact vs `dwebp -nofancy` |
| SVG | on the shared engine -- its own filler is gone; fill AND stroke |

**Lossy WebP was the last hole and it is closed** (2026-08-16,
`rust/src/vp8*.rs`). It was the common one: essentially every WebP a website
serves is `VP8 `, not `VP8L`, and every one of them used to be a broken-image
box. The whole key-frame path is here -- boolean entropy decoder, frame header
(segmentation, filter deltas, multiple token partitions, probability updates),
macroblock modes, coefficient tokens, dequantisation, inverse WHT and DCT, all
sixteen intra predictors, and both loop filters -- plus the ALPH chunk, because
VP8 has no alpha channel of its own and a transparent WebP is a VP8 frame with
a separately-coded 8-bit plane beside it. Inter frames are refused by name; a
WebP still image is always a key frame, so this is the complete decoder for
what WebP is, not a subset of it.

  `make test-webp-vp8`  31 cases, **every one byte-exact** against
  `dwebp -nofancy` on the identical bytes -- 700k samples, zero differences.
  `make test-webp-vp8-negctl` · the VP8 corpus is in `test-img-fuzz` too.

Four things worth knowing before touching it:

- **THE TABLES ARE GENERATED, NOT TYPED** (`tools/gen_vp8_tables.py` ->
  `rust/src/vp8_tables.rs`). 3,164 probabilities and tree indices, lifted
  mechanically out of RFC 6386's own reference-decoder C source, with the
  enum names resolved from the RFC's own typedefs and every table's shape and
  checksum printed. This is not tidiness: a wrong probability does not shade a
  pixel, it desynchronises the arithmetic decoder into noise, and one wrong
  byte in three thousand is not findable by looking. The generator refuses
  rather than guesses -- it takes only real declarations (five of the tables
  also appear as *arguments* elsewhere in the document), it requires every
  occurrence of the right shape to agree, and it strips the RFC's page
  furniture first, which it did not at first and which is how "Bankoski" came
  to be parsed as an enumerator.
- **A B_PRED subblock on the macroblock's right edge takes its above-right
  samples from the row above the MACROBLOCK**, for all four subblock rows --
  not from the reconstructed subblock diagonally above it. This is the
  format's most-reimplemented bug. `--features vp8-tr-from-subblock` is it on
  a switch, and it reddens 19 of 31 cases: the twelve that survive are the
  smooth and low-quality ones the encoder never coded as B_PRED, which is the
  control showing which cases carry the property rather than merely that the
  suite runs.
- **The loop filter is a second pass over the finished frame**, not per
  macroblock. Intra prediction reads its neighbours' UNFILTERED samples; a
  decoder that filters each macroblock as it completes feeds filtered pixels
  into the next row's predictor and drifts further with every row.
- **`-nofancy` is doing real work in the oracle.** libwebp's default is a
  4-tap chroma upsample; ours is box, as jpeg.c's is. Matching the fancy
  upsampler too is a separate job, and folding it in would make one number
  answer two questions. What this gate proves is that the DECODER is exact.

The second negative control, `vp8-dc-always-avail`, removes the rule that
DC_PRED is the only mode which asks whether its neighbours exist (20 of 31).
A third was written and **deleted**: clearing the Y2 non-zero context on every
skipped macroblock is a real rule, and no case in this corpus reaches it, so
the control passed. A control that cannot be watched failing is worse than no
control, because it reads like one.

**Progressive JPEG** (2026-08-16) is a SECOND path inside `jpeg.c`, not a
generalisation of the baseline one, and the file comment argues why: baseline
never holds more than one block of coefficients, while progressive must hold
the whole image until EOI (~3 bytes/pixel at 4:2:0), so merging them would
charge every ordinary JPEG progressive's memory. Three things to know before
touching it:

- **A scan naming ONE component is non-interleaved and walks that component's
  OWN block grid**, `ceil(ceil(W*h/hmax)/8)` wide -- *not* the padded MCU grid.
  The two differ whenever the image is not a whole number of MCUs, which is
  most images, and getting it wrong shears the picture rather than blanking
  it. `-DJPEG_PROG_MCU_GRID` is that bug on a switch, and it fails **only**
  `prog_422`/`prog_420` at 23x17 -- at 64x48 the image IS whole-MCU, the two
  grids coincide, and the bug hides completely. That asymmetry is why both
  sizes are in the corpus.
- **Inside an EOB run, already-nonzero coefficients still each take a
  correction bit**, in band order. Skipping them leaves those bits in the
  stream for the next block to misread; `-DJPEG_NO_EOBRUN_CORRECTION` reddens
  all seven progressive cases and not one baseline case.
- Coefficients are `short`, as in libjpeg, and every shift a crafted file could
  push out of range is **checked and refused** rather than wrapped: a DC
  category of 11 with `Al=13` is representable in a file and not in the buffer.

The oracle is `djpeg -nosmooth -dct int` over the identical bytes, and the
result is **maxd=0 on all 13 decode cases**, baseline and progressive alike --
exact, not within tolerance. `jpeg_gen.py` additionally asserts that libjpeg
decodes the baseline and progressive encodings of one source to identical
pixels, because that is what makes the two reference files comparable at all;
if it ever stops holding the generator stops rather than quietly weakening.

  `make test-jpeg` `test-jpeg-negctl` `test-img-fuzz` -- the progressive corpus
  is in the fuzz corpus now, and was not before, so SOF2 had never been handed
  a malformed byte. 200,871 mutated decodes under ASan+UBSan+leak-check, clean.

## The BKL: what it actually costs, measured

**The concurrency model is one lock taken on every kernel entry**
(`c/kernel/cpu/interrupts.c`, one acquisition site, `syscall_is_bkl_free()` as
the allow-list). Everything below is a number off this machine, because
"the big kernel lock is the bottleneck" is a design statement and the numbers
disagreed with the obvious next move twice.

**FIRST MEASUREMENT (`make test-kbench`), and it was not about the lock:**

```
BKL: 3,299,718 acquisitions, 13,712 contended (0%)
syscall: 3,283,157 calls
  SYS_POLL_EVENT (8):  1,640,961 (49%)
  SYS_YIELD     (12):  1,640,961 (49%)
```

98% of every kernel entry was an application taking the global lock to be told
nothing had happened. That is not lock contention, it is traffic --
`SYS_WAIT_EVENT` (165) deleted it:

```
                      before        after
syscalls            3,283,157       1,234       2,660x
BKL acquisitions    3,390,115      18,571         183x
BKL time holding      4,211 ms    2,500 ms
BKL time waiting      6,685 ms    6,549 ms       <-- read this row
```

**The waiting barely moved**, and that is the finding, not a disappointment:
what went away was 3.4 million cheap acquisitions, and the wait that remains
was always real work -- it was merely diluted. Contention "rising" 0% -> 15%
is the same arithmetic: the denominator collapsed.

**SECOND MEASUREMENT, which refuted the obvious next step.** The plan was to
widen `syscall_is_bkl_free()`. `spinlock_t` now records the acquiring caller's
return address, and `kb_bkl_sample()` counts holders from inside the timer
tick -- which runs BEFORE the interrupt entry takes the lock, so the observer
is not itself a holder:

```
[kbench] BKL holders: 603 samples, held in 129 (21%)
  wm_run+0x2de              55% of held      <- the compositor
  interrupt_handler+0xc2    32%
  schedule / block_self / sched_become_idle   3-4% each
```

**No syscall appears at all**, so widening the allow-list would have moved
nothing. The lock is FREE ~80% of the time; it is a bottleneck because of who
holds it and for how long at a stretch.

**DO NOT QUOTE THE 55% AS A PRECISE NUMBER.** Three runs of this profile on four
cores have reported the compositor's share as 55%, 63% and 80%. That is the
sample size, not the machine: ~600 samples of which only ~100-130 catch the lock
held, so one percentage point is about one sample and a 25-point spread over
three runs is exactly what n~100 looks like.

The robust version is the same gate on ONE core, which `make test-kbench-1core`
already runs:

```
1 core   605 samples, held in 126 (20%),  top holder 99% of held
4 cores  603 samples, held in 101 (16%),  top holder 63%, second 27%
```

At one core the compositor is **99%** of held time. It is not the largest of
several holders, it IS the holder -- and the interrupt entry's share at four
cores is four times as many interrupts arriving, not a second bottleneck
emerging under load. `wm_run+0x2de` is the
`spin_lock(&g_bkl)` after the idle `hlt`: the compositor re-takes the global
lock on waking and holds it through the whole frame. The frame's own
accounting says where that goes -- **16.1 ms composite, 0.81 ms present**, so
releasing the lock around `fb_present()` would recover 5% of it and is not the
answer either. Splitting the composite is a project, not a patch: a full-screen
frame is ONE damage rectangle, so releasing between rectangles does nothing for
the worst case (34.6 ms).

**THIRD MEASUREMENT: the desktop now idles at zero.** `echo start >
/dev/kprof` over 12 s of live desktop, 49,283 samples:

```
sched_become_idle+0x2c   49.1%
file_read+0x21c          24.9%
wm_run+0x2d3             24.5%
```

All three are the instruction after a `hlt`. **98.5% of the machine's samples
are halted cores.** Read `file.c`'s comment above `tty_read` before quoting
this at anyone: a sampling interrupt on a halted core records the RIP
*following* the halt, so a core doing nothing is indistinguishable from a core
in a tight loop -- which is exactly how these three addresses were once read as
two busy-waits eating half the machine.

**AND THE LOCK THAT ACTUALLY SERIALISED THIS MACHINE WAS NOT THE BKL.**
`make test-smp` had been failing with "no wall-clock speedup (kmalloc still
serialized by the BKL?)". `tests/boot/run-smp-lockprobe.sh` samples every
lock's ticket counter across that workload -- on a RUNNING machine, no freeze
required -- and the guess in that message was wrong by 834x:

```
kheap_lock   267 -> 30,720,350     (+30.7 MILLION)
g_bkl      6,473 ->     43,309     (+36,836)
pmm_lock   2,740 ->      3,044     (+304)
```

`SYS_KHEAP_STRESS` is the ONE entry on `syscall_is_bkl_free()`'s allow-list, so
the BKL was never in it. **Per-core magazines** in front of the allocator
(`c/kernel/mm/kheap.c`) took that workload from 30.7 M acquisitions of
`kheap_lock` to about 112, and `make test-smp` from **T1=5s TN=41s to T1=5s
TN=6s** -- four cores doing four times the work in 1.2x the wall clock, where
it used to take eight times longer than serial.

Four things about that layer, each load-bearing, argued at the magazine block
in `kheap.c`:
- **Exact size classes only** (16/32/.../512). A pop is always a perfect fit,
  so there is no search, no split, and no "close enough" that would turn the
  magazines into a second and worse free list.
- **A lock per core, not lock-free.** The fast path takes its OWN core's lock,
  so a kmalloc from an interrupt cannot corrupt the magazine of the thread it
  interrupted. The win was never that the atomic disappeared -- it is that four
  cores no longer queue for the same one.
- **A block in a magazine is still ALLOCATED**: `kfree` there does not clear
  `F_FREE` or decrement `st_live`, so the double-free refusal still fires and
  `kheap_audit`'s arena walk still sees only two states.
- **Drain before OOM.** Returning NULL with blocks parked in magazines would be
  an out-of-memory that is not true.

**Instruments, all reusable:**
  `make test-kbench` · `tests/boot/run-smp-freeze-probe.sh` (every core's RIP,
  the lock each waits on, and every lock's ticket/serving/holder at a freeze) ·
  `tests/boot/qmp_lockdump.py` · `/dev/kprof` · `make bench-gfx-frame`

## The test suite: 610 targets, and a suite reaches 53

**AND "UNWIRED" DOES NOT MEAN "CI DOES NOT RUN IT" -- 328 of the 350 are run.**
This paragraph called the unwired count "the single most load-bearing fact
about testing in this tree" and had everything below follow from it, and the
number is real but it is not that fact. Measured 2026-08-17:

    CI runs (audit_tests.py --suites=host + =boot)   358
    audit says UNWIRED                                350
    UNWIRED **and yet run by CI**                     328

`tools/ci.sh` does not use the aggregates at all. It asks
`audit_tests.py --suites=host|boot`, and that path calls `classify()`, which
derives host-vs-boot **from the recipe** and skips only `NOT_CI` -- it never
consults `wired`. That is deliberate and the function says so: *"A hand-written
list of 'the suites CI runs' is the thing that rotted here ... so the CI asks
the Makefile instead, and a target added tomorrow is picked up without anyone
remembering."* So UNWIRED measures reachability from `ci-host:`/`ci-boot:`
declarations -- a mechanism CI stopped depending on -- while reading like
coverage. Both numbers are worth having; only one of them is about what runs.

**The half that IS a coverage hole is the negative controls, and it is 55 of
them.** `NOT_CI` drops every `test-*-negctl` from the suite listing, on the
stated ground that a control is "RUN BY its positive counterpart". Nothing
checked that. A control that is a separate target and is named by nobody is
excluded by the regex AND invoked from nowhere -- run never, while looking
exactly like a control that is covered. Counted 2026-08-17 by the new
STRANDED CONTROLS category: **55**, recorded by name in
`tests/audit-stranded.baseline` and gated on growth, the same shape UNWIRED
uses and for the same reason -- 55 findings would bury DEAD and MUTE, whose
whole value is being zero.

**The fix for one entry is a single line: `test-X: test-X-negctl`.** Naming it
beside the positive on a `ci-host:` line instead satisfies UNWIRED and still
runs it never, which is the worse of the two failures because it looks fixed.
That distinction is why the category reports the fix in its own output.

**Every number in this section is a measurement with a date, and they move.**
As of 2026-08-17 (second measurement that day, after a wiring pass): 610
`test-` targets, 53 wired into a suite, 350 unwired (all recorded by name in
`tests/audit-unwired.baseline`, and the gate is that the set does not GROW),
14 DEAD harnesses and **0 MUTE**.

**31 -> 53 wired is not 22 new tests; it is 22 that already existed being
named.** `ci-host:` and `ci-boot:` accept prerequisites from any fragment, so
membership is one line in the file that owns the target, and the audit's
"UNWIRED (NEW)" list is what says which line is missing -- it named seven the
day they landed, and four of those were `test-cells`'s siblings under a parent
(`test-term-host`) that no suite reached either. **Wire the parent, not the
member**: wiring `test-cells` alone would have left the other six in the same
state one level up. `--bless-unwired` REGENERATES the baseline rather than
appending to it, so a wiring pass must re-run it -- otherwise the file keeps
recording as debt things that are no longer debt, and the next person to
un-wire one of them is not flagged.

`test-audit` still exits non-zero, and it is worth knowing why before reading
it as a regression: all 14 remaining findings are the DEAD category -- QMP
drivers under `tests/qmp/` that no Makefile target names. Unlike UNWIRED,
DEAD has no baseline, so the gate has no way to record a deliberate decision
about a manual tool. The sweep classifies 530 of
them: 337 host, 156 device, 3 that need an argument, and 34 aggregates it skips
because it already runs every one of their members individually.

Re-measure before quoting: this paragraph was 522/352 for long enough that the
gap became the finding.

**A gate nobody runs is a gate that rots, and it rots silently.** One sweep of
all 522 found five host targets that had stopped BUILDING -- not failing, not
flaky: not compiling -- each because a source file grew a dependency and a link
line did not follow:

```
test-fb-clip        fb.c -> gfx_mask_corner, glass_build_lut, gl_isqrt
test-wpt-* (4)      svg.c -> gfx_path/gfx_fill (phase-2 stroke)
test-semantics      the same
test-css-web-negctl canon.c -> floor()  (missing -lm)
test-leak           fault.c, vma.c -> pcache_* (the file-backed page cache)
test-libc-diff      three separate breaks, one of them a trap: see below
```

**Run everything with `make test-sweep`.** Host targets in parallel, device
targets one at a time, then EVERY failure re-run alone before it is believed --
because six makes share one `build/` and two racing to produce the same object
make a target fail for reasons that have nothing to do with it. Measured, not
feared: `test-audio-codec-fuzz-deep`, `test-h265`, `test-demux` and
`test-csstext-all` all failed in the parallel phase and pass by themselves. A
sweep that reports those is MANUFACTURING bugs, which is worse than missing
them. `make test-sweep-host` is the half worth running after an ordinary change
(minutes); the full one boots QEMU about 156 times and takes an afternoon, which is
why it is not wired into `test` or `ci`.

**THE MIRROR-IMAGE TRAPS IN `test-libc-diff`.** The header of `tests/libc.mk`
already documents one: the "ours" build still LINKS glibc, so a missing
implementation TU is a runtime FALLBACK, not a link error. The sweep found the
other half. `setrlimit` was absent from `tests/unit/libc_rename.h`, so OUR
`setrlimit` overrode glibc's for the whole process -- and ASan calls it from
`DisableCoreDumperIfNecessary()` during its own init, before `main`. The result
was a segfault inside `__asan_init` with no output whatsoever, which reads like
a broken test binary rather than a missing `#define`. **A missing rename does
not merely fail to test our version; it silently replaces the reference.**

**`test-audit` is the meta-gate and its MUTE category is the one to watch**:
"computes a verdict and exits 0 anyway", i.e. a test that cannot fail. It is
EMPTY now, and getting there meant fixing the detector rather than the tree --
24 of its 28 entries were false, including `qmp_desktop_look.py`, which half of
one day's commits cite as evidence. Three blind spots, each a shape this tree
actually uses: `sys.exit(main())` propagates a status the literal-integer
search cannot see; a shell script's exit status is its LAST COMMAND'S (`exec
python3 ...`, a bare `[ "$ok" -gt 0 ]`, `set -e`); and a harness no target runs
cannot make a gate pass wrongly, so listing the dead ones twice buries the few
that can. The ones that remain are declared in `ALLOW_MUTE` with a reason each
-- a library, two scoreboard REPORTERS whose per-site FAIL is the measurement
they exist to produce, and a fixture builder. **An empty category is the point:
a list of 28 with 24 false entries is a list nobody reads, and the 29th -- a
real gate that swallowed its verdict -- joins it unnoticed.**

Each milestone: spec → plan → implement. Specs in `docs/superpowers/specs/`.

language=chinese
