# Browser security model

The browser carries document identity into requests, filters responses exposed to scripts, applies embedding and sandbox policies to frames, and bounds selected sources of script work and memory use. These mechanisms live in `c/apps/gui/browser/` and use the system socket and TLS stack for HTTPS. Their implementation and current limitations are described below.

## Origins and document identity

Script fetch represents an HTTP or HTTPS origin by scheme, host, and effective port. In `c/apps/gui/browser/js/runtime/webapi.c`, `same_origin()` compares scheme and port directly and compares hosts without ASCII case sensitivity. `wurl_origin()` serializes the scheme and host, adding a port when it differs from HTTP port 80 or HTTPS port 443. Paths, queries, and fragments are outside this comparison. Endpoint parsing uses `c/net/http/url.c`.

The JavaScript URL parser in `c/apps/gui/browser/js/net/url.c` also supplies origins to frame policy and native frame messaging. It serializes tuples for HTTP, HTTPS, FTP, WS, and WSS. Blob origin serialization derives a tuple from an embedded HTTP or HTTPS URL. Other cases serialize as `"null"`.

A fetch realm retains a resolution base, initiating origin, and site for cookies. Requests snapshot the initiating origin, cookie site, and referrer when created. The resource loader similarly keeps the committed document URL through `bfetch_set_document()` and the resolution base through `bfetch_set_base()`. This preserves document authority while relative resources resolve against changing bases. See `c/apps/gui/browser/js/runtime/webapi.c` and `c/apps/gui/browser/app/runtime.c`.

A network frame with the sandbox origin restriction receives an opaque initiator before its Web APIs are installed. Its document URL still resolves references. Fetch treats destinations as cross origin, serializes the initiator as `"null"`, and suppresses the referrer. Cookie and Web Storage accessors throw `SecurityError`. Local frame loading treats `about:blank` as inheriting the embedding origin, then applies the frame’s sandbox flags during settlement. These paths are in `c/apps/gui/browser/frame/passive.c` and `c/apps/gui/browser/js/runtime/platform.c`.

## Fetch policy

The browser has two main HTTP request paths:

| Path | Policy owner |
|---|---|
| Navigation and resource GETs | The resource loader in `c/apps/gui/browser/app/runtime.c`; embedded requests carry an owner callback that checks redirect destinations. |
| Script fetch and XMLHttpRequest | Fetch realms and CORS state in `c/apps/gui/browser/js/runtime/webapi.c`; XMLHttpRequest calls the JavaScript fetch binding. |

The native script fetch state defaults to CORS mode and credentials restricted to the same origin. For CORS requests from another origin, methods outside GET, HEAD, and POST, or headers outside the implementation’s safelist, trigger an OPTIONS preflight. Its response must authorize the origin, method, and requested headers before the application request proceeds.

`cors_check_response()` runs after response headers arrive and before the response body stream is exposed to script. Credentialed responses from another origin require a matching `Access-Control-Allow-Origin` and `Access-Control-Allow-Credentials: true`. Duplicate headers that allow an origin, and a wildcard origin with credentials, cause rejection. `no-cors` responses from another origin receive an opaque view: status zero, empty headers, and an inaccessible body.

CORS response filtering exposes safelisted headers and those authorized by `Access-Control-Expose-Headers`. It filters `Set-Cookie` and `Set-Cookie2` from script access. Native request construction drops author headers controlled by the browser, including `Host`, `Cookie`, and `Origin`. Header validation in `c/net/http/http1.c` checks token syntax and rejects values containing carriage returns, line feeds, or NUL bytes. JavaScript header guards are implemented in `c/apps/gui/browser/js/net/fetch_body_prelude.inc`.

Redirect processing checks the current response’s CORS policy and the next destination’s installed connect policy. An origin change removes `Authorization` supplied by the author; subsequent hops recheck same origin mode and preflight requirements. Redirect taint considers both the previous destination and initiating origin. A tainted request serializes its origin as `"null"`. See `fetch_redirect()` in `c/apps/gui/browser/js/runtime/webapi.c`.

The preflight cache matches initiating origin, target URL with its fragment removed, method, and credential use, then checks header authorization. Opaque initiators bypass cache storage because their shared `"null"` serialization cannot identify individual origins.

## Cookies and request context

Resource loading and script fetch share a browser cookie jar. `c/net/http/cookies.c` applies domain and path matching, expiry, Secure, HttpOnly, SameSite, and cookie prefix checks. Domain validation and site comparison use the vendored ICANN and PRIVATE public suffix data described in `c/net/http/cookies.h`.

Fetch credentials determine whether a script request may carry cookies. SameSite selection then uses the request’s site for cookies, scheme, navigation status, and method safety. The resource loader distinguishes navigation from resource requests and carries ancestor context for embedded documents. A sandboxed frame derives its site for cookies from document URLs, while its opaque fetch initiator still affects credentials and script cookie access. See `c/apps/gui/browser/app/runtime.c` and `c/apps/gui/browser/frame/passive.c`.

`document.cookie` uses the jar’s script access context, which hides HttpOnly cookies and protects them from script replacement. Cookie serialization reports failure when the complete header exceeds capacity, allowing network callers to reject the request. Persistent cookies use checked disk snapshots through `c/apps/gui/browser/storage/cookies.c`; session cookies remain in browser memory.

## TLS and certificates

Both HTTP request paths reach the system socket service. Browser transport uses TLS ALPN to select HTTP/1.1 or HTTP/2. Connection reuse matches host, port, and TLS use, preventing reuse based solely on a shared destination address. The integration is in `c/apps/gui/browser/app/runtime.c`, with pool matching in `c/net/http/hpool.c`.

For a TLS socket, `c/net/core/sock.c` passes the requested host and system verification time to `tls_start()`, then advances the handshake through `tls_step()`. Successful TLS completion makes the socket ready; handshake failures produce socket errors.

The clients in `c/net/tls/tls.c` and `c/net/tls/tls12.c` implement TLS 1.3 and TLS 1.2. Both use the chain checker backed by `x509_verify_chain()` in `c/net/tls/x509.c`. It checks the leaf’s SAN DNS names, falls back to the common name when SAN is absent, validates dates for certificates selected into the path, verifies parent signatures, and requires a path to a held trust anchor. `c/crypto/trust/roots.c` includes the generated root bundle.

TLS 1.3 full handshakes require a verified CertificateVerify message as proof of possession of the certificate’s private key. Resumption follows the PSK authentication path. `tls_start()` rejects handshakes when the system randomness service reports weak entropy.

The stack requests OCSP stapling. `c/net/tls/ocsp.c` checks the response signer, signature, certificate identifier, status, and time fields. Both TLS versions pass its verdict through the policy in `tls_check_staple()`.

## Frame sandboxing and embedding policy

`c/apps/gui/browser/frame/sandbox.h` defines sandbox restrictions and their shared parser. An empty sandbox attribute sets every restriction. Recognized `allow-*` tokens clear the associated restrictions; token matching ignores ASCII case.

A network frame combines inherited restrictions, the iframe attribute captured at navigation, and sandbox directives from enforced response CSP headers. Restrictions from multiple enforced policies are unioned. Scripted network frames use their own page context and QuickJS runtime, with sandbox state applied before author scripts run.

| Restriction | Consumer |
|---|---|
| Scripts | The native frame loader renders the document while skipping script runtime creation. |
| Origin | Web API installation supplies an opaque initiator; cookie and Web Storage access throw `SecurityError`; message origins serialize as `"null"`. |
| Modals | Page bindings suppress message output and return dismissal values. |
| Auxiliary navigation | The sandbox prelude validates popup URLs and returns `null` for blocked requests. |

These consumers are in `c/apps/gui/browser/frame/passive.c` and `c/apps/gui/browser/js/runtime/page.c`.

Embedding checks in `c/apps/gui/browser/frame/policy.c` evaluate the parent’s `frame-src`, `child-src`, and `default-src` fallback chain, the child’s `frame-ancestors`, and applicable X-Frame-Options headers. Enforced `frame-ancestors` takes precedence over X-Frame-Options. HTTPS documents reject HTTP frame children.

The network frame owner also connects CSP checks to scripts, styles, images, HTTP connections, worker loading, and base URLs. Redirect callbacks check new destinations before fetching them. String code generation permission reaches QuickJS through `JS_SetStringCodeGenerationAllowed()`.

The native WindowProxy bridge in `c/apps/gui/browser/frame/window.inc` serializes message data into the receiving runtime and checks the target origin at delivery. Document epochs and connectedness checks discard messages after navigation or removal. Its shared queue holds up to 64 messages, with a 64 KiB serialized size limit per message.

## Script and resource budgets

The runtime combines interruption during script execution with deadline checks between scheduled work units.

| Mechanism | Default | Source |
|---|---|---|
| Armed page entry watchdog | 45 seconds of charged time or 2,000,000 interrupt handler invocations | `c/apps/gui/browser/js/runtime/page.c` |
| Worker watchdog | 8 seconds or 400,000 interrupt handler invocations | `c/apps/gui/browser/js/workers/worker.c` |
| Scheduled task turn | 8 ms, checked between work units | `c/apps/gui/browser/js/runtime/task_budget.h` |
| QuickJS stack guard | 2 MiB for page and worker runtimes | Page and worker runtime creation |
| Shared timer and animation callback queue | 4,096 entries | `c/apps/gui/browser/js/runtime/page.h` |

Fuel supplies a backstop for synchronous execution under a frozen harness clock. It counts interrupt handler invocations associated with QuickJS branch and call polling. Page watchdog accounting excludes designated module loader network waits. An embedder cancellation callback also lets a close request unwind script at an interrupt poll.

The scheduler carries a shared turn deadline through page phases and interleaved worker work. Later passes resume unfinished phases, providing opportunities for input and painting between batches.

Buffered responses use a default 16 MiB body limit. The native resource loader selects 4 MiB for embedded responses and 64 MiB for navigation at the top level. The native frame loader additionally counts fetched resource bytes against an 8 MiB total per frame. Script fetch streaming applies backpressure by pausing pumping when its queued bytes exceed 512 KiB. See `c/apps/gui/browser/net/fetch.h`, `c/apps/gui/browser/app/runtime.c`, and `c/apps/gui/browser/js/runtime/webapi.c`.

## Storage partitioning

Web Storage wrappers retain their origin, storage kind, and session identity. Backend operations look up these retained keys, keeping a surviving wrapper attached to its original namespace.

| Service | Identity |
|---|---|
| `localStorage` | Serialized document origin and storage kind. |
| `sessionStorage` | Document origin, storage kind, and the identity of the top level tab. |
| Cookies | Shared browser jar, selected by domain, path, and request context. |
| IndexedDB | Database map captured by the installed document context. |
| CacheStorage | Named cache map captured by the installed document context. |
| HTTP response cache | Resource URL and a hash of the outgoing Cookie header over 64 bits. |

Web Storage provides 16 backend area slots in total, with 256 items and a 256 KiB byte quota per area. CacheStorage has a default 64 MiB byte quota across its named caches. These limits are defined in `c/apps/gui/browser/storage/backend.h` and `c/apps/gui/browser/js/storage/cache.c`.

The application installs local storage and cookie persistence before opening pages. Local storage snapshots use checksums and readback verification through `c/apps/gui/browser/storage/persistence.inc`. Tab closure drops the corresponding session storage areas; navigation retains the tab identity. Application wiring is in `c/apps/gui/browser/app/main.c`.

The HTTP cache uses the Cookie header captured for the outgoing request, before response cookies update the jar. Embedded requests bypass cache lookup and storage so their owners receive response CSP and X-Frame-Options metadata. See `c/apps/gui/browser/app/runtime.c` and `c/apps/gui/browser/net/cache.c`.

## Limitations

- `SECURITY.md` and `TRANSPARENCY.md` describe the browser and TLS stack as experimental and unsuitable for hostile workloads or valuable credentials. Page, frame, and worker QuickJS runtimes share the browser process; runtime ownership and Web API checks provide no process boundary against native memory corruption.
- CSP sandbox flags from the response at the top level feed creation of child frames, while the page at the top level retains zero sandbox flags. The frame CSP consumers described above do not establish equivalent enforcement for scripts and connections at the top level. See `c/apps/gui/browser/frame/passive.c` and `c/apps/gui/browser/js/runtime/page.c`.
- Fetch option handling is incomplete. In `c/apps/gui/browser/js/net/fetch_body_prelude.inc`, fetch takes mode and credentials from its initialization argument, losing those stored on a supplied `Request` when that argument omits them. Direct fetch construction also bypasses the Request constructor’s header guard. Native preflight safelisting in `c/apps/gui/browser/js/runtime/webapi.c` has fewer value checks than the JavaScript guard.
- Several parsed sandbox flags await consumers. Native network frames support depth one and lack form submission, direct DOM access across runtimes, and navigation across windows. Opaque local `about:blank` and srcdoc frames keep scripts disabled even with `allow-scripts`, because their adoption path shares the embedding runtime. Workers remain absent in opaque network frames. See `c/apps/gui/browser/frame/window.inc`, `c/apps/gui/browser/js/runtime/platform.c`, and `c/apps/gui/browser/frame/passive.c`.
- Watchdogs act at QuickJS poll points, and turn deadlines act between work units; native calls can exceed those intervals. Page runtime creation sets a stack guard without setting a total QuickJS heap limit. Streaming responses can exceed the buffered body ceiling, as `c/net/http/http1.c` sends streaming bytes directly to the sink.
- Local Web Storage has no partition by top level site. Its fallback for a document URL rejected by the HTTP parser uses the raw URL string, so matching strings share storage. IndexedDB and CacheStorage have memory stores tied to the document context rather than persistent backends shared by origin. See `c/apps/gui/browser/js/runtime/webapi.c`, `c/apps/gui/browser/js/storage/idb.c`, and `c/apps/gui/browser/js/storage/cache.c`.
- The older OCSP policy comment in `c/net/tls/ocsp.h` lists stale responses among fatal failures. The current correction in `c/net/tls/tls.c` accepts and logs `OCSP_E_STALE`, including signed responses with expired or future timing and responses lacking `nextUpdate`. Missing staples and unavailable issuer certificates also permit continuation. Online OCSP is absent; `TRANSPARENCY.md` records broader cryptographic review and conformance limits.

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/js/runtime/webapi.c` | Fetch identity, CORS, redirects, credentials, cookie access, storage bindings, and stream backpressure. |
| `c/apps/gui/browser/js/net/fetch_body_prelude.inc` | JavaScript fetch, Request construction, and header guards. |
| `c/apps/gui/browser/js/net/url.c` | JavaScript URL parsing and origin serialization. |
| `c/net/http/url.c` | HTTP endpoint parsing and relative resolution. |
| `c/apps/gui/browser/app/runtime.c` | Resource requests, cookie context, policy metadata, transport selection, and body limits. |
| `c/apps/gui/browser/net/fetch.h` | Fetch contracts and buffered response limits. |
| `c/net/http/http1.c` | Header validation, buffering limits, and streaming body delivery. |
| `c/apps/gui/browser/frame/sandbox.h` | Sandbox flags and parsing of attributes and response policy. |
| `c/apps/gui/browser/frame/policy.c` | Embedding, resource, script, and worker policy checks. |
| `c/apps/gui/browser/frame/passive.c` | Network frame ownership, sandbox setup, policy wiring, and resource budgets. |
| `c/apps/gui/browser/frame/window.inc` | WindowProxy messaging, origin checks, epochs, and queue bounds. |
| `c/apps/gui/browser/js/runtime/platform.c` | Local frame settlement, origin inheritance, and sandbox access checks. |
| `c/apps/gui/browser/js/runtime/page.c` | Page runtime creation, watchdog, scheduler, and sandbox consumers. |
| `c/apps/gui/browser/js/runtime/page.h` | Timer and animation callback capacity. |
| `c/apps/gui/browser/js/runtime/task_budget.h` | Shared deadline for task turns. |
| `c/apps/gui/browser/js/workers/worker.c` | Worker runtimes, scheduling, stack guard, and watchdog. |
| `c/apps/gui/browser/storage/backend.c` | Web Storage key matching and mutations. |
| `c/apps/gui/browser/storage/backend.h` | Storage keys and quotas. |
| `c/apps/gui/browser/storage/persistence.inc` | Checked snapshots of local storage. |
| `c/apps/gui/browser/storage/cookies.c` | Cookie snapshots and restore validation. |
| `c/apps/gui/browser/js/storage/idb.c` | IndexedDB store tied to the document context. |
| `c/apps/gui/browser/js/storage/cache.c` | CacheStorage store tied to the document context and byte quota. |
| `c/apps/gui/browser/app/main.c` | Persistence installation and tab storage identity and cleanup. |
| `c/apps/gui/browser/net/cache.c` | HTTP cache keys and hashing of the Cookie header. |
| `c/net/http/cookies.c` | Cookie validation, matching, and SameSite decisions. |
| `c/net/http/cookies.h` | Cookie contexts, limits, and public suffix policy. |
| `c/net/http/hpool.c` | Connection pool identity matching. |
| `c/net/core/sock.c` | Socket progression through TLS. |
| `c/net/tls/tls.c` | TLS 1.3 authentication, chain and staple policy, resumption, and entropy checks. |
| `c/net/tls/tls12.c` | TLS 1.2 client authentication path. |
| `c/net/tls/x509.c` | Hostname checks, certificate dates, signatures, and trust paths. |
| `c/net/tls/ocsp.c` | Stapled OCSP verification and result classification. |
| `c/net/tls/ocsp.h` | OCSP interface and older policy description. |
| `c/crypto/trust/roots.c` | Generated trust bundle inclusion. |
| `third_party/quickjs/quickjs.c` | Runtime allocation defaults and implementation of memory limits. |
| `SECURITY.md`, `TRANSPARENCY.md` | Operational restrictions and claim rules. |

## See also

- [Browser architecture](overview.md)
- [Loading pages: fetch, URLs and the HTTP cache](loading-and-fetch.md)
- [Frames, documents and isolation](frames-and-isolation.md)
- [The JavaScript runtime](javascript-runtime.md)
- [Storage, crypto, text and media APIs](storage-crypto-media.md)
- [TLS and certificates](../networking/tls.md)
- [Security model](../architecture/security-model.md)
