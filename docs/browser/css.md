# CSS: parsing, cascade and computed style

The browser combines NetSurf LibCSS with native code for stylesheet assembly, custom properties, nesting, additional properties, animation and JavaScript interfaces. LibCSS supplies declaration parsing, selector matching and the core cascade. The adapter converts its computed styles into values used by layout and painting, then applies native property extensions. The main implementation is in `c/apps/gui/browser/css/engine.c` and `c/apps/gui/browser/css/extra.c`.

## Pipeline and ownership

`c/apps/gui/browser/app/main.c` assembles author stylesheets in document order. Import expansion runs while each sheet still has its response URL. Variable expansion normalizes nesting before collecting custom properties and substituting references. The resulting text feeds both the LibCSS adapter and the extension pass.

| Stage | Implementation | Result |
|---|---|---|
| Collect style elements and linked sheets | `c/apps/gui/browser/app/main.c` | Author CSS in document order |
| Expand imports and rebase resource URLs | `c/apps/gui/browser/css/import.c` | Dependencies inserted at their import positions |
| Normalize nesting | `c/apps/gui/browser/css/nesting.c` | Flattened rules with preserved declaration order |
| Expand custom properties | `c/apps/gui/browser/css/vars.c` | Text with substituted `var()` references |
| Select, inherit and convert styles | `c/apps/gui/browser/css/engine.c` | LibCSS computed styles and `struct cstyle` |
| Apply native extensions | `c/apps/gui/browser/css/extra.c` | Additional layout, paint and animation values |

The allocation path, `css_expand_vars_alloc()`, invokes `css_normalize_stylesheet()` before variable collection. Core and extension stylesheet parsing also use that normalization entry point, keeping their selector input consistent.

Some declarations remain text until a consumer has the required context. A percentage translation needs the element’s box; a gradient stop expressed in `em` needs its font size. `c/apps/gui/browser/css/style.h` retains declaration spans into either the stylesheet copy owned by the extension pass or the element’s style attribute. Consumers resolve those spans with font and geometry information.

## LibCSS and the adapter

The vendored engine lives under `third_party/css/libcss/`, with project changes in its parser and selection code. `c/apps/gui/browser/css/engine.c` supplies callbacks through which LibCSS reads the browser’s DOM: element names, classes, IDs, attributes, parents, ancestors and siblings. Other callbacks supply structural and interaction state.

`css_init()` creates a selection context and installs the stylesheet for the user agent. Its rules define initial presentation for HTML elements, lists, headings, links and form controls. A supplementary sheet for the user agent handles full quirks mode. The document’s quirks setting also controls LibCSS’s parser configuration.

`css_apply()` temporarily attaches the author sheet to the context and walks the document to select styles. The parsed author sheet is cached against a private copy of its normalized bytes and its quirks setting. Comparing content allows callers to reuse and rewrite their input buffers safely.

The adapter supplies viewport dimensions, device dimensions, colour scheme, a preference for reduced motion and a preference for reduced transparency through LibCSS’s media context. Its unit context supplies viewport sizes, default font metrics and the root style used for relative units.

## Selectors and the cascade

Selector parsing lives in `third_party/css/libcss/src/parse/language.c`; matching and cascade precedence live in `third_party/css/libcss/src/select/select.c`. These implement type, class, ID and attribute selectors, combinators, structural pseudo classes and selector lists. Project extensions support complex alternatives inside `:is()` and `:where()`, and lists of compound selectors inside `:not()`.

The adapter connects `:hover`, `:active` and `:focus` to browser interaction state. Form callbacks supply checked, enabled and disabled state. The navigation fragment supplies `:target`. Supported HTML and SVG presentation attributes enter through LibCSS’s presentation hint interface. These connections are in `c/apps/gui/browser/css/engine.c`.

For core properties, LibCSS compares origin, importance and specificity, with source order resolving equal precedence. Each element’s style attribute is parsed as an inline stylesheet and supplied during selection.

Native extensions reuse LibCSS’s selector matcher. The adapter builds a synthetic stylesheet whose `z-index` declarations encode the IDs of extension rules. A matching callback records every applicable rule and the specificity of its matching selector arms. This supplies the extension pass with more information than the final winning declaration would retain.

`c/apps/gui/browser/css/extra.c` applies those author declarations in this order:

1. Normal stylesheet declarations, sorted by specificity and source order.
2. Normal inline declarations.
3. Important stylesheet declarations, sorted by specificity and source order.
4. Important inline declarations.

`c/apps/gui/browser/css/extra_cascade.inc` splits importance groups before merging declarations. Processing declarations in their written order preserves shorthand and longhand competition within each group.

## Imports and changing stylesheets

`css_import_expand_alloc()` in `c/apps/gui/browser/css/import.c` inserts imported text where the import appeared. Resource URLs are rebased against the source sheet’s response URL, including its final URL after redirects. Import media conditions become enclosing `@media` rules, preserving their conditions for later selection.

The browser supplies callbacks for fetch and URL resolution. The fetch callback uses the browser’s resource machinery and pumps progress while waiting. Cycle detection follows the current dependency path, allowing the same dependency to contribute rules through multiple branches.

`browser_css_collect()` walks style elements and linked sheets together. Repeated links retain their cascade positions while sharing fetched resource bytes. Appending a sheet is atomic with respect to output capacity: a failed append preserves the accumulated prefix.

Live stylesheet handling is in `c/apps/gui/browser/app/stylesheets.inc`. It tracks link occurrences, response bases and retained CSS across frames. Changes to stylesheet producers trigger reconstruction in document order, followed by style selection and layout. Link load or error events are dispatched after application processing.

## Nesting and feature queries

`c/apps/gui/browser/css/nesting.c` lowers nested selectors into rules accepted by LibCSS. An ampersand becomes `:is(parent-list)`, carrying the parent list’s maximum specificity. A child selector without an ampersand receives a parent descendant prefix; leading combinators retain their relationship to that parent.

Declarations surrounding nested rules become consecutive rules using the original parent selector. This preserves both their source order and the specificity of individual arms in the parent selector. The scanner accounts for comments, strings, escapes, brackets and function arguments. Balanced braces inside values of custom properties remain value text.

Media groups retain their conditions. Support groups are evaluated through the engine and lowered to active or inactive media groups.

`CSS.supports()` in `c/apps/gui/browser/js/style/cssom.c` reaches `css_supports_decl()`. The engine consults native property handlers and LibCSS parsing. The vendored parser also calls browser hooks when evaluating `@supports`, allowing feature queries to account for native property producers.

## Custom properties

`c/apps/gui/browser/css/vars.c` collects declarations of custom properties into a document table before LibCSS parses declaration values. Collection consults the engine’s evaluator for media queries. Candidate values are ranked by importance, a classification by base theme, an estimated selector specificity and source order.

The classification by base theme favours declarations with an unqualified selector arm over declarations whose arms all require a class, ID or attribute. Values are stored in an arena at their full accepted length. Checks for bracket balance mark unusable values as unavailable for substitution, protecting subsequent stylesheet parsing from an incomplete value.

The preprocessor resolves references between stored values and expands `var()` expressions with fallbacks. The allocation path measures the expanded output before publishing it, so capacity failure preserves the previous complete output.

Reads of computed custom properties have another path in `c/apps/gui/browser/css/engine.c`. They search the element’s inline declarations, then its ancestors’ inline declarations, and finally the document table.

## Computed and layout values

`style_node()` in `c/apps/gui/browser/css/engine.c` selects each element’s style and composes it with its parent. The root style establishes the reference for descendant `rem` units. Nodes retain two representations:

| Representation | Purpose |
|---|---|
| LibCSS style in `computed` | Core property inheritance and access to computed properties |
| `struct cstyle` in `style` | Values consumed by layout, painting and native extensions |

LibCSS interns computed styles and counts their references, allowing equal styles to share storage. The adapter keeps a reference on each styled element after conversion.

`struct cstyle` preserves information needed during later resolution: percentage flags, fixed offsets for values that mix lengths and percentages, margin kinds and retained declarations. Layout resolves percentages against the containing block. Percentage padding uses the containing block’s width for every edge.

Unit conversion preserves the fractional scale until multiplication. Normalization of line height resolves dimensional values before they become an inheritance source; a unitless line height remains a ratio. This keeps an inherited dimensional line height tied to the font context in which it was computed.

`css_computed_text()` serializes supported computed properties after calling `css_ensure_styled()`. That function uses remembered author CSS, interaction revisions and DOM invalidation information to refresh styles. Scoped refreshes also invoke the registered extension pass.

Transform reads use the CSSOM bridge in `c/apps/gui/browser/js/graphics/matrix.inc`. It flushes layout and resolves the winning transform against the element’s reference box. Elements with a box receive matrix serialization; boxless elements receive the computed function list.

## Backgrounds and gradients

`c/apps/gui/browser/css/background.c` shares background parsing between declaration capture and later consumers. It represents image layers, size, position, repetition, origin, clipping, attachment and colour. Shorthands retain their original spans with provenance bits, allowing the extension cascade to merge them with longhands while preserving storage lifetime.

`c/apps/gui/browser/css/gradient.c` parses linear, radial and conic gradients, including repeating forms. Its representation retains colour spans, omitted positions, interpolation hints and stops with two positions. Lengths relative to the font resolve with font context; positions that depend on image geometry reach the painter.

`c/apps/gui/browser/paint/gradient.c` constructs geometry, fixes stop positions and interpolates colours with premultiplied alpha. It resolves `currentColor` from the element’s foreground colour and alpha. Literal colours use the shared colour evaluator.

`c/apps/gui/browser/paint/background.c` prepares cached bitmap images and gradients through one layer model. It resolves positioning areas, intrinsic bitmap dimensions, `cover`, `contain`, repetition and clipping. Position percentages use the remaining space after subtracting the image size from the positioning area.

## Interpolation and animation

`c/apps/gui/browser/css/interp.c` supplies value interpolation, transform mathematics, composition and easing. Generic interpolation combines corresponding numeric tokens when the surrounding token structure agrees. Compatible transform functions interpolate component by component. Other pairs use matrix decomposition and recomposition; singular endpoints use a discrete change at halfway.

`c/apps/gui/browser/css/extra.c` captures keyframe rules and animation and transition shorthand spans. `c/apps/gui/browser/js/style/anim.c` resolves those descriptions into entries that sample opacity and transforms. Entries copy endpoint text because stylesheet recompilation can replace its original storage.

CSS animation and Web Animations sampling share the page deadline queue in `c/apps/gui/browser/js/runtime/page.c`. The bridge in `c/apps/gui/browser/js/style/waapi_native.inc` retains target wrappers and writes sampled values into `struct cstyle`.

The animation system saves cascade values and reapplies sampled overlays after style changes. Transforms are read during painting. Opacity changes refresh opacity and visibility snapshots for the display list through the integration in `c/apps/gui/browser/app/main.c`.

## CSSOM and font interfaces

`element.style` and `getComputedStyle()` use the declaration interface in `c/apps/gui/browser/js/dom/dom.c`. Inline declarations read and rewrite the style attribute. Computed declarations read selected values through the engine. `c/apps/gui/browser/js/style/cssom.c` adds property accessors, validation and serialization.

Stylesheet CSSOM objects use source text as their model, preserving selector text, rule text and rule order. `document.styleSheets` gathers style elements and creates `CSSStyleSheet` objects owned by the DOM. `insertRule()` and `deleteRule()` serialize changed rules into the owning style element and notify DOM invalidation. The browser’s stylesheet observer then rebuilds the author CSS.

CSSOM geometry accessors flush pending layout through an embedder callback. `CSS.escape()` and `CSS.supports()` are installed alongside these interfaces. Matching of media queries uses the engine’s evaluator.

`c/apps/gui/browser/js/style/fontface_prelude.inc` defines `FontFace`, `FontFaceSet` and `document.fonts`, with descriptors, collection operations, iteration and promises. A fallback implementation lives in `c/apps/gui/browser/js/style/cssom.c`. Both report loaded status and resolve readiness immediately. Platform font initialization loads faces from disk in `c/kernel/gui/text.c`.

## Style reporting

`c/apps/gui/browser/css/report.c` collects pipeline observations in `struct css_report`, declared in `c/apps/gui/browser/css/report.h`. The application resets the record before stylesheet processing and prints it after the full style pass.

The record covers discovered sheets, skipped links, fetch outcomes, assembled bytes, parses, accepted declarations and parser drops. Drop buckets distinguish rejected selectors, unknown at-rules, unknown properties, rejected values and trailing tokens. Parser recovery has its own counters because recovery events and named drops count different populations.

A counter for cache hits identifies passes that reuse parsed author CSS. The report labels cached parses so their lack of fresh parser activity can be interpreted correctly. Import processing prints dependency outcomes through the application’s import summary.

`css_report_verbose` controls bucket and sheet detail, with level 2 logging individual drops as they occur. `css_report_get()` exposes the record to native consumers. These counters describe LibCSS parsing; native extensions can consume declarations rejected by that parser.

## Limitations

- Substitution of custom properties uses one document table and heuristic selector ranking. Subtree overrides and changes to theme classes can differ from a cascade evaluated per element. See `c/apps/gui/browser/css/vars.c`.
- Layer blocks lack layer precedence; container and scope groups receive unconditional group handling in the vendored parser. Imports with layer or support qualifiers remain unexpanded. Nesting refuses ampersands at the top level, nested scope or container rules, nested selectors qualified by namespace and child rules below parents that are pseudo elements.
- `:has()` is accepted as inert syntax, while `:lang()` and callbacks for visited state return false. The extension matcher excludes targets that are pseudo elements, and `getComputedStyle()` ignores its argument for a pseudo element. See `third_party/css/libcss/src/parse/language.c`, `c/apps/gui/browser/css/engine.c` and `c/apps/gui/browser/js/dom/dom.c`.
- Painted animation covers opacity and transforms. CSS timing capture uses shorthands; timing given only with longhands, timing functions for each keyframe and CSS animation events are unimplemented. Web Animations rejects unsupported properties, pseudo targets, additive composition and implicit endpoints, and restricts transform forms. CSS entries are capped at 32; the native Web Animations bridge has 128 target slots.
- Backgrounds reject fixed and local attachment. Background declarations have a ceiling of 16 layers; gradients have 32 stored stop entries, including expanded positions and hints. The adapter estimates `ex` and `ch` metrics. See `c/apps/gui/browser/css/background.h`, `c/apps/gui/browser/css/gradient.h` and `c/apps/gui/browser/css/engine.c`.
- Stylesheet CSSOM exposes style elements, with a ceiling of 64 elements in the collection. Exposure of linked sheets, constructed stylesheets, adoption and complete collection liveness remain incomplete. Added font faces do not enter layout’s font selection, and font checks return true. Linked stylesheet loading also lacks a MIME filter. See `c/apps/gui/browser/js/style/cssom.c`, `c/apps/gui/browser/js/style/fontface_prelude.inc` and `c/apps/gui/browser/css/report.c`.

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/app/main.c` | Stylesheet collection, import callbacks, style passes and repaint integration |
| `c/apps/gui/browser/app/stylesheets.inc` | Live stylesheet tracking, reconstruction and link events |
| `third_party/css/libcss/src/parse/language.c` | Parsing of declarations, selectors and conditional groups |
| `third_party/css/libcss/src/select/select.c` | Selector matching and cascade precedence |
| `third_party/css/libcss/src/select/computed.c` | Composition and lifetime of computed styles |
| `third_party/css/libcss/src/select/arena.c` | Interning of computed styles |
| `c/apps/gui/browser/css/engine.c` | DOM adapter, selection, conversion, computed reads and style flushing |
| `c/apps/gui/browser/css/style.h` | Layout and paint representation, retained declarations and animation contracts |
| `c/apps/gui/browser/css/extra.c` | Native property capture, extension selection and keyframe collection |
| `c/apps/gui/browser/css/extra_cascade.inc` | Importance splitting and declaration merging |
| `c/apps/gui/browser/css/import.c` | Dependency expansion and resource URL rebasing |
| `c/apps/gui/browser/css/import.h` | Import callbacks and resource budgets |
| `c/apps/gui/browser/css/nesting.c` | Nesting normalization and declaration ordering |
| `c/apps/gui/browser/css/vars.c` | Collection, storage and substitution of custom properties |
| `c/apps/gui/browser/css/interp.c` | Interpolation, transform mathematics, composition and easing |
| `c/apps/gui/browser/css/background.c` | Background declarations and layer geometry |
| `c/apps/gui/browser/css/background.h` | Background representation and layer ceiling |
| `c/apps/gui/browser/css/gradient.c` | Gradient parsing |
| `c/apps/gui/browser/css/gradient.h` | Gradient representation and stop ceiling |
| `c/apps/gui/browser/paint/background.c` | Bitmap and gradient layer preparation |
| `c/apps/gui/browser/paint/gradient.c` | Gradient geometry, stop fixup and colour sampling |
| `c/apps/gui/browser/js/dom/dom.c` | Inline and computed declaration interfaces |
| `c/apps/gui/browser/js/style/cssom.c` | Stylesheet objects, mutation, feature queries and geometry interfaces |
| `c/apps/gui/browser/js/graphics/matrix.inc` | Computed transform resolution and serialization |
| `c/apps/gui/browser/js/style/fontface_prelude.inc` | Font face and font set interfaces |
| `c/apps/gui/browser/js/style/anim.c` | Web Animations interfaces and CSS animation clock |
| `c/apps/gui/browser/js/style/waapi_native.inc` | Animation values applied to native styles |
| `c/apps/gui/browser/js/runtime/page.c` | Shared timer and animation deadlines |
| `c/apps/gui/browser/css/report.c` | Parser instrumentation and stylesheet summaries |
| `c/apps/gui/browser/css/report.h` | Report fields and accessors |
| `c/kernel/gui/text.c` | Platform font initialization |

## See also

- [Loading pages: fetch, URLs and the HTTP cache](loading-and-fetch.md)
- [The document tree](dom.md)
- [Layout: block, flex, grid and text](layout.md)
- [Painting and hit testing](paint.md)
- [DOM and event bindings](dom-bindings.md)
- [Testing the browser](testing.md)
