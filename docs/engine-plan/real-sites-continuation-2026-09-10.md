# Real-site continuation — 2026-09-10

Status: **partial, goal remains open**. The listed sites are specimens, never
hostname/framework branches in the product. No authentication bypass, spoofed
browser signal, automatic POST replay, or video playback was added.

## What changed in this continuation

- `browser_rt.c` / `bfetch.h`: an HTTP/2 handle borrowed before GOAWAY can be
  replaced **before stream allocation**. `BXFER_START_WAIT` leaves the request
  buffer owned by its caller, returns the replacement fd, and lets the ordinary
  frame loop await TLS. Both resource GET and JS fetch honor this state. Old
  accepted streams retain their original session until their owners release it.
- `js_webapi.c`: unanswered GET/HEAD transport/truncation failures get at most
  one retry over the complete fetch, preserving the deadline. POST, other
  methods, responses already exposed to JS, and bodies failing after headers
  are not retried. This closes another timing window, not every network error.
- The browser's buffered resource policy is shared at
  `BROWSER_BUFFERED_BODY_MAX = 16 MiB`; the generic HTTP parser remains 8 MiB for
  other clients. The concrete compressed-script specimen was **2,724,940 wire
  bytes / 10,589,862 decoded bytes**. Its downloaded gzip passed host `gzip -t`;
  the final guest also logged that exact decoded module size. This is a bounded
  per-response limit, **not** a global process-memory budget or streaming inflater.
- Dynamic CSS buffers from the previous work now report a refused complete
  occurrence as `error`, not a fabricated `load`. Its retained bytes can be
  applied after an earlier occurrence is removed. Failure to reserve a closing
  media wrapper is included in the refusal budget too.
- `qmp_site.py` now captures `[img]` failures and fetch stalls, and actually
  includes resource-failure diagnostics in its verdict. The previous GitHub
  record said PAINTED/no gap despite four image timeouts. Old JSON snapshots
  were preserved, not rewritten to conceal the instrument's old behavior.
- Optional `BROWSER_TRACE_VISIBILITY=1` traces the shared CSSOM producer and up
  to four caller filenames. It never changes style values. The flag is an object
  prerequisite so switching it off rebuilds the producer. The first diagnostic
  attempted `JS_NewError().stack`, which returned `undefined`; its correction
  uses QuickJS's native caller filename accessor, without invoking page code.
- Stronger sanitizers found an existing QuickJS `M4` opcode-packing signed
  shift overflow (171 shifted by 24). Packing is unsigned; the final argument
  stays `int` because `code_match` consumes `va_arg(int)`. Missing-setter errors
  now name the property atom; they still reject the assignment.
- The named exception was `input.files`: its getter-only descriptor stopped a
  strict binding assignment. `js_forms.c` now accepts nullable assignments and
  real empty FileLists, preserves assigned identity, and rejects arrays and
  forged prototypes. State/brand live in private weak collections, not the
  old page-writable `__fcFiles` expando. Non-file inputs return null. These
  follow the [HTML input API](https://html.spec.whatwg.org/multipage/input.html#dom-input-files)
  and [Web IDL nullable conversion](https://webidl.spec.whatwg.org/#es-nullable-type).
  The native picker, DataTransfer producer and cross-realm list bridge remain
  absent; no filesystem bytes, uploads or fabricated selected files were added.
- The first files guest passed all 23 JS checks yet painted the markup's
  `value="not-a-selected-path"` as a chooser label. `forms.c` now rejects native
  nonempty file values, returns the empty selected value and paints the chooser
  label independently of markup. JS `value` rejects invented paths too.
- The complete forms sanitizer then found another existing QuickJS UB:
  `JS_NewFloat64` narrowed 4,294,967,295 to int32 before testing the result.
  An ordered range guard now precedes the cast; the existing bit comparison
  still preserves negative zero, fractions, NaN and infinities. The old cast
  was watched failing under UBSan; the corrected full forms fixture passes.
- Site evidence now retains a **second, post-diagnostic screenshot**, rather
  than treating a briefly stable early frame as final. Initial screenshot,
  score and load counters stay unchanged for comparison. Later module-fetch
  observations and completed text dumps are separate fields. Correction to
  the earlier comment: about:text/boxes/images return without navigation in
  the current browser. Neither screenshot is proof of quiescence or usability.
  Optional explicit input-ID/text flags attempt native typing without Enter;
  the record names it an attempt until its screenshot is inspected. The
  workload and site runners share the same WM-to-client coordinate transform.
- `browser_paint.c`: control damage signatures now consume the same prepared
  `fpaint` as drawing. Layout text is not a textarea's live value: the old gate
  drew native edits but submitted zero frames. Text, placeholder, caret,
  selection, scroll and toggle state are now included; focus-ring damage also
  includes its 2 px out-of-box extent. No site branch or forced full repaint.
  Canvas/video backing-store invalidation remains separate and unaddressed.
- `about:input` arms bounded native event/focus/default/paint provenance, with
  guest monotonic timestamps. It does not log keys, field contents or password
  lengths, invoke page callbacks, or mutate focus. Real navigation disarms it.
  A frame marker means the browser's redraw returned, not compositor presentation.
  The site instrument now waits for that marker after the final observed default;
  the screenshot remains the visible-content evidence.
- On-demand image diagnostics include ancestor styles when a DOM image has no
  display-list item. The latest z.ai pair are hidden analytics pixels, not card
  thumbnails; they cannot establish an image decoding defect.

Earlier work retained: healthy idle H2 reuse, bounded peer stream admission,
draining-session lifetime, one image transport owner for DOM/render interests,
growable author/variable CSS, and shared UTF-8 text advances for long-text paint,
caret, selection and hit testing. These are not all new in this continuation.

## Verification and controls

| Scope | Evidence | Boundary |
|---|---|---|
| H2 transport adapter | 168 checks, 0 failures; ASan + UBSan | Real adapter/protocol, simulated socket peer |
| Pre-send negative control | POST and resource-loader waiting assertions went red | Held replacement TLS, old accepted response retained |
| JS read retry | 15 checks, 0 failures; ASan + UBSan | GET/HEAD retry, POST refusal, partial response refusal, bounded repeated failure |
| Large decoded response | 6 checks, 0 failures; ASan + UBSan | 10 MiB binary payload delivered, 17 MiB and corrupt input refused |
| Large-response negative control | 8 MiB policy fails the intact arrayBuffer assertion | Real JS fetch and Rust decoder, host-generated compressed fixture |
| Dynamic CSS | Both buffer cases pass; dynamic stylesheet fixture 13 checks / 0 failures | Whole-occurrence error and recovery; old load-event behavior watched red |
| Cache invalidation | 652 checks, 0 failures | Host real HTTP1/H2/cache adapter |
| Ordinary Web API | 227 checks, 0 failures | Host JS/runtime/parser suite |
| QuickJS opcode control | Old shift triggers UBSan; corrected compiler fixture passes | Actual compiler, not a copied bit expression |
| Nullable files and native state | 23 shared JS checks + 6 host assertions pass, including ASan + UBSan | Empty FileLists only; getter-only/native-markup controls fail exact assertions |
| Numeric boxing | 16 checks, 0 failures under UBSan; old cast goes red | Boundary integers, fractions, signed zero, huge numbers, infinities and NaN |
| Form regressions | 195 native form checks and 33 JS/native UTF-16 checks pass | Not a real-site authentication test |
| Native input submission | Shipping app_main passes native focus, insertion, caret-only movement, backspace and readonly checks in 42 virtual polls | Host event injection + actual draw/flush calls, not hardware presentation |
| Input controls | No-focus, diagnostic navigation and static control signature each fail exact assertions | All three are prerequisites of the positive gate |
| Site instrument | 15 parser/decision/coordinate checks pass; dropped errors/late evidence/box geometry/default-only wait fail controls | Instrument correctness, not website compatibility |
| Build wiring | 241 fragments, 240 reachable, 1 declared; CSSOM ABI passes | Recheck final log if fragment count changes in shared tree |

Logs: `build-h2-slot-0910/final-adapter.log`,
`build-loader-final-0910/{fetch-retry,body-limit,css-limit-events,opcode-match}.log`,
`build-loader-final-0910/final-webapi-regression.log`,
`build-loader-asan-0910/fetch-body.log`.
Files/numeric evidence: `build-loader-final-0910/input-files-native.log` and
`input-files-numeric-final.log`. The earlier `input-files-asan.log` is the
preserved **failing** run at the numeric narrowing bug, not a passing result.
On Darwin, these ASan runs explicitly disable unsupported LeakSanitizer; this
does not claim leak-check coverage. The first combined run stopped at the M4
UBSan error; only the later corrected run is the passing result.

The CSS refusal fixture initially ran against the empty startup tab: a helper
returning false for “no JS context yet” was mistaken for readiness. Its fix is
a positive event-readiness signal; it now observes both the error and recovery.
Negative-control FAIL lines in the combined logs are expected, not suppressed.
The first files host fixture also needed its HTML script slice NUL-terminated
at QuickJS's input length; its initial closing-tag SyntaxError was an apparatus
error, not a browser regression. That control run was not counted as evidence.

Input/paint logs: `input-delivery-before-paint.log` records the original positive
gate failing `native edit paints and submits changed control frame` and
`native backspace submits restored placeholder frame`. Final source-linked gate:
`build-loader-final-0910/input-complete-final.log` (also repeats the 15 instrument
checks). `input-frame-wired-final.log` is the first correctly wired rebuild. The intermediate
`input-frame-final.log` accidentally reused an older host binary: the new Make
fragment was included before its source-list producer, leaving only the fixture
and `.mk` as prerequisites. The include is now after `interaction_runtime.mk`,
with an explicit refusal for wrong order. `make -pn` confirms browser.c, painter,
forms and `.inc` prerequisites, and the final log shows actual recompilation.
Initial host compile errors (missing header and `fail` counter spelling) are
preserved in `input-delivery.log`; they are apparatus failures, not browser ones.
`input-paint-regressions.log` passes form caret, control chrome, long caret
advance, and all 33 UTF-16 checks; its negative-control FAILs are expected.

## Native input evidence

`build-real-sites-0910/native-css/results.json` contains four isolated-fixture
cases on the CSS snapshot: pointer-events passthrough/override/bubbling,
transparent box default navigation, transformed matrix click/resize, and native
caret editing. Physical End/Left/Z and end-of-field clicks were checked on:

- 1,600 ASCII characters;
- 600 CJK characters;
- 800 mixed characters;
- 1,400 password characters.

Each insertion and subsequent end click reached the expected selection index.
The mixed-field screenshot was visually inspected. This is **not** an actual
DeepSeek verification click or authenticated first-token test. The original
user VM's QMP socket disappeared; its stale screenshot was not clicked.

The shared `input-files.html` fixture additionally passed a native button click
after strict FileList binding. The first native image exposed the false label;
`native-input-files-final/input-files-click.png` was inspected after correction
and shows `Choose File` plus `INPUT-FILES-CLICK PASS`. That guest's load stages
totalled 930 ms; this is a tiny local fixture, **not** a real-site speed claim.
The final numeric-fix snapshot repeats the same native fixture successfully:
`native-number-boxing/results.json`, 1,030 ms initial load stages and an actual
button click. Neither local fixture reads a file or submits a message.

The first real-page input attempt (`live-native-input`) is preserved as
HARNESS: its box parser assumed one space after height, but the real printf
field pads `766x42   <textarea>`. The corrected generic parser accepts the
actual whitespace and refuses absent/duplicate IDs rather than guessing.
An optional input diagnostic failure now keeps earlier load/paint evidence
instead of aborting before the record's guest fields have been filled.

The corrected real attempt is `live-native-input-final/z-ai.json`, using
`snapshot-number-boxing`. Its late/input screenshots show the heading and
composer, but **the input screenshot still shows the placeholder, not Python**.
The log confirms `[wm] ptr 716 372`, matching the attempted point; this rules
out a pointer-coordinate mismatch for that observation, not a page hit/focus
or event-delivery defect. No Enter was sent. Do not promote the record's
`native_input_attempt.text` (sent keys) to observed field contents. Interactive
typing remains unverified/visually unsuccessful in this real-page attempt.

Further provenance (`live-input-observed`, `snapshot-input-observed`) confirms
the textarea is the actual down/up hit, focus stays there, all six native
defaults execute without cancellation and value length grows 1 through 6.
The screenshot nevertheless still shows the placeholder. The first paint-fix
guest (`live-input-paint`, `snapshot-input-paint`) does the same. These records
do not confirm a completed post-edit frame: the former key-default-only wait
could end before callbacks, painting and presentation. Thus the independently
proven control signature defect is **not yet the full real-site diagnosis**.
Neither attempt submitted a message or interacted with a challenge.

The final frame-aware attempt, `live-input-frame/z-ai.input.png`, was visually
inspected and **shows Python in the real textarea**. JSON confirms six native
defaults and a subsequent paint marker. With tracing enabled, the guest's first
key-arrived was 84,670 ms and redraw returned at 89,120 ms: a **4,450 ms diagnostic
interval**, including event handlers, callbacks/IO and drawing, not host wall
time or pure rendering cost. The final default preceded redraw by 1,360 ms.
The old one-second post-default screenshot could therefore still be early.
This is not evidence that the signature fix alone changed the live site's
six-key burst; its independent native one-event gate proves that defect.
At redraw the current focus had moved to a page button, so continued typing,
responsiveness and stable composer focus are still not accepted. No Enter or
message submission was performed.

## Event fairness, media regions and calculated inset continuation (04:38)

The old six-character screenshot above remains genuine. Correction beside it:
draining the entire native queue let all six keys precede a pending page
callback. The new event budget allows that callback after the first key. The
site opens a dialog and focuses its button, so the later five keys correctly
reach that button, not the textarea. This is **not** proof of responsive
continuous typing, and forcing focus back would hide a legitimate page action.

### Native event burst fairness and focus provenance

`browser.c` now bounds an event burst to 32 events or 20 guest milliseconds,
whichever is reached first. The clock starts only after the first event; the
budget is checked before polling another event, leaving the queued event for
the next turn. The old drain-all path remains under
`BROWSER_UNBOUNDED_EVENT_BURST`. This cannot preempt one long JS handler and
is **not** a 20 ms input-to-frame guarantee.

`test-event-fairness` exercises actual `app_main`, real form defaults and paint
with twelve keys whose fixture callback advances a virtual clock by 25 ms,
then forty same-tick keys. The required negative control failed the interim
flush assertions; the positive retained all 52 characters in order and passed
63 virtual polls. Native input delivery still passed 42 virtual polls, and the
interaction/runtime and browser-loading checks remained green. These virtual
times are stimuli, not browser performance measurements.

Bounded `about:input` diagnostics now include focus/blur, ancestor computed
layout fields and up to four caller script names. They do not print field
contents. `qmp_site.py --boxes` also captures a post-input box dump after the
already-retained screenshot. `live-focus-layout` identified a real `role=dialog`
inside a fixed wrapper with neither top nor bottom set, painted at
`(0,562),1126x0`. The public stylesheet declares
`inset:calc(var(--spacing)*0)` with `--spacing:.25rem`; the old extension producer
accepted only pixel literals. Caller provenance points to the page's ordinary
dialog focus-management code; no focus override was added.

### Media-query extension region correctness

The former 512-entry map silently applied later groups unconditionally. Nested
closures also wrote the newest child's end rather than the parent's; quoted
braces/comments could change region boundaries. `css_extra.c` now stores only
disjoint inactive spans in a checked dynamic allocation, finds balanced blocks
while skipping strings/comments, and binary-searches the spans. Allocation
failure refuses the extension sheet instead of dropping media conditions.
This does not implement `@supports`, `@container` or cascade layers.

`test-css-media-regions` passes six checks, including 700 active groups, 700
inactive groups, nesting beyond 64, comments and quoted braces; ASAN/UBSAN also
pass. Its prerequisite `CSS_MEDIA_LEGACY_REGIONS` binary was watched failing
both the inactive-parent boundary and the post-512 condition assertions.

Apparatus correction: the first guest fixture asked `getBoundingClientRect()`
for transformed coordinates. Both snapshots returned pre-transform `20,20`,
while inspected screenshots showed the first green text box moving from x=920
to x=60. Keep those failed runs (`native-media-before`, `native-media-after`):
they expose a separate CSSOM-transform projection gap. The fixture now logs
rects only as diagnostics. The runner requires exact labels/coordinates from
the final complete **painted text** block, with six parser controls rejecting
old pixels, stale frames, incomplete frames and missing/duplicate labels.

Corrected guest runs (`native-media-before-final`, `native-media-after-final`):
the old immutable snapshot failed with `MEDIA700=[920,148]`; the new one passed
with `MEDIA700=[60,148]`, `MEDIANESTED=[100,202]`. Guest initial load phases were
1130 ms and 1110 ms respectively: correctness evidence, not a speedup claim.
The final inset snapshot repeats this actual painter check successfully.

### Inset length/percentage arithmetic and ordinary dialog interaction

`css_inset_math.inc` replaces the px-only producer for `inset` and its logical
family. It validates a complete 1–4 or 1–2 component list, carries `auto`,
preserves percentage kind, and evaluates scalar arithmetic with typed lengths.
Font/viewport units resolve per matched element; percentage terms reach the
existing layout solver unchanged. Fractional multiplication is evaluated
before integer-pixel conversion. Raw spans retain the existing sheet/attribute
owner and merge per edge with the author extension cascade.

The implementation follows the relevant [CSS arithmetic typing](https://www.w3.org/TR/css-values-4/#calc-type-checking)
and [inset shorthand](https://www.w3.org/TR/css-position-3/#inset-shorthands)
contracts for this supported subset. Limits and deliberate omissions are in
the source: nested `calc`, scalar multiplication/division, px/absolute,
em/rem/viewport units and percentages are supported; min/max/clamp, dimension
cancellation, font-metric units, CSS-wide keywords and unresolved var/env are
not. The physical `top:calc(...)` producer is **not** added, and the pre-existing
physical-vs-logical cross-cascade limitation remains. The old positioned-inset
test that preserves `top:9px` before an unsupported physical calc stays green.

`test-inset-math` now passes 116 checks and ASAN/UBSAN. Its prerequisite legacy
binary reports 28 failures, including `got 90, want 0` for the zero-inset overlay
and `got 0, want 255` for mixed percentage/pixel height. Tests cover root vs
element fonts, per-element reuse, resize, shorthand mapping, auto, importance,
invalid suffix/units/types, division by zero and explicit depth/work bounds.
Existing positioned insets pass 70 checks, projection 18, extension cascade
22, and media regions six. `test-mk-wired` remains 241 fragments / 240 reachable
/ one declared. Logs: `inset-math-before.log`, `inset-math-regression.log`,
`inset-math-sanitizers-input.log`, `inset-math-final.log`.

The ring-3 disk was actually rebuilt, then copied to `snapshot-inset-math`.
Ordinary local dialog guest before/after (`native-inset-before/after`):

- Old: native open reached the handler but geometry failed at
  `(24,1034),1126x254`; close-button client y=1211 was off-screen.
- New: native open yields `(16,16),1094x530` in the 1126x562 viewport; inspected
  screenshot shows the dialog and focused close button. A real native click
  closes it and returns focus to the opener; the closed screenshot was checked.
- The same guest run also passes media painted coordinates and native fixed
  input/page-scroll regression. Initial phase durations are retained, not used
  to claim this interaction became a particular amount faster.

Real z.ai (`live-inset-math`) now visibly displays **Sign in to start chatting**
after the first native `P`. The wrapper has top/bottom=0 and a full
`1126x562` box, replacing the prior zero-height off-screen wrapper. This settles
the focus provenance: the site is requesting login. The dialog is still at the
top rather than vertically centered, and its styling remains incomplete.
Initial guest phases: 16.84 s. First key-arrived 86,340 ms, redraw 88,460 ms:
2,120 ms with diagnostics, not host timing or a controlled speedup comparison.
The JSON retains one default, a subsequent redraw and post-input boxes; the
screenshot actually shows `P`, not `Python`. No login, message, CAPTCHA target
or first-token path was exercised. The existing inline ReferenceError and
resource failures remain.

## Media range operands and complete image diagnostics continuation (05:03)

The previous media-region fix made the extension tier honor its conditions;
it did **not** establish correctness of the shared range-query parser. The
17,398-byte captured GitHub marketing-header stylesheet uses
`@media(width>=1012px)` to switch column/full-width navigation to desktop rows.
This is a specimen, not a product selector. Inspection and boundary controls
isolated two independent defects in `third_party/css/libcss/src/parse/mq.c`:

1. Name-first ranges populated the value from the feature IDENT (`width`), not
   the dimension token. The length matcher therefore always returned false.
2. The old operator "inversion" was a logical complement, not operand reversal:
   `width>=1012px` must store `1012px<=width`, preserving the inclusive edge.
   The lexer already emits `>=`/`<=` correctly; it was not changed.

The old claim in `parse/mq.h` is retained beside its correction. Ordinary
LibCSS selection, the extension tier, variable expansion and `matchMedia`
already consume this shared parser. No hostname, class, framework or URL
condition was added. Range semantics were checked against
[Media Queries 4](https://www.w3.org/TR/mediaqueries-4/#mq-range-context).

Verification, all under independent build directories:

- `test-media-range`: **87 checks, 0 failures**; same gate under ASan/UBSan is
  clean (parser TU explicitly instrumented, the rest of the prebuilt LibCSS
  archive is not sanitizer-instrumented). Old parser: **23 failures** including
  `FAIL range (width>=1126px): got 0 want 1` and wrong actual desktop display.
- A second prerequisite control restores **only** complement inversion, with
  the value fixed. It was observed failing both `(width>=1126px)` and
  `(width<1126px)` at equality. This prevents the IDENT defect masking the
  independent edge error. Logs: `media-range-before.log`,
  `media-range-fixed.log`, `media-range-boundary-controls.log` under
  `build-loader-final-0910/`.
- Existing media-regions **6**, extension-cascade **22**, device-media **8**
  checks passed. `test-mk-wired`: **241 fragments, 240 reachable, 1 declared**.
- Native `media-range.html` uses ordinary navigation labels and two actual
  decoded SVG image cards with titles. Old guest: `MEDIA-RANGE FAIL wide`,
  cards at `[24,238]` and `[24,419]` despite viewport width 1126. New guest:
  `MEDIA-RANGE PASS wide`, cards `[24,184]` and `[284,184]`.
- The runner then dragged the real WM window corner: viewport **702**, hidden
  navigation and stacked cards `[24,130]`, `[24,311]`,
  `MEDIA-RANGE PASS narrow`. Both wide and narrow screenshots were inspected.
  `native-media-range-after/results.json` records completion, not just a
  successful build. Initial guest phases 910/920 ms are **not a speedup claim**.
- `make BUILD=build-real-sites-0910 build-real-sites-0910/disk.img` completed
  before taking `snapshot-media-range`; a second real ring-3 rebuild produced
  `snapshot-media-range-diagnostics` with the completion marker described below.

This patch does not implement responsive image candidate selection,
unitless-zero/boolean viewport queries, every media feature, ratio parsing,
three-valued unknown-feature handling or full CSS unit arithmetic. Those gaps
remain distinct; a working range breakpoint is not all of Media Queries 4.

### Diagnostic completion is separate from dispatch

The earlier Bilibili baseline reported `about_text_arrived:false`, yet all
three exact trigger load lines later appeared in its serial log. The old
six-second observation budget could expire while the page was still busy.
`--diagnostic-wait` is now a bounded host observation budget (default 25 s,
maximum 60 s); it is **never** used as browser performance evidence.

Correction to the first instrument change: waiting longer for `load:about:*`
still proves dispatch only, not a complete dump. The guest now terminates
`about:images` with `[images] end state`, after its DOM/layout/queue census.
The instrument preserves old `about_*_arrived` fields and adds independent
`about_*_completed` fields. Text/box output must also have a full terminator
after the latest exact trigger. An earlier dump or an incomplete line cannot
be borrowed; an arrived but incomplete command is not requeued. Older guests
without the image terminator are explicitly incomplete, not zero images.

`test-sites-errors` passes **22 checks**. Its prerequisite negative control
was observed printing
`FAIL diagnostic arrival cannot borrow an older completed image census` when
dispatch was substituted for completion. Python syntax checks passed. This
changes observation only, not resource state or page content.

### Bilibili image census and failed GitHub retries

The `snapshot-inset-math` Bilibili baseline had guest initial phases **5.47 s**.
The inspected screenshot shows the banner, a carousel and two image/title
columns. It has three carousel transport timeouts, page-reported hydration/
cross-origin errors and incomplete long-title truncation. No video was played.

`snapshot-media-range` Bilibili had guest initial phases **5.39 s**, not a
controlled speedup. Its actual serial census now supplies separate facts:
**24 DOM img elements**, all with nonempty fallback `src`; **23 picture** and
**44 source** elements with `srcset`; **55 display-list image items**, of which
**50 hold decoded pixels**, including inline SVGs. Of the **20 URL-bearing
items**, **15 have positive cache entries** and **5 negative**; all five logged
transport timeouts, not decode failures. Pending and unanswered are zero at
that observation. Therefore neither 50 nor 55 is the count of successfully
downloaded thumbnails. The missing logo in the inspected late screenshot is
one of the actual timed-out resources. Responsive `<source>` selection remains
unimplemented, but is not why these fallback URLs were absent from the DOM.

Both GitHub trending attempts with `snapshot-media-range` failed before page
rendering: guest no response after 12 seconds, host HTTP 200. Both records are
preserved in `live-media-range/` and `live-media-range-retry/`; no live GitHub
layout improvement or rendering-time result can be claimed from these runs.

### Formatting whitespace swallowed positioned image layers

Apple with `snapshot-media-range-diagnostics` completed all three diagnostic
dumps, verifying the new completion contract in the guest. Its initial phases
were **4.12 s**. The late screenshot still has a blank hero. Unlike Bilibili,
there were no reported image transport failures: **12 DOM img** fallback URLs,
but **zero URL-bearing display-list image items** (the 32 decoded items were
inline SVGs). The hero image and ancestors have no display:none/opacity-zero
style in the recorded chain. A separate promise rejected with
`SyntaxError: Invalid transform`; it was not suppressed.

Reducing the ordinary layout structure first gave a **passing** minified test.
Adding only a newline/tab or a comment between its normal-flow title and its
absolute flex image layer made the image disappear: **54 checks, 10 failures**
in `absolute-image-whitespace-before.log`. This controls for resource delivery,
selectors, query parsing and animation code, none of which change with the gap.

`layout.c:flex_run` checked `skipped()` before its block boundary. That predicate
includes absolute/fixed positioning, so an anonymous run starting at formatting
whitespace consumed the entire following out-of-flow sibling. `flex_collect`
never received it and could not call its existing positioned layout owner.
The correction checks the block boundary first. Display:none still does not
create a slot or gap. Flex and grid share the collector; no site class, forced
visibility, or fake resource success is involved.

- Required `LAYOUT_FLEX_RUN_SWALLOW_OOF` control restores exactly the old order
  and prints `FAIL: whitespace cannot consume absolute flex image layer`.
- `test-absolute-auto-height`: **54/54** normal and ASan/UBSan; intrinsic
  **65/65**, inline-flex **36/36**, positioned-insets **70/70**, positioned
  projection **18/18**, and `test-mk-wired` pass. Negative-control failures in
  `absolute-image-whitespace-fixed.log` are expected and checked prerequisites.
- Native old snapshot, same local `absolute-image.html` and real SVG:
  `ABSOLUTE-IMAGE FAIL {"image":[0,0,0,0],"layer":[0,0,0,0],"decoded":[240,100]}`.
  This is a decoded image with **no layout**, not an image decoder failure.
- An actual independent ring-3 disk rebuild completed before immutable
  `snapshot-absolute-image` was made. Corrected native result:
  `ABSOLUTE-IMAGE PASS {"image":[224,92,240,100],"layer":[24,92,640,220],"decoded":[240,100]}`.
  The inspected screenshot shows the actual green/white SVG above its title.
  `native-absolute-image-after/results.json` records completion, and artifact
  hashes stayed unchanged. This fixture gives the picture an explicit width;
  it proves traversal/presentation, not automatic intrinsic-width sizing.

Apple's corrected run (`live-absolute-image`) now retains **one actual display
item for the hero**; the URL-bearing item census rises from **0 to 2**, both
decoded with positive cache entries. Initial guest phases **5.00 s**, no logged
promise exception in this run. These are separate live responses, not a timing
speedup or proof that every transform path is repaired. The instrument's
`PAINTED` verdict is **not compatibility**: inspection shows the hero only as a
narrow vertical strip. The hard-coded 24px unloaded max-content fallback in
`content_width(img)` still ignores decoded intrinsic dimensions, a strong
next hypothesis for the remaining auto-width defect. Do not replace authored
widths or force a viewport-sized image; reduce and verify the sizing contract.

### Decoded image dimensions now drive layout, not only bitmap painting

Correction to the preceding hypothesis: it was reproduced and fixed in the
shared replaced-image sizing path. `content_width(img)` returned 24px even
after decoding, and `ic_fit()` changed only the display item's height after
the box table and following siblings were already placed. A third path treated
an image passed directly into flex/grid content layout as an empty container,
so it emitted no image item. None of these fixes branches on the specimen.

`image_decoded_size()` now feeds decoded width/height and ratio into intrinsic
measurement and inline/block/float/direct flex/grid placement before boxes
close. It respects definite zero, percentage bases and the supported min/max
constraints. A positive decoded-cache generation also invalidates geometry:
the browser consumes it before load-handler CSSOM reads or on its normal frame
settle, without requiring a DOM write. No fetching or script callback occurs
inside the reflow. The old `ic_fit` limitation comment remains beside this
correction; it is now only a transient/cache-refused-item fallback.

The contract follows [CSS replaced-element dimensions](https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-width)
and [intrinsic sizing](https://www.w3.org/TR/css-sizing-3/#intrinsic-sizes), not
an authored-width override. HTML width/height remain the engine's existing
fallback hints rather than full cascaded presentational hints. This does not
implement responsive candidate/density selection, ratio-only vector sizing,
all absolute-replaced sizing or every flex/grid transferred-size rule. An
undecoded direct flex/grid image still lacks a loading display item; the real
DOM image lifecycle fetches it, then this change emits it after decoding.
The non-JS/display-list-only fetch path is not established for that case.

Evidence retained:

- Host `image_intrinsic_test.c` first reproduced **32 failures / 100 checks**
  (`image-intrinsic-before.log`). Expanded production coverage is **141 checks,
  0 failures**, also under ASAN/UBSAN. The dimension oracle is not a PNG/JPEG
  codec test. `test-image-intrinsic` requires both independent negatives:
  restoring old sizing fails decoded box/title/24px contribution assertions;
  removing direct-self placement fails `exactly one image item for replaced
  element`. These failures were observed, not inferred.
- Other production gates remained green: absolute auto-height **54/0**,
  intrinsic **65/0**, inline-flex **36/0**, positioned insets **70/0** plus
  projection **18/18**, percentage height **41/0**, and browser-loading
  **36 virtual polls, 0 blocking drains**. Required negative output in
  `image-intrinsic-regression.log` is intentional, not a production failure.
- Native delayed **actual SVG** in `native-image-intrinsic-before` reports
  `FAIL {"flow":[240,240],"titleDelta":0,"picture":24,"ratio":[24,50],"direct":[120,20]}`.
  `snapshot-image-intrinsic` reports
  `PASS {"flow":[240,100],"titleDelta":0,"picture":120,"ratio":[120,50],"direct":[120,50]}`.
  Its passive case makes no post-decode DOM writes or CSSOM reads: the dump
  shows image `(24,80,240,100)`, following title `(24,180,1078,24)`.
  The inspected passive screenshot contains the green image, title directly
  below, and two correctly proportioned small images. The first old screenshot
  was taken before final repaint; the old **geometry assertion**, not a blank
  screenshot comparison, proves the defect.
- A separate native disk keeps new sizing but disables only the browser's
  generation consumer (`BROWSER_IMAGE_GEOMETRY_NEGCTL=1`). The passive case
  exits 1 with `late dimensions did not reflow passive page`: image
  `(24,80,240,100)` but title `(24,320,1078,24)`. This is the actual item-only
  snap trap: decoded pixels alone do not establish corrected flow geometry.
  The normal mutable build was then rebuilt with flag stamp `on` and the
  compile line without `BROWSER_IMAGE_GEOMETRY_NO_FLUSH`.

`test-image-intrinsic-guest` wires the no-flush negative as a prerequisite of
the positive active/passive run. It requires explicit immutable ISO, positive
disk and negative disk arguments; it never guesses that a mutable shared disk
is safe. Its first attempt failed in the apparatus before boot: putting QMP
under the deep artifact directory exceeded macOS's 104-byte socket address
limit. `browser_load.py` now owns a short private `/tmp` socket directory and
cleans it after QEMU exits. The original error is preserved in
`image-intrinsic-guest-path-failure.log`, not counted as a product failure.
After that correction the wired gate exited **0**: its required negative
printed the stale-title assertion above, and both positive cases completed.
Both native gate runs retained unchanged artifact hashes. Reproduction:

```sh
make BUILD=build-real-sites-0910 \
  IMAGE_INTRINSIC_GUEST_ISO=build-real-sites-0910/snapshot-image-intrinsic/logit.iso \
  IMAGE_INTRINSIC_GUEST_DISK=build-real-sites-0910/snapshot-image-intrinsic/disk.img \
  IMAGE_INTRINSIC_GUEST_NEG_DISK=build-real-sites-0910/snapshot-image-no-flush/disk.img \
  test-image-intrinsic-guest
```

Apple's live `live-image-intrinsic` late screenshot now shows the blue hero
and centered logo across the viewport, with readable overlaid introduction
and event link, rather than the old narrow strip. The actual hero box is
`(-941,44,3008,546)` in a 1126px-wide page: its natural-width picture is centered
and clipped, not squashed to viewport width. Eight URL-bearing image items
have positive decoded cache entries; all three diagnostic dumps completed.
This run's initial guest phases total **6.60 s** (including **1.01 s** stylesheet
fetch and **1.07 s** script fetch); it is **not a measured speedup** over the
previous separate 5.00 s live response. A font stylesheet request to `/wss/fonts`
returned **404**, and navigation styling/horizontal overflow remain imperfect.
The instrument correctly says `ERRORS`; this is a repaired hero-layout defect,
not whole-site compatibility. No event/video playback was attempted.

## Real-site observations

All durations below are **guest monotonic load-phase durations**, not the
instrument's host `load_seconds` or `paint_seconds`. Module network retrieval
is still nested inside `scripts_execute`, so that phase is not pure JS CPU.
Each run used a fresh isolated 1 GiB QEMU and an immutable disk/ISO snapshot.
Live responses, builds and network conditions differ: these are observations,
not a controlled before/after speedup benchmark.

| Specimen / snapshot | Guest result | What remains wrong |
|---|---|---|
| GitHub home / `snapshot-css` | 92.51 s total, 65.28 s scripts phase | Mobile-like menu overlays main content; four image timeouts; not visually compatible |
| GitHub trending / `snapshot-setter` | 64.09 s total, 46.33 s scripts phase; 32 initial modules, no module failures | Menu occupies the first screen; one timer interruption; trends list not visibly usable |
| GitHub trending / `snapshot-media-regions` | 59.28 s total, 42.69 s scripts phase; 32 modules, no module failures | Inspected late screenshot still has the oversized menu; one timer interruption. Earlier retry in `live-trends-layout` failed fetching and is preserved separately |
| GitHub trending / `snapshot-media-range` | Two fetch failures before rendering; no initial load-phase result | Shared media parser is fixture-verified; live header/trends improvement not verified |
| Bilibili / `snapshot-h2-image` | 6.36 s total; cards/two-line titles and some thumbnails visible | Empty areas/banner timeout; no video test or full image-completion claim |
| Bilibili / `snapshot-media-range` | 5.39 s initial phases; inspected carousel, thumbnails and titles; 15 URL-bearing image items have decoded cache entries | 5 image transport timeouts, incomplete title truncation and page errors; 50 decoded items include inline SVGs, not 50 downloaded thumbnails |
| DeepSeek / `snapshot-h2-image` | 4.66 s total; login form visible | Thin controls/blank QR area; not logged in, no first token |
| Apple / `snapshot-h2-image` | 11.50 s total | Hero image absent; region chooser overlaps navigation |
| Apple / `snapshot-media-range-diagnostics` | 4.12 s initial phases; text/boxes/images diagnostics all completed | Hero absent despite fallback URLs and no image fetch errors; formatting-whitespace layout defect reduced separately, plus Invalid transform rejection |
| Apple / `snapshot-absolute-image` | 5.00 s initial phases; hero item retained, two URL-bearing image items decoded; no reported exception in this run | Inspected hero is a thin strip, not a correct image; automatic intrinsic width remains unresolved despite PAINTED instrument verdict |
| Apple / `snapshot-image-intrinsic` | 6.60 s initial phases; inspected hero and overlaid title now visible at natural ratio; eight URL-bearing items decoded | Font stylesheet 404, imperfect navigation styling/overflow; no full-site or speedup claim |
| Bing Python / `snapshot-h2-image` | 2.77 s then 2.55 s after redirect | Results still disappear despite six external scripts and all 92 guest stylesheets arriving |
| z.ai / `snapshot-presend` | 17.83 s load phases; visible home title | Input/layout incomplete; large script hit old decode ceiling |
| z.ai / `snapshot-body-limit` | 15.00 s load phases | A separate run hit GOAWAY after sending requests; config failure remained |
| z.ai / `snapshot-read-retry` | 18.43 s initial phases; 10,589,862-byte late module loaded | Later missing-setter exception; screenshot can regress to header-only; initial phase duration is not time-to-usable |
| z.ai / `snapshot-input-files` | 16.63 s initial phases; `files` exception no longer logged, chat-input appears in boxes | Early screenshot remains header-only; original capture time cannot represent later module completion |
| z.ai / `snapshot-input-files-native`, new instrument | 15.22 s initial phases; **late screenshot shows title, textarea and feature buttons** | Cards still loading and layout imperfect; one inline ReferenceError and resource failures remain; not time-to-usable or full compatibility |
| z.ai / `snapshot-number-boxing`, native input attempt | 16.64 s initial phases; late form visible, guest pointer reaches requested composer coordinates | `Python` is absent from inspected input screenshot; hit/focus/event chain still needs diagnosis; no message submitted |
| z.ai / `snapshot-input-frame`, frame-aware input | 16.59 s initial phases; inspected screenshot shows Python after six native defaults and completed redraw | 4.45 s first-key-to-redraw diagnostic interval; focus then on page button; loading cards and layout remain imperfect |
| z.ai / `snapshot-event-fairness`, bounded burst | 16.41 s initial phases; first key/default then callback before remaining keys | Only P visible; page focuses an off-screen dialog, continuous typing not accepted |
| z.ai / `snapshot-inset-math` | 16.84 s initial phases; login-required dialog is now visible and its fixed wrapper fills viewport | Dialog not centered; only P before legitimate dialog focus. No authentication or first-token evidence |
| Wikipedia OS / `snapshot-presend` | Failed before rendering | Guest TLS handshake/certificate boundary; host HTTP 200 does not establish identical route |
| Hacker News / `snapshot-read-retry` | Failed before rendering | Guest says no source address for any destination, with TCP table empty; not socket exhaustion |

Artifacts are under `build-real-sites-0910/live-*`: JSON, PNG, serial log and
host document. The host document is **not** a byte-for-byte guest DOM capture.
The broader samples' failures were retained, not counted as new supported sites.

Bing provenance (`live-body-limit/bing-search.serial.txt`) identifies the hide
as a real CSSOM write from `#inline-script-4`, called by `#inline-script-6` and a
later external callback. No `visibility=visible` write appeared. The prior
“missing restore script” candidate is insufficient in this build; the actual
restore/redirect ordering still needs a reduced runtime fixture. Do not force
visibility, fabricate cookies or alter redirect parameters to make it pass.

## Snapshot identities

- `snapshot-read-retry`: ISO
  `788611412c1b89a2112b8d881cf533087927c09a8ec506e5afed552e4c0695fc`, disk
  `a287e7eb8798ad4b8aabd0b00176fc0b877e601b34d1cce5c43ab0ce9e04f3b9`.
- `snapshot-setter` includes the compiler fix/property-name diagnostic: same ISO,
  disk `51ca3c6d349a7af2bbd93ebdad1a99c23eb514f72ffb3876be5bd310b4941a45`.

Both were built with command-line `BUILD=build-real-sites-0910` and an actual
`disk.img` rebuild; no shared `build/` image was overwritten by this work.

Further immutable disks use the same ISO hash above:

- `snapshot-input-files`: `1a6a306b71d0bbf97e6ba0344daaba952c0ea17956ae4606980439618f98c91b`.
- `snapshot-input-files-native`: `7ef489fc88db6d19dbf0a1d24102854e00ea65b3e7cf54daa7298c5faa1b38c1`.
- `snapshot-number-boxing`: `5a6c2b0100836df8124f2a0daeb83652f0a403db4d16fe702b697178cac57762`.
- `snapshot-input-observed`: `de06c525610cc3d328ffae007ffcc84f503cc0cf04fedea66aafec8c14269d69`.
- `snapshot-input-paint`: `3e2705f228e67fc78a1764eac31e9164fc9228d51606a868e8ac7926af611a51`.
- `snapshot-input-frame`: `5d573680ecdd79a0de6339a70e2cb92c0a66584bbfc71b99188214e330f764da`.
- `snapshot-event-fairness`: `ffa528428896f47c2fe5c5b9d7433d3719873da5fe2f481ade11750fd99e55aa`.
- `snapshot-focus-layout`: `c168408e3f51a42001d03ecf83efffe013fbadb9fc26aa96829f3dd3f36c82e0`.
- `snapshot-media-regions`: `e6c56168ea5b5a23d5b66c80a3f3865aa54adc404ac960c3930f8c8eed4f78fc`.
- `snapshot-inset-math`: `95005424d03bcf00f20ced2740b1c6c2ace3d959c108ba54d787820dae6d1e2a`.
- `snapshot-media-range`: `014a2edde2259ce46179c56807403d3579f7bfc4af2ef57364cecf6930d61e3b`.
- `snapshot-media-range-diagnostics`: `f68b4d7fc54e183d75718114afc1eb55f8b33bbea68744931c2011915e0e6ec4`.
- `snapshot-absolute-image`: `64d4df5162ebec4f0709af202bccbc4cf4a8149467e8d9a8e8c3457ad3f366ac`.
- `snapshot-image-intrinsic`: `cbcf32994656273ce2387108f6d60c032c3b9cd4acf2a6bb5ea5da57618e5c2d`.
- `snapshot-image-no-flush` (negative only): `cdef18607b3133890019b4ab0ba291726e5e2ae6ba832e5305ae6d1c206c6765`.

## Continuation: module timing attribution and complex query candidates

### Real-site observations retained, not promoted to full compatibility

`live-trending-intrinsic/` on `snapshot-image-intrinsic` now has a visible
Trending heading and first repository record. The formerly huge header/menu no
longer covers that content. Both initial and late screenshots were inspected.
Header contrast, tabs and image/avatar layout remain incomplete. Guest initial
phases still total **63.56 s**: document 1.78 s, styles fetch 2.07 s, full
style/layout/paint 5.04 s, scripts execute 44.27 s, lifecycle settle 9.45 s.
There were 32 loaded modules, zero module failures, and later callback watchdog
exceptions at 45.01/45.51/45.75 s. The last two have comparatively few JS branch
interrupts, but that alone does not identify which native call consumed time.

The new instrumented attempts `live-trending-profile/` and
`live-trending-profile-retry/` both failed before receiving the document: guest
TLS verification and HTTP/2 negotiation completed, then the first response
timed out after 12 s. Host HTTP 200 was retained but does not prove the guest
route. Zero module counters from the harness startup page are **not** a GitHub
module measurement. No validation bypass, UA spoof or site-specific fallback
was introduced. These two failures cannot establish a rendering regression.

`live-zai-module-profile/` on `snapshot-module-profile` completed initial phases
in **12.81 s**, including 9.05 s scripts execute. Initial module-only accounting
was compile 2870 ms, prefetch wait 0 ms, fetch 60 ms, link 30 ms, evaluation/jobs
650 ms; two compiles covered 3,317,723 source bytes. Later dynamic imports,
including the 10,589,862-byte viewer module, are not covered by this initial
dump. The inspected late screenshot contains the title, textarea, feature
buttons and login-required dialog; cards are still loading and the dialog is
not centered. One inline ReferenceError and resource failures remain. This is
neither first usable frame timing nor authentication/first-token evidence, and
must not be compared directly with the user's older five-minute observation.

### Exclusive module phase instrumentation

`js_module.c` now transfers ownership of one guest-clock interval among
compile, prefetch wait, fetch, link, and evaluation/jobs. Nested dependency
loading suspends the enclosing phase's ownership rather than charging its IO
twice. Error paths restore ownership too. The browser emits `[module-perf]`
after the existing load summary; the site parser retains those fields.

These are guest elapsed intervals, **not thread CPU time**. They do not make
the loader asynchronous, and no observer-overhead reduction is claimed.
Classics and later dynamic imports still require separate attribution. The
negative control deliberately charges IO to compilation and was observed
printing `FAIL prefetch interval belongs to network wait` and
`FAIL cache-miss interval belongs to fetch`; the exclusivity check still
passes. Production module profile checks are 14/0; module budget checks 15/0;
site result parser checks 23/0. Logs: `build-loader-final-0910/` files
`module-profile-gates.log` and `module-profile-parser-gates.log`.

### Necessary-literal filtering for querySelectorAll

`js_select.c` keeps the existing strict parser and full selector matcher.
For a single selector branch it may choose a required ID, attribute presence,
class or unqualified type from the **rightmost compound** and walk candidate
nodes in C before applying that same full matcher. Attribute operators/flags,
combinators and pseudo-classes are not approximated. No result is cached.
Selector lists and optional literals inside `:is`, `:not` or `:has` do not
become mandatory filters. `querySelector` deliberately retains its early-exit
walk, since eagerly materializing all candidates can regress first-match
queries. There is no hostname/framework/bundle dispatch.

The shared fixture `tests/fixtures/engine-expansion/selector-candidates.html`
creates 600 rows and runs 30 compound queries. Construction and result checks
are outside its `performance.now()` query interval. The first attribute-bearing
candidate fails the value comparison, and post-query mutation must appear in a
fresh query without changing the older static NodeList.

| Same workload | Before | After |
| --- | ---: | ---: |
| Host operation count: JS attribute reads | 36,000 | 360 |
| Host operation count: wrapper calls | 18,030 | 180 |
| Guest query interval, round 1 | 1600 ms | 20 ms |
| Guest query interval, round 2 | 1570 ms | 30 ms |
| Matches across 30 queries, both rounds | 150 | 150 |

The native traversal still visits all 18,000 real candidates; this removes JS
and wrapper work, not DOM nodes. Evidence is `selector-candidates-before/` and
`selector-candidates-after/` under `build-real-sites-0910`, each with serial,
phase JSON, screenshots and input hashes verified unchanged after VM exit.
These are synthetic workload results, **not GitHub or z.ai page speedups**.

The expanded simple/candidate gate passes **37/0**, including scope, ancestor
matching, lists/tree order, pseudo arguments, attribute flags/escapes,
HTML/foreign case, quirks, shadow roots, detached fragments and mutations.
The existing selector suite passes **50/0**. The candidate-disabled negative is
a prerequisite and prints `FAIL candidate filter avoids JS attribute reads on
impossible matches` while still confirming all five matches in thirty queries.
The earlier simple-query negative is still required. `test-simple-selector`
is also on `ci-host`; `test-mk-wired` reports 241 fragments, 240 reachable and
one declared. Final logs are `selector-candidates-final-gates.log` and
`selector-candidates-sanitizer.log` under `build-loader-final-0910`.

ASan/UBSan also passes the same **37/0** checks with no recovery and leak
detection disabled (no leak-freedom claim). Its standalone production output is
`selector-candidates-asan-positive.log`, exit 0. The combined sanitizer log
also contains intentional crashes from the prerequisite module-retry negative
controls (`forget-error` and `eager-free`); those are expected red controls,
not production selector sanitizer failures. The final disk rebuild after the
measurement comment is retained as `selector-candidates-final-build.log`;
the measured immutable snapshot was not overwritten.

One older issue was exposed by the new case test: `setAttribute` lowercases
foreign attribute names even with candidate filtering disabled. The test uses
`setAttributeNS(null, ...)`, whose existing writer preserves spelling, to
isolate selector behavior. The writer bug remains a separate unimplemented
fix; it was not hidden by loosening the selector assertion.

Actual ring-3 disk builds completed using command-line
`BUILD=build-real-sites-0910`. Immutable snapshots share ISO
`788611412c1b89a2112b8d881cf533087927c09a8ec506e5afed552e4c0695fc`:

- `snapshot-module-profile`, baseline/instrumentation disk:
  `c2af231ace5de91dab154a3713377389c45c802535d0650d2de1e90825537a4f`.
- `snapshot-selector-candidates`, measured optimized disk:
  `a8690b75a5488978f33af92b7a9f076d14d55526331016ecd53b5e4faa5531b1`.

## Logical auto margins, inset-sized flex, and replaced block placement

The old z.ai observation remains valid: its fixed, full-height flex backdrop
had a 400x73 panel at `(363,0)`. The public stylesheet contains
`.my-auto{margin-block:auto}` and `.mx-auto{margin-inline:auto}`. These are
specimens, not selectors or site names added to browser dispatch.

Three independent generic defects were found and repaired:

1. The logical margin producer accepted only early pixel values, losing
   `auto`, percentages, font/viewport units and bounded affine `calc()` values.
   It now retains validated source spans, resolves them per matched style, and
   preserves auto/percentage/fixed-offset kinds separately. An entire shorthand
   must validate before either edge changes. Important-only logical rules also
   participate in compiled-rule admission and per-edge cascade merging.
2. The flex bridge asked only `spec_h()`. Opposing fixed/absolute insets gave
   the backdrop a definite final height, but that height never reached the
   child alignment solver. The bridge now uses the existing authored
   containing-block height resolver. It does not borrow previous box records;
   zero is definite, and ordinary content-sized flow remains indefinite.
3. Dedicated block form-control and image paths bypassed ordinary block
   placement. Even after parsing `auto`, they used the raw left margin cache.
   Both now distribute horizontal auto margins through `block_left()` after
   control sizing or image decode/clamping establishes the used width.

Logical margins accept the corresponding physical margin value types; flex
auto margins absorb available positive space before alignment. The changes
follow those rules, rather than introducing a dialog-centering heuristic.
See [CSS Logical Properties, flow-relative margins](https://www.w3.org/TR/css-logical-1/#margin-properties)
and [Flexbox, auto margins](https://www.w3.org/TR/css-flexbox-1/#auto-margins).
This patch still does **not** implement vertical/RTL logical mapping,
physical/logical cross-cascade, CSS-wide keywords, full CSS math, or wrapped
column intrinsic sizing. The shared parser's existing bounds and refusal of
unsupported expressions remain in place. Physical margin percentages leave
the new fixed-offset field zero; used layout caches are not reused as the
specified offset on resize.

### Controls and native evidence

The initial host fixture produced **57 checks / 23 failures** before repair
(`build-loader-final-0910/logical-margin-before.log`). Its earlier compile
error was a harness `void *` dereference, fixed before this baseline; it is not
counted as a product failure. The expanded final suite passes **76/0**, also
under ASan/UBSan with recovery disabled and leak detection disabled. The three
required negative builds were each observed failing:

- `CSS_LOGICAL_MARGIN_LEGACY`: `logical block auto preserves both edge kinds`
  got `0`, wanted `5`; mixed percentage/pixel margin got `0`, wanted `45`.
- `LAYOUT_FLEX_INSET_HEIGHT_LEGACY`: inset-sized row/column child `y=0`,
  wanted `120`.
- `LAYOUT_REPLACED_MARGIN_LEGACY`: block button/input/textarea/select/image
  remained at `x=0`, wanted `149` or `150` after actual sizing.

All three negatives are prerequisites of both positive and sanitizer targets.
`test-logical-margin` is on `ci-host`. `test-mk-wired` passes: 241 fragments,
240 reachable, one declared. Existing positive regressions also pass: margins
120, flex-column bridge 59, inline-flex 36, extra cascade 22, percentage height
41, positioned projection 18, decoded image sizing 141, positioned insets 70,
and inset math 116 checks. Combined logs include expected negative failures;
their final positive sections, not a raw `FAIL` search, determine the result.
Logs are `logical-margin-final-gates.log`,
`logical-margin-final-regressions.log`, and
`logical-margin-final-insets.log` under `build-loader-final-0910`.

The native `logical-margin.html` fixture uses real button clicks to open an
ordinary panel, read its geometry, close it, and restore focus to the opener.
Its own page handlers change state; the driver neither repairs geometry nor
injects activation JavaScript. The panel is 300x160 within a fixed inset
backdrop; the button's measured border-box width is 102.

| Native build | Panel top, expected 201 | Button left, expected 512 | Result |
| --- | ---: | ---: | --- |
| Before all three fixes | 20 | 427 | Geometry assertion fails |
| Margin producer + flex height only | 201 | 427 | Geometry assertion still fails |
| All three fixes | 201 | 512 | Open, close, restored focus pass |

The intermediate guest failure is retained in `native-logical-margin-after/`;
it exposed the separate control path instead of being called a complete fix.
The final wired `test-logical-margin-guest` requires the old-image negative
first. That negative must reach `AssertionError: logical margin dialog is not
centered: LOGICAL-MARGIN OPEN FAIL`; failure to boot/load cannot satisfy it.
The final gate exits 0. Its positive JSON records `native_dialog_close: true`
and `case_complete: true`. Open/closed screenshots were visually inspected;
they show the centered panel/button, then the closed panel and focused opener.
Both immutable-image before/after hash checks report `unchanged: true`.

Evidence: `build-real-sites-0910/logical-margin-guest-gate.log` and
`build-real-sites-0910/site-general/layout/positioned-insets/logical-guest/`
and `logical-guest-negctl/`. Actual ring-3 disk rebuilds completed, including
link, app packaging and disk population; `logical-margin-controls-build.log`
is the final build log. These snapshots share the ISO hash recorded above:

- Baseline `snapshot-selector-candidates`, disk
  `a8690b75a5488978f33af92b7a9f076d14d55526331016ecd53b5e4faa5531b1`.
- Intermediate `snapshot-logical-margin`, disk
  `27ecf6f628ac3fcfd07d0459b87bc4cbc31236b2ed73c3984b05fca1ecfaf264`.
- Final `snapshot-logical-margin-controls`, disk
  `51eb6896e42a31cda3db93962d6bf94c6c78a05b7f7f12a978989a962fd40587`.

### Public-site observations on the final snapshot

`live-logical-margin/zai.json` and `python-home.json` retain serial logs,
initial/late screenshots and painted text. Both screenshots were inspected.
The classifier says `ERRORS` for both, not complete compatibility:

- **z.ai:** the late page shows its heading, prompt field, category buttons and
  sign-in button; card/loading placeholders remain. Guest initial load phases
  total **14,940 ms**, including **11,260 ms** script execution. Initial module
  phases are compile 3490, fetch 20, link 40 and evaluation/jobs 820 ms over
  3,317,723 compiled bytes. A later 10,589,862-byte viewer module still loads
  outside that initial dump. `aliyun_csp_inline_test_func` remains undefined;
  several image/telemetry requests fail. This is not a speedup comparison, a
  complete module-cost attribution, or a first-usable/first-token measurement.
- **Python.org**, an additional ordinary content site: logo, search control,
  navigation, introductory text and lower section headings paint. The top
  navigation wraps and the carousel/interactive content is incomplete. Guest
  initial phases total **24,040 ms**, of which stylesheet fetch is 7150 and
  script fetch 14,110 ms. Three `ajax.googleapis.com` assets fail with guest
  `connect refused: no source address for any dst`; missing `$`/`jQuery`
  exceptions follow. This establishes an address-selection/connection failure
  before those dependent scripts, not a TLS or layout root cause. The exact
  DNS/route cause remains unmeasured; no validation was disabled or network
  identity spoofed. Resource-count gaps remain diagnostic heuristics, not
  proof that all mandatory resources succeeded.

The explicit `chat-input` typing follow-up first exposed a **harness** defect,
retained as `live-logical-margin/zai-input.json`: the guest armed input tracing,
but `diagnostic_progress(..., 'input')` raised `KeyError('input')` because the
completion-marker table knew only text/boxes/images. It was not a browser input
failure. `qmp_site.py` now recognizes a complete, newly dispatched
`[input-trace] armed ...` line. The five new parser checks were observed red
(exit 1) before this correction, then all **28** parser checks passed. The
required `test-sites-errors-negctl` explicitly fails the new complete-arm
check; the positive target requires that negative. Old/partial arm markers and
unrelated trace stages cannot satisfy the current command. Logs:
`build-loader-final-0910/input-diagnostic-before.log` and
`input-diagnostic-final-gates.log`. Python syntax checks and `test-mk-wired`
also pass. This driver-only correction does not change the measured disk.

The corrected-driver run then completed:
`live-logical-margin/zai-input-fixed-driver.json`. A native click hit the
observed `chat-input` textarea; the first `P` reached its value and painted.
The site's own bundle then moved focus to its login-required dialog, so the
remaining five letters did not enter the field. JSON records one observed
native default, subsequent paint, complete post-input boxes, and
`submitted: false` — **not** successful six-character typing. No login button
was activated, focus forced back, or authentication bypassed.

The actual dialog panel is now `(363,244,400,73)` in a 1126x562 viewport,
within half a pixel of the vertical center; the earlier observation was
`(363,0,400,73)`. The input screenshot was inspected and shows the centered
login-required panel and `P` in the field. Three content-card thumbnails have
also painted by this later observation, correcting the earlier run's
still-loading observation without pretending they were present at initial
load. The dialog's absolute close/focus control still has an over-wide
1126x28 box and an overflowing focus outline; its sizing and the dialog's
spacing are **not fixed** by centering the panel. This run's initial guest
phase was 8080 ms, versus 14,940 ms in the earlier final-snapshot run; these
varying public-site samples are not a controlled before/after speedup.

## Positioned auto widths: content, images, and disjoint click targets

The next measured defect was the real z.ai dialog's 1126x28 close-control
box, despite a 400px panel. `layout_abspos_child()` filled the containing width
when `width:auto` had fewer than two horizontal insets. That was a deliberate
old omission, not a new finding about site CSS. The adjacent source comment
keeps its old experiment: a full-corpus attempt recovered 13 test families but
reduced discriminating passes from 3101 to 3062, with CSS-stripped references
confounding the count. This continuation does **not** claim to rerun that old
corpus or rewrite its baseline. Direct geometry and actual hit ranges now
provide independent evidence for restoring the general behavior.

The width path now measures the positioned subject's min/max-content sizes,
keeps its out-of-flow descendants excluded, and computes the shrink width from
the remaining inline space. No computed style is temporarily rewritten. A
minimum unbreakable width may overflow available space; the old float helper
cannot be reused unchanged because it caps that minimum and would skip an
out-of-flow root. Min/max percentages retain the full containing-block basis.
Percentage padding is resolved against that same basis before measurement,
not against the resulting narrower box on a second pass.

Controls and images keep their intrinsic width, including when both horizontal
insets are specified; ordinary non-replaced boxes still stretch between two
insets. External replaced boxes use their known HTML width or the shared
existing default width, not fallback-child text. The default dimensions are
now shared with the canvas/video flow paths rather than copied into another
independent literal. This is size allocation, **not** playback, canvas drawing
or frame-loading support. The existing approximate flex/grid static-position
origin, full RTL/orthogonal static positioning, replaced vertical constraints,
and incomplete physical inset math remain outside this change.
The governing distinction is in [CSS 2.2, absolutely positioned widths](https://www.w3.org/TR/CSS22/visudet.html#abs-non-replaced-width)
and its [replaced-element rules](https://www.w3.org/TR/CSS22/visudet.html#abs-replaced-width).

The initial host fixture fails **21 of 87 checks** with the old code. Its
expanded form now passes **99/0**, also under ASan/UBSan with no recovery and
leak detection disabled. Cases include left/right/static anchors, margins,
min/max, border/padding, resize, long-token overflow, hidden/positioned
descendants, five ordinary control types and image/external-box intrinsic
width. `LAYOUT_POSITION_AUTO_WIDTH_LEGACY` is a required negative prerequisite
for both positive and sanitizer targets. It visibly restores a 390px box where
72px was expected and 400px controls where their independently measured
intrinsic widths are 62/76/78/13px. The gate is on `ci-host`.

Final regression positives pass: logical margins 76, positioned insets 70,
positioned projection 18, inline-flex 36, flex-column bridge 59, percentage
height 41 and decoded images 141 checks. The earlier same-change intrinsic
suite also passes 65 checks. `test-mk-wired` remains 241/240/1. Logs are
`absolute-auto-width-before.log`, `absolute-auto-width-regressions.log` and
`absolute-auto-width-final-gates.log` under `build-loader-final-0910`.

The native fixture loads a real SVG, shows an ordinary panel and asks the
driver to click a separate Count button, then Close. It compares the Close
button to an independently sized normal-flow control; page JavaScript only
observes layout and responds to native events.

| Same guest fixture | Old snapshot | New snapshot |
| --- | ---: | ---: |
| Close control width | 416 | 60 |
| Small non-replaced label width | 416 | 90 |
| Decoded image size | 396x165 | 240x100 |
| Separate button, then Close | Geometry assertion rejects run | Both native clicks succeed |

The old-image run exited 1 at `positioned auto width geometry failed:
AUTO-WIDTH FAIL`, not at boot or load. The wired
`test-absolute-auto-width-guest` requires this exact negative before running
the new snapshot. The final gate exits 0; JSON records
`native_separate_control_click: true`, `native_auto_width_close: true` and
`case_complete: true`. Open/closed screenshots were inspected: the correctly
sized green image and separate controls paint, then the panel disappears.
Both images' artifact records say `unchanged: true`. Evidence is
`build-real-sites-0910/absolute-auto-width-guest-gate.log` and
`site-general/layout/positioned-insets/auto-width-guest{,-negctl}/`.

The actual ring-3 disk rebuild completed (`absolute-auto-width-build.log`).
The immutable `snapshot-absolute-auto-width` disk is
`66ef961fd2989528cf11e7ae54f08e6c2716465c4e020b394b23083d9b765843`;
it uses the same previously recorded ISO. The old snapshot is
`snapshot-logical-margin-controls`, hash `51eb6896...40587` above.

**Full WPT sweep unavailable:** `build/wpt` does not exist. The standalone
reftest binary linked successfully but walked/judged **0** files and exited 0;
that is not passing evidence. Its denominator/report and its hardcoded
`build/reftest/failures.txt` path must not be treated as an isolated valid run.
No baseline or gate was weakened, and no corpus was downloaded silently.
An installed corpus via `make wpt-fetch WPT_ROOT=build/wpt`, followed by the
real reftest gate with an appropriate root, is still needed to settle the
broad pixel-regression boundary.

### Live-site follow-up

Artifacts are under `build-real-sites-0910/live-absolute-auto-width/`:

- **z.ai:** a native `P` enters `chat-input`, paints, and triggers the site's
  own login-required dialog. Its panel stays `(363,244,400,73)`, while the
  close/focus control is now **22x28 rather than 1126x28**. The screenshot was
  inspected: the page-wide focus outline is gone. Its offset/icon/spacing are
  not yet correct, and placeholders remain at this observation. This run
  reports the existing inline reference error. Initial guest load is 14,470 ms
  with 11,130 ms script execution; no controlled site speedup is claimed.
  Input was not submitted; neither login nor a challenge was activated.
- **GitHub Trending:** the host gets HTTP 200, but the guest receives no
  document response within its 12-second deadline. The guest connects to
  `20.205.243.166`, verifies the certificate chain and negotiates HTTP/2 before
  stalling. `FETCH-FAIL`, zero document bytes and no load phases mean there is
  no trends rendering or performance result from this run. The failure is
  after TLS negotiation; the exact transport/application cause remains open.
- **MDN position reference**, a new Web Components-heavy documentation sample:
  the title, document columns and text paint, but overlapping navigation makes
  it visibly incorrect. It reports missing `CSSStyleSheet` and
  `assignedElements`, plus fetch errors; `ERRORS` is retained, not compatibility.
  Initial guest phases total 40,370 ms, including 28,480 ms script execution.
  The exclusive module trace attributes **23,210 ms to fetch**, 690 to compile,
  30 to link and 4550 to evaluation/jobs, across 48 fetched modules and
  1,023,396 compiled bytes. This shows nested module IO inside the outer script
  phase, not 28 seconds of JavaScript CPU or a complete late-work profile.
  A separate old-snapshot replay (`mdn-position-before.json`) has the same
  overlapping navigation in its inspected screenshot, the same 21 console
  diagnostics and 334 late painted text runs. Its initial phase is 43,510 ms.
  This establishes that the visible component failures predate auto-width
  repair; it is not a controlled timing speedup or proof of pixel equivalence
  across the entire document.

MDN also disproves an older sufficiency claim worth preserving: the
`js_dom.c` comment says absent adopted stylesheets let the component library
fall back to appended `<style>` nodes. Its actual public
[component module](https://developer.mozilla.org/static/client/4585.98853dfcfa18f7de.js)
also performs `instanceof CSSStyleSheet` **in that fallback**. With the global
interface absent, style finalization throws before fallback styles are
installed. The 79,030-byte module is retained as `mdn-component-module.js`;
it was inspected, not executed on the host. Correct stylesheet object
branding/interfaces and slot distribution need implementation/verification;
neither a fabricated constructor nor a site-specific bypass was added.

## Continuation: DOM-owned stylesheet interface (2026-09-10, 07:18 CST)

The MDN failure above identifies an interface contract, not a hostname fix.
The existing stylesheet objects could already parse rules and write `<style>`
text, but were plain objects with no `CSSStyleSheet` interface. The generic
fallback evaluates `value instanceof CSSStyleSheet` even when adoption is
absent, and therefore throws before appending its styles. The implementation
now gives DOM-owned sheets their actual `CSSStyleSheet -> StyleSheet`
prototype chain, native receiver checks, read-only owner/rules getters and
prototype methods. Owner/rule references are marked for QuickJS collection.

The old public `__sx` index is replaced by private native ownership. Writeback
resolves the DOM handle before dereferencing the arena node; serial equality
alone cannot protect a freed arena. The host fixture also exposed two older
defects: lookup-only `wrapper_of()` returned null if styleSheets was the first
access to the owner node, and native rule writeback omitted JS layout
invalidation. The identity-preserving DOM wrapper and ordinary text
invalidation paths now handle these cases.

This is deliberately **not constructible stylesheet support**. `new
CSSStyleSheet()` explicitly throws, and `replace`, `replaceSync` and
`adoptedStyleSheets` remain absent. An empty successful constructor would
misrepresent support to feature detection. The current
[CSSOM interface specification](https://drafts.csswg.org/cssom/#the-cssstylesheet-interface)
also specifies constructed sheets, replacement and other contracts that this
partial implementation does not yet satisfy. In particular, existing rule
array mutability/parser validation, collection identity/liveness, external
`<link>` sheets, disabled/media effects, and synchronous author-sheet refresh
inside a guest JS geometry read remain separate work. The shadow cascade,
layout and slot distribution gaps have not been papered over. The old
`js_dom.c` fallback-sufficiency comment is preserved beside its correction.

Verification:

- The new host fixture first parsed two real stylesheet owners; the corrected
  baseline ran 52 checks and failed 48. The very first draft passed `-1` to a
  parser that requires a byte length and was discarded as apparatus failure.
  The retained `stylesheet-interface-before.log` is the corrected fixture.
- Final host gate: **52/52**, including two complete page runtime lifecycles,
  native receiver checks, owner identity, rule aliases, both fallback input
  types, real computed widths, and an owner/rules/reference cycle across GC.
  Required `NO_INTERFACE` control prints `FAIL CSS-SHEET interface exists`;
  required `NO_WRITEBACK` prints `FAIL CSS-SHEET insertRule changes real
  computed geometry`. Both were observed exiting 1. Controls that crash or
  never reach the named assertions are rejected.
- The final 52 checks also pass ASan/UBSan with no recovery; leak detection
  was disabled, so this is not a leak-clean claim. Existing CSSOM **149/149**,
  dynamic CSS wiring **9/9**, and DOM wrapper lifetime **24/24** regressions
  passed, with their required controls. `test-mk-wired`: 241 fragments,
  240 reachable, one declared. Logs live in `build-loader-final-0910/` as
  `stylesheet-interface-{before,gates,regressions,final}.log`.
- The actual browser was relinked and the independent disk rebuilt. Final
  immutable `snapshot-stylesheet-owners/disk.img` SHA256 is
  `ec159ad8686acc65b21e96dc701e43ec66bf300c8602054b61d1a4a4032cf7ad`;
  the ISO remains
  `788611412c1b89a2112b8d881cf533087927c09a8ec506e5afed552e4c0695fc`.
- `test-stylesheet-interface-guest` requires the old
  `snapshot-absolute-auto-width` disk to reach the exact `CSS-SHEET READY FAIL
  missing-interface` assertion before accepting the positive run. Final native
  clicks then show **96 -> 144 -> 96 px**, all at `(24,130)` with height 40.
  The 144px intermediate and final 96px screenshots were inspected. These
  observations are after a committed frame, not synchronous replacement
  conformance. `cssom-final/sheet-guest/results.json` has `case_complete=true`;
  positive and negative artifact hashes remain unchanged. The initial guest
  phases total 960 ms for this small owned fixture, not for MDN or a real-site
  speedup claim. The initial pre-owner-handle-guard native run is retained
  separately under `cssom/sheet-guest/` rather than overwritten.

The real MDN replay against this interface-stage snapshot exposed the next
defect, recorded below; fixture success did not settle its component layout.

### Follow-through: nested download progress painted an uncommitted DOM

The `snapshot-stylesheet-owners` MDN replay is **CRASH**, not a pass with fewer
errors. It advanced beyond the old missing interface and faulted at
`0x451f4e20`; disassembly of the corresponding `browser.elf` identifies the
first source-byte load in `css_shadow_parse+0x50`, with fault address
`0x6000000060`. Evidence is retained in
`build-real-sites-0910/live-stylesheet-owners/mdn-position.{json,serial.txt}`.

The exposed generic ordering defect is in `browser.c::load_tick`: synchronous
nested module IO calls the download progress callback while JavaScript is
still executing. The callback used to repaint even if the script had dirtied
the DOM and the outer frame had not rebuilt its display list. Generated
pseudo styles are borrowed by that list and can be replaced with the DOM.
The callback now preserves the last committed pixels whenever the DOM is
dirty. It does not consume the invalidation, execute callbacks or recursively
settle/layout from a transport callback; normal outer settling owns the next
paint. Close-event handling remains before the guard.

The host control deliberately uses an ordinary **append**, not freed memory:
it proves that the old callback paints an uncommitted tree without needing to
crash the apparatus. A clean progress callback paints, a dirty callback does
not, and after the outer settle the new text paints. The old build fails
exactly the dirty-paint assertion while retaining the clean and post-commit
controls. The new build passes all **8** observations, including ASan/UBSan
with recovery and leak checking configured as above. The required negative
is a prerequisite of `test-progress-paint`, and both new regular host gates
are prerequisites of `ci-host`.

Two apparatus corrections are retained in the test history rather than
reported as product failures: multi-word assertions initially looked for a
single painted run across word boundaries; then a forward-referenced make
source list was absent when prerequisites were expanded, leaving an old
positive binary after `browser.c` changed. Single-token paint markers and
concrete source/header prerequisites fixed those checks. The actual retained
baseline in `progress-paint-before.log` has one intended failure and seven
positive observations. Logs are `progress-paint-{before,gates,sanitize}.log`
and `stylesheet-interface-final-regressions.log` under
`build-loader-final-0910/`. The latter reruns CSSOM 149, live CSS 9 and DOM
wrapper lifetime 24 on the final stylesheet owner implementation, all green.

The actual ring-3 browser/disk was rebuilt again into immutable
`build-real-sites-0910/snapshot-stylesheet-progress/`. Its disk SHA256 is
`f910a265e53837ea9f0007cb31ff1557fa07c0aeb95bb707d8eee5a88f2b54b1`;
the ISO hash is unchanged. This is the current continuation snapshot; the
earlier interface-only/crashing snapshots are kept as evidence, not replaced.
The required native stylesheet gate was rerun on this combined snapshot:
old-disk missing-interface control observed red, then **96 -> 144 -> 96 px**
via native button clicks, `case_complete=true`, artifacts unchanged. Its
independent results are under `cssom-progress/sheet-guest/`, with command log
`stylesheet-progress-guest.log`; initial guest phases are 1060 ms for the
owned fixture. This supersedes the earlier 960ms fixture observation as the
combined-build verification, not as a meaningful performance comparison.

Real MDN replay on this snapshot (`live-stylesheet-progress/mdn-position.json`):

- **No app fault**, load completion present, 48 modules loaded with zero module
  fetch failures. The prior CSSStyleSheet ReferenceErrors are absent.
- **Still ERRORS**, with 28 page-reported diagnostics: 19 missing
  `assignedElements`, four missing `hasFocus`, four downstream `coordsAt`
  reads and one undefined `toString` read. This is additional executed code,
  not a score improvement by hiding exceptions. The late screenshot was
  inspected: the article title and columns paint, but navigation menus still
  overlap the article. No component-layout compatibility claim is made.
- Initial guest phases total **26,660 ms**: 15,040 script execution and 6640
  lifecycle settling. Exclusive module intervals are 7800 ms fetch, 660 compile,
  80 link and 6490 evaluation/jobs, over 1,023,396 compiled bytes. The earlier
  non-crashing pre-interface sample took 40,370 ms with 23,210 ms module fetch;
  that network change prevents attributing the timing difference to this fix.

Full WPT remains unrun because the corpus is absent. Constructed stylesheets,
adoption, shadow/slot styling and layout, document focus state and the newly
executed editor path remain separate acceptance work. No public login,
challenge handling or video playback was performed.

## Remaining acceptance work

1. The named `files` setter is fixed and z.ai's late form is visible. The old
   six-character input screenshot is confirmed, but bounded event scheduling
   exposes a legitimate login-required focus change after the first key. The
   previously off-screen dialog is now visible. Continue with its uncentered
   flex/auto-margin layout and expensive handler/callback phases; do not defeat
   login or force focus back. Keep load, first usable frame and continuous
   interaction as separate acceptance boundaries.
   Correction beside that older next-step claim: the logical auto-margin,
   inset-sized flex and block control/image placement defects now have the
   host and native red-to-green evidence above, and the real z.ai panel is
   centered after ordinary typing triggers it. Its over-wide absolute close
   control, spacing and sustained responsiveness remain acceptance work.
   Further correction: positioned auto sizing now removes that over-wide box
   in both native fixtures and the real page; close-control offset/icon,
   spacing, and sustained responsiveness remain outstanding.
2. Reduce Bing's actual hide/restore lifecycle and the folded layout defects;
   verify GitHub trends, Apple and DeepSeek with screenshots/interactions.
3. The exclusive module instrument is now wired and measured on z.ai, but both
   instrumented GitHub attempts failed before the document. Obtain a successful
   GitHub trace before assigning its 44.27 s script phase to compilation vs
   nested IO. Module-graph loading remains synchronous; frame-owned/nonblocking
   loading with held-child input controls is still outstanding. Candidate
   filtering has a guest workload win, not yet a real-site before/after claim.
4. Add aggregate transport/buffer admission: direct `bxfer_dial` still retains
   an unpooled session if pool admission fails. Do not turn this into a silent
   unlimited socket path or a fatal error where a queue is required.
5. Continue image/layout coverage (responsive candidates, background images,
   decoded pixels) and repeat genuinely different real-site classes. Whitespace
   no longer consumes positioned flex layers. The former next step, decoded
   intrinsic width in shrink-to-fit flex/picture wrappers, is now fixture- and
   Apple-hero-verified above. Remaining image/box disagreements, non-JS direct
   flex/grid image discovery, responsive sources and full hint cascade stay
   separate from Bilibili's transport timeouts and title truncation.
6. Network-family/TLS diagnoses need route/address evidence. No TLS validation
   was disabled. Hardware pointer-plane positioning is separate from text caret
   advance and was not fixed by the caret tests.
7. Keep the newly isolated CSSOM transformed-rect projection gap distinct from
   media conditions. Physical inset calc, physical/logical cross-cascade,
   logical writing-mode mapping and full CSS math are not completed here.
8. The additional MDN specimen exposes absent CSSStyleSheet identity in a
   fallback path and absent slot assignment interfaces. Address these as
   actual stylesheet/DOM contracts, not by fabricating successful constructors,
   returning empty assigned-node lists or suppressing the visible menus.
   Correction beside that earlier next step: DOM-owned stylesheet identity,
   owner binding and rule writeback now have the host/native evidence above.
   Constructed sheets, adoption, full CSSOM collection/rule semantics and slot
   distribution remain outstanding; interface availability alone is not
   component rendering compatibility.
   Further correction: real MDN now completes without the intermediate
   progress-paint crash or missing stylesheet interface, but slot/focus/editor
   errors and overlapping menus remain, as recorded above.

No complete-site compatibility or first-token claim is made. The shared dirty
worktree was preserved; no commit, reset or unrelated cleanup was performed.
