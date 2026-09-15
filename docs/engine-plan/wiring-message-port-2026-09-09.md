# MessagePort clone and endpoint lifetime — 2026-09-09

The previous live MessageChannel implementation captured the sender's `data`
object in a timer closure and delivered that same object later. Its close only
cleared one peer pointer and handlers. A receiver that installed a new handler
after close could still receive an already queued message. Calling start()
also synchronously dispatched held messages.

`js_platform.c` now includes `js_message_port.inc`. The implementation keeps
endpoint state in a private WeakMap, clones at postMessage time, and captures
the shared serializer and page timer functions before page code can replace
the public globals. Each message is queued through the existing page task
consumer, preserving FIFO and the checkpoint between callbacks. start() merely
enables queued asynchronous tasks; it never invokes a listener synchronously.

Closing a destination marks it closed, advances its generation, cancels its
timer handles, drops queued payloads and disentangles the pair. A stale timer
checks the destination generation before delivery. Closing the sender does
not discard a message already queued on a still-live receiver. Navigation uses
the existing page_runtime close path to dispose timer callbacks before their
context is freed, rather than adding another global queue to reset.

The shared serializer already copied cycles, Date, Map, Set, ArrayBuffer and
views. This change adds rejection for Promise, WeakMap/WeakSet/WeakRef, DOM,
MessagePort and SharedArrayBuffer objects, alongside its existing function and
symbol refusals. It copies an own `__proto__` data property using
DefineProperty, avoiding accidental prototype mutation. Non-empty transfer
lists explicitly throw DataCloneError: transfer/detachment is not implemented.
There is a 4096-message per-port queue bound; exhausted storage/task capacity
throws QuotaExceededError instead of reporting successful delivery.

This remains same-page MessageChannel. It does not make iframe WindowProxy or
cross-origin messaging available. The inherited JS structuredClone helper is
also not claimed fully conformant: arbitrary Proxy detection and every host
object's internal-slot semantics need a native serialization/class boundary.
This patch does not guess at those using constructor names or site checks.

Primary reference: [HTML message ports](https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports).

## Evidence

```sh
make BUILD=build test-message-port
make BUILD=build test-message-port-asan
make BUILD=build test-message-port-platform
make BUILD=build test-mk-wired
```

The initial real implementation failed 6 of 21 checks, recorded in
`build/wiring-next/message-port-before.log`. The positive target requires both
controls: MESSAGE_PORT_NO_CLONE must fail sender-mutation isolation;
MESSAGE_PORT_NO_CLOSE_FENCE must fail closed-destination delivery. The controls
respectively produce 3 and 1 assertion failures, rather than depending on a
compiler error or crash. Production checks cover synchronous clone failure,
transfer refusal, page overrides, asynchronous start, FIFO, receiver and sender
close distinctions, own proto-key copying, and navigation disposal. A native
counter (outside the old JS context) observes whether an old-page callback ran.
The final log is `build/wiring-next/message-port-verified.log`.

Guest fixture: `tests/fixtures/engine-expansion/message-port.html`. It uses only
channel tasks for completion, without an auxiliary timeout, and emits
`MESSAGE-PORT-WIRING checks=10 failures=0` on success. Root owns the guest run
and disk integration. Host verification is not presented as guest delivery.

Integration correction: the unified disk run now observes the fixture's ten
checks with zero failures in the actual guest browser. Unified host/ASan reruns
also pass 21 checks; neither control terminates by GC abort. See
[the integration record](general-browser-progress-2026-09-09.md) for artifact
hashes and the boundary around later concurrent edits.
