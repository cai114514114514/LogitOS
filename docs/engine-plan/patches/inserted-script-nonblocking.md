# Keep native input live while inserted scripts fetch

Implementation handoff, 2026-09-09. Production was inspected read-only. No build was run and no transport files were edited. `browser_rt.c` / `bfetch.h` remain externally owned.

## Correct the diagnosis before changing the caller

The parent reports a Bing home page painted in 3.28 seconds but subsequent native input still stalled behind late scripts. That is guest evidence supplied by the parent, not a new measurement performed by this audit.

The current direct classic-script blocker is `browser.c:run_pending_inserted_scripts`, not `res_fetch`:

```c
int fd = bfetch_start(urlbuf);
while (bfetch_state(fd) == BF_PENDING) bfetch_pump();
```

This loop does not return to `app_main`, call `poll_event`, call the progress tick, or yield. A nonblocking network primitive inside an unbounded wait loop is still a blocking UI consumer. The 60-second request attempt deadline is not an input-latency budget. Adding `load_tick()` is insufficient: that function drains and discards every native event except close. Pumping the complete browser event loop recursively is worse: navigation could destroy the runtime/document beneath the active caller.

The older diagnosis mentioning `bfetch_prefetch_wait` / `res_fetch` remains relevant to the **second** blocker: evaluating a module may synchronously fetch its dependency graph through `js_module.c`. Fixing external classic fetch does not make module graph evaluation asynchronous. Initial document/style/parser-script fetch batches also remain synchronous. Do not claim this patch fixes those separate boundaries.

## Existing structures and integration points

- `g_pending` is a 64-entry raw-node FIFO. The DOM sink only enqueues; the drain removes, marks done, reads type/src, fetches, evaluates, then dispatches a classic resource event. A callback can append another script and the same drain executes it immediately. The current guard permits up to 256 dequeues per call.
- The drain is called after parser scripts in `load_once`, after `js_page_run_due` in the timer branch, and after delayed lifecycle/scroll consumers near the end of `app_main`.
- `finish_page_load` checks only `g_img_owed`. Synchronous draining currently hides the missing dynamic-script load dependency. Once the drain yields, leaving this check unchanged makes window load fire before a pending inserted script and its chained loader.
- `wait_idle` has wake budgets for JS work and image work, not a browser-owned inserted-script request. A new pending request must be a wake source even with no timer, fetch API request, or pending image.
- `load_once` dispatches pagehide and closes the old JS runtime before freeing DOM storage. `tab_dehydrate` independently closes the runtime/DOM. Merely zeroing `g_pending_n` at the next `js_page_open` would leak requests and retain stale node references.
- Existing bfetch APIs are sufficient: start/start_from, state, status, URL, pump, take and release. `bfetch_take` consumes the request and transfers body ownership; do not release the same request again. Prefetch has no nonblocking cache-ready/take interface; replacing the direct loop with prefetch plus wait simply moves the stall.

## Smallest safe state machine

Implement in `browser.c` first, with **one outstanding external script fetch and the existing FIFO execution policy**. This removes network waiting from the event loop without simultaneously changing script ordering. It preserves explicit `async=false` insertion order. It does not claim newly implemented default-async completion ordering or parallel dynamic downloads: the present production script binding has no native force-async scheduling metadata. A later parallel scheduler must distinguish the HTML force-async flag from presence of the async attribute; `script.async=false` cannot be inferred from an absent attribute.

Replace raw entries with records containing:

```text
page_epoch, insertion_sequence
owned node wrapper (JSValue), originating context
prepared kind (classic/module/import map), external flag
owned resolved request URL or copied inline source
request_id = -1, state = QUEUED / FETCHING / READY / FAILED
owned bytes, length, final response URL, failure metadata
load-blocking ownership flag (if explicit accounting is used)
```

Use the existing safe handle doors: `js_dom_node_value(ctx,n)` returns the canonical wrapper, and `js_dom_node_from(value)` resolves it through the wrapper's invalidated/serial-checked handle. Hold an owned value until retirement. Check the page epoch before using the context or wrapper. A raw pointer plus a serial is insufficient after an entire arena is freed; a connected-DOM scan also incorrectly drops an otherwise live detached script. A script removed after preparation should not be silently treated as never inserted. Explicit destruction can invalidate its wrapper; skip dispatch to an invalid target and retire safely. Do not invent a callback target or let node-slot reuse redirect the event.

Prepare without evaluating JS: classify type, snapshot the source and committed document base, allocate ownership, then mark the node prepared/run-once. Snapshotting prevents an intervening native turn or script changing src/text from silently changing an already admitted request. Queue-full/OOM handling must not stamp a successfully admitted state unless its entry exists. Preserve the current external-empty-src error, MIME classification, nomodule, HTML-body rejection and refusal behavior. Reading arbitrary JS properties in the insertion sink can call getters; do not use it as a shortcut to async metadata.

Suggested functions:

```text
pending_scripts_enqueue(node)       prepare and retain, never evaluate or wait
pending_scripts_step()              bounded progress; returns changed/evaluated
pending_scripts_have_work()         includes queued and in-flight records
pending_scripts_reset()             release IDs/bytes/wrappers before JS teardown
```

One outer-loop step:

1. Advance network once when there is script work, then inspect the head. Do not loop until a request changes state.
2. QUEUED external: start with its owned absolute URL. On successful admission store the ID and return. On invalid URL fail asynchronously through the retirement path. A valid request which cannot allocate a table slot remains queued with a bounded admission age; `bfetch_start` conflates invalid input and capacity, so validate the URL separately. Do not start a tight retry loop or reset its admission clock on every turn.
3. FETCHING: if pending, return immediately. If complete, copy the final URL before take/release; detach request ownership exactly once. Keep empty successful responses successful. Failed transfer/HTTP response becomes FAILED.
4. READY/FAILED: remove the entry into a local owned record **before** calling JS or dispatching an event, so callback insertions cannot overwrite the active record. Evaluate at most one script per step, using the existing CPU/fuel budget. For classic scripts dispatch load after execution even when execution throws; dispatch error for resource failure or rejected HTML body; inline classics have no resource load event. Do not manufacture module load for an evaluation promise that has not settled.
5. Keep load-blocking ownership through the resource callback. Its onload can append the next script. Only after the callback unwinds should the record retire. Recheck page epoch / pending navigation before doing another script task; never continue a stale document's chain after navigation is scheduled.
6. Return an effect flag for settlement/paint. A still-pending network request alone is not a reason to restyle or repaint every 10 ms.

With one fetch in flight FIFO inherently prevents a ready later `async=false` script overtaking an earlier one. The initial step can prepare/start during `load_once`; it must return without waiting. Keep parser-script semantics unchanged. For a future parallel version, use a sequence-ordered non-async lane plus a ready async lane, cap active IDs, and never interpret completion order as insertion order.

## Callers, lifecycle and wake accounting

Centralize the per-turn step after native event dispatch and JS/lifecycle callbacks have unwound. The existing two frame-loop drain sites must not accidentally each run an unbounded batch. Call the bounded step from the initial load path to start work, then allow `load_once` to return. The outer loop is the place that processes actual keyboard/mouse input and can safely navigate.

Add a pending-script load dependency to `finish_page_load`: it must not fire while an admitted load-blocking script record remains. The simplest bounded FIFO patch can conservatively check outstanding inserted-script work while the document's load is still pending; once the single window load has fired, late scripts do not reopen it. Explicit accounting is preferable if preparation time or module semantics are expanded: initial dynamic scripts are prepared before `g_load_event_pending` is set, so do not decide their ownership solely by that flag's current value. Initialize a document loading phase at navigation start. A script.onload that inserts another script must not produce a gap in the outstanding dependency. Window load callbacks may insert resources but must not cause a second window load.

Add script work to the `BROWSER_PUMP_MS` wake cap, independently of `js_page_pending` and `g_img_owed`. A local READY or newly queued executable task can continue on the next outer turn; a FETCHING/capacity-blocked task should wait for native input or the bounded pump interval, not busy-spin. No global `wait_idle(0)` with script work outstanding.

Call `pending_scripts_reset` after old-page pagehide dispatch but before `js_page_close` in `load_once`, and before runtime teardown in `tab_dehydrate`. Disable the old sink/advance epoch as part of teardown. This catches scripts enqueued by pagehide as well as previously running requests. Free retained JS values while their context is valid, release only IDs owned by this queue, clear owned buffers and load counters. Do not call `bfetch_close_all` as a substitute: unrelated transport ownership must remain with its existing consumer. Close and exceptional exit paths should share reset as appropriate. A tab return re-prepares from retained document bytes; do not transfer an in-flight request to a new runtime merely because the tab index is reused.

Keep existing browser lifecycle ordering: after a script or resource callback schedules navigation, consume it only outside JS dispatch. A READY script from an old epoch must not execute after a user clicks another page or closes the tab.

## Actual app_main slow-resource gate and watched control

Derive sources from `SCRIPT_RESOURCE_SRC`, replacing only the test driver and fake transport fixture. Keep actual browser.c, DOM bindings, forms, layout, painter, timers and native event routing. Extend a private fake transport wrapper with a held response; do not modify production networking for the test.

The fixture should have a visible input and a timer inserting `/slow.js`. The transport logs the true absolute request and holds BF_PENDING. It advances a deterministic virtual clock and enqueues native mouse/key events while held, but it **must not dispatch them, call loader_poll_hook recursively, mutate the input, or run JS itself**. Native input reaches app_main only when the production consumer returns. Release the response after the real input observer sees typed text, with a bounded virtual-time fallback to prevent the old synchronous loop hanging forever. The fallback is a test failure, not a successful slow response.

Require all of the following:

- The request is still BF_PENDING when the input's real DOM value and newly painted text show the native edit. Assert edits occurred before response release, not merely that queued keystrokes eventually appeared.
- After release, the real external source executes once, its exact node receives load once, and that callback inserts and finishes a second script. Keep the existing empty/throw/404/HTML-body resource-event controls.
- Two inserted `async=false` scripts, with the first held, execute in insertion order. One-in-flight admission is an explicit expected property for the minimal patch, not evidence of parallel async scheduling.
- An initial script-insertion case delays window load until the script and its onload chain finish. A post-load insertion does not fire window load again. With all JS timers/images finished, the held script still progresses through the real outer-loop pump wake path.
- While held, a native relative-link click or Ctrl+T/Ctrl+W cancels the old realm's outstanding work. After delayed completion no old code or resource event runs on the destination, no recycled script node receives the old event, and owned fake request IDs are released. Cover detached-but-live script separately from explicitly destroyed script.
- A macro such as `BROWSER_INSERTED_SCRIPT_SYNC_WAIT` restores the actual old wait loop. Watch it reach the deterministic fallback with input unprocessed and fail the exact `native input painted while script pending` assertion. Make that red control a prerequisite of the positive target; reject unrelated assertions, crashes, build errors and harness timeouts as substitute evidence.

Guest acceptance remains necessary: serve a normal slow classic script over HTTP, type into the visible input before the server releases the body, capture the actual characters, then observe the chained script marker. Measure guest input/paint timestamps. Repeat the Bing specimen only after this controlled production-consumer gate; a local HTTP pass does not establish H2 transport correctness.

## Boundaries to retain beside the patch

This design fixes the external **classic** fetch wait without changing bfetch ownership or raising timeouts. JS CPU work still needs its existing watchdog. A module root made READY by this queue can still block in dependency loading; prefetching only its root does not solve that graph. Full module responsiveness requires graph acquisition before synchronous QuickJS resolution/evaluation, or a dedicated resumable module pipeline. Initial navigation batches and correct force-async IDL scheduling are separately scoped work. Keep those limits explicit in code comments and the final report.


## Integration correction: 2026-09-09

The read-only proposal above is now implemented in browser.c. The head owns a canonical DOM wrapper, request ID, copied preparation data and runtime epoch across outer frames. Reset cancels only owned requests before realm teardown. A pending request causes bounded pump wake rather than recursive event polling or redraw on every wait. Initial load waits for inserted scripts; post-load insertions do not reopen it. The final retirement now rechecks load before parking: review found the first version could otherwise leave load pending with no wake source.

The wired actual-app-main gate has ten passing modes and ten watched old-behavior controls. Nine restore synchronous waiting and include native paint, timers, FIFO/load chains, initial load, no-timer wake, navigation/tab/close cancellation, detached live nodes, explicitly destroyed/reused slots and prepared src mutation. The tenth restores the old load-before-drain ordering and fails at the first indefinite park, before the host no-op wait can rescue it.

The first integrated guest image completed six native cases with unchanged ISO/disk hashes. Its HTTP server held a dynamic script for eight seconds as stimulus. While that body was still pending, native typing painted abc; the guest input marker was 1190 ms after script insertion (this includes driver interaction scheduling and is not a keystroke latency benchmark). The screenshot visibly shows abc and before=true. Source, load and chained-script completion then occurred once. This guest run preceded the final load-before-park correction; the no-timer initial-load guest is tracked separately in the main report.

The associated Bing replay now submitted native Python without waiting for the earlier consecutive 60-second script timeouts, but exposed a separate preexisting unsafe form-submit teardown: a submit listener could synchronously load the destination while its JS stack still existed, then crash freeing the old event objects. That failure is retained with matching AEX and core, and must not be described as search acceptance. The main report tracks the deferred form navigation fix and final replay.
