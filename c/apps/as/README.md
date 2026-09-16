# AetherScript compiler maintenance map

The original `as` entry point currently contains two language paths. Version 1
has been removed. Version 2 keeps the existing bytecode compiler and VM only
until migration is complete; unversioned legacy source now selects version 2.
All future language development stays on the 3.x line, with compatible minor
releases. The current native declaration is `# aether: 3.0` (`3` is its alias).
An unimplemented minor is rejected. Version 3 is under construction:
it builds a typed syntax tree and lowers the supported subset to LLVM IR.
Do not describe that subset as the complete language or as native self-hosting.

Correction to the earlier temporary A2 policy: the final target is now A3 only.
Every remaining VM/bytecode file and compatibility cache is a cutover blocker,
not a supported final configuration. `test-as-a3-only` preserves the original
public API/example inventory so deleting features cannot satisfy that target.

## Directory layout

The former flat `typed_*.c` collection is organized by compiler stage. Directory
names carry the stage; filenames carry the operation. For example,
`typed_parse_expr.c` is now `frontend/expression.c`, and `typed_llvm_numeric.c`
is now `backend/llvm/numeric.c`. Public C symbols keep their existing names so
this source-layout change does not also change the embedding API.

```text
as/
  cli/           main, commands, structured reports
  include/       public project API
  frontend/      lexer, parser, source snapshots
  sema/          names, types, scope and lifetime checking
  ir/            shared checked-node and type model
  backend/llvm/  native code generation
  runtime/       support linked into generated programs
  common/        diagnostics, numeric rules, system constant manifest
  editor/        completion API consumed by Studio
  legacy/        remaining VM, bytecode and bootstrap migration dependencies
  sources.mk     compiler source groups used by builds and mutation tests
```

`legacy/` is a relocation of existing migration dependencies, not their removal.
The A3-only audit still counts every one of those units as a cutover blocker.
The shared IR is currently a checked syntax tree, not a new lowering pass.

`sema/completion.c` resolves member queries against the same imported modules
as checking. `completion_object.c` adds checked struct/class receivers and uses
the class resolver for inheritance and private methods. The parser records the
original member dot and field declaration tokens; resolution or specialization
must retain those tokens so tools can locate the same source after node rewrites.
Studio consumes these queries asynchronously. Local-name and builtin-container
completion still depend on `editor/`, which remains a migration obligation.

`sema/command.c` checks native Command composition and scoped Process ownership;
`backend/llvm/command.c` lowers those operations. `runtime/command.c` owns managed
descriptions, capture and public methods, while `command_launch.c` owns argv/fd
preparation, child launch and reaping. Keep partial-launch cleanup there: a
failed second fork must terminate and reap the first child before returning.
`test-as-commands` exercises actual children plus injected syscall failures;
`test-as-shell` and `test-ash` run the original shipped shell as native code.

`runtime/port_stats.c` builds the managed statistics snapshot from counters in
`port.c`. It must use the compiler's ordinary string-key hash/equality callbacks
and root the dictionary while inserting entries. Otherwise caller insertions
can change the hash convention, or a collection can reclaim the snapshot while
it is still being built. The stats gate forces both resize and collection.

`sema/pointer.c` resolves the original integer pointer constructors to Ptr[T].
`backend/llvm/pointer.c` emits checked index scaling and address arithmetic.
Ptr is an i64 machine address on the supported 64-bit targets, with no GC root
or object wrapper. Dereferences require lexical unsafe and current CAP_RAW;
raw pointers never imply a safe Region borrow. All indexed loads/stores,
including the captured-place compound assignment path, use alignment 1.
The pointer gate checks real byte layout, signed widening, negative indices,
generic signatures and evaluation order; its controls alter live owned data
and capability checks, without probing arbitrary addresses.

`runtime/allocation.c` owns manual alloc/dealloc memory, separately from the GC.
Its registry validates a release by matching a live allocation base; it never
reads a header through a caller-supplied address. Raw pointers still have no
lifetime token, so address reuse and stale aliases remain unsafe obligations.
The allocation gate instruments actual calloc/free calls, tests refusal and
failure paths, and verifies balanced frees across native return/exception paths.
Do not use this registry as a substitute for Region ownership/borrow checking.

`runtime/region.c` and `region_borrow.c` now provide the native Region ownership
backend. Owners move between empty slots, which clears the source. Shared or
mutable borrow descriptors keep their owner live; nested reborrows suspend a
mutable parent's access. Failed acquisition/release preserves existing state.
The descriptors have explicit cleanup and do not participate in GC. This C
backend is not yet exposed as source-level region(): static move, escape and
loop/exception lifetime analysis still needs to be connected. The A3-only
audit deliberately continues to report region as unresolved.

Correction (2026-09-16): the scoped source-owner subset is now connected.
`with owner = region(n)`, checked byte indexing, `len(owner)` and nested
`with moved = owner.move()` lower to real native ownership slots. The separate
`sema/region_flow.c` pass intersects liveness at joins and computes loop fixed
points; exception edges carry the state at the throwing operation. The LLVM
backend consumes cleanup slots on move and releases them on every scope exit.
Source borrows, Slice/MutSlice interoperability and transfer across calls remain
absent, so the full Region migration audit remains unresolved.

Further correction (2026-09-16): scoped `borrow(start, stop)` and
`borrow_mut(start, stop)` now produce `Slice[u8]` / `MutSlice[u8]`. The lexical
loan pass in `sema/region_borrow.c` checks shared/exclusive access, nested view
suspension and temporary loans across call arguments. Views work with checked
functions/generics/function values, iteration, formatting and owned Bytes
snapshots. Their opaque descriptors remain in native cleanup slots; the view
ABI is still data/length. Conflict diagnostics include the originating loan and both source
identities. A function parameter cannot yet acquire a reborrow descriptor, and
GC-container borrow entry points and returning views with explicit lifetime
contracts remain migration work.

Common owner bindings and aggregate/signature escape checks live in
`sema/resource.c`; `sema/port.c` owns only Port builtin/method checking. This
separation keeps Region analysis independent of fd operations.

`tools/as_examples.py` classifies explicitly versioned shipped examples. Make
builds all A3 entries into `$(BUILD)/as-native/` and installs them under
`/usr/as/bin/`; `/bin/ash` is an alias copy of that same shell artifact.
`test-as-packaged-guest` runs the actual packaged artifacts on a private guest
disk without the compiler or VM, including GUI input and the shell transcript.
It obtains installation paths from `as-native-example-files`, the same mapping
as the production disk recipe. Missing artifacts never select the old engine.

Use a root-qualified include such as `"sema/internal.h"` across compiler
directories. Bare `"internal.h"` is ambiguous under the OS-wide include search.
The standalone completion engine uses local/relative includes so Studio does
not need the compiler's private include paths.

`sources.mk` excludes `runtime/` from compiler links; `runtime/sources.def`
continues to own the native runtime list. Mutation tests query
`make -s as-host-sources` instead of maintaining another source list. When
adding a new compiler directory, register it in `sources.mk` in the same patch.

`runtime/buffer.c` owns the shared fixed byte storage. Mutable `Buffer` and
immutable `Bytes` are separate language types: freezing copies data, while
length/index/membership reads share the storage helpers. Bytes equality compares
contents, including inside Any; Buffer equality keeps object identity. Changes
to this boundary must pass `test-as-buffer` and `test-as-bytes`, whose failure
controls are prerequisites rather than optional CI names.

`frontend/layout.c` checks module-level ABI record declarations; their nominal
type stores constant byte offsets and widths. `backend/llvm/layout.c` lowers
packed scalar accesses with alignment 1 and copies fixed spans to immutable
Bytes. Records share Buffer's nonmoving allocation and byte operations, but
remain distinct nominal types; assignment aliases the same record. The `p`
field kind is a raw u64 address and never keeps its pointee alive. Ordinary
`struct` remains a value type with LLVM-computed layout.

Formatting keeps the layout tag and named fields, including when the record is
inside Any or a container. Metadata distinguishes inline fixed spans from
ordinary Bytes references; interpreting span bytes as a pointer would corrupt
debug output or read unrelated memory. Formatting performs no collection until
publishing the final string, with the original record rooted by the caller.

`test-as-layout` compares all records enumerated from the actual ABI header
against independently compiled C structs, plus source diagnostics, reference
lifetime and observed failing controls. The guest gate also exercises a real
kernel write through `addr(Time())` storage. This does not by itself migrate
the remaining A2 `abi.as` call wrappers or Studio language service.

Correction after that layout milestone: `fsroot/as/lib/abi.as` is now generated
as native A3. `tools/abi_native.py` owns typed marshalling and polling;
`include/abi/logit_calls.abi` remains the single calling-convention inventory.
`sema/storage.c` checks ByteStorage/MutableByteStorage promises before generic
specialization. Strings explicitly acquire a terminated Bytes copy and a root;
byte outputs require mutable storage and validated extents. Raw argv pointees
remain the caller's lifetime responsibility.

`test-as-abi` checks 82 wrapper transports and 21 deterministic polling paths
at O0/O2, including independent packed-word and C-string observations. Its
prerequisite controls break packing, string marshalling, bounds and timeout
comparison and require each failure to be observed. The guest suite executes
real file, clock and wait operations in both build modes. This completes this
ABI module's migration, not the remaining libraries or Studio integration.

`fsroot/as/lib/sys.as` now implements its original service surface in A3 over
that ABI. Its private `_Arguments` object owns both the NUL-terminated Bytes
list and raw argv vector: keeping only the vector would lose the pointees at
a later allocation. Construction happens before fork, and failed exec exits
127. Failed spawn never enters waitpid(-1). Whole-file reads retain their
original capacity bound and expose failure with Optional; text writes encode
explicitly to Bytes. Sleep uses checked monotonic time rather than RTC wrap.

`test-as-sys-lib` executes caller diagnostics, deterministic waits, failed-fork
handling and actual execve argument inspection under forced GC. Its negative
controls corrupt marshalling and remove the failed-pid guard. The guest suite
executes files, directories, cwd, clocks and native children, and also runs the
rewritten original `sysdemo.as` with a complete output oracle.

`image.as` and `gui.as` now use these same native ABI records. Image decoding
keeps the original retry limits, validates returned dimensions, and retains
owned, terminated paths. GUI blits validate the full source byte extent;
length-delimited text and pixel owners survive each synchronous syscall.
The original `guidemo.as` now runs natively with explicit event-width casts.
`test-as-image-lib` and `test-as-gui-lib` observe actual marshalled arguments
under forced collection, with required controls for corrupted fields/bounds.
The guest suite compares six image formats with independent pixel references
and verifies window scanout, visible text, keyboard delivery and process exit.

The original `asview.as` is now a native A3 application too. `make test-asview`
runs both modes with independent image pixels, real keyboard/close input,
directory navigation, clipping and actual kernel capability refusals. Its
Optional directory iteration exposed a frontend ordering bug: the for source
must be evaluated before loop backedge facts are forgotten. Its text wrapping
also exposed stale ABI metadata: measurement packs `(px << 2) | face`, matching
the kernel, rather than the earlier half-size encoding. Required host controls
now independently decode those call words, including mono/bold combinations.

`backend/llvm/roots.c` releases completed statement temporaries. Previously,
six distinct image assignments retained six 5 MiB owners until return, despite
overwriting the source variable. The collector therefore ran correctly but
could not recover those objects. The cleanup walks only the completed subtree,
leaving outer-loop iterables and unfinished call arguments rooted. The required
`test-as-temporary-roots` control disables cleanup and observes excess retained
bytes; its positive fixture also preserves construction and handler roots.

`port(fd)` now uses the same scoped Port type as open. Its wrapper owns no fd:
close and every generated cleanup detach it without consuming the original
owner. Arbitrary fd numbers require CAP_RAW; inherited 0/1/2 remain available.
Borrowed reads never prefetch beyond the requested extent, and line reads one
byte at a time because a pipe cannot rewind. Owned ports retain 4 KiB buffering.

`with reader, writer = pipe()` atomically acquires two native Port owners under
CAP_PROC. The pair is a scoped acquisition, not a copyable List or tuple. The
runtime allocates both wrappers before requesting descriptors; the compiler
registers both cleanups only after success and releases writer before reader.
Read/write direction, EOF, descriptor reuse and partial allocation failures are
tested on real pipes. Native startup ignores SIGPIPE so broken writes become
IOError and execute cleanup. Raw exec retains OS signal inheritance; future
high-level process launchers must restore SIGPIPE in external command children.

The CLI also routes `as FILE ARGS` for explicit A3 sources through its native
run command. This preserves existing launcher contracts and forwards argument
boundaries/exit status. The actual guest platform guard uses freestanding C;
the old AS_SELFHOST_COMPILER guard was absent because that macro only compiled
cli/main.c. Missing host LLVM now produces AS3501 before creating build files.

`runtime/file.c` owns descriptor acquisition, scoped path traversal, transfer
loops and close on every exit. `backend/llvm/file.c` lowers checked arguments
and attaches source locations to runtime status codes. Whole-file operations
use Bytes; they do not yet implement the language's unique resource/with model.
`test-as-files` exercises real files and mocked OS failures, and the guest suite
repeats actual file operations under each kernel capability grant.

Correction (2026-09-16): the preceding limitation describes whole-file helpers.
Native scoped file owners now live in `runtime/port.c`; `sema/port.c` checks
acquisition and rejects copying or escaping an owner. `backend/llvm/port.c`
lowers operations/iteration and `backend/llvm/resource.c` emits reverse-order
cleanup on normal exit, return, loop exits and exception propagation. A try
handler inside a with retains that owner. Closing while another exception is
pending preserves the original exception; ordinary close failure propagates
outside the scope being released. Never route cleanup through an already exited
inner try or retry a consumed descriptor after close reports EINTR.

Refactoring correction: Port builtin/method checks remain in `sema/port.c`;
shared owner binding and escape checks moved to `sema/resource.c` for Region.

`test-as-ports` covers real descriptor reuse, O0/O2 sanitizers and OS fault
injection, including an independently recorded close order. Its required
negative controls remove cleanup, swallow a close error and truncate a write.
The guest suite executes the same scoped file program. Acquisition originally
required `with owner = open(...)`; now `with view = port(fd)` also acquires a
scoped wrapper without ownership of the descriptor. Returning/passing owners, aggregate storage,
borrowed `port(fd)`, pipelines and Region still require migration. These are
diagnosed limitations, not VM fallback paths.

Correction: the preceding borrowed-port limitation is stale; borrowing and
anonymous pipe pairs now have native coverage. Process stages, pipeline
composition, redirection, ownership transfer and Region remain migration work.

`runtime/bytes.c` validates UTF-8 for explicit Bytes.decode; generated text
scanners retain its immutable backing allocation. Its checker and lowering live
in the corresponding sema/bytes.c and backend/llvm/bytes.c modules. Do not turn
str(Bytes) formatting into implicit decoding, or treat legacy byte indexing as
Unicode scalar indexing. `test-as-binary` exercises this boundary and the
multi-file binary project, including observed failure controls.

`frontend/import.c` resolves explicit `std.*` imports and directory components;
the parser records aliases against the original module/member identity. Bare
imports still prefer local source, including unsaved overlays. The stable type
arena in `sema/types.c` includes generic parameters and compound patterns, not
only concrete runtime layouts. Capacity failure must not reinterpret a failed
function's indented body as module-level statements. `test-as-imports` covers
these rules and observes both lookup and recovery mutations failing.

The original `tests/unit/aslexdump.as` developer tool now builds natively from
A3 and reads real files through Bytes.decode. `test-selfhost-lex` no longer
copies a frozen lexer cache; it compares debug/release tool output against the
C frontend. This is native lexer tooling, not completed compiler self-hosting.

## Where a change belongs

| Module | Responsibility |
| --- | --- |
| `cli/main.c` | Existing CLI and language-version dispatch |
| `cli/commands.c` | Native command options, toolchain processes, artifact publication |
| `frontend/parser.c` | Owned source snapshots, imports, types, declarations and statements |
| `frontend/import.c` | Qualified module paths, explicit standard-library roots and overlay-aware shadowing |
| `frontend/support.c`, `frontend/internal.h` | Parser cursors, registered node allocation and module diagnostics |
| `frontend/expression.c` | Expression precedence, postfix operations and rebased interpolation fragments |
| `frontend/text.c` | Formatted string segments and statically checked interpolation nodes |
| `sema/scope.c` | Module/import names, function-wide bindings, global identities |
| `sema/expression.c` | Literal, operator, call and expression typing |
| `sema/check.c` | Statement flow, definite assignment, layout and lifetime checks |
| `sema/resource.c` | Shared scoped owner binding and aggregate/signature escape checks |
| `sema/internal.h` | Internal checker state and scope/expression interfaces |
| `sema/generic.c` | Constraint checks, type substitution, independent concrete instances |
| `sema/types.c` | Stable type arena allocation and one capacity diagnostic per snapshot |
| `sema/callable.c` | Callable signatures, function values, generic callback inference and indirect-call checking |
| `sema/optional.c` | Optional injection, proven local unwraps and presence facts |
| `sema/comprehension.c` | Expression scopes, hidden iteration bindings and filtered element types |
| `sema/class.c` | Class construction, bound methods and initialization/escape checks |
| `sema/inheritance.c` | Class ancestry, exact override signatures and lexical super resolution |
| `sema/exception.c`, `runtime/exception.h` | Exception names, codes and the shared native value layout |
| `backend/llvm/emit.c` | Lower checked nodes to LLVM values, storage and control-flow blocks |
| `backend/llvm/support.c`, `backend/llvm/internal.h` | Shared SSA/label state, native type spelling, roots and failure edges |
| `backend/llvm/numeric.c` | Checked arithmetic, shifts, powers, comparisons and numeric casts |
| `backend/llvm/calls.c` | Resolved function, scalar conversion and text/list builtin calls |
| `backend/llvm/compound.c` | Captured assignment targets, old-value roots and storage lookup after RHS effects |
| `backend/llvm/dict.c` | Concrete dictionary storage, hash/equality callbacks and method lowering |
| `backend/llvm/callable.c` | Native function adapters, indirect calls and shared argument conversion |
| `backend/llvm/sequence.c` | Sequence membership, Any and callable equality |
| `backend/llvm/range.c`, `runtime/range.c` | Lazy range values, full-width counts, indexing and allocation-free direct loops |
| `backend/llvm/loop.c` | Shared iteration, dictionary snapshots and loop control flow |
| `backend/llvm/comprehension.c` | Filtered list construction through the shared iteration path |
| `backend/llvm/optional.c` | Optional payload injection and tag-aware equality |
| `backend/llvm/class.c` | Native object payloads, receiver construction roots and bound adapters |
| `backend/llvm/types.c`, `runtime/type.h` | Native layout metadata, dependency-ordered array aliases and formatting calls |
| `cli/report.c` | Diagnostics, source identities and symbol results |
| `ir/model.h` | Shared tables, node layouts, ownership rules and call-symbol IDs |
| `runtime/sources.def` | Authoritative list of native runtime translation units |
| `runtime/heap.c` | Nonmoving tracing GC, explicit roots and allocation safety points |
| `runtime/allocation.c` | Manual zeroed allocations, live-base release validation and current capability checks |
| `sema/region.c`, `sema/region_flow.c` | Scoped Region acquisition, move checking and control-flow liveness |
| `sema/region_borrow.c` | Lexical and call-argument loan conflicts, with source-linked diagnostics |
| `backend/llvm/region.c`, `backend/llvm/resource.c` | Native owner slots and cleanup on every scope exit |
| `runtime/region.c`, `runtime/region_borrow.c` | Unique byte-region ownership and nested borrow records, released by native scope cleanup |
| `runtime/object.c` | Zero-initialized class payload allocation with a generated field scanner |
| `runtime/list.c`, `runtime/text.c` | Native homogeneous list storage and immutable text operations |
| `runtime/any.c` | Explicit dynamic boxes, exact type checks and concrete payload scanners |
| `runtime/equal.c` | Field-aware boxed equality with an explicit comparison stack |
| `runtime/format.c` | Native layout formatting, recursive containers and growing text output |
| `runtime/dict.c` | Open addressing, tombstones, native key/value scanning and snapshot iteration |
| `runtime/convert.c`, `runtime/integer_parse.h` | Scalar formatting and strict text parsing; shared exact integer parser |
| `runtime/exception.c`, `runtime/print.c` | Pending errors/propagation and scalar/error output |

The public project API is in `include/project.h`. Its construction step copies source
overlays, so an editor may subsequently change its own buffers. A project's
nodes, tokens and diagnostics remain valid until `as_typed_free()`.

## Invariants that are easy to break

- **Source positions belong to a module.** Imported-file diagnostics must use
  that module's source buffer, not the entry source. The legacy diagnostic
  formatter is global; `at_error()` switches it temporarily and restores it.
  That adapter is serialized, not a thread-safe compiler API.
  Interpolation fragments rebase tokens to the original snapshot before freeing
  temporary lexer text. Generated holes must never own a second source identity.
- **Error recovery does not own tree reachability.** The project node registry
  owns every allocated node, including nodes abandoned during parse recovery.
- **Types and bindings are resolved before lowering.** Container types are
  interned. Call symbols distinguish function indices, named built-ins, casts
  and constructors. The backend must not resolve source names again.
- **A branch can terminate.** Definite assignment merges only paths that reach
  the next statement. A loop may execute zero times, so body assignments alone
  cannot initialize a variable after the loop.
- **Comprehension names have expression scope.** The iterable resolves before
  the hidden iteration binding is installed. Filter/element expressions resolve
  it before function locals, globals or imports. Nested scopes restore their
  predecessor and Optional facts; construction roots retain partial outputs.
- **Module initializers are native functions, never checker side effects.**
  Imports establish dependency postorder. Module roots are registered before
  any initializer runs. Loads, including address-based array reads, check an
  initialized flag because an initializer can call a function that reads a
  later global. Function-wide binding collection prevents late local stores
  from accidentally reading an identically named global first.
- **Runtime failure is not a normal return value.** The current native ABI
  records the first error and returns through a failure block. Every language
  call must check that error state before using the returned value.
- **A handler owns its captured error.** The nearest protected region receives
  a failure first. Catch entry clears pending state into an independent stack
  record; rethrows reuse that record. Handler/else failures go to the outer
  region. Definite assignment enters every handler with pre-try facts.
- **Array storage and array values differ.** Field assignment and call-scoped
  slice borrowing use addresses. Value expressions may copy structs/arrays.
- **A partial constructor cannot expose self.** The checker intersects field
  initialization masks across reachable paths. The allocator zeroes payloads
  so GC can safely scan before construction finishes; this does not make an
  uninitialized field legal to read. Bound Callable environments root self.
- **ASan needs native IR attributes.** Hand-emitted LLVM skips Clang's source
  frontend. The test helper marks generated functions `sanitize_address` in a
  private copy before compilation; a sanitizer flag alone only checked the C
  runtime. Negative receiver-root controls verify actual language field loads.
- **Optional narrowing never changes storage layout.** Expression nodes retain
  the value type before injection/unwrapping. A local's declared slot remains
  Optional even when a particular read is proven to have its payload type.
  Collection literals have separate construction roots because their elements
  may collect before the final Optional value is formed.
  Borrow checks walk the full address chain and the full returned value type;
  checking only direct `slice[index] = ...` or a direct Slice return misses
  borrowed fields nested in structs and arrays.
- **Temporary storage belongs to a function invocation.** Node IDs name entry
  block slots. Allocating inside a loop accumulates stack space at O0 even
  after the temporary has stopped being useful. Array LLVM types also have
  names, so deeply nested types never depend on a fixed string buffer fitting
  their recursively expanded spelling.
- **A private signature is still one signature.** Calls can supply missing
  parameter types; reachable returns determine an omitted result. Conflicting
  call types are errors. Recursive unresolved results require annotations.
- **A generic template is checked before instantiation.** Its body can only
  use operations promised by its constraints, including when no caller exists.
  Each concrete argument tuple owns a substituted tree. Register that instance
  before checking its body so same-type recursion reuses it. Reports show the
  declaration once; LLVM only sees concrete instances.
- **Publication follows a successful link.** Copy the completed binary beside
  the destination, flush it, then rename. Never truncate the user's previous
  executable when checking or linking fails. Earlier notes said directory fsync
  was pending; it now follows rename. A directory-sync failure can happen after
  the new name is visible and is reported as a publication failure.

## Editing conventions

- Edit source with patches. Keep each declaration and control-flow operation
  readable on its own; use braces for conditional and loop bodies.
- Use the local `.clang-format` for touched files. Do not reformat the unrelated
  legacy compiler or VM as a side effect of a new feature.
- Give helpers one responsibility. Prefer named enums and explicit branches to
  numeric sentinels or nested conditional expressions.
- Comments explain ownership, failure behavior, algorithmic choices and traps.
  Avoid comments that merely repeat a statement or promise unimplemented work.
- When adding a construct, update parsing, checking, lowering and positive/
  negative examples together. Until every stage supports it, report a specific
  unsupported-feature diagnostic; do not silently route it through the VM.

## Current verification boundary

`make BUILD=build-as-typed test-as-migration` audits the A2 source corpus,
compares both bytecode compilers and runs the portable examples using new
library caches. `test-as-migration-guest` also runs the shipped source compiler,
cached programs and OS-resource examples on a private disk. The inventory and
intentional numeric changes are in `docs/AETHERSCRIPT_A1_TO_A2.md`. Retired A1
fixtures remain negative controls; existing native A3 programs stay native.

`make BUILD=build-as-typed test-as-typed` checks the native subset on the host,
including O0/O2 execution, integer widths, shifts/power, private inference,
import/type identities, source overlays, failure propagation and publication.
Its negative controls deliberately remove assignment merging and generic
constraint enforcement and must fail at the matching assertions. The generic
gate also checks distinct LLVM signatures and instance reuse. `as test` has
observed assertion failures.

`test-as-native-parity` adds catch/rethrow, explicit errors, text operations and
conditional expressions, with an observed uncleared-error negative control.
The remaining A2 feature gaps are tracked in `docs/AETHERSCRIPT_A2_TO_A3.md`;
this growing test suite does not establish complete parity.

`make BUILD=build-as-typed test-as-typed-guest` builds AEX programs and executes
them on a private guest disk containing no AetherScript compiler or VM. Both
debug/release outputs and deliberately failing programs must pass the oracle.
See `docs/AETHERSCRIPT_LANGUAGE_3.md` for behavior, migration and exact limits.

The earlier runtime map named `runtime/native.c`; it has been split into the
units above, with both the driver and tests consuming `sources.def`. Native
frontend linking is independently tested without Value, VM or bytecode files.

The original LLVM map listed one emitter. Its shared generation state, numeric
operations, call lowering and dictionary lowering now have separate units.
Registers, labels, exception handlers and root registration remain shared per
function; splitting modules must not reset any of that state mid-function.

Multiple assignment has separate `frontend/assignment.c`,
`sema/assignment.c` and `backend/llvm/assignment.c` units. Targets participate in
the ordinary lexical binding pass, but initialization facts and native stores
are deferred until all right sides have been checked/evaluated. Extraction must
validate the full sequence length before any store; partial assignment on an
exception would make an apparently atomic swap dependent on evaluation order.

`sema/builtin.c` owns signatures for process/collector introspection.
`sema/constants.c` resolves the target system identities from
`common/system_constants.def`, which also feeds completion. Values come directly from
the LogitOS ABI even when compiling on another host. User bindings take
precedence; builtin constants are i64 values, not context-dependent literals.
Keep the constant manifest and included ABI headers in compiler prerequisites:
otherwise an ABI update can leave yesterday's numbers in native artifacts.

`runtime/process.c` copies launch arguments into fresh managed lists; keep both
the growing list and the not-yet-inserted text rooted across allocations.
`gc_stats` and `gc` count tracked allocations, including native backing objects;
they must never return the byte count from `gc_live_bytes` under another name.

`Buffer` uses `runtime/buffer.c` and `backend/llvm/buffer.c`. Its fixed backing
storage is byte-sized; indexed values remain i64 to preserve the existing
buffer API. Index bounds and value range are separate checks. Do not reuse the
ordinary i64 assignment store: it overwrites the next seven bytes. A failed
compound update must leave the byte unchanged. Buffers are traced references;
their payload contains no GC pointers. Read-only views, unique Region ownership
and raw pointer interoperability are separate migration work still to finish.

Capability values have their own checker, emitter and runtime units:
`sema/capability.c`, `backend/llvm/capability.c` and `runtime/capability.c`.
They are immutable snapshots of the process grant. Only the trusted launcher
installs held authority; `scope` and `without` cannot replace it. Kernel and
language bit assignments differ and require explicit translation at startup.
The optional path is an interior text view of the nonmoving capability, so
text root scanning must keep that allocation alive after the Cap local dies.
Path normalization precedes component-boundary checks. Losing a launcher path
must clear the entire grant. The file/process/raw acquisition adapters still
need to consult this held state as those APIs are migrated.

Raw scalar memory operations now use `sema/memory.c` and
`backend/llvm/memory.c`. The checker carries an unsafe depth per function;
nested function bodies must declare their own unsafe blocks. Lowering emits
width-specific unaligned LLVM loads/stores, with held CAP_RAW checks at each
operation. An integer address neither roots its owner nor carries authority.
The safe Buffer API remains separately bounds-checked and value-checked.
Typed pointer views, manual allocations, unique regions and C ABI calls still
need their migration; scalar peek/poke support does not cover those lifetimes.

Correction (2026-09-16): integer Ptr[T] and manual alloc/dealloc have since
migrated, as described above. Unique Region ownership/borrowing and C ABI calls
remain separate work; raw-pointer registry checks do not establish lifetimes.

Correction (2026-09-16): the raw bridge now includes `mem2str`, `mem2cstr` and
LogitOS `syscall`. The memory checker accepts bounded Buffer/Bytes or a u64
address; `runtime/memory.c` checks known bounds, copies the data and validates
UTF-8. A C-string conversion requires a terminator within its bounded scan,
rather than the old VM's silent 4096-byte truncation. addr also accepts Bytes.

`sema/system.c` checks an i64 syscall number plus up to three i64/u64 words.
`backend/llvm/system.c` evaluates them in order and separates runtime status
from the kernel's signed return value. `runtime/system.c` calls the LogitOS
interrupt only in freestanding x86_64 builds; hosted execution raises
RuntimeError, never a host syscall or a fabricated kernel return. Both new
runtime boundaries recheck held CAP_RAW. The kernel still enforces its resource
categories and scope. This does not implement portable system-library wrappers
or the layout/ownership rules needed for the remaining ABI library.

`test-as-system` runs diagnostic cases, UTF-8/bounds/ownership checks and a
private host transport that observes the exact ABI words. That transport is
test-only; real effects are verified by the guest suite, including six kernel
grants and the migrated original `examples/sys.as`. Its required controls
remove authority/bounds checks and swap argument words, and must fail visibly.

`Dict[K, V]` has native storage and generated Hashable key callbacks. Indexing a
missing key raises KeyError; `get(key, default)` evaluates its default before
looking up the bucket, because that evaluation can resize the dictionary.
Keys/values and `for key in dict` use snapshots. The loop snapshot itself must
remain rooted independently while the body removes/replaces dictionary keys.
One-argument get still requires the pending Optional implementation. Floating
and Any keys, dynamic calls and complete container APIs remain unfinished.

Callable values carry a native entry point and an environment pointer. Plain
module functions use null environments and generated adapters. Signatures
are checked before lowering; every indirect call checks pending exceptions
before its result is consumed. Generic callbacks can use other arguments or
an explicit Callable annotation to infer their concrete signature. Capturing
closures and lambda syntax are still pending, so this is not full closure parity.

Correction: native closures now live in `frontend/closure.c`,
`sema/closure.c`, `backend/llvm/closure.c` and `runtime/closure.c`. Captured locals
share typed heap cells; environments keep cell pointers, never value snapshots.
Root all incoming parameters before allocating cells. Capture discovery uses
the enclosing active checker, including comprehension bindings; that borrowed
checker pointer must be cleared when checking returns. Generic instances clone
nested function trees and resolve captures against the cloned parent.
Captured Optionals need local snapshots before narrowing because other closures
can replace their payload. Captured call-scoped Slice values remain forbidden.

Native classes now support single inheritance. Fields retain their prefix
offsets; a derived-to-base conversion must preserve the concrete allocation
type before changing the static pointer view. A vtable selects overrides even
inside inherited methods. `super` selects the lexical parent while retaining
the same object, including when self is captured by a nested function.

`runtime/class.h` defines the shared C/LLVM header. A constructor's static field
mask does not cover unknown derived fields, so escaping self also checks the
dynamic object's complete mask. This includes bound methods and closures: a
closure may otherwise hide a partially initialized receiver inside its cells.
`super.init` is a direct constructor call, never an escaping callback. Its
successful edge establishes inherited fields; an exception edge does not.
Resolve self by its declaring cell, including through nested captures. A local
with the same spelling is not a receiver; using it for super would apply a
class layout to unrelated storage. Assignment, unpacking and loop binding all
use this same origin check to keep the receiver binding immutable.

Equatable and Ordered constrain generic comparisons. List/Dict equality uses
object identity; Callable equality compares both entry point and environment.
Array/Slice/List membership compares elements without boxing them. Arbitrary
struct equality and dynamic Any comparisons are not implicitly invented.

Earlier notes listed GC/List as absent. Precise roots, homogeneous List and
dynamic text now have native host/guest acceptance. Full class objects,
complete borrow analysis, user-defined protocols, generic structs, exception classes,
complete standard-library support, Studio host-build transport and performance
comparison remain unfinished. Native scalar guest acceptance is now present;
it does not establish resource cleanup or the complete Studio workflow.
