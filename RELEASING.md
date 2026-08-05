# Release Checklist

A source repository and a bootable ISO have different licensing and provenance
surfaces. Do not publish an Aether binary merely because the root project license
is open source. The core, outer components, third-party files, and aggregate ISO
do not share one license.

This checklist is a project policy, not legal advice.

## 1. Define the release

- Start from a named commit with a clean working tree.
- Record the release version, commit, build host, and build date.
- Pin the versions of Clang/LLVM, LLD, NASM, Rust, GRUB, Python, xorriso, QEMU,
  and any generators used.
- Record the exact build and test commands.

## 2. Close source provenance

- Record exact upstream commits/releases and source archive SHA-256 values for
  QuickJS, musl libm, LibCSS, libparserutils, libwapcaplet, and the clox adaptation
  base.
- Regenerate the CA store from a named Mozilla/curl snapshot and record its URL,
  date, certificate count, and SHA-256.
- Preserve every required copyright and permission notice.
- Generate an SPDX or CycloneDX SBOM from the pinned inventory.

The known unresolved items are maintained in [THIRD_PARTY.md](THIRD_PARTY.md).

## 3. Use redistributable assets

- Verify the vendored Noto source and generated-subset hashes against
  `third_party/fonts/README.md` and `fsroot/fonts/README.md`.
- Include both Noto OFL files and copyright/metadata records in the release
  notices. The font sources and subsets remain OFL-licensed, not MIT-licensed.
- If fonts are changed, give modified subsets distinct internal names and update
  their exact source commit, hashes, licenses, and copyright notices.
- Replace `fsroot/wallpaper.png`; the current local generator derives it from a
  proprietary macOS Desktop Picture. Record the replacement's source and license.
- Inspect the disk image for undeclared fonts, keys, certificates, images, and
  other generated host inputs.

## 4. Satisfy binary-license obligations

The release bundle should include at least:

- Aether's `LICENSE`, `LICENSING.md`, and the complete GPL-3.0-or-later and MIT
  texts from `LICENSES/`;
- `THIRD_PARTY.md` and all vendored license files;
- the QuickJS, musl, NetSurf, and Robert Nystrom notices;
- MPL 2.0 information for the Mozilla-derived trust data;
- the exact Rust toolchain's MIT, Apache-2.0, copyright, and applicable exception
  notices for code entering the static libraries;
- GNU GRUB's applicable GPL license and notices.

For `kernel.elf`, publish the complete corresponding Aether Core source and the
scripts and build definitions needed to produce the binary, as required by
GPL-3.0-or-later. Keep the MIT application and ABI notices intact; their presence
in the same image does not make the whole image a single-license work.

Because `grub-mkrescue` embeds GRUB in the ISO, distribute the corresponding GRUB
source, or make it available in another GPL-compliant manner appropriate to the
distribution. Record the exact GRUB package/version rather than pointing only to
whatever version happens to be current upstream.

## 5. Verify the artifact

- Build from a clean checkout using only the declared inputs.
- Run every connected host test, self-host test, and relevant QEMU integration
  test. Record passes, failures, and skips without collapsing them into a single
  "green" claim.
- Boot the exact ISO that will be published.
- Inventory the final ISO and disk image, not just the source tree.
- Scan the binary strings and archive members for unexpected absolute paths,
  credentials, host usernames, and undeclared objects.
- Rebuild in a second clean environment and compare outputs. If they differ, do
  not call the release reproducible; document the differences.

## 6. Publish evidence

- Publish SHA-256 checksums for the source archive, ISO, disk image, and SBOM.
- Publish the known limitations and unresolved audit findings that apply to the
  release.
- Link the dated test log and build manifest.
- Prefer a signed tag and signed checksums or a provenance attestation.

The release notes must retain the experimental/non-production warning from
[SECURITY.md](SECURITY.md) and the claim definitions from
[TRANSPARENCY.md](TRANSPARENCY.md).
