# DS request diagnosis — 2026-09-11

## Current result: first real answer visibly confirmed

At 2026-09-11T10:41:16Z, the running LogitOS QEMU browser visibly displays
**“Hi there! How can I help you today?”**. The same ordinary UI submission
receives HTTP 200 with `payload=event-stream` on fetch 15/XHR 43. This is an
actual server response rendered by the browser, not a synthetic fixture or
inference from HTTP status alone. Private evidence is
`build-ds-first-answer/attempt-06/first-answer.png` and
`first-answer-result.json`, which records the screenshot and immutable browser
hashes. The successful VM is left running on `snapshot-parent-fetch-v2`,
browser `4410b6d017cb15c6e023b6c9c3484002529a765f2045fd0fa0d64818f1202e77`.

The first attempt in this boot ended with a page-initiated cancellation of
POST fetch 8 while in WF_DIAL, then visibly showed “Network Error”. Later
background work and both previously cancelled cross-origin connections
completed successfully. Root made one further ordinary UI submission after
those five Workers returned; Enter was sent at 10:39:06Z. It started one new
Worker, whose completion was followed by the successful event-stream.

The intended retry text was `hi`, but its final user bubble reads `hihi`.
One typing pass was not visible in the first screenshot; another, with paced
press/release events, was made before submission. The earlier input did arrive,
so this evidence must not be described as proof that those keystrokes were
dropped or that the actual prompt contained exactly two characters. Exactly
one Enter was sent for this successful retry. The previous error bubble stays
visible above the successful answer and belongs to the prior submission.

This confirms a first answer after initialization. It does not prove that
cold-start latency, every rendering issue or the prior code-40301 cause is
fully resolved. The optional six-reader optimization and native-call timing
diagnostic have separate normal local guest verification; their newer
snapshots were not substituted into this successful live session. A temporary
wasm3 performance evaluation was not integrated into the product.

## Initial evidence and investigation history

The retained transport evidence establishes one user-authorized test message,
four same-origin POST responses with HTTP 200, and the UI hint “Check network
and retry.” It does not establish which request carried a chat stream, whether
an HTTP-200 body was a business error, or whether any ready/delta/close event
reached the application. The original paths and bodies were not captured.
No path is inferred from the number or ordering of POST requests.

Evidence supplied by the root agent:
`build-ds-render-fixed/real-request/submission-transport-summary.json` and
`build-ds-render-fixed/real-request/chat-after-enter.png`. The screenshot is
referenced as the recorded UI outcome; this report does not reproduce account,
conversation, or identity content. No real request was retried by this audit.

## Public-code evidence

Only the already retained public JavaScript was read:

| Retained file | SHA-256 |
|---|---|
| `public-script-1.js` (vendor bundle) | `dd2b98c5c23bb52b693c08bf7e61ed601c8699507e67308b6c52bf0ced61863a` |
| `public-script-2.js` (main bundle) | `961139c4240e9c4bf5fc6f7ca444c4acfb3774f6c12948203f2cdb5f993599d0` |

Both files are under `build-ds-render-fixed/real-request/`. Offsets below are
zero-based character offsets in Python's decoded UTF-8 string, not byte offsets
(the main bundle contains Chinese strings). They identify stable local source
regions without copying request data or relying on source-map availability.

| Region | What it establishes |
|---|---|
| Main ~851460, `Su.cleanUp` | If the assistant still has a provisional identifier, hints are allowed, and there is no existing user-message hint, cleanup chooses the server-unavailable hint only when both ready and close are true; otherwise it chooses `hintNetworkError`. |
| Main ~852269 and ~865737 | A separate preparation-failure branch emits the same network hint before starting the completion stream. The existence of this branch is relevant to ambiguity; its validation algorithm was neither inspected for solving nor executed. |
| Main ~831000–835300 | The completion transport is a POST using an XHR-backed client. Headers determine stream versus ordinary JSON handling. Incoming text is parsed from XHR progress callbacks. Completion/failure runs cleanup through a finally/dispose event. |
| Vendor ~37555–38600 | The adapter installs a progress listener, a loadend listener, and a one-shot readystatechange listener. The latter compares `readyState` with the constant on the XHR instance, then removes itself. |
| Main ~821680 | The stream parser consumes only the newly appended portion of cumulative `responseText`, using a saved character count. Replaying cumulative text as new data would be wrong; a final buffer containing only complete data is also insufficient if no progress callback ever supplies it. |
| Main ~848800 | Ready and delta handlers themselves mark headers as received; they are not gated on the prior headers callback. A lost headers callback therefore does not logically imply lost ready/delta events. |

The two uses of the identical network hint are distinct application branches.
A generic caught WebAPI error during the XHR/network path can also reach stream
disposal without an uncaught-console entry. This is why an empty console and
HTTP 200 cannot select one branch by themselves.

## Confirmed ordinary XHR defect and repair

The production XHR bootstrap in `c/apps/browser/js_webapi.c` originally placed
its five state constants only on `XMLHttpRequest`, not on its prototype.
Consequently `request.HEADERS_RECEIVED` was undefined. The retained vendor's
one-shot listener silently skipped the headers callback and removed itself.

The fix defines UNSENT, OPENED, HEADERS_RECEIVED, LOADING, and DONE on both the
interface and prototype, with non-writable, non-configurable, enumerable data
properties. Instances inherit them. This follows the standard XHR interface
and Web IDL constant-property rules:
[XMLHttpRequest interface](https://xhr.spec.whatwg.org/#interface-xmlhttprequest),
[Web IDL constants](https://webidl.spec.whatwg.org/#define-the-constants).

`tests/unit/xhr_constants_test.c` reuses the real QuickJS/WebAPI/HTTP parser and
the existing seven-byte in-memory transport from `webapi_test.c`. Its ordinary
local text response exercises the same listener shape. The repaired build
passes 15 checks. `XHR_CONSTANTS_CONSTRUCTOR_ONLY` removes only the prototype
constants: exactly nine checks fail, including the skipped headers hook and
header-before-progress order. In that same negative build, the independent
progress listener still receives the complete body, and HTTP 200/loadend still
complete successfully. A second ordinary local JSON fixture shows another consequence: the default
streaming flag remains true when the header hook is skipped, so the client
bypasses its JSON completion handler and feeds ordinary JSON text to its
stream-shaped callback. The corrected build classifies JSON at headers time
and delivers it through the ordinary-response branch. The retained main bundle
contains that same control flow (~833300–834600): its success handler returns
early while the streaming flag is true. This can mask an HTTP-200 business
response behind a generic network hint. The original responses' content types
and bodies were not retained, so this is a proven compatibility/classification
fix and a concrete candidate, not proof of the live failure's root cause.

Permanent gate: `make BUILD=build-ds-xhr-constants-fix test-xhr-constants`.
The negative control is a prerequisite of the positive target; `test-webapi`
and `ci-host` include it. Evidence:
`build-ds-xhr-constants-fix/xhr-constants/{gate.log,old.log}`.

## Remaining discrimination without another real submission

1. Ordinary XHR event/stream defects have now been repaired and verified:
   callback exceptions could previously become status-zero network errors or
   strand the body Promise; final decoder output could be absent from progress.
   The permanent host suite passes 21 checks plus ASan/UBSan, with exact old
   controls failing 6 and 3 checks. A real HTTP/1.1 chunked server and independent
   guest pass 8/8; the old guest fails all eight while its ordinary HTTP-200
   response remains a valid network control. The host explicitly withholds EOF
   until the early screenshot, proving incremental visibility. See
   [event/stream repair](xhr-stream-events-fix-2026-09-11.md) and
   [real guest evidence](xhr-chunked-guest-2026-09-11.md). These use ordinary
   local data only, without the site's validation workflow.
2. The retained main bundle explicitly catches unexpected transport errors,
   reports parse/JSON/schema errors through its tracker, and always emits
   stream disposal in `finally`. Root's new generic fetch completion diagnostics
   can distinguish response headers, body completion, and coarse payload type
   without logging headers, queries, body text, or credentials. These diagnostics
   cannot retrospectively recover the unrecorded first request bodies.
3. Even with transport and progress validated locally, the original four 200s
   remain compatible with an HTTP-200 business rejection or a pre-stream
   preparation failure. Which occurred remains unconfirmed until independent
   non-sensitive runtime stage evidence is available. No challenge computation,
   validation bypass, credential inspection, or additional DS request is part
   of this report.

## Combined executable and diagnostic boundary

The frozen combined browser is
`build-ds-render-fixed/snapshot-xhr/browser.aex`, SHA-256
`fa4cd5c8646cb55bfd4467cec5ac118759ef8ac1f88f458a33443d0ce9eef6a7`.
It includes the XHR fixes, Cookie clock-rollback repair and the already-verified
rendering changes. The guest tests consumed exactly this browser and the same
frozen kernel as the real-request baseline.

Fetch now logs a fixed payload category (`json`, `event-stream`, `html`, `other`,
or `unspecified`) at headers and a separate `fetch-complete` record at successful
protocol/body-decoding completion, correlated by the existing request handle.
The completion includes the received byte count and local delivery mode. It
does not claim application success or expose arbitrary header values, paths,
queries, bodies or login material. The streaming and buffering controls pass
with these diagnostics enabled. They improve the next trace, but cannot recover
the missing original response bodies or establish which identical network-hint
branch the first submission followed.

## Reopened test window

After the local guest gates passed, the root-owned baseline QEMU was stopped.
Its private disk was copied and checked; journal replay recovered 7 blocks,
discarded 0, and the repack retained all 9 `/browser` user-state inodes. The
combined browser was placed in this new private disk and booted with an explicit
UTC RTC. No running disk was repacked and the default `build/disk.img` was not
written by this operation.

The new visible window reopened the DS home page, with no second chat submission.
Existing history and the account area appeared without entering credentials,
and neither Cookie snapshot-load nor persistence errors recurred. This verifies
login restoration across this actual restart/repack. Evidence:
`build-ds-render-fixed/real-request-fixed/reopen-result.json` and
`homepage-settled.png`; the directory is private because its screenshots show
the user's interface. Home-page JSON responses have separate HTTP-200 and
body-completion records. Background preflight failures were also observed for
two auxiliary hosts, so this is not a claim that every site request succeeded.
Their relationship to the earlier failed chat remains unproven.

The visible QEMU uses `/tmp/logit-ds-fixed.sock` and the private disk under
`real-request-fixed/`; `process.json` records its PID and launch arguments.
The only chat-message submission in this task remains the original failed one.

## Minimal metadata for the next root-controlled observation

The preceding submission count describes that recorded window, not a limit on
later user-authorized tests. The root agent alone operates the live page. This
independent audit reads the retained public bundles and local implementation;
it performs no new requests and does not inspect account storage or responses.

Correlate these records with one request handle **including its generation**.
Do not infer a request's role from its ordinal position, a reused slot, or four
similar HTTP status records.

| Stage | Smallest useful record | What it can distinguish |
|---|---|---|
| Response headers | HTTP status and fixed MIME category: `event-stream`, `json`, `html`, `other`, `unspecified` | A stream-shaped response versus an ordinary response or absent/unexpected MIME. No arbitrary header string is required. |
| Body producer | Successful end versus failure/abort, protocol/body-decoding failure stage, received byte count | Headers alone versus an ended body; transport truncation versus successful protocol/body decoding. This does not establish XHR or application consumption. |
| XHR consumer | State-2 notification count; progress count and final cumulative text length; one terminal classification from load/error/abort, plus loadend | Whether the headers hook has an opportunity to run and whether the final decoded text reaches the ordinary progress path before completion. Record numeric counts, not text. |
| Completed ordinary JSON | Parse result; exact top-level `code` and `data.biz_code`, each classified as missing, non-integer, or a finite safe integer | A parse error versus a numeric application-level result. Zero or absent codes alone do not establish application success. Never log messages, nested result data, or strings used as codes. |
| Caught failure | Fixed phase and allowlisted error name/type, otherwise `other` | A browser exception, JSON conversion error, hook failure, network failure, timeout, or abort without error messages or stacks. |

The retained vendor defines useful fixed client-error classes near character
offsets 32600–35000: `BROKEN_ON_HEADERS_RECEIVED`, `INVALID_CONVERSION`,
`INVALID_JSON`, `HTTP`, `BROKEN_ON_AFTER_RESPONSE`, `NETWORK`, `TIMEOUT`, and
`ABORTED`. These are application-client classifications; a generic fetch logger
cannot claim to observe them solely from the HTTP status. If a temporary
diagnostic sees an error object, map only this fixed allowlist (and standard
exception names such as TypeError/SyntaxError) to enums. Never print arbitrary
`name`, `type`, `code`, `message`, or `stack` strings supplied by the page.

For an ended `event-stream` response with XHR progress, the unresolved stage is
valid event parsing/application handling, not necessarily the transport.
MIME and text length do not prove that a ready, delta, or close event existed.
For ended JSON, a nonzero code is useful business-result evidence; the exact
meaning still belongs to the application's classification. A missing completion
record by itself means only that completion was not observed; a failure record
or a bounded observation is needed before calling it a transport failure.

The current narrow, read-only XHR review found no new defect in the exact
completion adapter's used API subset. It creates a fresh XHR, requests text,
sets headers/credentials, reads all response headers, consumes cumulative
responseText in download progress, and finishes from loadend. State constants,
callback isolation, final decoder progress, and listener removal now cover that
path. The adapter's upload-progress branch is conditional, and this completion
call supplies only download progress; the old empty upload EventTarget is not
evidence for this chat failure. Timeout/reuse/full EventTarget behavior was not
expanded into another speculative repair list.

The callback review also checked listener-object receiver binding, isolation of
each callback, final progress before DONE/load/loadend, and abort during final
progress. No additional production edit or repeated test run was warranted by
that review. The existing 21-check host/sanitizer and 8-case local guest evidence
remain the validation for the repaired stream behavior, rather than proof of
the site's successful response.


## First-answer continuation: baseline on 2026-09-11

The user requested continued debugging until a genuine first reply is visible.
A fresh private copy of the preceding closed test disk was checked (6 journal
blocks recovered, none discarded) and booted with the same frozen kernel and
`fa4cd5c8646cb55bfd4467cec5ac118759ef8ac1f88f458a33443d0ce9eef6a7`
browser. Its evidence is in `build-ds-first-answer/attempt-01/`; no default user
disk was modified. The page restored history and the account area.

One ordinary UI message was sent in this continuation: “Please reply with only:
connection test succeeded.” The settled screenshot shows the submitted user
bubble and “Check network and retry.” No assistant reply is visible. All four
observed POST completions were HTTP 200 JSON, with no `event-stream` completion.
This establishes a failure before an observed answer stream, not a general
network outage.

A brief paused, read-only inspection collected only native request metadata:
slot, generation, method, host and the static API pathname. Request handles 9
and 4102 were the retained challenge-preparation endpoint; handle 8196 was
session creation. The earlier handle 8 had already been overwritten and is not
assigned a pathname from the later snapshot. No headers, authentication values,
response bodies or challenge payloads were extracted. The Worker allocator
still held its initial next-id value and all slots were empty in this page
instance.

The retained public main bundle wraps ordinary cross-origin classic Worker URLs
in a Blob whose bootstrap calls `importScripts`. The browser's Worker
constructor fed `blob:https://...` into its HTTP URL parser and rejected it
before allocation. A local echo fixture reproduces that general browser defect;
Blob lifecycle and pure Worker-global repairs are being verified independently
of the site's script. This source-level explanation is a candidate for the
real preparation failure; the next diagnostic guest must establish which
stage is actually reached after the repair. No application response or
verification result is synthesized.


## Blob Worker integration observation

`build-ds-first-answer/snapshot-worker/` freezes the diagnostic browser
`4ae8148379b76e8988721ccf499b664644a7bca4917c56f8157c860129778017`
with the same earlier kernel. Its local guest echo fixture passed 8/8;
the earlier browser passed 1/8, retaining the ordinary HTTP Worker positive
control. See `blob-worker-lifecycle-fix-2026-09-11.md`.

The previous private browser window was closed and its VM stopped before
snapshotting its disk. `attempt-02/` retains all 9 browser-state inodes. Only
its invalid Cookie snapshots were recovered from the known generation-170
backup; that backup passed the strict decoder at the current clock and had
28 naturally unexpired records. The default user disk was not modified.
This was explicit backup recovery in the test image, not acceptance of invalid
Cookie records by a relaxed decoder. Home-page JSON returned both code 0 and
business code 0, and no Cookie-load or persistence failure was observed.

UI input needed time to settle under this guest. The first automated Enter
produced no observed fetch; the complete text was confirmed in a subsequent
screenshot before another Enter. One submitted user bubble was then observed.
The input timing is recorded in `attempt-02/submit.json`; it is not counted as
two completed chat requests or evidence of a broken send button.

The resulting trace has three completed POST JSON responses, all `code=0` and
`data.biz_code=0`, with XHR load=1/error=0/abort=0. Two Worker instances now reach
constructor-created, script-fetch-complete, eval-complete and running. This
confirms the Blob startup repair changes the real execution path. The settled
page has the user bubble and waiting dots; no answer text or event-stream has
yet been observed. A later asynchronous Worker stage is still unresolved.
The numeric metadata is in `attempt-02/safe-trace.log`, with a private screenshot
`settled.png`; response/challenge bodies are not part of this diagnostic record.

The next diagnostic adds only per-Worker promise rejection/handled metadata
and fixed standard error categories. Native engine messages may be matched to
a finite missing-global allowlist, with only the allowlisted symbol emitted;
no raw error messages or arbitrary properties are logged. A provisional
rejection is not automatically a final unhandled rejection or a fatal worker
error. Worker WebAssembly isolation is being exercised with local arithmetic
modules before any possible installation in a second runtime.


## Missing Worker Fetch confirmed and repaired

The `snapshot-promise` browser (SHA-256
`092ceda5526a7c0c73d424102bec194bd3274990ae555926ab6c6948ee9e6f3c`)
confirmed a native ReferenceError matching the finite `fetch` missing-global
pattern in Worker 1 during normal home-page preparation. The rejection was
later handled and replaced by an object rejection. This was observed before
any new chat text was submitted in `attempt-03`; see its private
`pre-submit-safe-trace.log`.

The general Worker Fetch implementation reuses the browser's asynchronous
transport, CORS, Cookie, redirect and response-stream code, with realm-owned
hooks, timers, request cancellation and teardown. It does not proxy the
worker through page JavaScript or disable cross-origin checks. Its final
host and sanitizer gates passed 41/41, with independent availability/owner/
pump controls. See `worker-fetch-realm-fix-2026-09-11.md` and
`worker-fetch-host-fixture-2026-09-11.md`.

Root's real local HTTP/1.1 guest fixture passed 8/8 on `snapshot-fetch`, while
`snapshot-promise` passed precisely 3/8 (ordinary HTTP Worker echo and parent
Fetch before/after remained correct). Both original result files are retained
under `worker-fetch-guest-{old,current}/`; independent offline revalidation
also checks the complete pixel matrix and actual requests to two distinct
local server ports. No test site or account data is used by these fixtures.

`snapshot-fetch` freezes browser
`1431b7434af7b8b0f51ca466e57e568706d83a2077cc8dd7aeab5bad22189b32`.
The Wasm source matches the earlier verified realm-ownership snapshot;
subsequent unfinished GC changes were removed. Only the separate normal
arithmetic gate was run at integration: a page and two Workers return 42,
with the page and remaining Worker continuing after ordinary termination,
7/7. The stopped memory-lifecycle investigation is not claimed as completed
and its experimental gate is excluded from the default dependency chain.

In `attempt-04`, login again restores and home-page business codes succeed.
Worker resource fetching now reaches HTTP 200 and complete (26612 bytes),
with no missing-fetch rejection. UI input still stalls during native Wasm
execution. Seven valid browser instruction-pointer samples, containing only
RIP/CR3 and mapped native symbol names, fall inside `call_any`; compiler
inlining means this does not prove frequent Wasm function calls. The first
Worker eventually enqueues an outbound message and exits, followed by another
Worker starting. This is evidence of lengthy synchronous execution, not proof
of an infinite loop or an answer. No site's module or computation payload was
extracted or replayed for this observation. No answer stream or assistant
reply is claimed at this stage.


### Attempt 04: ordinary message submitted after input recovered

The input eventually became responsive. `attempt-04/current.png` visibly
contains the ordinary text `hi`; one Enter was then sent at
2026-09-11T08:39:39Z, with the serial byte offset retained in `submit.json`.
This avoids counting the earlier, not-yet-visible typing attempts as chat
submissions. Three POST responses completed as HTTP 200 JSON with code=0 and
biz_code=0; Workers 3 and 4 started and received messages. Worker resource
fetching again completed successfully. These are preparation steps, not an
assistant answer or evidence that the completion request has been sent.

The scheduler audit found no total wall-time bound in one worker pump: it
services all worker fetch realms and then all tasks from the turn's sequence
snapshot. It also drains up to 100000 reactions per checkpoint. A sequence
snapshot prevents newly queued task chains from extending that scan, but does
not make multiple already-pending expensive tasks fair to input and painting.
A generic task-boundary budget is being implemented and independently checked
with finite arithmetic callbacks. It will yield only after a complete callback
or reaction returns; it cannot preempt one native Wasm invocation.


Both ordinary Worker results in attempt 04 eventually returned. The next
observed JSON response on XHR 31/fetch 4104 contained code=40301 (HTTP 200),
and `server-result.png` visibly shows “Validation failed”. No assistant answer
or event-stream was observed. The screenshot was captured approximately
20 minutes after submitting `hi`; this is an observation interval, not a
precisely measured server latency. The meaning/cause of the validation
failure has not been established. In particular, the long computation does
not on its own prove expiration or an incorrect result.

The generic scheduler repair passes 59 finite host checks and the same
sanitizer checks. Root's ordinary local QEMU case additionally observes
Worker 1's parent message in the same 10-ms clock tick as completion, before
Worker 2 starts; the old binary delivered it only after Worker 2 had already
started. Current result/pixel matrix is 3/3; old is exactly 2/3 with ordinary
arithmetic and parent input-event controls retained. These input events are
synthetic DOM events, not a physical-keyboard latency measurement. The first
old report had an apparatus-only failure from requiring two HTTP requests
for one cacheable shared script. Its separate revalidation accepts one or
two fetches while still requiring both correct Worker result records; the
original evidence is unchanged. Artifacts: `worker-turn-guest-{old,current}`.

### Attempt 05: parent delivery improved; validation still fails

`snapshot-turn-preview` freezes browser SHA-256
`46ca7ce8b47ddaa1c94dadbba5381226db693e78b4c9d5ac8922ad7c53a22554`.
It contains the verified task budget and four compiler-only Wasm reader
inline annotations. The latter retain the original reader bodies and checks.
Private profile transfer again passes the strict decoder, preserving nine
browser inodes and 28 naturally unexpired Cookie records without recovery;
the actual page restores its login.

One visible `hi` was submitted at 2026-09-11T09:25:17Z. Worker 3's result is
now delivered to the parent before Worker 4 returns, showing that the parent
priority repair changes this real execution sequence. Both Workers eventually
return. XHR 41/fetch 20484 then completes as HTTP 200 JSON, 55 bytes,
`code=40301`; the screenshot captured at 09:46:08 UTC again visibly says
“Validation failed”. No assistant answer has been observed. The elapsed
observation interval is not a measurement of server response time, nor proof
of the meaning or cause of code 40301.

An additional normal Worker resource request, id 9, times out with state 1,
which is **WF_DIAL**, and no observed request-start diagnostic. Socket opening
has begun, but HTTP transmission has not been observed. The first call to
`fetch_step` can see an expired deadline after unrelated long native work:
`fetch_dial` previously did not initialize the first-step gap baseline. Later
step gaps already receive scheduling-delay compensation. Separately, a parent
message handler can enqueue Fetch after the page's network phase, then the
same worker pump enters the next expensive callback before servicing that
Fetch. These are two concrete local scheduling defects. Their causal link to
the server's validation rejection remains unestablished.

The next bounded repair gives parent-result delivery an explicit handoff to
one non-blocking page Fetch checkpoint before returning to input/paint, and
initializes the existing first-step timing baseline. It retains the existing
30-second deadline and 250-ms gap threshold. It does not wait for connection
establishment or preempt a native Wasm call. Independent finite local fixtures
will check actual HTTP send order and delayed-first-pump versus continuous
poll timeout behavior before another real submission.

### Attempt 06 build and local verification

The two fixes pass 18 parent-to-Fetch ordering checks and 15 initial-deadline
checks, including the same valid finite inputs under ASan/UBSan. Their old
source controls fail precisely the expected two ordering and three timing
assertions. The fairness suite now passes 60 checks. Page, Worker, Fetch,
XHR and stream regressions pass; detailed boundaries are recorded in
`parent-fetch-handoff-and-initial-deadline-2026-09-11.md`.

Root freezes browser SHA-256
`4410b6d017cb15c6e023b6c9c3484002529a765f2045fd0fa0d64818f1202e77`
in `snapshot-parent-fetch-v2`. Local QEMU Worker Fetch passes 8/8 and the
finite Worker scheduling fixture passes 3/3, with matching pixels, unchanged
input images and both test VMs stopped. The first attempted freeze caught a
concurrent test-only Makefile edit, published no valid manifest and is marked
incomplete; the replacement freeze has no source drift.

The old live guest stopped via QMP. Its close button was clicked, but browser
close processing was not observed before shutdown. Checked profile transfer
then preserves nine browser inodes and 28 unexpired Cookie records, with
strict decoder status zero and no backup recovery. `attempt-06` starts the
verified binary and restores login; initial page responses again have HTTP
200 and business code zero. These results establish startup and persistence,
not a successful chat response.

The normal text `hi` was visibly confirmed in `input-wait-latest.png`, then
one Enter was delivered at 2026-09-11T10:14:13Z (serial byte offset 57831).
Input processing still waits for the current native call to return. By
10:20:04 UTC, `waiting-answer.png` shows exactly one submitted user bubble
and waiting dots. Three preparation responses have HTTP 200, code zero and
business code zero, and both new Workers' resources receive HTTP 200. No
answer text is yet observed. A request diagnostic alone is not proof of wire
transmission: `fetch_dial` also emits it when socket opening fails. The local
ordering fixture separately instruments actual transport `f_send`.

Two previously ambiguous cross-origin GET failures were checked against the
adjacent fixed browser reason text. Both are `aborted`, as are two earlier
preflight instances. They establish explicit request cancellation, not EOF,
CORS rejection or an opening failure. No request payload was examined and no
site policy was changed; see `fetch-cancellation-diagnostic-audit-2026-09-11.md`.
