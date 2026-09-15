# Component and embedded-context wiring audit — 2026-09-09

This is a production-source audit with executable browser-loop specimens. Iframes
are one example of the wider problem: a component API can exist while parser
startup, native input, task delivery, or rendering never reaches its consumer.
No fix branches on a site, vendor, framework, or bundle name.

## Findings, ordered by impact

1. **Sandbox refusal had a blank-document bypass (repaired here).** The engine
   explicitly does not implement sandbox permissions and promises `SecurityError`.
   `js_platform.c`'s `navigate()` selected the blank-source fallback before either
   guarded loader, so missing/empty `src` and empty `srcdoc` yielded readable
   documents whose injected scripts actually executed. The diagnostic log contains
   `[frame] AUDIT-SANDBOX-EMPTY-FRAME-SCRIPT-RAN`, not merely API-presence evidence.
   `navigate()` now refuses before source selection; `startLoad()` also refuses
   before malformed-URL fallback because attribute setters call it directly.
   This preserves refusal, **not sandbox support**. The fixture tests four source
   forms and attempts actual script insertion. The negative control restores the
   three blank-source bypasses and requires three observed execution markers.
   A second prerequisite control restores the malformed-URL return before refusal;
   it was observed red with refused=3, one real injected execution, and two failed
   assertions (`frame_url_review_negctl.log`). Positive refused=4 and injection=0.

2. **A child execution realm is not a working embedded Window (unresolved).**
   `js_frame.c:220` installs only `window`, `self`, and console in the fresh realm;
   it does not install child DOM, timers, network, parent/top, or message listeners.
   The executed child diagnostic reports
   `document=undefined parent=undefined postMessage=undefined timer=undefined`.
   Meanwhile `js_platform.c`'s `makeWindow()` returns a parent-realm plain object:
   a property written by the child script is absent on `contentWindow`, and its
   `location` getter returns the parent's `G.location` (identity observed true).
   Thus `contentWindow.location.href=...` targets the parent Location object.
   `win.postMessage` is a no-op; there is no child receiver, meaningful cross-frame
   `event.source`, or origin-checked cross-context delivery. The separate real
   MessagePort work does not repair this seam. Minimum honest interim changes are
   explicit refusal/absence for unsupported entry points; full repair needs a
   WindowProxy and per-context DOM ownership, not more properties on this object.

3. **Embedded pixels and input have no child consumer (unresolved).**
   `js_frame.c:90` already records “NO PIXELS”: child documents stay in DOMParser
   arenas, while `layout_page`, the display list, and painter remain page globals.
   `layout.c` mentions iframe as a replaced/margin category, but does not lay out
   or composite its `contentDocument`. `focus.c:154` admits the iframe element;
   it does not route keyboard/focus into a child document. Parent hit testing
   cannot target unpainted child content. A real fix needs a child viewport,
   document-specific layout/display lists, compositing and input retargeting.
   Inline child script execution is not proof of a rendered interactive widget.

4. **Parser initialization skipped a real script consumer (repaired here).**
   `js_page.c` installs DOMParser before platform. Platform's prelude synchronously
   navigates its native snapshot of parser-created frames; `settle()` only adopts
   a child when `__frameAdopt` exists. `js_frame_install()` previously ran after
   that prelude, so an identical dynamic `srcdoc` script ran and the parser one
   did not. It now runs before the prelude, after its DOMParser dependency.
   The actual `browser.c` `app_main` fixture observed **1 child execution before,
   2 after**, with unchanged child script bytes. The old late-install order is a
   required negative control. The older direct `__frameAdopt` unit test could not
   find a missing production installer call; the bootstrap scan test only read
   child text, so neither established this behavior.

5. **Source attribute navigation is inconsistent (unresolved, reproduced).**
   `navigate()` tests nonempty `srcdoc` rather than attribute presence;
   `setAttribute('src')` calls `startLoad()` even when `srcdoc` exists; removal
   does not enter the same navigation algorithm. The diagnostic specimen observes
   a `SecurityError` for empty-srcdoc plus a foreign src, another after changing
   src while srcdoc remains, and the old document retained after srcdoc removal.
   The bounded follow-up is one source-selection function for parser, insert,
   attribute/property changes and removal, with srcdoc-presence priority. Test
   actual document/script replacements, including empty source and reinsertion.

6. **Redirect admission checks only the requested origin (source evidence;
   transport reproduction still needed).** `startLoad()` checks `u.origin`, then
   fetches and adopts response text without checking the final `resp.url` origin.
   The fetch implementation follows redirects (`js_webapi.c:1280`) and supports a
   same-origin mode that rejects foreign redirects, but this caller uses default
   mode. Under the engine's current cross-origin refusal policy, request that
   strict mode and verify the final response URL before adoption. This audit did
   not execute a cross-origin redirect server, so it does not claim a reproduced
   data-exposure exploit.

7. **Native component default actions bypassed their JS implementation
   (repaired this round, broader than iframe).** Native mouse/keyboard activation
   reached `browser.c::control_activate()` without the invoker branch that
   `.click()` used. Popover opened only through the JS path; the same missing
   consumer affected command buttons. The shared `activateInvoker()` is now
   retained privately by `js_semantics`, invoked after uncanceled native click,
   with runtime budget and close cleanup. It does not redispatch click or run
   checkbox activation twice. The real browser-loop gate drives mouse, outside
   dismissal, Escape, Enter, command activation and prevented default; it observes
   exactly eight queued toggle events. Disabling the native bridge leaves stage
   3 at zero events, reproducing the earlier guest symptom.

## Evidence and acceptance boundary

- `tests/fixtures/engine-expansion/frame-bootstrap.html` is an ordinary page used
  by `tests/unit/frame_bootstrap_wiring_test.c` through actual `app_main`. It
  records child console execution, parent DOM integrity and refused sandbox
  injection. It does not call `__frameAdopt` or pump page jobs itself.
- `tests/frame_bootstrap_wiring.mk` makes late-install and both sandbox-bypass controls
  prerequisites. Outputs live under `build/wiring-next/` in the single build tree.
  Baseline logs: `frame_bootstrap_before.log`, `frame_bootstrap_after.log`;
  latest combined positive: `frame_bootstrap_review.log`.
- `tests/fixtures/engine-expansion/frame-widget-audit.html` is a diagnostic
  specimen, not a passing acceptance fixture. Its output is preserved in
  `build/wiring-next/frame_widget_audit.log`; retained defects above are expected
  observations and must not be presented as a green iframe-support result.
- Popover evidence: `popover_native_before.log` shows native stage 3/event count 0;
  `popover_native_review.log` shows the full actual-input loop passing. A test
  locator initially clicked the panel's repeated “Outside” prose instead of the
  external button; selecting the first painted occurrence fixed the apparatus,
  with no light-dismiss production change.
- These are host tests of production browser-loop code, **not guest completion**.
  Root owns the single disk build and final guest mouse/keyboard/script checks.
  Child painting, real WindowProxy, sandbox permissions, cross-frame messages,
  navigation precedence and redirect-origin verification remain explicitly open.

The reusable repair order is component lifecycle first, then context ownership,
message/event delivery, and rendering/input consumers. Adding more API names
before those consumers exist would repeat the defects this audit demonstrates.
