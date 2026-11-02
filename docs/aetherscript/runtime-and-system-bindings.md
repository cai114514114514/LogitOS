# AetherScript runtime and system bindings

Compiled AetherScript programs combine native machine code with the C runtime in `c/apps/as/runtime/`. The runtime supplies managed memory, text and byte storage, I/O, resource ownership, exceptions, and access to LogitOS services. The LLVM backend generates the roots, scanners, error checks, and cleanup paths that connect these facilities to language values.

## Runtime interface

`c/apps/as/runtime/native.h` declares the core interface used by generated code. Integers retain their declared widths, floating point values use native floating point types, and text carries a pointer and byte length. Lists record their element size and scanner. These representations are lowered in `c/apps/as/backend/llvm/support.c`.

Runtime type descriptions in `c/apps/as/runtime/type.h` support operations that inspect values dynamically. The compiler emits this metadata in `c/apps/as/backend/llvm/types.c`; formatting uses it to traverse containers and values held in `Any`, as implemented in `c/apps/as/runtime/format.c`.

Several helpers publish results through output parameters. This avoids platform differences in C aggregate returns. Error conventions depend on the helper: allocation can return a null pointer or a success flag, while file and system operations return an exception code. The backend translates these results into language errors through checks in `c/apps/as/backend/llvm/support.c`.

`c/apps/as/runtime/sources.def` lists the runtime translation units. The compiler driver and the rules in `tests/astyped.mk` consume this manifest, giving runtime source registration one authority.

## Startup and executable dependencies

The LLVM backend requires a language entry function with either of these signatures:

```text
def main() -> None:
def main() -> i64:
```

The generated C-compatible `main` performs the following steps:

1. Initializes process arguments and capability state.
2. Registers roots for global values.
3. Calls module initializers in the project’s computed initialization order.
4. Calls the language entry function.
5. Calls `at_finish()` to report a pending exception and determine failure status.

An entry function returning `None` produces status zero on success. An `i64` result is truncated to the generated C entry function’s 32-bit return type. An uncaught language exception produces status 1. These paths are emitted in `c/apps/as/backend/llvm/emit.c`.

`at_process_init()` retains the argument vector and ignores `SIGPIPE`, allowing failed pipe writes to reach error handling and scope cleanup. The language `args()` operation returns fresh managed copies. External commands launched through `c/apps/as/runtime/command_launch.c` restore the default `SIGPIPE` disposition before execution. See `c/apps/as/runtime/process.c`.

The native build driver in `c/apps/as/cli/commands.c` uses two linking paths:

| Target | Build inputs | Runtime dependencies |
| --- | --- | --- |
| `host` | Generated LLVM code and runtime C sources, compiled and linked through Clang | Host C library and operating system services |
| `logitos-x86_64` | Generated object, toolchain `crt0.o`, `native.a`, and `libc.a` | LogitOS executable loading, process services, and resources used by the program |

The `as-toolchain` target in `tests/astyped.mk` prepares the runtime archive, startup object, C library archive, runtime sources and headers, standard library sources, and a copy of `tools/mkaex.py`. The C library archive includes the math objects needed by generated floating point operations.

For `logitos-x86_64`, the driver links an ELF executable with entry `_start` and text base `0x50000000`, then packages it as a CLI AEX image. The executable contains the linked runtime and C library code. Clang, LLD, and the Python packager operate during construction of the artifact.

`c/apps/crt0_cli.asm` reads arguments from the kernel’s startup stack, publishes the environment pointers, calls the generated C entry function, and passes its return status to process exit. A program’s operational dependencies also include its inherited descriptors, arguments, environment, and any files or external commands it accesses.

## Managed memory and resource ownership

The heap in `c/apps/as/runtime/heap.c` uses mark and sweep collection with stable allocation addresses. Each allocation records its payload size and scanner. Generated scanners follow references within concrete layouts, including aggregate fields and elements.

Roots form a linked stack of `AtRoot` records. Generated functions save the entry root frame, register reference slots, and restore that frame on return or failure. Runtime C helpers follow the same protocol. Allocation and collection are safepoints: references needed across either operation must already be reachable from roots. The interface contract is documented in `c/apps/as/runtime/native.h`, with compiler support in `c/apps/as/backend/llvm/emit.c` and `c/apps/as/backend/llvm/support.c`.

Substrings can point inside an allocation. The collector resolves such addresses to their containing allocation, retaining the backing storage through the view. During collection it builds a sorted address index for this lookup, with a list traversal as the fallback when index construction fails.

Containers supply their own scanning rules. `AtList` retains its backing allocation and scans initialized elements up to its count; its object identity survives backing storage growth. Closure environments retain captured cells, preserving shared access to captured values. See `c/apps/as/runtime/list.c` and `c/apps/as/runtime/closure.c`.

Two additional ownership mechanisms serve storage with other lifetime requirements:

| Storage | Ownership mechanism |
| --- | --- |
| Regions | Generated scope cleanup owns the allocation; borrow records track shared readers, a mutable borrower, and nested borrows |
| Manual allocations | A registry tracks allocations created by `at_manual_alloc()` and validates base addresses passed to `at_manual_dealloc()` |

A region move transfers the owner and clears the source slot. Release checks outstanding borrows before freeing storage. The implementation is in `c/apps/as/runtime/region.c` and `c/apps/as/runtime/region_borrow.c`.

Manual allocation uses the C allocator and requires `AS_CAP_RAW`. The registry validates a deallocation address by searching its entries before freeing storage. See `c/apps/as/runtime/allocation.c`.

## Text, bytes, and C arguments

`AtNativeText`, declared in `c/apps/as/runtime/exception.h`, contains a data pointer and an `int64_t` byte length. Text operations use that length for concatenation, comparison, searching, splitting, and printing, preserving embedded NUL bytes. Slicing can produce a view into existing storage; concatenation and case conversion allocate managed storage. See `c/apps/as/runtime/text.c`.

`Buffer` supplies mutable byte storage, while `Bytes` supplies immutable byte storage. Both use the layout in `c/apps/as/runtime/buffer.h`. Creating a `Bytes` copy snapshots the source. Decoding validates UTF-8 and returns a text view over the immutable bytes; the collector retains the backing allocation through that view. These operations are implemented in `c/apps/as/runtime/buffer.c` and `c/apps/as/runtime/bytes.c`.

C string arguments need termination and ownership across argument evaluation. The generated binding library’s `_c_string()` helper rejects embedded NULs and creates a `Bytes` value containing a trailing NUL. Wrappers retain these copies in named temporaries while evaluating later arguments. This also handles text views whose end lies inside another string. See `fsroot/as/lib/abi.as`.

`at_memory_text()` applies a similar ownership rule to raw-memory conversion: it copies the requested bytes into managed immutable storage, then validates and decodes them. The resulting text retains that snapshot. See `c/apps/as/runtime/memory.c`.

## I/O and scope cleanup

`c/apps/as/runtime/print.c` accumulates output in a 1 KiB process buffer. A completed language print appends a newline, submits populated chunks, and flushes standard output. Coalescing arguments reduces output fragmentation, while the final flush orders completed prints before subsequent port or child-command output.

Whole-file helpers in `c/apps/as/runtime/file.c` acquire a descriptor, transfer data, close the descriptor, and publish the result after successful completion. Reads and writes retry interruption and continue across short transfers. Permission-related failures become `PermissionError`; other descriptor operation failures become `IOError`. Allocation and argument validation have their own error codes.

File acquisition checks held language capability bits and path scope. With a restricted host path scope, it walks directory components through held descriptors and rejects symlinks, avoiding a path-check/open race. The guest path calls LogitOS C library file operations after the runtime scope check.

Ports provide descriptor access with retained read buffering in `c/apps/as/runtime/port.c`. Owned ports keep unread bytes between operations. Borrowed wrappers limit read-ahead to the current operation’s requested extent, allowing a later descriptor user to receive the remaining input. Releasing a borrowed wrapper preserves the underlying descriptor.

The compiler emits cleanup for ports, processes, regions, and region borrows in `c/apps/as/backend/llvm/resource.c`. Cleanup runs when control leaves a resource scope, including return, loop exits, and exception propagation. A release failure during successful scope exit can raise an error. During propagation, cleanup preserves the pending exception.

## Errors and propagation

`AtNativeException` carries an error code, message, file, line, and column. Shared field and name definitions in `c/apps/as/runtime/exception.h` keep compiler handling and runtime reporting consistent.

Generated checks attach source locations to failures such as allocation failure and invalid operations. Helpers returning exception codes supply the reason; generated code adds the calling expression’s location. `c/apps/as/backend/llvm/support.c` implements these checks and the branches through cleanup toward a handler or function failure path.

`c/apps/as/runtime/exception.c` stores the pending exception and the `at_failed` flag. Throwing assigns a location to a new error and preserves the location when rethrowing a captured error. Taking an exception copies it to the handler and clears pending state. The collector marks the pending message and file so they survive propagation across function frames.

Generated language calls check `at_failed` before continuing with their result. At process completion, `at_finish()` writes an uncaught error to standard error and returns status 1.

## Calling LogitOS services

`c/apps/as/common/system_constants.def` defines language-visible system names through `AS_SYSTEM_CONSTANT`. Syscall numbers and event constants derive from `include/abi/logit_abi.h`; language capability values derive from `c/apps/as/runtime/capability_bits.h`.

`c/apps/as/sema/constants.c` expands the manifest into a name/value table. Name resolution checks local bindings, globals, and functions before consulting this table, then gives a system constant type `i64`. The compiler uses the LogitOS ABI values even when it runs on another operating system. See `c/apps/as/sema/expression.c`.

The `syscall` builtin accepts an `i64` syscall number and up to three argument words, each typed `i64` or `u64`. The semantic checker requires an `unsafe` block. For example, `fsroot/as/lib/abi.as` contains:

```text
def proc_pid() -> i64:
    unsafe:
        return syscall(SYS_GETPID)
```

The backend fills omitted argument words with zero and calls `at_system_call()`. In a freestanding x86-64 build, the helper checks `AS_CAP_RAW` and enters the kernel with `int 0x80`:

| Register | Meaning |
| --- | --- |
| `rax` | Syscall number on entry; signed result on return |
| `rdi` | First argument |
| `rsi` | Second argument |
| `rdx` | Third argument |

The helper returns a runtime status and writes the kernel result through an output parameter. Generated code turns a nonzero runtime status into a language exception. The kernel result, including a negative error value, reaches the wrapper for interpretation according to that service’s ABI. See `c/apps/as/sema/system.c`, `c/apps/as/backend/llvm/system.c`, and `c/apps/as/runtime/system.c`.

At startup, `at_caps_init()` queries the guest process’s grant and filesystem prefix through `SYS_CAP_QUERY`. It translates kernel capability bits into language bits, mapping kernel filesystem authority to both language read and write permissions. Capability values can capture this state and derive reduced bit sets or path scopes. Runtime acquisition checks consult the held process authority. See `c/apps/as/runtime/capability.c`.

The kernel reports the calling process’s grant in `c/kernel/exec/proc.c` and applies filesystem and network category gates in `c/kernel/exec/syscall.c`. Service handlers perform further checks for their operations.

`fsroot/as/lib/abi.as` supplies generated record layouts, extent checks, argument packing, and syscall wrappers. `fsroot/as/lib/sys.as` builds file, process, clock, DNS, and ping conveniences on these bindings. `tools/gen_abi.py` generates bindings from the ABI definitions and checks that generated syscall names have entries in the constant manifest.

## Limitations

- Hosted `at_system_call()` returns `RuntimeError` after its raw-authority check succeeds. Without that authority it returns `PermissionError`. See `c/apps/as/runtime/system.c`.
- Text slicing and searching use byte offsets; case conversion and whitespace handling cover ASCII. See `c/apps/as/runtime/text.c`.
- `Buffer`, `Bytes`, and region payloads have a 64 MiB allocation ceiling through `AT_BUFFER_LIMIT`. See `c/apps/as/runtime/buffer.h`, `c/apps/as/runtime/buffer.c`, and `c/apps/as/runtime/region.c`.
- Raw addresses carry no bounds or lifetime ownership. Callers must retain referenced storage and manage manual allocation lifetimes. A raw address alone does not keep a managed allocation alive. See `c/apps/as/runtime/allocation.h`, `c/apps/as/runtime/allocation.c`, and `fsroot/as/lib/abi.as`.
- Print helpers ignore stdio write and flush return values, so output failures do not become language exceptions through this path. Their shared buffer assumes one producer per process and has no thread synchronization. See `c/apps/as/runtime/print.c`.

## See also

- [AetherScript overview](overview.md)
- [Type system and semantic analysis](type-system.md)
- [IR and the LLVM backend](ir-and-backend.md)
- [System calls](../architecture/system-calls.md)
- [Applications and AEX packages](../architecture/applications-aex.md)
- [The C library](../userland/libc.md)

## Source map

| File | What it contains |
| --- | --- |
| `c/apps/as/runtime/native.h`, `c/apps/as/runtime/type.h` | Core runtime interface, roots, containers, and type metadata |
| `c/apps/as/runtime/sources.def` | Runtime translation-unit manifest |
| `c/apps/as/runtime/heap.c` | Root stack, marking, interior-address lookup, collection, and allocation |
| `c/apps/as/runtime/list.c`, `c/apps/as/runtime/closure.c` | List scanning and captured-cell retention |
| `c/apps/as/runtime/region.c`, `c/apps/as/runtime/region_borrow.c` | Region ownership, moves, borrowing, and release |
| `c/apps/as/runtime/allocation.h`, `c/apps/as/runtime/allocation.c` | Manual allocation contract and ownership registry |
| `c/apps/as/runtime/text.c` | Text construction, views, comparison, and search |
| `c/apps/as/runtime/buffer.h`, `c/apps/as/runtime/buffer.c`, `c/apps/as/runtime/bytes.c` | Byte storage, copying, allocation ceiling, and UTF-8 decoding |
| `c/apps/as/runtime/memory.c` | Text snapshots from raw memory |
| `c/apps/as/runtime/format.c`, `c/apps/as/runtime/print.c` | Dynamic value formatting and buffered print output |
| `c/apps/as/runtime/file.c`, `c/apps/as/runtime/port.c` | File acquisition, transfers, and descriptor resources |
| `c/apps/as/runtime/process.c`, `c/apps/as/runtime/command_launch.c` | Arguments, broken-pipe handling, and external command launch |
| `c/apps/as/runtime/exception.h`, `c/apps/as/runtime/exception.c` | Text and exception records, pending errors, and final reporting |
| `c/apps/as/runtime/capability_bits.h`, `c/apps/as/runtime/capability.c` | Language capability identities, initialization, and path scopes |
| `c/apps/as/runtime/system.c` | Capability check and LogitOS syscall bridge |
| `c/apps/as/common/system_constants.def`, `c/apps/as/sema/constants.c`, `c/apps/as/sema/expression.c` | System-name definitions and compiler resolution |
| `c/apps/as/sema/system.c`, `c/apps/as/backend/llvm/system.c` | Syscall type checking and LLVM lowering |
| `c/apps/as/backend/llvm/emit.c`, `c/apps/as/backend/llvm/types.c` | Entry generation, scanners, runtime declarations, and type metadata |
| `c/apps/as/backend/llvm/support.c`, `c/apps/as/backend/llvm/resource.c`, `c/apps/as/backend/llvm/calls.c` | Error checks, propagation, cleanup, and language calls |
| `c/apps/as/cli/commands.c`, `tests/astyped.mk` | Native linking and toolchain preparation |
| `c/apps/crt0_cli.asm`, `tools/mkaex.py` | Guest executable startup and AEX packaging |
| `include/abi/logit_abi.h`, `c/kernel/exec/syscall.c`, `c/kernel/exec/proc.c` | Kernel ABI, category gates, and capability query |
| `fsroot/as/lib/abi.as`, `fsroot/as/lib/sys.as`, `tools/gen_abi.py` | Generated bindings, service conveniences, and binding consistency checks |