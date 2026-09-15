# Worker Fetch host gate, 2026-09-11

This independent fixture fixes the observable requirements before accepting the
Worker Fetch implementation. It runs the actual page and dedicated Worker
schedulers in separate QuickJS runtimes. Only our worker startup source uses
`loader_fakebfetch`; fetch traffic goes through the production Fetch, HTTP/1,
Cookie, CORS and redirect code over `stream_net.h`'s in-memory socket vtable.
The two logical HTTPS origins, `page.test` and `peer.test`, never resolve or
connect to a real server. This is host integration evidence, not TCP/TLS, guest
or real-site completion evidence.

The saved pre-change `js_worker.c` and `js_webapi.c` produce exactly one named
failure, `both dedicated Workers expose fetch`. The fixture stops after this
presence check so absent APIs cannot manufacture unrelated timeout failures.
The original sources and their SHA-256 hashes are under
`build-worker-fetch-fixture/old/` and `before-source-hashes.txt`; the baseline
compile recipe is `build-worker-fetch-fixture/baseline.mk` and the observed
failure is `before.log`.

The final positive gate checks:

- A page and two Workers fetch independently, including script-relative text
  and ArrayBuffer bytes, while the page's Fetch function and location remain
  unchanged and Worker DOM/XHR remain absent.
- Valid and refused CORS responses, credential omission/inclusion, a shared
  valid Cookie jar, non-simple-request preflight, and cross-origin redirect
  removal of author Authorization headers retain the existing policy path.
- Three simultaneously held responses remain independently owned. Worker
  AbortController rejects its own body and leaves page/peer bodies alive.
  A test-only native hook confirms that a foreign page owner cannot abort a
  pending worker handle; opaque handles are never exposed to page script.
- Blob workers reject relative fetch against their opaque base, while absolute
  fetch uses the creator's origin/site and still works after URL revocation.
- terminate(), self.close(), and close inside a fetch reaction release requests
  and prevent late parent delivery. Final page close is measured while both a
  page and a Worker still own unfinished responses, and closes both transports.

`make BUILD=build-worker-fetch-fixture test-worker-fetch-san` passes **41/41**
checks normally and under ASan/UBSan. Three permanent negative controls are
prerequisites of the positive gate:

| Control | Exact observed failures |
| --- | --- |
| `WORKER_NO_FETCH` | 1, the initial Worker Fetch presence check |
| `WEBAPI_FETCH_NO_OWNER` | 2, foreign-owner abort refusal and peer body survival |
| `WORKER_FETCH_NO_PUMP` | 3 of the six startup/liveness checks: worker text, bytes, and script-relative request |

The owner control retains cleanup in the request's original context: it
measures the ownership guard without freeing values through another runtime.
No control reads freed memory or reproduces third-party exploitation.

The existing pure URL/Encoding Worker gate now explicitly compiles with
`WORKER_NO_FETCH`, preserving its original noFetch assertion and its exact
13-failure negative control. It passes **21/21** with the feature isolated.
`test-mk-wired` passes with 288 fragments, 287 reachable directly and the one
declared standalone wrapper. Full output is
`build-worker-fetch-fixture/final-gates.log` and source fingerprints are in
`final-source-hashes.txt`. Darwin ASan does not provide leak detection; the
sanitizer command therefore uses `detect_leaks=0`.
After the implementation agent's final parent-terminate immediate-reap change,
the current and sanitizer binaries were rebuilt from fresh sources and both
again pass **41/41**. Those final runtime logs are
`final-runtime-current.log` and `final-runtime-san.log`; the source fingerprints
were refreshed after that run. The independent controls above retain their
earlier exact observed failures; their injected ownership/pumping conditions
were unchanged by the final cleanup scheduling correction.

The first 37-check run had one fixture error: a Blob self-close test used a
relative URL, so the expected opaque-base TypeError arrived before close. That
test now uses an absolute URL and first proves that a real held transport
exists. The final close assertion therefore measures actual request cleanup.

This subtask changed only its test files, Make include/wiring, the explicit
pure-global test build flag, and this report. Production implementation belongs
to the browser-context agent. No external request, account input, VM operation
or user disk access was performed by this fixture task.
