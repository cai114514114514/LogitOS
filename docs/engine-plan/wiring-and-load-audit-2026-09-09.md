# Browser wiring and style/script load work — 2026-09-09

This continues the expansion integration. The user's clarification was that the
main delays happen while loading styles and running scripts. Measurement was
therefore split into resource download, CSS cascade, extension cascade, layout,
JS runtime installation, external script download and script execution.

## Evidence boundary

All reported milliseconds below come from the running ring-3 browser's guest
clock: QEMU TCG, 4 vCPUs, 1 GiB, 1280×800 display, one measurement VM at a time.
The desktop's already-running VM was left alone. Two sequential samples per
case are retained; these are small controlled comparisons, not statistical
claims about arbitrary live sites or physical hardware.

`tools/perf/browser_load.py` replays committed HTML/CSS specimens. For CSS
attribution it removes scripts and image network requests, serves retained CSS
locally, and appends a completion probe. It preserves the SVG workload. This is
an explicit isolation fixture, not a successful real-site compatibility result.
The large Wikipedia specimen still hits the existing 16,384-item display-list
limit. Both compared versions hit it; this work does not repair that limit.

## Confirmed bottlenecks and changes

| Guest measurement | Before, median | After, median |
| --- | ---: | ---: |
| DeepSeek retained specimen: full styled layout | 2815 ms | 185 ms |
| Same specimen: navigation through script/lifecycle startup | 3860 ms | 1095 ms |
| Wikipedia retained specimen: JS environment installation | 1780 ms | 385 ms |
| Same large specimen: navigation through startup | 5085 ms | 3480 ms |
| Script workload: 600 elements, 10 queries + 60 fragment inserts | 555 ms | 40 ms |
| Delayed-image fixture: return to interactive event loop | 3695 ms | 535 ms |

The dominant style delay in the first specimen was rasterisation *inside flex
measurement*. A full pass decoded inline SVG 78 times; 2900 guest milliseconds
were in the decoder. The same source is now decoded once within one layout
pass, with trial and final display items borrowing the resulting bitmap.
The final full pass decodes eight SVGs. A first 16 MiB cache trial failed to
improve time: two source icons each rasterise at the decoder's 2048² limit, so
it cached small icons and missed the expensive ones (38 decodes / 2920 ms).
The 64 MiB bound covers those final bitmaps. Cache overflow retains the original
item-owned behavior; it does not drop artwork. It is a reuse-cache limit, not a
total image-memory limit. The cache is destroyed at the next layout, so source
changes cannot reuse stale pixels.

Browser initialization formerly traversed the whole document through JavaScript
queries to initialize only parser-created iframe/details elements. The shared
private native scanner now wraps matching nodes only. The work-count regression
measured 12,144 → 114 wrappers; parser iframe loading, named-details exclusivity
and dynamic details behavior still execute. No public feature is removed or
replaced by a placeholder. `JS_INSTALL_PROFILE=1` enables the per-installer guest
instrument; normal compilation omits that instrumentation.

Two native/script traversal defects were also corrected: fragment insertion
scanned the existing parent subtree again, and selector traversal constructed a
new child collection at every visited element. The regression records named and
script scan visits 4252 → 24 each, script offers 45 → 9, and transient query
collections 531 → 0. These operation counts alone were **not** a speed result:
the first guest query workload remained around 530 ms because matching/getter
work still dominated. A further private native fast path is selected only after the complete selector
parser proves one simple tag/class/id/universal selector. Complex selectors
remain in the full matcher. The unchanged guest fixture now reports query
510/550 ms → 20/20 ms, and total script work 540/570 ms → 40/40 ms, with 6000
matches in every run. This improves DOM-heavy script work; it does not claim
the interpreter's arbitrary arithmetic or all third-party bundles got faster.

Empty `::before`/`::after` styles are now rejected before composition only when
computed content is explicitly `normal` or `none`. Inherit, attribute content,
and empty strings remain on the real composition path. The reset-sheet control
went from 16 compositions to zero. On the large specimen this is a smaller
improvement than layout or JS installation; no disproportionate claim is made.

Pictures no longer block runtime initialization and page script execution. Eight
requests are kept live across event-loop turns; ready bodies alone are decoded.
Native keyboard events and timers execute before the delayed image settles.
Window load/pageshow are deferred until this implementation's displayed-image
work is terminal; load-handler DOM changes are then settled and painted. Bad
URLs terminate, removed-image/navigation requests cancel, and tab hydration
reuses retained resource bytes. This does not add the still-missing per-image
load/error events or fetching for images absent from the display list. The
HTML standard's image/load distinction is the reference, not a claim of full
resource-event compliance: [HTML images](https://html.spec.whatwg.org/multipage/images.html).

## Wiring repaired

- Production CSSOM now calls the embedder's style/layout flush. Consecutive
  write/read pairs get separate flushes, and consuming dirty style for a read
  still leaves a repaint scheduled.
- The later CSSOM install preserves the existing live matchMedia object;
  stylesheet media and script media use the same evaluator. Startup and resize
  publish the actual page viewport. Reference: [CSSOM View](https://drafts.csswg.org/cssom-view/).
- `@supports(content:…)` and `CSS.supports` share the actual generated-content
  subset. `counter()` and URL content remain rejected. Nonempty stylesheets
  with no extension rules no longer suppress inline extension properties.
- Native focus now flushes current styles without clearing pending paint.
  CSS read caching uses the DOM's monotonic mutation generation. The old
  fingerprint used allocated arena capacity, which does not change for every
  mutation; same-turn hidden→shown→hidden transitions could therefore reuse
  stale styles. Two independent controls reproduce the missing flush and stale
  cache. Semantics 89/89, focus 9/9 and computed-style 35/35 now pass.
- Wasm classes register in every new runtime; page teardown calls its resource
  reset before destroying the context. Controls reproduced both second-page
  pool exhaustion and the retained-import-cycle shutdown assertion.
- Unsafe successful fetch/XHR responses now consume the cache invalidation
  implementation. A real guest GET → cached GET → POST → GET sequence observes
  version 0 → 1 and the corresponding server request counts. This invalidates
  the target resource; optional Location/Content-Location invalidation is not
  added. Reference: [RFC 9111 §4.4](https://www.rfc-editor.org/rfc/rfc9111.html#section-4.4).
- Stylesheet activity uses rel tokens and disabled state, replacing the old
  filename guesses for accessibility themes. A normal `high_contrast.css`
  stylesheet now actually applies. Modal keyboard fallback targets the modal
  when its control has been blurred, and keypress rereads a target that a
  keydown listener may have removed.

## Verification and remaining scope

`make test-browser-expansion` includes the new loading, SVG reuse, CSS wiring,
Wasm lifecycle, cache invalidation, bootstrap and traversal gates. Their negative
controls are prerequisites and were watched failing. `make test-browser-wiring`
checks expanded make source/object inventories, .inc ownership, strong symbols
in the actual ELF and reviewed source consumer edges. Its negative control
removes a reflow registration in a temporary source copy while retaining the
ELF; it reports `MISSING_CALL cssom_reflow`. This is source/link evidence, not a
runtime certificate.

The SVG ownership/overflow suite also passed ASAN/UBSAN. Before/after page-content
crops (x=140..1259, y=190..699) for both retained specimens are pixel-identical.
This verifies preservation of the existing rendering, including its existing
limitations, rather than claiming those pages are fully correct.

The independent cache guest driver initially failed in its apparatus: it waited
for a load event on an unloaded New Tab, then raced the boot-time Finder launch.
It now waits for browser startup and lets initial desktop setup settle. Only the
subsequent actual cache/version pass is treated as product evidence.

See [the explicit wiring inventory](wiring-inventory-2026-09-09.md) for reviewed
remaining gaps: full text formatter, vertical writing, element scrolling,
Popover native paint, and WAAPI paint. Additional findings include incomplete
native mutation notifications outside text/split, Selection/Range association,
Wasm exported-function cycles, and synchronous external dynamic-script fetches.
They are not silently counted as completed by the new green audit.

Raw guest phase records and screenshots are in
[evidence/wiring-performance](evidence/wiring-performance). Build-only and
host-only checks remain distinguished from these guest measurements.

## Final release acceptance

`make -j6 build/disk.img test-browser-expansion` completed with exit 0 after a
forced fresh browser ELF/AEX link. The production image has the initialization
profile flag OFF; its flag stamp forces recompilation when the setting changes.
A final boot of **build/disk.img**, not a scratch image, passed:

- the script workload: 6000 matches, 20 ms querying + 20 ms insertion;
- slow image: input and timer callbacks before window load, startup 530 ms;
- CSS support/generated text, consecutive geometry reads, same-turn native
  hidden/shown focus checks, and a real WM resize triggering matchMedia change.

The source/ELF wiring audit and all prerequisites passed on this rebuilt binary.
`release-artifacts.json` records final ELF, AEX, disk and ISO hashes. The eight
owned before/after image files were deleted (4 GiB); compact logs, JSON and PNG
screenshots remain in this document's evidence directory. An unrelated active
`build-weakfin` VM was left intact; it does not run this release's main disk.
