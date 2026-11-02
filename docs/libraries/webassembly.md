# WebAssembly engine

LogitOS provides a WebAssembly binary decoder, validator and interpreter under `c/lib/wasm/`. Modules describe functions through indices and access data through offsets into linear memory. The embedding application supplies allocation storage and host imports. The browser connects the engine to QuickJS through native bindings and a JavaScript prelude that defines the `WebAssembly` namespace.

## Interfaces and ownership

The public interfaces describe a loading and execution sequence:

| Operation | Interface | Contract |
|---|---|---|
| Decode and validate | `wasm_load()` | Populate a `struct wasm_module` supplied by the caller; return a status code |
| Instantiate | `wasm_instantiate()` | Allocate instance state, resolve imports, apply segments and execute the start function |
| Find an export | `wasm_export_index()` | Match a name and export kind; return whether an index was found |
| Invoke a function | `wasm_invoke()` | Read arguments and write results through arrays supplied by the caller; return a trap code |

The declarations are in `c/lib/wasm/wasm.h` and `c/lib/wasm/wasm_exec.h`. Loading is also available as separate calls to `wasm_decode()` and `wasm_validate()`. Instantiation requires a module that has passed validation.

Allocation uses a `struct wasm_arena` owned by the caller. The allocator rounds allocation offsets to multiples of eight, checks size multiplication and alignment overflow, and zeroes allocated storage. The caller controls the arena’s capacity and lifetime. This allocation interface lets browser bindings and host harnesses use the same engine implementation.

A decoded module borrows its input bytes. Function signatures retain pointers into the buffer; names, function bodies, initializers and data segments retain pointers or spans into it. Instances borrow the module, their arena and imported function records. The embedding application must preserve these objects throughout execution. Browser compilation retains a native copy of the module bytes to satisfy this contract.

## Binary decoding

`wasm_decode()` in `c/lib/wasm/wasm_parse.c` checks the magic bytes and binary version, then reads sections through bounded cursors. It checks section ordering, section sizes and agreement between function declarations and code bodies. Custom sections receive UTF-8 validation of their names before their remaining payload is skipped.

The integer readers check LEB128 encoding length and unused high bits. They accept padding within the permitted integer width. A padded encoding can still represent a valid WebAssembly integer.

Names receive UTF-8 validation, including checks for overlong encodings, surrogate code points and values above the Unicode range. Vector readers compare element counts with the remaining input before allocating records. Reader bounds checks compare requested lengths with the remaining range, which avoids overflow in a calculation that adds an offset and a length.

The decoded module preserves WebAssembly index ordering: imports precede definitions within each index space. Lookup arrays connect function indices to signatures and global indices to types.

Decoding also walks every function body syntactically. This consumes instructions and their immediates, so truncated instructions fail during decoding before type checking begins.

## Validation

`wasm_walk()` in `c/lib/wasm/wasm_valid.c` serves both syntax checking and validation. Its declaration and observer interface are in `c/lib/wasm/wasm_int.h`. The decoder calls the walker for syntax only; validation supplies operand and control stacks. Sharing this walk keeps the two passes aligned on instruction boundaries.

Each validation control frame records its entry stack height, result type and reachability. After an unconditional branch, return or unreachable instruction, the frame permits polymorphic operands. Values pushed by later instructions still participate in checks on type and stack height. Closing a block checks its result and remaining operands.

Module validation covers:

- Type, function, local, global, label, memory and table indices.
- Function arguments and results, branch operands and types for indirect calls.
- Global mutability and memory alignment annotations.
- Declared limits, uniqueness of export names and the start function’s empty signature.
- Constant expressions for global initializers and segment offsets.

An accepted constant expression contains a numeric constant or a read of an imported immutable global, followed by its closing instruction.

The error API distinguishes malformed encodings, invalid programs, unsupported features and implementation resource failures. `wasm_err_is_rejection()` identifies verdicts about module contents, allowing harnesses to account for unsupported features and exhausted resources under different categories.

## Instantiation and execution

`wasm_instantiate()` in `c/lib/wasm/wasm_exec.c` resolves imports, allocates memory, tables, globals and execution stacks, and initializes defined globals. It checks every active element and data segment destination before applying segment writes. It then executes the start function when the module declares one.

For each defined function, a prepass observes `wasm_walk()` to record matching block, else and end positions and a bitmap of instruction starts. The interpreter uses this metadata to execute structured control flow. Before dispatch, it checks that its cursor names a recorded instruction start; a failed check returns `WASM_TRAP_DESYNC`. This makes errors at instruction boundaries observable when the executor consumes immediates incorrectly.

Runtime values occupy an untagged `union wasm_val`, with views for integers, floating point and raw bits. Validation establishes the types used by execution. Parameters, locals and operands occupy the instance’s value stack, while control frames occupy another stack. Function calls recurse through C and check the configured depth bound.

Numeric instructions include checks for division by zero, signed division overflow and invalid conversions of floating point values. Shift counts are masked to the operand width. Helpers for floating point handle NaNs, signed zero and rounding cases. Runtime failures return trap codes through the call chain; `wasm_trapstr()` supplies descriptions.

## Linear memory and tables

Linear memory uses pages of 65,536 bytes. Loads and stores calculate effective addresses in 64 bits and check the access width against the available bytes. Assembly and storage proceed byte by byte to implement little-endian values and permit unaligned accesses. These paths are in `c/lib/wasm/wasm_exec.c`.

Memory growth allocates a zeroed block from the instance arena and copies existing bytes into it. The memory instruction returns the previous page count on success or `0xFFFFFFFF` on refusal. Embedders can request growth through `wasm_mem_grow()`.

An imported memory can supply `struct wasm_hostimport.memp`, the address of the host’s base pointer variable. Growth updates this pointer and the host page count. An embedding that continues accessing imported memory after growth needs this connection to follow the relocated bytes. Successive arena allocations retain earlier blocks until the arena is released.

Tables contain function indices, with `WASM_NOFUNC` marking an empty slot. Active element segments populate them during instantiation. `wasm_table_get()` and `wasm_table_set()` provide checked access to the storage.

An indirect call checks the selected slot, its function index and its signature. Signature comparison examines parameter and result types, so equivalent signatures declared at different type indices can match.

## Imports and host functions

A `struct wasm_hostimport` identifies an import by module name, field name and kind. Instantiation searches these records to satisfy the module’s declarations.

| Import kind | Host supplies |
|---|---|
| Function | A `wasm_hostfn` callback and context pointer |
| Global | A numeric value, value type and mutability |
| Memory | Byte storage, pointers to the page count and the maximum, and an optional destination for a base pointer update |
| Table | Storage for function indices, size and maximum pointers |

A host callback receives borrowed argument and result arrays of `union wasm_val`. It returns success or a trap code that execution propagates. The adapter must read arguments and write results according to the imported function’s declared signature.

Memory and table imports connect execution to host storage. Global imports initialize entries in the instance’s global array. The interface is declared in `c/lib/wasm/wasm_exec.h`; matching and dispatch are implemented in `c/lib/wasm/wasm_exec.c`.

## Browser bindings

`c/apps/gui/browser/js/wasm/wasm.c` textually includes the decoder, validator and executor. This carries the engine implementation with the binding into browser and host harness builds. The library’s core harnesses also compile its source files directly.

The JavaScript layer in `c/apps/gui/browser/js/wasm/wasm_prelude.inc` defines `Module`, `Instance`, `Memory`, `Table`, numeric `Global` objects, error classes and namespace methods. Private `WeakMap` instances hold wrapper state. Native primitives perform byte handling, allocation, numeric conversion and engine calls.

Compilation copies the input bytes and calls `wasm_load()`. The `compile()` and `instantiate()` methods perform their work inside Promise executors, so construction failures reject the returned Promises. Installation adds `compileStreaming()` and `instantiateStreaming()` when `Response` exists; these methods await `arrayBuffer()` before constructing a module.

Module loading errors become `CompileError`. Missing imports and the checks on import shape in the prelude produce `LinkError`; invalid arguments can produce `TypeError`. Execution traps become `RuntimeError`. During exported calls, an exception thrown by an imported JavaScript function propagates as the original exception. Native function argument and result conversion carries `i64` through BigInt.

A memory’s JavaScript `ArrayBuffer` aliases its native bytes. `jw_mem_sync()` compares the current base and length with the cached buffer after exported calls and during buffer access. A changed base or length causes the cached buffer to be detached. This covers relocation caused by the memory instruction and by JavaScript growth requests.

## Interrupts and teardown

The interpreter polls a host callback after `WASM_POLL_PERIOD` events, currently 10,000. Calls and branches back to loop heads count as events. `wasm_set_interrupt()` installs the callback; a stop request returns `WASM_TRAP_INTERRUPTED`. The declarations are in `c/lib/wasm/wasm_exec.h`.

For browser exports, `jw_poll()` forwards polling to the owning QuickJS runtime’s interrupt handler. The binding converts interruption into an uncatchable QuickJS error, allowing the runtime watchdog to stop execution even when page code surrounds the call with exception handling.

Each binding installation owns a `struct jw_realm` containing native resource records. Private native function data captures this owner, and resource tokens invoke finalizers. `js_wasm_reset()` retires installations belonging to its context, detaches cached buffers and releases arenas and retained callbacks. Native methods retained across reset reject access through the retired owner.

Page installation and teardown call the binding in `c/apps/gui/browser/js/runtime/page.c`. Worker setup and teardown contain corresponding hooks in `c/apps/gui/browser/js/workers/worker.c`.

## Limitations

- The engine accepts numeric modules in the MVP style with sign-extension instructions. Functions and blocks have at most one result, and modules have at most one memory and one function table, counting imports. Bulk memory, saturating conversions, SIMD, atomics, shared memory, memory64, reference values, GC, exception handling and tail calls are unsupported. Correction: the older opening comment in `c/lib/wasm/wasm.h` lists sign-extension instructions as refused; `c/lib/wasm/wasm_valid.c`, `c/lib/wasm/wasm_exec.c` and `tests/unit/wasm_exec_test.c` implement and exercise opcodes `0xC0` through `0xC4`.
- Validation accepts memory declarations through 65,536 pages. The executor’s default `WASM_MEM_PAGES_CAP` is 512 pages, or 32 MiB, for defined memory allocation and growth. Browser memory construction and growth use the same ceiling. Defined table allocation depends on arena capacity; the browser table constructor accepts an initial size through 10,000,000 entries. Arena exhaustion can cause earlier refusal.
- Validation reserves 65,536 operand-type slots and 4,096 control frames. Execution reserves 65,536 value slots and 8,192 control frames, with `WASM_MAX_CALL_DEPTH` set to 1,024. Browser function adapters accept up to 16 parameters. Each binding owner has 128 slots per native resource kind, and the registry permits 64 live or retained retired owners.
- Imported globals are copied into instance state. Browser import conversion also takes mutability from the module declaration. Memory and table imports check current minimum sizes but omit maximum compatibility checks. Imported memory bases and page counts are captured per instance, so growth by one instance can leave another instance using an older base and size. These behaviors follow from import resolution and growth in `c/lib/wasm/wasm_exec.c` and import adaptation in the browser binding.
- Native import matching uses C strings. The browser copies each module and field name into a buffer holding at most 127 bytes plus its terminator. Longer names and names containing NUL fail to match. Tables store indices within an instance; the JavaScript setter rejects functions from another instance after a table has an adopting instance. `Table.grow()` and `Module.customSections()` are absent from the prelude.
- Cached buffers are detached when their base or length changes. For an attached memory, a successful growth by zero keeps the same base and length, so the cached buffer stays attached. Buffer ownership also has a lifetime gap: `jw_mem_buffer()` creates an alias without retaining the owning resource token, while `jw_free_mem()` and `jw_free_inst()` can release backing storage during collection. A retained buffer can consequently outlive that storage.
- The browser installs its interrupt hook after `wasm_instantiate()` returns, so the start function runs before the hook is attached. Workers with an embedded content security policy omit WebAssembly installation while the policy’s WebAssembly code-generation gate remains unimplemented, as recorded in `c/apps/gui/browser/js/workers/worker.c`.

## Test coverage

The harnesses in the repository define the following checks. Test execution for this review: not run.

| Target | Coverage |
|---|---|
| `test-wasm` | Binary structure, type validation, optional specification corpus and interrupt checks |
| `test-wasm-exec` | Numeric results, traps, memory, tables and instruction-boundary checks |
| `test-wasm-js` | JavaScript API behavior, conversions, imported callbacks, memory buffers and realm ownership |
| `test-wasm-realms` | Installation and reset across contexts and runtimes |
| `test-wasm-lifecycle` | Production page close, reopen and retained-resource cleanup |
| `test-wasm-page` | QEMU browser fetching modules and checking executed results |

`tests/wasm.mk`, `tests/wasm_realms.mk` and `tests/wasm_lifecycle.mk` attach negative controls as prerequisites of their positive targets. The controls alter integer decoding, stack validation, immediate consumption, memory synchronization, polling, installation or teardown. The QEMU control serves a truncated module and checks its rejection.

## Source map

| File | What it contains |
|---|---|
| `c/lib/wasm/wasm.h` | Module records, arena interface, loading API and error classes |
| `c/lib/wasm/wasm_int.h` | Bounded readers, instruction walker and observer interface |
| `c/lib/wasm/wasm_parse.c` | Allocation, binary decoding, names, sections and index spaces |
| `c/lib/wasm/wasm_valid.c` | Instruction syntax, stack typing and module validation |
| `c/lib/wasm/wasm_exec.h` | Values, imports, traps, instance accessors and interrupt API |
| `c/lib/wasm/wasm_exec.c` | Instantiation, interpretation, memory growth, tables and polling |
| `c/apps/gui/browser/js/wasm/wasm.c` | Native bindings, allocation, ownership and buffer synchronization |
| `c/apps/gui/browser/js/wasm/wasm.h` | Installation and reset contract |
| `c/apps/gui/browser/js/wasm/wasm_prelude.inc` | JavaScript constructors, imports, Promises and error mapping |
| `c/apps/gui/browser/js/runtime/page.c` | Page installation and teardown hooks |
| `c/apps/gui/browser/js/workers/worker.c` | Worker installation policy and cleanup |
| `tests/wasm.mk` | Core, binding, interrupt and QEMU test rules |
| `tests/wasm_realms.mk` | Realm ownership tests and negative controls |
| `tests/wasm_lifecycle.mk` | Page lifecycle tests and negative controls |
| `tests/unit/wasm_test.c` | Binary structure and validation assertions |
| `tests/unit/wasm_exec_test.c` | Execution results, traps and sign-extension assertions |
| `tests/unit/wasm_interrupt_test.c` | Poll cadence and interruption assertions |
| `tests/unit/wasm_js_test.c` | JavaScript API and imported-memory assertions |
| `tests/unit/wasm_realms_test.c` | Context and runtime ownership assertions |
| `tests/unit/wasm_lifecycle_test.c` | Production page lifecycle assertions |
| `tests/qmp/qmp_wasm_page.py` | Browser execution checks in QEMU |

## See also

- [Libraries](overview.md)
- [Workers, streams and WebAssembly](../browser/workers-streams-wasm.md)
- [The JavaScript runtime](../browser/javascript-runtime.md)
- [Testing the browser](../browser/testing.md)
- [Browser security model](../browser/security-model.md)
