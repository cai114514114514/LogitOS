# Third-Party Software and Data

LogitOS OS uses the first-party GPL-3.0-or-later/MIT boundary defined in
[LICENSING.md](LICENSING.md). This file records code and data that came from, or
was materially derived from, other projects. Those components remain subject to
their upstream licenses; neither first-party license replaces or overrides them.

Local adapters, ports, patches, build integration, and other original changes
use GPL-3.0-or-later in core paths and MIT in outer paths, to the extent the
project is able to license them. Changes to upstream-covered files remain
subject to upstream terms where required.

## Summary

| Component | Purpose | Repository location | Version or snapshot | License |
|---|---|---|---|---|
| QuickJS | JavaScript engine | `third_party/quickjs/` | 2024-01-13 | MIT |
| musl libm subset | Math functions used by QuickJS | `third_party/libm/` | Exact upstream revision not recorded | MIT and other permissive per-file terms |
| NetSurf LibCSS | CSS parsing, cascade, and computed style | `third_party/css/libcss/` | Exact upstream revision not recorded | MIT |
| NetSurf libparserutils | Parser utilities used by LibCSS | `third_party/css/libparserutils/` | Exact upstream revision not recorded | MIT |
| NetSurf libwapcaplet | Interned strings used by LibCSS | `third_party/css/libwapcaplet/` | Exact upstream revision not recorded | MIT |
| Crafting Interpreters / clox | Architectural lineage and adapted implementation in AetherScript | `c/apps/as/` and related design documents | Exact upstream revision not recorded | MIT for upstream code |
| Mozilla/NSS-derived CA trust store | Public TLS trust anchors | `tools/roots/`, generated into `c/crypto/trust/roots_bundle.inc` | 130-root host bundle imported 2026-06-08; exact source revision/hash not recorded | MPL 2.0 for the Mozilla-derived bundle |
| Noto Sans SC and Noto Sans Mono | Source fonts and generated runtime subsets | `third_party/fonts/`, `fsroot/fonts/` | Google Fonts commit `2796410152d4f9524b68ed46e69c1b60f8e0f7c3` | SIL OFL 1.1 |
| DejaVu Sans | Unmodified shaping font shipped as `/fonts/text.ttf`; the only runtime font with Arabic, Hebrew and OpenType Layout tables | `third_party/fonts/DejaVuSans.ttf` | Debian `fonts-dejavu-core` 2.37-8build1 | Bitstream Vera Fonts License and the Arev fonts copyright |
| macOS Desktop Picture | Locally generated wallpaper | Ignored `fsroot/wallpaper.png`; see `tools/mkwallpaper.sh` | Host-installed macOS asset | Proprietary; not covered by this repository's license |
| GNU GRUB | Bootloader code embedded in generated ISO images | Generated `build/logit.iso`; no GRUB source is stored here | Host-tool version is not pinned | GPLv3 or later, subject to the exact GRUB distribution used |
| Rust core and compiler support | Runtime code pulled into Rust `staticlib` outputs and linked into LogitOS binaries | Generated Rust archives and final kernel/browser binaries | Rust toolchain version is not pinned | Primarily MIT or Apache-2.0, with upstream-noted exceptions |
| html5lib-tests and web-platform-tests HTML parser cases | Shared conformance test DATA for the HTML5 parser (no upstream code is used) | `third_party/html5lib-tests/` | tokenizer cases from html5lib-tests `master`, tree-construction cases from web-platform-tests `html/syntax/parsing/resources`, both imported 2026-08-07 | MIT (html5lib-tests) and BSD-3-Clause (web-platform-tests) |
| TinyCC (tcc) | On-device C compiler, ported as a LogitOS program (`tests/tcc.mk`) | `third_party/tcc/` | 0.9.27 (`third_party/tcc/VERSION:1`) | LGPL-2.1 for the compiler; a separate GPL-2.0-or-later grant with an explicit linking exception for the runtime-support object `lib/libtcc1.c` that is statically linked into every program `tcc` compiles on this OS -- see the dedicated section below |

## QuickJS

- Upstream: <https://bellard.org/quickjs/>
- Official source repository: <https://github.com/bellard/quickjs>
- Copyright: Fabrice Bellard and Charlie Gordon, as stated in the vendored
  source files.
- License: MIT.
- Imported by LogitOS OS commit `9374d8c` as the QuickJS 2024-01-13 core.

The repository vendors the core engine, regexp, Unicode, bignum, and utility
sources. LogitOS OS compiles them for its freestanding ring-3 environment with
its mini-libc and local build configuration. The standalone upstream tools and
standard library are not shipped as separate LogitOS applications.

The upstream copyright and MIT permission notice are retained in the vendored
QuickJS source files, including `quickjs.c`, `quickjs.h`, `cutils.*`,
`libregexp.*`, `libunicode.*`, and `libbf.*`.

## musl libm subset

- Upstream: <https://musl.libc.org/>
- Source repository: <https://git.musl-libc.org/cgit/musl/>
- Upstream copyright and license file:
  <https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT>
- Imported by LogitOS OS commit `9374d8c`.

`third_party/libm/` is a selected, double-precision-oriented subset of musl's
math library. The import omitted many float, long-double, Bessel, gamma, and FMA
sources and is compiled against LogitOS OS's freestanding headers.

musl's math sources contain code with several permissive origins, named in the
license file preserved with the import, `third_party/libm/COPYRIGHT` -- musl's
own upstream `COPYRIGHT`, per the URL above, not a summary written for this
project. Its `src/math/*` paragraph (`third_party/libm/COPYRIGHT:120-127`)
states the sources of the divergence directly: "Much of the math library code
... is Copyright (C) 1993,2004 Sun Microsystems or Copyright (C) 2003-2011
David Schultz or ... Bruce D. Evans or ... Stephen L. Moshier or ... Arm
Limited ... and labelled as such in comments in the individual source files.
All have been licensed under extremely permissive terms." That is not a
theoretical caveat here: of the 83 files under `third_party/libm/`, 18 open
with the Sun/FreeBSD `fdlibm` notice (for example
`third_party/libm/__rem_pio2.c:1-12` and `cbrt.c:1-12`, each "Copyright (C)
1993 by Sun Microsystems, Inc. ... Permission to use, copy, modify, and
distribute this software is freely granted, provided that this notice is
preserved" -- permissive, but not the MIT text) and 13 carry an explicit
`SPDX-License-Identifier: MIT` Arm Limited notice instead (for example
`third_party/libm/exp_data.c:1-6`, "Copyright (c) 2018, Arm Limited. SPDX-
License-Identifier: MIT"). `grep -l 'Copyright (C) 1993' third_party/libm/*.c`
and `grep -l 'Arm Limited' third_party/libm/*.c` reproduce both counts. The
summary row's "MIT and other permissive per-file terms" refers to exactly this
split; the authoritative list is the files themselves plus
`third_party/libm/COPYRIGHT`, not a second copy of it here. The exact musl
release or commit used for the import was not recorded and should not be
inferred from this tree.

## NetSurf CSS libraries

The following libraries were imported in LogitOS OS commit `f840b45` and trimmed
to the source, public headers, generators, and license files needed by the
browser port. Upstream tests, documentation, examples, CI configuration, and
NetSurf build files were not retained.

### LibCSS

- Upstream: <https://git.netsurf-browser.org/libcss.git/>
- Location: `third_party/css/libcss/`
- Copyright: Copyright (C) 2007 J-M Bell.
- License: MIT; see `third_party/css/libcss/COPYING`.

### libparserutils

- Upstream: <https://git.netsurf-browser.org/libparserutils.git/>
- Location: `third_party/css/libparserutils/`
- Copyright: Copyright (C) 2007-8 J-M Bell.
- License: MIT; see `third_party/css/libparserutils/COPYING`.

### libwapcaplet

- Upstream: <https://git.netsurf-browser.org/libwapcaplet.git/>
- Location: `third_party/css/libwapcaplet/`
- Copyright: Copyright 2009 The NetSurf Browser Project.
- License: MIT; see `third_party/css/libwapcaplet/COPYING`.

LogitOS OS's DOM select handler and computed-style adapter live in
`c/apps/browser/css_engine.c`; that adapter is project code and is covered by
the outer-component MIT License defined in `LICENSING.md`.

## Crafting Interpreters / clox lineage

- Upstream project: <https://github.com/munificent/craftinginterpreters>
- Upstream license: <https://github.com/munificent/craftinginterpreters/blob/master/LICENSE>
- Copyright: Copyright (c) 2015 Robert Nystrom.
- License for upstream `.c` and `.h` code: MIT.

AetherScript is not a verbatim vendored copy of clox, but its compiler and VM
have clox lineage and contain materially adapted implementations of concepts
and routines such as the Pratt compiler, upvalue resolution and capture,
closure closing, mark-sweep tracing, classes, and method dispatch. Relevant
project code is under `c/apps/as/`.

AetherScript adds and changes substantial functionality, including
indentation-based syntax, modules, exceptions, lists and dictionaries,
f-strings, comprehensions, bytecode serialization, a self-hosted compiler,
system calls, raw-memory operations, an OS standard library, and IDE
integration. These additions do not remove the obligation to retain the clox
upstream notice for adapted code.

Required upstream notice:

> Copyright (c) 2015 Robert Nystrom
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

The Crafting Interpreters book prose, illustrations, website, and other
non-code material use different terms, including CC BY-NC-ND 4.0. LogitOS OS
does not claim those materials under either first-party license.

## Mozilla/NSS-derived CA trust store

- Reference source: <https://curl.se/docs/caextract.html>
- Mozilla Public License 2.0: <https://www.mozilla.org/MPL/2.0/>
- Location: `tools/roots/*.pem`.
- Generated form: `c/crypto/trust/roots_bundle.inc`.

Git history records that commit `98d59fc` imported certificates from the host's
`/etc/ssl/cert.pem`, described there as an authentic Mozilla/NSS-derived store,
then deduplicated the set by SubjectPublicKeyInfo. `tools/genroots.py` extracts
only supported RSA and P-256/P-384 public keys into the generated C bundle.

Mozilla-derived PEM CA bundles are distributed under MPL 2.0. The individual
certificates also identify and are signed by their issuing certificate
authorities. The exact source bundle revision, download URL, and SHA-256 were
not recorded at import time, so the repository cannot currently reproduce the
external bundle provenance byte-for-byte from a named upstream snapshot.

## Fonts

- Upstream snapshot: Google Fonts commit
  `2796410152d4f9524b68ed46e69c1b60f8e0f7c3`.
- Source inputs: `third_party/fonts/NotoSansSC-VF.ttf` and
  `third_party/fonts/NotoSansMono-VF.ttf`.
- Generated runtime files: `fsroot/fonts/ui.ttf` and
  `fsroot/fonts/mono.ttf`.
- License: SIL Open Font License 1.1.

The repository preserves the unmodified upstream fonts, metadata, licenses,
source URLs, and SHA-256 values in `third_party/fonts/`. `tools/mkfont.py`
creates fixed-weight character subsets and changes their internal family names
to `LogitOS UI` and `LogitOS Mono`. It retains the upstream copyright and license
metadata. This is especially important because Noto Sans SC's notice reserves
the name `Source`.

The source fonts and modified subsets remain under the OFL; neither LogitOS
first-party license relicenses them. The normal disk-image rule packages only
the checked-in subsets and never reads Apple or other undeclared host fonts. It
also installs both complete OFL texts and the source record under
`/licenses/fonts/`.

`DejaVuSans.ttf` is a separate, later addition and is NOT part of the Noto
set above. It is vendored byte for byte from Debian's `fonts-dejavu-core`
2.37-8build1 (SHA-256
`b4c632e3cdf9acc7f28758fb5a323c8524d7fc6660d46904d9b6cbe2809c419c`) and is
shipped to the disk image unmodified as `/fonts/text.ttf`, because the two Noto
subsets carry neither Arabic nor Hebrew and lost their GSUB/GPOS tables to
subsetting -- so without it `c/lib/text/shape.c` has nothing to apply. It is
under the Bitstream Vera Fonts License plus the Arev fonts copyright, whose
rename clause binds only modified fonts; the full text is preserved as
`third_party/fonts/LICENSE-DejaVu.txt` and installed to
`/licenses/fonts/LICENSE-DejaVu.txt`.

## Wallpaper used by local builds

`tools/mkwallpaper.sh` explicitly converts a host-installed macOS Desktop Picture
into `fsroot/wallpaper.png`. The output is ignored by Git but is included in the
normal disk image when present. It has no recorded redistribution grant and is
not covered by either LogitOS first-party license. Replace it with an original or
suitably licensed asset, preserve that asset's provenance and notice, and
rebuild the disk before distributing a binary image.

## GNU GRUB in generated ISO images

- Upstream: <https://www.gnu.org/software/grub/>
- License: GNU GPL version 3 or later, subject to the exact files and GRUB build
  supplied by the host environment.
- Generated location: the bootloader content installed into `build/logit.iso`
  by `grub-mkrescue`.

GRUB is not vendored in the Git source tree, but it is more than a host-only tool:
`grub-mkrescue` places GRUB boot code into the distributable ISO. Anyone publishing
an LogitOS ISO must identify the exact GRUB version, include the applicable license
and notices, and satisfy the GPL corresponding-source requirements for the GRUB
binary in the manner required by that distribution. LogitOS Core's GPL license is
a separate grant from separate copyright holders and does not relicense the GRUB
files placed in the ISO.

## Rust runtime code in linked binaries

- Upstream: <https://github.com/rust-lang/rust>
- Linkage reference: <https://doc.rust-lang.org/reference/linkage.html>
- License summary: Rust is primarily distributed under MIT or Apache-2.0, with
  portions under BSD-like licenses; consult the `COPYRIGHT` and license files for
  the exact pinned toolchain.
- Generated inputs: `rust/target/**/liblogit_rust.a`.

The Rust crate uses `crate-type = ["staticlib"]`. Rust documents that a static
library contains the local crate together with its upstream dependencies. Even
though this crate is `no_std` and has no Cargo package dependencies, generated
archives and final LogitOS binaries may contain Rust `core` and compiler-support
code from the installed toolchain. A binary release therefore needs to pin the
Rust version and ship the notices applicable to that version.

## Provenance gaps to close

This inventory is intentionally explicit about missing historical metadata.
Future vendor updates should close these gaps rather than guessing:

1. Record the exact upstream commit or release and a source archive SHA-256 for
   musl libm and all three NetSurf libraries.
2. Preserve musl's upstream `COPYRIGHT` file alongside the selected libm
   sources in addition to the existing per-file notices.
3. Record the exact Crafting Interpreters revision used as the adaptation base.
4. Replace or regenerate the CA trust store from a named Mozilla/curl snapshot
   and record its URL, date, certificate count, and SHA-256.
5. Replace the local Apple-derived wallpaper with an original or redistributable
   default and record its source and license.
6. Pin the GRUB and Rust toolchain versions used for binary releases and collect
   their exact license files and corresponding-source information.
7. Keep local modifications as reviewable patches or commits when any vendored
   component is updated.

Other host tools such as LLVM/Clang, NASM, QEMU, xorriso, Python, and Perl are not
stored in this repository. They normally do not become part of LogitOS binaries,
but release tooling must verify the actual outputs rather than relying on that
assumption. GRUB and Rust are called out above because their code can enter the
generated ISO or linked binaries.

## html5lib-tests and web-platform-tests parser cases

- Upstream (tokenizer): <https://github.com/html5lib/html5lib-tests>
- Upstream (tree construction): <https://github.com/web-platform-tests/wpt>,
  directory `html/syntax/parsing/resources`
- Licenses: MIT for html5lib-tests, BSD-3-Clause for web-platform-tests. Both
  texts are vendored beside the data as `LICENSE.html5lib-tests` and
  `LICENSE.wpt`.

Only DATA is vendored -- `.dat` and `.test` case files. No upstream code is
compiled or shipped; the runner (`tests/unit/html5lib_test.c`) is ours. The
HTML parser itself is hand-written, and these cases exist so that claim can be
answered with a pass rate instead of an opinion.

Note that the tree-construction suite no longer lives in html5lib-tests: its
README now points at web-platform-tests as the sole maintained home, which is
why the two halves come from two repositories.

## TinyCC (tcc)

- Upstream: <https://repo.or.cz/tinycc.git> (mob branch; `RELICENSING` is that
  branch's own relicensing ledger, vendored at `third_party/tcc/RELICENSING`).
- Location: `third_party/tcc/`.
- Version: `0.9.27`, `third_party/tcc/VERSION:1`.
- Ported as a LogitOS program by `tests/tcc.mk`, `tests/sysroot.mk` and
  `tools/mksysroot.py` -- an active porting workflow this audit does not
  touch; this section only records what the vendored source itself says.

**tcc's own compiler and linker are LGPL-2.1.** The top-level license file is
the LGPL 2.1 text (`third_party/tcc/COPYING:1-2`, "GNU LESSER GENERAL PUBLIC
LICENSE / Version 2.1, February 1999"), and the individual front-end sources
cite it consistently: `third_party/tcc/libtcc.c:1-9`, `tcc.c:1-9` and
`x86_64-gen.c:1-11` each open "Copyright (c) ... / This library is free
software; you can redistribute it and/or modify it under the terms of the
GNU Lesser General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version." "Version 2 of the License" for the Lesser GPL is 2.1 -- LGPL 2.1's
own preamble says so (`third_party/tcc/COPYING:9-10`, "the successor of the
GNU Library Public License, version 2, hence the version number 2.1") -- so
the per-file header and the vendored COPYING text name the same license, not
two different ones.

**`RELICENSING` records an attempt to move the whole project to MIT that did
not complete.** `third_party/tcc/RELICENSING:31` records one "NO" vote
(Daniel Glöckner, for `arm-gen.c` specifically -- his own next line, `:32`,
is "YES" for everything else he touched) and `:45-46` record two "?"
(unanswered: Timo VJ Lähde for `tiny_libmaker.c`, "TK" for `tcccoff.c` and
`c67-gen.c`). None of those four files are
in this project's `TCC_TUS` build list (`tests/tcc.mk:62`: `tcc libtcc tccpp
tccgen tccelf tccasm x86_64-gen x86_64-link i386-asm`), so the unresolved
files are not compiled here -- but the ledger as a whole never reached
unanimous "YES", so TinyCC as a project remains under the LGPL-2.1 grant in
`COPYING` rather than the MIT text `RELICENSING` was collecting signatures
for. Treat any "TinyCC is MIT" claim as premature.

**`lib/libtcc1.c` is a second, separate grant, and it is the one that matters
for every program `tcc` compiles here.** `third_party/tcc/lib/libtcc1.c:4-9`
states copyright (Free Software Foundation, 1987-1995) and GPL version 2 "or
(at your option) any later version" as the base license, same as the
compiler above but GPL rather than LGPL. `libtcc1.c:11-18` then adds a
runtime exception; the one clause that settles it is line 11-13:

> the Free Software Foundation gives you unlimited permission to link the
> compiled version of this file into combinations with other programs, and to
> distribute those combinations without any restriction coming from the use
> of this file.

This is GPL-2.0-or-later (not LGPL) on the *file*, with the same runtime
exception GCC's own `libgcc` uses (unlimited permission to link the compiled
object into other programs and distribute the combination without
restriction). One clause settles the question this audit was asked: **a
program a user compiles with this `tcc` and links against the resulting
`libtcc1.a` does NOT become GPL-covered by that linkage** -- the exception
says so in as many words, in the file's own header. `LICENSES/GPL-2.0-or-
later.txt` carries the version-2 text this file names first; the "or (at
your option) any later version" clause also permits treating it as
GPL-3.0-or-later (this project already carries `LICENSES/GPL-3.0-or-later.txt`
for its own core) if a release chooses that option instead. Which text a
release actually names is a choice for the user and the porting line, not a
question this audit resolves -- only the fact that the exception exists, and
what it permits, is settled here.

**Whether that exception is the whole story depends on what else is in
`libtcc1.a`, and here it is not.** `third_party/tcc/lib/Makefile:47`
(upstream's own build) makes the *default* x86_64 archive
`libtcc1.o va_list.o alloca86_64.o alloca86_64-bt.o $(BCHECK_O)` --
`bcheck.c` (the bounds checker) included. `bcheck.c` is **not** GPL-with-exception;
its own header is plain LGPL: `third_party/tcc/lib/bcheck.c:6-9`, "This
library is free software ... under the terms of the GNU Lesser General
Public License ... either version 2 of the License, or (at your option) any
later version" -- no linking exception, because bounds-checking objects are
meant to be replaceable at link time by design, not baked into every binary
license-free. **This project's own build does not compile `bcheck.c` in.**
`tests/sysroot.mk:120-121` builds `usr/lib/tcc/libtcc1.a` from exactly three
sources -- `lib/libtcc1.c`, `lib/va_list.c`, `lib/alloca86_64.S` -- and
`--libtcc1-in-libc` (`tests/sysroot.mk:89`, the default) folds those same
three objects into `usr/lib/libc.a` as well, so `bcheck.o` is absent from
both archives a device-compiled program can link against. The LGPL
bounds-checker is vendored in the tree but not built into this OS's `tcc`.

**Two of those three files carry no license header of their own.**
`third_party/tcc/lib/va_list.c:1` opens directly with a plain comment ("/*
va_list.c - tinycc support for va_list on X86_64 */") and
`third_party/tcc/lib/alloca86_64.S:1-4` opens with a bare "/* alloca86_64.S
*/" comment -- neither states a license, a copyright holder, or a version.
Both compile as members of the same `libtcc1.a` archive `libtcc1.c` heads
(`tests/sysroot.mk:122`, "Member names match upstream's libtcc1.a"), and
both are TinyCC's own runtime-support code rather than a separate import, but
that is an inference from *placement*, not a license statement found in
either file. **This audit does not resolve that gap** -- it states it, rather
than assuming the neighboring file's exception carries over by proximity.

What a binary release needs, concretely: the LGPL-2.1 text (already in
`LICENSES/LGPL-2.1.txt`) for the compiler itself if `tcc.aex`/`host/tcc` is
distributed as a binary without its accompanying source -- LGPL 2.1 section 6
permits static linking (`libtcc.c` et al. are statically linked into
`tcc.aex`, there being no dynamic linker on this OS) provided the recipient
can obtain source or relinkable object code for the LGPL-covered parts, which
this git repository already provides for anyone who receives it; the
`LICENSES/GPL-2.0-or-later.txt` text (or, if the "or later" option above is
exercised, `LICENSES/GPL-3.0-or-later.txt`) plus the quoted linking-
exception clause for `libtcc1.a`; and a decision, not yet made in this tree,
about what notice (if any) accompanies `va_list.o`/`alloca86_64.o` given the
previous paragraph. `tcc.aex` is not on the project's main disk image
(`Makefile`'s `$(DISK)` rule) -- it is built to its own image by
`tests/boot/mk-tcc-disk.py`, a file inside the tcc porting workflow this
audit left untouched -- so none of the above is currently wired into
`RELEASE_NOTICES` or the main `/licenses` tree, and it does not need to be
until `tcc` ships as part of that image.
