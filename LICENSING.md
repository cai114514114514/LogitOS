# Licensing boundary

LogitOS intentionally separates its copyleft operating-system core from its
permissively licensed outer development and application surface. This document
is the authoritative first-party path mapping. Third-party material always
keeps its upstream terms as recorded in [THIRD_PARTY.md](THIRD_PARTY.md).

## LogitOS Core — GPL-3.0-or-later

Project-authored files in these paths are licensed under
`GPL-3.0-or-later`:

- `c/boot/**`
- `c/kernel/**`
- `c/drivers/**`
- `c/fs/**`
- `c/net/**`
- `c/crypto/**`
- `c/lib/**`, except the shared `c/lib/image/**` sources listed below
- `linker.ld`

This boundary covers the kernel, boot entry, hardware drivers, filesystem,
TCP/IP and other network protocols, LogitTLS, project cryptography, and support
code linked into `kernel.elf`. A distributed `kernel.elf` is therefore a
GPL-3.0-or-later covered work as a whole, while separately identifiable
third-party or permissively licensed source files retain their own notices.

The generated `c/crypto/trust/roots_bundle.inc` is an explicit exception: its
Mozilla/NSS-derived trust data remains under MPL-2.0. Generated files do not
acquire GPL merely because their output path is inside a core directory.

## Outer components — MIT

Project-authored files in these paths are licensed under `MIT`:

- `c/apps/**`
- `c/lib/image/**`
- `include/**`
- `rust/**`, excluding generated `rust/target/**` artifacts
- `tools/**`
- `tests/**`
- `docs/**`
- `fsroot/**`, except externally sourced fonts, images, trust data, or other
  material identified in `THIRD_PARTY.md`
- root build and project files, including `Makefile`, `grub.cfg`, and project
  Markdown documents

Keeping `include/abi/**` under MIT is deliberate: applications may use the
public syscall and executable-format ABI without adopting the kernel license.
Ring-3 applications are separate programs rather than part of `kernel.elf`.

The C image parsers and Rust crate are deliberately MIT because the same code is
linked into both the GPL kernel and independently distributed userland. When
linked into `kernel.elf`, the combined kernel remains GPL-covered; the shared
source does not cease to be MIT. A userland application that links only these
MIT shared sources does not acquire the kernel's GPL license from them.

## Third-party and adapted material

The path rules above apply only to copyright held by LogitOS OS contributors.
They do not overwrite notices on third-party or materially adapted files.
Important examples include:

- `third_party/**`: upstream licenses recorded beside the components and in
  `THIRD_PARTY.md`;
- Mozilla/NSS-derived root data: MPL-2.0;
- Noto fonts and generated subsets: OFL-1.1;
- DejaVu Sans (`third_party/fonts/DejaVuSans.ttf`, shipped unmodified as
  `/fonts/text.ttf`): Bitstream Vera Fonts License and the Arev fonts
  copyright;
- AetherScript's clox-derived material: upstream MIT terms remain preserved;
- GRUB code embedded in generated ISO images: GPLv3-or-later under GRUB's own
  copyright and release obligations;
- Rust compiler/runtime objects entering binaries: their upstream terms and
  exceptions.

Local changes to a third-party file remain subject to the upstream license when
that license requires it. A project-authored adapter in a core path uses
GPL-3.0-or-later; an adapter in an outer path uses MIT, provided the upstream
terms permit that treatment.

## Contributions

By contributing original work, a contributor agrees to the license assigned to
the destination path above. A change spanning both boundaries is licensed per
file. New core source files should carry:

```text
SPDX-FileCopyrightText: YEAR NAME
SPDX-License-Identifier: GPL-3.0-or-later
```

New outer source files should use `SPDX-License-Identifier: MIT`. Use the
comment syntax appropriate to the file. Do not add a first-party SPDX tag to
third-party material unless its copyright and license have been verified.

## Distribution language

Use these descriptions:

- **Source repository:** multi-license; LogitOS Core is GPL-3.0-or-later and
  project-authored outer components are MIT.
- **Kernel binary:** GPL-3.0-or-later as a combined work, with compatible
  third-party notices retained.
- **Application binaries:** retain their applicable outer/third-party terms;
  an application that incorporates GPL-covered core source must instead comply
  with GPL-3.0-or-later for that combined binary.
- **Bootable ISO:** a multi-license aggregate containing the GPL-covered LogitOS
  kernel, separate GPL-covered GRUB bootloader code, MIT applications, and
  additional third-party data and software.

Never describe the repository, kernel, or ISO as “entirely MIT.” Conversely,
do not claim that independent MIT applications or every file in the ISO is GPL.

## Transition from the former MIT default

The former repository-wide MIT grant remains valid for copies and versions that
were distributed under it. It cannot be retroactively withdrawn. Copyright
holders may offer their work under different terms in later versions; this
repository does so for the core from the version containing this policy onward.
Future core modifications accepted under GPL-3.0-or-later are not thereby
available under the historical MIT grant.
