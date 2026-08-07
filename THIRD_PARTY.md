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

musl's math sources contain code with several permissive origins. The vendored
files retain their individual notices, including MIT-licensed Arm code,
Sun/FreeBSD fdlibm notices, and OpenBSD/Stephen L. Moshier notices. Those
per-file terms control their respective files. The exact musl release or commit
used for the import was not recorded and should not be inferred from this tree.

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
