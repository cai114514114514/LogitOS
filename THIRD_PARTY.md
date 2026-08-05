# Third-Party Software and Data

Aether OS is licensed under the repository's [MIT License](LICENSE) for code
authored by Aether OS contributors. This file records code and data that came
from, or was materially derived from, other projects. Those components remain
subject to their upstream licenses; the Aether OS license does not replace or
override them.

Local adapters, ports, patches, build integration, and other original changes
are licensed under the Aether OS MIT License to the extent the project is able
to license them.

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
| macOS Heiti SC and Menlo | Local build inputs for generated font subsets | Not tracked; see `fsroot/fonts/README.md` | Host-installed fonts | Proprietary; not covered by this repository's license |
| GNU GRUB | Bootloader code embedded in generated ISO images | Generated `build/aether.iso`; no GRUB source is stored here | Host-tool version is not pinned | GPLv3 or later, subject to the exact GRUB distribution used |
| Rust core and compiler support | Runtime code pulled into Rust `staticlib` outputs and linked into Aether binaries | Generated Rust archives and final kernel/browser binaries | Rust toolchain version is not pinned | Primarily MIT or Apache-2.0, with upstream-noted exceptions |

## QuickJS

- Upstream: <https://bellard.org/quickjs/>
- Official source repository: <https://github.com/bellard/quickjs>
- Copyright: Fabrice Bellard and Charlie Gordon, as stated in the vendored
  source files.
- License: MIT.
- Imported by Aether OS commit `9374d8c` as the QuickJS 2024-01-13 core.

The repository vendors the core engine, regexp, Unicode, bignum, and utility
sources. Aether OS compiles them for its freestanding ring-3 environment with
its mini-libc and local build configuration. The standalone upstream tools and
standard library are not shipped as separate Aether applications.

The upstream copyright and MIT permission notice are retained in the vendored
QuickJS source files, including `quickjs.c`, `quickjs.h`, `cutils.*`,
`libregexp.*`, `libunicode.*`, and `libbf.*`.

## musl libm subset

- Upstream: <https://musl.libc.org/>
- Source repository: <https://git.musl-libc.org/cgit/musl/>
- Upstream copyright and license file:
  <https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT>
- Imported by Aether OS commit `9374d8c`.

`third_party/libm/` is a selected, double-precision-oriented subset of musl's
math library. The import omitted many float, long-double, Bessel, gamma, and FMA
sources and is compiled against Aether OS's freestanding headers.

musl's math sources contain code with several permissive origins. The vendored
files retain their individual notices, including MIT-licensed Arm code,
Sun/FreeBSD fdlibm notices, and OpenBSD/Stephen L. Moshier notices. Those
per-file terms control their respective files. The exact musl release or commit
used for the import was not recorded and should not be inferred from this tree.

## NetSurf CSS libraries

The following libraries were imported in Aether OS commit `f840b45` and trimmed
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

Aether OS's DOM select handler and computed-style adapter live in
`c/apps/browser/css_engine.c`; that adapter is project code and is covered by
the repository MIT License.

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
non-code material use different terms, including CC BY-NC-ND 4.0. Aether OS
does not claim those materials under its MIT License.

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

## Fonts used by local builds

`tools/mkfont.py` defaults to subsets of macOS Heiti SC and Menlo. Those fonts
are proprietary, are intentionally ignored by Git, and are not licensed by the
Aether OS MIT License. Do not redistribute generated subsets unless the font
license permits it. For redistributable builds, use suitable OFL-licensed fonts
and pass them to the font-generation tool. See `fsroot/fonts/README.md`.

The normal disk-image rule includes `fsroot/fonts/ui.ttf` and
`fsroot/fonts/mono.ttf`. Consequently, an ISO built with the default proprietary
inputs must not be assumed redistributable merely because the Aether source is
MIT-licensed.

## GNU GRUB in generated ISO images

- Upstream: <https://www.gnu.org/software/grub/>
- License: GNU GPL version 3 or later, subject to the exact files and GRUB build
  supplied by the host environment.
- Generated location: the bootloader content installed into `build/aether.iso`
  by `grub-mkrescue`.

GRUB is not vendored in the Git source tree, but it is more than a host-only tool:
`grub-mkrescue` places GRUB boot code into the distributable ISO. Anyone publishing
an Aether ISO must identify the exact GRUB version, include the applicable license
and notices, and satisfy the GPL corresponding-source requirements for the GRUB
binary in the manner required by that distribution. Aether's root MIT License does
not relicense the GRUB files placed in the ISO.

## Rust runtime code in linked binaries

- Upstream: <https://github.com/rust-lang/rust>
- Linkage reference: <https://doc.rust-lang.org/reference/linkage.html>
- License summary: Rust is primarily distributed under MIT or Apache-2.0, with
  portions under BSD-like licenses; consult the `COPYRIGHT` and license files for
  the exact pinned toolchain.
- Generated inputs: `rust/target/**/libaether_rust.a`.

The Rust crate uses `crate-type = ["staticlib"]`. Rust documents that a static
library contains the local crate together with its upstream dependencies. Even
though this crate is `no_std` and has no Cargo package dependencies, generated
archives and final Aether binaries may contain Rust `core` and compiler-support
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
5. Use redistributable fonts by default for release artifacts.
6. Pin the GRUB and Rust toolchain versions used for binary releases and collect
   their exact license files and corresponding-source information.
7. Keep local modifications as reviewable patches or commits when any vendored
   component is updated.

Other host tools such as LLVM/Clang, NASM, QEMU, xorriso, Python, and Perl are not
stored in this repository. They normally do not become part of Aether binaries,
but release tooling must verify the actual outputs rather than relying on that
assumption. GRUB and Rust are called out above because their code can enter the
generated ISO or linked binaries.
