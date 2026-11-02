# The JavaScript runtime

The browser embeds QuickJS and connects it to the document, browser APIs and application event loop. A document’s runtime survives individual script evaluations, retaining listeners, timers and promise reactions until teardown. The embedding contract is in `c/apps/gui/browser/js/runtime/page.h`; `c/apps/gui/browser/js/runtime/page.c` coordinates realm creation, platform installation, execution budgets and cleanup.

## Engine and embedding

The `Makefile` builds the vendored QuickJS sources with musl math routines and the project’s C library support, then links them into the browser. QuickJS supplies language parsing, execution, objects, promises and module machinery.

The engine includes extensions used by the browser embedding:

| Interface | Purpose |
|---|---|
| `JS_EVAL_FLAG_COMPILE_NO_RESOLVE` | Compile a module while postponing dependency resolution. |
| `JS_GetModuleReqEntriesCount()` and `JS_GetModuleReqEntryName()` | Read compiled module dependencies for prefetching. |
| `JS_StallCensus()` | Inspect promise states and suspended async functions. |
| `JS_SetProbeHooks()` | Observe missing property lookups and engine errors in diagnostic builds. |
| `JS_MarkNativeCode()` and `JS_MarkNativeFunction()` | Apply host function representation and operation or constructor shape to platform functions. |

These interfaces are declared in `third_party/quickjs/quickjs.h` and implemented in `third_party/quickjs/quickjs.c`.

`js_page_open()` creates a `JSRuntime` and `JSContext`, installs the execution interrupt handler and sets a 2 MiB stack limit. The configuration in `c/apps/gui/browser/js/runtime/page.c` reserves stack space for constructing an overflow error and its backtrace.

## Realm lifecycle and ownership

The application opens the runtime after creating the document. It opens one for each document, including pages whose markup carries no script elements, so later event handlers and inserted scripts can execute. Navigation cleanup closes the runtime before freeing the document tree. These call sites are in `c/apps/gui/browser/app/main.c`.

The teardown order follows the references held by each subsystem:

| Owner | Retained state |
|---|---|
| QuickJS runtime and context | Objects, compiled code and pending jobs. |
| DOM bindings | Node wrappers and event listeners. |
| Page task queue | Timer and animation callbacks, arguments and deadlines. |
| Installed document services | Fetch resolvers, platform hooks, rejection records and worker references. |

`js_page_close()` first invalidates task ownership, releases subsystem references and disposes queued callbacks. DOM cleanup clears the nodes’ wrapper associations before QuickJS frees the context and runtime. The DOM outlives this sequence because wrappers contain node handles.

Task tokens contain an owner address and an epoch. Opening or invalidating an owner advances its epoch, allowing the acceptance check to reject work retained from an earlier document even when allocation addresses are reused. The queue mechanism lives in `c/apps/gui/browser/app/page.c`.

A close request during an active JavaScript entry records cancellation. The embedder retries teardown after execution unwinds. The interrupt probe provides a checkpoint for window closure while script execution is in progress.

### Additional document contexts

`js_page_context_create()` allocates document state and a queue with a stable address. Before opening the context, the embedder can enable owned Web APIs, platform services and workers through the context configuration functions.

`js_page_context_activate()` selects the context between execution entries. It switches DOM and service ownership, console output, watchdog settings, profiling records and timer dispatch state. The clock is shared across contexts, and each document keeps its performance time origin. These operations are implemented in `c/apps/gui/browser/js/runtime/page.c`.

The earlier statement in `c/apps/gui/browser/js/runtime/page.h` that exactly one page runtime exists sits beside its correction: runtime ownership now applies per selected document context.

## Platform installation

Browser facilities enter the realm through native functions and JavaScript preludes. Installation order follows their dependencies in `c/apps/gui/browser/js/runtime/page.c`:

1. Create core globals, console methods, timers, animation callbacks and `performance.now()`.
2. Install the DOM and bind the window event target.
3. Install document Web APIs using the document URL.
4. Install platform services and the remaining enabled API modules.
5. Apply document sandbox restrictions and interface shape passes.

`js_platform_install()` evaluates a prelude with native helpers passed as arguments. The prelude returns retained hooks for parser lifecycle changes, mutation delivery and rejection notifications. This arrangement lets C invoke platform closures after bootstrap. The implementation is in `c/apps/gui/browser/js/runtime/platform.c`.

At the script phase boundary, the application calls `js_platform_document_parsed()`. Its hook changes `document.readyState` to `interactive` before deferred script execution. Lifecycle listeners record timing markers and change the state to `complete` on document load.

The interface shape pass in `c/apps/gui/browser/js/runtime/idl_shape.c` places supported members on interface prototypes and establishes tags and receiver checks. The following pass in `c/apps/gui/browser/js/runtime/native_shape.c` applies host function names, representations and constructor behavior. Private roots passed between the two stages give the second pass access to values that now sit behind getters.

## Classic scripts and modules

`js_page_eval()` evaluates classic scripts with the global evaluation goal. Its filename supplies diagnostic and dynamic import context; its script node supplies `document.currentScript`. After evaluation, the embedding runs a microtask checkpoint, clears the current script association and ends the execution slice. This order is visible in `c/apps/gui/browser/js/runtime/page.c`.

The application classifies script elements and dispatches them through loading phases. Import maps are processed at their position in the classic script pass, before deferred modules. Inline scripts receive distinct compiler names; `js_module_inline_referrer()` records the corresponding document base URL. See `c/apps/gui/browser/app/main.c`.

### Module loading

`c/apps/gui/browser/js/runtime/module.c` installs QuickJS’s module normalizer and loader. The application calls `js_module_prepare()` before author scripts so classic scripts can also use dynamic imports.

The loader resolves a dependency, retrieves its bytes and compiles it with dependency resolution postponed. It reads the compiler’s requested module entries, prefetches their resolved URLs, then calls `JS_ResolveModule()`. Both root modules and dependencies use this path.

Using compiled dependency records keeps prefetching and linking tied to the same parsed imports. Prefetching consults QuickJS’s module map to reuse compiled dependencies. Resource retrieval uses `res_fetch()` in `c/apps/gui/browser/app/runtime.c`, which consumes a cached prefetch result or enters the fetch path.

The loader sets `import.meta.url` before executing each module body. `js_module_eval()` then evaluates the compiled module and runs pending jobs before inspecting its evaluation promise. Rejection produces a failure result. A pending evaluation promise can resume through later event loop work, including completion of timers or fetches.

### Import maps

`c/apps/gui/browser/js/runtime/importmap.inc` supplies resolution shared by prefetching, static imports and dynamic imports. It supports top-level imports, scoped mappings, exact keys and package prefixes. Resolution selects the most specific matching scope, then the most specific matching key.

Null mappings block resolution. Existing keys win when maps merge, and recorded resolution history pins a mapping once a referrer has used it. Parsing stages entries and commits them after validation, so a rejected map has no effect on the installed one.

Navigation calls `js_module_reset()` to clear import maps, inline referrer records, loader counters and module profiling state.

## Event loop and task budgets

The application loop checks `js_page_pending()`, advances work through `js_page_run_due()`, settles document changes and schedules painting. Before sleeping, it combines the next JavaScript deadline with pending resource work. These connections are in `c/apps/gui/browser/app/main.c`.

`js_page_run_due()` progresses through the following phases:

| Phase | Work |
|---|---|
| Rejection notifications | Run a checkpoint and deliver eligible notifications. |
| Web APIs | Progress fetches and installed document services. |
| Messaging | Deliver port messages and progress WebSockets. |
| Workers | Advance worker work and process page reactions. |
| Animation | Sample CSS and Web Animations state and run reactions. |
| Timers | Dispatch a snapshot of due timers and animation callbacks. |

Each call uses an 8 ms deadline shared across phases and interleaved workers, defined by `JS_TASK_TURN_MS` in `c/apps/gui/browser/js/runtime/task_budget.h`. Checks between phases and callbacks let the application regain control for input and painting.

Dispatch retains its phase and timer snapshot when it yields. Resumed calls also service active fetch exchanges. The comment in `c/apps/gui/browser/js/runtime/page.c` records the reason: a large timer snapshot previously starved network streams while dispatch resumed across application loop passes.

### Microtask checkpoints

`js_dom_run_jobs()` in `c/apps/gui/browser/js/dom/dom.c` calls `JS_ExecutePendingJob()` to run promise reactions and async continuations. It integrates mutation delivery before and during the drain. Script evaluation, timers and other callback paths invoke this checkpoint.

The platform implements `queueMicrotask()` through a resolved promise in `c/apps/gui/browser/js/runtime/platform.c`.

Rejection tracking records promise identity and reason when QuickJS reports a rejection. A later page turn delivers eligible `unhandledrejection` or `rejectionhandled` notifications after queued reactions have had an opportunity to attach handlers. The state machine is in `c/apps/gui/browser/js/runtime/rejections.inc`.

## Timers and animation frames

`setTimeout()`, `setInterval()` and `requestAnimationFrame()` share the document’s task queue. Entries retain callbacks, arguments, deadlines and sequence numbers.

Dispatch snapshots time and sequence. New callbacks and rearmed intervals receive a later sequence, placing them in a subsequent snapshot. Equal deadlines run in sequence order, so a chain of callbacks scheduled for the current time spills into later dispatches. The mechanism is in `c/apps/gui/browser/app/page.c`, with JavaScript adaptation in `c/apps/gui/browser/js/runtime/page.c`.

Window and worker timers share conversion and nesting rules through `c/apps/gui/browser/js/runtime/timers.h`. Delays undergo signed 32-bit conversion; negative results become zero. When the running task’s nesting level exceeds five, delays below 4 ms become 4 ms. Intervals reinitialize after invocation using the current clock. Cancellation marks an executing interval so its callback can cancel its own repeat.

Microtask checkpoints temporarily reset timer nesting and restore the surrounding timer’s level afterward.

Animation callbacks registered before the next boundary share a deadline and timestamp. The scheduler sets that deadline 16 ms ahead. Their timestamps and `performance.now()` use the injected monotonic clock relative to page opening.

## Execution watchdog

The watchdog bounds synchronous entries into QuickJS. Classic script evaluation, module evaluation and timer callbacks bracket execution with slice entry and exit calls.

The defaults in `c/apps/gui/browser/js/runtime/page.c` are 45 seconds of charged elapsed time and 2,000,000 interrupt handler calls. The fuel limit supplies a backstop when an injected clock stays frozen. QuickJS invokes the handler through its interrupt polling mechanism; fuel counts handler invocations.

Module retrieval and prefetch waits use scoped I/O accounting. Their elapsed time extends the current deadline, while computation already charged to the slice keeps counting. Nested waits count overlapping time once, and an epoch ties each completion to its own slice. The slice continues to charge compilation and evaluation.

When a limit expires, the handler interrupts QuickJS, increments the hit count and reports fuel, elapsed time and excluded network wait time. Ending the entry disarms its deadline, so the next execution slice starts with fresh accounting.

## Structured cloning

`structuredClone()` combines graph traversal in `c/apps/gui/browser/js/runtime/clone_graph.c` with native buffer operations in `c/apps/gui/browser/js/runtime/clone.c`.

The walker captures intrinsic methods and native class identities during installation. A visited-object map tracks cycles and repeated references so the clone reproduces both. Supported payloads include objects, arrays, maps, sets, dates, regular expressions, errors, boxed primitives, array buffers and buffer views. Object and array traversal copies enumerable own string properties.

Private brand and serialization hooks identify platform objects that share QuickJS’s generic object class. The Web API layer supplies hooks through `js_webapi_clone_hosts()`, with the bootstrap contract defined in `c/apps/gui/browser/js/runtime/clone.h`.

Buffer transfer proceeds in stages. Preparation validates transfer types and duplicates, allocates receiving buffers and traverses the graph. Commit revalidates every buffer, copies its current bytes and detaches the sources. Performing the final copy after graph traversal captures writes made by author getters; validating all buffers before detachment handles getters that detach a listed source.

## Stall detection

Stall diagnostics expose outstanding waits alongside exceptions and missing API reports.

`JS_StallCensus()` walks QuickJS’s garbage collection object list to count promises by state, pending promises with registered reactions and incomplete async function frames. It copies function names, filenames, line numbers and bytecode offsets when available. The census in `third_party/quickjs/quickjs.c` runs entirely in C and reads engine structures in place.

`js_stall_report()` formats the census and folds matching suspension sites using function, file, line and bytecode offset. Its calling contract requests a settled execution point with an empty JavaScript stack. `js_stall_blocked()` checks for pending reactions, async frames and outstanding operations reported by armed trackers. Both functions live in `c/apps/gui/browser/js/runtime/stall.c`.

An embedder can call `js_stall_arm()` after opening the page and before its first script. Its wrappers attribute fetches, stream reads, XHR, timers, animation callbacks, observers and lifecycle listeners. Fetch waits are grouped by origin. Observer discovery examines constructors whose names end in `Observer` and whose prototypes expose an `observe` method.

`tests/unit/webapi_probe.c` provides armed and census measurement modes.

## DevTools and runtime diagnostics

Console methods and exception reporters write messages to serial output and the page output buffer. Error stacks go to serial output. `js_page_set_note_sink()` observes appended fragments before buffer truncation, allowing an instrument to assemble complete messages. These interfaces are in `c/apps/gui/browser/js/runtime/page.c`.

| Instrument | Information |
|---|---|
| Slice profiler | Entry labels, interrupt polls, elapsed accounting, gaps and watchdog hits. |
| Module profiler | Compilation, prefetch waits, retrieval, linking and evaluation with jobs. |
| Platform probe | Missing platform property lookups and engine-created errors, including caught errors. |
| Runtime diagnostics | XHR outcomes, selected JSON classifications, long jobs, worker budgets and port timing. |
| Installation profiler | Elapsed time for stages of realm setup. |

`js_prof_dump()` reports slice records. The earlier description of `out_ms` as engine idle time is corrected in the source: long gaps at entry boundaries can also be charged there. Module profiling partitions nested loading intervals so a dependency’s retrieval time receives one attribution. See `c/apps/gui/browser/js/runtime/page.c` and `c/apps/gui/browser/js/runtime/module.c`.

With `JS_PROBE_TRACE`, `c/apps/gui/browser/js/runtime/probe.c` identifies platform globals by comparison with a bare QuickJS context and installs engine hooks. Its missing-property hook reads raw prototype links directly from the object chain. Reset reports accumulated event counts and releases tracked objects.

`JS_RUNTIME_DIAGNOSTICS` enables metadata records. `c/apps/gui/browser/js/runtime/runtime_diagnostics.inc` classifies XHR outcomes and selected response JSON fields through a private native hook. Job timing comes from the DOM and worker implementations; port diagnostics measure queue residence and delivery time. `tests/runtime_diagnostics.mk` selects these builds. The `Makefile` maps `JS_INSTALL_PROFILE=1` to `JS_PAGE_INSTALL_PROFILE` for installation timing.

The F12 panel in `c/apps/gui/browser/app/main.c` reads application loading counters, script evaluation results and live module statistics. The recorder implementation in `c/apps/gui/browser/js/runtime/devtools.c` supplies script and network records, resource-index associations and collection-count mismatch checks. `tests/unit/devtools_test.c` exercises that recorder.

## Limitations

- The 8 ms budget checks dispatch boundaries. A synchronous callback or native operation can exceed it; watchdog interruption depends on reaching an engine interrupt checkpoint.
- A page microtask checkpoint stops after 100,000 jobs and emits a diagnostic. Timer and animation callback ownership shares a 4,096-entry capacity.
- String timer handlers raise a `TypeError`; their TrustedScript and content security policy handling remains unimplemented.
- Module loading includes synchronous fetch and prefetch waits. Initial script acquisition uses application parser batches.
- Import maps accept HTTP(S) addresses and reject integrity metadata. Limits include 512 mappings, 2,048 resolution-history pairs, 1 MiB of JSON and 767-byte URLs.
- Additional document contexts enable selected owned services. Optional extensions retain ownership restrictions, and module/import-map state remains in file-level globals.
- Structured clone transfers copy buffer contents. Shared memory and host values without serialization support produce `DataCloneError`.
- Armed stall tracking changes function identities and promise reaction counts. Its settlement handlers also affect rejection handling.
- Diagnostic tables and output buffers have caps. The recorder in `c/apps/gui/browser/js/runtime/devtools.c` has no production callers in the browser; the F12 panel uses application counters.
- The browser has no JavaScript or origin sandbox suitable for adversarial pages, as documented in `TRANSPARENCY.md`.

## See also

- [Browser architecture](overview.md)
- [Loading pages: fetch, URLs and the HTTP cache](loading-and-fetch.md)
- [DOM and event bindings](dom-bindings.md)
- [Frames, documents and isolation](frames-and-isolation.md)
- [Workers, streams and WebAssembly](workers-streams-wasm.md)
- [Testing the browser](testing.md)

## Source map

| File | What it contains |
|---|---|
| `Makefile` | QuickJS compilation, browser linkage and installation profiling selection. |
| `third_party/quickjs/quickjs.h` | Embedding interfaces and browser extensions. |
| `third_party/quickjs/quickjs.c` | Interpreter execution, module extensions, interrupt polling and stall census. |
| `c/apps/gui/browser/js/runtime/page.h` | Page lifecycle and document context contracts. |
| `c/apps/gui/browser/js/runtime/page.c` | Installation, evaluation, timers, watchdog, profiling and teardown. |
| `c/apps/gui/browser/app/page.h` | Task ownership tokens and queue interfaces. |
| `c/apps/gui/browser/app/page.c` | Epoch checks, cancellation and task ordering. |
| `c/apps/gui/browser/app/main.c` | Navigation, script phases, event loop and F12 panel. |
| `c/apps/gui/browser/app/runtime.c` | Resource retrieval and prefetch cache consumption. |
| `c/apps/gui/browser/js/runtime/platform.c` | Platform bootstrap, lifecycle hooks and task helpers. |
| `c/apps/gui/browser/js/runtime/idl_shape.c` | Interface prototypes, member placement and receiver checks. |
| `c/apps/gui/browser/js/runtime/native_shape.c` | Host function classification and representation. |
| `c/apps/gui/browser/js/runtime/module.c` | Module resolution, prefetching, loading, evaluation and profiling. |
| `c/apps/gui/browser/js/runtime/importmap.inc` | Import map parsing, merging and resolution history. |
| `c/apps/gui/browser/js/runtime/task_budget.h` | Shared dispatch budget. |
| `c/apps/gui/browser/js/runtime/timers.h` | Timer conversion, nesting and repeat rules. |
| `c/apps/gui/browser/js/dom/dom.c` | Job checkpoints, mutation delivery and callback exception reporting. |
| `c/apps/gui/browser/js/runtime/rejections.inc` | Deferred rejection notification state. |
| `c/apps/gui/browser/js/runtime/clone.h` | Private host serialization hook contract. |
| `c/apps/gui/browser/js/runtime/clone.c` | Buffer validation, copying and detachment. |
| `c/apps/gui/browser/js/runtime/clone_graph.c` | Structured clone graph traversal. |
| `c/apps/gui/browser/js/runtime/stall.h` | Stall measurement contract and observer effects. |
| `c/apps/gui/browser/js/runtime/stall.c` | Armed trackers, report formatting and blocked-state checks. |
| `c/apps/gui/browser/js/runtime/probe.c` | Platform lookup and engine error instrumentation. |
| `c/apps/gui/browser/js/runtime/devtools.c` | Script and network recorder implementation. |
| `c/apps/gui/browser/js/runtime/webapi.c` | Document services, clone hooks and diagnostic integration. |
| `c/apps/gui/browser/js/runtime/runtime_diagnostics.inc` | XHR and response JSON classification. |
| `c/apps/gui/browser/js/workers/worker.c` | Worker scheduling and execution diagnostics. |
| `c/apps/gui/browser/js/workers/ports.c` | Message delivery and queue timing diagnostics. |
| `tests/runtime_diagnostics.mk` | Runtime diagnostic build selection and fixtures. |
| `tests/unit/webapi_probe.c` | Page probe harness and stall modes. |
| `tests/unit/devtools_test.c` | Recorder behavior and collection mismatch checks. |
| `TRANSPARENCY.md` | Claim rules and browser security limitations. |
