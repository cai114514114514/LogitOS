# Contribution workflow

A LogitOS contribution brings implementation, design rationale, provenance, and verification evidence to review. `CONTRIBUTING.md` defines contribution and authorship policy; `AGENTS.md` sets the working rules for the shared repository. Changes intended for publication also follow `RELEASING.md`.

## Branches and shared ownership

Keep a contribution branch focused on one change, including its tests and documentation. Identify the submitted commit so reviewers can relate the diff, artifacts, and results to the same revision. Agree on the destination branch and integration method during review.

Before editing, read `CLAUDE.md` for the subsystem map and `AGENTS.md` for working rules. Check `git status` and file modification times for concurrent activity. If another contributor is working in a file you need to change, put the proposed patch in your report, following the ownership rule in `AGENTS.md`.

For a source file whose documentation you have yet to read, use `docs_for_source` on the `logitos-docs` MCP server configured in `.mcp.json`, then read the primary pages with `read_doc`. `tools/docs/mcp_server.py` implements these tools. The fallback index is `docs/README.md`. Task recipes live in `.agents/skills/`, reached through the `.claude/skills` symlink by Claude Code.

Give local builds their own output directory through a command-line assignment to `BUILD`. The `Makefile` sets `BUILD := build`; `AGENTS.md` preserves the correction explaining that command-line assignments override this value while environment assignments lose to it. Match artifact target paths to the selected directory.

## Commits and design records

Stage and commit named paths belonging to the contribution, as required by `AGENTS.md`. Include the headers, generators, build definitions, and fixtures needed to reconstruct the change. `tools/verify-commit.sh` records the failure behind this rule: commits omitted headers that remained available as untracked files in the contributor’s working tree.

Explain the problem, resulting behavior, and verification evidence in the commit message or review description. Record AI assistance and human implementation according to `CONTRIBUTING.md`.

Keep enduring design reasons beside the code. Under `AGENTS.md`, comments should explain choices, measurements and their methods, subtle failure modes, and reasons for deferred behavior. When correcting an established claim, retain a brief account of the previous claim beside the correction so readers can reconcile older descriptions with the current implementation.

## AI authorship

`CONTRIBUTING.md` assigns production implementation in the project’s own source to AI coding agents. Humans set goals, choose architecture, review, test, verify, and integrate. The policy covers shipped code and the tools and tests that gate it. Contributors of either kind may write prose.

The policy permits human implementation for merge conflict resolutions, reverts, build configuration, data files, vendored code, and emergency repairs to a broken build or security hole. Emergency repairs receive subsequent review. Other implementation typed by a human should be identified and explained in its commit message.

For agents writing production code, the policy requires both:

- A model released after July 2026.
- An Artificial Analysis Intelligence Index score above 40, checked against that source at the time of use.

Record the model that produced the implementation in a `Co-Authored-By:` trailer and include the session link. Attribute dispatch and review to their participants when relevant. `CONTRIBUTING.md` and `TRANSPARENCY.md` preserve an earlier attribution error in which trailers named the model that dispatched and reviewed the work as its implementation author.

Name the tool or model in the commit message or pull request description and describe its role, such as drafting, porting, review, fuzzing, or test generation. The contributor remains responsible for inspecting the output, checking implementation lineage, preserving notices, verifying compatible licensing, and reporting test results.

## Review and verification

Give reviewers the behavior before and after the change, its design rationale, affected interfaces, and supporting evidence. `CONTRIBUTING.md` requires the commands as run, the host environment, and the results, including failures, skips, and tests left unrun. Security-sensitive parsers should include malformed input and boundary cases.

During development, run the smallest relevant test, then the applicable host and QEMU integration tests. `AGENTS.md` requires guest evidence for claims about OS or browser behavior and timing taken inside the guest for performance claims. Observe the behavior directly, such as displayed frames or captured audio.

Check that the harness exercises the changed implementation. Use a control that introduces the targeted defect, observe the expected failure, restore the implementation, and record the diagnostic. Wire the negative control as a prerequisite of its positive test. `tests/license.mk` demonstrates this structure: it damages copies in a shadow tree and checks both the failure status and the diagnostic naming the damaged property.

Connect new test Make fragments through literal include paths. `tools/mk_wired.py` follows these includes transitively, including fragments in nested directories.

Run these commands from the repository root with the required build tools installed:

| Command | Role in review |
|---|---|
| `make` | Build the default ISO target in `Makefile`. |
| `make build/disk.img` | Build and pack applications through the development disk image dependencies. |
| `make test-mk-wired` | Audit test Make fragment reachability. |
| `make test-license-audit` | Run the licensing audit through its prerequisite negative controls. |
| `make verify-commit` | Build and test a committed revision through `tools/verify-commit.sh`. |
| `python3 tools/docs/stale.py` | Report pages whose recognized source references changed since their recorded verification commit. |

The artifact paths in this table use the default build directory. For application changes, include the disk image target in verification: `all` depends on the ISO, while the disk image dependencies build the applications.

By default, `make verify-commit` clones `HEAD` into a temporary directory, checks that the clone is clean, builds the ISO and development disk image, and invokes `make test-fullsystem`. `tests/fullsystem.mk` forwards `REV` to select another revision and `VERIFY_ARGS` for script options. Testing a clone exposes dependencies supplied by untracked files or other working tree changes.

After changing code, inspect the documentation staleness report and check affected pages against the source. Correct them, then record verification through `tools/docs/stale.py`. For this page, the command is `python3 tools/docs/stale.py --mark docs/contributing/workflow.md`; use the corresponding path for each page reviewed.

For security fixes, follow the private reporting process in `SECURITY.md` before publishing exploit details. After coordination, include a regression test and identify the affected revisions and security boundary.

## Licensing and provenance

Use `LICENSING.md` to determine the license for each destination file. Project source in the core paths uses GPL-3.0-or-later. Listed outer paths, including project application sources, public headers, tools, tests, and documentation, use MIT. Shared sources and externally derived data have documented exceptions. Apply the mapping per file and account for the source incorporated into distributed binaries.

New project source files should carry the SPDX copyright and license identifiers required by `CONTRIBUTING.md`. Preserve upstream terms on imported or adapted material.

Classify substantial additions as project implementation, adaptation, vendored code, generated output, external data, or a host dependency. For additions outside project implementation, update `THIRD_PARTY.md` with the upstream URL, pinned revision, input hash, license and notices, imported and omitted paths, local changes, runtime or host role, and integration test.

Update `TRANSPARENCY.md` when authorship, provenance, supported environments, or public capability claims change. Support public claims with an implementation location, a named test and environment, a standards scope, or a dated demonstration that can be reproduced.

## Release preparation

`RELEASING.md` defines the release checklist:

1. Select a named commit with a clean working tree. Record the version, revision, host, date, tool versions, and build and test commands.
2. Pin external sources and generated inputs, including trust data. Record input hashes, preserve notices, and generate an SPDX or CycloneDX software bill of materials.
3. Inspect packaged assets and data for provenance and redistribution terms.
4. Prepare license texts, upstream notices, and corresponding source and build definitions for distributed binaries covered by the GPL.
5. Build from declared inputs, run the connected host and self-host tests and relevant QEMU tests, and boot the ISO intended for publication. Inventory the final images and inspect them for credentials, host paths, and undeclared objects.
6. Compare outputs from a second clean environment. Publish checksums, the build manifest, dated logs, known limitations, and unresolved findings. Prefer signed tags and checksums or a provenance attestation.

Two descriptions in the checklist need to be read alongside the current recipes. Its GRUB distribution instructions describe an ISO containing GRUB. The default ISO recipe in `Makefile` now uses `tools/mkiso.py` and the project BIOS loaders; `make iso-grub` builds the GRUB variant. Apply bootloader distribution obligations to the artifact being published.

The checklist also warns about the local wallpaper derived from a macOS Desktop Picture. The current release recipe uses the procedural image generated by `tools/mkwallpaper.py`. `make release-disk` produces the release disk image from the shared pack list, applies the exclusions in `Makefile`, and adds the generated wallpaper. Include that image in the release inventory and inspection.

Include AMD Polaris microcode in the inventory. `Makefile` packs the blobs from `third_party/amd-firmware/amdgpu` into both development and release disk images, together with `third_party/amd-firmware/LICENSE.amdgpu` and `third_party/amd-firmware/MANIFEST`. The manifest records the upstream revision and blob hashes; `tools/gpu/amd/fwpin.py` derives the driver’s digest pins from it. `tools/mkesp.py` also accepts `--firmware-dir` to package firmware on an EFI System Partition. For every distribution containing these blobs, include the copyright, permission notice, disclaimers, and notices required by `third_party/amd-firmware/LICENSE.amdgpu`.

Release notes retain the experimental status and operational security warnings from `SECURITY.md` and follow the claim definitions in `TRANSPARENCY.md`.

## Limitations

- `CONTRIBUTING.md` describes authorship trailers and session links as records without automated verification of authorship or model eligibility. Review must assess those records.
- Commit verification options change the evidence: `--quick` skips the full-system test, and `--in-place` uses the working tree. Report options passed through `VERIFY_ARGS`.
- `tools/license_audit.py` uses a fixed license alias table. Its disk parser expects the disk argument immediately after the packer command, while the current recipe inserts `MKFS_PROFILE` and supplies packing entries through variables. Its image reader also uses the default build directory. These constraints require source and artifact inspection when assessing licensing coverage.
- Documentation staleness checks compare recognized source references between committed revisions. Uncommitted edits require direct review. Pages without a verification record are listed as unverified; the script’s failure status reflects pages with changed sources.
- Test results establish behavior for the revision, environment, and cases exercised. Broader claims require evidence under `TRANSPARENCY.md`.

## See also

- [Making your first change](../getting-started/first-change.md)
- [Building LogitOS](../getting-started/building.md)
- [Testing](../architecture/testing.md)
- [Writing tests](writing-tests.md)
- [Documentation style](documentation-style.md)

## Source map

| File | What it contains |
|---|---|
| `CONTRIBUTING.md` | Contribution policy, AI authorship, disclosure, provenance, and review duties. |
| `AGENTS.md` | Shared workspace rules, comment standards, build isolation, and evidence requirements. |
| `CLAUDE.md` | Subsystem map and dated corrections to development assumptions. |
| `.mcp.json` | Documentation MCP server configuration. |
| `.agents/skills/logitos-docs/SKILL.md` | Documentation lookup and maintenance workflow. |
| `tools/docs/mcp_server.py` | Documentation search, source lookup, page reading, and staleness tools. |
| `tools/docs/docs_index.py` | Source reference extraction and comparison with verification commits. |
| `tools/docs/stale.py` | Staleness reporting and verification recording. |
| `TRANSPARENCY.md` | Authorship evidence and rules for public capability claims. |
| `LICENSING.md` | License assignments by path and distribution requirements. |
| `THIRD_PARTY.md` | External component inventory and provenance records. |
| `SECURITY.md` | Experimental status, operational warnings, and vulnerability reporting. |
| `RELEASING.md` | Source and binary release checklist. |
| `Makefile` | Build defaults, disk packing, release exclusions, and artifact recipes. |
| `tests/fullsystem.mk` | Full-system test and commit verification targets. |
| `tools/verify-commit.sh` | Verification through a clean clone and options that change its scope. |
| `tools/mk_wired.py` | Test Make fragment reachability audit. |
| `tests/license.mk` | Licensing audit target and prerequisite negative controls. |
| `tools/license_audit.py` | License, provenance, fixture, and packaged notice checks. |
| `tools/mkiso.py` | BIOS ISO writer used by the default recipe. |
| `tools/mkwallpaper.py` | Procedural release wallpaper generator. |
| `third_party/amd-firmware/MANIFEST` | AMD firmware upstream revision and blob hashes. |
| `third_party/amd-firmware/LICENSE.amdgpu` | Firmware redistribution terms and required notices. |
| `tools/gpu/amd/fwpin.py` | Firmware digest pin generation from the manifest. |
| `tools/mkesp.py` | EFI System Partition image construction and firmware packaging. |
