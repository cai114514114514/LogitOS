# Cookie repair, request context wiring and fetch diagnostics — 2026-09-10

## Outcome and acceptance boundary

This follows the findings in
[the modal fix and Cookie audit](modal-pointer-all-and-cookie-audit-2026-09-10.md).
That earlier document remains the historical audit: its statement that Cookie
product code was unchanged described that pass, not this implementation.

The eight recorded Cookie findings now have implementation changes and local
regressions. Existing fetch/XHR and native loader paths preserve the requesting
document's identity. Persistent Cookie state survives a real guest restart.
The actual browser can distinguish received HTTP errors from transport, CORS
and response-body failures in its serial diagnostics.

**DeepSeek verification followed by `poor connection` is not yet closed.** The
user confirmed the CAPTCHA interaction works. The previous VM had exited; the
last retained handoff serial file ended at 2026-09-10 19:48 local time and did
not contain the HTTP result of that reported failure. It showed successful TLS
connections and a separately caught `substr`-on-undefined exception, which is
not evidence of the login failure's cause. No account request was replayed, no
CAPTCHA response was automated, and no real phone number or credential was
included in this report or the new request diagnostics. A new manual attempt
in the final guest is required to identify that last failing exchange.

## Changes

| Finding | Implemented behavior |
|---|---|
| Secure integrity | Reject insecure replacement/deletion overlapping an existing Secure cookie, and prevent a lower-protection admission from evicting the existing protected entry. |
| Schemeful SameSite | One `cookie_request_kind` uses initiator host/scheme, navigation kind, method safety and explicit browser initiation. Unknown/opaque context is conservative. |
| Cookie creation | Incoming `Set-Cookie` and `document.cookie` use the creation-side SameSite rule as well as independent credentials/HttpOnly policy. |
| Public suffixes | One generated table supplies Domain admission, retrieval, restoration and site comparison; ICANN, PRIVATE, wildcard and exception rules are included. |
| Persistence | Versioned, CRC-checked snapshots, strict core revalidation, checked writes/readback, private directory/files, and persistent-only restoration are wired at browser startup and accepted mutations. |
| Prefixes | Prefix-only names, case-insensitive prefix recognition, explicit root Path and forbidden Domain attributes are validated in the common core. |
| Header pressure | Complete serialization or an explicit failure; cache lookup and transmission agree. Diagnostics expose required bytes/capacity/count, never cookie values. The script getter also reports capacity failure explicitly. |
| Parsing/lifetime | Combined 4096-byte name/value limit, 400-day lifetime ceiling, stricter Max-Age parsing and duplicate attribute handling; typed/history/restore navigation no longer inherits an unrelated previous page as its creator. |

The maintained PSL snapshot is version `2026-09-08_12-18-37_UTC`, with 10,325
rules. Provenance, pinned input hashes, license and update commands are in
[tools/psl/README.md](../../tools/psl/README.md). Generated offsets avoid one
runtime pointer relocation per rule; ordinary builds make no network request.

Each asynchronous request owns its initiator snapshot. CORS Origin, credentials,
preflight lookup, response Cookie admission and redirects read that snapshot,
not the mutable global page URL. The preflight key includes the creator and
complete destination URL. Native resource resolution now separates the
committed document from a stylesheet/import base. Cross-site redirect history
does not become same-site again on a return hop. Cross-origin redirects drop
the author Authorization header.

Persistence uses two checked snapshots and updates both on a successful
mutation so an older backup cannot resurrect an acknowledged deletion. I/O,
allocation and capacity failures latch an observable status. Exact unchanged
content, session-only changes and last-access updates do not trigger disk
writes; no hash collision can suppress a credential mutation. The guest adapter
requires durable 0700/0600 modes and validates ownership, file type and links.

The new `fetch-request`, `fetch-response` and `fetch-detail` events share a
request ID through preflight and redirects. They contain method, sanitized host,
port, TLS flag, phase, status and local numeric counters. URL paths, queries,
fragments, raw headers and bodies are excluded; malformed authority is redacted.
CORS response/preflight, timeout and connection-start failures have explicit
boundaries. HTTP 403 remains a Response; a closed or truncated exchange rejects
the promise/body at the appropriate point.

Broader regression also exposed an existing narrow-embedder AbortError defect:
`new (DOMException || TypeError)(message, name)` lost the name when DOMException
was absent. The common error factory now retains the error name without
advertising a fabricated DOMException implementation. Original stream assertions
were preserved; the initial three positive/two control failures disappeared.

## Verification

All fixtures use synthetic values and local transports/servers. Host results:

- Cookie jar: 220 checks; hardening: 141 checks; hardening ASan/UBSan clean
  with leak detection disabled on this host.
- PSL: 61,950 comparisons over all 10,325 raw rules, plus 78 upstream cases.
- Six added core negative controls fail in their own expected groups
  (5/3/14/4/7/3); four existing core controls remain effective.
- JavaScript request context: 23 checks; native context: 29; native navigation:
  nine link/form/script/tab cases. Context controls fail the intended cache and
  document/base identity assertions.
- Cookie/CORS: 55; response boundary: 28; streams: 62; buffered stream control: 32.
- Cookie persistence: 59 plus private guest adapter: 17; independent host
  processes restore exact persistent values and omit previous session values.
  Both missing-write and prefix-read controls fail after the second process.
- Existing storage regression: backend 37, platform 3, JavaScript persistence
  16, core persistence 61, including independent-process restoration.
- Request diagnostics: eight host scenarios plus malformed-authority privacy
  control, 45 behavior checks passed. Removing diagnostics keeps behavior
  passing but fails observability.

Guest evidence includes the actual `browser.aex`, not just a host link:

- The local diagnostic page exercises application-level error content in a
  200 response, HTTP 403, a POST with no response, truncated response body,
  CORS rejection, redirect and preflight. All seven complete with expected
  page-visible results and matching diagnostic IDs. No POST retry occurs.
- The persistence driver boots one writable disk copy twice without
  `-snapshot`. Both script and network persistent Cookies return after reboot;
  session Cookies do not; HttpOnly remains hidden from script. The prior
  immutable disk reaches the seed page correctly, then fails precisely the
  persistent restoration assertion on its second boot.
- This is restart-after-completed-mutation evidence. It is not a new sweep of
  every possible power interruption during disk writes.

Final integration passed with an exit status of 0:

```sh
make -j4 BUILD=build-cookie-context-final-tests \
  test-cookie-jar test-cookie-cors test-browser-context \
  test-browser-cookie-context test-cookie-persistence test-fetch-diagnostics \
  test-stream test-stream-control test-webapi test-fetch-fresh-retry test-mk-wired
```

The aggregate additionally confirmed Web APIs 227/227, fresh-connection retry
15/15, and `mk-wired: 246 fragments, 245 reachable, 1 declared`. Expected FAIL
lines in the log belong to prerequisite negative controls, not failed positive
gates. The component storage/navigation runs also completed with exit status 0.

Both `logit.iso` and the app-containing `disk.img` were rebuilt. A temporary
failure in another concurrent task's `net` CLI link resolved when that task
finished adding its own dependencies; this pass did not overwrite its changes.
Final immutable snapshots are in `build-cookie-context-fix/snapshot-final/`:

| Artifact | SHA-256 |
|---|---|
| logit.iso | `0efc9e94e227a9eaa4c0d4bad042e85725cfddd73d0953cf8dd785fd6277b584` |
| disk.img | `cd9a9e5659deb5b853f4ccaf4d77b950f4ba18ba44b1823bd6bb9a44efdfcfe5` |
| browser.aex | `759d7e01fff37bdf79909ff34134ed5da8b3ddee0902ad799aa4bf99a1400615` |

The final diagnostic guest again passed all seven scenarios. The final
persistence gate ran its old-browser negative control and the new two-boot
positive control successfully. Source image hashes matched after execution.

Evidence, relative to the repository:

- `build-cookie-context-fix/verification/cookie-context-final-gates.log` and
  the component `browser-context-*.log` files beside it.
- `build-cookie-context-fix/fetch-diagnostics/guest-final/results.json`,
  `serial.log`, and `result.png` (the rendered seven-PASS page).
- `build-cookie-persistence-fix/final-guest-gate.log` and
  `cookie-persistence-guest/results.json` (both boots and artifact hashes).
- `build-cookie-core-fix/core-gates.log` and the pinned PSL provenance files.

A visible final guest was opened on `https://chat.deepseek.com/`, using a
separate writable disk in `build-cookie-context-fix/handoff-final/`. Its first
five main-site GET exchanges returned HTTP 200; the cross-origin telemetry
OPTIONS/GET returned 204/200. That observes startup, not account verification.
The user was asked for the result of one manual verification in that window;
until it occurs, the original `poor connection` cause remains unproven.
The earlier `snapshot-v1` evidence and the user's previous handoff disk remain
intact. No commit was created and no IME product source was edited by this pass.

## Remaining scope

Correction from the user's restart report: the earlier two-boot persistence
acceptance kept the same disk image between boots. It did not cover a system
repack triggered by `make run`. That separate producer destroyed saved browser
state even when the Cookie files had been written correctly. The new
[profile rebuild repair](browser-profile-rebuild-2026-09-10.md) preserves the
entire `/browser` tree and has now passed two real guest boots with an actual
mkfs rebuild between them, covering persistent Cookies and localStorage.

This repair covers existing top-level request and navigation consumers. It does
not implement complete multi-frame Web APIs: frame-side document/fetch/XHR and
cross-origin DOM isolation remain explicitly unsupported, rather than borrowing
the top-level globals. The Cookie layer accepts canonical A-label hosts; this
does not establish full IDNA support in the separate URL parser. Persistence
assumes one browser-process writer and does not implement multi-process merge
or locking. Multiple substantive persistent mutations in one response still
perform separate checked commits; no general performance speedup is claimed.

Standards used: [HTTPWG Cookie storage/retrieval](https://httpwg.org/http-extensions/draft-ietf-httpbis-rfc6265bis.html),
[WHATWG Fetch](https://fetch.spec.whatwg.org/),
[HTML site definitions](https://html.spec.whatwg.org/multipage/browsers.html#same-site),
and the [official PSL](https://publicsuffix.org/list/).
