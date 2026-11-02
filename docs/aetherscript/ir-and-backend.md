# AetherScript IR and the LLVM backend

AetherScript’s native compiler checks a project into a typed tree, then lowers that tree to textual LLVM intermediate representation (IR). The host command driver invokes Clang to compile the IR and link the C runtime. For LogitOS, it invokes LLD and the AEX packager to produce an application package. The main implementation is in `c/apps/as/ir/model.h`, `c/apps/as/backend/llvm/`, and `c/apps/as/cli/commands.c`.

## Compilation path

`c/apps/as/include/project.h` exposes the checking and emission interfaces. `as_typed_check_with_library()` loads the entry module and imports, constructs an `AsTypedProject`, and calls `at_check_functions()` after parsing succeeds. `as_typed_llvm()` consumes that project and writes LLVM IR to a stream.

| Stage | Result | Implementation |
|---|---|---|
| Parse | Modules, declarations, and syntax nodes | `c/apps/as/frontend/parser.c` |
| Check and specialize | Resolved types, symbols, conversions, captures, and concrete generic instances | `c/apps/as/sema/check.c`, `c/apps/as/sema/generic.c` |
| Lower | LLVM type definitions, globals, functions, and runtime calls | `c/apps/as/backend/llvm/` |
| Compile and link | Host executable or LogitOS ELF executable | `c/apps/as/cli/commands.c` |
| Package | AEX containing the LogitOS executable | `tools/mkaex.py` |

Earlier `CLAUDE.md` notes describe a bytecode VM and compiler self-hosting. The history comment in `c/apps/as/cli/main.c` records their replacement by the current C frontend and native compilation path.

## The checked intermediate representation

The compiler’s IR is a typed tree whose records are defined in `c/apps/as/ir/model.h`. Parsing, semantic analysis, tooling, and LLVM emission share this representation.

| Record | Role |
|---|---|
| `AsTypedProject` | Owns module, function, type, global, and node tables, plus module initialization order. |
| `AtModule` | Owns source text, tokens, diagnostics, imports, and the index of its initializer function. |
| `AtFunction` | Holds parameters, result type, locals, body, captures, and generic specialization information. |
| `AtType` | Describes scalar and compound types, fields, inheritance, type arguments, and external layout offsets. |
| `AtNode` | Represents an expression or statement with its resolved type, symbol, conversions, and source location. |
| `AtLocal` | Records storage type, assignment facts, and capture relationships. |

References into the type, function, and local tables use indices. Fixed tables preserve pointers retained during recursive checking. Nodes have individual allocations owned through a growable registry, so growing that registry preserves child pointers.

Checking records decisions for lowering. `AtNode.type` identifies the resolved type, `symbol` identifies a binding or call target, and `field` identifies a resolved field. Integer literals retain their checked magnitude in `integer`. The emitter uses that magnitude directly; its comment records an earlier defect where reparsing decimal `010` as a C-style literal changed its value to octal eight.

Call symbols use nonnegative function indices and negative builtin identifiers. `c/apps/as/backend/llvm/calls.c` dispatches on those resolved symbols.

Generic specialization occurs during semantic analysis. `at_specialize()` caches instances by template and type arguments, substitutes signatures, and clones bodies. It publishes the instance signature before checking the body so recursive calls can reuse it. The backend emits functions with concrete storage types.

## Lowering to LLVM IR

`Gen` in `c/apps/as/backend/llvm/internal.h` carries the output stream, current function, temporary and label counters, exception handler, and resource cleanup chain. Feature modules share this state, giving each function a common namespace for generated values and labels.

`Val` pairs a language type index with a textual LLVM operand. `at_ir_expression()` produces values; `at_ir_address()` obtains storage for assignments, fields, indexing, and array borrows.

The emitter reserves local slots and expression workspaces in each function’s entry block. Expressions inside loops reuse those workspaces. This placement prevents repeated stack allocation on every iteration at `-O0`, where such allocations would otherwise survive until function return. The lowering design also lets LLVM promote suitable stack slots during optimization. See `c/apps/as/backend/llvm/emit.c` and `c/apps/as/backend/llvm/closure.c`.

Conditionals, loops, and exception paths become basic blocks. Conditional expressions and Boolean short circuit expressions merge results with `phi` instructions. Conditional arms receive dedicated exit blocks so their predecessors remain valid when nested guards introduce more branches.

`as_typed_llvm()` writes aggregate definitions, runtime type metadata, class method tables, globals, source paths, string constants, helper declarations, generated scanners, dictionary callbacks, language functions, adapters, and the process entry wrapper. It checks project diagnostics before emission and reports failure if lowering or stream output fails.

## Native value layouts

`at_ir_type()` in `c/apps/as/backend/llvm/support.c` maps checked types to LLVM representations.

| Language value | LLVM representation |
|---|---|
| Signed and unsigned integers | Integer of the declared width |
| `bool` | `i1` |
| `f32`, `f64` | `float`, `double` |
| `str`, borrowed slices | `{ ptr, i64 }` containing data and length |
| Callable | `{ ptr, ptr }` containing code and environment |
| Fixed array | Named array type |
| Value struct | Named struct type |
| Optional | Struct containing an `i1` presence flag and payload |
| Classes, containers, and resource handles | `ptr` |
| Typed raw pointer | `i64` address |
| `None` function result | `void` |
| `None` value | `i8` |

Integer signedness selects arithmetic, comparison, and conversion instructions. The underlying LLVM integer type carries the width.

`c/apps/as/backend/llvm/types.c` derives native sizes and field offsets through LLVM `getelementptr` expressions. It embeds these expressions in metadata matching `AtNativeType` and `AtNativeField` from `c/apps/as/runtime/type.h`. Formatting and dynamic value operations consume that metadata.

External layout records use declared byte offsets and widths stored in `AtType`. Their runtime representation is byte storage. `c/apps/as/backend/llvm/layout.c` emits scalar accesses with alignment one, supporting packed fields and overlapping members. Reading a fixed byte span creates a `Bytes` snapshot.

Class allocations begin with the fields defined by `AT_CLASS_HEADER` in `c/apps/as/runtime/class.h`: the method table pointer, required initialization mask, and completed initialization mask. The same macro supplies the C header and LLVM prefix. Language fields follow that prefix.

## Function calls and the runtime ABI

Generated language functions use LLVM’s default calling convention. Parameters and results use the representations above, and Clang lowers their signatures for the selected target.

Direct calls identify their target by checked function index. Nested function definitions receive an environment pointer before their source parameters. Callable values carry code and environment pointers, and indirect calls pass the environment first. Module functions receive adapters that accept this callable signature and forward to their direct entry points. These paths are implemented in `c/apps/as/backend/llvm/calls.c` and `c/apps/as/backend/llvm/callable.c`.

Captured bindings live in heap cells whose payloads use the binding’s native type. Closure environments retain pointers to those cells, making assignments visible through closures sharing the binding. `c/apps/as/backend/llvm/closure.c` emits cell allocation and access; `c/apps/as/runtime/closure.c` stores and scans environment cells.

The runtime boundary uses helper signatures declared by the emitter and defined in C headers under `c/apps/as/runtime/`. Text helpers return aggregate values through output pointers. For example, text concatenation fills an `AtNativeText` and returns an integer success indicator. The comment in `c/apps/as/runtime/native.h` explains that output pointers avoid differences in C aggregate return conventions between platforms.

The emitter also adapts scalar representations where required. Boolean formatting and printing widen the language’s `i1` value to the C helper’s `i32` argument.

## Exceptions, collector roots, and cleanup

Checked integer addition, subtraction, and multiplication use LLVM overflow intrinsics. Division guards reject zero divisors and signed minimum divided by minus one before emitting the division instruction. Shift and conversion guards reject inputs that would produce invalid LLVM operations. These checks are implemented in `c/apps/as/backend/llvm/numeric.c`.

The runtime keeps a pending exception and an `at_failed` flag. Generated language callers inspect the flag after calls and branch to a handler or their propagation block. A propagating function restores its collector frame and returns; the caller checks the flag before continuing with the result.

Runtime helpers have operation-specific result contracts. Helpers returning exception codes pass through `at_ir_runtime_status()`, which attaches the source path, line, and column before transferring control. Allocation helpers pass through an allocation check. The exception record and final reporting live in `c/apps/as/runtime/exception.h` and `c/apps/as/runtime/exception.c`.

Collector roots connect stack slots to generated scanners. Each language function saves the root chain with `at_gc_frame()` and restores it on return or propagation. Locals, captured cells, and intermediate managed values receive roots before later allocations can collect them. This includes an earlier call argument while a later argument is being evaluated.

Completed statements clear their temporary reference slots through `c/apps/as/backend/llvm/roots.c`. The source comment records the motivating failure: overwritten image buffers stayed reachable through expression slots until function return, exhausting the guest arena.

`AtCleanup` tracks resources owned by lexical scopes. Returns, loop exits, and exception propagation emit releases for the scopes they cross. During propagation, release results preserve the pending exception. `c/apps/as/backend/llvm/resource.c` implements this cleanup path.

## Process startup and the LogitOS ABI

The emitter generates `main` with a C-compatible argument count and argument vector. The wrapper initializes runtime process arguments and capabilities, registers global roots, runs module initializers, and calls the language entry function.

`c/apps/as/frontend/parser.c` records module initializers in import postorder. `c/apps/as/backend/llvm/emit.c` consumes that order and checks for exceptions after each initializer.

The language entry has zero parameters and returns `i64` or `None`. For an integer result, the wrapper truncates the value to a 32-bit process status. `at_finish()` reports an unhandled exception and gives its failure status priority over the language result.

For LogitOS executables, `c/apps/crt0_cli.asm` supplies `_start`. It reads the initial stack prepared by `c/kernel/exec/load/exec.c`, passes the argument count in `RDI` and argument vector in `RSI`, publishes the environment pointers, aligns the stack, and calls generated `main`. It passes the returned status to the kernel through `int 0x80`.

The language `syscall()` builtin lowers through `c/apps/as/backend/llvm/system.c` to `at_system_call()` in `c/apps/as/runtime/system.c`.

| Register | Meaning at the LogitOS interrupt |
|---|---|
| `RAX` | Syscall number on entry, signed result on return |
| `RDI` | First argument |
| `RSI` | Second argument |
| `RDX` | Third argument |

`c/apps/as/sema/system.c` requires an `unsafe` block, an `i64` syscall number, and up to three `i64` or `u64` argument words. Lowering evaluates supplied words in source order and supplies zero for omitted words. The runtime returns its own status separately from the kernel result, preserving negative kernel results as language values.

On LogitOS, `at_caps_init()` queries `SYS_CAP_QUERY` and translates kernel grants into language capability bits. The raw syscall bridge checks `AS_CAP_RAW`; the kernel applies its syscall category checks in `c/kernel/exec/syscall.c`.

`fsroot/as/lib/abi.as` supplies generated external layouts and syscall wrappers. Layout assertions in `c/apps/as/sema/abi_layout.inc`, included by `c/apps/as/sema/constants.c`, check sizes, offsets, and widths against C structures. `tools/gen_abi.py` generates these companion definitions.

## Host tools and runtime linking

The root `Makefile` defines the host compiler target through `ASC`, whose output is placed under `BUILD`. `c/apps/as/sources.mk` selects compiler sources. `c/apps/as/runtime/sources.def` supplies the runtime translation unit manifest used by the command driver and toolchain build.

The `as-toolchain` target in `tests/astyped.mk` prepares a toolchain directory under `BUILD`. It copies runtime sources, headers, the runtime manifest, and standard-library sources. It also prepares the guest link inputs:

| Input | Preparation |
|---|---|
| Native runtime archive | Compile the runtime manifest’s C units with the application flags and archive them. |
| Startup object | Assemble `c/apps/crt0_cli.asm` as ELF64. |
| Application C library archive | Archive application libc objects and bundled math objects. |
| AEX packager | Copy `tools/mkaex.py` into the toolchain directory. |

The bundled math objects satisfy library calls produced by floating point power. `c/apps/as/backend/llvm/numeric.c` emits `llvm.pow.f64`; `tests/astyped.mk` includes the math objects in the guest C library archive.

The host driver in `c/apps/as/cli/commands.c` invokes the tools as follows:

| Target | Compilation and linking |
|---|---|
| `host` | Clang compiles the emitted IR and every runtime C source in one invocation, with `-g`, and links the host executable. |
| `logitos-x86_64` | Clang compiles the IR with `--target=x86_64-elf`, `-c`, `-mno-red-zone`, `-fno-pic`, and `-fno-pie`. LLD links the object, startup object, runtime archive, and C library archive with `-nostdlib`, `-e _start`, and `-Ttext=0x50000000`. Python invokes the copied packager with `--cli`. |

The Clang invocation uses `-O2`; `--debug` selects `-O0`. Guest runtime archive compilation follows the flags in its Make recipe.

Tool and library selection have distinct settings:

| Setting | Selection |
|---|---|
| Clang | `AETHER_CLANG`, defaulting to `clang` |
| Linker | `AETHER_LD`, defaulting to `ld.lld` |
| Toolchain directory | `--toolchain`, then `AETHER_TOOLCHAIN`, then a directory named `aether-toolchain` derived from the compiler executable’s path |
| Standard library | `--stdlib`, then `AETHER_STDLIB`, then `aether-toolchain/lib` derived from the compiler executable’s path on the host |

Standard-library lookup derives its fallback from the executable path. Changing the toolchain directory requires configuring the library path as well when the library is elsewhere.

The driver launches tools through `fork()` and `execvp()` with argument vectors. It writes intermediate files into a private temporary directory and captures tool output for diagnostics. Successful artifacts are copied to a temporary sibling of the destination, flushed, and published by rename.

For inspection, `--emit-llvm` selects textual IR output. It writes to standard output by default; `-o` selects a destination file.

## Limitations

- Native compilation and linking require host tools. In the freestanding command driver, commands reaching the execution path use `as_typed_eval()` when LLVM output is not requested. Source shorthand launches an installed native artifact. These paths are defined in `c/apps/as/cli/commands.c`.
- Hosted calls to the LogitOS syscall bridge return `AT_E_RUNTIME` after the capability check. The interrupt transport in `c/apps/as/runtime/system.c` is compiled for freestanding x86-64 execution.
- Compiler resource limits include 64 modules, 512 functions, and 1,024 types. Generic instances consume these tables too. The limits are defined in `c/apps/as/ir/model.h`.

## Source map

| File | What it contains |
|---|---|
| `c/apps/as/include/project.h` | Checking, project lifetime, evaluation, and LLVM emission interfaces. |
| `c/apps/as/ir/model.h` | Typed project records, node forms, call symbols, and resource limits. |
| `c/apps/as/frontend/parser.c` | Project construction, imports, and initializer ordering. |
| `c/apps/as/sema/check.c` | Function checking and storage validation. |
| `c/apps/as/sema/generic.c` | Type substitution, instance caching, and body cloning. |
| `c/apps/as/sema/system.c` | Syscall builtin checking. |
| `c/apps/as/backend/llvm/internal.h` | Generator state, values, and lowering interfaces. |
| `c/apps/as/backend/llvm/emit.c` | Expressions, statements, scanners, functions, and process entry emission. |
| `c/apps/as/backend/llvm/support.c` | LLVM type mapping, guards, roots, and exception propagation. |
| `c/apps/as/backend/llvm/types.c` | Array definitions and runtime type metadata. |
| `c/apps/as/backend/llvm/calls.c` | Builtin and direct call lowering. |
| `c/apps/as/backend/llvm/callable.c` | Argument adaptation, indirect calls, and callable adapters. |
| `c/apps/as/backend/llvm/closure.c` | Captured cells, closure construction, and local roots. |
| `c/apps/as/backend/llvm/numeric.c` | Arithmetic, overflow checks, and conversions. |
| `c/apps/as/backend/llvm/roots.c` | Clearing completed statements’ temporary roots. |
| `c/apps/as/backend/llvm/resource.c` | Resource acquisition and cleanup across control flow. |
| `c/apps/as/backend/llvm/layout.c` | External layout field access. |
| `c/apps/as/backend/llvm/system.c` | Syscall word and result lowering. |
| `c/apps/as/runtime/native.h` | Runtime helper signatures and collector root ABI. |
| `c/apps/as/runtime/type.h` | Native type and field descriptors. |
| `c/apps/as/runtime/class.h` | Shared class header definition. |
| `c/apps/as/runtime/heap.c` | Collector root chain and tracing heap. |
| `c/apps/as/runtime/closure.c` | Closure environment storage and scanning. |
| `c/apps/as/runtime/exception.h` | Text and exception records and exception codes. |
| `c/apps/as/runtime/exception.c` | Pending exceptions and final reporting. |
| `c/apps/as/runtime/capability.c` | Startup grant query and capability translation. |
| `c/apps/as/runtime/system.c` | Raw syscall permission check and interrupt transport. |
| `c/apps/as/runtime/sources.def` | Runtime compilation unit manifest. |
| `c/apps/as/cli/main.c` | CLI entry and compiler transition history. |
| `c/apps/as/cli/commands.c` | Tool invocation, path selection, linking, and artifact publication. |
| `c/apps/as/sources.mk` | Compiler source membership. |
| `c/apps/crt0_cli.asm` | LogitOS process startup and exit. |
| `c/kernel/exec/load/exec.c` | Initial process stack construction. |
| `c/kernel/exec/syscall.c` | Kernel syscall dispatch and capability category checks. |
| `fsroot/as/lib/abi.as` | Generated layouts and syscall wrappers. |
| `c/apps/as/sema/constants.c` | ABI constants and inclusion of layout assertions. |
| `c/apps/as/sema/abi_layout.inc` | Generated structure size, offset, and width assertions. |
| `tools/gen_abi.py` | Companion ABI definition generation. |
| `Makefile` | Host compiler build rule. |
| `tests/astyped.mk` | Toolchain preparation and guest runtime linking inputs. |
| `tools/mkaex.py` | ELF packaging as AEX. |

## See also

- [AetherScript: language and toolchain](overview.md)
- [AetherScript type system and semantic analysis](type-system.md)
- [AetherScript runtime and system bindings](runtime-and-system-bindings.md)
- [System calls](../architecture/system-calls.md)
- [Applications and AEX packages](../architecture/applications-aex.md)