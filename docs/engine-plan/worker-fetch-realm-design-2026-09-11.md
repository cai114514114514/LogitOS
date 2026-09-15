# Worker fetch realm design — read-only review, 2026-09-11

The smallest correct integration is an owner-scoped fetch binding over the
existing asynchronous HTTP/CORS/stream implementation. Installing the full
page WebAPI in a worker is not viable: its current install guard correctly
refuses a second live realm. Removing that guard would overwrite page state.
This document proposes interfaces; **no Worker fetch implementation or new
runtime validation was performed in this review**.

Later on the same date, the bounded implementation and its independent host
validation were completed in [Worker Fetch realm integration](worker-fetch-realm-fix-2026-09-11.md).
The original proposal below is retained as the design record, not silently
rewritten into a claim that every proposed future boundary was implemented.

## Existing seams and concrete blockers

| Source seam | Current behavior | Required owner boundary |
| --- | --- | --- |
| `js_webapi.c` `struct wfetch` | Already retains `ctx`, request URL, initiator, redirect/Cookie taint, Promise and stream callbacks | Add an explicit fetch-realm owner; retain the same native transport and policy code |
| `js_fetch_start` | Resolves against `g_loc` and snapshots it as both initiator and Cookie site | Resolve against the calling realm's base and separately snapshot its origin and site-for-cookies |
| `fetch_deliver_headers`, `mk_error` | Call global `g_mk_response`/`g_mk_error` using the supplied context | Keep each callback in its own realm; never call a page JSValue with a worker context |
| `js_fetch_abort` | Checks slot/generation, then assigns `f->ctx = ctx` | Validate calling owner and context as well as handle generation; never change a request's owner |
| `g_timers`, `js_later`, `timers_run` | Timer callbacks have no context/owner and are run/freed with the pump's context | Store timers per realm, including queue-retry callbacks |
| `js_webapi_pump` | Guards on the active page and visits every live request | Filter by explicit owner and call only that owner's callbacks |
| `js_webapi_close` | Releases all fetch slots with the page context | Release only the closing owner's requests/timers/hooks |
| `js_worker_pending`, `next_due`, `run_due` | Observe queued tasks; microtasks drain after startup or Worker task calls | Include fetch-only activity and retry timers; pump and drain the owning worker runtime without requiring a message/timer |

Useful code anchors at review time: `js_webapi.c:790` (request), `:944`
(release), `:975` (error), `:1349` (headers), `:1772` (start), `:1922`
(abort), `:1945` (timers), `:3870` (JS admission queue), `:4776` (install),
`:5124` (close), `:5157` (pending/pump); `js_page.c:760` and `:777`
(separate page and Worker pumps); `js_worker.c` `worker_drain_jobs`,
`mark_dead_ex`, `reap_dead`, and `js_worker_close_all` (worker lifecycle).
Concurrent diagnostic-only edits can move line numbers; the named functions
are the authoritative seams.

## Proposed bounded interface

Use one opaque handle for each fetch-owning realm. The following is a design
sketch, not an existing public header:

```c
struct js_fetch_realm;
struct js_fetch_settings {
    /* Owned copies inside the realm: never borrowed JS strings. */
    const char *base_url;       /* HTTP script URL or opaque Blob script URL */
    struct origin_identity origin;
    struct cookie_site_identity site_for_cookies;
    struct network_partition_identity partition;
};

struct js_fetch_realm *js_fetch_realm_create(
    JSContext *, const struct js_fetch_settings *);
int js_fetch_realm_install(struct js_fetch_realm *);
int js_fetch_realm_pump(struct js_fetch_realm *);
int js_fetch_realm_pending(const struct js_fetch_realm *);
long long js_fetch_realm_next_due(const struct js_fetch_realm *);
void js_fetch_realm_stop(struct js_fetch_realm *);
void js_fetch_realm_destroy(struct js_fetch_realm *);
```

The realm holds its context/runtime identity, immutable client identity,
Response/error constructors, body-stream factory callbacks, private fetch
queue hooks and retry timers. Native start/abort/slot/timer closures capture a
checked realm token with a generation, or look it up by the calling context.
An exposed integer handle or a reusable raw pointer is insufficient. Do not
repurpose JSContext opaque storage without checking its other consumers.

The native `g_fetch` slot pool may remain browser-wide and bounded at its
current aggregate limit, provided every slot has an owner. Slot availability
can share this capacity; queue/abort/timer callbacks remain owner-specific.
A round-robin or bounded per-owner pump prevents a busy worker from consuming
all work in a frame. Keep the injected transport, HTTP codec, Cookie jar and
persistence shared within their existing browser/profile scope. Preflight
cache entries remain native data, keyed by the existing initiator, target URL,
method, headers and credentials; carry the network-partition identity too if
multiple partitions are admitted. A worker install must not clear shared
slots, Cookie state, or the parent's preflight entries.

The page remains a client of this same core. Its public compatibility wrappers
continue handling location/history/storage/viewport as page-only state, and
delegate only fetch work to its realm. The current second-page install refusal
can stay: adding worker fetch does not establish multiple DOM page realms.

## Base URL, origin and credential policy

These are distinct inputs. An HTTP Worker's fetch base is its effective script
URL; a Blob Worker's base remains the opaque Blob script URL. It must not gain
the creator page's relative path by convenience. The origin is the captured
worker origin, inherited from the creator for the supported Blob case, not
`window.location`, mutable `self.origin`, or an origin reparsed from a failed
HTTP interpretation of `blob:`. Do not compare two serialized `"null"` strings
as proof that opaque origins are equal. If the script loader follows redirects,
it must provide the effective script URL rather than letting fetch guess it.

The site-for-cookies comes from the worker's creation environment. It must be
snapshotted separately from the script/base URL and treated as a subresource
request, never a top-level navigation. This matters even with correct CORS:
schemeful SameSite eligibility is a different decision from same-origin
credential mode. The existing one-page model supplies the top-level site; a
future child-document worker must explicitly supply its ancestor-derived
context instead of silently receiving the top page's authority.

Reuse `wf_creds`, preflight classification/cache, response-header exposure,
redirect checks and taint, authorization stripping, inbound/outbound Cookie
filtering, overflow refusal, transport errors, streaming and backpressure.
No body reaches the stream before its response CORS check. Native origin and
site identity remain stable across parent history changes or navigation. Keep
the page's existing method/header/bad-port/URL checks in the shared JS entry
path as well; extracting only `__fetchStart` would bypass checks that currently
live in the JS prelude. This is a reuse requirement, not a claim that every
Fetch Standard policy is already complete in the page implementation.

## JS surface and scheduling

Extract the already shared fetch prerequisites into a realm-safe prelude:
Headers, Request, Response, body readers/ReadableStream, abort machinery, and
the body types they consume. Preserve the existing closure-local queue and
its slot-based admission. Keep document fonts, viewport/media queries,
navigation, storage, page events and XHR-specific additions outside this
worker-only installation. Reuse the pure URL/encoding installers without
overwriting their native constructors. Blob/data URL processing must retain
its actual registry semantics; do not make the parent's Blob table a public
cross-realm fallback. If object URLs are exposed in the worker, their registry
owner and lifetime must be defined explicitly in the same change.

Each `jsworker` owns a fetch-realm handle after creating its JSContext. Every
`js_worker_run_due` iteration pumps live worker realms even if `g_tasks` is
empty, under the existing worker CPU slice. When network/stream operations
queue reactions, drain jobs in that worker's runtime and retain the existing
per-turn job limit. `pending` includes live transfers and queued requests;
`next_due` includes queue timers and a bounded network servicing interval.
Otherwise a plain `fetch(...).then(postMessage)` can start correctly and then
never run again because the page considers the worker idle.

Logical termination first marks the realm stopped, so no new request or
callback can enter it. Retire native requests, sockets, queue timers and
retained JSValues before `JS_FreeContext`/`JS_FreeRuntime`, through **both**
`reap_dead` and the direct `close_all` path. Do not dispatch new JS rejection
callbacks from teardown. If stop is requested during a pump/JS call, prevent
further work immediately and defer destruction until that call unwinds.
Destroying worker A must not abort page or worker B transfers. A stale abort
closure must not cancel a later owner reusing a slot.

## Acceptance before exposure

Use ordinary localhost fixtures with the real shared implementation: page
plus two live worker contexts, different bases, concurrent requests, real
stream chunks before EOF, and a worker with no timers/messages after starting
fetch. Check realm-local Promise/Response/error identity and parent objects
surviving both worker lifetimes. Have the server validate non-sensitive Origin
and credential presence for omit/default/include, preflight allow/refuse,
redirect taint and same-site/cross-site cases. Include cancellation while
queued, before headers and during the body; terminate either worker; page
close; repeated slot reuse; callbacks scheduling close; and allocation failure.

Meaningful negative controls disable owner checks/filtering, worker network
liveness, or owner-scoped cleanup independently and must fail a corresponding
local assertion while the HTTP positive control still works. Run the existing
webapi, stream, browser-context, Cookie-context, Worker and Blob Worker gates,
plus ASan/UBSan and explicit outstanding-request/resource accounting. Finish
with a fresh isolated `-snapshot` guest against only the local server. The
existing Blob Worker guest proves startup and pure globals; it does not prove
Worker fetch, and no such claim is made here.
