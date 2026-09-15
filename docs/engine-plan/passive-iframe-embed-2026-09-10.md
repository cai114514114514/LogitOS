# Passive iframe document embedding — 2026-09-10

This is the implemented continuation of `qr-frame-display-2026-09-10.md`.
The earlier native “open embedded page” action is retained as a fallback for
unavailable documents. This change actually loads and paints an independent
child document inside the original iframe. It does not complete scripted
browsing contexts or a WeChat login.

## Ownership and request path

`passive_frame.[ch]` owns the child's HTML buffer, parsed DOM, CSS source and
expanded backing, layout context, image cache, request ids, and viewport. It
never publishes a child DOM wrapper to the parent's JS realm. Layout and CSS
contexts are independently owned through `layout_context_create/activate/destroy`.
The painter activates only completed child display lists, clips to the iframe
and parent rectangular overflow, then restores the parent context. Its text
and dirty-region recorder remain part of the single outer paint operation.

`bfetch_start_embedded` takes an explicit document owner and top ancestor. The
frame document is an ordinary subframe navigation; images/styles are owned by
the child document and retain the ancestor. A cross-site/scheme ancestor makes
the child's site-for-cookies opaque. Neither mutable parent fetch base nor a
pretended top-level navigation supplies these Cookie decisions. Child network
responses never pass through parent fetch/CORS/DOMParser to expose their bodies.

Every redirect is checked before the next request. Full repeated CSP/XFO
headers are retained; omitted/truncated/overflowed metadata is unknown, and
unknown policy refuses embedding. Embedded requests bypass the old response
cache because it does not retain this metadata. Parent cache/hydration responses
without complete policy metadata also refuse embedding. H2 header-copy failure
now stops the exchange before headers/body delivery, rather than silently
omitting a failed CSP or XFO field.

`iframe_policy.[ch]` enforces parent frame-src/child-src/default-src, child
frame-ancestors/XFO, stylesheet/image sources, inline style/nonce, style
attributes, and base-uri. Policies intersect. Meta frame-ancestors is conservatively
refused instead of being treated as a response policy that could override XFO.
Blocked base URLs are ignored in favor of the document URL. A newly installed
restrictive parent meta policy revokes an already displayed frame. Policy
history deduplicates whole lines, never arbitrary substrings.

## Limits that are intentionally visible

- Four child documents; depth one; sixteen style elements/links and thirty-two
  images per child. HTML 256 KiB, each CSS 128 KiB, each image 512 KiB, aggregate
  raw resources 2 MiB per child; viewport dimensions at most 2048. Layout contexts
  separately cap retained decoded image pixels at 8 MiB, not transient decoder
  allocation.
- HTTP(S) only; credentials-in-URL, unrepresentable transport URLs, mixed content,
  sandbox/srcdoc/credentialless/iframe csp and CSP sandbox are refused. Hidden
  frames wait for actual geometry and can begin when they become visible.
- Passive HTML/CSS/images only. Child scripts, nested document loading, child
  navigation/form submission, child focus, timers, postMessage, and script-visible
  browsing contexts are absent. Media/canvas and controls never call the parent's
  global realm/form callbacks. Password/file markup values are not painted.
- Documents containing script elements receive a native footer:
  **“嵌入预览，页面脚本尚未运行”**. It is painted outside author DOM and indicates
  that static template labels are not an authentication result.
- Stylesheet `@import` loading, downloaded child fonts, data-URL image loading,
  transformed iframe painting, child scrolling, and inherited rounded overflow
  masks are not implemented. Rectangular viewport/overflow clipping is enforced.
  The loader retains unsupported-import diagnostics. Stylesheet link media
  attributes are not yet evaluated separately from the stylesheet's own media rules.

## Observed verification

All builds and writable fixtures use `BUILD=build-iframe-embed-fix`; the user's
running/default QEMU and `build/disk.img` were neither stopped nor overwritten.

- Full host browser fixture: twelve modes covering actual text/SVG pixels,
  allowed CSP, XFO, sandbox, parent frame policy, unknown metadata, hidden-to-visible
  geometry, dynamic meta tightening, removal, exact MIME, blocked base fallback,
  and a script-free document. Parent URL, tab count, child DOM isolation and absent
  child script/nested fetches are observed. Native footer has both present and
  absent cases.
- Disabling only nested paint causes exactly the required two failures: missing
  child text and decoded image. The parent still paints and child resources load.
- Parent runtime-scroll fixture retains moved paint/DOMRect and single native
  navigation checks. Its original URL assertion observed request start before
  document commit and failed despite the target subsequently painting. The
  final positive passes (50 virtual polls). The assertion was moved to the
  already-observed destination paint phase, keeping
  the exact destination and one-request checks; the negative still detects
  synchronous scroll-listener reentry. This changes the test timing, not product
  navigation behavior.
- Native transport: 23 checks pass. Owner-only old behavior fails exactly four
  Cookie ancestor checks, including a scheme boundary.
- Five full-browser modes pass ASan/UBSan, including child removal and teardown.
  Darwin leak detection is unavailable and disabled explicitly.
- Independent H2 adapter gate (sibling): 30 checks pass; ignoring header-copy
  failure produces exactly nine failures. Buffered GET's pre-existing one retry
  is exercised, not hidden; both attempts fail before metadata/body delivery.
- Policy (root): 75 checks plus sanitizer; context ownership (sibling): 67 checks
  plus sanitizer and separate layout/CSS singleton negative controls.
- `test-passive-frame-guest` runs a real isolated QEMU no-paint negative before
  the same positive fixture. Parent and child use different HTTP ports/origins.
  Both fetch exactly parent HTML, child HTML, CSS and SVG. Negative observes no
  child text and zero marker pixels. Positive observes child text and exactly
  3072 green + 1024 pink SVG pixels inside the iframe, while retaining the parent.
  The final positive also observes the native script-preview notice.
  See `build-iframe-embed-fix/guest-final/{guest-old,guest-current}/`.
- Full-tree `test-mk-wired` was rerun once at the end: this task's fragments are
  wired, but concurrently added `tests/agent.mk`, `tests/pcnet.mk`, and
  `tests/virtio_scsi.mk` leave nine unrelated targets unreachable. Those shared
  files were not changed to manufacture a green result.

## Actual DS observation and its boundary

After the first successful local pixel fixture, one ordinary navigation to the
public DS sign-in page was performed in the separate fresh-profile test guest.
There was no SMS request, verification-code submission, QR scan, or interaction
with the previously verified standalone-open action. Screenshot:
`build-iframe-embed-fix/guest/positive/public.png` (before the native preview footer
was added).

The original DS parent remained visible. Its WeChat iframe painted real child
HTML text, including simultaneous “已允许” and “已拒绝” template labels. The
passive loader completed with `images=0`, and **no QR image appeared**. Those
simultaneous labels are not evidence that authentication was accepted or refused.

The child response source/complete URL was not retained by that snapshot guest;
its native log retains the provider host but not temporary request state. The
available evidence cannot determine whether QR creation depends on child script,
script-installed CSS, or another child rendering/resource path. It would be
incorrect to state a specific QR-script diagnosis from these pixels alone. No
site DOM was replaced, no QR was fabricated, no child script was executed in the
parent realm, and no cross-origin DOM boundary was relaxed.

## Final artifact identity

Hashes captured after the final native preview notice guest pair:

- `build-iframe-embed-fix/browser.aex`: SHA-256 `06565ba62636084e7d91e4485d33b9d7ac113f2220acf0b9a3bc0e8c7d5e7757`
- `build-iframe-embed-fix/guest/final-disk.img`: SHA-256 `4ba1d326a7be120d7ecba12065cf77787b7edb22621fd30504409eaf8dc8e831`

ISO and no-paint control disk hashes are also recorded in
`build-iframe-embed-fix/guest-final/artifacts.json`.
