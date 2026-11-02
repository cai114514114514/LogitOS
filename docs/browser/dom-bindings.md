# DOM and event bindings

The browser exposes its native document tree through QuickJS objects, C accessors and JavaScript preludes. Node wrappers connect script identity to native storage; interface prototypes provide DOM members; feature modules add events, observers, ranges, selection, forms and frame windows. `c/apps/gui/browser/js/runtime/page.c` orders installation and teardown, while `c/apps/gui/browser/js/dom/dom.c` owns the live document’s core binding state.

## Native wrappers and interface prototypes

A live node wrapper carries an `elem_handle` containing a native `struct node` pointer and its allocation serial. `node_of()` validates both before returning the node. The serial distinguishes successive occupants of a recycled node slot, protecting retained script objects when native tree operations destroy nodes. The handle implementation is in `c/apps/gui/browser/js/dom/dom.c`; the native fields are defined in `c/apps/gui/browser/dom/tree.h`.

`wrap()` retrieves a cached wrapper through the node’s weak `jsw` slot or creates an object with the appropriate prototype. Traversal, queries and native callbacks use this wrapping path. The page’s `document` wraps the native document root, so that an element’s `ownerDocument` refers to the page document.

The QuickJS class supplies the native handle, finalizer and garbage collection hooks. The prototype supplies the script interface. `iface_proto_for()` chooses that prototype from the node kind, namespace and element name. `c/apps/gui/browser/js/dom/dom_iface.inc` defines the hierarchy:

| Interface | Parent interface |
|---|---|
| `Node` | `EventTarget` |
| `Document`, `DocumentFragment`, `DocumentType`, `CharacterData`, `Element` | `Node` |
| `Text`, `Comment`, `ProcessingInstruction` | `CharacterData` |
| `CDATASection` | `Text` |
| `HTMLElement`, `SVGElement`, `MathMLElement` | `Element` |
| `HTMLInputElement`, `HTMLBodyElement`, `HTMLDivElement` | `HTMLElement` |

This hierarchy makes inherited members, prototype edits and `instanceof` checks available through the same objects. Constructors handle native allocation as well: `Text`, `Comment` and `DocumentFragment` create nodes. During a custom element upgrade, the HTML constructor path returns the element supplied by the upgrade machinery.

`js_dom_init()` installs the interface hierarchy before wrapping the document. That order matters because wrapper caching preserves the prototype chosen when an object is created.

## Attribute reflection and Web IDL shapes

A reflected property translates between a JavaScript value and a content attribute. `c/apps/gui/browser/js/dom/reflect.c` implements string, boolean, numeric, enumerated and URL conversions. `c/apps/gui/browser/js/dom/reflect.inc` contains the generated element and property table; `tools/gen_reflect.py` reads reflection data from the Web Platform Tests corpus to produce it.

| Property | Implemented behavior |
|---|---|
| `title` | Converts an assigned value to a string and stores the attribute |
| `disabled` | Reads attribute presence and adds or removes the attribute on assignment |
| `input.size` | Applies numeric conversion, uses a default of 20 and raises `IndexSizeError` for zero |
| `a.href` | Resolves the stored attribute against the document base URL when read |

Reflected writes go through `js_dom_attr_write()` and `js_dom_attr_erase()` in `c/apps/gui/browser/js/dom/dom.c`, which connect attribute storage to style invalidation and to bookkeeping of named properties. Attribute reads and writes carry byte lengths, preserving embedded NUL characters.

After feature installation, two passes finish the exposed object shapes:

- `c/apps/gui/browser/js/runtime/idl_shape.c` evaluates `c/apps/gui/browser/js/runtime/idl_shape_prelude.inc`, which moves selected singleton members onto interface prototypes, adds receiver checks and assigns interface tags. Document members dispatch according to their receiver, so another document receives its own answer.
- `c/apps/gui/browser/js/runtime/native_shape.c` adjusts platform function names and constructibility and marks functions for native stringification. Its descriptor walk reads data and accessor descriptors as plain values.

`c/apps/gui/browser/js/runtime/page.c` runs the placement pass first, allowing the function pass to inspect the functions that placement creates. It passes private roots between the two passes for objects retained inside getter closures.

A compatibility bridge in `c/apps/gui/browser/js/dom/dom_iface.inc` supports installers that discover an installation prototype by creating a `div`. It moves their added descriptors to `Element.prototype` and reconnects the node hierarchy after the event supplement publishes its constructible `EventTarget`. Installers for a particular interface, including `c/apps/gui/browser/js/dom/semantics.c`, can address the published prototypes directly.

## Mutations and collections

The core bindings translate script mutations into native tree changes and rendering invalidations. Attribute changes record style scopes; child and text changes request layout work. The invalidation record coalesces overlapping subtrees and tracks following siblings where selector matching may depend on the changed element. Attaching a subtree records the work needed to render mutations made while it was detached. These mechanisms are in `c/apps/gui/browser/js/dom/dom.c`.

Insertion discovers window names and offers newly connected script elements to the embedder. The script sink queues work that `c/apps/gui/browser/app/main.c` executes after the inserting script yields.

The live document’s `childNodes` and `children` are native collection objects. Their indexed reads and `length` use a cache checked against the parent’s `child_gen`. After a mutation of the child list, the next successful rebuild materializes the current children; subsequent reads reuse the cache while the generation matches. Repeated collection accesses share one sibling traversal.

Other producers implement their own collection behavior. `c/apps/gui/browser/js/dom/select.c` supplies selector queries and query collections. `c/apps/gui/browser/js/dom/semantics.c` supplies live collections for table rows, cells, options and form elements, using proxy reads to obtain current members.

## Event registration and dispatch

The native listener store in `c/apps/gui/browser/js/dom/dom.c` uses a hash table keyed by node pointer, with a serial check on each node’s entry. Listener lists retain callback references and registration order.

Registration identity consists of event type, callback and capture flag. Each record holds `once`, `passive` and the state of handler properties. Assigning an `on*` property replaces its existing callback at the position established by the first assignment.

Dispatch follows three steps:

1. Capture the propagation path, including node serials, adjusted targets and shadow root information.
2. Invoke capture listeners along the path, capture and other listeners at the target, and the applicable listeners without capture on ancestors.
3. Release the captured path, clear the current target and dispatch state, and return the default action decision.

Capturing the path before callbacks keeps routing stable when a listener reparents nodes. Traversal across shadow roots uses `composed` to determine whether the path crosses to the host. Captured retargeting and information about closed roots feed `composedPath()`.

Each native listener invocation snapshots the current list. Removal marks a record so the snapshot can skip it; reference counts keep records alive during changes made from callbacks. A `once` listener is removed before its callback runs. Native `preventDefault()` records cancellation when the event is cancelable and the current listener permits cancellation.

The JavaScript supplement in `c/apps/gui/browser/js/dom/events.c` adds event families including `CustomEvent`, `FocusEvent` and `InputEvent`, legacy initialization methods, listener objects with `handleEvent`, removal through an abort signal and passive defaults. Constructed event subclasses retain the native event payload while receiving their interface prototype and extension fields. A standalone `EventTarget` stores its listeners in a JavaScript `WeakMap` and dispatches at the target.

Inline handler attributes are compiled on first invocation after the document policy callback approves compilation. Native callback exceptions are reported, and `js_dom_run_jobs()` processes queued QuickJS jobs after callbacks. The same checkpoint flushes native mutation records.

## Native event delivery and default actions

`js_dom_dispatch()` constructs a trusted event from the embedder’s event type and input fields, then returns whether the default action may proceed. Input callers in `c/apps/gui/browser/app/main.c` use that result for actions such as link activation and keyboard editing. Selected mouse and keyboard event types record transient user activation.

Script delivery through the native `dispatchEvent()` path clears the trust flag. `js_dom_deliver_event()` supports native delivery of an already constructed event, preserving payloads such as a submission’s submitter while marking the event trusted. Form bindings use this entry point for submission and form data events.

Native delivery brackets execution with the page’s execution slice and processes jobs afterward. That placement keeps callbacks and their queued work inside the page runtime’s scheduling machinery.

## Observers

`MutationObserver` combines JavaScript record producers with native mutation subscriptions. Registration, option filtering and observer queues live in `c/apps/gui/browser/js/runtime/platform.c`.

The JavaScript producers wrap child mutation and attribute methods. They collect added and removed nodes, sibling positions and previous attribute values, then apply registration options including `subtree`, `attributeFilter` and requests for old values.

`c/apps/gui/browser/js/dom/native_mo.inc` captures native changes to character data and the child insertion associated with text splitting. It copies the previous text bytes and records ancestry at mutation time. That ancestry lets delivery identify the subtree containing the edit even after the target moves.

Both producer paths share a queued QuickJS notification job. Native mutation callbacks capture records; the notification job transfers native records to the JavaScript queues and calls observer callbacks. `observe()`, `disconnect()` and `takeRecords()` flush pending native records before changing registrations or returning queued records.

`IntersectionObserver` and `ResizeObserver` are JavaScript implementations in `c/apps/gui/browser/js/runtime/platform.c`. Both measure through `getBoundingClientRect()` and deliver queued entries through promise jobs. Intersection observation computes viewport overlap using margins and thresholds, with scroll and resize events scheduling measurements. Resize observation compares the measured width and height with the preceding sample.

## Ranges and selection

A `Range` combines a JavaScript interface with native live boundary state. `c/apps/gui/browser/js/dom/live_range_native.inc` allocates a `dom_live_range` inside a QuickJS object. Its finalizer disposes the document subscription and frees the state. A `WeakMap` in `c/apps/gui/browser/js/dom/live_range_shim.inc` associates each script range with that object.

The mutation subscriber in `c/apps/gui/browser/dom/mutation.inc` adjusts boundaries as the tree changes. Insertions shift child offsets, removals move boundaries out of removed subtrees, and text replacements shift or clamp text offsets. Splitting attached text can transfer a boundary to the new text node.

Text boundaries use UTF-16 code units. `c/apps/gui/browser/dom/text_mutation.inc` implements replacement and splitting over the stored text, including surrogate handling, and emits the mutation records used by ranges. `c/apps/gui/browser/js/dom/characterdata.c` routes script editing methods through those operations.

The range shim implements boundary comparison, cloning, insertion, extraction and deletion. Content operations capture a traversal plan before editing so live boundary updates and changing child collections can coexist with a stable description of the selected content. `createContextualFragment()` uses the HTML fragment parser through a context element’s `innerHTML`.

Selection connects script to the native anchor and caret used by content editing in `c/apps/gui/browser/js/dom/forms.c`. Paths of child indexes identify nodes relative to the document element. The shim converts text offsets between JavaScript UTF-16 units and native byte positions. `document.getSelection()` and `window.getSelection()` return the same selection object; its getters read native state and its methods update that state.

## Forms and focus

Form bindings combine reflected attributes with native control state. Bindings for value and checked state in `c/apps/gui/browser/js/dom/forms.c` pass node wrappers to `c/apps/gui/browser/dom/forms.c`, whose control records use node pointers and serials. This allows values assigned to detached controls to survive insertion.

Focus requests refresh computed style before checking native focusability. The `activeElement` getter reads the native focus holder and falls back to the body or document element. A comment in `c/apps/gui/browser/js/dom/forms.c` describing a JavaScript cache maintained by events sits beside a correction; the current getter uses native focus state.

Submission and reset share native paths with control activation:

| Entry point | Behavior |
|---|---|
| `requestSubmit()` | Checks the supplied submitter, performs validation subject to the form’s validation attributes, and dispatches the cancelable submission event |
| `submit()` | Calls the submission backend with validation and submit-event dispatch bypassed |
| `reset()` | Dispatches a cancelable reset event before restoring control state |

The common submission path in `c/apps/gui/browser/dom/forms.c` checks node serials and connectivity after script callbacks. This accounts for handlers that remove or destroy the form or submitter.

`c/apps/gui/browser/js/dom/validation.c` exposes validity state and constructs submission events. `c/apps/gui/browser/js/dom/entries.c` connects native form entries to `FormData` and to delivery of native form data events, then reads the resulting entries for submission encoding.

## Frame bindings and document ownership

Auxiliary frame contexts in `c/apps/gui/browser/js/dom/frame.c` use documents supplied by the DOMParser bindings. Each receives a fresh `JSContext` on its creator’s `JSRuntime`, giving it fresh JavaScript intrinsics and a window associated with its document. The window delegates timers through the creator and tracks them for cancellation during frame release.

The iframe properties and navigation hooks in `c/apps/gui/browser/js/runtime/platform.c` connect these contexts to `contentWindow` and `contentDocument`. Wrappers for parsed documents are implemented in `c/apps/gui/browser/js/dom/domparser.c`.

Network frames use the machinery in `c/apps/gui/browser/frame/passive.c`. Its opening correction records the progression from passive rendering to documents that own page runtimes and execute classic scripts. Native entries select the child’s page and layout state, execute its work, then restore the caller’s state.

`c/apps/gui/browser/frame/window.inc` supplies window proxies and message transport between those runtimes. Messages carry serialized data or prepared port packets. Delivery checks runtime ownership, frame connectivity, navigation epochs and destination origins before entering the recipient.

The core DOM state travels through `js_dom_context`, declared in `c/apps/gui/browser/js/dom/dom.h` and implemented in `c/apps/gui/browser/js/dom/dom.c`. Each context holds its document roots, prototypes, listeners, wrapper tracking and invalidation state. `js_dom_context_activate()` switches ownership between script entries. Native wrapper notifications temporarily select their owning context while updating binding state.

## Wrapper lifetime and teardown

The weak wrapper slot provides lookup, while a graph of strong references supplies reachability. A wrapped child retains its parent wrapper, and the parent retains its wrapped children. `elem_gc_mark()` exposes these edges to QuickJS’s cycle collector. The document root connects wrapper components; the collector can reclaim unreachable detached parent and child cycles, including cycles through script properties. These mechanisms are in `c/apps/gui/browser/js/dom/dom.c`.

Wrapping a node creates the ancestor wrappers needed for this graph. Native insertion and removal notifications update its edges. Destruction invalidates handles before node slots are reused, and document close invalidates tracked wrappers before releasing the arena. Finalizers update counters belonging to the wrapper’s owning DOM context.

DOMParser wrappers use arena ownership tracked by reference counts. Each `dp_handle` retains a `dp_arena`; releasing the final reference frees the parsed document. Their weak node cache reuses surviving wrappers. This implementation is in `c/apps/gui/browser/js/dom/domparser.c`.

`js_page_close()` in `c/apps/gui/browser/js/runtime/page.c` invalidates pending work and releases feature references, including form bindings, before calling `js_dom_cleanup()`. DOM cleanup releases listeners, wrapper edges, cached prototypes and the document reference, and clears native wrapper slots. The page then frees its QuickJS context and runtime. A close request made during script entry first requests cancellation so teardown can proceed after execution unwinds.

## Limitations

- Window and document listeners share the native target at the document root, so window listeners observe the document as `currentTarget`. A compiled inline handler retains its callback when the attribute text later changes. See `c/apps/gui/browser/js/dom/dom.c`.
- Mutation observer child and attribute records depend on the wrapped JavaScript producers. The native text queue holds up to 4,096 records and drops records whose target serial has changed. See `c/apps/gui/browser/js/runtime/platform.c` and `c/apps/gui/browser/js/dom/native_mo.inc`.
- Intersection observation measures viewport geometry even with an element root. Intersection and resize observation use bounded timer rechecks; resize entries report the same measured box for the variants of their box size. See `c/apps/gui/browser/js/runtime/platform.c`.
- Selection represents one native selection, and `getRangeAt()` creates a range from its boundaries. Associated range identity, range geometry, `surroundContents()` and `Selection.modify()` remain unimplemented. Native range boundaries reject another document, and range insertion into a document raises `NotSupportedError`. See `c/apps/gui/browser/js/dom/forms.c`, `c/apps/gui/browser/js/dom/live_range_native.inc` and `c/apps/gui/browser/js/dom/live_range_shim.inc`.
- DOMParser nodes use their own wrapper class. Adoption into the live page copies the native subtree and detaches the source, returning a new wrapper identity. See `c/apps/gui/browser/js/dom/domparser.c`.
- Auxiliary frame contexts share their creator’s execution budget. Network frames lack direct DOM access between parent and child, including for the same origin; nested active frames and the full order of scripts that block the parser remain unimplemented. See `c/apps/gui/browser/js/dom/frame.c`, `c/apps/gui/browser/frame/passive.c` and `c/apps/gui/browser/frame/window.inc`.

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/dom/tree.h` | Native nodes, serials, wrapper slots and mutation interfaces |
| `c/apps/gui/browser/dom/mutation.inc` | Document subscriptions and live range adjustment |
| `c/apps/gui/browser/dom/text_mutation.inc` | Text replacement, splitting and UTF-16 offsets |
| `c/apps/gui/browser/dom/forms.c` | Native control state and submission |
| `c/apps/gui/browser/js/dom/dom.c` | Live wrappers, collections, invalidation, events, ownership contexts and cleanup |
| `c/apps/gui/browser/js/dom/dom.h` | Binding lifecycle and embedder interfaces |
| `c/apps/gui/browser/js/dom/dom_iface.inc` | Interface hierarchy, constructors and compatibility bridge |
| `c/apps/gui/browser/js/dom/reflect.c` | Typed attribute reflection |
| `c/apps/gui/browser/js/dom/reflect.inc` | Generated reflection table |
| `tools/gen_reflect.py` | Reflection table generator |
| `c/apps/gui/browser/js/dom/select.c` | Selector bindings and query collections |
| `c/apps/gui/browser/js/dom/semantics.c` | HTML interface behavior and live collections |
| `c/apps/gui/browser/js/dom/events.c` | Event constructors, listener normalization and standalone targets |
| `c/apps/gui/browser/js/dom/native_mo.inc` | Native text mutation capture and notification scheduling |
| `c/apps/gui/browser/js/dom/characterdata.c` | Character-data editing bindings |
| `c/apps/gui/browser/js/dom/live_range_native.inc` | Native range state and finalization |
| `c/apps/gui/browser/js/dom/live_range_shim.inc` | Range interface and content operations |
| `c/apps/gui/browser/js/dom/forms.c` | Control, focus and selection bindings |
| `c/apps/gui/browser/js/dom/validation.c` | Validity interfaces and submission events |
| `c/apps/gui/browser/js/dom/entries.c` | Form entries and form-data integration |
| `c/apps/gui/browser/js/dom/domparser.c` | Parsed-document wrappers and arena ownership |
| `c/apps/gui/browser/js/dom/frame.c` | Auxiliary frame contexts, windows and timers |
| `c/apps/gui/browser/frame/passive.c` | Network frame documents and runtime selection |
| `c/apps/gui/browser/frame/window.inc` | Window proxies and message transport |
| `c/apps/gui/browser/js/runtime/platform.c` | Observers, iframe properties and platform installation |
| `c/apps/gui/browser/js/runtime/idl_shape.c` | IDL placement-pass entry points |
| `c/apps/gui/browser/js/runtime/idl_shape_prelude.inc` | Member placement, receiver checks and interface tags |
| `c/apps/gui/browser/js/runtime/native_shape.c` | Platform function shapes |
| `c/apps/gui/browser/js/runtime/page.c` | Installation order, execution scheduling and teardown |
| `c/apps/gui/browser/app/main.c` | Native input delivery, default actions and inserted-script execution |

## See also

- [The document tree](dom.md)
- [The JavaScript runtime](javascript-runtime.md)
- [Frames, documents and isolation](frames-and-isolation.md)
- [Browser security model](security-model.md)
- [Testing the browser](testing.md)
