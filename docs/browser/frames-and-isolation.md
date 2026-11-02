# Frames, documents and isolation

The browser embeds network documents through native document owners and creates local frame documents through auxiliary JavaScript contexts. A network frame owns its DOM, styles, images and layout; script execution adds a runtime and document services. Local documents use fresh JavaScript intrinsics within the creator’s runtime. Navigation, embedding policy, sandbox flags and window proxies govern how these documents interact. The main implementations are `c/apps/gui/browser/frame/passive.c`, `c/apps/gui/browser/js/runtime/platform.c` and `c/apps/gui/browser/js/dom/frame.c`.

## Frame paths

| Path | Document and execution state |
|---|---|
| Passive network document | Child DOM, styles, images and layout, with execution suppressed by policy |
| Scripted network document | Child document plus a page context owning JavaScript execution, timers and Web APIs |
| Local document | A document parsed through `DOMParser`, with eligible scripts hosted in an auxiliary JavaScript context |

The opening comment in `c/apps/gui/browser/frame/passive.c` describes the former passive preview implementation. Its adjacent corrections record the addition of scripted documents, native input, window messaging and sandbox flags. The filename now covers both passive and scripted network frames.

Local frames use `JS_NewContext` on the creator’s runtime. This gives them fresh language intrinsics while allowing JavaScript values and document objects to pass between contexts. Their timers schedule through the creator, and promise jobs share its runtime queue. These choices support access between documents with matching origins and reuse the creator’s scheduling and watchdog machinery. The implementation is in `c/apps/gui/browser/js/dom/frame.c`.

## Embedded navigation

The application supplies the committed parent URL and captured response policy through `passive_frames_set_parent`. Policy belongs to the tab’s document, including when the application restores that document. The event loop calls `passive_frames_update` to discover iframe sources, advance loads and pump child execution. These integration points are in `c/apps/gui/browser/app/main.c`.

Each network frame’s `struct pframe` records its host element and serial, source and final URLs, document resources, layout and execution state. Source changes replace the loaded content. Host removal retires the document, and a frame awaiting usable geometry begins its request when layout supplies a content viewport. Relative sources resolve against the parent’s policy-checked base URL. The implementation is in `c/apps/gui/browser/frame/passive.c`.

Network URL resolution uses the browser’s URL Standard parser and then checks the transport’s component capacities. `bfetch_start_embedded` receives the owner URL, ancestor URL and a redirect callback. The transport consults that callback before following each redirect. The frame loader checks the final URL, response policy, status, HTML media type and body budget before parsing. The transport implementation is in `c/apps/gui/browser/app/runtime.c`; its interface is in `c/apps/gui/browser/net/fetch.h`.

Local navigation is coordinated by the iframe prelude in `c/apps/gui/browser/js/runtime/platform.c`. It handles insertion, source assignment, `about:blank` and `srcdoc`, then commits a parsed document and adopts eligible scripts. HTTP and HTTPS sources pass to the native frame loader when it is available.

Local frame records live in a closure’s `WeakMap`. A generation counter associates asynchronous continuations with their originating navigation. At commit, the prelude releases the previous auxiliary context; removal also releases the context and cancels its timers. Each local location callback captures the document that received it and checks that document’s current ownership and connection before navigating.

The browser also offers a native action for opening an eligible embedded source as the top document in the current tab. It resolves the source against the committed page URL and supplies that URL as the navigation initiator. The eligibility rules and action are in `c/apps/gui/browser/frame/open.h` and `c/apps/gui/browser/frame/open.inc`.

## Document ownership

A scripted network child uses `struct js_page_context` to own its runtime, task queue, console, location, watchdog state and sandbox flags. The context holds DOM and focus owners, with Web API, platform and worker owners enabled before opening the document. Creation and selection are implemented in `c/apps/gui/browser/js/runtime/page.c`.

`js_page_context_activate` saves the current working state and selects the next document’s DOM, focus and service contexts. Selection occurs between JavaScript entries. Its guards reject a switch while JavaScript or a tracked native I/O scope is active, keeping native callbacks associated with the document whose code is running.

Layout has its own selection operation, `layout_context_activate`, declared in `c/apps/gui/browser/layout/block.h`. Child execution selects both page and layout owners. Painting selects the child display list and clips it to the intersection of the iframe viewport and parent clipping regions. The painting path is in `c/apps/gui/browser/paint/renderer.c`.

Teardown closes the child runtime before freeing its DOM and layout. JavaScript wrappers retain native node handles, so their cleanup must precede node destruction. `drop_content` implements this order in `c/apps/gui/browser/frame/passive.c`; the ownership contract is documented in `c/apps/gui/browser/js/runtime/page.h`.

## Embedding and resource policy

`iframe_policy_frame` combines the parent’s source policy with the child response’s embedding policy:

| Input | Applied check |
|---|---|
| Parent CSP | `frame-src`, falling back to `child-src`, then `default-src` |
| Child CSP | `frame-ancestors`, matched against the parent’s origin |
| Child `X-Frame-Options` | `DENY` or `SAMEORIGIN`, when an enforced `frame-ancestors` directive is absent |
| Parent and child schemes | Mixed content check for an HTTP child inside an HTTPS parent |

The implementation in `c/apps/gui/browser/frame/policy.c` intersects multiple policies and uses the first occurrence of a directive within each policy. Its URL comparisons operate on parsed components. Unsupported nonempty `X-Frame-Options` values cause refusal.

The child’s resource policy governs stylesheets, images, connections and scripts. The first base element with an `href` passes through `base-uri` before supplying the resolution base. Inline scripts, handler attributes and string code generation have their own checks in `c/apps/gui/browser/frame/policy.c`.

The network loader accumulates meta policies over the document’s lifetime. For a scripted child, native mutation notifications update restrictions synchronously, so a following script or connection operation in the same callback sees them. Accumulated restrictions persist after the meta element is removed. This tracking is in `c/apps/gui/browser/frame/passive.c`.

## Sandbox flags

`c/apps/gui/browser/frame/sandbox.h` defines a restriction mask: each set bit restricts an operation. Zero represents an unrestricted mask. A present empty sandbox attribute sets every restriction; recognized `allow-*` tokens clear corresponding bits. Parsing uses ASCII whitespace and ASCII case folding.

A network document’s active mask is the union of:

1. The embedding document’s flags.
2. The iframe attribute captured when navigation begins.
3. Sandbox restrictions from the child’s enforced response CSP.

The loader captures response sandbox flags before adding meta policies. The CSP parser takes the first sandbox directive in each policy and unions restrictions across policies. The comment in `c/apps/gui/browser/frame/sandbox.h` records this union as a stricter deviation from the specification algorithm described there.

Changing the iframe’s sandbox attribute supplies flags for a subsequent navigation. Local document creation uses the same native attribute parser and includes the embedding document’s flags. The consumers are in `c/apps/gui/browser/frame/passive.c` and `c/apps/gui/browser/js/runtime/platform.c`.

The parser’s token effects are:

| Token | Restrictions cleared |
|---|---|
| `allow-scripts` | Scripts and automatic features |
| `allow-same-origin` | Opaque origin |
| `allow-forms` | Form submission |
| `allow-modals` | Modal operations |
| `allow-popups` | Auxiliary navigation and custom protocols |
| `allow-popups-to-escape-sandbox` | Propagation to auxiliary contexts |
| `allow-top-navigation` | Both top navigation restrictions and custom protocols |
| `allow-top-navigation-by-user-activation` | Top navigation with activation and custom protocols |
| `allow-top-navigation-to-custom-protocols` | Custom protocols |
| `allow-pointer-lock` | Pointer lock |
| `allow-orientation-lock` | Orientation lock |
| `allow-presentation` | Presentation |
| `allow-downloads` | Downloads |
| `allow-storage-access-by-user-activation` | Storage access |

The navigation, plugins and document-domain restrictions remain set for every parsed sandbox directive.

For a network document, the scripts flag selects the passive path. The document still completes loading and dispatches `load` on its parent iframe. When execution is permitted, `js_page_context_set_sandbox` installs the mask before the runtime opens.

The origin flag gives a scripted network document an opaque origin. Its Web API fetches use an opaque initiator, serialized as `Origin: null`; cookie access and the `localStorage` and `sessionStorage` getters throw `SecurityError`. Window messages serialize its origin as the string `"null"`. The consumers are in `c/apps/gui/browser/js/runtime/webapi.c` and `c/apps/gui/browser/frame/window.inc`.

For local documents, the origin flag makes `contentDocument` return null and restricts the `contentWindow` facade to its cross-origin property allowlist. The prelude checks the current document’s origin state on accesses through retained proxies. The checks are in `c/apps/gui/browser/js/runtime/platform.c`.

## Cross-frame messaging

Scripted network frames communicate through proxies created in the receiving runtime. The parent obtains the child proxy through `contentWindow`; the child receives a parent proxy through `parent` and `top`. A stable native window record connects these proxies to the host element and current child document. The proxy code is in `c/apps/gui/browser/frame/window.inc`.

`postMessage` serializes data into a native queue. Delivery selects the receiving page context, reconstructs the data there and creates a `MessageEvent` with the recorded sender origin and a source proxy belonging to the receiver. Native port packets transfer `MessagePort` endpoints through `MessageEvent.ports`. The packet interface is declared in `c/apps/gui/browser/js/workers/ports.h` and implemented in `c/apps/gui/browser/js/workers/ports.c`.

An omitted target origin, or the single slash string, selects the sender’s origin. The wildcard string `"*"` accepts any recipient origin. Other target strings pass through URL origin parsing. Delivery compares the selected target with the destination document’s origin; delivery to an opaque child requires the wildcard.

Each queued message captures the window record’s document epoch. Child replacement and removal advance that epoch, allowing delivery to discard messages from an earlier document. Native removal notifications retire the child immediately; runtime teardown follows after JavaScript unwinds. `passive_frames_pending` includes queued messages in the browser’s wake decision. The mechanisms are in `c/apps/gui/browser/frame/window.inc` and `c/apps/gui/browser/frame/passive.c`.

## Limitations

- `TRANSPARENCY.md` describes the browser and isolation code as experimental and unsuitable for hostile content or credentials. These document owners and policy checks fall short of production browser sandboxing.
- Network embedding supports one level of child documents. Network proxies reject direct DOM access, including between matching origins, and navigation through their `location` properties. The child pump ignores navigation requests recorded by the child’s own location API. The code is in `c/apps/gui/browser/frame/passive.c` and `c/apps/gui/browser/frame/window.inc`.
- Local auxiliary documents stay outside the layout and paint paths. Their script host runs inline classic scripts; external scripts and nested frame navigation fall outside its support. Opaque local documents stay inert even with `allow-scripts`, because their contexts share the creator’s runtime. The local `contentWindow` facade’s `postMessage` returns without delivering a message. The relevant code is in `c/apps/gui/browser/js/dom/frame.c` and `c/apps/gui/browser/js/runtime/platform.c`.
- Network script loading runs scripts without modules, full parser blocking or asynchronous scheduling. Form editing and submission run without native owners. A `require-trusted-types-for` policy keeps initial execution inert; iframe `csp` and `credentialless` attributes cause network frame refusal. The relevant code is in `c/apps/gui/browser/frame/passive.c` and `c/apps/gui/browser/frame/focus.inc`.
- Sandbox parsing includes flags for features the engine has yet to implement. Top document response flags propagate to frames, while the top document’s runtime retains its default mask. The native open action excludes sandbox and `srcdoc` attributes because it lacks sandbox propagation to the resulting top document. The relevant code is in `c/apps/gui/browser/frame/sandbox.h`, `c/apps/gui/browser/js/runtime/page.c` and `c/apps/gui/browser/frame/open.h`.
- Network messaging supports serialized data and port transfers through the event’s ports array. Ports nested in message data and `ArrayBuffer` transfer fall outside that support. Clone failures raise `DataCloneError`; message byte or queue exhaustion raises `QuotaExceededError`. The code is in `c/apps/gui/browser/js/workers/ports.h` and `c/apps/gui/browser/frame/window.inc`.

The implementation also bounds resource use:

| Resource | Bound |
|---|---|
| Concurrent network frames | 4 |
| Network frame HTML body | 256 KiB |
| Stylesheet entries per network frame | 16 |
| Image attempts per network frame | 32 |
| Response body bytes accounted by each native frame loader | 8 MiB |
| Embedded transport response body | 4 MiB |
| Auxiliary contexts | 8 per creator context, 64 slots across the process |
| Network window identities per top document | 32 |
| Queued window messages | 64 |
| Serialized data per window message | 64 KiB |

These bounds come from `c/apps/gui/browser/frame/passive.c`, `c/apps/gui/browser/net/fetch.h`, `c/apps/gui/browser/js/dom/frame.c` and `c/apps/gui/browser/frame/window.inc`. The frame loader’s byte accounting covers its resource loads; total document memory and requests made through Web APIs sit outside that accounting.

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/frame/passive.c` | Network frame loading, resources, scripts, policies and lifecycle |
| `c/apps/gui/browser/frame/policy.c` | Embedding, resource and script policy checks |
| `c/apps/gui/browser/frame/sandbox.h` | Sandbox flags and shared attribute and CSP parsers |
| `c/apps/gui/browser/frame/window.inc` | Network window proxies, message queue and delivery |
| `c/apps/gui/browser/frame/focus.inc` | Child focus handling and form ownership limitations |
| `c/apps/gui/browser/frame/open.h` | Native open action eligibility |
| `c/apps/gui/browser/frame/open.inc` | Navigation from an embedded source to the current tab |
| `c/apps/gui/browser/js/dom/frame.c` | Auxiliary contexts, local windows, scripts and timers |
| `c/apps/gui/browser/js/runtime/platform.c` | Iframe navigation, local records and access checks |
| `c/apps/gui/browser/js/runtime/page.c` | Page owners, runtime selection and sandbox consumers |
| `c/apps/gui/browser/js/runtime/page.h` | Runtime ownership and teardown contracts |
| `c/apps/gui/browser/js/runtime/webapi.c` | Opaque origin effects on fetch, cookies and storage |
| `c/apps/gui/browser/js/workers/ports.h` | Native packet and port transfer interface |
| `c/apps/gui/browser/js/workers/ports.c` | Serialization and port ownership transfer |
| `c/apps/gui/browser/net/fetch.h` | Embedded transport interface and response ceiling |
| `c/apps/gui/browser/app/runtime.c` | Embedded requests and redirect handling |
| `c/apps/gui/browser/app/main.c` | Parent policy setup and frame event-loop integration |
| `c/apps/gui/browser/layout/block.h` | Layout context selection interface |
| `c/apps/gui/browser/paint/renderer.c` | Embedded display-list painting and clipping |
| `TRANSPARENCY.md` | Project statement rules and security limitations |

## See also

- [Browser architecture](overview.md)
- [Loading pages: fetch, URLs and the HTTP cache](loading-and-fetch.md)
- [The JavaScript runtime](javascript-runtime.md)
- [DOM and event bindings](dom-bindings.md)
- [Browser security model](security-model.md)
- [Testing the browser](testing.md)
