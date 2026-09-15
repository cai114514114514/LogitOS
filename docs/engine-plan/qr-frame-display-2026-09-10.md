# WeChat login frame: confirmed blank-box cause and bounded implementation choices

## Current evidence

The current guest's public login screenshot is
`build-terms-layout-evidence/ds-live/login.png`. Its right-hand WeChat login
panel contains a blank frame box. The same guest's `serial.log:539` reports:

```text
[iframe] cross-origin navigation refused: https://open.weixin.qq.com (top is https://chat.deepseek.com)
```

No phone number, verification code, Cookie value, QR payload, OAuth state, or
complete authorization URL is needed for this diagnosis. No foreground input
was changed, no SMS/verification action was taken, and no QR session endpoint
was requested during this investigation.

The previously captured real page CSS in
`build-modal-hit-fix/loaded-styles.css` contains:

- `.ds-sign-in-with-wechat-block__wrapper`: a 200px-tall clipped flex container.
- Its `iframe`: width 200px, height 100%, no shrinking.
- Its `:empty` rule: hide the wrapper when no child exists.

The upstream public SDK at
<https://res.wx.qq.com/connect/zh_CN/htmledition/js/wxLogin.js>, fetched normally
into `build-qr-display-fix/public/wxLogin.js`, creates an iframe with an
`open.weixin.qq.com/connect/qrconnect` source. It passes the caller's existing
callback/state options through and listens for origin-checked ready messages.
This confirms the mechanism; it does not establish the login page's current
option values or the actual callback behavior after authorization.

The normal unauthenticated homepage fetch returned HTTP 429 on this host;
the web reader returned 403. Neither was retried with changed identity.
Existing and current guest evidence was sufficient for the conclusion.

## Why changing canvas or removing an origin check cannot repair it

`js_platform.c::startLoad` explicitly refuses a cross-origin frame before its
network fetch. Therefore the outer page never obtains the frame document that
would contain the QR image/script. This is the immediate cause.

A second independent barrier remains even if a same-origin child is used:
`js_frame.c` records and implements no child pixels. The child is a parsed
DOMParser document with a limited execution realm. It has no full child DOM,
Web APIs, timers, external-script loader, or embedded viewport. The public
`layout_page/layout_items/layout_free` interface in `layout.h` owns one global
display list. No painter call composites `iframe.contentDocument` into its box.

The single-active-realm guard in `js_webapi_install` is necessary: installing
again would replace the parent's location, fetch/timer state and other globals.
The recent explicit Cookie/request owner work does not instantiate these modules
for simultaneously live child documents.

Consequently this is an absent embedded browsing context, not demonstrated QR
encoding, SVG decoding, canvas painting, CSS opacity, or Cookie failure. Removing
the cross-origin refusal or installing the parent's APIs into the child would
silently associate untrusted child data with the parent's document identity.

## Preferred complete path: document ownership before external pixels

The unit of work should be a browsing-context instance with a stable id and
navigation generation, parent/top links, committed origin and URL, viewport,
DOM/JS context, per-document resource and task ownership, and independent
style/layout output. A parent keeps a frame handle and rectangle, not an
unrestricted child DOM pointer.

| Stage | Required production scope | Acceptance boundary |
|---|---|---|
| 1. Independent document state | `js_dom.c` and bindings, `js_page.c`, `js_webapi.c`, `js_frame.c`, `page_runtime.[ch]`; per-realm classes/wrappers and task delivery | Two simultaneous local documents mutate/query their own DOM, timers and fetches; navigating/removing one cannot retarget the other's callbacks. Cross-origin parent reads remain denied. |
| 2. Actual embedded output | `css_engine.c`, `layout.c/h`, image ownership, `browser_paint.c`, browser frame loader | Independent child CSS and display list, clipped/transformed into its viewport; real child image/text pixels visible in the guest. Parent styles and geometry survive child reflow/removal. |
| 3. Working interactive widget | `js_platform.c` WindowProxy, child external-script scheduling, `postMessage`, navigation/focus/input dispatch, transport policy | Origin-checked messaging with a real receiver and source, permitted child/top navigation, correct input coordinates, redirects and destruction. An ordinary two-origin fixture completes an image-ready and callback exchange. |

Loading must preserve ordinary transport rules and explicit Cookie request
context: an iframe navigation is not a top-level navigation; site-for-cookies
comes from the ancestor chain. Redirects retain the requesting owner, while the
new child document receives the final committed origin. `sandbox`, embedding
restrictions such as `frame-ancestors`/X-Frame-Options, and unsupported document
schemes must be enforced or explicitly refused, not silently ignored.

A passive frame-rendering stage can prove QR-shaped image visibility only.
It cannot be reported as completed scan login: the real provider page may use
script to create/refresh the image, poll state, or navigate after authorization.
Each stage needs a local negative control and guest behavior, not only a second
JSContext or a linkable API. No implementation should branch on the provider.

This is a coordinated module-instancing change, not a small QR patch. This
investigation did not start that refactor while shared layout work is active.

## Initial interim proposal: user-selected “Open frame in new tab”

The existing browser can create and navigate a top-level tab (`tabs_new`,
`tabs_select`, `load_from`), but has no native frame-opening action. Right-click
currently dispatches the page's `contextmenu` event; there is no browser frame
menu. Thus manual native opening is implementable in a small browser UI patch,
not an already working feature.

A generic action would hit-test an iframe, resolve and copy its current `src`
against the committed owner document before freeing any DOM, and navigate the
unchanged HTTP(S) URL in a new foreground tab. It would not rewrite provider
parameters, replace the site's DOM, fabricate an image, or open automatically.
A too-long URL must fail explicitly rather than be truncated to `TAB_URL`.
Unsupported `srcdoc`, opaque sources, or sandboxed frames need an explicit
refusal until their navigation policy is represented. The new request must use
the original document as initiator; it must not falsely gain the typed-URL
`browser_initiated` classification merely because browser UI handled the click.

Likely code scope: `browser.c` native menu/action and a small helper/test;
`tabs.[ch]` only if a shared open-and-select helper is warranted. No frame-origin
DOM relaxation, network override, or layout singleton rewrite is needed.
A local fixture should verify exact source navigation, correct initiator,
unsupported-source refusal, stale/deleted element handling and no automatic
navigation from a page synthetic event.

### Callback limits of that interim path

- The existing tab model has exactly one live tab. `tab_dehydrate` closes the
  parent's JS runtime and cancels its timers/fetches; it does not preserve an
  OAuth callback closure or message listener running in the background.
- `sessionStorage` is keyed by the top-level tab id. A new tab does not share
  the original tab's session partition. Cookie/localStorage sharing does not
  prove that an application state check will succeed.
- The SDK's `self_redirect` option and the provider's callback implementation
  determine whether completion expects child navigation, top navigation or a
  live parent message receiver. The public SDK alone does not reveal this
  application's selected mode or callback logic.
- A provider page that redirects itself to a server callback using its existing
  state may work top-level. A callback that needs the destroyed parent listener
  or original tab session state will not be repaired by displaying its QR.

Therefore native frame opening is a plausible user-controlled route to show
the provider page, with callback success explicitly unverified. It must not be
presented as a completed WeChat login or as full iframe support. The full path
above is required for a dependable embedded login flow.

## Implemented interim action: same-tab “打开嵌入页面”

Correction to the initial proposal above (2026-09-10): the chosen action opens
the source in the **existing tab**, preserving that tab's sessionStorage
partition. The previous proposal and its new-tab limitation are retained so
readers can distinguish the rejected variant from the code now present.

`frame_open.h` owns the source eligibility and inset button rectangle.
`browser_paint.c` draws native chrome inside the existing iframe rectangle;
no author DOM or layout node is created. Its trusted hit helper consumes the
same rectangle, and the normal frontmost hit test retains clip, modal, inert
and pointer-events filtering. Tiny or transformed frame boxes do not expose
this action. This is a pointer action; there is no keyboard frame chooser yet.

`browser.c` records an actual left-button press, requires a release on that
same button and honors click cancellation. It re-hits and checks the live
node/serial after both mouseup and click script checkpoints; teardown clears
the stored press before the document is freed. Synthetic DOM clicks never
call the native action. `frame_open.inc` copies the current src and committed
initiator before calling the existing `load_from` navigation path.

The URL Standard parser resolves the source against the committed document
URL. Encoded query bytes are retained in the action regression. URL, host and
path lengths must also fit the existing transport buffers: a truncated target
is rejected. Sources with sandbox/srcdoc, unsupported schemes, userinfo,
unsupported IPv6 authority, control/space/backslash bytes, or excessive lengths
do not navigate. There is no alternate `<base href>` resolution implemented in
this native action yet. The full browser already links the URL parser;
minimal loader-only host variants without it explicitly leave this action
unavailable rather than falling back to the truncating transport parser.

Host evidence: `make BUILD=build-qr-display-fix test-frame-open` drives real
`app_main` input and production DOM/layout/painter against local fake HTTP.
Eleven modes pass: absolute source with encoded query, relative source and
same-tab sessionStorage, preventDefault, removal in a handler, sandbox added
in a handler, initial sandbox, srcdoc, data URL, total URL overflow, path-buffer
overflow, and unsupported userinfo. The required negative control removes only
the native action and visibly fails the three destination/initiator/paint
assertions. `test-mk-wired` finds this gate through the Makefile.
`test-frame-open-sanitize` also passes the open, remove-in-handler and late
sandbox modes under ASan/UBSan (`detect_leaks=0` on the Darwin host); no memory
or undefined-behavior diagnostic was emitted. Combined command and results:
`build-qr-display-fix/frame-open-final.log`.

This evidence proves native independent opening in the host fixture. Guest
button visibility, provider QR display, scanning and callback completion are
separate acceptance boundaries. No live scan or login was performed by this
implementation task, and the cross-origin embedded-document guard is unchanged.
