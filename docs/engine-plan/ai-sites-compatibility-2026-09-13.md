# Z.ai / Kimi compatibility — 2026-09-13

Work in progress. A rendered landing page and local API checks do not prove authenticated chat. No successful Z.ai or Kimi answer has been observed in this run.

**Current artifact update:** Cleanup removed the old repository build directories. Recovered inputs and subsequent frozen v4/v5/v6/v7/v8 builds are retained outside the repository at `/Users/wangzhe/.codex/artifacts/logitos-ai-sites-20260913`. See the [current v8 acceptance](/Users/wangzhe/.codex/artifacts/logitos-ai-sites-20260913/v8-acceptance.md) for integrated form/text/spacing/click/SVG fixes, actual phone-page logo rendering and retained-data upgrade/restart evidence. The remaining sections below describe the earlier state; their pending items and deleted build paths are historical, not the current status.

## Verified changes

| Change | Evidence | Remaining boundary |
|---|---|---|
| Typed gap and logical padding; percentage padding basis | 144/144 normal and sanitizer checks; actual Z.ai settled card gaps 12/12, composer inset 12/10, textarea inset 12/12 | Cyclic percentage gap and complete logical/physical cascade not covered |
| Button automatic size includes authored edges and line height | 110/110 normal and sanitizer; exact old behavior 30 failures; default and explicit dimensions preserved | Current real-site measurement still blocked by delayed module loading |
| Real document focus / WM notification / iframe focus | Host 19/19; native guest 7/7 including address bar, Finder, and frame focus | Existing Window/Document target alias remains |
| IndexedDB versionchange scope updates | 27/27 normal and sanitizer; exact old behavior 13 failures; IDB regression 42/42 | Not full IndexedDB conformance |
| Template innerHTML updates retained content | 21/21 ordinary DOM checks; old-source control 8 failures; native command callback retained | Existing template child-list, cloning and some outer serialization limitations remain; real Kimi check running |
| Private 4xx Response.json diagnostic | On/off 145/145; original stream 62/62 and XHR48/48; fixed enum privacy controls | Only categorical metadata; not a request/response body recorder |

## SMS observation

The user manually attempted Z.ai SMS login and reported no SMS and no obvious page error. Native request IDs correlate POST requests to chat.z.ai with HTTP422. Response.json detail is a string, with no recognized structured validation type/location. These observations do not establish that SMS dispatch occurred, or identify whether phone format, verification data or another business rule caused rejection. No authenticated request, challenge replay or synthetic success was injected.

The public frontend maps status422 to a phone-format message unless a recognized business code selects another message. The current categorical diagnostic did not capture the business code, so this is a frontend mapping, not a verified backend diagnosis. More SMS attempts are not requested while the browser issues below are being repaired.

## Rendering and resource work still active

- Backface visibility: a normal two-sided card fixture confirms hidden back faces are painted. Shared paint/hit handling and computed style support are being implemented. This is not full 3D projection/depth support.
- Z.ai module queue: multiple essential modules wait about60000 guest milliseconds and fail. Private diagnostics show RQ_QUEUED, no fd, pool total3/origin1/mux1; the matching session is still HP_PROTO_PENDING with refs2, socket bits7 and no H2 state initialized. A bounded normal pending-owner fix is in progress.
- Kimi real site after the template change is being checked. Earlier hasFocus and IDB exceptions disappeared; the template error and timer watchdog have not yet received final after-build acceptance.

## Controls and build provenance

Artifacts are under build-ai-sites-0913. Kernel and browser were built from source-v1, a stable 6680-file capture with explicit subsequent overlay manifests. Independent output directories and clean test profiles are used. Release hashes and source overlay hashes are stored alongside each image. Source-v1 is an evolving integration tree; a release manifest identifies the exact layers present at its build time.

Old browser plus the same new kernel rendered Z.ai fully. Disabling new focus notification calls alone did not remove the delay. Restoring only old spacing parser behavior also did not remove the delay. Subsequent native request diagnostics identified the queued pending-session condition; the earlier controls alone were insufficient to name its cause.

Live-driver corrections: reconnect initializes mouse position from the guest report; display geometry uses the guest-reported pixel size and scale, not the requested QEMU size. Both failures were harness issues and are not reported as product fixes.

Private request/account logs are not delivery artifacts. Use categorical JSON reports and clean-site captures. The integration diagnostics do not log request or response bodies.
