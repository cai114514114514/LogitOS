# Live specimens to general browser defects — 2026-09-09

Scope: GitHub, Bilibili image cards/titles (no video playback), z.ai, DeepSeek,
Apple and Bing homepage/search. No production hostname/framework/bundle branches,
identity spoofing, challenge bypass or forced site visibility were added.

## Measurements and what they mean

All durations below are guest monotonic `[load-perf]` milliseconds, converted to
seconds. Each row is one live network navigation in QEMU (1 GiB, four TCG vCPUs).
They are observations, not controlled network benchmarks or medians. In
particular, `scripts_execute` includes nested module downloads and is not pure
JavaScript CPU. The scoreboard's host `load_seconds` is retained only as driver
metadata; it is not the performance measurement.

| Specimen | Baseline total s | Intermediate total s | Observation |
|---|---:|---:|---|
| GitHub | 93.42 | 149.14 | CE initialization exception gone; the now-running module graph exposes watchdog charging network wait as script time. Not an overall speedup. |
| Bilibili | 7.54 | 8.10 | Card/navigation layout still fails; no video-play claim. |
| z.ai | 56.03 | 20.65 | Device-width query loop no longer interrupted; backend fetch still fails. |
| DeepSeek | 3.14 | 3.07 | Invisible mobile menu no longer covers desktop content. `/en/` redirect is a separate load. |
| Apple | 13.30 | 11.32 | Boxless navigation wrapper support; DOMMatrix/gallery gap remains. |
| Bing home | 2.40 | 2.45 | Search/control rendering still under investigation. |
| Bing Python | 4.51 | 5.39 | Result text appears then disappears; event/lifecycle ordering investigation ongoing. |

Raw phase records are `build/site-general/phase-comparison.json`; individual
baseline and intermediate directories contain original serial, host specimen,
result JSON and PNG. `after-runtime/artifacts.json` and `after/artifacts.json`
name their linked binaries/disk; later edits do not inherit those artifacts'
verification.

## General defects, production consumers and boundaries

- **Custom elements:** `createElement` must synchronously initialize defined
  custom elements, before an append. A live bundle reads a constructor-created
  delegate immediately. Connection/reconnection/fragment/move paths now run the
  callbacks; the original TypeError is absent in the guest. Dedicated gate 12/12.
  The existing platform suite has the same four failures before and after (226
  checks); it is not reported green.
- **Media queries:** the actual device width/height, separately from viewport
  width/height, feeds both stylesheet media matching and JS `matchMedia` and
  `screen`. The original exact script made 248,073 queries and was interrupted;
  the fixed host replay makes 56 queries and completes. Guest script phase fell
  from 52.66 to 16.97 seconds, but backend requests still fail. Unavailable screen
  work-area dimensions are absent rather than invented.
- **Definite percentage heights:** nested definite content/padding boxes,
  zero/calc/replaced/fixed sizing use the appropriate containing block, not a
  fabricated viewport basis for auto ancestors. Dedicated gate 41/41; guest
  geometry specimen passes. Flex/grid post-distribution auto bases remain a
  separate boundary.
- **Invisible groups:** exact-zero ancestor opacity suppresses descendant paint
  while preserving layout and pointer hit testing. Fractional group compositing
  is not claimed. Native Show/Hide clicks ran in the guest; independent dirty
  region probe also checks a zero-height ancestor with overflowing child.
- **Boxless wrappers:** display:contents is parsed and flattened for formatting
  without changing DOM parentage. The first guest fixture caught a second door:
  CSSOM's fallback union incorrectly gave the boxless wrapper a rectangle. This
  integration failure is retained; follow-up fixes must pass the same fixture.
- **Grid font units:** rem tracks use root font size; em tracks use local font
  size. Dedicated positive and old-behavior negative control exist.
- **Late callbacks:** production event loop now drains inserted scripts and
  consumes navigation after scroll/load listeners before indefinite park.
  Host old behavior fails three modes; actual guest Shift-wheel followed by no
  extra input runs direct navigation, script insertion and script navigation.
  This supersedes the earlier review-only handoff status.
- **Caret geometry:** address measurement/paint use one font and UTF-8 boundary
  hit testing; long controls measure and paint identical bounded UTF-8 runs,
  preserve caret clip margin, and map password hit testing to mask glyphs. JS
  selection offsets convert native bytes to UTF-16, with directed native
  selection. Four real native End/Left/Z/click sequences now read the right
  ASCII/CJK/mixed/password values and offsets. However screenshot inspection
  exposed missing control paint for flex/grid items, so readbacks alone are
  explicitly not visual acceptance. A separate fix is in progress.

Caret limits: native offsets cannot represent a caret inside a UTF-16 surrogate
pair and clamp to the preceding scalar boundary. Full grapheme/complex-script
shaping, multi-line selection paint, and length-changing text-transform source
mapping are not complete.

## Failures retained rather than hidden

The current Bing specimen has both a hide branch and a later restore branch.
It is not sufficient to call disappearance an intentional refusal: event ordering
must be traced. No visibility override or fabricated cookie is used.

Apple gallery reads all 16 DOMMatrix fields from computed transforms. Adding an
identity-only constructor would disguise the separate missing computed-transform
consumer. See `build/site-general/runtime/apple-matrix-audit.md` for exact bundle
consumers and implementation/verification requirements. This interface is not
claimed implemented by the present batch.

z.ai telemetry CORS refusal is separate from same-origin backend fetch failure.
The old diagnostic merged request serialization and protocol start failures;
production now distinguishes them and preserves HTTP/2 close/error/GOAWAY state.
This instrumentation alone does not fix transport or prove server intent.

## Integration status

The main disk was rebuilt, all 212 then-current fragments were reachable, and
`make -j3 test-browser-expansion` exited 0 before the later integration-discovered
fixes. Every new functional gate has a prerequisite old-behavior control that
was observed failing. This green run is not reused for changes made after it.
Guest evidence is deliberately separate: the caret readback sequence passed;
late callback modes and opacity clicks passed; display:contents CSSOM failed.
Final reruns and artifact hashes are required after remaining edits settle.


## Integration correction after native input and paint verification

The preceding table and pending claims describe an intermediate build, not the
final implementation. The following evidence supersedes those claims without
removing the failures that exposed the missing consumers.

- `display:contents` now also returns no CSSOM box: the same guest specimen that
  exposed the fallback-union defect passes with a zero-size wrapper and intact
  child layout. Grid rem/em, percentage grid items, and positioned descendants
  of auto-height containing blocks pass their actual guest geometry checks.
- Flex/grid items now emit their own form-control payload, so the native caret
  tests both paint text and read back the resulting edit. ASCII 1600, CJK 600,
  mixed 800 and password 1400-character fixtures passed End/Left/Z and pointer
  placement. Screenshots are in `build/site-general/final-guest/`; input values
  are recorded in its `results.json`.
- Control chrome now takes author computed background/border styles. Apple's
  white vertical navigation bars were a real regression in the intermediate
  build; the latest native screenshot has no such bars. This does not prove
  hero media or full page fidelity. Latest guest total was 15,620 ms, including
  11,730 ms in script execution plus nested downloads; it is not a speedup claim.
- DOMMatrix/DOMMatrixReadOnly construction, matrix fields and actual computed
  transform serialization are connected. The guest fixture passes both its
  initial geometry and a native click changing the reference width. Unsupported
  matrix arithmetic methods remain absent. Anchor URL components were already
  implemented; a partial host probe omitted their translation unit. The corrected
  official URL corpus run passes 1175/1175 and the guest component fixture passes.
- The module watchdog excludes nested synchronous network waiting while retaining
  CPU/fuel limits. GitHub's original CE exception and the subsequently exposed
  module watchdog exception are absent in the after-watchdog guest run, but that
  navigation still took 126,280 ms (109,020 ms script phase). Thus functional
  progress did not make GitHub fast. z.ai's same wave took 17,770 ms versus the
  56,030 ms baseline; its blank page/backend transport failure remains unresolved.
- External classic scripts now dispatch their actual load/error events, including
  a successful empty 200 response. An exception thrown by an executed script is
  not a resource-load error. Dynamic script descendants execute in the guest.
  Module/TLA completion events are not fabricated by this classic-script path.
- Those late script fetches resolve against the live page URL instead of the
  address bar's unsubmitted edit buffer. The native address-edit fixture types a
  draft while a timer inserts scripts and still observes all five expected
  resource events and the chained script. This is not a claim of complete dynamic
  base/history semantics.

### Final guest fixture accounting

`build/site-general/final-guest/results.json` records **11 completed cases**.
The driver then failed before navigating to case 12: it kept the startup tab plus
11 specimen tabs and reached `TAB_MAX=12`. This was a harness limit, not a missing
late-callback marker after a real navigation. The driver now closes each finished
specimen tab. `final-guest-tail/results.json` adds **three completed cases** in a
fresh guest: late callback mode 2, native opacity Show/Hide, and definite percentage
height. The original failure log is retained. These 14 distinct case results are
not a claim that the original 13-case command exited successfully.

### Remaining live-site failures and concurrent network integration

Bilibili's grid navigation spacing, card image boxes, stats and titles now have
usable geometry in `integrated-sites/bilibili`. Images still wait: the latest
guest diagnostic has 23 nonempty image sources, 17 unanswered images, and eight
queued requests. The source inventory and HTTP/2 pool witness are recorded in
`build/site-general/runtime/bilibili-images-audit.md`. Admission of a new socket
is checked before attempting to join an existing HTTP/2 session. The production
pool can refuse a new connection while still permitting that join. The repair
must distinguish reuse from a fresh dial; simply removing connection caps is
incorrect. No host-specific image workaround was added.

`browser_rt.c` and `bfetch.h` are concurrently owned by another group. At the
latest source capture the dial contains its `TEMP-B` experiment (raw HTTP/1
socket creation while the request is marked as bxfer-owned). Our group has not
rewritten or adopted that experiment. `final-relink.log` records a successful
real browser ELF link, AEX packaging and disk rebuild, but successful linking
cannot establish transport correctness. `final-gates.log` exits 2 at
`test-cache-invalidation`: **652 checks, 35 failures**. Its prior run also failed
while an external bxfer declaration was still incomplete. Neither run is green.
The earlier aggregate success remains evidence only for that earlier build.
A full aggregate rerun and live image/backend checks are required after the
network owner settles this code.

All new outputs stay under `build/site-general`; no global scratch cleanup or
unrelated agent process was touched. The latest completed fixture PPMs were
losslessly converted, pixel-verified against PNG, and removed (79,872,416 bytes).


### Correction from the native Bing investigation

Native `FORM-GET /search?q=Python&form=QBLH...` proves the input value and Enter
submission. It does **not** prove a valid search-page navigation: the existing
about:boxes diagnostic left the address edit buffer set to that diagnostic, and
`follow_link()` still used this buffer even after late script fetching had been
fixed. It consequently attempted to load the bare `/search?...`. This second
producer is now being repaired alongside the no-action form producer, with native
link/relative-action/no-action controls. The 60-second blank-search observation
is retained as an invalid-normal-search attempt, not blamed on the remote site.
The shared disk also changed during that VM's lifetime. The perf driver now
records input hashes before boot/after shutdown and rejects artifact drift;
its two-tab harness check completed with unchanged inputs.

The restored external network source (TEMP-B removed at 21:13) was independently
retested: `cache-after-external-restore.log` still reports 652 checks / 35 failures.
Therefore those failures cannot be attributed solely to TEMP-B. The old disk and
the restored source are distinct artifacts. Our isolated set of newly added
layout/caret/runtime gates did exit 0 in `owned-final-gates.log`, including their
observed-red controls; that does not cancel the aggregate cache failure.


## Assembled native navigation, positioning and transparent-box acceptance

After the two owners froze layout and native tests, root applied the transparent
box patch and rebuilt the actual browser ELF, AEX and disk. The successful link
and packaging are in `assembled-disk.log`; `assembled-artifacts.json` identifies
the images. Every captured browser source hash was identical before/after that
build and the following six-case guest run.

`assembled-guest/results.json` records **six completed native cases**, driver
exit 0 and unchanged ISO/disk hashes:

1. Percentage top plus opposing inset auto margins: native Python input/Enter
   reached the form; native page scrolling retained the fixed control's geometry
   and elementFromPoint target. The screenshot contains the input and PASS
   geometry after a real 40 px scroll.
2. Relative link after an unsubmitted address edit reached the correct origin.
3. Relative-action GET submission after that edit reached the correct origin.
4. No-action GET submission used the current document URL.
5. A native Ctrl+T/Ctrl+W round trip preserved the saved document address, then
   the relative link reached the correct destination. An earlier suspicion of
   corrupted JS hydration origin was disproved: hydration already uses ht->base;
   saved tab metadata/chrome and the old link resolver were the affected paths.
6. Blank transparent boxes received native clicks: an overlay covered its link,
   preventDefault canceled a second link, and a third link's unpainted right edge
   actually navigated. No synthetic event dispatch or direct destination URL
   substitutes for these actions.

The new host gates are connected to `test-browser-expansion` and each executes
its old-behavior control first. Positioning is 70/70 (percent and margin controls
fail 18 and 9), fixed projection is 18/18 (control fails 6), transparent boxes
119/119 (control fails 51), and native document-base navigation has four passing
modes with observed wrong-origin negatives. The fixed boundary also reaches
selection geometry, nested element scrolling, nearer top layers and
scrollIntoView. Calc insets and transformed ancestors establishing a fixed
containing block remain unsupported; no identity or guessed value hides them.

The live Bilibili assembled specimen has compact navigation and correctly sized
240x135 cards with stats, titles and authors visible. Guest total is 9490 ms,
full style/layout 620 ms and scripts_execute 6530 ms; the latter still includes
nested downloads. Images remain unresolved: 16 unanswered/pending, first eight
slots state/status 0, load event still owed. The diagnostic has 693 display items
(dynamic peak 1085, below the 16384 capacity), so this capture does not show a
new item-limit truncation from transparent boxes. Timers, cross-origin callbacks
and request timeouts still report errors; this is not site compatibility PASS.

The first native navigation baseline fixture omitted a harness load marker, then
another attempt clicked outside painted link text before the transparent-box
fix. Both apparatus failures are retained. The corrected pre-fix run at
`navigation-guest-before-activated` proves real activation followed by the broken
bare `/navigation-target.html` load; the assembled run reaches the document.


## Final integration update: asynchronous scripts and safe form navigation

Correction to the earlier assembled evidence: its Bing native input was eventually consumed only after consecutive guest late-script waits of 62880, 60010 and 60010 ms. That ruled out the speculative pointer-overlay explanation for that delay. browser.c now keeps a pending script's request and canonical DOM handle across outer frames, pumps once per drain call, and returns to native events while downloading. FIFO order, resource load/error events, prepared source snapshots and cancellation remain wired. Initial parser batches, module dependency downloads inside QuickJS, and general force-async scheduling are still separate limitations.

The new queue exposed two additional lifetime boundaries, both fixed rather than hidden by the apparatus. First, retiring the last script could leave the initial load event pending with no wake source; the outer loop now rechecks load after the drain. Second, form.submit()/requestSubmit() could synchronously destroy the realm from inside a running event handler. The real Bing async-first core, extracted alongside its exact AEX, has the chain __JS_FreeValueRT -> JS_CallInternal -> JS_Call -> invoke_at -> dispatch_event -> js_dom_dispatch -> form_submit_ex -> app_main. Its result page loaded before the old stack faulted.

Form navigation now resolves against the committed document URL and writes the SAME pending navigation record as location requests through a native C producer. The existing outer consumer loads only after JS dispatch returns. This deliberately does not call the location.href setter: its same-document shortcut would wrongly swallow a same-URL form reload. Realm close discards its remaining navigation requests, including requests from pagehide. The new host modes show the callback's after statement, submit/requestSubmit event differences, an actual same-URL second request, and both form/location write orders. Restoring the synchronous path crashes both submission modes at the specific JS_FreeRuntime gc_obj_list assertion before the after statement; those strict controls are prerequisites.

The final disk was rebuilt with make -j6 build/disk.img (real ELF link, AEX packaging and filesystem rebuild). Browser and LibCSS source hashes were unchanged throughout this build. The eleven-case final-integrated-guest run completed with unchanged ISO/disk hashes: initial inserted-script chain without a native wake, input during a held script, both script form methods triggered by native Enter, pointer-events passthrough/explicit-auto/bubbling, classic resource events, resource completion during address editing, positioned fixed input plus scroll, transparent-box default actions, relative form action and no-action form navigation. Screenshots and compact JSON are retained under evidence/site-generalization-2026-09-09; full logs remain under build/site-general.

The initial load console marker precedes settlement and the compositor, so the driver's first generic capture still showed Waiting despite the subsequent PASS paint. The driver now explicitly waits for that later painter record and captures it separately. This is an apparatus correction, not evidence that the load DOM mutation failed.

Pointer-events auto/none is now a real inherited LibCSS property, with computed serialization, value-level supports and shared native/CSSOM hit filtering. An auto child remains targetable inside a none ancestor; painting, keyboard focus and event bubbling are unchanged. The final host gate passes 45 checks and its hit-filter control fails 10; transparent-box integration now passes 123 checks. The native screenshot visibly retains the red none overlay while the underlying input receives Python, and the auto child fires its click and ancestor bubble.

The final live Bing replay no longer waits through the old late-script timeouts before native Python submission, and no longer crashes during or after search navigation. Its guest homepage total is 3410 ms; search load is 7490 ms, including 6270 ms styles_fetch. These are individual live observations, not controlled medians or input-latency numbers. The capture actually shows two styled Python.org results, then loses the result body while keeping the search box/navigation: painted text drops from 72 runs/423 bytes to 9 runs/60 bytes. Therefore search compatibility still FAILS. In this same run only 29 of 92 stylesheets downloaded, 63 failed transport, and all six external scripts were LOST with kernel socket table full. Those failures are established; their presence alone is not proof of which specific DOM/style mutation hides the body.

Network ownership remains separate: the other group has been modifying browser_rt.c/bfetch.h and transport integration. A refreshed mtime check reads browser_rt.c 21:32:29 and bfetch.h 20:49:56; no uncoordinated network rewrite was applied here. The concrete H2 admission handoff is patches/bfetch-h2-admission.md. The current cache-invalidation positive gate still reports 35 failures out of 652, so the aggregate is not green. GitHub's previously observed long initial script/module waits, z.ai's blank application/backend boundary, and Bilibili's unresolved image requests are not claimed solved by the local input fix.

Only this group's QEMU processes were closed. Private test disks/ISOs and the temporary raw core backup were removed after hashes and useful artifacts were retained. No unrelated build trees, model files, external-group VMs or staged work were deleted or committed.


### Final gate status and retained evidence

The final make -k -j3 test-browser-expansion completed all reachable prerequisites and exited 2. Its only failed positive make target is test-cache-invalidation: 652 checks, 35 failures. The separate 54-failure cache output is its intentional old-behavior control, not an additional positive result. All new asynchronous-script (10 modes), navigation (4 original + 5 form-stack modes), pointer-events (45 checks) and transparent-box (123 checks) positives ran and passed in that aggregate. The strict old-behavior controls ran first, including both specific synchronous-form teardown assertions. Final test-mk-wired passes: 223 fragments, 222 reachable, one explicitly declared wrapper.

The extra page-lifecycle regression passes. The independent test-webapi gate has one repeated-response-header joining failure in 221 checks; its location/navigation checks pass. This header failure is tracked separately from the cache gate rather than hidden under the successful navigation result.

The no-input initial-load guest was rerun with its corrected paint boundary: INSERTED-INITIAL PASS is visible in the retained screenshot, and the disk/ISO hashes remain unchanged. The eleven-case run also retains its first early Waiting capture to make the apparatus correction reviewable. Sixty-one remaining PPM captures were converted to byte-equivalent PNG pixels, reclaiming 181888428 bytes without removing useful visual evidence.


### Network handoff confirmed by the owner

The user explicitly confirmed after the final tests that the other group is STILL modifying browser_rt.c/bfetch.h and instructed this group to preserve those files. Accordingly no further transport patch is applied here. The pending work is a coordination boundary, not permission inferred from a stale timestamp.

The disappearing-result analysis is now more specific than the resource-failure count: a retained response contains an inline BM compute step that sets the content container's visibility to hidden, while its external BD loader restores visible (or follows a conditional redirect). The exact restoring resource URL is present in the latest guest's socket-table-full LOST list. Source excerpts, resource mapping and the distinction between saved source and observed guest execution are in patches/bing-hidden-resource-chain.md. No cookie, redirect, visibility override or site branch was introduced to conceal that missing dependency.


The repeated-header failure was additionally checked with a narrow A/B: a temporary source copy reverses only the new navigation producer and close cleanup, preserving every other workspace change. Both it and current production compile and report the same 221 checks / one header failure, with byte-identical run logs. This establishes that this particular failure does not depend on this producer diff; it does not claim a historically green baseline. The production SHA remained unchanged, and both temporary executables were deleted. The retained manifest is evidence/site-generalization-2026-09-09/webapi-producer-ab.json.
