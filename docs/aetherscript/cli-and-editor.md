# The AetherScript command line and editor support

AetherScript’s CLI checks projects, generates LLVM IR, builds native programs, runs tests, and answers semantic completion queries. The host compiler is the `$(BUILD)/asc` Make target; LogitOS installs its guest counterpart at `/bin/as`. Studio uses compiler subprocesses for project diagnostics and member completion, together with a completion engine linked into the editor. These tools share the project frontend declared in `c/apps/as/include/project.h`.

## Commands and dispatch

`c/apps/as/cli/main.c` first calls `as_typed_command()` in `c/apps/as/cli/commands.c`. Requests passed back to the entry point include the simple checker and lexer inspection interface.

AetherScript 3 sources begin with `# aether: 3.0` or the alias `# aether: 3`. The version reader also accepts a first-line shebang followed by the version declaration. `c/apps/as/common/version.c` implements this recognition.

| Form | Behaviour |
|---|---|
| `asc check FILE` | Load the entry and imports, parse them, check types, and report diagnostics. |
| `asc build FILE` | Generate LLVM IR, compile and link a native program, then publish its artifact. |
| `asc run FILE [-- ARGS...]` | Build a temporary host executable and run it. |
| `asc test FILE [-- ARGS...]` | Build and run a generated test entry point on the host. |
| `asc complete FILE --at BYTE_OFFSET` | Write a JSON completion report for the entry source at the given byte offset. |
| `asc FILE [ARGS...]` | Recognize an AetherScript 3 source and enter the host build-and-run flow. |
| `asc -lex FILE` | Print lexer token records. |

In host source shorthand, arguments following `FILE` become program arguments. For the `run` and `test` subcommands, `--` introduces program arguments. During these host execution flows, `args()[0]` contains the source entry path. `c/apps/as/cli/commands.c` implements argument forwarding; `tests/unit/as_cli_test.py` asserts shorthand behaviour for empty arguments, spaces, Unicode text, option-shaped arguments, and exit status.

## Building and checking a project

`Makefile` defines the host compiler through its `ASC` variable. The `as-toolchain` target in `tests/astyped.mk` assembles the adjacent generated toolchain directory. It copies runtime C sources and headers, standard-library sources, and the AEX packager, and builds the guest runtime archive, startup object, and C library archive.

After building the compiler and toolchain, invoke the compiler executable as `asc` in the following examples. The working directory is the repository root; the input is `fsroot/as/examples/hello.as`.

```sh
asc check fsroot/as/examples/hello.as --json
asc run fsroot/as/examples/hello.as
asc build fsroot/as/examples/hello.as --target logitos-x86_64
```

These requests produce a project report, execute a temporary host program, and publish a guest AEX package, respectively.

The frontend copies loaded source text into an owned project. Diagnostics, semantic queries, and LLVM lowering use that project’s source and typed declarations. This ownership lets callers release overlay input after checking while keeping the checked text available for later operations. The interface is in `c/apps/as/include/project.h`; loading and ownership are implemented in `c/apps/as/frontend/parser.c`.

Library configuration follows this order:

1. `--stdlib DIR`.
2. The `AETHER_STDLIB` environment variable.
3. The default library location.

On the host, the default is the `lib` directory inside a sibling directory named `aether-toolchain`, derived from the path supplied in `argv[0]`. On LogitOS, the default is the guest installation directory `/usr/as/lib`. These rules are in `c/apps/as/cli/commands.c`.

Imports search beside the importing source and then under the configured library root. An import beginning with `std.` selects the library root directly. `c/apps/as/frontend/import.c` implements the search, including recognition of source overlays.

## Options and environment

The project command parser in `c/apps/as/cli/commands.c` accepts the following options.

| Option | Meaning |
|---|---|
| `--json` | Request structured diagnostics or native build/execution results. Completion produces JSON by default. |
| `--at BYTE_OFFSET` | Set the completion caret to a decimal byte offset. |
| `--emit-llvm` | Generate LLVM IR after checking. |
| `-o OUTPUT` | Choose the published executable, package, or LLVM output path. |
| `--debug` | Use `-O0` for native compilation; the default is `-O2`. Host compilation also passes `-g`. |
| `--target host` | Select the default native host target. |
| `--target logitos-x86_64` | Select a LogitOS AEX package. |
| `--toolchain DIR` | Set the directory containing runtime sources and guest build inputs. |
| `--stdlib DIR` | Set the standard-library source root. |
| `--overlay PATH CONTENT_FILE` | Read replacement text from `CONTENT_FILE` for project path `PATH`. May be repeated. |
| `--snapshot-stdin` | Read a framed collection of source overlays from standard input. |
| `-- ARGS...` | Supply program arguments for `run` or `test`. |

`AETHER_TOOLCHAIN` supplies the toolchain directory when `--toolchain` is omitted. Its default is a sibling directory named `aether-toolchain`, derived from `argv[0]`. `AETHER_CLANG` selects the compiler executable, defaulting to `clang`; `AETHER_LD` selects the guest linker, defaulting to `ld.lld`.

The simple checker in `c/apps/as/cli/main.c` has this interface:

```text
as check [--json] [--stdin] FILE
```

Here, `--stdin` supplies the entry’s source text, while `FILE` provides its project path and diagnostic label. Imported files are loaded through the project frontend.

For `check`, the presence of `--overlay`, `--stdlib`, or `--snapshot-stdin` selects the project command parser. Studio uses the snapshot form to provide unsaved text for multiple files.

## Native compilation and output files

For the host target, the driver emits LLVM IR and invokes Clang with the runtime C sources listed in `c/apps/as/runtime/sources.def`.

For `logitos-x86_64`, it compiles the IR to an object, links that object with the toolchain’s startup and runtime archives, and invokes the copied packager originating at `tools/mkaex.py`. The result is an AEX package. `c/apps/as/cli/commands.c` owns these subprocesses.

| Operation | Output |
|---|---|
| Host `build` | Generated executable named `a.out` by default. |
| `build --target logitos-x86_64` | Generated package named `a.out.aex` by default. |
| `build --emit-llvm` | LLVM text on standard output. |
| `build --emit-llvm -o OUTPUT` | LLVM text published at `OUTPUT`. |
| Host `run` or `test` | Temporary executable used for the request. |

The native build uses a private temporary directory containing IR, object, executable, package, and subprocess-output files. The driver removes these intermediates when the operation finishes.

`publish_artifact()` in `c/apps/as/cli/commands.c` copies a finished artifact to a unique staging file beside its destination, flushes it, renames it, and flushes the parent directory. Placing the staging file beside the destination supports publication across filesystems. Completing compilation and linking before publication preserves an existing destination when either build phase fails.

A JSON native build result contains `ok`, `phase`, `artifact`, and `snapshot`. Execution results contain `ok`, `phase`, `exit_code`, `output`, `output_truncated`, and `snapshot`; captured output combines standard output and standard error. Toolchain failures use `phase: "link"` with the subprocess status and captured output.

LLVM emission to standard output produces the IR stream. Named LLVM output can also produce a JSON project report on standard output.

## Entry points, tests, and status codes

Native executable lowering accepts an entry-module `main()` with zero parameters and a result type of `i64` or `None`. The parser also supports script statements by constructing initialization and entry functions. These mechanisms are in `c/apps/as/backend/llvm/emit.c` and `c/apps/as/frontend/parser.c`.

Host `test` discovers zero-argument functions whose names begin with `test_` and whose result type is `None`. The generated entry point runs module initialization and then the selected tests. Failure checks after each test stop subsequent calls after an uncaught failure. `tests/unit/as_typed_test.py` includes assertions for failure status, diagnostic location, suppression of a later test, and snapshot metadata.

Checking returns `0` on success and `1` for source errors. Usage and snapshot-protocol errors return `2`. Host execution returns the program’s status; compiler and linker failures propagate their subprocess status. The driver maps a subprocess terminated by a signal to `128` plus its signal number.

## Execution on LogitOS

Guest source shorthand reads the source’s version declaration and derives an installed executable from its basename. For example, `as PATH/NAME.as ARGS...` selects the guest installation path `/usr/as/bin/NAME.aex`. The launcher replaces its process with that executable and forwards the following arguments.

`launch_installed_program()` in `c/apps/as/cli/commands.c` implements this mapping. `Makefile` supplies the installation mapping, and `tests/astyped.mk` defines compilation rules for packaged examples.

The guest dispatcher also contains an evaluator route. After checking, guest `build`, `run`, and `test` requests without LLVM emission call `as_typed_eval()`. That evaluator runs module initialization followed by the entry function. Its implementation is in `c/apps/as/cli/eval.c`.

## Unsaved source snapshots

An overlay supplies replacement text for the exact path the project loader would read. This lets a request capture an edited entry together with edited imports.

`c/apps/as/include/snapshot.h` defines the pipe protocol; `c/apps/as/common/snapshot.c` parses it.

| Part | Encoding |
|---|---|
| Header | `AETHER-SNAPSHOT-1` followed by LF. |
| Source count | Decimal integer followed by LF. |
| Entry lengths | Path byte length followed by LF, then source byte length followed by LF. |
| Entry contents | Raw path bytes immediately followed by raw source bytes. |
| End | EOF after the declared entries. |

The parser checks lengths, complete payloads, duplicate paths, embedded NUL bytes, and trailing data. Length framing carries UTF-8 source bytes directly.

Studio captures document text and revisions before starting the compiler child, constructs the payload in memory, and writes it incrementally through a pipe. `c/apps/gui/studio/core/source_snapshot.c` builds the capture; `c/apps/gui/studio/core/runner.c` manages transport.

## Diagnostics format

Plain diagnostics use this header:

```text
path:line:column: code: message
```

The renderer follows it with the source line, a caret, and available help or related borrow information. `c/apps/as/common/diagnostic.c` implements rendering and source coordinates.

The project JSON report written by `as_typed_report()` in `c/apps/as/cli/report.c` contains:

| Fields | Meaning |
|---|---|
| `version`, `language`, `ok` | Report version, language version, and checking result. The current values for the first two are `1` and `3`. |
| `file`, `source_bytes`, `source_checksum` | Entry source identity. |
| `diagnostics` | Diagnostics across loaded modules. |
| `truncated` | Diagnostic storage or allocation exhaustion affected completeness. |
| `sources` | Loaded paths, byte lengths, and checksums. |
| `symbols` | Type, function, method, and variable declarations with locations and type information. |

Each project diagnostic carries `severity`, `path`, `code`, `message`, `help`, `start`, `end`, `line`, `column`, `end_line`, `end_column`, `source_checksum`, and `related`. Import-cycle diagnostics include locations for the ordered cycle.

Byte ranges start at zero and use an exclusive end. Lines and columns start at one; columns count Unicode scalar values. Diagnostic spans expand to whole UTF-8 characters. Checksums use 32-bit FNV-1a over source bytes.

Failures handled before project construction, such as failure to open the simple checker’s entry file, use the smaller JSON envelope from `as_diagnostic_write()` in `c/apps/as/common/diagnostic.c`. That envelope contains entry metadata and diagnostics. The project report adds language, loaded-source identities, and symbols.

## Studio integration

Studio’s runner starts compiler children in the project directory and connects their input and output through pipes. It preserves the configured compiler path as `argv[0]`, which the CLI uses to locate its adjacent library and toolchain.

| Editor action | Behaviour |
|---|---|
| F5 | Save dirty documents and invoke source shorthand. |
| F6 | Invoke `check --json --snapshot-stdin` with captured open documents. |
| F8 | Stop the active run or check child. |
| Ctrl+Space | Request completion immediately. |
| Up and Down with completion open | Select a candidate. |
| Tab with completion open | Insert the selected candidate. |

The bindings are in `c/apps/gui/studio/core/studio_editor.inc`. `c/apps/gui/studio/core/jobs.c` coordinates saving and job startup.

Edits schedule a background check after an 850 ms pause. The runner uses nonblocking pipes, bounded reads, and child-status polling to keep subprocess work within the UI loop. These mechanisms are in `c/apps/gui/studio/core/engine.c` and `c/apps/gui/studio/core/runner.c`.

Before attaching diagnostics, Studio checks the captured document revision and bytes, report metadata, current contents of reported sources, and agreement between diagnostics and process status. Selecting a problem opens its owning file and validates its source identity before selecting the byte range. The validation lives in `c/apps/gui/studio/core/diagnostics.c`, `c/apps/gui/studio/core/source_snapshot.c`, and `c/apps/gui/studio/core/language.c`.

## Completion and highlighting

Studio combines two completion mechanisms.

The engine in `c/apps/as/editor/completion.c` uses tolerant tokenization, symbol collection, inference from assignment shapes, and fuzzy ranking. It supplies names, parameters, declarations, keywords, builtins, module names, and receiver methods. `CmpProviders` in `c/apps/as/editor/completion.h` lets Studio supply module listings and source contents.

For member expressions in AetherScript 3 text, Studio requests compiler completion using a captured snapshot and caret offset. `c/apps/as/sema/completion.c` resolves imported module declarations. `c/apps/as/sema/completion_object.c` resolves checked struct and class members, including inherited fields and methods through the checker’s class-resolution rules.

The completion report contains entry identity, `caret`, `replace_start`, `handled`, `items`, `truncated`, and `sources`. Items carry a name, kind, type, declaration path, and declaration span.

`c/apps/gui/studio/core/completion_job.c` validates the request generation, captured text, caret, and source metadata before installing results. A report with `handled: false` selects local completion. A handled query with an empty result keeps that result authoritative.

Typing a letter, underscore, or dot schedules completion after 180 ms. Syntax highlighting shares keyword and builtin classification through `as_word_kind()`, called by `c/apps/gui/studio/core/highlight.c`.

For lexer inspection, `-lex` prints four numeric fields per token: token type, line, byte length, and the sum of token bytes modulo 9973. `c/apps/as/cli/main.c` drives this interface.

## Limitations

- Guest evaluator requests differ from host native execution: guest `build`, `run`, and `test` without LLVM emission evaluate the entry, without publishing an executable or performing host test discovery. Unsupported evaluator constructs report runtime errors. See `c/apps/as/cli/commands.c` and `c/apps/as/cli/eval.c`.
- Guest source shorthand, including Studio Run, requires an installed AEX matching the source basename. Saving edits does not rebuild that package. Host `run` and `test` reject execution of the `logitos-x86_64` target.
- AetherScript 2 execution and the former bytecode interfaces are retired. Host shorthand also rejects `--scope`; the refusal is asserted in `tests/unit/as_cli_test.py`.
- `--stdin` belongs to the simple checker. The project parser accepts overlay files or a framed snapshot and rejects combining `--overlay` with `--snapshot-stdin`.
- Snapshot input allows 64 files, 511 path bytes per entry, and 1 MiB of source per entry. Studio captures up to eight open documents. These limits are defined in `c/apps/as/include/snapshot.h` and `c/apps/gui/studio/core/runner.h`.
- Diagnostics retain up to 32 entries per module. Studio rejects check reports containing more than 32 problems or reporting truncation. Semantic completion emits up to 64 candidates. Native JSON output capture retains up to 1 MiB; Studio child output is capped at 128 KiB. See `c/apps/as/common/diagnostic.h`, `c/apps/as/sema/completion.c`, `c/apps/as/cli/commands.c`, and `c/apps/gui/studio/core/diagnostics.c`.

## Source map

| File | What it contains |
|---|---|
| `c/apps/as/cli/main.c` | Entry dispatch, simple checking, stdin input, and lexer inspection. |
| `c/apps/as/cli/commands.c` | Options, toolchain processes, native execution, publication, and guest launch routing. |
| `c/apps/as/cli/report.c` | Project diagnostics, source metadata, related locations, and symbol JSON. |
| `c/apps/as/cli/eval.c` | Evaluation of the checked tree for guest subcommands. |
| `c/apps/as/common/version.c` | Version declarations and shebang recognition. |
| `c/apps/as/common/diagnostic.c` | Checksums, UTF-8 coordinates, spans, and diagnostic rendering. |
| `c/apps/as/common/diagnostic.h` | Diagnostic records and storage limit. |
| `c/apps/as/common/snapshot.c` | Snapshot parsing, validation, and ownership. |
| `c/apps/as/include/project.h` | Checking, reporting, lowering, and evaluation interfaces. |
| `c/apps/as/include/snapshot.h` | Snapshot framing and input limits. |
| `c/apps/as/frontend/parser.c` | Source loading, project ownership, and generated script entry functions. |
| `c/apps/as/frontend/import.c` | Local and standard-library import lookup. |
| `c/apps/as/backend/llvm/emit.c` | Native entry validation and generated test execution. |
| `c/apps/as/runtime/sources.def` | Runtime source inventory used by the native driver. |
| `c/apps/as/editor/completion.c` | Tolerant completion and ranking. |
| `c/apps/as/editor/completion.h` | Completion records and provider interfaces. |
| `c/apps/as/sema/completion.c` | Semantic module completion and JSON reports. |
| `c/apps/as/sema/completion_object.c` | Struct and class member completion. |
| `c/apps/gui/studio/core/runner.c` | Child processes, pipes, polling, and capture. |
| `c/apps/gui/studio/core/runner.h` | Runner state and capture limits. |
| `c/apps/gui/studio/core/source_snapshot.c` | Document captures and snapshot payloads. |
| `c/apps/gui/studio/core/diagnostics.c` | Report validation and problem attachment. |
| `c/apps/gui/studio/core/completion_job.c` | Completion subprocess results and fallback selection. |
| `c/apps/gui/studio/core/language.c` | Completion routing and problem selection. |
| `c/apps/gui/studio/core/engine.c` | Background check scheduling. |
| `c/apps/gui/studio/core/jobs.c` | Saving and starting Run or Check. |
| `c/apps/gui/studio/core/studio_editor.inc` | Keyboard bindings and completion delay. |
| `c/apps/gui/studio/core/highlight.c` | Syntax highlighting with shared vocabulary. |
| `Makefile` | Host compiler target and guest installation mappings. |
| `tests/astyped.mk` | Toolchain assembly and native example packaging. |
| `tools/mkaex.py` | AEX packager copied into the generated toolchain. |
| `tests/unit/as_cli_test.py` | Shorthand argument, status, and scope-refusal assertions. |
| `tests/unit/as_snapshot_test.py` | Snapshot framing and unsaved-source assertions. |
| `tests/unit/as_typed_test.py` | Native execution, tests, reports, and publication assertions. |

## See also

- [AetherScript: language and toolchain](overview.md)
- [AetherScript frontend](frontend.md)
- [AetherScript type system and semantic analysis](type-system.md)
- [AetherScript IR and the LLVM backend](ir-and-backend.md)
- [Studio](../userland/studio.md)