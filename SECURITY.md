# Security Policy

## Project status

Aether OS is an experimental operating-system project. No version currently
receives production-grade security support, and there is no security SLA. The
latest working tree receives best-effort fixes.

Do not use Aether to protect real credentials, personal data, private keys,
cryptocurrency, production workloads, or other valuable secrets. Do not treat its
browser, TLS implementation, process isolation, filesystem, or device drivers as a
security boundary for hostile workloads.

Open source permits inspection; it does not by itself establish security.

## Reporting a vulnerability

Prefer GitHub private vulnerability reporting for this repository if it is
available. Include:

- the affected commit;
- the smallest reproducer or packet/file shape that triggers the issue;
- expected and observed behavior;
- likely impact and required attacker access;
- whether the issue has already been disclosed elsewhere.

If private reporting is unavailable, open a minimal issue asking the maintainer
for a private contact. Do not publish exploit details, secrets, or a weaponized
proof of concept in the issue.

The project will try to acknowledge, reproduce, fix, test, and credit valid reports,
but cannot promise response or release deadlines.

## Security boundaries that do exist

Aether implements ring-3 user processes, per-process address spaces, syscall
entry, executable loading checks, and fault containment intended to keep an
ordinary application fault from directly crashing the kernel. Network parsers and
cryptographic primitives also have host-side and QEMU test coverage.

Those mechanisms are meaningful engineering work, but they are not a complete
security model or a certification claim.

## Important non-guarantees

- The browser has no JavaScript/origin sandbox suitable for arbitrary hostile web
  content.
- AetherScript exposes raw memory and direct system calls by design.
- The TLS 1.3, X.509, and cryptographic implementations are custom, constrained,
  and not independently audited.
- The kernel may fall back to weak timing-derived entropy when hardware random
  instructions are unavailable; it emits a warning when this happens.
- There is no claimed multi-user authorization model, verified boot chain, exploit
  mitigation baseline, or formal verification.
- Drivers and filesystem parsers run with kernel privilege; malformed devices or
  images remain high-risk inputs.
- Passing QEMU tests does not prove correctness on physical hardware.

Current audit findings, remediation notes, residual risks, and dated test results
are recorded in [docs/CODE_AUDIT.md](docs/CODE_AUDIT.md). Project provenance and
the interpretation of public claims are described in
[TRANSPARENCY.md](TRANSPARENCY.md).
