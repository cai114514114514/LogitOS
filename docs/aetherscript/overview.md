# AetherScript: language and toolchain

AetherScript is LogitOS’s language for scripts and applications. The C toolchain under `c/apps/as/` implements a growing subset of language version 3.0, with indentation based syntax, static types, type inference, modules, managed objects and scoped resources. It checks source projects and emits textual LLVM intermediate representation (IR). Host tools compile that IR into host executables or LogitOS AEX packages. The guest executable provides checking, completion, installed program launching and evaluation of checked syntax trees.

## Language versions

`c/apps/as/common/version.c` recognizes a version declaration on the first line, or immediately after a shebang line. `c/apps/as/frontend/parser.c` checks the declaration for each source module, including imports.

| Declaration | Classification |
|---|---|
| `# aether: 3.0` | Current native language version. |
| `# aether: 3` | Alias for 3.0. Whitespace after `#` is optional. |
| `# aether: 2`, or an unversioned file | Legacy A2 classification used by dispatch and diagnostics. |
| `# aether: 1` | Retired A1 classification used for migration diagnostics. |

The opening of `c/apps/as/README.md` still describes A2 retaining its bytecode compiler and virtual machine during migration. The current sources read differently: `c/apps/as/sources.mk` selects the A3 compiler stages and editor support, and `c/apps/as/cli/main.c` reports that the A2 engine, bytecode execution and on-device compiler were removed. The version reader retains legacy classifications and diagnostic wording.

`TRANSPARENCY.md` and `THIRD_PARTY.md` record material adaptation from Crafting Interpreters/clox in AetherScript’s compiler and VM lineage, alongside subsequent project development.

## Toolchain structure

The compiler builds an owned source snapshot and a checked syntax tree. Tokens retain locations in the snapshot’s source buffers, so diagnostics, completion and code generation refer to the same source bytes. The public project API is declared in `c/apps/as/include/project.h`.

| Piece | Responsibility | Main sources |
|---|---|---|
| Frontend | Tokenization, indentation, parsing and module loading. | `c/apps/as/frontend/lexer.c`, `c/apps/as/frontend/parser.c`, `c/apps/as/frontend/import.c` |
| Semantic analysis | Names, types, definite assignment, generic specialization, ownership and borrowing. | `c/apps/as/sema/check.c`, `c/apps/as/sema/generic.c`, `c/apps/as/sema/region_flow.c`, `c/apps/as/sema/region_borrow.c` |
| Shared IR | Syntax nodes annotated with types, symbols, conversions and source locations. | `c/apps/as/ir/model.h` |
| LLVM backend | Textual LLVM generation, native calls, control flow, collector roots and resource cleanup. | `c/apps/as/backend/llvm/emit.c`, `c/apps/as/backend/llvm/resource.c` |
| Runtime | Managed allocation, containers, text, exceptions, capabilities and system resources. | `c/apps/as/runtime/sources.def` |
| CLI | Command dispatch, snapshots, tool invocation, execution and artifact publication. | `c/apps/as/cli/main.c`, `c/apps/as/cli/commands.c` |
| Editor support | Tolerant local completion and completion from checked receiver types. | `c/apps/as/editor/completion.c`, `c/apps/as/sema/completion.c` |

The shared IR is the checked syntax tree. For example, `AtNode` in `c/apps/as/ir/model.h` holds a resolved type and symbol together with its source token. The backend consumes those annotations when generating LLVM.

`c/apps/as/sources.mk` selects compiler translation units. Generated programs receive runtime code from the manifest in `c/apps/as/runtime/sources.def`. The CLI and runtime build rules read that manifest, keeping their runtime source inventories aligned.

## From source to executable

The host driver in `c/apps/as/cli/commands.c` coordinates these stages:

1. Load the entry source and imported modules into the project snapshot. Editor overlays can supply source bytes for unsaved files.
2. Parse and check the project, resolving names and types, specializing generics and checking resource lifetimes.
3. Emit textual LLVM through `as_typed_llvm()`.
4. Invoke the tools for the selected target.
5. Publish the completed artifact.

Import resolution follows the importing module’s location. Bare imports first search beside that module, then use the configured library root. An import beginning with `std.` selects the library root directly. These rules are implemented in `c/apps/as/frontend/import.c`.

For an executable, the LLVM backend selects a zero-argument `main()` returning `i64` or `None`. The parser can synthesize `main()` from statements at module scope. Module variable initializers have their own generated functions. See `c/apps/as/frontend/parser.c` and `c/apps/as/backend/llvm/emit.c`.

| Target | Compilation and linking |
|---|---|
| `host` | Clang compiles the LLVM text together with the runtime C sources and links a host executable. |
| `logitos-x86_64` | Clang compiles the LLVM text for `x86_64-elf`. LLD links the object with startup code, the native runtime archive and LogitOS libc. Python runs the bundled copy of `tools/mkaex.py` to produce a CLI AEX package. |

The generated entry code initializes arguments and capabilities, registers managed global roots, runs module initializers and calls the language entry function. Test builds generate an entry point that calls eligible test functions. Both forms are emitted by `c/apps/as/backend/llvm/emit.c`.

Artifact publication uses a temporary sibling of the destination. The driver copies and synchronizes the completed file, renames it into place, then synchronizes the parent directory. Keeping the temporary beside the destination supports the rename across builds whose private scratch directory resides on another filesystem. This sequence is in `c/apps/as/cli/commands.c`.

## Runtime and system bindings

Managed allocations use the tracing collector in `c/apps/as/runtime/heap.c`. Compiler-generated roots and scanners describe references in native storage. The collector marks reachable allocations and frees unreachable ones while keeping surviving objects at stable addresses.

Scoped resources receive cleanup code tied to control flow. `c/apps/as/backend/llvm/resource.c` emits release operations for ports, processes, regions and region borrows. Its callers handle scope completion, returns, loop exits and exception propagation in `c/apps/as/backend/llvm/emit.c` and `c/apps/as/backend/llvm/support.c`.

Capabilities are initialized inside each generated program. On LogitOS, `c/apps/as/runtime/capability.c` reads the kernel grant and maps it into language capability bits and a path scope. Hosted programs receive the runtime’s host capability set.

`fsroot/as/lib/abi.as` supplies generated layouts and typed wrappers for the kernel ABI. Direct syscall transport is implemented in `c/apps/as/runtime/system.c`, which checks raw capability before issuing the LogitOS software interrupt.

## Host tools and configuration

Checking, completion and LLVM text generation run through the compiler’s C implementation. Machine code generation is delegated to external tools by `c/apps/as/cli/commands.c`.

| Setting | Purpose |
|---|---|
| `AETHER_CLANG` | Clang executable; defaults to `clang`. |
| `AETHER_LD` | Linker for the LogitOS target; defaults to `ld.lld`. |
| `AETHER_TOOLCHAIN` or `--toolchain` | Runtime sources and target build inputs. |
| `AETHER_STDLIB` or `--stdlib` | AetherScript library root. |

The driver selects `-O2` by default and `-O0` with `--debug`. For host builds it also passes `-g`.

The `as-toolchain` target in `tests/astyped.mk` assembles runtime sources and headers, library sources, the runtime archive, startup object, libc archive and AEX packager. `Makefile` builds the host executable named `asc` and packages the guest executable for installation as `/bin/as`.

## CLI and guest execution

The host `asc` executable and guest `/bin/as` share command dispatch.

| Host subcommand | Result |
|---|---|
| `check` | Check the source project and report diagnostics. |
| `build` | Produce a native artifact, or LLVM text with `--emit-llvm`. |
| `run` | Build and execute a temporary host executable. |
| `test` | Build and execute a runner for zero-argument functions whose names begin with `test_` and whose return type is `None`. |
| `complete` | Query semantic completion at a byte offset. |

On the host, source shorthand routes an A3 source file through the build and run driver, forwarding program arguments.

On LogitOS, source shorthand reads the source declaration and derives an installed AEX name from the source basename. It replaces the current process with that program and forwards arguments. The destination is under the installed AetherScript binary directory. This dispatch is implemented in `c/apps/as/cli/commands.c`.

Guest command mode has another execution path. After checking, `build`, `run` and `test` reach `as_typed_eval()` when `--emit-llvm` is absent. The evaluator in `c/apps/as/cli/eval.c` walks the checked tree and handles scalar values, strings, user function calls, branches and loops. With `--emit-llvm`, the guest uses the LLVM text emitter.

## Editor integration

Studio sends source snapshots through a pipe for checking and semantic completion. The compiler copies overlay source bytes into its project, retaining the locations needed by diagnostics and completion results.

`c/apps/gui/studio/core/language.c` selects semantic completion for member queries and uses the tolerant completion engine for other contexts. The semantic path can resolve imported module members and checked object receivers.

Studio Run invokes source shorthand, as shown in `c/apps/gui/studio/core/runner.c`. On LogitOS this reaches the installed AEX launcher. Comments in the evaluator and its test fragment still describe a former Studio path through command-mode evaluation.

## Current status

The tree contains native lowering and runtime implementations for containers, closures, classes, exceptions, scoped resources, raw memory and kernel bindings. `tests/astyped.mk` defines host and guest execution suites. `tests/aseval.mk` defines evaluator output checks and mutation controls.

The migration audit in `tests/unit/as_a3_cutover_test.py` checks retained library APIs, examples, builtin lowering and retired engine artifacts. Its `test-as-a3-only` Make target depends on the typed guest suite.

The surviving lexer bootstrap check, `tests/unit/run-selfhost-lex.sh`, builds an AetherScript token-dump tool and compares its output with the C lexer across shipped library and example sources in debug and release modes.

This overview reflects source inspection on 2 November 2026.

## Limitations

- The compiler implements a subset of AetherScript 3.0. The version reader rejects unimplemented 3.x minor declarations.
- Native executable production depends on host tools. Guest command mode evaluates the checked tree; its `test` path uses that evaluator rather than the LLVM test runner. Guest source shorthand requires a matching installed AEX.
- The evaluator rejects methods, closures and other unhandled constructs in `c/apps/as/cli/eval.c`.
- Compiler storage is bounded. `c/apps/as/ir/model.h` provides space for 64 modules, 512 functions, 256 locals per function and 1 MiB per source file.
- Raw memory operations place memory validity and lifetime obligations on the programmer. Hosted execution rejects LogitOS syscall transport. These boundaries appear in `c/apps/as/runtime/system.c` and `TRANSPARENCY.md`.
- Current self-hosting coverage concerns the lexer. `Makefile` records removal of the former bytecode compiler and fixpoint stages; the native compiler remains implemented in C.
- Builds and tests were not run for this review.

## Source map

| File | What it contains |
|---|---|
| `c/apps/as/README.md` | Compiler maintenance notes and migration history, including older claims. |
| `c/apps/as/sources.mk` | Compiler source selection. |
| `c/apps/as/include/project.h` | Public project and overlay API. |
| `c/apps/as/common/version.c` | Version recognition and diagnostics. |
| `c/apps/as/frontend/lexer.c` | Tokens and indentation handling. |
| `c/apps/as/frontend/parser.c` | Project construction, parsing and module loading. |
| `c/apps/as/frontend/import.c` | Local, library and overlay import resolution. |
| `c/apps/as/sema/check.c` | Type and control flow checking. |
| `c/apps/as/sema/generic.c` | Generic type construction and specialization. |
| `c/apps/as/sema/region_flow.c`, `c/apps/as/sema/region_borrow.c` | Ownership and borrow analyses. |
| `c/apps/as/ir/model.h` | Checked tree, project storage and compiler limits. |
| `c/apps/as/backend/llvm/emit.c` | LLVM generation and program entry points. |
| `c/apps/as/backend/llvm/resource.c`, `c/apps/as/backend/llvm/support.c` | Resource cleanup and exception propagation. |
| `c/apps/as/runtime/sources.def` | Runtime translation-unit manifest. |
| `c/apps/as/runtime/heap.c` | Tracing collector and root management. |
| `c/apps/as/runtime/capability.c`, `c/apps/as/runtime/system.c` | Capability initialization and syscall transport. |
| `c/apps/as/cli/main.c`, `c/apps/as/cli/commands.c` | CLI dispatch, host tools, guest launching and publication. |
| `c/apps/as/cli/eval.c` | Checked-tree evaluator. |
| `c/apps/as/editor/completion.c`, `c/apps/as/sema/completion.c` | Tolerant and semantic completion. |
| `c/apps/gui/studio/core/language.c`, `c/apps/gui/studio/core/runner.c` | Studio language services and compiler subprocesses. |
| `fsroot/as/lib/abi.as` | Generated kernel ABI layouts and wrappers. |
| `tools/mkaex.py` | AEX packaging. |
| `Makefile`, `tests/astyped.mk` | Compiler, toolchain bundle and native verification rules. |
| `tests/aseval.mk` | Evaluator checks and controls. |
| `tests/unit/as_a3_cutover_test.py` | Migration inventory and lowering audit. |
| `tests/unit/run-selfhost-lex.sh` | AetherScript and C lexer comparison. |
| `TRANSPARENCY.md`, `THIRD_PARTY.md` | Claim boundaries and compiler provenance. |

## See also

- [AetherScript language guide](language-guide.md)
- [Type system and semantic analysis](type-system.md)
- [Lexer and parser](frontend.md)
- [IR and the LLVM backend](ir-and-backend.md)
- [Runtime and system bindings](runtime-and-system-bindings.md)
- [Command line and editor support](cli-and-editor.md)
- [Testing AetherScript](testing.md)
- [Applications and AEX packages](../architecture/applications-aex.md)
