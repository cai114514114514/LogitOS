# Blob Worker lifecycle — 2026-09-11

Ordinary creator-local classic Blob Workers now exchange real messages. The old
entry path saw the `://` inside `blob:http://...` and sent it through the HTTP
URL parser; no worker was created. The initial host reproduction retained a
passing ordinary HTTP echo control and failed 8 of 11 Blob checks. This is a
browser compatibility fix demonstrated with local echo and arithmetic scripts.

## Source ownership and lifetime

`js_webapi.c` retains two private object-URL hooks when installing the page
prelude. They configure the captured document origin and a process-wide page
generation, and resolve only entries in that page's existing Blob table. The
new `js_webapi_blob_snapshot` API checks the owning JSContext and copies bytes
into bounded native storage; no JSValue crosses a QuickJS runtime boundary.
Fetch and Worker therefore read the same table. Page generations prevent a
new page's first URL from reusing an earlier page's first URL in this process.

`js_worker.c` recognizes Blob URLs before the ordinary HTTP path. Construction
pins at most 8 MiB of source plus the creator origin before queuing startup.
Revoking the URL immediately afterward cannot delete the already accepted
script. A URL revoked before construction, or an unowned URL, delivers a
deferred error without executing source. Startup transfers the owned copy;
normal completion, termination, and page closure all free it. The direct
constructor-to-page-close path has explicit allocation accounting because it
does not pass through the normal worker reaper.

The constructor also checks whether its startup task was actually queued.
Injected allocation failure now throws and frees the source and slot; it
cannot hand the page a permanently starting worker with no task.

Worker location now exposes the captured href and origin as read-only values
and stringifies to href, allowing the existing URL implementation to accept it
as a base. Worker initialization calls the separate pure URL and encoding
installers described in [Worker pure globals](worker-pure-globals-2026-09-11.md).
It does not install the singleton page WebAPI in a worker. Existing classic
script loading/import policy is retained; the fixture's imported script is
ordinary same-origin local arithmetic.

The relevant standards are the [HTML Worker constructor and processing
model](https://html.spec.whatwg.org/multipage/workers.html#dom-worker) and the
[File API Blob URL entry model](https://w3c.github.io/FileAPI/#blob-url-entry).
These links define the behavior being implemented, not a claim of full
conformance.

## Host evidence

`make BUILD=build-ds-flex-fix test-worker-blob-asan test-mk-wired` passes:

| Gate | Result |
| --- | --- |
| Current real implementation | 15 checks, 0 failures |
| ASan/UBSan | 15 checks, 0 failures |
| Startup task allocation failure injection | 2 checks, 0 failures |
| `JS_WORKER_NO_BLOB` prerequisite old-entry control | 15 checks, 8 failures; exit 1 |
| Make reachability | 284 fragments, 283 reachable, 1 declared omission |

Artifacts are in `build-ds-flex-fix/worker-blob/`: `before.log`, `old.log`,
`final.log`. `tests/unit/worker_blob_test.c` links the production worker and
WebAPI through the existing Worker fixture. Its source-allocation audit
observes actual frees, including close-before-start and repeated constructor
OOM; it does not infer cleanup from zeroed registry flags. Darwin sanitizer
leak detection is disabled, so that instrumentation is a separate bounded
accounting check, not a claim of complete process leak detection.

The independently run existing Worker regression passes 28/28, pure globals
passes 21/21 (with its old control failing 13), and URL binding passes 70/70.
Those artifacts and the older terminate-control cleanup caveat are recorded
in the pure-globals report.

## Real isolated guest comparison

The reusable driver is `tests/qmp/worker_blob_guest.py`. Its only reachable
HTTP endpoint is a local server, exposed through explicit QEMU guest forwarding
with `restrict=on`. A real confirmed mouse click at `(238,578)` starts ordinary
scripts. No geometry-query script is injected; one unique color anchor locates
the fixture and eight status pixels corroborate the script's report.

The current guest uses a private cloned copy of
`build-ds-first-answer/snapshot-worker`, with browser SHA-256
`4ae8148379b76e8988721ccf499b664644a7bca4917c56f8157c860129778017`.
The old guest uses the already frozen `build-ds-render-fixed/snapshot-xhr`,
which has the earlier XHR fixes but predates this Worker work. Both use
`-snapshot`; input ISO/disk hashes remained unchanged and both QEMU processes
were terminated by their driver.

| Ordinary fixture | Current | Old |
| --- | --- | --- |
| HTTP Worker positive control | 42 | 42 |
| Blob parent-to-child-to-parent message | 42 | Constructor SyntaxError |
| Blob href, creator origin, isolated no-DOM global | Pass | No worker |
| URL/URLSearchParams/encoding/location stringification | Pass | URL missing |
| Blob imports local arithmetic script | 42 | Constructor SyntaxError |
| Revoked before construction | One deferred error | Constructor SyntaxError |
| Unowned Blob URL | One deferred error | Constructor SyntaxError |
| Terminate before start | Constructed, no message | Construction failed |
| Result and pixels | 8/8, eight green | 1/8, one green and seven red |

Neither report used its forced timeout. The current server received only `/`,
`/echo.js`, `/pure.js`, `/math.js`; the old server received only the first three.
The unchanged fixture hash is
`fbef0fb66dfcd8c4032165a7a885d0cde441ddc7b4d943266d9ac7a9a099bccb`.

Evidence:

- `build-ds-first-answer/worker-guest-current/{results.json,serial.log,before.png,after.png}`
- `build-ds-first-answer/worker-guest-old-rerun/{results.json,serial.log,before.png,after.png}`

The original `worker-guest-old` attempt is retained as **apparatus failure**,
not negative evidence. The asynchronous boot Finder launch landed after the
driver's Browser-launch mark, so the launch checker incorrectly classified it
as a wrong app even though Browser subsequently launched. The driver now waits
for the named Finder boot event before marking and confirming its own dock
click. The valid rerun above reaches the same fixture and preserves the
working HTTP control.

## Boundaries

This covers creator-local classic dedicated Workers in the current single
active page WebAPI realm. It does not add child-document script realms,
module/nested Worker support, transferables, a complete WorkerLocation IDL,
Worker fetch, or Worker WebAssembly. Blob URL generations here are scoped to
the browser process, not persistent globally unique URL identifiers. No
third-party worker, account, challenge, or site-specific algorithm ran in this
test. These results establish the ordinary Blob startup and lifetime behavior;
they do not establish completion of a real site's later asynchronous work.
