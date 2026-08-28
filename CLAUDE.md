# LogitOS — notes for Claude

A from-scratch x86_64 OS kernel (C + nasm + a little Rust), booted two ways —
GRUB/Multiboot2 and its **own UEFI loader** (`c/boot/efi/`) — aiming toward a
macOS-style desktop that runs software not written for it. Real kernel, not a
simulation.

**This file was rewritten on 2026-08-28** after every falsifiable claim in the
previous version was checked against the tree: **482 claims, 173 of them stale or
false** (79 load-bearing, 53 outright false), and **108 components the document
never mentioned at all**. Numbers below carry the date they were measured. The
previous version's most expensive habit was a present-tense sentence about a
world that had moved; where that happened the correction is kept beside the old
claim rather than quietly overwritten, because somebody is going to arrive
holding the old sentence.

---

## READ THIS FIRST: this tree does not fully build on its own documented host

The Toolchain section below names **macOS / Apple Silicon** as the development
host. A large share of this tree's host gates **cannot compile or link there** —
not fail, not flake: not build. That was true for months and nothing noticed,
because `tools/ci.sh` does not run on Darwin either.

**`make test` — the first command in this file — is RED on this host.** It is
`test: test-crypto test-net $(ISO) $(DISK)`, and `test-crypto` shells out to
`test-cpufeat`, which runs CPUID **on the host** and so reports `0/56 present`,
`1622 checks, 12 failed` on arm64. It FAILS rather than SKIPS, and has since
`a37597829` (2026-08-07).

The causes are few and each takes down many gates at once. **Learn these five
shapes; they will be the reason your gate is red, and none of them is about the
code under test:**

| shape | what happens | what it took down |
|---|---|---|
| **Fortified `mem*` macros** | Apple's `<string.h>` makes `memset`/`memcpy` MACROS at *every* -O level (verified: `#ifdef memset` fires at -O0 and -O2). A file that declares them as bare externs *after* a host `<string.h>` expands the prototype into `__builtin___memset_chk` → "expected parameter declarator" | ~12 host gates via `layout.c`, `sock.c`, `ip.c`. **Fixed 2026-08-28** with `#ifndef memset` guards |
| **`__attribute__((weak))` on a DECLARATION is an ELF idiom** | `settings.c:9-22` states the idiom: "weak makes them resolve to NULL instead of failing the link". That is an ELF property. On Mach-O an undefined weak symbol is a **hard link error**, so the protection the comment promises does not exist here | `test-layout-box`, `test-ip-route`, `test-raw-host`, `test-h2mux*`, `test-vfs-mount`, `test-mm`, `test-oom`, `test-procfs` |
| **Hand-copied source lists** | `CANVAS_SRC`, `PROBE_SRC`, `H2MUX_SRC`, `MSE_INC` are copies of a TU list the tree kept growing. A source file grew a dependency and the link line did not follow | `test-canvas` (quoted here as "46 checks" and unobservable since), `test-frameworks`, `test-platform-*`, `test-mse*`, `test-demux-expect`, `test-tcp-host` |
| **Shell and coreutil differences** | stock macOS `bash` is 3.2.57, where `"${arr[@]}"` on an empty array is fatal under `set -u`; BSD `wc -l` pads to `     717` so a string compare against `717` fails | `test-tls-interop` (exits 2 having run **zero** cases), `test-tls-server`, `test-crypto-diff-control` |
| **Host capability absent, gate FAILS instead of SKIPPING** | darwin/arm64 ASan has no leak detector; there is no x86 CPUID; `emmintrin.h` refuses to compile | `test-cpufeat` → `test-crypto` → **`make test`**; `test-demux-fuzz`, and worse its **control passes for the wrong reason** — the ASan abort satisfies it without ever reaching the injected bug; `test-nn`, `test-lm-*` |

**The rule that follows, and it is the one to apply to any gate you write:** a
gate that cannot run on this host must **skip loudly** — one line naming the
missing capability and the command that would settle it — and never pass
silently. A gate that fails for a reason unrelated to the code under test is
noise that trains people to ignore red.

**And check the apparatus before believing any of the above is fixed.** These
were measured on 2026-08-28; a fix pass was in flight the same day.

---

## Build / run / test

```sh
make        # -> build/logit.iso        (the KERNEL only -- see below)
make run    # QEMU: a virtio-gpu window at 1920x1200 + serial on the terminal
make shot   # boots headless, screendumps over QMP, writes a PNG
make test   # headless boot; asserts LOGIT_BOOT_OK -- and see the caveats
make debug  # QEMU frozen with a gdb stub on :1234
```

- **`make run` gives the guest 1 GiB, and THAT NUMBER IS A CEILING RATHER THAN
  A PREFERENCE** (raised from 512 MiB on 2026-08-29 because the browser peaks
  near 600 MB). `c/boot/boot.asm:80` identity-maps EXACTLY the first 1 GiB — one
  PD of 512 2 MiB pages — and `mmhost.h:43` makes that an assumption the whole
  kernel rests on, in its own words: "the kernel identity-maps the low 1 GiB, so
  phys == virt". **Nothing clamps the allocator to it**: `pmm.c:251` takes
  `total_frames` from the multiboot map and `pmm_init` frees every AVAILABLE
  region firmware reports, so above 1 GiB the PMM hands out frames it cannot
  address and `next_table()`'s `memset(mm_p2v(frame), 0, 4096)` writes to a
  VIRTUAL address in PDPT[1] — **the USER region, where every GUI app is linked
  (0x49000000, 0x50000000)**. And the failure shape is the bad one: `pmm_alloc`
  scans from low, so a 2 GiB machine boots fine, runs fine, and starts
  corrupting only when demand pushes past the first gigabyte — exactly under the
  load that made someone want more memory. Going higher is the same wall as
  structural gap #2 and needs a physmap at a high virtual base (the seam exists:
  `mmhost.h`'s `MM_HOSTTEST` branch is already `mm_host_base + phys`) or user
  space moved out of PDPT[1]. The ~100 boot harnesses that spell `-m 512M` are
  deliberately untouched — `test-oom` and `test-swap` are calibrated against a
  memory size.
- **`make run` opens a virtio-gpu window, not a VGA one.** `QEMU_GPU := -vga none
  -device virtio-gpu-pci,xres=1920,yres=1200` (Makefile:1278). VGA survives only
  as `fb.c`'s multiboot-LFB fallback. The resolution is the single largest lever
  on how the desktop feels — see the performance section.
- **`make shot` is the cheapest instrument in the tree** and was absent from this
  block for months. Its own comment: *"the check that separates 'the OS is
  broken' from 'the window is not painting' — it reads the scanout the guest
  produced, with no host window involved."*
- **`make test` asserts more than LOGIT_BOOT_OK.** It runs `test-crypto` and
  `test-net` first, and `tests/boot/run-test.sh` additionally requires the trust
  banner to read exactly `EXPECT_ROOTS=130`, `EXPECT_SKIPPED=0`, and to name
  `isrg_x1` — so changing `tools/roots/` is *supposed* to redden it until
  somebody updates the number and says why.

**`make` alone does NOT rebuild a ring-3 program.** `all: $(ISO)` and `$(ISO):
$(KERNEL) grub.cfg` — every app is an `.aex` on `$(DISK)`, a separate target that
only `run` and the boot harnesses depend on. Editing `c/apps/browser/*.c` and
running `make` prints "Nothing to be done for 'all'" and leaves the old binary on
the disk image. `make build/disk.img` is the check. Nothing is broken here — the
ISO genuinely does not contain the apps — but "I ran make and it was fine" is not
evidence about anything above the kernel.

**`BUILD` is overridable and it reaches the recipes** (`make test-X
BUILD=/tmp/mine`). That is how several agents can build this tree at once without
manufacturing each other's failures — see "a sweep that manufactures bugs" below.

## Toolchain (macOS / Apple Silicon host, x86_64 target)

- Compile: `clang --target=x86_64-elf -ffreestanding` (clang cross-compiles natively)
- Link: **`ld.lld`** — Apple `ld` only emits Mach-O, so the LLVM linker is required (`brew install lld`)
- Assemble: `nasm -f elf64` (32-bit boot code lives in elf64 objects via `bits 32`)
- ISO: `i686-elf-grub-mkrescue` + `xorriso`; ESP: `tools/mkesp.py` (+ OVMF for `test-uefi`)
- Run: `qemu-system-x86_64` — **TCG only.** An x86_64 guest on an arm64 host has
  no hardware acceleration; `QEMU_SMP ?= -smp 4 -accel tcg,thread=multi`. Every
  timing in this file is under emulation and says so.
- The kernel is built **`-msse -msse2`** (Makefile:96) and has been since M15.
  `-mno-sse` appears nowhere. Anything in this file that explains a design by
  "the kernel cannot do floating point" is wrong; it can.
- IDE diagnostics about inline-asm constraints or missing headers are **false
  positives** unless `.clangd` is being honoured — the real build passes `-I` for
  every source dir (`INCDIRS`) and the x86_64 target. `kprintf`/`kmalloc`/`vfs_*`
  reported as undeclared is the signature.

---

## The five rules this tree has paid for

Each of these was learned by losing a day. They are not style.

### 1. Suspect the apparatus first

A sweep of every target once produced 45 failures; **most were the test, not the
system**, and the expensive ones shared a shape:

> **The measurement was right and the sentence around it sent the reader
> somewhere else.**

| it said | it was |
|---|---|
| "isolated forms are half again as wide as joined ones", 8 Arabic failures | a screenshot of Preview playing an *audio* file — every row read `444 px` regardless of input |
| `fresnel s=0: got 190, double says 255`, 26 failures | the oracle did not model a clamp the implementation argues for over twenty lines |
| "something is still polling instead of blocking" | ~50 passes *per core* against a budget of 200 that does not mention cores |
| a `TypeError` traceback in the harness | the harness had found a real bug one line earlier and then walked off the end of it |
| `0 px wrong` and failing | `rel=mismatch` — zero is the WORST outcome, printed in the words of the best |
| `test-swap` green | the workload completes without swap; an optimisation shrank the desktop and the calibration went hollow |

**And it kept happening on 2026-08-28**, four times in one afternoon, to the
instruments used to investigate a performance complaint:

- `settle_pointer` was constructed without a serial log, so it fell back to
  finding the arrow in a screendump — and this machine puts the pointer on the
  display's **hardware cursor plane**, so the arrow is not in the composite. The
  drag never grabbed anything and the profile that came back was four halted
  cores.
- A kprof parser's regex did not match kprof's actual row format and printed
  "0 samples over 0 sites" **directly under a header saying 8,471 samples were
  taken**.
- A QEMU boot died with "qemu exited early" because a stale `qmp.sock` sat in the
  working directory; QEMU will not bind a unix socket path that exists. It read
  as a broken guest.
- `test-ime-os` — the on-device gate for the whole input method — passes green
  while the feature is unusable, **by construction**: QMP injects scancodes
  beneath the host keyboard, so every link from a person's fingers to QEMU is
  bypassed.

Cheap checks: does the number change when the input changes? Does the control
fire? Does the same gate at another size or core count agree? Is the file it read
the file you edited? **Is the harness looking at the machine, or at itself?**

### 2. If you read the Makefile, join the continuations first

Six tools in this tree parse make. **All six are correct now** and each carries
the reason above it — `tools/audit_tests.py:147`, `tools/negctl_drift.py:153`,
`tools/mk_wired.py:41,76`, `tools/license_audit.py:260`,
`tests/boot/mk-tcc-disk.py:30` all do `re.sub(r"\\\r?\n[ \t]*", " ", text)` first,
and `tests/boot/sweep-classify.py` reads `make -pRrq`, whose output make has
already joined. **Do not go re-fix them.** What survives is the rule, and the
history of why:

- an `md5sum` of `make -n build/disk.img | grep -m1 mkfs.py` matched before and
  after a change, which is what convinced somebody the change was safe. It was
  comparing the first of several `mkfs` invocations, on a line the change never
  touched — and the change had made every app's host path empty. It shipped.
- `negctl_drift.py` lost any rule whose prerequisites wrap, and reported three
  browser variants where there were four. (There are **five** now: `browser.elf`,
  `browser-nofetch`, `browser-nostream`, `browser-nofocus`, `browser-noplat`.)
- `audit_tests.py` took `line.split(":", 1)[1]` as the prerequisite list, so
  `test-fs-boot: a b c \` lost everything after the backslash. **The count the
  audit exists to produce was inflated by its own parser.**

### 3. One jar, TWO doors

A constant that must agree between two places, spelled twice, agrees on the wrong
value about as often as the right one. This tree has paid for it three times:

- `/dev/log` vs `LOGIT_PATH_LOG` — mini-libc's `syslog()` and `/bin/syslogd` both
  said `/dev/log`, as every Unix has since 4.2BSD. On this machine `/dev` is
  **synthesised** and holds exactly eight control files, so nothing can be
  created in it by anyone. Both doors agreed, on a value that could not work.
- `LOGIT_ARG_MAX` — `sh.c` said 32 and `exec.c` said 48, and **each end silently
  truncated at its own number**: sh dropped the 33rd word of a line, exec dropped
  every argument from the 49th. One header now (`include/abi/logit_exec.h`).
- The IME toggle chord was spelled in **eight** literals — five kernel strings, a
  comment, a header sentence, and two harnesses. It is `IME_TOGGLE_NAME` now.

### 4. A gate nobody runs is a gate that rots, silently

Five host targets were once found to have stopped **building** — not failing:
not compiling — each because a source file grew a dependency and a link line did
not follow. That list has only grown; see the host-reality table at the top.

The mirror-image trap lives in `tests/libc.mk` and is worth internalising: the
"ours" build still **links** glibc, so a missing implementation TU is a runtime
**fallback**, not a link error. And the other half — a missing entry in
`libc_rename.h` does not merely fail to test our version, **it silently replaces
the reference**: our `setrlimit` overrode glibc's for the whole process, and ASan
calls it during its own init, producing a segfault inside `__asan_init` with no
output at all.

### 5. A control that cannot be watched failing is worse than no control

Because it reads like one. Live examples in the tree right now:

- `test-bidi-negctl` prints "negative control ok" on a machine where the Unicode
  corpus does not exist. The negctl binary exits non-zero — but because it cannot
  open `BidiTest.txt`, not because bidi resolution is absent.
- `test-demux-fuzz-negctl` claims ASan catches an injected NAL-length over-read.
  On darwin/arm64 ASan aborts at startup (no leak detector), and **that abort
  satisfies the control** without ever reaching the injected bug.
- `test-url` reports `32/32 (100.0%)` while printing that both WPT corpora are
  absent.
- **61 controls are "stranded"**: `NOT_CI` drops every `test-*-negctl` from the
  suite listing on the ground that a control is "run by its positive
  counterpart". Nothing checked that. 55 are recorded as debt in
  `tests/audit-stranded.baseline` and **6 are new**. The fix for one is a single
  line — `test-X: test-X-negctl` — and naming it on a `ci-host:` line instead
  satisfies the audit and still runs it never, which is worse because it looks
  fixed. `tests/license.mk` and `tests/logreporter.mk` are the worked examples.

---

## Source layout

All **C** source lives under `c/`, headers **colocated** with their `.c`. PNG,
BMP, ICO, WebP and inflate are **Rust** (`rust/src/*.rs`, linked as
`$(RUST_LIB)`); QuickJS, musl libm, NetSurf LibCSS and TinyCC are under
`third_party/`.

`INCDIRS` is one flat list built from `find c include -type d`, so every
`#include "foo.h"` resolves without a path qualifier:

```make
INCDIRS := $(addprefix -I,$(filter-out %/include/sys %/include/uonly,$(sort $(shell find c include -type d))))
```

**That flat list assumes header basenames are unique, and they are not.** Twice a
mini-libc POSIX header took a name the kernel already used; the list is sorted,
`c/apps/libc/include` sorts before `c/kernel/...`, so **kernel** files including
`"foo.h"` silently got the **userland** one and failed on undeclared kernel
functions in files nobody had edited. A clean clone was immune while the header
stayed untracked, which is how both survived a while. `sys/wait.h` was fixed by
excluding one directory; `sched.h` by moving it to
`c/apps/libc/include/uonly/` — note the ordering trap, `-Ic/apps/libc/include/uonly`
must come **before** `$(INCDIRS)` in `UCFLAGS` or the kernel's header wins anyway.
Before adding a header to `c/apps/libc/include`, check its basename against
`c/kernel`, `c/drivers`, `c/net`, `c/fs` and `c/lib`.

```
c/boot/            multiboot + long-mode entry (asm)  +  efi/ = a from-scratch UEFI loader
c/kernel/{core,cpu,mm,sched,exec,gui,pci,audio,module}/   kernel by subsystem (nine, not eight)
c/drivers/{char,timer,block,net,usb,virtio,audio,core}/   device drivers
c/fs/              vfs + logitfs + procfs + ramfs + lfsro + fsck
c/net/{link,ip,transport,core,dns,http,tls,ssh}/          network stack (eight, not seven)
c/crypto/{hash,aead,kdf,pubkey,trust,pq}/ + cpu_report.c  from-scratch crypto (pq = ML-KEM)
c/lib/{image,text,gfx,audio,video,media,ime,nn}/ + string.c
c/apps/            shared: logit.h clib.h logit_stat.h hidden.h crt0.asm crt0_cli.asm
c/apps/gui/          clock textedit monitor terminal files preview studio gallery
                     settings widgets greeter ch  (TWELVE) + aui.{h,c} = the widget toolkit
c/apps/coreutils/    sh + coreutils + login sshd syslogd httpd ping ps clip …
c/apps/as/           AetherScript: /bin/as
c/apps/browser/      browser + render engine + QuickJS bindings (the largest app)
c/apps/libc/         mini-libc — 44 files, 12.9k lines of src + 3.8k of headers
c/apps/lm/           /bin/lm — transformer inference on the device
rust/src/            png bmp ico webp vp8* inflate imgbuf
include/abi/         FIVE files: logit_abi.h logit_exec.h logit_pack.h sockerr.h logit_calls.abi
```

**`include/abi/logit_pack.h` and `fsroot/as/lib/abi.as` are GENERATED** by
`tools/gen_abi.py` from `include/abi/logit_calls.abi` and open with "DO NOT EDIT".

**`c/lib/` is SHARED, not ring-3-only.** 21 of its translation units compile into
the kernel: `text` (9), `gfx` (6), `image` (4), `ime` (1) and `string.c`. `C_SRC`
filters out only `c/lib/{video,audio,media,nn}` and three files of `image`. The
kernel is `c/lib/gfx`'s busiest caller and `c/lib/ime` runs in ring 0.

**There is a LICENSE BOUNDARY running through `c/lib/`** and nothing in a layout
listing suggests it. `LICENSING.md`: GPL-3.0-or-later covers `c/boot c/kernel
c/drivers c/fs c/net c/crypto c/lib` **except** `c/lib/image/**`; MIT covers
`c/lib/image/**`, `include/`, `rust/`, `tools/`, `tests/`, `docs/`, `fsroot/`. So
`c/lib/gfx/foo.c` is GPL and `c/lib/image/foo.c` is MIT. `make test-license-audit`
gates it and is green. `CONTRIBUTING.md` requires SPDX lines on new first-party
files; **adoption is zero** (`grep -rl SPDX-License-Identifier c/ rust/` = 0) and
nothing checks it.

**`tools/` is NOT build tools only** — 67 entries. Besides the generators it holds
the CI driver, three make-parsing gates, bisection and clean-clone verification,
scoreboard/WPT ranking, the language-model oracles, and `perf/`, `mmtrace/`,
`roots/`, `pkgroots/`.

**Two things in `c/apps/` are misplaced** and are named rather than moved because
moving them touches the root Makefile *and* six `tests/*.mk` fragments. Do not add
to either. `c/apps/{audio,media,net,video}/` are **not applications** — they hold
`audiocheck`, `demuxcheck`, `msecheck`, `h2check`, `vidcheck`, `vidcheck265` and
`vidbench`, on-device harnesses that happen to be built as `.aex`. Three of the
seven are built by the **root Makefile**, not by a fragment. The newest real
application, `c/apps/lm/`, correctly went into its own directory.

Tests live under `tests/`: `tests/unit/` = host unit/fuzz gates + stubs +
generators; `tests/boot/` = QEMU harnesses; `tests/qmp/` = QMP mouse/keyboard/
screenshot drivers. **105 `tests/*.mk` fragments**, each owned by a line of work;
`make test-mk-wired` proves every one of them is reachable from the root Makefile
(the failure it exists for is invisible from both ends — the fragment builds, its
gates run by hand, and `make test-X` answers "No rule to make target", which reads
as a typo in the docs).

---

## What this machine is, subsystem by subsystem

The previous version of this file described about a third of the tree. The rest
is below, one line each plus **the one thing its own source argues that you would
otherwise get wrong**. An absent claim is worse than a stale one: a reader of a
thin section concludes the subsystem does not exist.

### Boot and CPU

- **UEFI loader** — `c/boot/efi/loader.c` (1,238 lines), `trampoline.S`, its own
  `build.sh`, `tools/mkesp.py`. **Its thesis is that it impersonates GRUB**: it
  forges a Multiboot2 info block so *the kernel does not change*. One kernel, two
  loaders. `make test-uefi` boots it under OVMF; `test-uefi-negctl` is a loader
  built with `-DEFI_BAD_MAGIC` whose one wrong bit must make `LOGIT_BOOT_OK`
  never appear. The ESP is a **superfloppy, not GPT** — decided by experiment,
  because it was not obvious which this repo's OVMF honours.
- **ACPI / LAPIC / IOAPIC / MSI / SMP / per-CPU** — `c/kernel/cpu/`. `pci_msi.c`'s
  `dev_irq_request()` tries MSI-X, then MSI, then INTx. **The IRQ stubs live in
  `c/drivers/core/irq.c`, not `boot/isr.asm`**, because `isr_common` is a
  file-local nasm label — and they must mirror it *including the FXSAVE/FXRSTOR
  pair*, because the kernel is `-msse2`. The IDT gates are installed via `sidt`,
  not through `idt.c` (whose setter is static).
- **W^X / NX / SMEP / SMAP** — `c/kernel/cpu/prot.c`. `elf.c` maps each `PT_LOAD`
  with the permissions the ELF asks for. **Before this, every executable page in
  every process was also writable and a program could rewrite its own code.**
- **`cpu_report.c`** records why **AVX is deliberately OFF**, and it is a trap
  that would otherwise be rediscovered as data corruption: `isr.asm` wraps every
  C handler in FXSAVE/FXRSTOR, which saves x87 and XMM0-15 and **nothing else** —
  enable AVX without migrating to XSAVE and every interrupt silently truncates
  the top 128 bits of every YMM register.
- **`cpufeat.c`** — the leaf-availability rule is a silent-wrong-answer trap:
  querying past the highest supported CPUID leaf returns *that leaf's* result,
  not zero, so a naive "read leaf 7" on an old CPU invents features.

### Memory

`c/kernel/mm/`: pmm, vmm, vma, fault, kheap, rmap, reclaim, swap, pcache, shm,
oom, mmsys, tlb.

- **The reclaim invariant is THREE terms, not two.** This file said twice that a
  frame is evictable only if `rmap_count(f) == pmm_refcount(f)`, "the same number
  from two independently maintained structures". The live form is
  `rmap_count(f) + pcache_holds(f) == pmm_refcount(f)` — **three** structures
  (`reclaim.h:154`, `pcache.c:642`).
- **`pmm_alloc_reserve()`'s reserve is 128 frames, not 32** (`pmm.c:98`).
- **Swap gives the BKL back mid-transfer** (`swap.c:61-181`, landed `331651bc3`).
  This file used to close that bullet with "closing it needs submit/poll on
  `struct blk_ops` — an ask for the block line". The ask was granted
  (`blkdev.h:125-126`) and the edit made.
- **The page cache HAS a real workload.** This file said "that machinery has
  never had a real workload; the only caller of `SYS_MMAP_FILE` is a script
  written to exercise it". False in both halves: **every execve** maps program
  text through pcache (`exec.c:455,549,715`), `mman.c:158` routes every
  non-anonymous `mmap()` through it, and `/bin/lm` maps a model file for real
  work. Two places in the source still quote the retired sentence
  (`c/lib/nn/model.h:26`, `c/apps/coreutils/syslogd.c:6`).
- **Readahead** (`pcache.c`, 2026-08-25): the trigger is **two consecutive pages
  and nothing else**, and the trail is kept by HITS as well as misses — if only
  misses advanced it, the window would collapse on every batch. 1,000 pages cost
  **34 device reads**, not 1,000.
- **Shared memory** (`shm.c`) is a third structural consumer of the reclaim
  invariant, and the failure it prevents is silent: two processes sharing a page
  that happens to be all zeroes — what a segment looks like the instant before
  the first write — would come back holding **two different frames**. No crash,
  no log line: A writes 42, B reads 0, forever.
- **The OOM killer** (`oom.c`) does *not* kill the biggest. It prefers the largest
  **headless** process if that alone frees enough, and "enough" is `reclaim_high()`
  rather than a constant "so the two mechanisms cannot drift apart". The metric is
  resident set from the rmap sweep, explicitly not reserved bytes — "an mmap of
  4 GiB that was never touched costs nothing and freeing it frees nothing". Its
  negative control, `OOM_KILL_NEWEST`, is a policy somebody could genuinely
  propose.
- **`mm_protect_test.c` is a 474-line suite nothing runs.** Its own header says
  "every interesting bug in it is SILENT", and it names two: `mm_fault_classify()`
  answers `MM_FAULT_COW` on the PTE's COW bit alone and never consults the VMA,
  so a COW page that keeps its marker across an `mprotect` **serves the very
  write the caller just forbade**; and a resident page made `PROT_NONE` is not
  present, frame retained, still referenced — invisible to every loop that walks
  present PTEs.

**OPEN BUG, verified 2026-08-28: `munmap` does no cross-core TLB shootdown.**
`vmm_unmap_range_in()` clears the PTE, calls `pmm_free()`, does
`if (active) invlpg(a)` — the current core only — and returns. Forty lines below,
`vmm_protect_range_in()` ends with `if (n && tlb_flush_all &&
vmm_space_busy_elsewhere(cr3)) tlb_flush_all();`. Threads share one CR3, so a
multithreaded process calling `munmap()` leaves a sibling on another core holding
a cached **writable** translation to a frame the PMM has already handed out —
ring 3 writing ring 0's memory, silently. **The tree already knows**, at
`uthread.c:197-201`: *"WHAT IS STILL OPEN, and it belongs to munmap rather than to
this file … Closing it means flushing inside vmm_unmap_range_in BEFORE the frames
are released. Reported, not patched from the outside."* Note the asymmetry: the
thread-exit path bumps a generation counter and is bounded by one timer tick; a
plain ring-3 `munmap()` (`mmsys.c:229`) does neither and is **unbounded**.

### Processes, exec, and the things a program can ask for

`c/kernel/exec/`: proc, file, exec, elf, aex, ksignal, ksigframe, coredump,
ptrace, kpoll, syscall. **165 syscalls** in `include/abi/logit_abi.h`.

- **The loader REFUSES dynamic linking, by name and with an argument.**
  `elf.c:485`: `PT_DYNAMIC` — "this loader applies no relocations"; `PT_INTERP` —
  "this system has no dynamic loader"; `ET_DYN` — "a PIE is *defined* by needing
  `R_X86_64_RELATIVE` applied at its load base. Refusing is what makes the fixed
  link bases honest rather than accidental." Everything is static at a fixed base.
  **ASLR is therefore not absent, it is unrepresentable.**
- **The loader streams.** `exec.c` used to `kmalloc(whole file)`, which fell
  through kheap's `grow()` and asked `pmm_alloc_contig()` for the next power of
  two **in one piece**: 128 MiB of file took a 256 MiB arena, and a 256 MiB file
  was refused with 456 MiB free. Now the ELF header, the program headers and each
  segment come through a 512 KiB static bounce, and read-only whole pages are
  mapped from the page cache. Peak kernel heap arena went **249,856 KiB → 16,384
  KiB**. `make test-bigexec` loads 16/32/64 MiB twice per boot — once fresh, once
  after 112 MiB of page cache has been churned, because `pmm_alloc_contig` is a
  linear first-fit with no fallback and a load that works on a just-booted machine
  says nothing.
- **`wm_run` spawns `/bin/login`, not `/bin/sh`.** On an image with no accounts
  (every freshly built one) login prints a line and execs `/bin/sh` as root — the
  old behaviour, one exec later. The consequence: the shell is never root on a
  machine that *has* an account.
- **A read-only fd holds NO bytes.** `file.h:8`. `backing` is NULL and every read
  is a `vfs_pread()` at `off`. The buffer survives only for **writable**
  descriptions, because the VFS write op is whole-file (`write(path, buf, size)`,
  **no `->pwrite`**) — a property of the op table, not a policy. There are **six**
  fd kinds: F_VFS, F_PIPE, F_TTY, F_SOCK, F_EVENT, plus a `live` flag for
  generated `/proc` files.
- **Core dumps** (`coredump.c`) are ELF64 `ET_CORE` files **real gdb can read**,
  written to **four fixed slot names** (`/core.1` …), not one per pid — so a
  crash loop overwrites, and the kernel's `[core]` serial line names the slot.
  `/bin/readcore` reads them on the machine, because "a dump nothing on the
  machine can read is a file, not a dump".
- **ptrace** (`ptrace.c`) — its scope is its safety argument: "nothing here
  schedules, blocks a thread, or touches the signal state machine beyond posting
  SIGSTOP/SIGCONT through the ordinary `ksig_post()` — which is the reason the
  feature is small enough to be trusted." Explicit lock order, one direction.
- **poll/eventfd/timerfd** (`kpoll.c` + `kpollsys.c`) — the core/file split exists
  **for the gate**: `file.c` cannot be compiled for the host, and a poll core
  reachable only through it could be tested only by booting QEMU, i.e. the one
  property that matters (an event arriving between the readiness check and the
  sleep is not lost) would be tested by hoping the race happens.
- **Loadable kernel modules** (`c/kernel/module/`) — an ET_REL x86-64 loader with
  relocation, a credential check and an explicit export table. `modelf.c` is
  deliberately pure — "it calls nothing: no kmalloc, no kprintf, no VFS, no
  locks" — so the relocation arithmetic is host-testable. Its bounds checks are
  written `if (off > imglen || size > imglen - off)` rather than `off + size >
  imglen`, "which is the same expression until `off + size` overflows and then is
  the opposite of it". **No userland consumer exists** — there is no `insmod`.
- **User threads and the futex** (`uthread.c`) — `NUT 128` bounds the *machine*;
  `LOGIT_THREADS_MAX (64)` bounds one process. And the real ceiling on this
  machine is neither: `VMA_MAXAREA` is **16**, because a normal `pthread_create()`
  stack is its own mmap'd VMA — which is why `/bin/sshd` allocates thread stacks
  out of its own `.bss`.

### Storage

**Status, verified twice: this machine keeps a file across a reboot.** The old
"corrupts after repeated non-snapshot boots; use `-snapshot`" note was true of v3
and is not true now.

**Block layer** (`c/drivers/block/`): `blkdev.c` is a multi-device table —
virtio-blk (preferred), AHCI/SATA, NVMe, ATA PIO as fallback — plus `part.c` for
MBR/GPT. `blk_flush()` is a real write barrier on every backend that can reorder,
and `blk_flush_count()` exposes barriers-since-boot so a test can *count* them.
There is now a real **async engine**: `blk_submit`/`blk_poll`/`blk_wait` on
`struct blk_ops`, a per-medium in-flight interlock, and a DMA bounce path.

- **AHCI refuses NCQ, with arithmetic**: "not here because the arithmetic says it
  would move nothing on this machine and could only make it slower" — both
  sources of queue depth eliminated by measurement, one by `blkdev.h`'s
  one-in-flight interlock and one by a full-boot census.
- **NVMe** has a PRP list now (`g_prp_list`, one page per queue) — the file's own
  top comment still says it does not, which will mislead the next reader.

**LogitFS on-disk v4** — `c/fs/logitfs_fmt.h` is the single definition site and
`tools/mkfs.py` mirrors it. **4 KiB blocks, 512 MiB image (131,072 blocks), 8,192
inodes.** It went 16,384/256 → 131,072/8,192 on 2026-08-20 because *one file of a
C toolchain* — `cc1plus`, 39,797,952 B — did not fit in the whole filesystem.
`LFS_VERSION` stays **4**: every changed quantity was already a superblock field,
so a pre-2026-08-20 image mounts unchanged. Inodes are 128 B with `direct[12]` +
single-indirect + **double-indirect**. **Block 0 is never rewritten at runtime**,
which is why the superblock needs no checksum.

**The journal is metadata-only + ordered data (ext4 `data=ordered`)** — say that
out loud, because "it has a journal" is not the same claim. Bitmaps, the inode
table, indirect blocks and **directory data blocks** are staged into the log and
installed only after the commit record is on media; ordinary file data blocks go
straight to their final location, always before the metadata pointing at them
commits. That is sound only because of `bfree()`: frees are **deferred to
commit**, so the allocator cannot hand a block back to the very operation that
released it. (That was a real bug, found by the crash sweep, fixed in `d9dccbf`.)

**The commit record self-verifies**: `hcrc` over the header block rejects a torn
header; `bcrc` over exactly the *n* body blocks rejects a **stale** header
standing over a newer transaction's bodies — which was corruption *caused by
recovery* on a filesystem that never crashed. Three barriers, each with a distinct
failure mode if removed, are documented above `log_commit()`. **Read that comment
before touching `logitfs.c`.**

- **`test-barrier` requires *at least* 3 barriers per file write, not exactly 3**
  (`barriers.as:58`). A change that issued five would pass unnoticed.
- **`alloc_hint`** (`logitfs.c:364-424`) exists because the 512 MiB geometry would
  otherwise have shipped a **64× write slowdown**: `balloc()` is O(n²) bit tests
  to fill an image — 64 MiB took 133 M iterations (0.068 s), 512 MiB would take
  8.5 G (4.46 s), and those are *native* numbers on a machine that runs under TCG.
  The invariant is an equivalence, not an optimisation: every block in
  `[data_start, alloc_hint)` is USED, so `bit_clear()` must LOWER the hint. **The
  gate its own source names — `test-fs-allochint` — does not exist.**
- **fsck** both detects and repairs, and its rule is "fix only what has ONE
  correct answer": a block claimed by two inodes is **refused, whole**, because
  both fixes destroy a file and nothing on the disk says which. A **read-only**
  fsck runs at **every mount**, so every boot harness in the tree asserts the
  bitmap agrees with the inodes. A mount-time finding never fails the mount.
- **Modes and owners survive a reboot.** `logitfs_getattr`/`setattr` are at
  `logitfs.c:1375`/`:1434`, installed at `:1531`, both under `#ifndef
  LOGITFS_NO_ATTR` — **both or neither**, because vfs.c treats half an
  implementation as none. `xmode` has a presence bit (`LFS_MODE_SET`) because
  **mode 0 is a legal mode**, and that distinction reaches ring 3 as
  `LSTA_MODE_STORED`: a stat that cannot tell a chosen 0644 from a defaulted one
  is a stat that lies quietly.
- **`/proc`** (`procfs.c`) is a real mounted `struct filesystem`. Its design is
  lifetime, not format: **procfs holds no pointer to a process, ever** — that is
  what makes use-after-free structurally impossible rather than carefully avoided.
  A read of a dead pid returns `ENOENT`, chosen over a stale snapshot ("the answer
  that looks like it works") and over 0 ("indistinguishable from an empty file"),
  and that is sound only because `next_pid` is monotone. `/bin/ps` makes **no
  syscall a `cat` does not also make** — `SYS_PROCS` still exists and is not
  called.
- Around it: `vfs.c` + `vfs_path.c` (resolution as its own host-testable TU),
  `vfs_meta.c`, `vfs_cred.c`, `vfsctl.c` (control as synthetic files), `ramfs.c`,
  `lfsro.c` (instance-aware read-only reader — `logitfs.c` is a singleton and
  cannot be two), `fsbench.c`.
- **`/dev` holds exactly eight synthetic files and nothing can be created there.**
  Four from `vfsctl.c` (`vfsctl`, `vfsmounts`, `vfsmeta`, `fsbench`) and four from
  `kdiag.c` (`kmsg`, `kstat`, `ktrigger`, `kprof`). `2>/dev/null` is `ENOENT`.

**Tests** — host: `test-fs-cache` 29 · `test-fs-journal` 48 · **`test-fs-crash`
1744** · `test-fsck` 167 · `test-fs-format` 25 · `test-bulkread` 34 + control.
`test-fs-crash` cuts power at **every device write** of write/mkdir/delete/
rename/overwrite × 3 loss patterns, and after every cut demands mountable,
fsck-clean, bystanders byte-for-byte, victim whole-or-absent, no block twice.
Boot (minutes each, **no `-snapshot`** — that is the point for five of the six):
`test-fsmount`, `test-durability` (5 boots, 3 files byte-for-byte),
`test-fscrash` (4 SIGKILLs), `test-fsreplay`, `test-hugefile`. **`test-barrier` is
the exception and legitimately passes `-snapshot`** — it counts barriers and never
asserts survival. **Byte-for-byte, never a length check**: a filesystem that hands
one block to two files produces a file of exactly the right length holding
someone else's data.

### Networking

`c/net` is **eleven directories, 38 `.c` files**: core (dhcp, lsock, net, raw,
route, sock, unix), dns, http (cookies, hpack, hpool, http, http1, http2, url),
ip (icmp, ip, ip6, ip6_addr, nd, reasm), link (arp, eth), **ssh** (7 files), tls
(ocsp, tls, tls12, tls_psk, tls_server, x509), transport (tcp, udp).

**THE WM LOOP NO LONGER PUMPS THE NETWORK.** This file said `net_poll()` is
pumped from the compositor, and a whole diagnosis of throughput as "gated on the
frame rate" was built on it. Receive runs on **SOFTIRQ_NET raised by the NIC
ISR**; TCP's timers run on a **10 ms ktimer** raising the same softirq
(`net.c:125-160`). `net.c:249` records that the surviving `wm.c` call "is now
harmless and its owner may delete it whenever they like". Note also `net.c:249-252`
on quoting line numbers at all: *"Its line NUMBER is deliberately not quoted here
— it was 5483 when this was written and 5550 four hours later. The text is the
anchor."*

- **TCP is not the M10 stack.** It has **congestion control (RFC 5681), window
  scaling (RFC 7323), SACK (NSACK 8), RFC 6298 RTO estimation, fast retransmit,
  TCP_NODELAY, timestamps, and a passive-open server path.** `NCONN` is **32**,
  not 8; the receive ring is **128 KiB**, not 64 (doubled when window scaling
  arrived — "a 64 KiB ring made the option decorative"); out-of-order reassembly
  is a sorted interval set, NOOO=16.
- **An interface TABLE, not `g_nic`** — but `g_nic` is still there and still means
  "the primary NIC", now *derived* from the table rather than being it. Loopback
  registers FIRST so `RT_OIF_LO == 1` holds by construction (`_Static_assert`).
- **A routing table, not a ternary.** The old form was `((dst & mask) == (ip &
  mask)) ? dst : gw` — one gateway, and no way to say "no route": an unroutable
  datagram went to the gateway, which is a leak, not a fallback. `route.c` is
  longest-prefix-then-metric with the default route as plen 0. `nm -u route.o` is
  **empty** — it knows only integers.
- **IPv6 is real and is not ARP-shaped.** `ip6.c` (477), `nd.c` (882),
  `ip6_addr.c`. **There is no `net_cfg.ip`**: an interface holds several addresses
  at once, each with an RFC 4862 state and two lifetimes, and which one sources a
  packet is decided **per destination** by RFC 6724. Neighbour Discovery is
  ICMPv6 — it runs over IP, over multicast, with a five-state per-neighbour
  machine, and carries the host's whole address configuration as a side effect.
  `test-ip6-fallback` asks the question that matters: does a v4-only answer behave
  **exactly** as it did before IPv6 existed?
- **Fragment reassembly** (`reasm.c`) tracks coverage with a **per-byte bitmap**,
  not a running count, and the comment says why that is necessary: summing
  fragment lengths is the teardrop bug — two overlapping fragments make the sum
  reach `total` with a hole still in the buffer.
- **HTTP/2** (`http2.c` + `hpack.c`, 2,600 lines) — "a header block is decoded
  even when nobody wants it", because skipping it would leave our dynamic table a
  different size from the peer's and from that moment every indexed header
  decodes to the wrong field, silently. HPACK's Huffman decoder is **canonical,
  not a trie**, verified by Kraft equality. `hpool.c` keys connections on
  **(host, port, tls) and all three must match** — reusing example.com's
  connection for evil.com because they resolved to the same address sends the
  Cookie to the wrong peer; origin identity is never inferred from the socket.
- **`http1.c`** (1,401 lines) is the ring-3 replacement for the kernel's
  `http.c`: bounded, non-blocking, host-testable, with a transport vtable, header
  lists, chunked decoding and gzip. The kernel version "builds its request by
  concatenating string literals, so it can carry no header list at all: no
  Cookie, no POST body, no conditional GET."
- **SSH-2 server** — `c/net/ssh/` (7 files) + `/bin/sshd`. curve25519-sha256,
  ssh-ed25519, aes128-ctr, hmac-sha2-256, argued against a **captured
  OpenSSH_10.2p1 KEXINIT**, and a real OpenSSH client logs in. Two
  pseudo-algorithms are **refused by name**, and the reason is that naming one is
  an opt-in: `kex-strict-s-v00@openssh.com` (Terrapin) — "half-implementing it is
  worse than not offering it, because the client believes the stricter contract is
  in force". **`sshd` has never run on the product image** — nothing spawns it.
- **AF_UNIX** — stream + datagram + **seqpacket** (which fell out: datagram
  already needs a record-length ring, so seqpacket is the stream path with that
  ring on). Bindings live in `unix.c`, **not as VFS nodes**, and the cost is
  stated rather than hidden: no `ls`, no `stat`, unlink does not release. The
  abstract namespace is **refused, not stubbed**.
- **Raw sockets** with **IP_HDRINCL refused outright** (a caller-built IP header
  is the mechanism for forging a source address). `/bin/ping` is an ordinary
  program over `SOCK_RAW`, not a bespoke syscall.
- **Cookies** (`cookies.c`) — one jar, two doors, and the gate knew only one. The
  request kind is **three-valued** (`SAME_SITE` / `CROSS_SITE` /
  `CROSS_SITE_NAV`) and `SAME_SITE` is **0**, so an uninitialised int is the
  dangerous value. `CK_HEADER_MAX` is one number because it used to be three,
  disagreeing by 8×. HttpOnly used to make a cookie the **preferred eviction
  victim of the script it hides from** — three individually correct lines. That
  gap is now closed on both doors and `test-cookie-cors` drives the transport
  path directly.

### Crypto and TLS

`c/crypto/{hash,aead,kdf,pubkey,trust,pq}` — SHA-2 (224/256/384/512, 512/224,
512/256), SHA-3/SHAKE (`pq/keccak.c`), HMAC/HKDF/PBKDF2 at every width,
ChaCha20-Poly1305, AES-128/192/256 in GCM/CTR/CBC, X25519, P-256/384/**521**,
**Ed25519 (sign, verify, keygen)**, RSA PKCS#1 v1.5 + PSS, **ML-KEM**.

- **`genroots.py` no longer skips P-521 or Ed25519** — both verify. What it
  refuses is Ed448 and unrecognised curves, recorded by name in
  `logit_roots_skipped[]`, **currently empty**: all 130 PEMs compile in.
- **The trust store is generated and CAN silently become empty.** `genroots.py`
  takes the roots directory as `argv[1]`, and a stale path globs zero PEMs. An
  empty C array is legal, both root loops are bounded by `logit_nroots`, so the
  machine fails **closed** and every https fetch dies naming no cause. The
  generator refuses an empty result now; a caller that *means* zero anchors passes
  `--allow-empty`.
- **`check_provenance.py`** defines what "authentic PEM" means: the SHA-256 of the
  certificate's **DER bytes** must appear in Mozilla's certdata snapshot or a
  closed legacy list. Not "is this well-formed" — "did anyone account for how it
  got here".
- **The mode never lives in a backend.** `struct aes_backend` grew a fourth
  primitive (block decrypt) rather than letting `aes_modes.c` hide an inverse
  cipher. Two backends that made the same equivalent-inverse-schedule mistake
  would otherwise agree while decrypting garbage.
- **CTR's counter is not GCM's**: SP 800-38A increments the whole 128-bit block;
  GCM's inc32 wraps only the low four bytes. They agree until a counter block ends
  in `ffffffff`, at which point GCM replays a keystream block.
- **CBC's padding check is a constant-time accumulate over all 16 candidates** —
  the byte-by-byte early exit leaks the pad length and therefore the plaintext
  length.
- **AES-NI is about constant time, not speed.** The portable backend indexes a
  256-byte S-box with secret state and branches on secret bits inside the GF
  multiply. "Any speedup is a side effect, and cannot be measured under TCG."
  **On this host only ONE backend is exercised** (`aes-gcm backends exercised: 1`)
  — the differential is an x86-only property of the gate.

**TLS: what it speaks, and what it does not.** `c/net/tls` is 5,229 lines and
`c/crypto` 5,221 — against OpenSSL's ~500,000, which is the honest frame.

| | |
|---|---|
| versions | TLS **1.3** and **1.2**, both CLIENT; `tls_server.c` is 1.3-only |
| 1.3 suites | AES-128-GCM-SHA256, CHACHA20-POLY1305-SHA256, AES-256-GCM-SHA384 |
| 1.2 suites | ECDHE×{ECDSA,RSA}×{AES-128-GCM, AES-256-GCM, ChaCha20} |
| groups | x25519, secp256r1, secp384r1, **X25519MLKEM768** (1.3 client only) |
| certs | RSA v1.5 + PSS, ECDSA P-256/384/521, Ed25519 anchors, 130 roots |
| resumption | PSK / session tickets — exists |
| ocsp | stapling, checked, REVOKED refused; no online fetch |

The **hybrid is client-only and asymmetric on purpose**: the ClientHello carries
the hybrid share *and* a bare x25519 share, because leading with the hybrid alone
made every classical server answer HelloRetryRequest (measured: 29 interop
failures, all `X25519MLKEM768 -> x25519`). 36 bytes buys that back.

**THE HOLE THAT WAS NOT A GAP.** `verify_flight()` is a dispatch over whatever
messages the server chose to send, not a state machine, so every check is
conditional on the message that carries it arriving. The CertificateVerify
signature check lived inside `if (mt == HS_CERT_VERIFY)`; **omit the message and
the branch never runs**, and nothing afterwards noticed — `tls_check_chain` proves
the certificate is authentic and says nothing about whether the peer holds its
private key. Certificates are public. An on-path attacker could replay any site's
real chain, skip the signature, and this client printed "chain of 2 verified".
Closed with a presence flag plus a post-loop refusal.

**What is still not there**, named because an absent claim is worse than a stale
one: **no client certificates** (the 1.2 client *does* answer a CertificateRequest
at `tls12.c:411` with an empty Certificate message, as RFC 5246 requires — but
neither side ever offers one); **no 0-RTT**; **no Certificate Transparency** —
nothing parses an SCT, so a certificate no log ever saw verifies exactly like one
that was logged; no DTLS; **no protocol state machine** — FREAK, Logjam, SMACK and
CCS-injection are all state-machine bugs in which the primitives are perfect, and
**there is still no test that sends handshake messages out of order or repeats
one**; and constant-time discipline is **per-site, not global**.

### Text, and the claim that was wrong for months

**This file said "no bidi/shaping" in the M14 bullet. There are 4,529 lines of it
and it is in ring 0 on the main draw path.** `c/lib/text/` holds `bidi.c` +
`bidi_data.inc`, `shape.c`, `otlayout.c` (GSUB/GPOS), `script.c` +
`script_data.inc`, `cff.c` (CFF/CFF2 Type 2 charstrings), `fontcolor.c`,
`glyphras.c`, `ttf.c`, `utf8.c`. `c/kernel/gui/text.c:82` — *"Everything below
goes through `shape_line()` — ONE function"* — and `nm build/kernel.elf` carries
`T bidi_reorder`, `T shape_run`, `T script_arabic_joining`, `T cff_glyph_path`.
Two differential gates, **neither containing a case we invented**: `test-bidi`
against the UCD's own BidiTest.txt, `test-shape` against HarfBuzz.

**Both need external inputs this host does not have** (`/usr/share/unicode/`, a
HarfBuzz venv) and `test-bidi` fails hard rather than skipping — see rule 5.

- **TTF parses two outline formats**: `glyf` (quadratic, simple + composite) and
  `CFF ` (Type 2, in `cff.c`), with `CFF2` accepted. cmap format 12 preferred,
  format 4 second. **No hinting**, argued in `cff.h:18-20`: hint masks are parsed
  "exactly far enough to know how many mask bytes follow — and then discarded,
  because we do not hint".
- **Glyphs rasterize at 16 sub-scanlines, not 4.** `glyphras.c` converts an
  outline to a `gfx_path` and calls `gfx_fill_mask_subs(..., 16)`; 4 is what a
  button passes.
- **`fontcolor.c` (COLR/CPAL, CBDT/CBLC, sbix) is gated, green and DEAD** —
  nothing outside its own test calls it, and there is no colour or emoji face on
  the disk for it to act on.
- **The fonts, and why there are three.** `fsroot/fonts/` ships `ui.ttf`
  (Noto Sans SC subset: GB2312 + ASCII + CJK punctuation) and `mono.ttf` (Noto
  Sans Mono: **printable ASCII plus NBSP — no CJK at all**, ~~so Han typed into
  the Terminal has no glyph~~ — **AND THAT CLAUSE IS FALSE, MEASURED 2026-08-29
  OFF THE SCANOUT.** The bolded half is true and the guest prints it
  (`/fonts/mono.ttf: 97 glyphs`); the conclusion does not follow, because a
  mono request does not use one font. `text.c`'s `tl_fonts()` builds
  `{requested, F_UI, F_TEXT, F_MONO}` and `shape.c`'s `font_for()` returns the
  first face with a glyph — F_UI, the 7,655-glyph Noto Sans SC. Ink columns on
  a mixed `ab<IME>cd` line: a=183, b=192 (+9), 你=201 (+9), 好=219 (+18),
  c=237 (+18), d=246 (+9) — ASCII advances 9 px, Han 18 = **exactly two cells**
  (`shape.c:1215`, `w = (adv > cell*3/2) ? cell*2 : cell`), and a/b/c/d all
  advance identically, which is what rules out a silent fall to a proportional
  face. The sentence is also datably stale: `26d5cb2fd` (2026-08-17) fixed a
  signed-char bug that dropped every byte ≥ 0x80, eleven days before this file
  was rewritten. **What IS broken is next door**: `terminal.c:412` wraps on
  BYTES while `cols` counts display CELLS, so Chinese wraps after ~cols/3
  characters leaving a third of every row blank, and the wrap can land inside a
  UTF-8 sequence. `third_party/fonts/` additionally ships **DejaVuSans
  whole** as `/fonts/text.ttf`, and the reason is a measurement: **subsetting
  broke shaping** — "the two Noto subsets carry no Arabic and no Hebrew, and
  subsetting stripped their GSUB/GPOS/GDEF tables, so with only those two the
  shaper has nothing to apply and Arabic still renders as disconnected isolated
  letters."
- **There is no bold and no italic face on the disk**, and `gui_text_run()` has no
  weight or slant parameter. Every `<h1>` and `<strong>` on the web renders at
  regular weight. `css_engine.c` computes `o->bold`; the painter reads it **zero
  times**. Closing this is a font asset + an ABI parameter + kernel face
  selection, in that order.

### The 2D engine (`c/lib/gfx`) — Open Logit

It exists because there were **three** coverage/paint paths and every new app
started from `gui_rect`: the kernel's M14 glyph rasterizer, a second rasterizer in
the widget toolkit, and a third hand-rolled path in the browser. **All three are
deleted.** An engine that coexists with what it replaced is a fourth path.

`raster.c` was the last and largest to go. `c/lib/text/glyphras.c` is what
replaced it: a **converter** from font outline to `gfx_path` in device 24.8, plus
one `gfx_fill_mask_subs(..., 16)`. It rasterizes nothing. The number, from
`make test-glyph-agree` against an oracle that is neither rasterizer (a 32×32
supersampled point sample of the true outline, in double, over 572 bitmaps and
363,650 pixels): the bridge scores **mean 0.310/255, worst pixel 16**; raster.c
scored **0.490/61**. The replacement is closer to the true geometry than the thing
it replaced — 1.6× in the mean, 3.8× in the worst pixel.

**The migration found a real defect in the engine, and only this way could it
have been found.** `span_add` accumulated straight into the 0..255 byte row,
converting each sub-scanline's covered length on the spot with an integer divide —
so truncation was paid **once per sub-scanline** and the error GREW with `subs`,
backwards for the one knob that exists to buy accuracy. At the default 4 it cost
up to 3/255 and nobody noticed; at 16 it cost up to 15/255 on every antialiased
pixel of every glyph, always the same direction, and the first measurement of the
port showed the whole typeface coming out lighter. `g_acc` sums lengths exactly
and converts once per pixel through a 16.16 reciprocal that is **exact for every
`subs` dividing 65280**.

**Phase 1** is paths, nonzero + evenodd, a scanline coverage rasterizer, four
paints (solid / linear / radial / image), Porter-Duff src-over, an affine
transform applied to **paths**, and a rectangle clip. **Phase 2 landed**
(`8e85be37b`) — `gfx_stroke.c` exists and `test-gfx` is raster + paint + **stroke**
+ **clip**. `svg.c`'s own scanline filler and Newton `dsqrt`/`dsin` are **gone**;
it is a consumer of the engine like everything else, and it is **not in `C_SRC`**.

Each half arrived with its own independent oracle: the stroker's is the distance
from a pixel to the **flattened** source polyline (to the polyline and never to
the ideal curve — measuring against the curve charges the path's flattening error
to the stroker and fails a correct one) plus the miter wedge and square-cap
half-square in closed form; path clipping's is the AND of two analytic predicates
that already existed, supersampled the same way. The clip tests use deliberately
**asymmetric** extents and origins — a centred clip hides a transposed axis.

**Three techniques carry the cost.** Masks are generated at DEVICE resolution and
blitted into a POINT rect of the same device size, so the compositor's
nearest-neighbour rescale is the identity and antialiasing survives 150%/200%.
Only what CURVES is rasterized — a rounded rect is a 9-slice, so **O(r²), not
O(w·h)**: 0.144 µs against 6.96 µs for the same 200×40 r=8 shape, and zero on the
second card. Masks are cached by exact device geometry.

**The bar is a number against an INDEPENDENT reference.** Worst pixel error:
circles r=3..48 **0.095**, ellipses 0.091, rounded rects 0.047, triangles 0.119,
corner tiles 0.078; src-over over 175 alpha/coverage combinations **1/255**. A real
bug found by building the reference first: `gfx_over` truncated `da*(255-a)/255`,
which at a=1, da=1 floors the destination's surviving alpha to **zero** — 17/255
off the definition. Everything faint over something faint was wrong that way.

**`fb.c` and `wm.c` are NOT untouched by the engine** — this file claimed they
were, one paragraph after saying `fb.c` calls `gfx_mask_corner`. `fb.c` makes five
engine calls; `wm.c` carries 34 `gfx_` references and builds a path of its own.
`SYS_GUI_BLIT` is how **ring 3** reaches the screen; the kernel compositor calls
the engine directly.

**Still dead, measured**: `gfx_fill_clipped` (the surface-writing half of path
clipping) has **zero** callers outside its own gate; so do `gfx_fill_subs` and
`gfx_m_invert`. `gfx_gradient_strip_paint` is dead **deliberately** and the
argument is at `browser_paint.c:654`.

### The browser and the web platform

`c/apps/browser/` is the largest application in the tree. The whole
HTML→DOM→CSS→layout→paint pipeline runs in ring 3; the kernel is a primitive
provider (network fetch, font metrics, drawing).

**The HTML parser is a real WHATWG parser, not a tag stripper.** This file
described it as "`net/dom.c`: ~60 named entities, implied `<tbody>`, optional
end-tag auto-closing". That scanner is **deleted**. What is there is
`html_tokenizer.c` (1,548 lines, every tokenizer state including the eleven
script-data escaped ones) + `html_tree.c` (2,939 lines: the stack of open
elements, the list of active formatting elements) + `dom.c` + `dom_serialize.c`,
with **2,231** named entities. `make test-html5lib` is **green**: 1,723/1,818
tree-construction (94.8%) against a **baseline ratchet**, not a pass rate.

**CSS is NetSurf LibCSS** (`third_party/css`, 321 `.c`) driven from
`css_engine.c` + `css_extra.c` + `css_vars.c` + `css_interp.c`. `net/css.c` and
`net/paint.c` **do not exist**.

- **Flexbox** (`layout_flex.c`, 950 lines) and **Grid** (`layout_grid.c`, 2,250)
  both exist and are green — `test-flex` 176 checks, grid 318 across four gates.
  **Neither is a separate translation unit**: `layout.c` textually `#include`s
  them, deliberately — *"Fourteen Makefile rules across five fragments list
  layout.c in a source list, and two of them are the harnesses that MEASURE this
  file. A source list is exactly the thing a measured line must not be able to
  edit."* Adding a TU would have let the engine edit its own scoreboard.
- **The flex trap, and it is the negative control**: growing distributes free
  space in proportion to `flex-grow`; **shrinking does not** distribute in
  proportion to `flex-shrink` — it uses `flex-shrink × the item's inner flex base
  size` (§ 9.7 4b). The raw factor produces plausible layouts with correct totals
  and wrong individual sizes.
- **`css_vars.c` records its own first version as the wrong one.** A flat
  last-wins text scan for `--name: value` ignores **media context**, so on a light
  machine it takes the value from `@media (prefers-color-scheme: dark)`. Anyone
  who assumes "var() is a text substitution" reimplements exactly that.
- **`js_url.c` is a SECOND URL parser on purpose**, not a binding over
  `c/net/http/url.c`: "the corpus is exhaustive precisely because every browser
  tried to write this as a splitter first, and the only way to answer 'why does
  this case do that' in a hurry is for the code and the prose to have the same
  joints."
- **`js_anim.c` is scope chosen by subtraction, and the arithmetic is the
  design**: 10,714 WPT subtests fail on one line, `'animate' in Element.prototype`.
  Of those, 8,513 have twins that fail for a CSS reason and belong to another
  line; 2,202 do not. That is what was built.
- **The rule `js_platform.c` states, and it is worth more than its inventory**:
  *"every entry below is either a name a page in `tests/fixtures/webapi/` actually
  reached for (with the page named in the comment) or is marked as
  requested-but-unmeasured. Nothing in this file is here because a browser is
  'supposed to' have it."*
- **Canvas 2D** (`js_canvas.c`) is the consumer `c/lib/gfx` never had, and it
  makes gfx calls while owning no scanline loop. Two engine contracts shaped it:
  `gfx_path_matrix` **refuses a mid-build call**, so canvas keeps the path matrix
  at identity and transforms every point itself (the opposite of the CSS painter,
  which knows its matrix before the first point); and a `gfx_surface` is
  **straight RGBA8, byte for byte what ImageData is**, so `getImageData` is a copy
  and the gate can assert BYTES. `toDataURL`/`toBlob` **throw** rather than
  fabricate — this tree decodes PNG and does not encode it.
- **The one-line "safe" answer is the wrong one**, and the corpus says so rather
  than an argument: `baidu-async-search.js` writes `var o = a.getContext === i ?
  !1 : a.getContext("2d"); if (o === !1) return !1;` — with the method absent the
  page takes its fallback cleanly; with a null-returning method the guard does not
  fire and the page walks on holding null.
- **When a page reports its own error, read it.** A modern bundle catches its own
  exceptions, so what reached the log was six byte-identical
  `[error] TypeError: not a function` lines from a minified bundle. Two
  instruments closed it: `console.error(err)` now prints the **stack** (in *both*
  consoles — they share no TU), and the TypeError **names the callee** (the
  property atom is in the bytecode at `OP_get_field2`). Cost is zero on the path
  that works: the check runs only after `JS_CallInternal` has already returned an
  exception. Two traps, both found by writing the control first: a plain call must
  not inherit the previous method call's atom, and `o.a?.()` on a nullish `o.a`
  **skips** the call so its atom is never consumed.
- What that found immediately: **`Node.isEqualNode`**. React's Float compares
  hoisted `<title>`/`<meta>`/`<link>` with it; absent, it threw six times, React
  declared hydration lost (#418), switched to client rendering (#423), and the
  client render died. stripe went 69 painted text runs → 38 → **0** and scored
  BLANK **with no failed request and no missing subresource**.

**WPT.** The headline is `149318/246542 subtests (60.6%) over 9,181 harness
files`, and the suite is built to distrust it: **the gate is a ratchet against an
expected-failure list, not the percentage.** The corpus is OPTIONAL and the
capability is not — `WPT_ROOT` points anywhere, an absent directory makes the
runner say so and exit 0. **`third_party/wpt` does not exist and has not since
2026-08-21** (`4179053ef`, "the corpus is fetched at a pinned revision, not
vendored — 59,422 files leave the tree"); `WPT_ROOT ?= build/wpt`, populated by
`make wpt-fetch` at the revision in `tools/wpt_revision.txt`.

**THE RUNNER MUST BE `browser.aex`, or every number is of a browser that does not
exist**, and the second half of that is the most instructive failure in this
repository: *linking a translation unit is not running it.* The runner linked
`css_extra.c` and `layout.c` and then never called `css_apply()`,
`css_extra_apply()` or `layout_page()`. `make test-wpt ONLY=css/css-grid` read
**531/11152 with and without the grid implementation** — 11,152 subtests
structurally unreachable, and the line shipping grid unable to tell its own work
from a no-op. The source list is expressed as a **subtraction** from the
Makefile's own variables so it cannot drift silently.

Reftests are judged by pixels against a reference render out of the same corpus
against a 17,452-entry baseline — the runner's own output said "there is no
reftest harness here" until somebody checked.

**Ranking, not rates.** `tools/wpt_rank.py`, `cssom_rank.py` and
`cssom_compare.py` turn the rate into a work order, and `cssom_compare.py` names
the trap the ratchet cannot see: *"A file that never completed contributes NO
subtests to the denominator; revive it and it contributes its subtests, most of
which fail at first. So the raw pass count can FALL while the result is strictly
better."* Its headline number is **files revived**.

**The site scoreboard** boots one QEMU per live site and writes a dated snapshot;
the delta between two snapshots is the product. Its header states its own blind
spot: *"`changed px` cannot tell a rendered page from a flat dark block."*
`text run/B` is the cheap middle — not WHERE the pixels are but **WHICH WORDS** are
among them, with the coordinate of every run, collected at the one site that
paints document text so it cannot drift from what was drawn. It found its own
reason to exist on the first run: bilibili scored PAINTED with ~255,000 changed
pixels and its titles were painted *inside* the thumbnail's box, i.e. underneath
the image drawn after them. *"Missing" and "painted underneath" look identical in
a list of strings and completely different in a list of coordinates.*
**`HARNESS` is a verdict now** (`dc02b9801`) — a row that measured nothing no
longer publishes as a browser finding — and `make test-sites-merge` gates the
merge rule.

### Media

- **H.264** (`c/lib/video/`) is **no longer baseline-only**: `h264_cabac.c` exists
  and the matrix covers Baseline (CAVLC, I/P), **Main (CABAC, B slices)** and
  **High (8×8 transform, scaling matrices, B pyramids)** — *"a High-profile stream
  is what every real web video is, so the Baseline half of this list is now the
  regression half."* `make test-h264` gates **27** named x264-generated cases plus
  a committed fixture, byte-exact. Bit-exactness is the bar, not a tolerance:
  H.264 reconstruction is exactly specified integer arithmetic.
  Gotchas worth keeping: `mbinfo` uses raster order for `mv[]` but **Z order for
  `nz[]`**; mvp's A/B/C all come from the partition's top-left corner and only C
  steps right; an INTRA neighbour is *available* with refIdx −1; `ref_idx` is
  coded per partition but read back per 8×8 quadrant; deblocking compares
  reference **pictures**, not indices; and a weight of 128 is inferrable though not
  codable, so clamping to 127 silently darkens every frame.
- **H.265/HEVC** — nal, cabac, pred, mc, deblock; nine gates. `test-h265` is the
  bit-exact list, `test-h265-diff` "the honest picture", `test-h265-m10` is
  **Main 10** gated at the same bar. **`test-h265-b` is red**: B slices,
  declared incomplete, `got 79 want 80`. And a size-dependent failure sits outside
  all nine: `test-vidbench-guest` returns `decode error -3` for h265 at 1280x720
  while 640x360 works.
- **MPEG-1/MPEG-2** (`mpeg12*.c`) — 31 of 31 bit-exact. **The IDCT is pinned to
  ffmpeg's `-idct simple` on purpose**, and that is what makes the gate mean
  anything: Annex A specifies the inverse DCT only by an *accuracy requirement*,
  so two conforming decoders may differ by ±1 — and because P and B pictures
  predict from the reconstruction, that ±1 accumulates for the rest of the GOP.
- **MJPEG** decodes each frame through `c/lib/image`'s `img_decode()`. **That is
  why the video library now depends on the image library**, and why `vidcheck`
  and `terminal` grew `$(IMGCHK_OBJ) $(GFX_OBJ) $(RUST_LIB)` — 459,728 →
  1,766,096 bytes for vidcheck. Still the right trade: a second baseline JPEG
  decoder in `c/lib/video` is the fourth-rasterizer mistake in another subsystem.
- **Audio codecs**: wav, mp3, flac, vorbis, aac, **opus**. Opus implements the
  **CELT half only** and **refuses SILK and hybrid frames by name** with distinct
  error codes, "because a player that gets silence cannot tell 'this codec does
  not do speech yet' from 'this file is quiet'". `vorbis.c` has **its own bit
  reader** because Vorbis packs LSB-first, the opposite of every other codec here
  — getting it backwards reads a plausible number of bits and then desynchronises
  in the middle of a stream.
- **Kernel audio** (`c/kernel/audio/` + `c/drivers/audio/hda.c`, 1,143 lines) —
  CORB/RIRB verb rings, codec-graph walk, BDL scatter-list DMA. The driver
  reports the codec graph it found rather than just "ok", because *"the DAC must
  be told which stream number to listen to, or the DMA engine runs happily, LPIB
  advances, every register reads back correct — and there is silence. That
  failure looks exactly like success from the controller's side."* The mixer is a
  **thread, not a poll loop**: "a refill is due every 21 ms forever, and a poll
  loop either burns a core or misses the deadline". The ISR does a counter bump
  and a `sem_post` and nothing else.
- **Containers** (`c/lib/media/`): `demux.c` + `mp4.c` + `mkv.c` + `avclock.c`.
  **Sniffing is by CONTENT, not by name** — a `.mp4` that is really Matroska opens
  as Matroska. `avclock.c` is a **policy, not a conversion**: a container hands out
  two streams of timestamps written by an encoder on another machine, and they
  mean nothing until something decides what "now" is; on this machine a
  from-scratch H.264 decoder under TCG is not guaranteed to keep up, so the
  falling-behind policy is the design, not an error path.
- **`subs.c` (WebVTT + SRT, 1,030 lines) is parsed and unreachable.**
  `media.h:99` says it in one line: `MEDIA_TRACK_OTHER 3 /* subtitles, timecode,
  data: indexed, never read */`. Zero files outside tests include it.
- **Video has no player.** The whole h264/h265/mpeg12 stack cannot be reached from
  `<video src>`; Preview picks image-or-video by sniffing the Annex-B start code.
  `test-mse-os` is red — playback stalls with `decoded=60 shown=59` and the audio
  master clock not advancing.
- **VP8 inter frames exist and are OFF.** `rust/src/vp8_inter.rs` (1,292 lines) is
  a full inter-frame decoder, bit-exact against ffmpeg on 4 IVF cases including a
  stream with **4 hidden alt-ref frames** — behind a Cargo feature, **default
  off**, and `$(RUST_LIB)` passes no `--features`, so a WebP decode still hits
  `return None; // interframe`. It exists, it is gated, and **it has no consumer**.
- **VP9 is a committed gate whose decoder was never committed.** `tests/vp9.mk`
  defines `test-vp9` against `tests/unit/vp9_test.c` and `c/lib/video/vp9*.c`,
  none of which exist. The fragment's header is honest about what it *would*
  measure ("VP9_GATE IS EMPTY, AND THAT IS THE MEASUREMENT … 17 of 17 cases decode
  every frame and every one of them is WRONG"), but it is describing a decoder
  that is not in the tree.

### Image decoders

Not derivable from the file list: **PNG, BMP, ICO, WebP and inflate are RUST**;
JPEG, GIF, SVG and EXIF are C in `c/lib/image/`.

| format | state |
|---|---|
| PNG | complete — every bit depth (1/2/4/8/16), all five filters, Adam7, tRNS |
| GIF | complete — animation, per-frame sub-rects, all disposal modes |
| BMP / ICO | complete, including RLE4 and 32bpp with a real alpha mask |
| JPEG | baseline **and progressive**; **maxd=0** against `djpeg -nosmooth -dct int` on all 13 cases |
| WebP | VP8L **and VP8 key frames** with the ALPH plane; byte-exact vs `dwebp -nofancy` |
| SVG | on the shared engine — fill AND stroke; its own filler is gone |

Four things to know before touching VP8:

- **The tables are GENERATED, not typed** (`tools/gen_vp8_tables.py` from RFC
  6386's own reference source). 3,164 probabilities and tree indices. This is not
  tidiness: a wrong probability does not shade a pixel, it **desynchronises the
  arithmetic decoder into noise**, and one wrong byte in three thousand is not
  findable by looking. The generator refuses rather than guesses — and it did not
  strip the RFC's page furniture at first, which is how "Bankoski" came to be
  parsed as an enumerator.
- **A B_PRED subblock on the macroblock's right edge takes its above-right samples
  from the row above the MACROBLOCK**, for all four subblock rows. This is the
  format's most-reimplemented bug; `--features vp8-tr-from-subblock` is it on a
  switch and reddens 19 of 31 — the twelve that survive are the smooth cases the
  encoder never coded as B_PRED, which is the control showing *which* cases carry
  the property.
- **The loop filter is a second pass over the finished frame.** Intra prediction
  reads its neighbours' UNFILTERED samples; filtering per macroblock feeds
  filtered pixels into the next row's predictor and drifts.
- **A third control was written and DELETED**: clearing the Y2 non-zero context on
  every skipped macroblock is a real rule and no case in the corpus reaches it, so
  the control passed. A control that cannot be watched failing is worse than none.

**Progressive JPEG is a SECOND path inside `jpeg.c`, not a generalisation** —
baseline never holds more than one block of coefficients while progressive must
hold the whole image until EOI, so merging them would charge every ordinary JPEG
progressive's memory. **A scan naming ONE component walks that component's OWN
block grid**, not the padded MCU grid — the two differ whenever the image is not a
whole number of MCUs, which is most images, and getting it wrong shears the
picture. `-DJPEG_PROG_MCU_GRID` fails **only** at 23×17 and hides completely at
64×48, which is why both sizes are in the corpus.

### The desktop: window manager, IME, and what it costs

`c/kernel/gui/`: `wm.c` (5,594 lines — the largest file in the tree), `fb.c`,
`text.c`, `notify.c`, `clipboard.c`, `ime_ui.c`.

- **Damage tracking is real and it works.** The compositor recomposites only
  rectangles something reported as damaged, and that is a **correctness** change
  underneath: `dirty_rect` used to throw all four arguments away, and now every
  caller is held to reporting the true extent. A caller that under-reports leaves
  pixels on screen that no longer belong there and **nothing repaints them** —
  there is no periodic full repaint left to cover for it. The menu bar repaints a
  24-point strip twice a second instead of the whole screen.
- **Two primitives read a NEIGHBOURHOOD and therefore cannot be clipped**:
  `fb_blur_rect` and `fb_liquid_glass`. The backdrop they would sample outside the
  clip is *last frame's output — already frosted*. So `dmg_expand` grows any
  damage rectangle that touches a glass panel until it contains the **whole
  panel**. That is a deliberate, argued design, and it is also where the frame
  time goes (below).
- **`SYS_GUI_FLUSH` carries no rectangle**, and `wm.c:2113` states the cost:
  *"the flush carries no rectangle, so the smallest honest extent an app can be
  held to is its whole canvas. That is the floor on an app repaint, and it is an
  ABI limit, not a compositor one."*
- **The pointer is on the display's hardware cursor plane** (`virtio_gpu.c`), so
  pure motion costs nothing — *"at 1920×1200 that is 2.3 M pixels of work to move
  an arrow eight of them"* if it were composited. **Consequence for harnesses: the
  arrow is not in a screendump**, so any driver that locates the cursor in the
  picture will silently fail.
- **Notifications** (`notify.c`) — "before this the machine had exactly two ways
  to say anything: open a window in the user's face, or say nothing".
- **The clipboard** (`clipboard.c`) holds a UTF-8 well-formedness invariant, and
  both halves are argued as jointly necessary: validating on the way in without
  truncating safely on the way out still produces a broken character; truncating
  safely without validating walks a byte sequence whose structure was never
  checked. `/bin/clip` is its instrument and a **test harness first** — the claim
  is that the clipboard survives the death of the process that filled it, and the
  only way to test that is two real processes.
- **The settings store** (`settings.c`, 1,520 lines) is kernel-resident, and
  reason 4 in its header is the one a newcomer gets wrong: *"LogitFS rewrites a
  WHOLE FILE per write. Two apps holding two copies of the settings and each
  writing the whole file back would silently destroy each other's keys — not a
  race on one key, a total loss. There has to be exactly one writer."*

**The Chinese input method.** `c/lib/ime/pinyin.c` (freestanding, no libc, no
allocator, **compiled into the kernel**) + `c/kernel/gui/ime_ui.c` (the
composition state machine and candidate bar). Dictionary `/ime/pinyin.dat`,
**572,983 bytes / 25,945 keys**, indexed in place at load and never copied.

- **The toggle is `Shift+Space`** — `IME_TOGGLE_MOD` / `IME_TOGGLE_NAME` in
  `ime_ui.h`, defined **once**. It was `Ctrl+Space` until 2026-08-28, spelled in
  eight literals, and it had to change for a reason no test in this tree can see:
  **macOS consumes Ctrl+Space itself** as "select the previous input source", so
  the chord never reached the guest. The guest side was never broken —
  `run-ime-test.sh` drives the identical chord over QMP, beneath the host
  keyboard, and passes.
- **`Shift+Space` is not free**: it no longer types a space, so typing fast enough
  to still hold Shift from a capital when the space arrives toggles the IME. That
  is stated in `ime_ui.h` rather than discovered.
- Composing: `a-z` appends, `'` separates syllables, **Space commits candidate 1**,
  **1-9** commit from the current page (9 per page), **Enter** commits the raw
  letters, **←/→ or PgUp/PgDn** page, **Esc** cancels. An unknown key **drops**
  the composition rather than committing it — "a candidate they never chose is
  worse in their document than three lost keys" — and Ctrl/Alt/Cmd + anything
  cancels, so Ctrl+S saves without a half-typed romanisation.
- **Shift+letter while not composing types a capital straight through**, so names
  and acronyms need no toggle.
- **The IME is PER WINDOW** (`g_on[wi]`), not a machine mode.
- **It commits a full code point, and `(char)k` truncates it** — `aui.c:1459`
  records the trap. Any widget or app that stores a key as a `char` silently
  corrupts every Chinese character while looking correct for ASCII.
- **Until 2026-08-28 the toggle changed NOTHING a user could see.** Measured by
  injecting the chord over QMP and screendumping either side: **175 changed pixels
  of 2,304,000, all of them the clock.** The state line goes to serial; the
  candidate bar does not exist until a composition is open. So the machine
  answered a deliberate keystroke with silence while the *host* answered
  Ctrl+Space with an animation — **the only key that replied was the wrong one**,
  which is exactly how the binding came to feel like Ctrl+Space. There is a
  menu-bar 中/EN indicator now (`draw_menubar`, damaged through
  `wm_damage_menubar()`), and the toggle moves 521 pixels instead of 175.

---

## The desktop is slow, and this is where the time goes — measured 2026-08-28

The complaint was "very laggy, and I don't know why", at the shipped default
(`make run`, 1920×1200, 4-core TCG on an Apple M4 Max). `tests/qmp/qmp_repaint.py`
is the instrument built for exactly this question — *"'It still feels laggy' is
not a number. This driver turns the sentence into one table, per EVENT CLASS"* —
and **no make target had ever run it**; three other drivers import it as a
library.

| interaction | ms/composite | fps | composited px/frame | present (copy+DMA) | full-screen frames |
|---|---|---|---|---|---|
| drag a small window | 26.2 | 38 | 890k (38.7%) | 1.68 ms (6.4%) | **0** |
| **drag a large window** | **94.0** | **10.6** | 1.54M (66.9%) | 2.67 ms (2.8%) | **0** |
| dock hover | 46.2 | 21.6 | 223k (9.7%) | 0.44 ms (0.9%) | **0** |
| **one keystroke into TextEdit** | **93.9** | **10.6** | 1.57M (68.0%) | 2.67 ms (2.8%) | **0** |
| dark-mode switch | 51.3 | 19.5 | 942k (40.9%) | 1.55 ms (3.0%) | 17 |
| **scroll the Terminal** | **103.8** | **9.6** | 1.55M (67.3%) | 2.68 ms (2.6%) | **0** |

**Two obvious hypotheses die on these numbers.** It is **not** full-screen
repainting — `full-screen frames = 0` everywhere except the theme switch, which
genuinely repaints everything. It is **not** the GPU — present is **0.9%–6.4%** of
a frame.

**Cost is linear in composited pixels, with a large per-pixel constant on glass.**
Re-run at 1280×800 (2.25× fewer pixels): every workload came back **2.11–2.45×**
faster, and the per-pixel cost was identical at both resolutions — 29/30 ns/px for
an ordinary drag, **207/201 ns/px for the dock**, which is almost all glass.
Fitting those two rates says the menu bar + dock account for roughly half of a
large-window drag frame.

**~~kprof confirms it independently.~~ DO NOT QUOTE THE kprof NUMBER — THE
INSTRUMENT WAS BROKEN AND THE CONTROL THAT PROVED IT IS THE INTERESTING PART.**
The old sentence read: "after removing the 114 user-mode samples,
`fb_liquid_glass_cut` is 76 of 178 kernel-busy samples and `gl_isqrt` another
14 — 50.6%." Re-measured 2026-08-29 with an experiment whose answer is known
from a third source: an IDLE desktop composites exactly one thing, the menu bar,
1920×36 = 69,120 px twice a second — and over 44 s the counters read 88
composites at **cpx/composite = 69120.0 with no rounding**. At kprof's sampling
rate, 615 samples were DUE in `fb_liquid_glass_cut`. It reported **107 (17%)**
and put **1,322 on `spin_unlock_irqrestore`** — the `sti` at a non-nested kernel
exit, because `interrupts.c:156` takes `g_bkl` with `spin_lock_irqsave` so every
kernel entry runs IF=0 and its samples land on the unlock that re-enables them.
kprof under-attributes the compositor by ~6× here. **The conclusion survives by
another method — 43–61% of a browser repaint frame, from the compositor's own
counters, which self-verify (every one-second interval decomposes into an
INTEGER number of 69,120 px and 1,895,445 px frames, residual exactly 0) — but
the 50.6% and the "two methods that share no code agree" are withdrawn.**

**AND THE GLASS RATE HAS ALREADY HALVED.** This file and `wm.c:426-448` both
quote ~205 ns/px. Measured 2026-08-29: **102 ns/px on the menu bar, 107 on the
dock hover**, two shapes sharing no arithmetic. `fb.c:1168-1260` is why —
band/tilt are E-entry tables, the cut folding and horizontal half-distance are
a per-column `colq[]`, and a **row-dominant run** evaluates the SDF, both
`gl_isqrt` calls, the normal and the displacement ONCE PER ROW for ~90% of the
dock. So "cache the per-pixel field" below is **largely already done, by
hoisting rather than caching** (`4aa5f5cd7`), and what remains inside the glass
is the BLUR — two moving-sum passes over the live backdrop, which can never be
cached. `GLASS_FIELD_SLOW` existed in `fb.c` and nothing in the tree ever
defined it — a control that could not be exercised; `make GLASSSLOW=1` now
exercises it.

**THE LARGER LEVER IS THE FLUSH RECTANGLE, NOT THE GLASS.** A browser scroll
damages 1731×1095 = **1,895,445 px = 82.3% of the screen**, and **two thirds of
the glass in that frame is not the browser's** — 196,533 of 297,873 glass pixels
are the dock and the Finder's titlebar, in the frame only because `dmg_expand`
grew the browser's rectangle into panels the browser never touched.

**But the most important number in that profile is 94.95% idle.** Three of four
cores are halted while the machine feels slow. The lag is not a capacity problem:
it is a **single-core, BKL-held, 94 ms critical section**, during which nothing
else advances, with everything halted between frames. Adding cores does nothing —
which is why it is slow and the fan does not spin.

**What to do, in leverage order:**

1. **Change the resolution.** `make run QEMU_GPU="-vga none -device
   virtio-gpu-pci,xres=1280,yres=800"` is **2.2×** for free, and the *logical*
   desktop is identical — 1920×1200 runs at 150% scale with a 1280×800 pt desktop,
   so you lose HiDPI crispness and nothing else. Large-window drag 10.6 → 22.7 fps.
2. **Cache the glass field.** Reading `fb.c:1053-1164`, the only backdrop-dependent
   work in the per-pixel loop is three `unpack(g[...])` samples and the
   partial-coverage destination read. Everything else — two `gl_isqrt` calls, the
   SDF, coverage, the normal, the displacement, band/facing/tilt/env/fresnel/
   spec/shadow — is a **pure function of (i, j, w, h, radius, cut)**, recomputed
   every frame for a panel whose geometry has not changed. `glass_build_lut(E,
   REFRACT)` already caches part of it; the per-pixel field is what is missing.
   Roughly 1.8 MB for the menu bar and dock.
3. **Give `gui_flush` a rectangle.** A keystroke recompositing 1.57 M pixels is the
   typing/scrolling/browser row, and it is an ABI gap, not a compositor one.
4. **Release the BKL inside the composite.** A project, not a patch: a full-screen
   frame is one damage rectangle, so releasing between rectangles does nothing for
   the worst case.

**Do not quote `bench-gfx-frame` at this.** It builds the gallery through `aui.c`
and `nm` finds **zero** `browser_paint` symbols in it, so it measures the engine as
the toolkit uses it and never as the browser does — and on a loaded host it is not
repeatable: two runs of the same binary read **19,871 µs and 12,707 µs**.

---

## The BKL: what it costs, measured

**The concurrency model is one lock taken on kernel entry** — but there are **TWO
acquisition sites now**, and the second is not in `interrupts.c`: the device
model's `irq_isr_entry()` (`c/drivers/core/irq.c:186`) takes `g_bkl` itself under
the same nested-entry rule, with **no allow-list check of any kind**. Anyone
widening `syscall_is_bkl_free()` or counting acquisitions has to read both.

**First measurement, and it was not about the lock:** 98% of every kernel entry
was an application taking the global lock to be told nothing had happened —
`SYS_POLL_EVENT` 49%, `SYS_YIELD` 49%. `SYS_WAIT_EVENT` deleted it: syscalls
3,283,157 → 1,234 (**2,660×**), BKL acquisitions 3,390,115 → 18,571 (183×). **The
waiting barely moved** (6,685 ms → 6,549 ms), and that is the finding: what went
away was 3.4 million cheap acquisitions, and the wait that remains was always real
work.

**Second measurement, which refuted the obvious next step.** The plan was to widen
the allow-list. Sampling holders from inside the timer tick — which runs *before*
the interrupt entry takes the lock, so the observer is not itself a holder —
**no syscall appears at all**. The lock is free ~80% of the time; it is a
bottleneck because of *who* holds it and *for how long at a stretch*: the
compositor, which re-takes it on waking and holds it through the whole frame.

**Do not quote a precise share.** Three runs on four cores reported the
compositor's share as 55%, 63% and 80% — that is n≈100, not the machine. The
robust version is one core, where it is **99% of held time**: not the largest of
several holders, *the* holder.

**Third: the desktop idles at zero.** 49,283 samples over 12 s of live desktop —
`sched_become_idle` 49.1%, `file_read` 24.9%, `wm_run` 24.5%. All three are the
instruction after a `hlt`. **98.5% of samples are halted cores.** Read `file.c`'s
comment above `tty_read` before quoting this at anyone: a sampling interrupt on a
halted core records the RIP *following* the halt, which is exactly how these three
addresses were once read as two busy-waits eating half the machine.

**AND THE LOCK THAT ACTUALLY SERIALISED THIS MACHINE WAS NOT THE BKL.** `make
test-smp` had been failing with "no wall-clock speedup (kmalloc still serialized
by the BKL?)". Sampling every lock's ticket counter on a *running* machine showed
the guess wrong by 834×: `kheap_lock` +30.7 **million** against `g_bkl` +36,836.
Per-core magazines took it to about 112 acquisitions, and `test-smp` from
**T1=5s TN=41s to T1=5s TN=6s**.

Four things about that layer, each load-bearing: **exact size classes only** (a
pop is always a perfect fit, so there is no search and no "close enough" that
would make the magazines a second, worse free list); **a lock per core, not
lock-free** (the win was never that the atomic disappeared — it is that four cores
no longer queue for one); **a block in a magazine is still ALLOCATED**, so the
double-free refusal still fires; and **drain before OOM**, because returning NULL
with blocks parked in magazines is an out-of-memory that is not true.

`tools/bkl_shared.py` enumerates every shared mutable static the BKL still covers,
and it is a tool rather than a paragraph for a measured reason: *"c/fs/logitfs.c
carries the most honest comment in the tree about this, and it names four of the
ten statics declared immediately above it. Five of the six it omits are the
journal TRANSACTION itself. If the careful comment lists less than half, prose is
the wrong medium."*

---

## AetherScript — and the self-hosting tax

`c/apps/as/` — a from-scratch language, shipped as `/bin/as`, ~7.9 kLOC. Do not
plan against the M20 feature list: dict, closures, classes (`class`/`super`,
copy-down inheritance), exceptions with unwinding, bitwise/shift/`**`, a
16-byte tagged `Value` (deliberately **not** NaN-boxed — an AetherScript int is a
full int64, so there are no spare bits in 8; the cost worth removing was the
memory round-trip, not the footprint), shapes with property inline caches, a
global-lookup cache with generation invalidation, mark-sweep GC over a
**contiguous object registry**, and computed-goto dispatch all exist.

**M27 ports** made OS endpoints first-class values — `O_PORT`/`O_PROC`, the `|>`
pipeline operator, `-> path` / `<- path` redirection, `with` scopes with
deterministic release, and iteration that reuses `OP_LEN` + `OP_INDEX_GET` rather
than inventing an iterator protocol. Its payoff is
`fsroot/as/examples/ash.as`: **the system shell, written in AetherScript**, with
no `fork`, no `dup2`, no `waitpid` and no file-descriptor arithmetic anywhere —
against 982 lines of C in `sh.c` doing the same job.

**M28 capabilities is IMPLEMENTED**, not a locked design waiting to be built:
`tests/as-m28.mk` gates attenuation across the whole 64×64 (held, requested)
lattice, and both negative controls were watched failing at their predicted counts.
`as_caps_set()` and `as_cap_attenuate()` have no script-visible entry point on
purpose. **The next unbuilt pillar is M29 tasks** — no target, no source, no spec.

**THE SELF-HOSTING TAX.** `fsroot/as/lib/asc.as` is a second compiler for
AetherScript, written in AetherScript, and it compiles itself to a **byte-identical
fixpoint**. The gate for that is **`test-selfhost-fixpoint`** (with `-lex` and
`-compile`); `test-as-bcstable` is a different and coarser gate that hashes every
compiled stdlib module against a checked-in baseline, existing to catch a codegen
perturbation "long before the fixpoint test would notice a 37 KB binary moved".

**The opcode block in `asc.as` is GENERATED, not hand-copied** — this file said
the opposite and called it "the single most important thing to know before
touching this language". `tools/gen_as_opcodes.py --write` **exists**; the
`OP_*`/`AS_BC_VERSION`/`K_*`/`T_*` constants live in a marker-delimited region
emitted from the C authority, and `--check` re-emits and compares **byte for
byte**, so a hand edit inside the markers is rejected as a whole-region diff and a
renumber is one command. What is **still hand-maintained and only checked** is
`aslex.as`'s `T_*`/`KEYWORDS`, `complete.c`'s IDE tables, `vm.c`'s `dispatch[]`
and `as_bc.c`'s `OPNAMES` — the tool names `aslex.as` as the remaining
silent-miscompile hazard.

**`make run` DOES run `check-asops`** (via `$(AS_LA)`'s order-only prerequisite),
which this file also had backwards. What genuinely does not reach it is `all` /
`$(ISO)` and `test-ash`.

---

## Porting real software: mini-libc, TCC, and the sysroot

`c/apps/libc/` is a real freestanding C library — **44 files, 12,863 lines of
`src/` plus 3,761 of headers**. `AS_LIBC := $(wildcard c/apps/libc/src/*.c)` feeds
`LIBC_OBJS`, so **a new `.c` here needs no build-system change**; that is why this
area parallelises and most of the tree does not.

**The gate is a diff against glibc, and that is the point**: this code is either
pure computation or a thin wrapper over a call the host also has, so "correct"
means "agrees with glibc" — which gives every function a *reference* instead of a
hand-written expectation that only records what its author already believed. Each
test source compiles twice (once against our headers with `-nostdinc`, once as an
ordinary host program) and the two stdouts are diffed byte for byte.

**Nothing here is stubbed to success**, and that is a rule: `flock` returns
`ENOSYS` because a caller that gets 0 believes it holds a lock; `ioctl`'s tty
requests return `ENOTTY`, matching `termios.c` rather than inventing a second
answer; `statvfs`/`utime` return `ENOSYS` because a fabricated `f_bsize` is worse
for a caller sizing a buffer than an error is.

**TinyCC is vendored, patched, and runs on the device.** `third_party/tcc/` (398
files, 91k lines) is **not a pristine upstream copy** — it is patched in place
under `TCC_LOGIT`, and the load-bearing patch is the link base: `ELF_START_ADDR`
0x400000 → 0x50000000, because *"tcc's default 0x400000 is shared kernel low
memory on this machine; a default that needs `-Wl,-Ttext` to be safe is a default
every user gets wrong once."*

**The bar that cannot be talked around** is `test-tcc-identity`: the same
`hello_id.c` and the same `tccpp.c` (3,903 lines of tcc's own source, 11 mini-libc
headers) are compiled to objects **by `/bin/tcc` on the device** and by the host
tcc, and the `.o` files must be **byte-identical** — retrieved off the (non
`-snapshot`) image by the harness's own embedded LogitFS reader, with a negative
control (one `-D` on one side must change the bytes).

`tools/mksysroot.py` lays out `/usr/include`, `/usr/lib/libc.a`, `crt1.o/crti.o/
crtn.o` and `/usr/lib/tcc/{include,libtcc1.a}` so `tcc hello.c -o hello` needs no
flags. Two facts: the names are **read out of the port**, not assumed
(`libtcc.c:974` adds `crt1.o` and `crti.o` before the user's files); and a sysroot
built by the compiler it serves would be circular, so `libtcc1`'s objects come
from clang. **The header trap**: mini-libc's headers were written for clang, and
tcc 0.9.27 has no `__int128`, no vector types, a subset of `__builtin_*` — every
header is parsed by the host tcc before it ships.

**Why TCC and not GCC, measured rather than chosen**: `cc1` is 35.7 MB with 515
undefined symbols, **334 of them from four from-scratch bignum/polyhedral
libraries** (isl 222, mpfr 71, mpc 21, gmp 20); tcc 0.9.27 is 36,151 lines with
an **empty libc gap list, proven by linking**, and emits ELF directly — no
assembler, no linker. GCC is blocked by binutils plus four libraries, not by the
kernel and not any longer by the filesystem.

**All six `test-sysroot*` targets are UNWIRED** — they exist and resolve and no
suite reaches them.

---

## Six subsystems this file did not describe, found by counting

Not noticed — **measured**, by comparing each subsystem's make-target count
against how often this file mentions it. The method is repeatable:

```sh
make -pRrq | grep -oE '^test-[a-z0-9]+' | sed 's/^test-//' | sort | uniq -c | sort -rn
# then grep -c each prefix in this file
```

Re-run on 2026-08-28 against the *previous* version of this document, it returned
**63 subsystem prefixes with ≥2 make targets and ≤1 mention** — including `uefi`,
`procfs`, `coredump`, `ptrace`, `oom`, `klog`, `panic`, `signal`, `devmodel`,
`ime`, `hda`, `nn`/`qwen`/`gguf`, `opus`/`aac`/`vorbis`, `mpeg12`/`vp9`, `ssh`,
`tcc`/`sysroot`/`selfhost`, `pkg`, `license`, `flex`, and the whole
`cssom`/`cssparse`/`selectors`/`csstyle` family. Eleven of them scored **zero**.

**An absent claim is worse than a stale one and much harder to see.** A wrong
sentence misdirects and can be corrected; a missing one lets a reader conclude the
subsystem does not exist, and there is nothing for a correction to attach to. IPv6
was the sharpest case: the networking sections ran M9→M12 and said "net"
thirty-seven times, so nobody concluded the documentation was thin — they
concluded the stack was IPv4-only.

That is why this rewrite names things it cannot describe in full. **If a
subsystem has make targets and no paragraph here, that is a bug in this file.**

Two more that deserve naming and had none:

- **`c/lib/nn` + `/bin/lm`** — 4,175 lines: tensors, f32/int8/int4 matvec, a
  LOGITLM single-file loader, a q8 KV cache, a forward pass, **real Qwen3-0.6B
  weights**, and 13 make targets in the tree's largest fragment
  (`tests/nn.mk`, 674 lines). The format was designed around **a filesystem limit
  that no longer exists** — "256 inodes, 223 already used; a model is therefore
  ONE FILE" — and `nn.h` still argues from it. `/bin/lm` says outright why it is
  not like its neighbours: *"vidcheck, audiocheck, h2check exist to print one
  number a harness can diff. This one is different: there is no independent oracle
  for 'how fast is this machine's own arithmetic', because the number IS the
  measurement."* **`matmul.c` includes `<emmintrin.h>` unguarded**, so `make
  test-nn` cannot compile on the documented host.
- **`tools/mmtrace/`** — a QEMU TCG plugin that records this machine's exact
  user-page reference string from outside the guest, plus a simulator that
  computes Belady's MIN on it. It answers the question the reclaim section
  explicitly does not: *"reclaim.h defends the clock against an active/inactive
  LRU and the defence is sound, but it is an argument about mechanism, not a
  measurement of quality: nothing in this tree says how many of the page faults
  this machine takes were AVOIDABLE."* `make test-mmsim` is declared **the
  control** — "run this before believing any number below" — and is green.

---

## The test suite — re-measured 2026-08-28

```
744 make targets (660 of them test-)      205 harnesses
118 wired into a suite                    395 unwired  (346 baselined, 49 NEW)
21 DEAD  (no target names them)            2 MUTE      61 stranded controls (55 baselined, 6 NEW)
CI would run 433 (282 host + 151 boot)    audit: 76 findings
```

Every one of those numbers moved from the previous version's (610/53/350/14/0),
in eleven days and ~145 commits. **Re-measure before quoting.**

**"UNWIRED" does not mean "CI does not run it"** — 372 of the 395 are run.
`tools/ci.sh` does not use the aggregates at all: it asks
`audit_tests.py --suites=host|boot`, which derives host-vs-boot **from the recipe**
and never consults `wired`. That is deliberate and the function says so: *"A
hand-written list of 'the suites CI runs' is the thing that rotted here, so the CI
asks the Makefile instead, and a target added tomorrow is picked up without anyone
remembering."* So UNWIRED measures reachability from `ci-host:`/`ci-boot:`
declarations — a mechanism CI stopped depending on — while reading like coverage.
Only **27** unwired targets are genuinely never run.

**The half that IS a coverage hole is the controls** (rule 5 above) and the DEAD
harnesses. **7 of the 21 DEAD are boot harnesses for shipped features**:
`run-sysroot-device.sh`, `run-pcachefill.sh`, `run-pcachepeak.sh`,
`run-elfshare.sh`, `run-execshare-test.sh` — and three kernel source files *cite*
two of those as the gates for live properties (`shm.h:21`, `fault.c:375`,
`mmsys.c:369`). The property the whole shm design rests on is gated by a script
nothing runs.

**MUTE is 2, and both are a fourth blind spot in the DETECTOR** rather than two
muted gates: `lfs_setexec.py` and `mk-tcc-disk.py` fail through
`sys.exit("message")`, which exits 1, and `find_mute`'s `exits_nonzero` list
matches a digit, a named variable, `raise SystemExit` and a bare assert — a
**string** argument matches none of them. An empty category is the point of the
category: a list of 28 with 24 false entries is a list nobody reads, and the 29th
— a real gate that swallowed its verdict — joins it unnoticed.

**Run everything with `make test-sweep`.** Host targets in parallel, device
targets one at a time, then **every failure re-run alone before it is believed** —
because several makes share one `build/` and two racing to produce the same object
make a target fail for reasons that have nothing to do with it. Measured, not
feared: `test-audio-codec-fuzz-deep`, `test-h265`, `test-demux` and
`test-csstext-all` all failed in the parallel phase and pass by themselves. **A
sweep that reports those is MANUFACTURING bugs**, which is worse than missing them.
`make test-sweep-host` is the half worth running after an ordinary change; the full
one boots QEMU ~156 times.

**Other gates worth knowing by name.** `tools/check-test-liveness.py` finds tests
that **cannot fail** (rule 1 fails the build; rules 2-4 warn) and is more specific
than prose: *"five drivers 'click the address bar' at a coordinate that has been
inside the window-manager titlebar for some time; they pass because the browser
happens to start with that field focused, so the click is decoration."* It is
currently RED on 7 scripts. `tools/break-build.sh` produces deliberately broken
disk images so `run-fullsystem-test.sh` can be **watched going red** for each
claim it makes — *"a green test that has never been shown to fail is not
evidence."* `tools/verify-commit.sh` builds from a clean clone because *"three
commits landed in one day whose own clean clone did not compile, twice from the
same mistake: a file was committed and the header it includes was not."*
`tools/perf/` measures every duration **inside the guest**, because "the host is
contended — other agents run QEMU concurrently — so host wall clock is worthless
here".

---

## Two binaries that could not be linked, and `make` said ok

Found by **deleting** `build/vidcheck.elf` and `build/terminal.elf` and asking for
them back. They did not come back: `undefined symbol: img_decode / img_free /
kmalloc / kfree`.

**It survived because make was RIGHT.** The stale `.elf` files were newer than the
new `.o` files, so make correctly reported them up to date and relinked nothing. A
full `make build/disk.img` exits 0, packs both binaries, and boots. Nothing is
wrong until someone deletes a file — and then it is four undefined symbols in a
link line nobody edited.

**`kmalloc` in ring 3 is a per-application shim**, and it is written down nowhere
else: `c/lib/image`'s five files and a dozen more in `c/apps/browser` declare
`void *kmalloc(unsigned long);` as a bare extern and rely on the **application** to
define it (`preview.c:64`, `browser_rt.c:44` each carry `{ return malloc(n); }`).
A new ring-3 program that links any of those libraries needs those two lines and
gets no diagnostic until the link.

**How to find the next one:** a link line is only proven by a link that actually
runs. `rm` the binary and ask for it back; timestamps cannot answer this question.

---

## What this machine is still missing

Measured 2026-08-28 by twelve parallel dimension audits, each finding subjected to
two independent adversarial refutation passes. **82 gaps survived**; the ones below
are the structural ones, in the sense that most of the rest are their projections.
Measured against this tree's own stated goal — *a macOS-style desktop that runs
software not written for LogitOS* — not against a generic OS checklist.

**1. There is no execution contract for a foreign program.** No pty, no termios,
no sessions, no process groups (`ptmx|openpty|setsid|tcsetpgrp` = 0 hits in
`c/kernel` and `c/fs`); `tty_read` echoes in the kernel one byte at a time and
cannot be turned off; **`/bin/sh` does not accept `-c`** (`"-c"` appears zero times
in `sh.c`) while `system()`, `popen()` and `sshd:642` all pass it; `execve` does
not honour `#!`. The consequence is not "a few missing programs": every `isatty`
branch, readline/ncurses/vi/less/top, `configure`/`install-sh`, `ssh host 'cmd'`,
and the 55 `.as` scripts that would like to live in `/bin` all hit the same wall —
which is why `ash.as`, the shell written in AetherScript, cannot be a login shell.

**2. There is no loading contract for a foreign binary.** `PT_INTERP`,
`PT_DYNAMIC` and `ET_DYN` are refused by name; zero relocations; no `dlopen`. The
user address space is one PDPT entry, `[0x40000000, 0x80000000)`, with a 496 MiB
mmap window, and every GUI app's link base is assigned by hand in the Makefile. A
runtime that reserves a large address space first — V8, a JVM, Go, ASan — dies on
the first reservation. That is **not** a RAM limit (a 64 MiB program loads fine);
it is an address-space limit. ASLR is unrepresentable as a consequence.

**3. There is no write-at-an-offset contract.** The VFS op table has
`write(path, buf, size)`, create-or-overwrite, and **no `->pwrite`**; no writable
file mapping; no `msync`; `ftruncate` can only grow. Changing one byte means
reading the whole file into the kernel heap and writing it all back. Stacked on
top: `file_close()` discards the whole-file flush's return value, `SYS_CLOSE`
returns 0 unconditionally, and nothing on the machine can report free space
(`statvfs` → `ENOSYS`, no `df`). **The only symptom of a failed write is that the
file quietly is not there.** sqlite, git, an editor and a package manager all need
this.

**4. There is no contract between an application and the desktop.** One process,
one window — a second `SYS_GUI_CREATE` returns 0 (success) and does nothing. The
menu bar is three string constants in the kernel and there is no syscall to publish
a menu. The input path has no key-release event and no focus event. The clipboard's
four flavours are all UTF-8 text; there is no drag-and-drop. Any ported toolkit
needs a second top-level surface and key-up on its first screen, and the ABI has
nowhere to put them.

**5. There is no clock running the gates.** See the top of this file.

Ranked below those, with the same evidence standard: the **munmap TLB hole**
(small change, XL consequence, documented in-tree as open); the browser being a
single process holding every capability while it is the only thing that runs
adversary code, with no ASLR and no stack canaries; **no bold and no italic**;
**no iframe, Worker, ServiceWorker, WebSocket or IndexedDB**, and `localStorage`
that does not survive a reboot; **no text coreutils and no on-machine editor** —
no `grep sed awk find sort diff tar less vi make df du kill ln chmod`; **the
machine has never booted on real hardware** (128 boot harnesses, all
`qemu-system-x86_64`), so every UEFI/NVMe/xHCI/HDA/MSI claim is a claim about a
machine nobody has seen; no modesetting, EDID or multi-monitor (one global
`fb_w`/`fb_h`, scanout 0, resolution frozen at firmware hand-off); no power
management at all (`struct driver` has no suspend/resume hook); `/dev` holds eight
synthetic names and no `mknod`; no init or service supervision (the only
`proc_spawn` is `/bin/login`, so **`sshd` has never run on the product image**);
no FAT, ISO9660 or USB mass storage — **this machine can boot from a FAT ESP and
cannot read one byte of it**; and no delivery loop: `all:` produces only the
kernel, there is no on-device `mkfs`, `pkgverify` deliberately does not install,
there are zero tags, and the build embeds absolute host paths.

**Three kinds, and they need different work:**

- **(a) Absent** — everything above. New code.
- **(b) Built with no real consumer** — the failure mode this tree already names.
  `fs_prefix` (grant, ceiling check, inheritance and `/proc` printing all exist;
  **zero enforcement points**) · `.lpk` signatures (root key compiled into the
  kernel; `aex.c` checks only CRC32) · panic/KASSERT/kdiag (921 lines; all four
  kernel call sites are inside `kdiag.c`'s own self-test, and
  `panic_from_exception` has zero callers) · **`layout_text.c`'s 1,352 lines of
  UAX #14 are not in `BROWSER_PIPE`**, so pages still break lines anywhere and
  bidi/shaping have never been called by a page · `fontcolor.c` · `gfx_fill_clipped`
  / `gfx_fill_subs` / `gfx_m_invert` · `syslogd` (never started; zero programs call
  `syslog()`) · ptrace (one `.as` example) · kernel modules (no `insmod`) · shm
  (only the libc wrapper) · the whole h264/h265 stack (unreachable from
  `<video src>`) · `subs.c` · VP8 inter frames · `virtio-rng` (not wired into
  `rng.c`, and its boot self-test prints 32 bytes of the entropy stream to a serial
  log every harness captures — harmless while nothing consumes it, a leaked seed
  the moment something does).
- **(c) Red gates** — the host-reality table at the top, plus `test-h265-b` (79/80),
  `test-mse-os` (playback stalls), `test-vidbench-guest` (h265 @ 720p), `test-vp9`
  (no decoder), `check-abi` (one generator refusal holding **17** targets down),
  and `test-audit`'s own 76 findings.

**What to do first, by leverage:** get (c) green, close the munmap hole, then write
the pty. While the gates are red nothing downstream can be *proved*, and this batch
of red covers the two largest investments in the tree (the browser and reclaim);
most of them are two-line wiring. munmap is the only item in the tree where a few
lines removes a silent ring-3-writes-ring-0. And pty is the single thing that
unlocks the whole "run other people's software" line at once — `sh -c`, `#!`, text
tools, an on-machine editor, usable ssh, and the daily work of the tcc self-hosting
line all hang off it. Address space and `pwrite` are the next two XLs; each is its
own project and neither should start while the gates are red.

---

Each milestone: spec → plan → implement. Specs in `docs/superpowers/specs/`.
`docs/CODE_AUDIT.md` is a 776-line security audit with 8 severe and 23 high
findings, all re-verified to file+line, that nothing in this tree pointed at until
now. `SECURITY.md` states the frame every section above should be read inside:
*"Do not treat its browser, TLS implementation, process isolation, filesystem, or
device drivers as a security boundary for hostile workloads."*
