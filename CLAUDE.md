# Aether OS — notes for Claude

A from-scratch x86_64 OS kernel (C + nasm), booted via GRUB/Multiboot2, aiming
toward a macOS-style desktop. Real kernel, not a simulation.

## Build / run / test

```sh
make        # -> build/aether.iso
make run    # QEMU: VGA window + serial on terminal
make test   # headless; asserts kernel prints AETHER_BOOT_OK on serial
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
  positives unless `.clangd` is being honored — the real build passes `-I` for
  every source dir (`INCDIRS` in the Makefile) and the x86_64 target.

## Source layout

All source lives under `src/`, headers **colocated** with their `.c`. Header
names are unique, so the Makefile's `INCDIRS := $(shell find src include -type d)`
makes every `#include "foo.h"` resolve without path qualifiers. `include/` keeps
only the cross-cutting kernel↔user ABI (`include/abi/aether_abi.h`).

Tests live under **`tests/`** (moved out of `tools/` in the 2026-06-09 declutter):
`tests/unit/` = host unit/fuzz tests (`make test-tcp-host`/`test-as`/`test-png`/…)
+ `tcpstub/` + generators; `tests/boot/` = QEMU boot harnesses (`run-*.sh`, driven by
`make test`/`test-nvme`/`test-shell`/`test-libc`/`test-smp`); `tests/qmp/` = QMP
mouse/keyboard/screenshot drivers. `tools/` is now **build tools only** (mkaex,
mkfs, mkfont, genroots, gen_compile_commands, gen_libcss, mkwallpaper).

```
src/boot/                                        multiboot + long-mode entry (asm)
src/kernel/{core,cpu,mm,sched,exec,gui,pci}/     kernel by subsystem
src/drivers/{char,timer,block,net}/              device drivers
src/fs/                                          vfs + aetherfs
src/net/{link,ip,transport,core,dns,http,tls}/   network stack
src/crypto/{hash,aead,pubkey,trust}/             from-scratch crypto
src/lib/{image,text}/ + string.c                 shared libs (png/gif/ttf/utf8…)
src/apps/                                        shared: aether.h clib.h crt0.asm crt0_cli.asm
src/apps/gui/                                     windowed apps: clock textedit monitor terminal netapp widgets
                                                 + aui.{h,c} = immediate-mode widget toolkit (linked into each)
src/apps/coreutils/                              sh + coreutils (ls cat echo wc head …)
src/apps/as/                                     AetherScript language (M20): /bin/as
src/apps/browser/                                browser + render engine (dom, layout,
                                                 css_engine, browser_paint, js_dom) — also links QuickJS
src/apps/libc/                                   mini-libc (string/stdio/malloc/setjmp…)
```

**File paths quoted in the Notes below are pre-reorg names** (e.g. `net/tcp.c` is
now `src/net/transport/tcp.c`, `kernel/wm.c` → `src/kernel/gui/wm.c`); basename +
subsystem are unchanged, so they're easy to find under `src/`.

## Roadmap

M1 Boot & Hello ✅ · M2 interrupts + keyboard ✅ · M3 memory (PMM + heap) ✅ ·
M4 multitasking (preemptive scheduler) ✅ · M5 storage (ATA + AetherFS + VFS) ✅ ·
M6 userland (GDT/TSS + ring3 + int 0x80 + ELF loader) ✅ · M7 graphics
(framebuffer + VMM + Aether desktop) ✅ · M8 window system (font + double-buffer +
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
`lib/ttf.c` + `kernel/raster.c` + `kernel/text.c`): UTF-8 decode, a from-scratch
TTF parser (cmap fmt4/12, glyf simple+composite, hmtx) and an integer-only AA
rasterizer (4× vertical oversample + fractional horizontal coverage → 0–255
alpha), with a glyph cache + font fallback. `fb_text` routes through it, so the
whole UI is anti-aliased; the Terminal uses `text_draw_mono` (SYS_GUI_TEXT_MONO).
Fonts live on the AetherFS disk (`/fonts/{ui,mono}.ttf`, subset by
`tools/mkfont.py`: Heiti SC GB2312 + Menlo, glyf), loaded by `text_init()` after
fs mount; QEMU `-m 512M`. The CJK font is 6 MB, so AetherFS gained **double-
indirect** inodes (`fs/aetherfs.c` + `tools/mkfs.py`; files >4 MB). The 8×16
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
- Disk: QEMU `-drive ...,if=ide` primary master; `drivers/ata.c` (PIO
  read+write) + `fs/aetherfs.c` + `fs/vfs.c`. AetherFS is a **hierarchical,
  read-write inode FS** (on-disk v3, 4 KiB blocks: superblock, free-block
  bitmap, inode table with direct[12]+single-indirect, directories = inodes of
  dirents). Subdirectories + files up to ~4 MiB; `vfs_*` are path-based. Build
  the image with `tools/mkfs.py` (Makefile `$(DISK)` packs `fsroot/*`, a seeded
  `/docs/`, and each app's `.aex` at the root).
- Userland: `kernel/gdt.c` (TSS rsp0), `boot/enter_user.asm`, syscalls via
  int 0x80 in `kernel/syscall.c`; `user/` builds the ring-3 ELF.
- M8 window system: `tools/genfont.py` -> `include/font8x16.h` (committed, no PIL
  at build). `kernel/fb.c` draws into surfaces (`fb_target`/`fb_blit_surface`)
  with a screen back buffer + `fb_present()`. `drivers/mouse.c` = PS/2 mouse
  (IRQ12); `drivers/rtc.c` = CMOS wall clock (menu bar + Clock app).
- M9 networking: `kernel/pci.c` (0xCF8/0xCFC config) + `drivers/e1000.c` (QEMU
  e1000 8086:100E, MMIO BAR, RX/TX descriptor rings, **polled** — no NIC IRQ).
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
  `src/crypto/trust/roots_bundle.inc`) -- a near-full mirror of the host
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

## Application platform (on top of M8)
- Apps are `.aex` files on the AetherFS disk = real **ring-3 processes** scheduled
  by M4. `kernel/wm.c` is the window manager AND the GUI/app backend.
- Executable format: `include/aex.h` (header wrapping an ELF), built by
  `tools/mkaex.py`; loaded by `kernel/aex.c` (reuses `elf_load`). Each app links
  at a distinct base (Makefile APP_RULE: clock 0x40000000, textedit 0x41000000,
  monitor 0x42000000, terminal 0x43000000). Single instance per app.
- Process model: `thread_create_user` (sched.c) spawns a ring-3 thread with its
  own kernel stack; `schedule()` sets TSS rsp0; `thread_exit()` reaps it.
  `sched_current_data()` maps the running thread to its `struct app`.
- ABI: `include/aether_abi.h` (shared with userland `user/aether.h`). Syscalls via
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
- Adding an app: write `user/<name>.c` (include "aether.h", define `app_main`),
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
off (`-DAETHER_OS` guards `CONFIG_ATOMICS`; single-threaded). Builds via the
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
Aether). A POSIX-ish process model independent of the window manager:
`src/kernel/exec/{proc,file,exec}.c`. **proc.c** = a PCB table (pid/ppid/state/
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
"file not found"; `drivers/block/ata.c` now retries the command 8x. Known-open
aetherfs issues (separate): cross-boot write durability (corrupts after repeated
non-snapshot boots; use `-snapshot`) and under-enumeration of runtime-`mkdir`'d
dirs. `tests/qmp/qmp_term.py` drives the GUI terminal; QMP key injection must be slow
(PS/2 1-byte buffer).

M19 virtio ✅ (the "VGA is too primitive" follow-up): a modern (virtio 1.0)
paravirtual device stack in `src/drivers/virtio/` replacing the legacy devices.
`virtio.c` is the virtio-pci transport (parses the vendor-0x09 PCI caps to find
the modern cfg structures in BAR4, negotiates VIRTIO_F_VERSION_1, sets up split
virtqueues, synchronous request/poll). **virtio-blk** (`virtio_blk.c` +
`drivers/block/blkdev.c`) replaces ATA PIO as the disk (aetherfs bread/bwrite go
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

Pre-M20 prerequisite — **mini-libc 大补**: `src/apps/libc` grew into a real
freestanding C lib. `io.c` is the single errno/syscall TU (POSIX wrappers over
int 0x80); `stdio.c` is now **fd-backed buffered FILE I/O** (fopen/fread/fgets/
fseek/…); added `fcntl.h`/`unistd.h`/`setjmp.h` (+`setjmp.asm`), `strtok`/`memmem`.
Also fixed stdio bugs (short-write loss, `%g` trailing zeros, `%*`/`.*` neg width,
round-half-to-even). Only browser/JS + `/bin/as` link mini-libc; CLI coreutils use
`aether.h` inline syscalls. IDE: `tools/gen_compile_commands.py` + a self-sufficient
`.clangd` (full INCDIRS) kill the host-SDK false-positive squiggles.

M20 AetherScript ✅: a **from-scratch language**, `as`/`.as`, in `src/apps/as/`
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
test-as` (host, 55 checks incl. fib + import) + `make test-as-os` (boots Aether,
runs the examples incl. an import demo over serial). Perf: fib(32) ~126ms host
(≈CPython). Deferred: dict, closures, GC, computed-goto dispatch.

Each milestone: spec → plan → implement. Specs in `docs/superpowers/specs/`.

language=chinese
