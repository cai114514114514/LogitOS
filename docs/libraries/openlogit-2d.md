# OpenLogit 2D: geometry, rasterization and canvas

OpenLogit supplies the software 2D rendering core used by LogitOS applications and the kernel compositor. Its geometry builders produce flattened paths, its rasterizer converts those paths into coverage, and its painters composite colors and images through that coverage. Applications can draw into borrowed pixel buffers or record a command list for submission into an offscreen surface. The implementation lives under `c/lib/gfx/`.

## Rendering interfaces

The public headers organize the rendering pipeline into geometry, immediate drawing, resources and canvas recording:

| Header | Role |
|---|---|
| `c/lib/gfx/include/gfx.h` | Geometry, fixed point math, paints, masks and composition operators. |
| `c/lib/gfx/include/openlogit_draw.h` | Immediate raster operations on borrowed RGBA buffers and coverage buffers. |
| `c/lib/gfx/include/openlogit.h` | Devices, surfaces, image resources, command lists and submission. |
| `c/lib/gfx/include/openlogit_canvas.h` | Saved drawing state, transforms, clips, fills, strokes, sprites and groups. |
| `c/lib/gfx/include/openlogit_display.h` | Drawing into packed display pixels. |

A canvas records commands into a list; submission executes them through the software backend. Immediate raster calls reach the same coverage implementation through `c/lib/gfx/adapters/openlogit_draw.c`. Older entry points such as `gfx_fill` forward through that adapter in `c/lib/gfx/adapters/openlogit_compat.c`.

Geometry and coverage also serve other libraries. `c/lib/text/glyphras.c` converts font outlines into paths and requests coverage through `ol_raster_mask_subs`. The clock application records painted paths into surfaces in `c/apps/gui/clock/dial.c`.

## Surfaces and pixel formats

`struct gfx_surface` describes borrowed memory with a pixel pointer, width, height and byte stride. Each pixel contains R, G, B and A bytes in that order. Alpha is straight: RGB channels store color without multiplication by alpha. The format matches the application GUI blit interface. See `c/lib/gfx/include/gfx_raster.h`.

The resource API adds lifecycle and generation tracking around storage owned by the caller:

| Representation | Storage and purpose |
|---|---|
| `struct gfx_surface` | A view over RGBA pixels for immediate raster operations. |
| `struct ol_surface` | A render target with front and work buffers and a generation counter. |
| `struct ol_image` | A source resource containing RGBA pixels or A8 coverage. |
| `struct ol_pixel_target` | Packed opaque display pixels, with channel positions supplied through the display binding. |

`ol_surface_create` accepts `OL_FORMAT_RGBA8_STRAIGHT` and validates the supplied front and work buffers, including their disjoint memory ranges. The front buffer contains the published image; submission renders into work memory and copies successful output back to front. Copies cover pixel bytes within each row, preserving row padding. Surface creation retains the supplied initial front contents; callers can initialize an image with `ol_surface_upload` or record a full `ol_cmd_clear`. These operations are implemented in `c/lib/gfx/core/ol_device.c` and `c/lib/gfx/core/ol_exec.c`.

`ol_image_create` accepts `OL_FORMAT_RGBA8_STRAIGHT` and `OL_FORMAT_A8`. RGBA images permit padded rows. A8 images contain one coverage byte per pixel and require stride equal to width. `ol_image_update` changes a rectangular region and advances the generation used by recorded resource references. See `c/lib/gfx/core/ol_image.c`.

Packed display targets use pixels of 32 bits and a row width measured in pixels. The display binding supplies the red, green and blue bit positions. `c/kernel/gui/fb/fb.c` binds the current drawing target and framebuffer channel layout through this interface.

## Coordinates and path construction

Geometry uses integer fixed point arithmetic:

| Value | Representation |
|---|---|
| Path coordinates and matrix translations | 24.8, with 256 units per pixel. |
| Matrix linear coefficients | 16.16, with 65536 representing one. |
| `struct gfx_rect` coordinates and dimensions | Whole pixels. |
| Gradient stop positions | 0 through 65536 across the ramp. |
| Arc and rotation angles | 24.8 degrees. |

`GFX_PX` converts whole pixels into path coordinates. `struct gfx_matrix` holds the six coefficients of an affine transform. `gfx_m_mul` applies its right operand first and supports writing the result into either input object. The declarations and implementation are in `c/lib/gfx/include/gfx_math.h` and `c/lib/gfx/geometry/gfx_math.c`.

A `struct gfx_path` uses arrays provided by the caller for interleaved point coordinates and subpath starts. Builders support moves, lines, quadratic curves, cubic curves and closure. Rectangle, rounded rectangle, ellipse and circle helpers use those same builders.

The path matrix transforms endpoints and control points as they arrive. Curves then undergo recursive subdivision against a tolerance measured in device pixels, initially 1/32 pixel. This order makes the subdivision respond to the rendered size of a curve, so an enlarged curve is flattened after enlargement. See `c/lib/gfx/geometry/gfx_path.c`.

Set the path matrix before adding points. Exhausting point or subpath storage, or setting the matrix after points have been stored, latches an error. `gfx_path_status` reports the first recorded cause. Rasterization checks the path’s error flag before sweeping its edges, preventing a partial contour from being filled across missing geometry.

Additional shape builders in `c/lib/gfx/geometry/gfx_arc.c` include elliptical arcs and SVG endpoint arcs. Arcs become cubic pieces spanning at most 90 degrees, which then pass through the path’s transform and flattening machinery.

## Stroke construction

`gfx_stroke_path` expands a flattened path into a fillable outline. `struct gfx_stroke` supplies the full width, cap and join styles, miter limit, dash lengths and dash phase. Widths and dash lengths use 24.8 coordinates; the miter limit uses 16.16. See `c/lib/gfx/include/gfx_stroke.h`.

Closed contours produce two contours with opposite winding. Filling them with `GFX_NONZERO` paints the region between them. Open contours connect the two sides through butt, round or square caps. Joins support miter, round and bevel geometry, with miter joins falling back to bevel when the limit is exceeded. Dash runs receive their own end caps. The construction is implemented in `c/lib/gfx/geometry/gfx_stroke.c`.

Fills close subpaths implicitly. `gfx_close` also records a closure bit for the stroker, allowing it to join the closing edge. A repeated first point at the end of a contour also supplies closure information.

`ol_cmd_stroke` builds the outline in a scratch path provided by the caller and records it as a nonzero fill. Recording copies the outline coordinates, so callers can reuse the scratch path afterward. See `c/lib/gfx/core/ol_record.c`.

## Rasterization and antialiasing

The scan converter in `c/lib/gfx/raster/gfx_raster.c` builds edges from path contours, sorts them by their upper endpoints and maintains an active edge list while sweeping downward.

For each vertical sample, it computes crossings in horizontal order. The winding walk supports `GFX_NONZERO` and `GFX_EVENODD`. Edges include their upper endpoint and exclude their lower endpoint, preventing a shared vertex from contributing twice.

Antialiasing combines vertical sampling with fractional horizontal span coverage. Each sample contributes the covered length within each pixel. A wider accumulator sums these lengths before the finished row is converted into coverage values from 0 to 255. Delaying quantization avoids repeatedly truncating the contribution of each vertical sample.

The default `GFX_SUBS` is four samples per pixel row. Raster entry points accept a sample count bounded by `GFX_MAX_SUBS`, currently 32; command recording validates that count. The font outline adapter requests 16 through `GR_SUBS` in `c/lib/text/glyphras.c`. The comments in `c/lib/gfx/include/gfx_raster.h` explain the different cost and accuracy choices for frequently drawn shapes and cached glyphs.

A completed row goes to either a writer for a coverage buffer or a paint compositor. Both consumers share the edge sweep.

Capacity of the edge table is checked during construction. When the total edge count exceeds the capacity of the active list, a preliminary sweep checks the active and crossing lists before coverage rows are emitted. Smaller paths proceed directly because their total edge count already bounds both lists.

## Paints, gradients and composition

`struct gfx_paint` describes the source evaluated through coverage. Constructors provide solid colors, linear gradients, radial gradients, conic gradients, radial gradients defined by two circles and image paints. The constructors and row dispatch live in `c/lib/gfx/raster/gfx_paint.c`.

Gradient stops hold a position, RGB color and alpha. `gfx_paint_stop` inserts stops in position order. Between stops with different alpha, `gfx_paint_sample` weights colors by alpha and converts the result back to straight color. A transparent stop then adds no hue to the visible part of the transition. See `c/lib/gfx/raster/gfx_gradient.c`.

Linear and radial ramps support endpoint padding, repetition and reflection. A conic gradient maps one turn to its ramp; padding and repetition use the same angular mapping, while reflection runs forward over the first half turn and backward over the second.

A gradient paint matrix maps paint coordinates into device space. Sampling sends device pixel centers through its inverse. Optional dithering applies a 4×4 Bayer pattern when quantizing color channels. Linear gradients without a paint matrix or dithering advance their ramp parameter incrementally across a row.

The general comment in `c/lib/gfx/include/openlogit_canvas.h` describes paint coordinates as device coordinates. The implementation of gradient fills in `c/lib/gfx/canvas/state.c` now composes the canvas transform with the gradient’s paint matrix through `paint_in_ctm`, making the gradient follow the filled geometry.

Image paints carry an inverse transform from device coordinates into image coordinates. Sampling supports nearest and bilinear filtering, plus repeat, reflect, edge clamping and an optional box filter for reduction. Sprite recording selects filtering that clamps to the source crop, keeping neighboring atlas texels outside its filter footprint. See `c/lib/gfx/raster/gfx_image_sample.c` and `c/lib/gfx/core/ol_record.c`.

For a recorded `GFX_IMAGE` fill, the paint’s `img_ref` must identify a live RGBA `ol_image` on the list’s device, and the sampled pixel range must lie within that resource. Recording retains the resource and its generation. Sprite recorders arrange resource retention themselves.

The default composition operator is source over. `gfx_over` combines source alpha with coverage and computes the resulting straight RGBA pixel. Other operators include source, clear, destination masking, XOR, additive composition, multiply and screen. See `c/lib/gfx/include/gfx_blend.h` and `c/lib/gfx/raster/gfx_compose.c`.

## Clipping

Rectangle clipping intersects the surface and path bounds before the sweep, reducing the rows and columns processed.

Path clipping uses a prepared A8 coverage mask. Subject coverage and clip coverage are multiplied at the same device coordinate, with rounding:

```c
(s * c + 127) / 255
```

Pixels outside the mask extent receive zero clip coverage. Preparing the clip before sweeping the subject gives the row consumer a completed coverage input. The mask structure is declared in `c/lib/gfx/include/gfx_raster.h`; the multiplication is implemented in `c/lib/gfx/raster/gfx_raster.c`.

The canvas keeps a rectangle clip and an optional coverage image. `ol_canvas_clip` intersects a rectangle in device coordinates. `ol_canvas_clip_rrect` builds its geometry through the current canvas transform. `ol_canvas_clip_path` transforms the supplied path’s stored points through that transform. Both rasterize into arena storage provided by the caller and multiply the result with the existing coverage mask. See `c/lib/gfx/canvas/clip.c`.

Recorded fills retain references to those masks. Restore reclaims arena space when the position in the command list still matches the position recorded at allocation. Otherwise, the storage remains available to recorded commands until list reset; the caller can then reinitialize the clip arena.

## Canvas recording and submission

`struct ol_canvas` holds the transform, rectangle clip, coverage mask, opacity and sample count. Save and restore copy that state through a stack. The embedded stack has 16 entries, and `ol_canvas_init_ex` accepts stack storage provided by the caller. `ol_canvas_set_samples` changes the sample count saved with the state.

For a transformed path fill, call `ol_canvas_path_begin` on an empty path before building it. This installs the current canvas transform as the path matrix and tightens the existing flattening tolerance when its estimated scale exceeds one. `ol_canvas_fill` recognizes the prepared path while its matrix still matches the canvas transform and records its geometry with an identity transform, applying the transform once.

For `ol_canvas_stroke` and `ol_canvas_clip_path`, build the input path with an identity path matrix so its stored points stay in canvas user coordinates. The stroke wrapper builds an outline before applying the canvas transform during recording; the clip wrapper transforms the stored points when preparing its coverage. These routes are implemented in `c/lib/gfx/canvas/state.c` and `c/lib/gfx/canvas/clip.c`.

Canvas images and sprites inherit the current transform, rectangle clip and opacity. `ol_cmd_nine_slice` divides an image into corner, edge and center regions, preserving border pixel sizes as the destination changes. See `c/lib/gfx/canvas/sprites.c`.

`ol_canvas_save_layer` redirects recording into a nested list backed by a group surface in caller scratch storage. Restore submits the group and records one layer command on the parent list. Group opacity is applied after the children have been composed, giving overlapping children a shared opacity. Inside the group, device coordinates are local to its clipped bounds; the adjusted canvas transform carries user coordinates into that space.

A frame using this API follows this sequence:

1. Reset the command list and initialize the canvas.
2. Build paths and record drawing operations.
3. Close the list and submit it into a surface.
4. Composite the published surface into the application window.
5. Present the window after completing its drawing.

The clock’s cached dial and moving hands demonstrate this flow in `c/apps/gui/clock/dial.c`. Window transport helpers live in `c/apps/gui/openlogit_window.h`: `ol_window_composite` accepts packed RGBA rows, while `ol_window_composite_region` packs a selected rectangle into caller scratch. `ol_window_present` schedules the window for composition.

Command recording copies path coordinates and paint values and retains referenced resources with their generations. Submission checks those generations, executes into work pixels and publishes the accumulated damage rectangle after all commands succeed. `ol_submit_damage` also returns damage and copy statistics. Recording errors latch until list reset. See `c/lib/gfx/core/ol_record.c` and `c/lib/gfx/core/ol_exec.c`.

## Kernel and user-space integration

`Makefile` includes `c/lib/gfx/` in the kernel source discovery and discovers its sources recursively for application graphics objects. The same geometry, rasterization and paint implementation serves both environments.

In the kernel, `c/kernel/gui/fb/fb.c` adapts framebuffer operations to `struct ol_display`. Rounded display primitives use cached corner coverage, and `c/kernel/gui/wm.c` builds and rasterizes paths for cursor artwork. Corner masks are cached by device geometry in `c/lib/gfx/raster/gfx_mask.c`, so their coverage is generated at the destination pixel size.

The compositor renders bands using drawing contexts containing target, clip, display scratch and raster workspace state. `c/lib/gfx/include/gfx_ctx.h` retains an older description of selection by core index. The installed hook in `c/kernel/gui/fb/fb.c` selects registered worker threads, preserving their drawing context when scheduling moves them between CPUs.

Command devices carry raster workspaces in device storage provided by the caller. Submission acquires the device’s busy flag; overlapping submissions on that device return `OL_BUSY`. Different devices use their own workspaces.

## Limitations

- The device backend executes synchronously in software. Completion serials identify successful surface commits and report software execution only. The objects are C objects within an address space.
- Surfaces and images are bounded to 4096 pixels per dimension. Rasterization supports 8192 edges, 1024 active edges and a 4096-pixel coverage row. Clip masks are bounded to 2048 pixels per dimension; packed display targets have an 8192-pixel dimension limit.
- Gradients hold at most 16 stops; additional `gfx_paint_stop` calls add nothing. The raster painter supports `GFX_RADIAL2`, but validation of command lists rejects paint kinds above `GFX_CONIC`.
- Canvas image and sprite calls return `OL_UNSUPPORTED` with an active coverage mask. Canvas group creation accepts `GFX_OP_SRC_OVER`.
- Curve subdivision stops at a recursion depth of ten, so the requested path tolerance is not an unconditional error bound.
- Callers must preserve object, pixel and arena storage for the required lifetimes. Immediate raster calls sharing a drawing context require serialized use.

## Source map

| File | What it contains |
|---|---|
| `c/lib/gfx/include/gfx.h` | Geometry and raster API umbrella. |
| `c/lib/gfx/include/gfx_math.h` | Fixed point units, rectangles and affine matrices. |
| `c/lib/gfx/include/gfx_path.h` | Path storage, builders and error contracts. |
| `c/lib/gfx/include/gfx_stroke.h` | Stroke widths, caps, joins and dash descriptors. |
| `c/lib/gfx/include/gfx_raster.h` | RGBA surfaces, coverage, fill rules, clipping and raster limits. |
| `c/lib/gfx/include/gfx_paint.h` | Paint descriptors, gradient stops and sampling options. |
| `c/lib/gfx/include/gfx_blend.h` | Composition operators and blend modes. |
| `c/lib/gfx/include/openlogit.h` | Devices, resources, command recording and submission. |
| `c/lib/gfx/include/openlogit_draw.h` | Immediate drawing into borrowed buffers. |
| `c/lib/gfx/include/openlogit_canvas.h` | Canvas state, drawing, clip arenas and groups. |
| `c/lib/gfx/include/openlogit_display.h` | Packed display targets and display operations. |
| `c/lib/gfx/include/gfx_ctx.h` | Selection interface for drawing contexts. |
| `c/lib/gfx/geometry/gfx_math.c` | Fixed point arithmetic and matrix operations. |
| `c/lib/gfx/geometry/gfx_path.c` | Path construction and curve flattening. |
| `c/lib/gfx/geometry/gfx_arc.c` | Arcs and additional shape builders. |
| `c/lib/gfx/geometry/gfx_stroke.c` | Stroke outlines, joins, caps and dashing. |
| `c/lib/gfx/raster/gfx_raster.c` | Edge sweep, antialiasing, capacity checks and clipping. |
| `c/lib/gfx/raster/gfx_paint.c` | Paint construction, row dispatch and source over composition. |
| `c/lib/gfx/raster/gfx_gradient.c` | Gradient sampling, spread modes, transforms and dithering. |
| `c/lib/gfx/raster/gfx_image_sample.c` | Image coordinate mapping and filtering. |
| `c/lib/gfx/raster/gfx_compose.c` | Composition operators and blend calculations. |
| `c/lib/gfx/raster/gfx_mask.c` | Corner coverage generation and caching. |
| `c/lib/gfx/core/ol_internal.h` | Private resource layouts, bounds and device locking. |
| `c/lib/gfx/core/ol_device.c` | Device capabilities and surface lifecycle. |
| `c/lib/gfx/core/ol_image.c` | Image validation, updates and generations. |
| `c/lib/gfx/core/ol_record.c` | Command storage, validation and retained resources. |
| `c/lib/gfx/core/ol_exec.c` | Execution, damage tracking and frame publication. |
| `c/lib/gfx/canvas/state.c` | Saved state, transforms, fills, strokes and groups. |
| `c/lib/gfx/canvas/clip.c` | Canvas clip masks and arena lifetime. |
| `c/lib/gfx/canvas/sprites.c` | Canvas images, sprites and recording of nine slice images. |
| `c/lib/gfx/adapters/openlogit_draw.c` | Immediate raster adapter. |
| `c/lib/gfx/adapters/openlogit_compat.c` | Forwarding implementations for older raster entry points. |
| `c/lib/gfx/adapters/openlogit_display.c` | Packed display drawing and binding validation. |
| `c/kernel/gui/fb/fb.c` | Kernel display adapter and registration of drawing contexts. |
| `c/kernel/gui/wm.c` | Compositor workers and cursor rendering. |
| `c/lib/text/glyphras.c` | Font outline conversion and coverage requests. |
| `c/apps/gui/clock/dial.c` | Application surface and canvas usage. |
| `c/apps/gui/openlogit_window.h` | Surface transport into application windows. |
| `Makefile` | Kernel and application graphics source selection. |

## See also

- [Graphics and windowing](../architecture/graphics-and-windowing.md)
- [Compositing and effects](../kernel/compositing-and-effects.md)
