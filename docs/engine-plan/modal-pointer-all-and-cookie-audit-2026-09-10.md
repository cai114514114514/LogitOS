# Modal pointer targeting fix and Cookie audit — 2026-09-10

Follow-up: the findings below were implemented and validated in
[Cookie repair, request context and fetch diagnostics](cookie-context-and-fetch-2026-09-10.md).
The audit's original evidence and scope statements are retained as history.

## Scope and outcome

The user's immediate blocker was a painted modal that did not receive clicks in
the LogitOS browser. The general HTML-box targeting defect is fixed and verified
with an ordinary local modal in the real guest. The subsequent request was to
review Cookies: that review is complete below; Cookie product code was not changed.

This does **not** close the broader real-site rendering/performance work, the
missing CAPTCHA instruction text, or DeepSeek login/first-token acceptance.
No CAPTCHA answer was selected, no verification bypass was added, and no live
Cookie values were exported for this audit. The fixed visible guest was opened
at the DeepSeek login page, with an empty phone field, for manual use.

## Why the modal was painted but not clickable

The old guest's native `down-hit`, `up-hit`, and `click-dispatched` diagnostics
repeatedly targeted the outer overlay for points inside the painted image.
`about:boxes` placed the image at page coordinates 263,195 with size 294x147;
the observed points were inside it, so this was not a global pointer offset.
The actual loaded stylesheet contained a fixed wrapper with
`pointer-events:none` and a descendant dialog with `pointer-events:all`.

LibCSS accepted only `auto` and `none`. Rejecting `all` silently left the dialog
and its children inheriting `none`; both native and CSSOM hit testing therefore
skipped them and reached the overlay. The correction adds the distinct `all`
value to parsing, bytecode, cascade, and computed serialization. The existing
HTML target filter excludes only computed `none`, so descendants can escape the
wrapper without changing page CSS or removing the overlay.

No hostname, URL, framework, or challenge-bundle branch was added. Precise SVG
fill/stroke/path targeting remains outside this patch. The standards distinguish
HTML hit-test control from the SVG value family; the implemented and measured
claim here is HTML-box compatibility, not complete SVG hit geometry.
See [CSS UI 4](https://drafts.csswg.org/css-ui-4/#pointer-events-control) and
[SVG 2](https://www.w3.org/TR/SVG2/interact.html#PointerEventsProperty).

## Verification

- Before the fix, the extended host fixture reported **17 failures / 80 checks**.
- Current `test-pointer-events`: **81 checks passed**. The extra check commits
  hidden-dialog geometry explicitly. Both native targeting and `elementFromPoint`
  are checked; actual button callbacks are checked separately in the guest.
- `test-pointer-events-sanitize`: **81 passed**, AddressSanitizer and UBSan;
  leak detection disabled on this host. The separately linked LibCSS archive
  is the normal host build, not an instrumented archive.
- Test-apparatus correction: direct host hit calls must settle pending layout,
  as the browser does before its next native input. Reading specified style
  alone does not flush. Native-first/CSSOM-second otherwise manufactures a
  stale-layout disagreement. No product test was removed to hide that failure.
- The original `CSS_POINTER_HIT_LEGACY` control and the new
  `CSS_POINTER_ALL_HIT_LEGACY` control were observed failing and are prerequisites
  of the positive host gate. The latter reports both native and CSSOM failures
  for `all dialog escapes none wrapper`.
- Guest negative control: the old immutable disk received the button-location
  click as a second `MODAL-ALL SHADE ... outside=2`, with the ACTION marker
  absent. Failed boot or absent fixture cannot satisfy this control.
- Guest positive: actual native clicks produced **ACTION 1 → CLOSED → REOPENED
  → ACTION 2**, with the expected ancestor bubbling. Screenshots visibly show
  `CLICKED 1`, no modal after closing, and `CLICKED 2` after reopening.
- `make test-mk-wired`: **242 fragments, 241 reachable, 1 declared**.
- An isolated `make -j8 BUILD=build-modal-hit-fix` built both
  `build-modal-hit-fix/logit.iso` and **`build-modal-hit-fix/disk.img`**, including
  the ring-3 browser. Unrelated shared changes were preserved; no commit made.

The guest gate uses the same baseline kernel ISO with old/new immutable app
disks. It does not derive a browser performance speedup from host wall time.

```sh
make BUILD=build-modal-hit-fix test-pointer-all-modal-guest \
  POINTER_ALL_GUEST_ISO=build-modal-hit-fix/baseline/logit.iso \
  POINTER_ALL_GUEST_NEG_DISK=build-modal-hit-fix/baseline/disk.img \
  POINTER_ALL_GUEST_DISK=build-modal-hit-fix/snapshot/disk.img
```

Artifact root: `build-modal-hit-fix/`. Evidence: `all-before.log`, `all-after.log`,
`guest-gate.log`, `site-general/layout/pointer-events/modal-guest/results.json`,
`pointer-all-first-click.png`, `pointer-all-closed.png`, and
`pointer-all-reopened.png`. The visible guest's writable disk is a separate clone
at `handoff/disk.img`; its login screenshot is `handoff/deepseek-fixed.png`.

Immutable disk SHA-256:

- Old: `59c263fdb90b3a5d2263c30df6ec417c336028033babde1f42365262148e109b`
- Fixed: `dc020c9099749dbe325b8a418169d9ea0b204f3c21c529b1ab06cabf0e3f524d`

## Cookie audit — findings, not implemented fixes

Source-reviewed: `c/net/http/cookies.[ch]`, the Cookie paths in
`c/apps/browser/js_webapi.c`, and navigation/redirect/cache wiring in
`c/apps/browser/browser_rt.c`. Findings below are static evidence, not live-site
exploitation tests or proof that a particular site's login failed because of them.

1. **High priority: Secure integrity protection is incomplete.**
   `cookies.c:670-719` checks the new cookie's Secure flag, but lacks the
   existing-Secure-cookie protection before replacement/deletion. Restoration
   should enforce the storage rule before either operation, rather than only
   preventing plaintext transmission. The comment claiming the Secure rules
   are complete must be corrected alongside the eventual fix.

2. **High priority: SameSite classification is schemeless.**
   `cookie_same_site` (`cookies.c:276`) accepts only host strings;
   `js_webapi.c:529` and `:1019` use that answer without comparing schemes.
   A single authoritative request-context classifier should include the scheme,
   initiator, and navigation context. Modern same-site comparison includes the
   scheme; see [HTML, Sites](https://html.spec.whatwg.org/multipage/browsers.html#same-site).

3. **High priority: SameSite is missing on Cookie creation.**
   `webapi_cookie_store_line` (`js_webapi.c:535`) and `fetch_take_cookies`
   (`:1081`) supply host/path/security/API type, but no site or navigation context
   to `cookie_set`. Its SameSite restriction is on outbound retrieval only.
   Apply the storage-side cross-site rule while preserving top-level navigation
   behavior and the independent credentials policy.

4. **High priority: the public-suffix approximation is not a complete boundary.**
   `cookies.c:10-47,179-287` documents a short suffix table and a ccTLD heuristic.
   It can over-reject and under-reject, and the same approximation feeds SameSite.
   The old characterization as simply conservative is incomplete. Use a maintained
   PSL, including its wildcard/exception handling, from one shared authority.
   No new site-specific exceptions were added in this audit.

5. **Session usability: persistent Cookies are not persisted.**
   `js_webapi.c:440-466` owns only a process-wide in-memory jar. It survives page
   navigation (`:4759`) but not process exit. Expires/Max-Age still operate while
   the process lives. The historical filesystem-durability rationale is recorded
   in code; this audit did not revalidate that filesystem claim. Persistence needs
   its own durable-storage/restart acceptance, not just a serializer.

6. **Prefix validation has incomplete edge conditions.**
   `cookies.c:682-688` does not test prefix-only names and checks the resolved
   root path rather than requiring an explicit Path attribute for `__Host-`.
   Complete these validations in the shared parser/storage rule; do not compensate
   in document.cookie alone.

7. **Header pressure can still silently lose state.**
   `cookies.c:830-853` stops at the first whole cookie that does not fit. A partial
   header is reported as an ordinary positive length. Sharing `CK_HEADER_MAX=8192`
   fixed divergent caller limits, but did not make total overflow unreachable.
   Keep request serialization and cache keys aligned, and expose truncation
   diagnostics without logging Cookie values.

8. **Lower-priority compatibility gaps remain.**
   Max-Age accepts a leading plus despite the nearby stricter comment; duplicate
   invalid Path/SameSite attributes can leave an earlier value active; limits are
   per-name/per-value rather than their combined size. Review these together
   with expiry limits. Typed navigations also deliberately use the previous
   document as initiator (`js_webapi.c:511-523`), which can under-send Strict
   Cookies once. That residual is documented, not a newly measured site failure.

The reference for storage, prefixes, SameSite creation, parsing and public-suffix
handling is the [HTTPWG 6265bis draft](https://httpwg.org/http-extensions/draft-ietf-httpbis-rfc6265bis.html)
(sections 5.6–5.8 and 8.9); this is a draft, not a claim that every rule comes
from the older [RFC 6265](https://www.rfc-editor.org/rfc/rfc6265.html).

## Cookie checks actually run

`make BUILD=build-modal-hit-fix test-cookie-jar test-cookie-cors` exited 0:

- Jar: **220 checks passed** (the fragment's older 216-check comment is stale).
- Cookie/CORS integration: **54 checks, 0 failures**.
- Prerequisite response-boundary gate: **28 checks, 0 failures**.
- Four jar negative controls failed exactly their expected cells; the transport
  negative control also failed as expected. These are pre-existing local fixtures,
  not tests against the user's account or a third-party endpoint.
- The same 220 jar checks passed an additional ASan/UBSan build with leak
  detection disabled; the compiler emitted existing test `sprintf` deprecation
  warnings, not sanitizer errors.

Logs: `build-modal-hit-fix/cookie-audit-gates.log` and
`build-modal-hit-fix/cookie-jar-sanitize.log`. These gates verify their covered
rules, **not** the newly identified gaps, persistent login across restarts, or
real-site authentication. No Cookie implementation was changed by this audit.
