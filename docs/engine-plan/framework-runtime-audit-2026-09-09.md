# Framework runtime / frame wiring audit — 2026-09-09

Read-only source audit after the WAAPI checkpoint correction. No site-name,
bundle-name or framework-name branching is proposed. These are production
call-chain findings, not claims that any particular real site passes/fails.
Line numbers refer to the shared working tree at audit time.

## 1. Parser iframe adoption runs before its native installer

**Definite missing invocation, small ordering fix.** `js_page.c:1234` invokes
`js_platform_install`. Its JS prelude calls `installIframes` (`js_platform.c:3884`),
which synchronously initializes the existing parser nodes (`:3701–3702`). A
`srcdoc` or `about:blank` document reaches `settle`; native adoption is guarded
by `typeof G.__frameAdopt` (`:3254–3255`). But `js_frame_install(ctx)` is called
only after the prelude returns (`:3979`). No later adoption loop is present.
The existing comment at `:3971` says `installIframes` only defines getters;
the initial-node scan makes that claim stale.

This distinguishes parser `srcdoc` bootstrap from dynamic insertion and
asynchronous same-origin fetch: the latter can arrive after the installer.
A document object and a load event do not prove its inline script ran.

Minimum change: install the frame native hooks before executing the prelude,
after verifying DOMParser registration already exists, or explicitly adopt
already-committed readable documents once hooks are installed. Preserve
navigation generations and avoid executing scripts twice. Gate: parser srcdoc
inline script emits an independent console marker, dynamic insertion emits its
own marker, both occur once and before their respective load observations.

## 2. `contentWindow` is disconnected from the frame execution context

**Missing bridge, plus explicit unsupported boundaries.** `makeWindow` builds a
parent-realm plain object (`js_platform.c:3387–3406`); `:3419` installs
`win.postMessage = function () {}`. The frame really executes inline scripts
through `js__frameAdopt` → `frame_install_globals` →
`js_domparser_offer_scripts` (`js_frame.c:354–392`), but that context has only
window/self and console (`:220–233`). It receives neither the `makeWindow`
object nor a window message EventTarget. Parent-to-child messages terminate
without delivery; child-to-parent `parent.postMessage` cannot use a real parent
binding. The plain object's location getter also returns `G.location`, not the
child document's committed URL (`js_platform.c:3391`).

Do not call this a one-function queue fix: the receiver/window identity must
first become authoritative across `contentWindow`, `defaultView`, parent/top,
location and the executing realm. Then use the existing page-owned task queue,
clone at send time, check origin at delivery, and invalidate queued tasks on
frame navigation/removal. A same-origin bidirectional ping/pong gate should
assert origin, source identity, FIFO order and stale-navigation cancellation.

Separate refusals remain explicit: frame external scripts and non-classic
script types log refusal (`js_frame.c:291–302`); no frame DOM/fetch/timers are
installed. Cross-origin and sandbox frames are refused before adoption
(`js_platform.c:3309–3320`). Do not remove those guards as a shortcut to widget
compatibility. [HTML cross-document messaging](https://html.spec.whatwg.org/multipage/web-messaging.html#web-messaging)
defines delivery against the target window and source/origin identity.

## 3. Shadow DOM construction exists, but layout never consumes its flat tree

**Implemented primitive without render consumers.** `js_dom.c:4220–4262`
validates and creates a shadow root with `dom_attach_shadow`, and the method
is exposed at `:4360`. `dom_flat_first_child` / `dom_flat_next_sibling` exist
(`dom.c:1144`, declarations `dom.h:517–518`), but a browser-source search finds
no callers. Current `layout_first_raw` / `layout_next_raw` use authored
`first_child` / `next` around generated pseudo content (`layout.c:407–425`).
Thus component initialization can succeed while shadow content never reaches
the display list and light content remains visible.

The `js_platform.c:2136` comment claiming attachShadow does not exist is stale;
`dom.h:493–516` correctly describes the still-missing render consumer and slot
assignment. This is not solved by advertising another JS method.

Minimum first slice: one authoritative formatting-child iterator that merges
generated content with shadow flat children, used consistently by intrinsic,
block, flex and grid layout. Pair it with scoped shadow styling; do not expose
page styles inside a shadow root by simply traversing it with the document
cascade. Slot assignment is still missing and needs its own real producer.
Gate: shadow-only label is painted, light-tree label is suppressed, then
assigned-slot/fallback behavior and scoped-style isolation are measured.
The [DOM shadow-tree model](https://dom.spec.whatwg.org/#shadow-trees) supplies
the ownership/root distinctions; successful `attachShadow()` alone is not a
rendering result.

## 4. MessageChannel delivers sender-owned objects, despite an existing clone helper

**Wrong data semantics in a live scheduler transport.** MessagePort's
`postMessage` captures `data` directly in `setTimeout` and delivers it unchanged
(`js_platform.c:611–618`). In contrast, the same file's BroadcastChannel
(`:716`) and window.postMessage (`:827`) call `G.structuredClone` before queuing.
A sender mutation after postMessage therefore changes what the port recipient
observes, and functions/non-cloneable values are not rejected by this route.
The close path only nulls its own peer pointer (`:604`); pending closures retain
the destination and there is no closed/generation test before delivery.

Minimum change: reuse one serialization/refusal implementation synchronously
at send, preserve FIFO after microtasks through the existing queue, and add an
explicit closed/generation fence. Refuse unsupported transfer lists rather
than ignore them. Gates: mutate a nested object after send and observe the
original snapshot; function payload throws; close before delivery suppresses
delivery without leaking the retained task. [HTML message ports](https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports)
define asynchronous delivery of serialized data, not shared object references.

## 5. Custom-element wrappers rescan even an empty registry and miss lifecycle doors

**Avoidable framework startup work and incomplete lifecycle wiring.** Every
appendChild/insertBefore/replaceChild wrapper calls `upgradeTree(arguments[0])`
(`js_platform.c:2304–2312`). `upgradeTree` recursively visits descendants
(`:2179–2190`) even when `defs` is empty; this creates JS wrappers and property
reads for a registration lookup that cannot succeed. This is a code-derived
extra-work finding, not a new guest timing measurement.

Separately, `upgradeOne` immediately returns for an existing `__ceState`
(`:2150`). Removal delivers disconnectedCallback only through the removeChild
wrapper (`:2316–2326`), while reconnection calls the same upgrade path that
returns before connectedCallback. Native innerHTML/replaceChildren removal
also does not enter that JS removeChild wrapper. A component can therefore
retain subscriptions after removal or miss initialization on reinsertion.

Minimum change: empty-registry fast return for the scan; keep construction and
connection-state transitions separate. Feed reactions from the authoritative
DOM insertion/removal mutation stream instead of adding another partial wrap
list. Gates: count native wrapper visits while inserting a large fragment with
zero definitions; define one element and verify exactly-once construction,
connect → disconnect → reconnect order through removeChild, innerHTML and
replaceChildren. Keep the performance control separate from behavior, so a
skipped callback cannot be sold as a speedup. See [HTML custom-element reactions](https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-reactions).

## Verification boundary and current integration

This audit did not edit production code or run guest probes for these five
findings. The first item is suitable for a narrow initial fix; the frame-window
and shadow-render items require lifecycle ownership work, not blind installer
calls. Startup optimizations already landed for iframe/details scans, selector
iteration and fragment insertion are not counted again as open defects.

The preceding WAAPI fix is independently verified: after `css_anim_tick`,
`js_page_run_due` now drains Promise jobs inside the same interrupt slice and
adds their count to the work result, even for a constant-value animation.
`test-waapi-paint` runs both omission controls first (7 paint failures, 2
checkpoint failures), then passes 39 checks. Pure ASan passes the same 39;
page-runtime passes 45; test-mk-wired passes. Logs:
`build/wiring-next/waapi-checkpoint-gate.log` and
`build/wiring-next/waapi-checkpoint-verified.log`. Production code was frozen
before this audit; root owns rebuilding the disk and rechecking the guest
completion marker.
