# Documentation style

LogitOS documentation helps a systems programmer find the implementation, understand its design, and assess the evidence for its behaviour. Write from the current source tree and explain the reasons behind decisions. `AGENTS.md` sets the engineering evidence standard; `TRANSPARENCY.md` and `CONTRIBUTING.md` define the rules for capability and provenance claims.

## How the documentation is organised

Choose a page based on the reader’s task and the depth of explanation needed.

| Area | Purpose | Starting points |
|---|---|---|
| Getting started | Build, boot, locate code, and make an initial contribution. | [Building LogitOS](../getting-started/building.md), [Repository map](../getting-started/repository-map.md) |
| Architecture | Explain subsystem responsibilities, interactions, and design choices. | [Architecture overview](../architecture/overview.md) |
| Kernel and drivers | Describe mechanisms, interfaces, initialisation, and hardware integration. | [Kernel initialisation](../kernel/init.md), [Driver overview](../drivers/overview.md) |
| Filesystems | Explain filesystem interfaces, storage structures, caching, and tools. | [Virtual file system](../filesystems/vfs.md) |
| Networking and cryptography | Describe protocol processing, cryptographic mechanisms, and trust policy. | [Sockets](../networking/sockets.md), [Cryptography overview](../crypto/overview.md) |
| Userland and libraries | Explain application facilities, services, and reusable components. | [The C library](../userland/libc.md), [Libraries](../libraries/overview.md) |
| Reference | Provide lookup tables for build targets and interfaces. | [Make targets](../reference/make-targets.md) |
| Contributing | Explain contribution practices and documentation maintenance. | [Contribution workflow](workflow.md) |

The [documentation index](../README.md) provides reading paths and page summaries. Task recipes live in `.agents/skills/`; `.agents/skills/logitos-docs/SKILL.md` describes source lookup and documentation maintenance.

An architecture page describes responsibilities and relationships; a subsystem page explains the mechanisms that implement them. Give each detailed explanation a clear home and link to it from related pages.

Keep graphics SDK and AetherScript coverage at overview level in this batch. Preserve dates and evidence context when referring to investigations in `docs/engine-plan/`, `docs/bugs/`, and `docs/reports/`, and verify their implementation claims against the current code.

## Page structure

Begin with one `#` heading containing the page title, followed by a paragraph stating the subject’s purpose and place in the system. Use short sections with descriptive `##` headings.

Organise the body around a developer’s work: where execution begins, which component owns state, how requests move through the system, and why the implementation has its current form. Explain ordering constraints, resource lifetimes, synchronisation, and failure handling where they affect callers.

Use tables for parallel information such as interfaces, responsibilities, or source locations. Use numbered lists for ordered procedures. Keep code snippets short and copy them from the tree, or derive them through a small transformation readers can follow. For commands, state the working directory, required inputs, and expected result.

Use at most one or two Mermaid diagrams per page. Explain the control flow, ownership, or subsystem relationships shown by each diagram.

End the technical body with a `## Source map` table containing repository-relative file paths and their roles. Follow it with `## See also` when related pages provide a useful next step.

## Language and presentation

Write clear technical English with concrete subjects and active verbs. Name the component that performs an action. Expand unfamiliar abbreviations on first use and use terminology consistently.

Explain the reasoning behind mechanisms. `AGENTS.md` asks comments to preserve information readers cannot recover from the implementation alone: the alternative considered, the measurement behind a choice, or the failure a design prevents. Carry that reasoning into the documentation.

Describe behaviour with positive statements. Collect supported restrictions once in a short list under `## Limitations`, with each restriction’s practical consequence and supporting source.

Use these presentation rules:

- Put paths, symbols, constants, targets, and command text in backticks.
- Use bold text for a term at the point it is defined.
- Keep each paragraph focused on one idea.
- Prefer plain phrases over invented compound labels.
- Use comparisons when the choice between alternatives explains the design.
- Omit marketing language, emojis, em dashes, rhetorical questions, and concluding filler.

## Ground explanations in the code

Read `CLAUDE.md` for the subsystem map and dated engineering context, then locate the current implementation. Follow declarations, callers, initialisation, and build selection until the claimed behaviour has a complete path through the code. Check comments against the implementation they describe.

Before changing a source file whose documentation you have not read, use `docs_for_source` on the `logitos-docs` Model Context Protocol (MCP) server with its repository-relative path, then read the primary pages with `read_doc`. Use `search_docs` to find pages by symbol or concept. `.mcp.json` configures the server implemented in `tools/docs/mcp_server.py`. Use `docs/README.md` as the index when MCP is unavailable.

For an interface, establish who supplies its inputs, who validates them, who owns allocated resources, and how results reach the caller. Follow consumers as well as declarations.

For example, `include/abi/logit_exec.h` defines `LOGIT_ARG_MAX` and `LOGIT_ARG_BYTES`. `c/apps/coreutils/sh.c` derives argument-storage sizes from them. In `c/kernel/exec/load/exec.c`, `copy_uvec()` checks vector length and string storage, and its execution caller shares the string budget across arguments and environment entries. Reading these together establishes the contract. The header also records the earlier truncation defect that motivated one authoritative definition.

Check build membership when describing where code executes or how an artifact is rebuilt. In `Makefile`, the default `all` target builds the ISO, while the disk-image rule includes application packages among its prerequisites. Follow those dependencies when documenting application changes.

When interpreting Make rules, join backslash continuations and follow included fragments transitively. `tools/mk_wired.py` demonstrates continuation handling and traversal of literal test-fragment includes. Its comments record how incomplete traversal previously misclassified reachable fragments.

## Cite sources and link pages

Cite source files inline where they support an explanation. Use repository-relative paths, such as `include/abi/logit_exec.h`. Name a function or declaration when it helps readers locate the relevant passage.

Keep the final source map focused on files supporting the page’s explanations. Describe each file’s contribution: an interface definition, implementation, integration point, build rule, or test assertion.

`tools/docs/docs_index.py` extracts source references from backticked tokens accepted by its `PATH_RE` pattern. Include complete implementation paths in prose or the source map so source lookup and freshness reporting can associate the page with its code.

Use relative Markdown links between pages. Check each destination and heading fragment. Link to pages in this documentation batch or files present in the tree.

Distinguish repository paths, guest installation paths, and generated artifact paths in the surrounding prose. `docs/SOURCE_LAYOUT.md` records source relocations alongside executable names and guest paths; use it to locate code behind older references.

## Record evidence and scope

`TRANSPARENCY.md` asks public feature claims to identify at least one of four things: an implementation location, a test command and environment, a supported standards scope, or a dated demonstration readers can reproduce. Match the wording to the evidence available.

| Evidence | What to record |
|---|---|
| Source inspection | Revision or inspection date, implementation paths, and mechanism traced. |
| Host test | Command, host environment, tested sources, result, and relevant adapters or stubs. |
| Guest test | Command, image revision, emulator configuration, exercised behaviour, and observed assertions. |
| Physical hardware test | Device and platform details, configuration, procedure, and observed result. |
| Performance measurement | Workload, units, clock source, configuration, repetitions, and variation. |

Distinguish inspection from execution through the verbs you use: a header “defines,” a function “checks,” and an observed run “passed.” Record failed and skipped tests alongside successful results. Report unexecuted tests as “not run,” as required by `CONTRIBUTING.md`.

Read the harness and its assertions before interpreting a result. Trace its linked sources and inputs, and identify the observation that settles the claim. `AGENTS.md` requires watching controls detect the intended defect and measuring OS behaviour in the guest. It also requires performance timing taken inside the guest.

Use the provenance categories in `CONTRIBUTING.md`: project-origin implementation, material adaptation from a named source, vendored upstream code, generated output, external data or assets, and dependencies used for host builds or tests. Consult `THIRD_PARTY.md` for recorded origins and notices. Attach security claims to a named property and supporting evidence.

## Keep pages accurate as the tree changes

Update documentation alongside changes to interfaces, source locations, defaults, initialisation order, and artifact construction. Search related pages, examples, and source maps for affected symbols and paths.

From the repository root, use the existing script directly:

```sh
python3 tools/docs/stale.py
```

The script compares each page’s indexed source references with committed changes between its recorded verification revision and `HEAD`. It reports suspect pages and pages without a verification revision. Suspect pages produce exit status 1. Read and update the affected pages against the implementation.

After checking a page against a committed source revision, record the check from the repository root:

```sh
python3 tools/docs/stale.py --mark docs/contributing/documentation-style.md
```

Substitute the path of the page reviewed. The script checks that the path belongs to an indexed documentation page, then creates or updates the verification ledger configured by `VERIFIED` in `tools/docs/docs_index.py`, recording the current abbreviated `HEAD` revision.

When correcting a claim the project previously relied on, retain a brief dated statement of the earlier claim beside its correction. `AGENTS.md` requires this practice, and `CLAUDE.md` provides examples. Put the current behaviour where readers can find it quickly.

`CONTRIBUTING.md` requires updating `TRANSPARENCY.md` when a change affects authorship, provenance, supported environments, or a public capability claim. Name any AI tool or model used in the contribution description and state its role.

Before submitting a documentation change:

1. Check every named path, symbol, constant, target, and command against the tree.
2. Follow the implementation and its callers for each behavioural claim.
3. Check relative links, heading fragments, examples, and source-map entries.
4. Attach dates and environments to recorded results.
5. Review related pages for conflicting descriptions and repeated explanations.
6. Record the verification revision for pages checked against committed code.
7. Report the checks performed and remaining gaps.

## Limitations

- Freshness reporting covers indexed source references. Review related callers, dependencies, and uncited files when assessing a change; reference extraction is defined in `tools/docs/docs_index.py`.
- Freshness comparisons use committed revisions. Review working tree edits alongside the report so the recorded baseline matches the source checked.
- Pages that lack a recorded verification revision appear as unverified and stay outside the suspect entries. Exit status 0 can accompany unverified pages, as shown in `tools/docs/stale.py`.
- The Git helper in `tools/docs/docs_index.py` returns empty output when Git fails. Confirm that the recorded revision and comparison revision resolve before relying on an empty change report.

## Source map

| File | What it contains |
|---|---|
| `AGENTS.md` | Rules for design reasoning, corrections, measurements, and test evidence. |
| `CLAUDE.md` | Subsystem map, dated findings, and corrections to earlier claims. |
| `TRANSPARENCY.md` | Capability claims, provenance terminology, and evidence scope. |
| `CONTRIBUTING.md` | Contribution policy, provenance categories, and reporting requirements. |
| `THIRD_PARTY.md` | Dependency origins, adaptations, external inputs, and licensing records. |
| `docs/README.md` | Documentation index and reading paths. |
| `docs/SOURCE_LAYOUT.md` | Source organisation and mappings from former paths. |
| `.mcp.json` | Documentation MCP server configuration. |
| `.agents/skills/logitos-docs/SKILL.md` | Source lookup and documentation maintenance recipe. |
| `tools/docs/mcp_server.py` | Documentation search, reading, source lookup, and freshness tools. |
| `tools/docs/docs_index.py` | Page discovery, reference extraction, and revision comparisons. |
| `tools/docs/stale.py` | Freshness reporting and verification-ledger updates. |
| `include/abi/logit_exec.h` | Shared argument limits and their design rationale. |
| `c/apps/coreutils/sh.c` | Shell use of shared argument limits. |
| `c/kernel/exec/load/exec.c` | Argument-vector validation and program loading. |
| `Makefile` | Build defaults, artifact dependencies, and test-fragment inclusion. |
| `tools/mk_wired.py` | Traversal of literal test-fragment includes and Make continuation handling. |

## See also

- [Repository map](../getting-started/repository-map.md)
- [Contribution workflow](workflow.md)
- [Writing tests](writing-tests.md)
- [Testing](../architecture/testing.md)
