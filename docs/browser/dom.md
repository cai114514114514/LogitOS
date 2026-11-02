# The document tree

The browser represents parsed HTML and content created by scripts as a native C tree shared by parsing, CSS, layout, editing, and JavaScript bindings. Document arenas provide node storage, strings, and indexes. Mutation notifications connect the tree to live ranges and JavaScript wrapper ownership, while forms, focus, and the top layer provide interactive state. The core representation lives in `c/apps/gui/browser/dom/tree.h` and `c/apps/gui/browser/dom/tree.c`.

## Native representation

Every native node is a `struct node`. Its type enumeration maps to JavaScript `nodeType` through `node_type_of()` in `c/apps/gui/browser/js/dom/dom.c`:

| Native type | Native value | Contents | JavaScript `nodeType` |
|---|---:|---|---:|
| `N_ELEM` | 0 | Element name, namespace, attributes, and children | 1 |
| `N_TEXT` | 1 | Text bytes and length | 3 |
| `N_COMMENT` | 2 | Comment bytes and length | 8 |
| `N_DOCTYPE` | 3 | Name and public/system identifiers | 10 |
| `N_DOCUMENT` | 4 | Document root | 9 |

The bindings also represent document fragments, processing instructions, and scratch documents as synthetic elements with sentinel names. Their JavaScript node types are 11, 7, and 9 respectively. These cases are handled in `c/apps/gui/browser/js/dom/dom.c` and `c/apps/gui/browser/js/dom/dom_iface.inc`.

Nodes carry parent, first child, last child, and sibling links, an owning `struct dom_doc`, a generation stamp named `serial`, and a `child_gen` counter for changes to their immediate child list.

Removal updates links in constant time through the link to the previous sibling. When subscribers are present, `c/apps/gui/browser/dom/mutation.inc` also walks previous siblings to compute the mutation’s child index.

Elements use `NS_HTML`, `NS_SVG`, or `NS_MATHML`. Tag names, attribute names, ID values, and class tokens are interned through libwapcaplet. Attribute values and text receive byte storage owned by the document. Interning lets consumers compare common names by pointer, while values retain their lengths.

Element construction derives `tag_id` for browser consumers and `htag` for HTML parsing and serialization. An older comment in `c/apps/gui/browser/dom/tree.h` says elements created through the DOM leave `htag` at zero; `elem_new()` in `c/apps/gui/browser/dom/tree.c` now initializes it from the element name.

`dom_parse()` calls the HTML tree builder in `c/apps/gui/browser/html/tree.c`. `dom_doc_element()` finds the document’s HTML element, and `dom_doc_body()` finds its first HTML body or frameset child. These accessors walk the current tree, making their answers follow subsequent mutations.

## Storage and lifetime

A `struct dom_doc` owns dense slabs of node slots and a bump arena for strings and arrays. Both use a default chunk size of 256 KiB; an oversized byte allocation receives a chunk sized for it. The allocator rationale in `c/apps/gui/browser/dom/tree.h` explains why allocating document data in chunks avoids repeated searches through the application allocator’s block list.

The document also keeps a ledger of interned strings, with one retained reference per distinct string it uses. Tree operations distinguish detachment, recycling, and document destruction:

| Operation | Effect |
|---|---|
| `dom_remove_child()` | Detaches the child while preserving its allocated subtree. |
| `dom_destroy_subtree()` | Detaches the subtree and returns its node slots to the document’s free list. |
| `dom_destroy_children()` | Recycles the children while retaining their parent. |
| `dom_free()` on the document root | Releases the document, its chunks, indexes, and references to interned strings. |
| `dom_free()` on another node | Destroys that subtree. |

Recycling clears a slot’s `serial`; allocation assigns the next document generation. A retained pointer paired with its serial can detect recycling while the document storage is alive.

Node recycling releases external style storage and computed styles through the registered cleanup callback. Arena bytes are reclaimed with the document. Full destruction scans every allocated node slot, including detached nodes, so their external style allocations receive cleanup too. These operations are implemented in `c/apps/gui/browser/dom/tree.c`.

## Mutation and live views

`dom_append_child()` and `dom_insert_before()` implement moves by unlinking an existing child before attaching it at its destination. They advance the affected parents’ `child_gen` counters.

JavaScript insertion uses `insert_run()` in `c/apps/gui/browser/js/dom/dom.c`. Its checks cover container type, document ownership, ancestry, and destination parent depth. Inserting a document fragment moves its children into the destination. The binding records layout invalidation for the child lists it changed and offers newly connected scripts to the application’s script queue.

Attribute setters and removal functions in `c/apps/gui/browser/dom/tree.c` update the stored attributes, ID index, and cached class tokens together. HTML attribute queries fold ASCII case; SVG and MathML queries preserve case. `struct dom_attr` carries a byte length, allowing attribute values to contain embedded NUL bytes.

The native mutation interface gives subscribers access to the tree at defined points:

| Notification | Timing |
|---|---|
| `DOM_MUT_REMOVE` | Before unlinking, with the old ancestry available |
| `DOM_MUT_INSERT` | After linking |
| `DOM_MUT_TEXT`, `DOM_MUT_ATTRIBUTE` | After committing the new data |
| `DOM_MUT_DESTROY` | Before recycling a node |
| `DOM_MUT_CLOSE` | Before releasing document storage |
| `DOM_MUT_SPLIT` | During text splitting, before truncating the original text |

Subscribers run synchronously under the callback contract in `c/apps/gui/browser/dom/tree.h`, which permits internal bookkeeping while delivery traverses the subscription list.

`struct dom_live_range` uses these notifications to maintain boundary positions. Removing a subtree moves boundaries inside it to the former parent position. Text replacement adjusts offsets; splitting attached text can transfer boundaries to the new text node. `c/apps/gui/browser/dom/text_mutation.inc` converts between UTF-8 storage and UTF-16 code unit offsets, preserving unmatched surrogates during replacement and splitting.

JavaScript `childNodes` and `children` views cache arrays of native child pointers against `child_gen`. Reads reuse the cache across child list mutations and rebuild it after a generation change. The implementation is in `c/apps/gui/browser/js/dom/dom.c`.

Setting `innerHTML` parses a fragment using the receiving element’s HTML context, imports its nodes into the destination arena, and frees the temporary document. Imported scripts are marked as already started before insertion. A materialized template content fragment serves as the replacement target for a template. These paths also live in `c/apps/gui/browser/js/dom/dom.c`.

## IDs and lookup

The ID index stores candidate nodes in hash buckets keyed by interned ID values. Attribute changes remove the old index entry and add the new one.

`dom_get_element_by_id()` searches from the native document root. `dom_get_element_by_id_in()` searches descendants of a supplied root, excluding the root itself. Both use exact case matching and filter candidates by tree scope.

For a unique candidate, the index supplies the result. Duplicate IDs trigger an iterative preorder walk to choose the first match in current tree order. This matters when scripts reorder nodes: an earlier lookup that took the first bucket entry could return a different element from the first one in the tree. The correction and its rationale are recorded in `c/apps/gui/browser/dom/tree.c`.

Selector queries and element collections live in `c/apps/gui/browser/js/dom/select.c`.

## Shadow trees

A shadow host reaches its root through `node->shadow`. The root is an `N_ELEM` with the sentinel tag `DOM_SHADOW_TAG`. Its parent points to the host for connectivity checks; the host’s child list contains its light children.

`dom_attach_shadow()` records the mode and flags. A call with the same mode can reuse an existing declarative root by clearing its children and replacing its flags. The HTML tree builder attaches recognized declarative shadow templates to eligible hosts. These paths are in `c/apps/gui/browser/dom/tree.c` and `c/apps/gui/browser/html/tree.c`.

Document ID lookup stops at shadow boundaries. Lookup rooted at a shadow root searches that root’s descendants. Subtree recycling visits attached shadow roots, and deep import copies roots carrying `SHADOW_CLONABLE`.

## Forms and editing

`fc_kind()` in `c/apps/gui/browser/dom/forms.h` classifies controls from their HTML tag and input type. The header also supplies geometry helpers that layout can use without linking the implementation of control state.

Interactive state lives in a hash table in `c/apps/gui/browser/dom/forms.c`. Entries are allocated as needed and checked against the node’s pointer and serial. They hold current values, dirty flags, checkedness, selections, option state, the value recorded on focus, and custom validation messages.

For textual input controls, a clean current value follows the value attribute. Editing or assigning the current value sets the dirty flag. Textareas derive their default from descendant text. Buttons, toggles, and hidden inputs use value reflection through the content attribute; select values derive from option selectedness. Reset clears the relevant dirty flags and restores defaults. Custom validation messages survive reset.

Value sanitization normalizes textarea line endings to LF, removes line breaks from the values of textual controls that accept one line, and trims surrounding ASCII whitespace for email and URL values. Number editing retains incomplete text for display; `fc_value()` exposes an empty API value for it, and validation can report bad input.

`fc_form_of()` resolves ownership within the control’s tree. For a connected control, a present form attribute selects the first matching ID and requires that element to be an HTML form. Detached controls use the nearest ancestor form. `fc_disabled()` implements disabled fieldsets with an exception for descendants of each fieldset’s first legend. Validation, reset, radio grouping, and entry collection use these helpers.

Text editing dispatches cancelable `beforeinput`, changes the value, and dispatches `input`. `fc_commit()` compares the current value with the value recorded on focus and dispatches `change` when they differ.

Editing a `contenteditable` host changes DOM text and elements directly. Its caret has anchor and focus positions consisting of a node and an offset. Native editing uses byte offsets in text nodes and child indexes in containers; the JavaScript selection bindings convert text positions to UTF-16 offsets. The implementation spans `c/apps/gui/browser/dom/forms.c` and `c/apps/gui/browser/js/dom/forms.c`.

## Validation, entries, and submission

Constraint validation lives in `c/apps/gui/browser/dom/validation.c`. `fc_validity()` reads current state to compute the flags for a required value, type, pattern, length, numeric range, step, bad input, and custom errors. Length checks account for whether the value came from user editing. `fc_will_validate()` determines participation in checking and reporting.

Form validation snapshots invalid controls and their serials before dispatching cancelable `invalid` events. This preserves the collected validation result when a handler edits another control. Reporting chooses the first surviving control whose invalid event was unhandled. The application’s reporter displays a status message and focuses that control.

URL and pattern validation use callbacks installed by `c/apps/gui/browser/js/dom/validation.c`. The URL callback uses the browser URL parser; the pattern callback uses the realm’s RegExp engine.

`fc_entries()` in `c/apps/gui/browser/dom/entries.c` builds a snapshot containing copied names and values. It walks controls in tree order and checks form ownership, including controls outside the form element. Collection handles disabled controls, checked toggles, the activated submitter, selected options, and direction entries.

`fc_entries_encode()` converts entry line endings to CRLF and produces `application/x-www-form-urlencoded` data. JavaScript FormData construction and navigation share the native collector. The bridge in `c/apps/gui/browser/js/dom/entries.c` and the FormData implementation in `c/apps/gui/browser/js/net/formdata.c` preserve changes made by `formdata` listeners.

Submission converges on `fc_submit()`:

| Entry path | Validation and events |
|---|---|
| `form.submit()` | Calls the navigation handler directly |
| `form.requestSubmit()` | Validates and dispatches a cancelable submit event |
| Activation of a submit button and implicit submission | Use the same validation and submit event path |

The eventful path honors the form and submitter validation attributes, guards against recursive submission, and rechecks node identities and connectivity after callbacks. The bindings also check a supplied submitter’s type and form owner.

The application installs `form_submit_ex()` in `c/apps/gui/browser/app/main.c`. It constructs entries, applies submitter action and method overrides, replaces the action’s query for GET submission, and queues navigation. The application consumes that request after callbacks return, preserving the submitting runtime through its current script entry.

## Focus and the top layer

A `struct focus_context` stores the focused node, its serial, and dispatch callbacks. `focus_current()` checks identity, connectivity, and inertness. The `document.activeElement` binding reads this native holder and falls back to the body or document element.

`focus_set()` records the new holder before dispatching blur, focusout, focus, and focusin. Serial and holder checks after callbacks allow handlers to move focus during the transition. Focusability covers controls, links, editable elements, and elements with a valid tabindex. Negative tabindex values permit programmatic focus; Tab traversal visits positive values in ascending order, followed by zero in document order. See `c/apps/gui/browser/dom/focus.c`.

The top layer supplies native ordering for modal dialogs and popovers. `c/apps/gui/browser/dom/top_layer.inc`, included by the focus implementation, stores node serials, restoration targets, popover sources, and entry kinds. Consumers prune entries after detachment or relevant state changes.

Dialog and popover methods in `c/apps/gui/browser/js/dom/semantics.c` update this registry. `c/apps/gui/browser/layout/block.c` lays out registered elements against the viewport. `c/apps/gui/browser/paint/renderer.c` paints the page followed by its layers and searches layers in reverse order for hit testing. Modal entries receive a backdrop and confine input and Tab traversal. Auto popovers support dismissal through pointer interaction and Escape, with focus restoration managed by the registry.

## Document contexts and wrapper ownership

Three structures carry different parts of document state:

| Structure | State |
|---|---|
| `struct dom_doc` | Node slots, arena bytes, references to interned strings, and indexes |
| `struct js_dom_context` | Binding roots, listeners, wrappers, prototypes, invalidation, and callbacks |
| `struct js_page_context` | Runtime and scheduling state plus associated DOM, focus, and platform contexts |

`js_dom_context_activate()` selects binding state between script entries. `js_page_context_activate()` coordinates DOM selection with focus and other runtime services. Their destruction paths check that owned runtime and wrapper state has been released. These mechanisms live in `c/apps/gui/browser/js/dom/dom.c` and `c/apps/gui/browser/js/runtime/page.c`.

A node’s `jsw` field caches its wrapper weakly. Bindings for a live page retain parent and child wrappers through strong references that the QuickJS class marking callback exposes to garbage collection. Requested wrappers and their ancestor chains establish this graph. Connected wrappers retain identity and expando properties, while unreachable detached wrapper components can be collected.

An earlier claim gave the weak cache alone the job of retaining expandos; the wrapper graph provides that ownership. Native removal updates graph edges; destruction and document close invalidate handles before storage is released. `js_dom_cleanup()` breaks remaining wrapper edges and releases binding references before runtime teardown.

`DOMParser` uses a native document owned by a `struct dp_arena` with a reference count; wrappers for parsed documents retain that arena. `document.implementation.createHTMLDocument()` builds a synthetic document tree in the page’s arena, allowing nodes to move into the page through the shared insertion path. These storage paths are implemented in `c/apps/gui/browser/js/dom/domparser.c` and `c/apps/gui/browser/js/dom/dom_iface.inc`.

During navigation, `c/apps/gui/browser/app/main.c` resets the top layer, closes the JavaScript page, clears focus, control state, and retained application pointers, then frees the native document. This ordering complements serial checks: serials identify slots within living document storage, while teardown clears references before releasing that storage.

## Limitations

- Shadow composition and slot assignment are incomplete. Rendering consumers use child links; the helpers that walk a flat tree have no rendering callers. See `c/apps/gui/browser/dom/tree.h` and `c/apps/gui/browser/dom/tree.c`.
- Invalid `appendChild()` and `insertBefore()` operations return null in cases where the DOM API specifies an exception. Their insertion guard checks destination parent depth against `DOM_MAX_TREE_DEPTH`. See `c/apps/gui/browser/js/dom/dom.c`.
- Date and time inputs use textual fallback behavior. File controls expose an empty selection, and image submit entries use zero coordinates. See `c/apps/gui/browser/dom/forms.h`, `c/apps/gui/browser/dom/forms.c`, and `c/apps/gui/browser/dom/entries.c`.
- Pattern checking depends on passing the RegExp Unicode sets capability probe; the installer reports unavailable support when that probe fails. See `c/apps/gui/browser/js/dom/validation.c`.
- Browser form navigation supports encoded GET submission. POST and dialog methods report unsupported status, and oversized encoded data or URLs are refused. See `c/apps/gui/browser/app/main.c`.
- Tab order holds up to 512 entries; the top layer holds up to 32. Hint popovers, fullscreen integration, and author backdrop styling remain unsupported by this implementation of the top layer. See `c/apps/gui/browser/dom/focus.c`, `c/apps/gui/browser/dom/top_layer.inc`, and `c/apps/gui/browser/js/dom/semantics.c`.
- Form state, validation callbacks, and the state of the top layer are global to the process, alongside the selectable DOM and focus contexts. See `c/apps/gui/browser/dom/forms.c`, `c/apps/gui/browser/dom/validation.c`, and `c/apps/gui/browser/dom/top_layer.inc`.
- Adoption from DOMParser into the live document copies into the destination arena and detaches the source, changing wrapper identity. Storage for a scratch document lasts until page teardown. See `c/apps/gui/browser/js/dom/domparser.c` and `c/apps/gui/browser/js/dom/dom_iface.inc`.

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/dom/tree.h` | Node representation, document API, mutation and range contracts |
| `c/apps/gui/browser/dom/tree.c` | Arenas, recycling, attributes, IDs, importing, and shadow roots |
| `c/apps/gui/browser/dom/mutation.inc` | Native subscriptions and live range adjustment |
| `c/apps/gui/browser/dom/text_mutation.inc` | Text replacement, UTF-16 offsets, and splitting |
| `c/apps/gui/browser/dom/forms.h` | Control classification, geometry helpers, and editing contracts |
| `c/apps/gui/browser/dom/forms.c` | Control state, editing, ownership, reset, and submission |
| `c/apps/gui/browser/dom/validation.c` | Constraints, reporting, and implicit submission |
| `c/apps/gui/browser/dom/entries.c` | Entry snapshots and URL encoding |
| `c/apps/gui/browser/dom/focus.c` | Focus contexts, transitions, and Tab traversal |
| `c/apps/gui/browser/dom/top_layer.inc` | Modal and popover ordering, restoration, and dismissal |
| `c/apps/gui/browser/html/tree.c` | HTML tree construction and declarative shadow attachment |
| `c/apps/gui/browser/js/dom/dom.c` | Wrappers, insertion, live child views, invalidation, and binding contexts |
| `c/apps/gui/browser/js/dom/dom_iface.inc` | Document interfaces and creation of scratch documents |
| `c/apps/gui/browser/js/dom/domparser.c` | Wrappers for parsed documents, arena ownership, and transfer |
| `c/apps/gui/browser/js/dom/forms.c` | Control, focus, selection, and submission bindings |
| `c/apps/gui/browser/js/dom/validation.c` | Validation bindings and engine callbacks |
| `c/apps/gui/browser/js/dom/entries.c` | FormData bridge and navigation encoding |
| `c/apps/gui/browser/js/dom/select.c` | Selector queries and element collections |
| `c/apps/gui/browser/js/dom/semantics.c` | Dialog and popover operations |
| `c/apps/gui/browser/js/net/formdata.c` | FormData construction and formdata events |
| `c/apps/gui/browser/js/runtime/page.c` | Page contexts and runtime lifecycle |
| `c/apps/gui/browser/layout/block.c` | Top-layer layout |
| `c/apps/gui/browser/paint/renderer.c` | Layer painting and hit testing |
| `c/apps/gui/browser/app/main.c` | Form navigation, input integration, and document teardown |

## See also

- [HTML parsing](html-parsing.md)
- [DOM and event bindings](dom-bindings.md)
- [Frames, documents and isolation](frames-and-isolation.md)
- [Layout: block, flex, grid and text](layout.md)
- [Painting and hit testing](paint.md)
