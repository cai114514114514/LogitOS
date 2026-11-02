# Painting and hit testing

The browser paints a flat display list and searches that list in reverse to resolve pointer targets. Layout supplies item geometry and ordering; the painter projects items into the browser viewport and draws into the application window. Text uses the kernel font renderer, while images, SVG rasters and canvas pixels travel through bitmap calls. The main implementation is `c/apps/gui/browser/paint/renderer.c`.

## Display lists and paint order

`struct item` in `c/apps/gui/browser/layout/block.h` connects geometry to a DOM node and carries clipping bounds, opacity and drawing data.

| Item type | Role |
|---|---|
| `IT_RECT` | Box backgrounds, borders and decoration; also geometry for transparent boxes |
| `IT_TEXT` | Text spans, font selection, colour and decorations |
| `IT_IMAGE` | Decoded images, including rasterised inline SVG |
| `IT_VIDEO` | Media boxes whose frames belong to the media engine |
| `IT_CONTROL` | Form controls drawn from their current state |
| `IT_CANVAS` | Canvas boxes whose pixels belong to a JavaScript context |
| `IT_HIT` | Inline whitespace advances used for pointer targeting |

Layout builds stacking contexts and paint groups in `c/apps/gui/browser/layout/stacking.inc`, then flattens them into the display list. The ordering covers context backgrounds, negative stacking levels, block backgrounds, floats, inline content, positioned content and positive stacking levels. Children of flex and grid containers use CSS order, with DOM order breaking ties.

Stacking contexts keep their contents together within the enclosing context. This preserves the relationship between a parent’s background and descendants with their own stacking levels. The older scalar sorting implementation remains behind a control flag in `c/apps/gui/browser/layout/block.c`; the default path includes the stacking tree implementation.

Painting visits page content first, then top layer entries in stack order. Items are filtered by their top layer owner so each entry receives one pass. Modal dialogs receive a translucent backdrop before their content. Dialogs and popovers share the registry declared in `c/apps/gui/browser/dom/top_layer.h` and implemented in `c/apps/gui/browser/dom/top_layer.inc`.

## Projection into the window

`browser_paint_scroll()` receives the viewport’s origin and size in window coordinates, plus page scroll offsets on both axes. A document item starts at the viewport origin plus its layout position minus page scroll. Content fixed to the viewport and top layer content retain their viewport positions as the page scrolls.

Element scrolling is applied through `js_cssom_project_item()`. Its implementation in `c/apps/gui/browser/paint/element_scroll.inc` is included by `c/apps/gui/browser/js/style/cssom.c`. It subtracts ancestor scroll offsets and, when projection requires it, reconstructs overflow clipping from ancestor padding boxes. Each ancestor contributes its own projected box, keeping an outer scroller’s clip stationary while content inside it moves.

The painter then composes CSS transforms from the item’s element through its ancestors. Each transform resolves percentages against its owner’s border box and applies that element’s transform origin. Mapping all four item corners produces the bounding rectangle used for placement and viewport culling.

The viewport clip stays anchored to the browser’s content area. The painter intersects each item’s rectangular overflow clip with that area and updates the native clip when the intersection changes.

Drawing coordinates use window logical points. Coverage tiles use device pixels, with widths and heights calculated from the difference between converted edges. This keeps adjoining edges aligned at fractional display scales. The wrappers in `c/apps/logit.h` send drawing requests to the kernel; `c/kernel/gui/wm.c` validates arguments and selects the application window’s surface.

## Backgrounds, rounded boxes and shadows

Three modules cooperate on box decoration:

| Module | Responsibility |
|---|---|
| `c/apps/gui/browser/paint/background.c` | Background image geometry, cached pixels and repetition |
| `c/apps/gui/browser/paint/gradient.c` | Linear, radial and conic gradient preparation and sampling |
| `c/apps/gui/browser/paint/box.c` | Rounded coverage, background composition and solid border edges |

Background preparation borrows decoded images from the layout cache. It resolves intrinsic dimensions, `cover`, `contain`, authored sizes, positioning and repetition in CSS coordinates before applying axis scaling. Keeping that order preserves the crop and tile geometry chosen for the source box.

The box painter composes background layers from the last declared layer toward the first, over the background colour. It computes coverage for the border, padding and content regions, composes the layers within each region, then applies coverage, ancestor clipping and element opacity. Applying opacity to the completed background preserves the relationship between overlapping layers.

Rounded boxes use horizontal and vertical radii for each corner. Percentage radii resolve against the corresponding box dimension, and one reduction factor handles overlapping corners. Coverage is generated in bounded device tiles. The tiled path handles percentage corners and differing solid border edges; fallback paths retain the older circular radius approximation documented in `c/apps/gui/browser/paint/renderer.c`.

Rounded overflow ancestors contribute masks to the tiled box path. The renderer also has cached corner paths for simpler clipping cases. Outer shadows paint below box decoration, and inset shadows paint above it. Browser coverage generation uses the shared graphics engine.

## Text painting

`c/apps/gui/browser/paint/text.inc` sends document text through `gui_text_run_w()`, carrying font size, monospace and bold selection, colour and a length bounded span. The kernel renderer in `c/kernel/gui/text.c` uses shared shaping machinery for measurement and drawing.

Spacing uses the segmentation shared with `c/apps/gui/browser/layout/text.c`. With word spacing, the painter keeps words together and advances across spaces. With letter spacing, it divides text at UTF-8 scalar boundaries. `browser_text_run_advance()` applies the same segmentation when measuring prefixes for input geometry.

Long segments are divided into native runs using the helper in `c/apps/gui/browser/dom/forms.h`. Each chunk ends at a UTF-8 boundary and is measured using the same chunking used for drawing. This keeps subsequent chunks and caret advances aligned with the native calls.

Underline, overline and strike-through are additional rectangles positioned from the item’s em box and font size. The painter records emitted document text runs and their positions; `browser_paint_text_dump()` and `browser_paint_text_counts()` expose that record for diagnostics.

## Images and SVG

`struct image` in `c/lib/image/img.h` contains dimensions and straight RGBA pixels. The image branch sends these pixels to `gui_blit()`. For an HTML image element, the painter subtracts border and padding from the destination so the bitmap occupies the content box. The item retains its outer geometry for input and damage tracking.

Inline SVG becomes an `IT_IMAGE` item associated with the outer SVG element. During layout, `c/apps/gui/browser/paint/svg_dom_paint.inc` serialises the current subtree, including live attributes and resolved paint properties. It sets the outer SVG dimensions to the size selected by layout, then `c/apps/gui/browser/layout/block.c` decodes the serialised input into a raster.

This layout boundary lets script-created elements, attribute edits and CSS paint changes contribute to the next image. The serializer carries winning paint declarations into the decoder, preserving the cascade result.

`c/apps/gui/browser/paint/svg_reflow_cache.inc` retains successful rasters using the complete serialised input as the key. A hash selects candidates; length and byte comparison establish a match. Entries borrowed by the current layout pass remain pinned, while unused entries can be evicted within the retention budgets.

The decoder in `c/lib/image/svg.c` and `c/lib/image/svg_scene.inc` handles paths and basic shapes, fills, strokes, affine transforms, local references and clipping paths. The resulting image follows the same bitmap projection path as other decoded images.

## Pointer geometry and hit testing

`browser_hittest_node_viewport()` accepts a viewport-local point, the viewport’s window origin and page scroll offsets. Passing the origin used for painting preserves rounding at fractional translated edges.

Native hit testing visits top layer entries first, then traverses each layer’s items in reverse order. Each candidate receives element scroll projection and the supported transform projection before its box and rectangular overflow clip are tested. The traversal filters hidden items, culled backfaces, inert content, modal exclusions and elements excluded by computed pointer targeting.

`css_pointer_targetable()` in `c/apps/gui/browser/css/style.h` reads the candidate element’s computed pointer value. This permits a descendant to override an inherited exclusion. Inert ancestry is checked through `c/apps/gui/browser/dom/focus.c`.

Layout supplies targeting geometry even where a box has no background or border. Such boxes receive transparent `IT_RECT` items through `c/apps/gui/browser/layout/block.c`. Inline whitespace receives `IT_HIT` items for its individual advances, preserving occupied regions across wrapped lines. The painter skips `IT_HIT` items.

A successful native hit returns an event target and, when applicable, a navigation URL. Text items resolve to their nearest element. A child inside an anchor can receive the event while the anchor supplies navigation. URL fallback follows the winning target’s ancestors, keeping an overlay’s action associated with that overlay. A modal backdrop hit targets the dialog.

`browser_input_item_geometry()` exposes projected item geometry to selection and control positioning in `c/apps/gui/browser/app/main.c`. Its coordinates follow the callers’ viewport-plus-page-scroll convention, including content fixed to the viewport.

JavaScript point queries use the traversal in `c/apps/gui/browser/js/style/cssom.c`. `document.elementFromPoint()` and `document.elementsFromPoint()` flush layout, visit top layers and display items in reverse order, and apply element scrolling and rectangular clipping. The plural query collects elements and ancestors, using a bounded table to suppress duplicates. Queries over blank viewport space fall back to the modal dialog, body or root element as applicable.

## Canvas and matrix bindings

`js_canvas_install()` in `c/apps/gui/browser/js/graphics/canvas.c` installs canvas methods on `HTMLCanvasElement.prototype` and registers the context methods. Repeated requests for a canvas’s 2D context return the same context object. That context owns its straight RGBA backing store, drawing state, current path, gradients and saved states.

Layout reserves an `IT_CANVAS` box. At paint time, `canvas_pixels()` finds the backing store by DOM node, and the renderer blits it into the layout destination. The bitmap dimensions come from canvas attributes; CSS determines the displayed box. Changing the canvas dimensions reallocates and clears the backing store and resets drawing state.

Path points receive the current transform when they are added. Later transform changes affect subsequent points. Saved drawing state includes the transform, paint selection, stroke parameters, text settings and clipping mask. Saved clips share buffers through reference counts; a new clipping operation creates a mask and intersects it with the previous clip.

Canvas text uses shaping to produce glyph outlines, which are painted into the backing store. `measureText()` obtains width from the matching shaping path. `drawImage()` accepts canvas elements and decoded HTML image elements, with source cropping and destination transformation. Pixel access methods copy between image data and the backing store.

`isPointInPath()` tests winding or crossing counts over flattened path geometry, including points on edges. `isPointInStroke()` tests the generated stroke outline. The supplied point is compared directly with stored path coordinates, and queries cover geometry beyond the bitmap bounds.

The file retains historical comments saying canvas export throws. The current `toDataURL()` and `toBlob()` implementations encode backing pixels as PNG and use PNG for unsupported requested formats. `toBlob()` encodes during the call and delivers its result through a queued callback.

Matrix bindings live in `c/apps/gui/browser/js/graphics/matrix.inc`, included by `c/apps/gui/browser/js/style/cssom.c`. `DOMMatrix` and `DOMMatrixReadOnly` support construction from transform strings or iterables of six or sixteen values, matrix fields, identity and dimensionality flags, and string serialisation. Computed CSS transform strings use the shared transform evaluator and resolve percentages against an available element layout box.

## Repaint damage and diagnostics

The renderer draws the viewport and records an item snapshot during traversal. It then compares the snapshot with the preceding pass. Records contain node identity, window geometry and an appearance signature.

The signature includes item data, current form control paint state, style span identities and cached background image identities. A background image becoming available can produce damage while its authored declaration stays the same. Changed records contribute both old and new extents, covering movement and size changes; added or removed records contribute their own extents.

`browser_paint_dirty_rect()` returns the result for the completed pass:

| Result | Meaning |
|---|---|
| `-1` | A full flush is required |
| `0` | Computed damage is empty |
| `1` | Output arguments contain viewport damage in window coordinates |

The page redraw path in `c/apps/gui/browser/app/main.c` uses this result to choose a full flush, skip publication or call `gui_flush_rect()`. The kernel records the reported window damage for compositing.

The renderer also exposes counters for shadow, transform, rounded clip and box coverage fallbacks. These counters and the emitted text record help identify which paint paths a page reached.

## Limitations

- Transform painting uses an affine projection. Text and images under rotation or skew remain level at their transformed bounds. The transformed rectangle path paints one solid shape and omits combined decoration such as gradients and shadows; masks exceeding 128 device pixels on either axis fall back to drawing at the transformed bounds. Perspective and depth sorting are outside this path.
- Native input projection follows pure translations. Scale, rotation, transformed overflow clips, rounded outlines and bitmap alpha lack matching hit geometry. CSSOM point queries omit the native traversal’s transform, backface and inert checks; their duplicate table holds 128 elements.
- Fractional subtree opacity lacks offscreen group composition. Text and image opacity use an estimated backdrop. Blurred inset shadows are skipped, and large outer shadow blurs can be reduced to fit coverage bounds. These paths are documented in `c/apps/gui/browser/paint/renderer.c`.
- The SVG decoder skips text, external resources and stroke dash arrays. Inline SVG contributes its outer image box to browser hit testing.
- Canvas backing stores are capped at 4,194,304 pixels; paths have 4,096 point slots and the save stack has 32 entries. Image scaling uses nearest neighbour sampling, radial gradients use the end circle, and stroke widths under nonuniform transforms use an approximate uniform scale. General composite modes and `Path2D` are absent. Letter spacing for native document text uses scalar boundaries, with shaping cluster spacing still unimplemented.
- Matrix bindings provide construction, fields, flags and serialisation; arithmetic and point transformation methods remain absent. Paint signatures omit changing canvas and video pixel contents, leaving an invalidation gap documented in `c/apps/gui/browser/paint/renderer.c`.

## See also

- [Layout: block, flex, grid and text](layout.md)
- [CSS: parsing, cascade and computed style](css.md)
- [DOM and event bindings](dom-bindings.md)
- [Frames, documents and isolation](frames-and-isolation.md)
- [Graphics and windowing](../architecture/graphics-and-windowing.md)
- [Testing the browser](testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/layout/block.h` | Display item types, geometry and layout interfaces |
| `c/apps/gui/browser/layout/block.c` | Item production, transparent hit boxes, images and inline SVG rasterisation |
| `c/apps/gui/browser/layout/stacking.inc` | Stacking contexts, paint groups and display list ordering |
| `c/apps/gui/browser/layout/text.c` | Shared text spacing segmentation and measurement |
| `c/apps/gui/browser/paint/renderer.h` | Painting, input geometry, hit testing and diagnostic interfaces |
| `c/apps/gui/browser/paint/renderer.c` | Paint traversal, transforms, clipping, effects, native targeting and damage snapshots |
| `c/apps/gui/browser/paint/text.inc` | Native text emission and matching prefix advances |
| `c/apps/gui/browser/paint/background.c` | Background image geometry, cached pixels and sampling |
| `c/apps/gui/browser/paint/gradient.c` | Gradient geometry, stops and sampling |
| `c/apps/gui/browser/paint/box.c` | Rounded coverage, background composition and solid borders |
| `c/apps/gui/browser/paint/element_scroll.inc` | Element scroll projection and overflow clip reconstruction |
| `c/apps/gui/browser/paint/backface.inc` | Facing calculations used by native painting and targeting |
| `c/apps/gui/browser/paint/svg_dom_paint.inc` | Live SVG serialisation with resolved paint properties |
| `c/apps/gui/browser/paint/svg_reflow_cache.inc` | SVG raster keys, borrowing and eviction |
| `c/apps/gui/browser/js/graphics/canvas.c` | Canvas state, drawing, geometry queries and export |
| `c/apps/gui/browser/js/graphics/matrix.inc` | Matrix objects and computed transform serialisation |
| `c/apps/gui/browser/js/style/cssom.c` | JavaScript geometry and point queries |
| `c/apps/gui/browser/css/style.h` | Computed style helpers for fixed positioning and pointer targeting |
| `c/apps/gui/browser/dom/forms.h` | Shared native text chunking and measurement helpers |
| `c/apps/gui/browser/dom/focus.c` | Inert ancestry checks used by native targeting |
| `c/apps/gui/browser/dom/top_layer.h` | Shared dialog and popover layer interface |
| `c/apps/gui/browser/dom/top_layer.inc` | Top layer ordering, ownership and modal input filtering |
| `c/apps/gui/browser/app/main.c` | Viewport painting, native input consumers and flush selection |
| `c/apps/logit.h` | Application drawing and window publication wrappers |
| `c/kernel/gui/wm.c` | Drawing request validation, window surfaces and damage reporting |
| `c/kernel/gui/text.c` | Native text measurement, shaping and drawing |
| `c/lib/image/img.h` | Decoded image representation and PNG encoding interface |
| `c/lib/image/svg.c` | SVG decoding and path construction |
| `c/lib/image/svg_scene.inc` | SVG scene traversal, transforms, references and clipping |