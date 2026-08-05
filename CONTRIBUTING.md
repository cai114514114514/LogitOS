# Contributing

LogitOS welcomes fixes, tests, documentation, ports, and new experiments. The main
requirement is that every contribution be technically and legally traceable.

## Licensing

By submitting an original contribution, you agree that it may be distributed
under the license assigned to its destination in [LICENSING.md](LICENSING.md),
and you confirm that you have the right to submit it under those terms:

- LogitOS Core paths use `GPL-3.0-or-later`.
- Project-authored outer paths use `MIT`.
- A contribution spanning both boundaries is licensed per file.

New project-authored source files should carry an SPDX copyright line and the
applicable `SPDX-License-Identifier`. Do not change a third-party license merely
because the file is stored beneath a first-party directory.

Do not copy code, generated tables, certificates, fonts, images, or other data into
the repository without recording its source and redistribution terms.

## Provenance categories

Every substantial addition should be identifiable as one of:

- project-origin implementation;
- material adaptation from a named source;
- vendored upstream code;
- generated output;
- external data or asset;
- host-only build or test dependency.

For anything other than project-origin implementation, update
[THIRD_PARTY.md](THIRD_PARTY.md) and retain the required upstream notices beside
the files when practical.

## Third-party intake checklist

Before adding or updating an external component, record:

1. Project name and canonical upstream URL.
2. Exact version, tag, or commit hash.
3. Download URL and SHA-256 of the imported archive or input.
4. License identifier and complete required notices.
5. Imported paths and omitted portions.
6. Local patches, build changes, and generated files.
7. Whether the component is shipped at runtime or used only on the host.
8. A test that exercises the integration.

Also check generated outputs. A host tool becomes a distribution concern when its
code or data is copied into an artifact; GRUB code in the ISO and Rust runtime code
in a `staticlib` are current examples.

Do not describe a component as GPL or MIT merely because an LogitOS adapter around
it uses that license. Each upstream license continues to govern its own material.

## AI-assisted contributions

Material use of a coding model or agent should be disclosed in the commit message
or pull request description. Name the tool or model when known and briefly state
what it did, such as drafting, porting, reviewing, fuzzing, or generating tests.

AI output is not accepted as its own provenance. The contributor must:

- inspect the output for copied or recognizably adapted material;
- verify compatible licensing and preserve notices;
- review the code rather than relying on the model's explanation;
- run proportionate tests and report the actual results;
- avoid claiming that an unverified feature, fix, or security property works.

## Claims and documentation

Use precise language:

- Prefer "project implementation" over the ambiguous phrase "from scratch."
- Say "adapted from" when implementation lineage is material.
- Say which subset of a protocol, standard, platform, or device is supported.
- Attach security claims to a specific property, test, or audit result.
- Record failed and skipped tests alongside passing ones.

Update [TRANSPARENCY.md](TRANSPARENCY.md) when a change affects authorship,
provenance, supported environments, or a public capability claim.

## Testing

Run the smallest relevant unit test while developing, then the applicable host and
QEMU integration tests listed in the README. In a contribution description, include
the exact commands, host environment, and any skipped or failing test. A test that
was not run must be reported as not run, not assumed to pass.

Security-sensitive parsers should include malformed-input and boundary tests. A new
test source should also be connected to a documented Make target or CI job; an
unreachable test file does not provide regression coverage.

Changes intended for a release must also satisfy [RELEASING.md](RELEASING.md).

## Security fixes

Do not open a public change containing weaponized exploit details before following
the private reporting process in [SECURITY.md](SECURITY.md). Once coordinated,
include a regression test and describe the affected versions and security boundary
precisely.
