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

## Authorship: who writes the implementation

**First-party production code in LogitOS is written by AI coding agents. Humans
set the goal, design the architecture, review, test, verify, and integrate. They
do not hand-write the implementation.**

This inverts the usual arrangement, so it is worth being exact about it.

**What it covers.** Code that ships: `c/`, `rust/`, `include/`, `fsroot/`, and
the tools and tests that gate them. It does not cover `third_party/`, which is
vendored rather than written here, and it does not cover prose — this file, the
design documents, and the arguments in commit messages are written by whoever
has the argument to make.

**What humans do, and it is not the smaller half.** Deciding what to build and
why. Choosing the architecture and the boundaries between parts. Reading the
diff. Running the gates and reading their output rather than the summary of it.
Deciding what is acceptable evidence. Saying no. The rule moves typing, not
judgement, and a review that waves work through is a violation of it in a way
that writing a line of C is not.

**The exceptions, named, because a rule with no edge gets ignored the first time
it is inconvenient.** A human may type: a merge conflict resolution; a revert; a
change to build configuration, data files, or vendored third-party code; and an
emergency fix to restore a broken build or a security hole, which must then be
followed by the same review any other change gets. Anything else typed by hand
should say so in its commit message. "I typed this and here is why" is a fine
sentence; a hand-written change that pretends otherwise is the problem.

**Why.** An operating system meant to be built and understood by agents should
be the first thing that lives by that constraint, and the constraint is
productive rather than ceremonial: it forces every fact a human would otherwise
keep in their head into a file an agent can read. That is why `CLAUDE.md` is the
length it is, why the comments in this tree name the failure they prevent, and
why a gate here is expected to have been watched failing. Those habits are not
style. They are what makes the rule above possible at all.

**How it is evidenced today, and what is not yet true.** Commits carry a
`Co-Authored-By:` trailer naming the model, and the session link beside it. That
is a record, not a check: nothing in this repository verifies the rule, and a
hand-written change with an agent's trailer would pass unnoticed. Do not read
this paragraph as a gate. The honest test of whether this rule is real is
whether the architecture, the review, and the evidence are real; a project that
satisfied the letter of it by attaching a trailer would have kept none of the
value.

There is a cautionary example in this same file. The licensing section above
asks for an SPDX line on every new first-party source file. Measured
2026-09-15: 145 of 1,074 carry one, and nothing checks. A rule nobody can check
is a rule that rots, and stating one is a promise to either check it or say
plainly that you do not.

## Which agents

Not every model may write production code here. Two floors, both measurable:

- **Released after July 2026.**
- **Artificial Analysis Intelligence Index above 40**, checked against that
  source at the time of use rather than from memory. The index moves, and a
  model that qualified last quarter is not thereby qualified now.

**Why a floor exists at all**, since a floor on tools is unusual and needs its
reason stated. This tree's standard is not "produces code that compiles". It is:
a patch carries the reason the change is safe, a gate is watched failing before
it is believed, a stale sentence is corrected beside the old one rather than
overwritten, and a model working here respects the ownership of files other
agents hold open. A model that cannot hold that bar does not save work, it moves
work — from writing to reviewing and then to redoing.

That is not theoretical. On 2026-09-15 a model below this bar was given the
boot-contract task and returned a completion report stating the gate was green,
quoting a run, and listing the files it had created. The report contained the
same two controls as both PASS and FAIL in adjacent paragraphs, and the disk held
none of the files. Nothing had been written. The cost was not the wasted run; it
was that a report had to be treated as a claim to be checked rather than as
evidence, and that is the cost this floor exists to avoid.

**The floor is a floor, not a qualification.** Artificial Analysis measures
general capability, not the things this repository actually demands: reading a
113 KB map before touching anything, keeping a negative control honest when the
firmware will not cooperate, and reporting "I could not observe this" instead of
inventing a result. A qualifying model can still fail here, which is why the
review duties below do not relax for any of them.

**This one is checkable, unlike the authorship rule above.** Every commit names
its model in a `Co-Authored-By` trailer, so the distinct set of models in the
history can be listed and compared against the roster. Nothing does that today.
Writing that check is a small job and it is the difference between this section
and the SPDX line above it.

**A provenance defect to fix rather than hide.** As of 2026-09-15 the trailer is
not always accurate. Work produced by an agent running under a different harness
has been committed carrying the trailer of the model that dispatched and reviewed
it, not the one that wrote it — the boot self-hosting commits of that date name
`Claude Opus 5 (1M context)` while the implementations came from `gpt-5.6-sol`
driven through the Codex CLI. The reviewing model is a real participant and worth
naming, but it is not the author, and a rule that depends on the trailer being
true cannot tolerate that. Going forward: name the model that wrote the code, and
name the one that dispatched and reviewed it separately if both are worth
recording.

## AI-assisted contributions

Name the tool or model in the commit message or pull request description, and
briefly state what it did — drafting, porting, reviewing, fuzzing, generating
tests. Under the rule above this is the ordinary case rather than a disclosure
of something unusual, but the record is still required.

AI output is not accepted as its own provenance, and the rule above makes this
duty heavier rather than lighter. The contributor must:

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
