# Worker Fetch realm integration — 2026-09-11

Ordinary dedicated Workers now use the same nonblocking Fetch, HTTP, CORS,
credential and redirect implementation as the page. The independent local
fixture first compiled the saved pre-change sources and observed the specific
absence of Worker fetch. The final implementation passes 41/41 host checks
and 41/41 ASan/UBSan checks, including a page plus two workers with concurrent
requests. This establishes local integration; actual guest and real-service
results are separate evidence owned by the root task.

## What changed

The existing JS fetch implementation was extracted into three shared includes:
`js_fetch_body_prelude.inc`, `js_fetch_object_url_prelude.inc`, and
`js_fetch_hooks_prelude.inc`. The page still evaluates its existing surface.
The Worker prelude uses the same Headers, Request, Response, body readers,
streams, Blob and abort implementation, without installing document fonts,
page URL fallbacks, XHR, media queries, navigation or storage. Its native URL
constructor is installed through the existing pure helper. Object URL tables
are private to their evaluating realm.

`js_webapi.c` now registers a fetch owner for each context. Its Response/error
hooks, retry timers, base URL, origin and site-for-cookies are independent.
The native request pool remains bounded and shared, and each request carries
its owner as well as copied origin/site identity. Cookie persistence and the
native preflight cache keep their existing browser scope. No page JS fetch
function proxies a worker request; no synchronous download replaces the
policy path.

For an HTTP Worker, relative requests use its script URL. For a Blob Worker,
the opaque script base remains distinct from the inherited creator origin:
an absolute HTTP request can use the creator's eligible credentials, while a
relative request rejects instead of silently borrowing the page directory.
The native install interface accepts base, origin and site separately; the
current single-page Worker creator supplies its captured origin/site.

Private native bindings also capture an owner generation. Request cancellation
checks the context, owner, slot and request generation, and never assigns a
foreign caller's context to a live request. This additionally prevents a
retained old binding from addressing a replacement owner when a native
embedder closes and reopens a context.

`js_worker.c` services fetch owners on every scheduler pass and drains each
worker's own Promise jobs after network work. Fetch-only activity and retry
timers participate in pending/next-due calculations, so a request does not
need a separate Worker message or timer to make progress. Logical close stops
new work immediately. Destruction closes only that owner's transports and
frees its retained values before freeing the context/runtime.

Parent-side terminate can reap immediately because no child JS call is active.
Worker-side self.close remains deferred until the active call unwinds; it
keeps one cleanup turn alive and prevents later reactions from sending a
parent message. Both ordinary reaping and page-wide close call fetch cleanup.
The separate Wasm owner task's install/reset hooks are wired at these same
lifecycle seams behind `WORKER_NO_WASM_REALM`; its implementation and Wasm
validation are not claimed by this Fetch report.

## Verification

The independent gate's full fixture and control design is documented in
[Worker Fetch host gate](worker-fetch-host-fixture-2026-09-11.md). Startup
scripts are ordinary local strings. Their fetch requests use the production
HTTP parser over an in-memory socket vtable with two synthetic HTTPS origins;
they do not contact an external server.

| Gate | Result |
| --- | --- |
| Worker Fetch final host | 41/41 |
| Worker Fetch final ASan/UBSan | 41/41 |
| Missing Worker fetch control | Exactly 1 named failure |
| Foreign-owner abort control | Exactly 2 named failures |
| Worker network pump disabled | Exactly 3 named failures |
| Existing page WebAPI | 227/227 |
| Existing page streaming | 62/62 |
| Existing browser request context | 23/23; preflight controls still fail as expected |
| Existing Worker regression | 28/28 |
| Pure URL/encoding Worker fixture, independently isolated with `WORKER_NO_FETCH` | 21/21; original control still fails 13 |
| Blob Worker initial combined regression | 15/15; startup-task OOM injection 2/2 |
| Make test reachability | 288 fragments, 287 reachable, 1 declared omission |

Final Fetch evidence is in `build-worker-fetch-fixture/`: the saved source
baseline and `before.log`, `final-gates.log`, and the fresh final-source runs
`final-runtime-current.log` and `final-runtime-san.log`.
`final-source-hashes.txt` records the reviewed product inputs. Those last two
runs explicitly include the final parent-terminate cleanup change.

Page and existing Worker logs are archived in
`build-worker-fetch-impl/evidence/{parent.log,worker.log}`. The initial cleanup
attempt made `js_worker_pending` stay true solely for destruction after a
parent terminate, breaking two existing immediate-quiescence assertions. The
failed run is retained as `worker-before-cleanup-correction.log`. Production
cleanup was corrected at the parent/worker execution boundary; the original
assertions were not weakened, and the final existing suite passes 28/28.

The old terminate/silent-error controls retain their existing nonzero-exit
contract, including the older terminate control's cleanup-abort limitation.
They are not sanitizer evidence. Darwin sanitizer leak detection remains
disabled; the Fetch gate separately checks actual live transport counts,
including page close with a page and worker both holding unfinished bodies.

## Boundaries

This adds fetch to the currently supported classic dedicated Worker model,
which uses separate QuickJS runtimes interleaved on one thread. It does not
add parallel execution, nested/module workers, child-document scripting or a
new network partition model. It reuses the page engine's existing Fetch and
streaming subset and does not establish complete Fetch Standard or WPT
conformance. The focused host fixture checks bodies, ownership and policy;
TCP/TLS, real guest rendering and real-service completion require their own
evidence. No account, challenge, external worker algorithm or real-service
request was used by this implementation subtask.
