# Layout: block, flex, grid and text

The browser’s layout engine turns a styled document and a canvas width into element geometry, scrollable extents and an ordered display list. The document traversal connects block and inline formatting to flex and grid solvers, Unicode line breaking and kernel text measurement. `layout_page()` in `c/apps/gui/browser/layout/block.c` starts layout; `c/apps/gui/browser/layout/block.h` defines the resulting items and geometry queries.

## Box tree and display list

Layout walks the DOM through formatting iterators. `layout_first()` and `layout_next()` place generated content before and after authored children and flatten wrappers with `display:contents`. Intrinsic sizing, margin collapsing, block flow, flex and grid share this child order. Generated display items retain the pseudo-element’s style and use its originating element as their event target. These mechanisms live in `c/apps/gui/browser/layout/block.c`.

The completed layout has two principal representations:

| Representation | Contents | Consumers |
|---|---|---|
| `struct boxrec` | Element border rectangle, descendant ranges and overflow edges | Element geometry and scroll measurements |
| `struct item` | Presentation rectangle, DOM provenance, clipping, text or resource references, and paint attributes | Painting and hit testing |

The box table gives geometry to containers whose children supply their visible content. `layout_node_box()` unions an element’s records, including the fragments produced by an inline element spanning several lines. Transparent block rectangles also enter the display list as pointer targets.

Display items represent rectangles, text, images, video, controls, canvases and whitespace hit regions. Video, control and canvas items reserve geometry; their state owners supply content during painting. The ownership contracts are documented in `c/apps/gui/browser/layout/block.h`.

Each box records ranges covering its descendant boxes and display items. Layout computes overflow while those ranges still describe document traversal order. `c/apps/gui/browser/layout/stacking.inc` then constructs stacking contexts and paint groups and flattens them into paint order. Painting traverses the list forwards; hit testing in `c/apps/gui/browser/paint/renderer.c` traverses it backwards.

## Block formatting

`layout_flow()` in `c/apps/gui/browser/layout/block.c` places block children vertically and gathers intervening inline siblings into line formatting contexts. It resolves a child’s width against the containing content width, establishes the content origin from padding and borders, formats descendants, and finalizes the child’s border height before advancing the flow position.

Vertical margin collapsing uses `struct mset`. It retains the largest positive margin and the most negative margin in an adjoining set; their sum determines the gap. The traversal gathers sibling margins, margins adjoining a parent’s first or last child, and margins passing through empty blocks. Clearance commits the pending set before moving below floats.

Floats contribute exclusion rectangles scoped to a block formatting context. Lines query the available horizontal interval across their vertical band. A word that fits the block’s full width can move below a float to obtain enough space. Formatting context roots, including flex containers, grid containers and containers with overflow clipping, scope the exclusions and include contained floats when calculating content height.

`layout_abspos_child()` handles absolute and fixed positioning. Recursive layout carries the containing block’s padding geometry, and positioned ancestors with an automatic height can receive a trial flow measurement before their absolute descendants are placed. Relative positioning shifts the emitted items and box records together, keeping displayed positions and geometry queries aligned.

## Inline formatting

`struct iflow` holds the pen position, line edges, accumulated line height, pending whitespace and float band. `flow_text()` measures and emits text, while `flow_node()` integrates text with inline elements, controls, replaced content and atomic inline boxes. Both are in `c/apps/gui/browser/layout/block.c`.

Whitespace collapse spans inline element boundaries. Consumed whitespace becomes a pending advance that is paid when subsequent content arrives. The pending state retains its source node, style and link so the resulting whitespace hit region has the appropriate event provenance.

Decorated inline elements use `struct ibox` to generate rectangles for their line fragments. Leading horizontal border and padding are reserved at the element’s beginning, and trailing border and padding at its end. Fragment backgrounds are inserted beneath their content. Line completion applies alignment and advances by the accumulated line height.

Inline blocks and inline flex or grid containers enter the line as atomic boxes. Their contents use the corresponding formatting algorithm, and their outer dimensions advance the inline pen. Images and native controls have dedicated sizing and item emission paths.

## Sizing and preferred ratios

Layout emits whole pixel coordinates. Helpers in `c/apps/gui/browser/layout/block.c` convert authored dimensions according to `box-sizing`, resolve percentage lengths with pixel addends, and apply minimum and maximum constraints. `height_basis()` obtains a containing height for percentage resolution. An unresolved height uses a negative sentinel, preserving zero as a valid definite height.

Percentage margins and padding are resolved from retained specified values. Recomputing these values against the current containing width keeps repeated layout and resizing from accumulating earlier pixel results. Margin resolution is shared through `c/apps/gui/browser/css/style.h`.

Intrinsic measurement provides contributions before placement. `content_width()` measures content without soft wrapping; `min_content_width()` derives contributions from unbreakable text portions and atomic children. Flex and grid use these values when determining bases, automatic minima and track sizes.

`preferred_ratio()` combines authored `aspect-ratio` with decoded image dimensions. An authored ratio applies to the box selected by `box-sizing`; an image’s intrinsic ratio relates its content dimensions. For `auto` followed by a ratio, decoded image dimensions take precedence when available. `ratio_px()` in `c/apps/gui/browser/layout/ratio.h` bounds conversion from fractional ratios to integer dimensions.

Ratio sizing participates before descendant layout and sibling placement. An automatic block height can include a trial content measurement when deriving its minimum, followed by minimum and maximum constraints. `image_decoded_size()` supplies decoded image dimensions to intrinsic measurement and placement. `tests/unit/css_layout_test.c` contains assertions covering ratio transfer, box sizing, content minima and image metadata.

## Flex layout

The flex implementation has a document bridge in `c/apps/gui/browser/layout/block.c` and a numeric solver in `c/apps/gui/browser/layout/flex.c`. `flex_collect()` gathers element items and anonymous inline runs. The bridge supplies computed styles, intrinsic contributions, preferred ratios and measurement callbacks to `layout_flex_run()`.

The solver performs four main stages:

1. Derive flex bases, automatic minima and hypothetical main sizes.
2. Sort items stably by `order` and collect lines using outer hypothetical sizes and gaps.
3. Resolve flexible lengths, freeze constrained items and redistribute remaining space.
4. Measure cross sizes, size lines, apply stretch, and position items using alignment and auto margins.

Growth uses `flex-grow` weights. Shrinkage uses `flex-shrink` multiplied by each item’s inner flex base size, so items with equal shrink factors and different bases lose different amounts. The resolution loop uses 16.16 fixed point. `round_line()` assigns remaining pixels by fractional remainder after resolution.

The output contract in `c/apps/gui/browser/layout/flex.h` distinguishes content dimensions from border geometry. `main_size` and `cross_size` are content sizes; `x`, `y`, `w` and `h` describe the border box relative to the container’s content origin.

Cross-size callbacks can perform trial layouts. Rollback restores item, box and float state, with `discard_items()` releasing image resources owned by abandoned entries. During final placement, stretched row items receive their used height while descendants are formatted. Percentage descendants and clipping can then use that height.

`c/apps/gui/browser/layout/block.c` includes `c/apps/gui/browser/layout/flex.c` and `c/apps/gui/browser/layout/grid.c` directly. This supplies the solvers to existing browser and host harness source lists that compile the block layout file. The text formatter remains another translation unit.

## Columns

Flex columns establish each item’s cross width before measuring its content height. `flex_column_spec()` sends those height contributions to the same flex solver, then places items at their resolved main sizes. A definite container height provides available main space; an automatic height is represented as indefinite space and derives its extent from the items. Definite main space also permits line collection for wrapping columns. The bridge is in `c/apps/gui/browser/layout/block.c`.

The integration fixture in `tests/unit/flex_column_bridge_test.c` describes a 200-pixel column containing a fixed 40-pixel header and a flexible scrolling body. It asserts a 160-pixel body and retains the overflowing child’s 400-pixel height. It also checks that the child’s paint clip follows the body’s resolved height.

## Grid layout

The grid bridge reads retained declaration text from `struct cstyle`, parses templates and placement, and calls `grid_layout()`. Retaining the declarations preserves named lines, areas, spans and track sizing functions for layout. The bridge supplies both the element and root font sizes when parsing font-relative lengths. See `c/apps/gui/browser/css/style.h` and `c/apps/gui/browser/layout/block.c`.

`c/apps/gui/browser/layout/grid.c` performs these stages:

1. Expand templates, construct named lines and place items, extending the implicit grid as needed.
2. Collapse unused repeated tracks and adjust gutters for `auto-fit`.
3. Size columns from item contributions.
4. Measure rows using the widths established by the columns, then size rows and align tracks and items.

Placement supports numbered and named lines, negative line references, spans, named areas, and row or column auto flow with dense placement. Track sizing handles fixed and percentage lengths, intrinsic sizes, fractional tracks, `minmax()` and `fit-content()`.

A spanning item distributes its contribution according to the tracks’ sizing functions and growth limits. `distribute_extra_space()` freezes tracks as they reach their limits and continues distribution among eligible tracks. This preserves the constraints of individual tracks while satisfying the spanning contribution.

Percentage item widths resolve once the grid area is known. `grid_resolve_width_cb()` receives the area width, including internal gaps across a span, before row measurement. Percentage height constraints resolve against the solved area height later. Width and span assertions are in `tests/unit/grid_percentage_item_test.c`; height constraint assertions are in `tests/unit/css_layout_test.c`.

The bridge places returned rectangles through the shared flex/grid item placement path. The grid’s used height follows its solved tracks, and descendant geometry contributes to scrollable overflow.

## Text shaping and line breaking

`text_measure()` in `c/apps/gui/browser/app/runtime.c` calls the application text measurement wrapper. Kernel measurement and drawing share the shaping implementation reached through `c/kernel/gui/text.c`. Its `shape_line()` implementation in `c/lib/text/shape.c` decodes UTF-8, resolves bidirectional runs, selects fonts and scripts, and shapes glyph advances.

Layout and painting share monospace and bold face selection. Helpers in `c/apps/gui/browser/dom/forms.h` split long strings at bounded UTF-8 run boundaries. Both consumers use those boundaries, keeping accumulated advances consistent across native text calls.

The browser uses two line formatting paths:

| Path | Selection and role |
|---|---|
| `flow_text()` with `struct iflow` | General inline flow, including mixed content, decorations and float exclusions |
| `flow_ltx_paragraph()` with `ltx_layout_runs()` | Compatible horizontal text contexts requesting transformation, spacing or indentation |

The paragraph formatter transforms and collapses styled runs into a common text buffer. It computes break opportunities across run boundaries, chooses lines greedily, and returns fragment geometry. The bridge copies processed bytes into storage owned by the completed layout and emits text items from the fragments. These stages are implemented in `c/apps/gui/browser/layout/text.c` and `c/apps/gui/browser/layout/block.c`.

The general inline path consults `ltx_break_utf8()` when cutting an overlong token. It selects a fitting legal opportunity when available and uses a UTF-8 character boundary as its fallback. Intrinsic text measurement uses the same break opportunity helper for text containing non-ASCII bytes. The shared Unicode line break rules live in `c/lib/text/linebreak_core.inc`.

Spacing crosses the measurement and paint interfaces. `ltx_measure_run()` adds CSS spacing to native advances, and `c/apps/gui/browser/paint/text.inc` draws with matching segments and advances. `tests/unit/text_wiring_test.c` contains assertions for transformed text, indentation, spacing, line height and recorded paint operations.

## Scrolling and overflow

Layout records used box dimensions and scrollable extents as distinct quantities. `block_height()` applies specified heights and minimum or maximum constraints, while descendants retain their geometry for overflow calculation. The fixture in `tests/unit/text_wiring_test.c` asserts an 80-pixel container height, a 240-pixel scrollable height and a following sibling placed after the 80-pixel box.

`box_overflow_pass()` starts from each box’s padding extent and includes descendant display items, box records and positive trailing margins. It runs before stacking order rearranges the display items. `layout_node_scroll()` exposes the resulting dimensions to CSSOM. Both functions are in `c/apps/gui/browser/layout/block.c`.

Layout stamps overflow clips onto items in document coordinates and intersects nested clips. For an automatic height constrained by a `max-height` expressed in pixels, the shared block wrapper finalizes descendant vertical clips after content measurement. The `has_clip` flag represents an active clip even when its extent is zero.

Element scroll positions live in CSSOM state. `js_cssom_project_item()` applies accumulated ancestor offsets for painting and hit testing and reconstructs nested clip intersections from padding boxes. Client geometry uses the same scroll offsets. These paths are in `c/apps/gui/browser/paint/element_scroll.inc`, included by `c/apps/gui/browser/js/style/cssom.c`, and consumed by `c/apps/gui/browser/paint/renderer.c`.

After relayout, `js_cssom_reconcile_element_scroll()` clamps stored positions to current ranges. Wheel handling and script scrolling share the offset state. The application rendering step dispatches queued scroll events. `tests/unit/element_scroll_test.c` checks these connections across client geometry, painting, clipping and hit testing.

## Incremental relayout

Mutation tracking in `c/apps/gui/browser/js/dom/dom.c` records an invalidation level and coalesces affected roots. Attribute changes mark the element’s subtree and following siblings, accounting for inheritance and sibling selectors. Child insertion and removal mark the parent’s subtree as a layout change.

`restyle()` in `c/apps/gui/browser/app/main.c` passes these scopes to `css_apply_scoped()`. The CSS engine compares old and resulting styles and classifies the changes. An unchanged scoped result lets the application finish the update early. Structural invalidation requests layout even when computed styles compare equal.

When an update reaches layout, `layout_page()` rebuilds the formatted document’s box table and display list at the current canvas width and reattaches cached decoded images. The application also routes changes classified as paint changes through this rebuild, because layout populates the display items’ paint attributes.

Geometry reads can settle pending work synchronously. `flush_layout()` in `c/apps/gui/browser/js/style/cssom.c` invokes the registered reflow hook when layout is missing or a dirty episode requires settlement. The application hook performs style and geometry settlement before the measurement returns.

Decoded images have a geometry generation tracked by `layout_image_geometry_pending()`. The application’s image resource and settlement paths flush newly available dimensions into layout, updating intrinsic sizes and following content positions. The implementation is in `c/apps/gui/browser/layout/block.c` and `c/apps/gui/browser/app/main.c`.

Opacity animation uses `layout_refresh_opacity()` to refresh item snapshots and rebuild stacking order, since opacity affects stacking context membership. Transform animation values are read by the painter and request repainting. `tests/unit/animation_refresh_test.c` observes layout build counts and compares refreshed output with a full layout.

## Limitations

- Formatting integration uses horizontal geometry. The flex bridges supply horizontal writing mode and left-to-right axes. Their baseline callbacks are unset, and the grid bridge supplies unavailable item baselines. See `c/apps/gui/browser/layout/block.c`.
- CSS multicolumn flow is unimplemented. `column-count`, `column-width` and `column-rule` appear in the ignored-property list in `c/apps/gui/browser/css/extra.c`.
- Grid lacks subgrid, masonry, page or column fragmentation, and the later column sizing passes that respond to changed contributions after row sizing. Absolute children use the shared positioning path, with an approximate static position for flex/grid containers and no grid-line positioning. See `c/apps/gui/browser/layout/grid.c` and `c/apps/gui/browser/layout/block.c`.
- The paragraph bridge excludes decorated or atomic inline content, active float exclusions, vertical writing and soft-hyphen text. Paragraph-wide bidi ordering across fragments, automatic hyphenation and spacing by shaped clusters remain incomplete. Letter spacing uses Unicode scalar boundaries. See `c/apps/gui/browser/layout/text.h`, `c/apps/gui/browser/layout/block.c` and `c/apps/gui/browser/paint/text.inc`.
- Allocation failures can reach reduced flex or grid paths. Resource bounds include 64 float exclusions per formatting context, 256 collected paragraph runs, 4,096 tracks per grid axis, and a 64 MiB combined budget for the item and box arrays. Arena exhaustion triggers layout retries; reaching the budget clears the partial result and reports refusal. See `c/apps/gui/browser/layout/block.c` and `c/apps/gui/browser/layout/grid.h`.

## See also

- [Browser architecture](overview.md)
- [The document tree](dom.md)
- [CSS: parsing, cascade and computed style](css.md)
- [Painting and hit testing](paint.md)
- [Testing the browser](testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/layout/block.c` | Formatting traversal, box records, block and inline flow, sizing, solver bridges, overflow and layout lifecycle |
| `c/apps/gui/browser/layout/block.h` | Display items, layout entry points, geometry queries and ownership contracts |
| `c/apps/gui/browser/layout/flex.c` | Flex bases, automatic minima, line collection, flexible lengths and alignment |
| `c/apps/gui/browser/layout/flex.h` | Flex inputs, measurement callbacks and output coordinate contracts |
| `c/apps/gui/browser/layout/grid.c` | Grid parsing, placement, track sizing, alignment and implementation limits |
| `c/apps/gui/browser/layout/grid.h` | Grid values, measurement callbacks, geometry interfaces and track bounds |
| `c/apps/gui/browser/layout/ratio.h` | Bounded conversion of ratios to pixel dimensions |
| `c/apps/gui/browser/layout/text.c` | Transformation, whitespace processing, break opportunities, spacing and line construction |
| `c/apps/gui/browser/layout/text.h` | Text formatter contracts and integration notes |
| `c/apps/gui/browser/layout/stacking.inc` | Stacking contexts, paint groups and display list ordering |
| `c/apps/gui/browser/css/style.h` | Computed layout values, margin resolution and retained grid declarations |
| `c/apps/gui/browser/css/engine.c` | Scoped style application and change classification |
| `c/apps/gui/browser/css/extra.c` | Additional declaration processing and ignored-property reporting |
| `c/apps/gui/browser/app/main.c` | Mutation settlement, reflow hooks, image geometry updates and animation handling |
| `c/apps/gui/browser/app/runtime.c` | Browser text measurement adapter |
| `c/apps/gui/browser/dom/forms.h` | Shared native text run boundaries and measurement helpers |
| `c/apps/gui/browser/js/dom/dom.c` | Mutation invalidation levels and affected scopes |
| `c/apps/gui/browser/js/style/cssom.c` | Geometry reads, synchronous layout flushing and element scroll state |
| `c/apps/gui/browser/paint/element_scroll.inc` | Scroll projection, nested clips, offset reconciliation and event dispatch |
| `c/apps/gui/browser/paint/text.inc` | Text drawing with shared spacing segments and native advances |
| `c/apps/gui/browser/paint/renderer.c` | Display list painting and hit testing |
| `c/kernel/gui/text.c` | Kernel text measurement and drawing through the shared shaper |
| `c/lib/text/shape.c` | UTF-8 decoding, bidi processing, font/script segmentation and glyph placement |
| `c/lib/text/linebreak_core.inc` | Shared Unicode line break classification and rules |
| `tests/unit/css_layout_test.c` | Assertions for ratios, flex columns and grid sizing constraints |
| `tests/unit/flex_column_bridge_test.c` | Flex column used heights, percentage descendants and scrolling bodies |
| `tests/unit/grid_percentage_item_test.c` | Percentage widths against resolved grid areas |
| `tests/unit/text_wiring_test.c` | Formatter output, spacing, paint operations and overflow geometry |
| `tests/unit/element_scroll_test.c` | Element scrolling across geometry, painting, clipping and input |
| `tests/unit/animation_refresh_test.c` | Opacity refresh comparisons and layout build observations |