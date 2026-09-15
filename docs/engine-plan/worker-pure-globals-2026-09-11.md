# Worker pure globals — 2026-09-11

The ordinary, locally supplied Worker fixture confirmed that URL,
URLSearchParams, TextEncoder and TextDecoder were absent. Two workers could
start and report their feature checks; the failure was their global surface,
not lack of message delivery. The initial real implementation failed 13 of 21
checks while parent-page controls passed. No external script or service was
run by this fixture.

`js_url_install_core(ctx)` now installs the existing native URL implementation
without the document binding hook. `js_webapi_install_encoding(ctx)` evaluates
the existing encoding prelude with only its two immutable table-lookup
primitives. It does not call the page's singleton `js_webapi_install`, replace
its retained callbacks, or mutate fetch/location/storage/Cookie state. Worker
initialization calls these pure entry points. Encoder/decoder stream
constructors are left absent in a realm without their TransformStream
dependency; the existing page path still has that dependency.

The fixture covers relative and non-special URL resolution, mutable linked
searchParams, iteration, URL-driven ordinary importScripts, UTF-8 roundtrip,
split decoder input, single-byte legacy decoding, encodeInto capacity, parent
objects and functions surviving two worker lifetimes, and queue drainage.

## Validation and a corrected audit hypothesis

`make BUILD=build-worker-globals-fix test-worker-globals-san test-mk-wired`
passes: 21/21 host and 21/21 ASan/UBSan; the prerequisite
`WORKER_NO_PURE_GLOBALS` control fails exactly 13 checks. It disables only the
new pure installers, leaving Worker creation/message delivery and the parent
controls in place. The test derives its production sources from WORKER_TEST_SRC
and uses the existing in-memory loader, not a duplicate WebAPI implementation.

Evidence is under `build-worker-globals-fix/worker-globals/`:
`baseline.log`, `old-globals.log`, and `gate.log`. The Make fragment is included
and connected to test-worker and ci-host; mk-wired passed with 284 fragments.
The existing URL binding regression passes 70/70. After the Blob-worker owner
reported its final source stable, the original Worker suite passed 28/28 with
its terminate and silent-error negative controls accepted; the same run also
repeated the pure-globals positive/negative checks successfully. These results
are in `url-regression.log` and `worker-regression.log` in the evidence directory.
The older terminate-control target accepts a nonzero exit: it logged the
expected dropped-task/quiescence failures and then aborted during cleanup.
That is the existing control's boundary, not a clean sanitizer run of the
negative binary; the positive 28-check executable exited normally. The new
pure-globals control is stricter: it requires exit 1 and exactly 13 failures.
ASan leak detection is disabled on this Darwin host; ASan/UBSan passing is not
a separate leak-accounting claim. The fixture does not navigate a real site or
establish that a site-specific worker completes.

**Correction retained beside the original hypothesis:** the first static
review suspected that repeated JS_NewClassID calls would replace process-wide
URL class IDs and invalidate parent objects. An explicit old-call-pattern
control passed instead. Inspection of this repository's QuickJS implementation
(`quickjs.c`, JS_NewClassID) showed why: it allocates only when the supplied ID
is zero. The original calls were already idempotent. That hypothesis is
retracted, its ineffective negative target was removed, and the passing
experiment remains in `old-classes.log` as the correction's evidence. The
production change is a pure-installer split and explicit runtime registration
guard, not a claim to have fixed proven URL ClassID corruption.

## WebAssembly ownership review: no Worker installation

This is a bounded, read-only review of js_wasm.c. No WebAssembly production
change, worker installation, site algorithm inspection, or external execution
was performed.

The current class-registration logic is already correct about process-wide
ClassID versus per-runtime registration. Resource ownership is a separate
issue:

* Four process-global arrays hold modules, instances, memories and tables
  (`g_jwmod`, `g_jwinst`, `g_jwmem`, `g_jwtab`). Slots have no owning context or
  runtime. Host callbacks retain an explicit JSContext and JSValue; memory
  slots cache an ArrayBuffer JSValue.
* js_wasm_install initializes every memory slot's cached buffer and instance
  link and every table's instance link, even if another realm has live slots.
  Installing it in a new worker would therefore change existing allocations'
  bookkeeping. It is insufficient to check that the ClassID is registered.
* js_wasm_reset(ctx) iterates all slots, freeing held JSValues with the runtime
  of the supplied context. Its header promises context-scoped cleanup, but the
  implementation currently assumes one active Wasm-owning page. A worker
  reset would not be owner-scoped under this implementation.
* Native tokens contain only kind/index. Native lookups check range/used but
  do not establish realm ownership. Any future independent reset/reuse design
  must also keep an old token finalizer from retiring a newly reused slot.
* `g_host_threw` is process-global execution state. An ownership refactor must
  retain correct exception propagation through nested/imported calls, not
  merely move allocation arrays and leave this flag shared.

The smallest complete future scope is the js_wasm binding's ownership and
lifetime layer, not the interpreter or a site's module. Give each owning realm
an explicit state containing its tables and originating context/runtime; use
that owner for native lookups and JSValue release. Installation must initialize
only new state, reset must retire only that owner's state, and token/finalizer
identity must survive retirement safely (owner plus generation or equivalently
non-reused retired state). Keep execution error state per invocation, with
nested calls restoring their caller's state. A bounded registry or a captured
native owner token can provide this without changing the public JS API.

Only after that layer passes ordinary local tests should worker setup and both
worker teardown paths call install/reset. Suitable controls are a small scalar
Wasm function, a Memory with a retained ArrayBuffer, imported callbacks, two
simultaneously live realms, terminating either realm first, surviving-realm
calls/grow, and repeated teardown/recreation. These tests must cover the real
binding and sanitizer/lifetime accounting. Removing the install initialization
loop alone does not repair reset, finalizer ownership or exception state.

This review is not evidence that the observed DS request reached a Wasm worker
or that Wasm ownership explains its network hint. That requires separate,
non-sensitive runtime stage evidence from the root-controlled page.
