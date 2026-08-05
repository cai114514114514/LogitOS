# Project Transparency

This document states what LogitOS OS is, where its code came from, what has been
tested, and what the project does not claim. It is intended to make technical
claims auditable rather than to diminish the value of the work.

Snapshot date: 2026-08-05. Repository history measurements below end at Git
commit `3973dec`; uncommitted working-tree changes are not counted.

## Short version

- LogitOS is a standalone experimental x86_64 kernel, not a Linux distribution,
  a Linux fork, or a compatibility skin over a host OS.
- The project is human-directed and heavily AI-assisted.
- The first-party operating-system core is GPL-3.0-or-later; project-authored
  outer components are MIT. Vendored, adapted, and data dependencies retain
  their upstream terms and are listed in `THIRD_PARTY.md`.
- AetherScript is a distinct project language with substantial original work,
  but its compiler and VM have material clox/Crafting Interpreters lineage.
- The network, TLS, cryptographic, browser, and isolation code are educational
  project implementations. They are not certified or independently audited for
  production use.
- Passing a test demonstrates a behavior in the named environment; it does not
  establish full standards compliance, hardware support, or security.

## Human and AI authorship

Git metadata through `3973dec` contains 267 commits. All 267 name `hzm` as the
commit author; 247 commit messages contain a `Co-Authored-By` trailer naming
Claude or Anthropic. This is strong evidence that AI assistance was material to
most of the recorded development history.

These numbers have limits:

- A commit trailer does not measure who supplied an idea, reviewed a change, or
  typed each line.
- Git cannot prove that an untagged commit had no AI assistance.
- Human direction, testing, selection, editing, and acceptance are not measured
  by line attribution.
- "First-party" in this repository means project-origin code rather than vendored
  or adapted upstream code. It does not mean "written without AI."

Future material AI assistance should be disclosed in the commit message or pull
request description. A human contributor remains responsible for reviewing the
result, establishing its provenance, and verifying that it may be licensed and
distributed.

## Component classification

| Class | Examples | Meaning |
|---|---|---|
| Project implementation | Kernel, drivers, LogitFS, network stack, TLS/crypto code, window system, applications | Maintained as LogitOS code; not copied from Linux. May be AI-assisted and informed by public specifications and literature. |
| Material adaptation | AetherScript compiler and VM | Substantial LogitOS-specific development built on identifiable clox/Crafting Interpreters lineage; both facts must be stated. |
| Vendored or ported code | QuickJS, NetSurf LibCSS and dependencies, musl libm subset | Upstream code compiled or adapted for LogitOS; governed by upstream notices. |
| External data/build input | Mozilla-derived trust roots, fonts | Not program code, but still has provenance and redistribution constraints. |
| Host tool | LLVM/Clang, LLD, NASM, QEMU, xorriso, Python, Perl | Used to build, generate, or test LogitOS; normally not shipped as LogitOS code. |
| Generated binary dependency | GNU GRUB boot image; Rust `core`/compiler support | Not stored as source here, but may be embedded into a generated ISO or statically linked binary and therefore matters to binary distribution. |

The detailed inventory, notices, known missing revisions, and redistribution
warnings are in [THIRD_PARTY.md](THIRD_PARTY.md).

## Meaning of common claims

### "Own" or "project implementation"

The implementation lives in and is maintained by this project rather than being
a renamed Linux subsystem. It does not imply that the design was invented without
reference to standards, books, papers, or prior systems.

### "From scratch"

This phrase is too ambiguous for an umbrella project claim and should be avoided.
Where it remains in historical plans or commit messages, it describes the intent
at that time, not a current provenance guarantee. New documentation should use a
specific claim such as "project implementation," "vendored port," or "adapted
from clox."

### "Real HTTPS" or "browses the web"

The stack has interoperated with selected public HTTPS servers in QEMU. The claim
does not mean full TCP, HTTP, TLS, X.509, HTML, CSS, DOM, or Web Platform
conformance. The browser has no JavaScript security sandbox and should not be used
for hostile content or real credentials.

### "Self-hosted AetherScript"

This refers to the AetherScript compiler pipeline compiling its own compiler
source in the project's self-host tests. It does not mean that the operating
system, native C compiler, assembler, or linker is self-hosted.

### "Secure" or "hardened"

These words must be attached to a precise property and test or audit result. They
must not be used as a blanket claim. Ring 3 and separate address spaces provide a
meaningful isolation mechanism, but LogitOS does not claim a production multi-user
security model, secure boot chain, browser sandbox, or independently validated
cryptography.

## Tested scope and evidence

The primary execution environment is x86_64 QEMU. The build has historically been
driven from macOS/Apple Silicon with cross-targeted LLVM tools; a later remediation
snapshot was built and tested under WSL/Ubuntu. Physical hardware coverage is
limited and should be stated per device rather than inferred from the existence of
a driver.

The most recent recorded code audit and test evidence is in
[docs/CODE_AUDIT.md](docs/CODE_AUDIT.md). Its results apply only to the tree and
environment it names. In particular, an audit finding marked fixed is not proof
that no related defect remains, and an emulated test is not hardware validation.

Known limitations and boundaries include:

- The TCP/IP stack is an intentionally small subset and does not claim RFC-complete
  behavior or IPv6 support. It configures itself with DHCP (static QEMU-oriented
  fallback) and its TCP sender uses a single outstanding segment; see
  [docs/NETWORK.md](docs/NETWORK.md).
- TLS and X.509 support a constrained set of algorithms and certificate forms
  (for example: no HelloRetryRequest, no resumption or 0-RTT, no client
  authentication, and no enforcement of extended key usage). The code is
  custom and has not received an independent cryptographic audit.
- The kernel DRBG is a SHA-256 Hash_DRBG-style construction with state/output
  separation (an output block is never the internal state) and periodic
  reseeding from RDSEED/RDRAND (every 1024 generate calls or 1 MiB of
  output). If RDSEED/RDRAND is unavailable, seeding falls back to
  timing-based weak entropy; the TLS client refuses to handshake in that
  mode rather than keying sessions from a predictable seed.
- The AES-128 implementation is byte-oriented with a secret-indexed S-box
  and is not constant-time (a cache-timing side channel). This is an
  accepted limitation for a QEMU/TCG demo TLS client, not a claim of
  side-channel resistance.
- Cryptographic correctness claims are backed by host-side evidence, not
  external review: `make test-crypto` runs 169 known-answer and negative
  vectors plus the ecdsa/rsa/rng batteries, and `make test-crypto-diff`
  replays 128,714 randomized differential cases against a self-checked
  pure-Python reference (all byte-identical at the time of writing).
- The browser implements a small portion of modern web standards and has no origin
  or JavaScript sandbox suitable for adversarial pages.
- AetherScript deliberately exposes raw-memory and direct-syscall operations; it
  is a systems language facility, not an untrusted-code sandbox.
- The project has ring separation and per-process address spaces, but not a
  production multi-user authorization or permissions model.
- LogitFS does not make a documented journaling or crash-consistency guarantee.
- Hardware support is narrow and primarily exercised through QEMU devices.
- Runtime fonts are renamed subsets of OFL-licensed Noto fonts, not original
  LogitOS typeface designs; their source and license records are vendored.
- The current local wallpaper is derived from a macOS Desktop Picture and is not
  cleared for redistribution; it must be replaced before a public binary release.

See [SECURITY.md](SECURITY.md) for the operational security warning and reporting
process.

## Rules for future claims and releases

A public feature claim should name at least one of the following:

1. A test command and the environment in which it passed.
2. A source location that implements the claimed mechanism.
3. A standards scope stating what subset is implemented.
4. A dated demonstration that can be reproduced.

Before calling a release reproducible or supply-chain complete, the project still
needs:

- exact upstream revisions and archive hashes for every vendored component;
- a machine-readable SPDX or CycloneDX SBOM generated from those pinned inputs;
- pinned compiler, linker, generator, and boot-tool versions;
- a reproducible CA-bundle snapshot;
- the exact GRUB source corresponding to the bootloader placed in each ISO and
  the exact Rust notices corresponding to statically linked runtime code;
- a clean-build procedure that does not depend on undeclared host files;
- CI that runs the connected unit, fuzz, self-host, and QEMU tests;
- release checksums and, preferably, signed tags or attestations;
- an explicit list of known failures alongside every release.

Until those items exist, LogitOS should be described as open-source and buildable in
documented development environments, but not as bit-for-bit reproducible.
See [RELEASING.md](RELEASING.md) for the concrete source and binary release gates.
