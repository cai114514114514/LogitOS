# Workers, streams and WebAssembly

The browser implements dedicated workers with QuickJS runtimes, streams with JavaScript state machines, and WebAssembly with the interpreter under `c/lib/wasm/`. Document scheduling connects worker tasks, Promise jobs, ports and Fetch activity to the browser event loop. API installation and teardown are coordinated by `c/apps/gui/browser/js/runtime/page.c` and `c/apps/gui/browser/js/runtime/webapi.c`.

## Dedicated workers

Each dedicated worker owns a QuickJS runtime and context, and its creator holds the public `Worker` object. The implementation in `c/apps/gui/browser/js/workers/worker.c` manages script loading, globals, timers, messages and termination.

Construction queues startup as a task, so the caller can attach message and error handlers before script loading or evaluation begins. The entry URL is checked against the creator’s scheme, host and port when both URLs parse as HTTP or HTTPS. For a Blob URL, construction captures the source bytes from the creator’s object-URL registry; that snapshot survives revocation before startup.

The worker global scope provides `self`, messaging, timers, `importScripts()`, console output, location information and text encoders and decoders. `importScripts()` resolves URLs against the worker’s URL, loads each script synchronously and evaluates it as a classic script.

The worker Fetch installer creates a request context for that worker and installs the shared Fetch, Body, stream, Blob and FormData implementations. Workers created by the top document also receive WebAssembly bindings. The installation code lives in `c/apps/gui/browser/js/workers/worker.c` and `c/apps/gui/browser/js/runtime/webapi.c`.

Worker execution is cooperative on the browser’s thread. The implementation’s design note ties this choice to two constraints: termination uses QuickJS’s interrupt handler, and shared browser services were built around serial access.

## Scheduling and lifetime

Startup, messages and worker timers use one task queue. `js_worker_pending()`, `js_worker_next_due()` and `js_worker_run_due_for_page()` connect it to document scheduling. Pending checks also inspect each worker’s Promise jobs, port messages and Fetch activity, keeping asynchronous work scheduled when the parent has no timers.

Before it enters another task, a worker resumes its pending Promise checkpoint. Page phases and workers share a turn deadline of eight milliseconds, defined in `c/apps/gui/browser/js/runtime/task_budget.h`. The scheduler checks the deadline between operations and retains queued work across yields. After delivering worker notifications to the parent, the page can service a network checkpoint and return for input and painting before further worker execution. The integration lives in `c/apps/gui/browser/js/runtime/page.c`.

Each worker also has a watchdog using elapsed time and a fuel counter. `terminate()` cancels queued tasks and reaps the worker while execution is in the parent. A worker’s `close()` marks it dead and defers destruction until its execution unwinds.

Cleanup releases ports, Fetch resources, WebAssembly resources and retained JavaScript values before freeing the worker context and runtime. Document shutdown calls `js_worker_close_all()` before freeing the parent context because worker records and pending deliveries retain values from that context.

## Workers in embedded documents

`js_page_context_enable_workers()` gives an embedded document a worker owner that holds stable worker records along with the document’s task queue. Stable storage matters because QuickJS interrupt handlers retain pointers into those records. The owner and policy interfaces are declared in `c/apps/gui/browser/js/workers/worker.h`.

The embedder supplies `struct js_worker_policy` callbacks for creation, imports, connections and string code generation. In `c/apps/gui/browser/frame/passive.c`, those callbacks consult the frame’s Content Security Policy and lifecycle state.

Blob workers inherit the creator’s callbacks. Network workers obtain policy from their entry response. The embedded loader checks redirects, response status, JavaScript MIME type, available response-policy information and resource budgets before returning source bytes. It also checks the response’s CSP sandbox flags.

The final response URL becomes the worker’s base URL. Storage owned by the response supplies policy for later imports, connections and evaluation, and stays alive until worker cleanup invokes its release callback. Frame retirement or policy failure causes subsequent policy checks to reject operations.

## Messages and ports

Worker messages cross runtimes through serialized bytes. `c/apps/gui/browser/js/workers/worker.c` uses QuickJS’s `JS_WriteObject()` and `JS_ReadObject()` with reference tracking, preserving cycles and repeated references within supported serialized graphs. Serialization occurs during the send operation; event delivery is queued.

`c/apps/gui/browser/js/workers/ports.c` implements native `MessageChannel` and `MessagePort` endpoints. An endpoint records its peer, owning context, generation, listeners and queued packets. Packets carry serialized data and identities for transferred endpoints.

Port transfer follows three stages:

1. Preparation validates the transfer list and serializes the data.
2. Commit rechecks ownership and generations, then detaches the sender’s wrappers.
3. Delivery creates wrappers in the receiving context and exposes them through a frozen `MessageEvent.ports` array.

The commit check catches ownership changes caused by author getters during serialization. Discarding a committed packet closes endpoints still in transit, so canceled deliveries release their transferred resources.

Assigning a function to `onmessage` starts a port. A listener added through `addEventListener()` receives queued messages after `start()`. `js_ports_pump()` dispatches a bounded snapshot for its context, with newly posted messages waiting for a later pump. Closing a sender preserves messages already queued at its live peer.

The parent/child window bridge in `c/apps/gui/browser/frame/window.inc` uses the same packet broker. It records the source origin and requested target origin, then checks the destination origin at delivery. A document epoch invalidates queued messages when navigation or removal retires their destination.

`c/apps/gui/browser/js/runtime/platform.c` provides deferred window self-messaging and `BroadcastChannel`. These paths use `structuredClone()` before scheduling delivery. A JavaScript channel implementation in `c/apps/gui/browser/js/workers/message_port.inc` provides a fallback when the native port broker is unavailable.

## Stream state and backpressure

`js_streams_install()` composes readable, writable, transform and pipe support. The modules evaluate JavaScript held in C strings and keep stream, reader, writer and controller records in private `WeakMap` objects.

| Component | Behavior | Implementation |
|---|---|---|
| `ReadableStream` | Chunk queues, reads, pulls, cancellation, iteration and teeing | `c/apps/gui/browser/js/streams/readable.c` |
| `WritableStream` | Ordered sink operations and writer readiness | `c/apps/gui/browser/js/streams/writable.c` |
| `TransformStream` | Readable demand connected to writable progress | `c/apps/gui/browser/js/streams/transform.c` |
| Pipes and queuing strategies | Endpoint lifecycle, count strategies and strategies measured in bytes | `c/apps/gui/browser/js/streams/core.c` |

Readable backpressure uses the strategy’s high-water mark minus the queued strategy size. The default strategy counts chunks. Another counter tracks queued buffer bytes for native Fetch throttling. Application strategies pick their own units, and the network layer measures retained body bytes.

Pulls begin after source startup settles and serialize asynchronous pull callbacks. A close request preserves queued chunks until readers consume them. Cancellation clears the queue and returns a Promise following the source’s cancellation callback.

Writable accounting includes queued writes and the write in progress. Sink operations run in order, with each result settling before the next operation begins. A writer’s `ready` Promise tracks queue pressure; `closed` tracks completion of closing or error processing. Abort forwards its reason through the controller’s `AbortSignal` and waits for the current sink operation before finishing error processing.

The shared queue clears consumed entries and periodically compacts its backing array. This releases chunk references while amortizing the cost of moving remaining entries.

Readable streams also provide asynchronous iteration, `ReadableStream.from()` and `tee()`. Teeing acquires one source reader and feeds two branches; cancellation of both branches forwards their reasons to the source.

## Pipes, Fetch bodies and text

`pipeTo()` acquires a reader and writer, waits for destination readiness and forwards chunks in sequence. Source completion closes the destination, source failure aborts it, and destination failure cancels the source. The `preventClose`, `preventAbort` and `preventCancel` options control those actions. An abort signal starts shutdown with its reason.

While a delivered write is pending and the destination remains writable, shutdown retains the endpoint locks and waits before running cleanup actions. It releases both locks when shutdown finishes. `pipeThrough()` starts the same pipe machinery and returns the supplied pair’s readable endpoint. These rules are implemented in `c/apps/gui/browser/js/streams/core.c`.

Transforms use Promise gates to connect readable demand to writes. A pull releases the write gate; filling the readable queue applies backpressure again. Closing invokes the transformer’s `flush()` callback before requesting readable closure.

Pages and workers install the same stream modules through `c/apps/gui/browser/js/runtime/webapi.c`. The response hooks in `c/apps/gui/browser/js/net/fetch_hooks_prelude.inc` enqueue incoming bytes as `Uint8Array` chunks. Stream cancellation reaches the native Fetch abort handle. Body consumption consults private disturbed state, and acquiring both pipe locks marks the source disturbed before its first read.

Text streams wrap transforms around the encoding classes:

- `TextEncoderStream` produces UTF-8 bytes. It retains a trailing high surrogate across chunks and emits a replacement character for an unmatched surrogate at flush.
- `TextDecoderStream` retains one decoder, decodes chunks with streaming enabled and flushes at completion. Its `encoding`, `fatal` and `ignoreBOM` attributes come from that decoder.

The wrappers are installed from `c/apps/gui/browser/js/streams/transform.c`; decoder state machines are in `c/apps/gui/browser/js/text/encoding_prelude.inc`.

## WebAssembly bindings and execution

`c/apps/gui/browser/js/wasm/wasm_prelude.inc` defines the JavaScript constructors, accessors, Promise methods, import conversion and error classes. `c/apps/gui/browser/js/wasm/wasm.c` supplies native primitives and includes the decoder, validator and interpreter sources directly. Bundling these dependencies with the binding keeps the build’s source list in step across browser and host harness builds.

| Stage | Responsibility | Source |
|---|---|---|
| Decode | Read binary sections, names, types and instruction encodings | `c/lib/wasm/wasm_parse.c` |
| Validate | Check operand and control stacks, indices, types and module conditions | `c/lib/wasm/wasm_valid.c` |
| Instantiate and execute | Resolve imports, initialize memory and tables, run the start function and interpret calls | `c/lib/wasm/wasm_exec.c` |

The library allocates through a `struct wasm_arena` supplied by the caller. Memory accesses use bounds checks, and execution failures return trap codes. The validator and interpreter handle integer sign-extension instructions alongside the MVP instruction set.

The JavaScript namespace exposes `Module`, `Instance`, `Memory` and `Table`, plus numeric `Global` objects and validation, compilation and instantiation methods. Compilation retains a private copy of the input because decoded records contain spans into those bytes.

`compile()` and `instantiate()` perform their work synchronously inside Promise executors, exposing failures as Promise rejections. When `Response` is available during installation, `compileStreaming()` awaits its argument and reads `arrayBuffer()` before constructing a module. `instantiateStreaming()` then constructs an instance.

Decode and validation failures become `CompileError`. Missing imports and imports of the wrong kind produce `LinkError`; execution traps become `RuntimeError`. During exported calls, exceptions thrown by imported JavaScript functions propagate as the original exception. Native argument and result conversion carries `i64` values through BigInt.

## WebAssembly memory and lifetime

A `WebAssembly.Memory` buffer aliases native linear memory. Growth can allocate storage and copy existing bytes. The binding calls `jw_mem_sync()` after exported calls and when accessing `.buffer`; it compares the current address and size with the cached buffer and detaches that buffer when either changes. This covers growth from a module’s memory instruction and growth requested through JavaScript.

Each binding installation owns a `struct jw_realm` containing module, instance, memory and table records. Native functions capture that owner through private function data. Resource tokens connect native allocation lifetime to QuickJS finalizers.

`js_wasm_reset()` retires owners belonging to its context, detaches cached buffers and releases arenas and retained callbacks. Page and worker teardown call it before freeing their contexts. Retained native methods remain bound to their retired owner and reject access after reset. These ownership rules are implemented in `c/apps/gui/browser/js/wasm/wasm.c`.

## Limitations

- Worker tasks share the browser thread, so long synchronous work delays the parent until it returns or reaches an effective interrupt check. Module workers and nested workers raise `NotSupportedError`; `SharedWorker` is absent. Each worker owner has eight slots.
- Embedded documents with opaque origins receive no worker installation. WebAssembly bindings for embedded workers wait on a CSP gate over code generation. The embedded loader refuses entry responses that require Trusted Types.
- Readable byte sources and BYOB readers are rejected, and typed transforms and transferable streams are unimplemented. `structuredClone()` rejects branded streams through the Fetch clone hooks; worker, port and embedded-window packet paths use QuickJS serialization, which carries no stream state.
- Messages sent from a parent to a worker reject nonempty transfer lists. Messages from a worker to its parent, native port messages and embedded-window messages can transfer ports through `MessageEvent.ports`; ports embedded in message data and ArrayBuffer transfer-list entries fail on these paths. Window self-messaging ignores transfer options. The native broker bounds endpoints, packet bytes, queue entries and listeners, and retains endpoint wrappers until close, transfer or context teardown.
- `BroadcastChannel` connects peers with the same name within one platform installation; broadcasting across documents or into workers is unavailable. Several legacy CJK decoder labels use a UTF-8 fallback with a warning, which also affects `TextDecoderStream`.
- WebAssembly rejects bulk memory, SIMD, atomics, reference types and multiple results. The binding omits `Module.customSections()` and `Table.grow()`. Table assignment rejects functions from another instance once an instance has adopted the table.
- Imported WebAssembly globals are copied as values at instantiation. Memory and table import checks compare current sizes with declared minimums, without checking maximum compatibility.
- WebAssembly linear memory is capped at 512 pages, or 32 MiB. JavaScript function bindings support up to 16 parameters, and each owner has 128 slots per native resource kind. Exported calls forward interpreter interrupt polls to QuickJS; the instance start function runs before that hook is attached.

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/js/runtime/page.c` | API installation, worker scheduling and document teardown |
| `c/apps/gui/browser/js/runtime/task_budget.h` | Shared turn deadline |
| `c/apps/gui/browser/js/runtime/webapi.c` | Page and worker Fetch installation and network pumping |
| `c/apps/gui/browser/js/workers/worker.c` | Worker lifecycle, globals, messages, watchdogs and scheduling |
| `c/apps/gui/browser/js/workers/worker.h` | Worker owner and embedder policy interfaces |
| `c/apps/gui/browser/js/workers/ports.c` | Native endpoints, serialized packets, transfer and dispatch |
| `c/apps/gui/browser/js/workers/message_port.inc` | JavaScript channel fallback |
| `c/apps/gui/browser/js/runtime/platform.c` | Window self-messaging and broadcast channels |
| `c/apps/gui/browser/js/runtime/clone.c` | Native structured-clone primitives and installation |
| `c/apps/gui/browser/js/runtime/clone_graph.c` | Structured-clone graph traversal, host hooks and transfer validation |
| `c/apps/gui/browser/frame/passive.c` | Embedded worker admission, loading and response policy |
| `c/apps/gui/browser/frame/window.inc` | Parent/child messaging, origin checks and document epochs |
| `c/apps/gui/browser/js/streams/core.c` | Stream installation, shared queues, strategies and pipes |
| `c/apps/gui/browser/js/streams/readable.c` | Readable queues, readers, iteration and teeing |
| `c/apps/gui/browser/js/streams/writable.c` | Sink operations, writer pressure, closing and abort |
| `c/apps/gui/browser/js/streams/transform.c` | Transform pressure gates and text-stream wrappers |
| `c/apps/gui/browser/js/net/fetch_body_prelude.inc` | Body consumption, disturbed-state checks and text-stream installation |
| `c/apps/gui/browser/js/net/fetch_hooks_prelude.inc` | Response streaming hooks and clone-brand registration |
| `c/apps/gui/browser/js/text/encoding_prelude.inc` | Incremental encoders, decoders and fallback behavior |
| `c/apps/gui/browser/js/wasm/wasm.c` | Native bindings, memory synchronization, resource ownership and interrupt forwarding |
| `c/apps/gui/browser/js/wasm/wasm_prelude.inc` | JavaScript WebAssembly API, import conversion and error mapping |
| `c/lib/wasm/wasm.h` | Module representation and arena interface |
| `c/lib/wasm/wasm_parse.c` | Binary decoder |
| `c/lib/wasm/wasm_valid.c` | Instruction walker and validator |
| `c/lib/wasm/wasm_exec.c` | Instantiation, interpreter, memory growth and traps |

## See also

- [The JavaScript runtime](javascript-runtime.md)
- [Loading pages: fetch, URLs and the HTTP cache](loading-and-fetch.md)
- [Frames, documents and isolation](frames-and-isolation.md)
- [Browser security model](security-model.md)
- [Testing the browser](testing.md)
