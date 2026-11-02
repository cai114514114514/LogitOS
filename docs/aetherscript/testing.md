# Testing AetherScript

AetherScript tests cover compiler diagnostics, generated native programs, the evaluator used by Studio, and compiled applications running inside LogitOS. Host suites compare diagnostics and program output with expected results. Guest suites exercise compiled AEX packages through the kernel’s loader, system calls and application services. Negative controls check that the test harnesses detect known defects.

## Choosing a target

Run these commands from the repository root. The host compiler recipe in `Makefile` uses the compiler source inventory in `c/apps/as/sources.mk`.

| Command | Coverage |
|---|---|
| `make test-as-fast` | Completion, the native lexer tool and the AetherScript shell suites. |
| `make test-as-typed` | Compiler diagnostics, native execution, feature suites and their controls. |
| `make test-as-check` | JSON diagnostics, source byte counts and diagnostic positions. |
| `make test-as-recovery` | Lexer and parser recovery, including checks against a compiler built with sanitizers. |
| `make test-as-completion` | Module and object completion, including controls for private declarations. |
| `make test-as-examples` | Shipped examples compared with frozen output expectations. |
| `make test-as-lexer-lib` | Native AetherScript lexer output compared with the C frontend. |
| `make test-as-eval` | Evaluator output, failure status and evaluator controls. |
| `make test-as-eval-asan` | The evaluator’s positive battery under AddressSanitizer and UndefinedBehaviorSanitizer. |
| `make test-as-typed-guest` | The host compiler suite followed by debug and release programs executed in QEMU. |
| `make test-as-packaged-guest` | Packaged example artifacts executed at their installation paths. |

The definitions are in `Makefile`, `tests/astyped.mk`, `tests/asdiag.mk` and `tests/aseval.mk`. For a focused compiler change, use the corresponding feature target, such as `test-as-generic`, `test-as-imports`, `test-as-assignment` or `test-as-region-borrow`, defined in `tests/astyped.mk`.

## Compiler diagnostics and native execution

`tests/unit/as_typed_test.py` drives the compiler CLI. Diagnostic cases submit source through `check --json --stdin`, parse the report and inspect diagnostic codes. The branch initialization case requires `AS3206` when a variable receives a value on one branch and is read after both branches merge. Another case checks the reported line and source byte count for a type error after Chinese text.

Position checks have a dedicated harness, `tests/unit/as_check_test.py`. Its expectations distinguish byte offsets from character columns and include Chinese text, incomplete expressions and an embedded NUL byte.

Native execution cases in `tests/unit/as_typed_test.py` follow this pipeline:

1. Emit LLVM intermediate representation with `build --emit-llvm`.
2. Link the emitted IR and runtime sources with Clang at `-O0` and `-O2`.
3. Execute both binaries and check their exit status and output.

Successful cases compare complete stdout. Expected failures check the exit status, error text on stderr and absence of an `UNREACHABLE` marker on stdout. Together, these assertions check the computed result and the propagation of runtime failures.

`tests/unit/as_runtime.py` reads runtime sources from `c/apps/as/runtime/sources.def`. Compiler mutation harnesses use `tests/unit/as_compiler.py`, which obtains the compiler inventory through the `as-host-sources` target. Sharing these inventories keeps test builds connected to changes in the compiler and runtime source lists.

## Golden outputs and lexer comparisons

`tests/unit/as_example_test.py` holds complete expected stdout in `OUTPUTS` and guest expectations in `GUEST_OUTPUTS`. The programs come from `fsroot/as/examples/`. Portable examples run at both optimization levels through the sanitizer helper in `tests/unit/as_managed_test.py`.

The example control demonstrates why complete output matters. It changes `a, b = b, a` to `a, b = a, b` in a temporary copy of `fsroot/as/examples/strings.as`. The changed program exits successfully and prints its final success marker. The output checker rejects the incorrect swap result earlier in stdout.

Comments beside the expectations record changes to floating point formatting, overflow reporting and sorted set output. When updating an expectation, preserve the reason for the change so the next reader can distinguish a language change from a weakened assertion.

Lexer tests use comparison implementations as oracles. `tests/unit/as_lexer_lib_test.py` builds a C lexer oracle from `tests/unit/as_lexer_oracle.c` and `c/apps/as/frontend/lexer.c`, then compares its output with the compiled AetherScript lexer across library and example sources and generated operator probes. Token text is encoded as hexadecimal to preserve quotes, control characters and UTF-8 bytes in the comparison.

The `test-selfhost-lex` target invokes `tests/unit/run-selfhost-lex.sh`. That script builds `tests/unit/aslexdump.as` in debug and release modes and compares its output with the compiler’s C lexer across the shipped library and example corpus.

## Testing the Studio evaluator

The freestanding compiler command path calls `as_typed_eval()` to execute the checked program, as shown in `c/apps/as/cli/commands.c`. This is the execution path covered by `tests/unit/as_eval_test.c`.

The evaluator battery captures stdout and compares it with expected strings. Its cases cover arithmetic, control flow, module initialization, strings and conversions, alongside cases that require a failure status. For example, the loop-break case prints a line after the loop, making an incorrect return from the enclosing function visible.

In `tests/aseval.mk`, `test-as-eval` depends on `test-as-eval-negctl`. The controls compile variants that disable loop unwinding, module initialization, formatted strings, string conversion or numeric behavior, then require the battery to fail. `test-as-eval-asan` builds and runs the positive battery with sanitizers.

## Executing compiled programs in the guest

`tests/boot/run-as-typed.py` compiles fixtures for `logitos-x86_64` in debug and release modes. The driver in `c/apps/as/cli/commands.c` selects `-O0` for debug and `-O2` for release, links the guest runtime and packages the executable as AEX.

The runner constructs a private disk containing compiled programs, helper applications and fixture data. It boots through `tests/boot/run-agent.py` with BIOS firmware and 512 MiB of RAM.

For captured commands, `tests/unit/as_typed_guest_capture.c` redirects stderr to stdout before executing the program. The guest harness redirects output and exit status into guest files, pauses QEMU and reads those files from the disk. This gives the checker the program’s output without serial messages interleaved inside it.

The runner checks:

- Expected exit status.
- Complete output for cases with fixed expectations.
- Error text and failure propagation for programs expected to fail.
- The expected source path and a nonzero line and column for runtime errors.
- Measurement or interaction results through specialized checkers.

The run directory retains build reports, source snapshots, artifact hashes, command captures and logs. A generated JSON report records selected cases, completed checks and completion status. `AS_TYPED_GUEST_OUT` selects the output directory; `AS_TYPED_GUEST_BASE` selects the base guest build.

Focused targets reuse this runner. For example, `test-as-clock-guest` includes programs that report a stopped clock or an incorrect clock rate, requiring the measurement checker to reject both. `test-as-capcheck-guest` and `test-as-input-guest` select capability and input cases. Their definitions are in `tests/astyped.mk`.

Packaging has another gate. `tests/boot/run-as-packaged.py` reads the artifact mapping from `as-native-example-files`, checks its names against a maintained behavior inventory and executes the artifacts at the mapped guest paths. Inventory equality makes each newly packaged example require an execution expectation.

## Negative controls

A negative control introduces a known defect and requires the relevant checker to detect it. Existing controls use temporary compiler sources, runtime sources, emitted IR or example programs.

| Control | Injected defect | Required observation |
|---|---|---|
| `test-as-typed-negctl` | Changes the branch initialization merge in a temporary copy of `c/apps/as/sema/check.c`. | The shared compiler battery fails at `branch-initialization`. |
| `test-as-examples-negctl` | Breaks the example’s swap assignment. | Full output comparison rejects the result while the success marker remains present. |
| `test-as-lexer-lib-negctl` | Changes string token emission or compound operator recognition in a temporary lexer module. | The compiled fixture reports an assertion failure. |
| `test-as-managed-negctl` | Disables collection in a runtime copy or removes temporary root registrations from emitted IR. | The fixture reports an assertion failure, or AddressSanitizer reports a heap use after free. |

These controls are prerequisites of their corresponding positive targets in `tests/astyped.mk`.

Check the failure mechanism when writing a control. `tests/unit/as_typed_negative.py` requires the mutant compiler to build successfully before running the shared battery. It then accepts the specific `branch-initialization` assertion. A compiler build error therefore fails the control itself.

## Adding a language test

Extend the closest feature harness under `tests/unit/`. Use `tests/fixtures/astyped/` for programs that need imported modules, shared inputs or execution on both host and guest. `tests/unit/as_typed_test.py` demonstrates inline source programs and fixture paths.

1. Start the program with an accepted language header, such as `# aether: 3.0`.
2. For a diagnostic regression, check the exit status and diagnostic code. Assert positions when the defect concerns source locations.
3. For an execution regression, check both optimization levels. Compare complete output for fixed results; for expected failures, check status, error text and propagation.
4. Add a control that reproduces the defect and require the intended assertion or runtime failure.
5. Make the control a prerequisite of the positive target. Add the positive target to `test-as-typed` when it belongs in routine compiler coverage.
6. For kernel interactions, extend the case expectations, source mapping, arguments and disk inputs in `tests/boot/run-as-typed.py`.

For shipped examples, update `tests/unit/as_example_test.py` or the behavior checker in `tests/boot/run-as-packaged.py`, according to how the example is exercised.

The host compiler also provides a `test` subcommand. The LLVM backend selects functions named with the `test_` prefix that take no arguments and return `None`. The arithmetic test in `tests/unit/as_typed_test.py` is:

```python
# aether: 3
def test_arithmetic() -> None:
    assert 3 ** 3 == 27
```

The generated runner checks failure propagation after each test call and stops after a failure. With `--json`, the CLI reports execution status, captured output and the source snapshot. These mechanisms are implemented in `c/apps/as/backend/llvm/emit.c` and `c/apps/as/cli/commands.c`.

After changing test wiring, run `make test-mk-wired`, then the affected positive target. Record the control’s observed failure mechanism, as required by `AGENTS.md`.

## Limitations

- Host suites establish behavior with the host toolchain and runtime. The typed guest runner covers BIOS boot under QEMU; physical hardware and UEFI require other evidence.
- The general typed and packaged guest targets consume a prepared base ISO and helper applications. Prepare that base build before invoking them; their prerequisites build the compiler, toolchain or test artifacts named in `tests/astyped.mk`.
- The managed-program sanitizer helper and evaluator sanitizer target disable leak detection because Apple Silicon lacks LeakSanitizer support. The managed fixture also checks reclamation through assertions, as documented in `tests/unit/as_managed_test.py`.
- Earlier documentation describes compiler self-hosting, bytecode comparison and fixpoint gates. Retirement comments in `Makefile` and `tests/ascross.mk` record their removal. The surviving `test-selfhost-lex` target compares the native lexer tool with the C frontend.
- The packaged runner gives startup coverage to selected long-running examples. Their dedicated stress gates carry the broader workload assertions, as described in `tests/boot/run-as-packaged.py`.

## Source map

| File | What it contains |
|---|---|
| `Makefile` | Host compiler recipe, fast suite, lexer gate and fragment includes. |
| `tests/astyped.mk` | Native feature suites, controls, toolchain rules and guest targets. |
| `tests/asdiag.mk` | Diagnostic, recovery and completion targets. |
| `tests/aseval.mk` | Evaluator battery, controls and sanitizer target. |
| `tests/ascross.mk` | Retirement explanation for the compiler bytecode comparison gate. |
| `c/apps/as/sources.mk` | Compiler source inventories. |
| `c/apps/as/runtime/sources.def` | Runtime source inventory shared by builds and tests. |
| `tests/unit/as_compiler.py` | Compiler inventory lookup for mutation builds. |
| `tests/unit/as_runtime.py` | Runtime inventory reader for host tests. |
| `tests/unit/as_typed_test.py` | Diagnostics, native execution and CLI regression checks. |
| `tests/unit/as_check_test.py` | Source byte counts and diagnostic position checks. |
| `tests/unit/as_typed_negative.py` | Branch initialization mutation and required assertion failure. |
| `tests/unit/as_example_test.py` | Example output expectations and swap control. |
| `tests/unit/as_managed_test.py` | Sanitizer helper and managed-memory controls. |
| `tests/unit/as_lexer_lib_test.py` | Lexer comparisons and token mutation controls. |
| `tests/unit/as_lexer_oracle.c` | Token output oracle using the C lexer. |
| `tests/unit/run-selfhost-lex.sh` | Native lexer tool comparisons in both build modes. |
| `tests/unit/as_eval_test.c` | Evaluator output and failure assertions. |
| `tests/boot/run-as-typed.py` | Guest compilation, execution, checks and reports. |
| `tests/boot/run-as-packaged.py` | Packaged artifact inventory and behavior checks. |
| `tests/boot/run-agent.py` | QEMU lifecycle and output capture through guest files. |
| `tests/unit/as_typed_guest_capture.c` | Guest stderr capture and restricted-child execution. |
| `c/apps/as/sema/check.c` | Branch initialization merge targeted by the compiler control. |
| `c/apps/as/cli/commands.c` | Toolchain invocation, evaluator dispatch and execution reports. |
| `c/apps/as/backend/llvm/emit.c` | Native test discovery and failure propagation. |
| `AGENTS.md` | Requirements for test wiring and observed control failures. |

## See also

- [AetherScript: language and toolchain](overview.md)
- [AetherScript lexer and parser](frontend.md)
- [AetherScript IR and the LLVM backend](ir-and-backend.md)
- [Building LogitOS](../getting-started/building.md)
- [Writing tests](../contributing/writing-tests.md)