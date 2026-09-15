# XHR callback isolation and terminal progress

The existing XHR comment correctly said responseText arrived incrementally, but
that did not guarantee the request could finish. Its direct callback calls let
a `readystatechange` exception reject the Fetch continuation and become a
fabricated network error (status 0). A progress exception rejected an ignored
inner Promise while leaving the outer body Promise pending. A load exception
could run the error path after success. These are ordinary API defects; this
local evidence does not identify the cause of DS's real chat failure.

`js_webapi.c` now isolates each event property callback and registered listener.
It reports callback exceptions through `reportError` when available, with a
console fallback, and continues subsequent listeners. Listener objects with
`handleEvent` are supported on this path. Reporting cannot itself interrupt
request processing. The exception boundary follows the [DOM listener invocation
algorithm](https://dom.spec.whatwg.org/#concept-event-listener-inner-invoke).

The previous implementation updated responseText when TextDecoder flushed at
EOF, then fired load without a final progress event. A progress-only consumer
could miss the final replacement character for incomplete UTF-8. It now fires
terminal progress after assigning the final text and before DONE, load and
loadend, including empty responses. Received byte count and known Content-Length
are supplied to progress/load/loadend. Abort during the terminal progress callback
prevents this request from subsequently taking its success path. The terminal
progress ordering is based on [XHR response end-of-body
steps](https://xhr.spec.whatwg.org/#handle-response-end-of-body).

## Verification

Run `make BUILD=build-xhr-progress-fix -j3 test-xhr-progress-san`.
The permanent fixture uses real QuickJS, `js_webapi.c`, the HTTP/1 parser and
the stream bridge with an in-memory transport. The server releases headers,
then separate UTF-8 byte fragments while keeping its response unfinished.
Assertions run between releases, so this is not a buffered-response imitation
or a replacement Fetch object.

- Current: 21 checks, 0 failures; ASan/UBSan: 21 checks, 0 failures.
- `XHR_CALLBACKS_PROPAGATE`: exact 6 failures for callback errors becoming
  network errors, preventing later callbacks, stranding progress completion,
  or interrupting terminal dispatch.
- `XHR_NO_FINAL_PROGRESS`: exact 3 failures for final decoder text, empty-body
  terminal progress, and abort from that progress event. Both controls are
  required prerequisites of the positive gate.
- Actual truncated HTTP body still reports an error with no load and one
  loadend. Abort closes the transport and produces one abort/loadend sequence.
- Related `test-stream`: 62 checks pass; `test-stream-control`: 32 pass,
  proving buffer-until-complete does not provide early chunks.
- Related `test-webapi`: 227 checks pass. Its prerequisites also ran the
  separately owned XHR constants regression: 15 pass, old constructor-only
  control has its expected 9 failures.
- `test-mk-wired`: pass (278 fragments, 277 reachable, 1 declared); scoped
  `git diff --check`: pass.

Logs: `build-xhr-progress-fix/acceptance.log`,
`build-xhr-progress-fix/final-gate.log`, and the two
`build-xhr-progress-fix/xhr-progress/old-*.log` files. The first run had 18
assertions; the final 21 add empty-body, abort-inside-final-progress and
throwing-network-error-listener cases. Production code stayed frozen during
those test additions for the root agent's combined browser build.

## Boundary

This patch leaves the existing first-entry-only LOADING readystatechange
notification behavior, absence of 50 ms event throttling, incomplete EventTarget
and ProgressEvent interfaces, timeout handling, repeated open/send lifecycle,
and unsupported XHR response types outside this change. It is not a claim of
full XHR conformance. Chunk decoding here is UTF-8 and the progress byte-count
cases use uncompressed responses; compressed-length semantics were not verified.

No DS request, account action, user VM input, or live disk access was made for
this subtask. Parent and sibling agents own the combined guest verification,
XHR constants and response metadata diagnostics. Real-site recovery needs
separate evidence.
