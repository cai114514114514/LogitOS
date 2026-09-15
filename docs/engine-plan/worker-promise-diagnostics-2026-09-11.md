# Worker Promise diagnostics, 2026-09-11

The previous diagnostic build reported startup, evaluation and synchronous
uncaught exceptions. An async callback can instead reject its returned Promise
while the worker remains running; those existing lines cannot distinguish that
case from a worker waiting for a message. This change observes that boundary in
`JS_RUNTIME_DIAGNOSTICS` builds only. It does not install Worker Web APIs or add
Promise handlers, and makes no claim about a real site's completion.

Each worker runtime receives its own `JS_SetHostPromiseRejectionTracker` and
captures native Error prototypes before any worker script. The hook prints the
local worker id, `promise-rejected-provisional` or `promise-handled`, a fixed
native type, and `missing-pattern`. A provisional rejection may be caught later;
it must not be reported as an established unhandled error. The missing field
only matches the complete QuickJS ReferenceError pattern for twelve standard
globals; it is a text-pattern diagnostic, not proof that a user-created error
describes the actual global state.

The hook does not read Error `name`, `stack`, or constructor properties. It only
reads the own data descriptor of `message` on a native ReferenceError and bounds
the primitive string to fewer than 96 UTF-16 code units before UTF-8 conversion.
Getters, arbitrary objects, Proxy reasons and altered prototypes are not
traversed. It preserves the runtime's preexisting pending exception, and the
existing 256-line per-page diagnostic budget bounds output. No URL, source,
message payload or original exception text is emitted by these new lines.

Two message stages distinguish startup from delivery: `inbound-message-dispatch`
is recorded at the call boundary (both deferred-start and running-worker paths),
and `outbound-message-enqueue` only follows a successful scheduler insertion.
They contain only the id and fixed labels. Runtime shutdown detaches the tracker
and releases that worker's prototype/atom references before freeing its context.

Local verification uses the existing production-linked Worker scheduler with an
in-memory fixture loader. `make BUILD=build-worker-promise-diag
test-worker-promise-diagnostics` passes 10 behavior checks and 12 metadata checks.
Two workers each generate 30 provisional rejections and 29 subsequent handled
notifications; the third only resolves a Promise and emits no rejection lines.
Both early and post-start message paths work, late catch and async callbacks
still run, and all getter/Proxy counters stay zero. Error messages containing a
fixture sentinel, and oversized error strings, never appear in diagnostic lines.

`JS_WORKER_NO_PROMISE_DIAGNOSTICS` removes only the tracker. The same 10 behavior
checks still pass, while exactly 8 named metadata checks fail; message-stage
checks remain green. This control is a prerequisite of the positive gate.
The x86_64-elf worker object also compiled with `RUNTIME_DIAGNOSTICS=1` in this
independent build. Evidence is in
`build-worker-promise-diag/runtime-diagnostics/promise-{on,old,old-check}.log`.
The ASan/UBSan build also passes 10 behavior and 12 metadata checks, with no
sanitizer findings (`promise-san.log`; leak detection is disabled because this
macOS host does not support it). `test-mk-wired` passes with 287 fragments,
286 directly reachable and the one declared standalone wrapper.

These are local synthetic and cross-compilation results. The parent task owns
guest and real-site observation; this subtask sent no external request and
did not operate a VM or user disk.
