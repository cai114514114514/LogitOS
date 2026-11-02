# OpenLogit effects, animation and SIMD kernels

OpenLogit provides software blur and glass effects, animation sampling, retained surface composition, UI interaction helpers and pixel kernels. Applications compose straight RGBA surfaces through recorded commands; the kernel compositor draws through adapters for packed display pixels. Callers supply storage, timestamps and presentation policy. The implementation lives under `c/lib/gfx/`.

## Rendering paths and ownership

Effects use two interfaces with different storage contracts:

| Interface | Pixel storage | Execution and temporary storage |
|---|---|---|
| Recorded layer and backdrop commands | Straight RGBA surfaces | Commands render into the destination’s work buffer, using effect workspace supplied by the caller. Successful submission copies output damage to the published buffer. |
| Display primitives and glass materials | Packed 32-bit pixels with configured channel positions | Drawing writes a borrowed target. Allocation callbacks provide temporary buffers retained by the display context. |

The recorded interface includes `ol_cmd_layer()`, `ol_cmd_backdrop()` and `ol_cmd_backdrop_rrect()`. Submission validates referenced resources and their generations before execution. The display interface uses `struct ol_display` to hold the target, channel mapping, clip and reusable scratch. These contracts are defined in `c/lib/gfx/include/openlogit.h` and `c/lib/gfx/include/openlogit_display.h`.

RGBA surfaces support transparent images and groups assembled by applications. Packed targets let the compositor use the framebuffer’s channel layout directly. `c/kernel/gui/fb/fb.c` maintains a display instance in each drawing context, giving concurrent compositor workers their own effect scratch.

## Layer blur, shadows and backdrops

`ol_cmd_layer()` records a source surface, destination rectangle and `struct ol_layer_options`. Options control opacity, filtering, blur, shadow colour, shadow opacity, shadow offset and an output clip. Execution draws the shadow before the filtered source. Composing overlapping children into a surface first lets the caller apply group opacity once. Recording and execution are implemented in `c/lib/gfx/core/ol_record.c` and `c/lib/gfx/effects/openlogit_effects.c`.

The RGBA blur accumulates colour multiplied by alpha, together with alpha itself, then converts the result back to straight colour. This keeps hidden RGB values in transparent texels out of the blurred colour. Sliding vertical sums and horizontal sums reuse neighbourhood totals. Each scratch column occupies 16 bytes; the required column count includes the blur margin.

`ol_cmd_backdrop()` filters the destination accumulated by earlier commands, then applies a tint. It snapshots the input region before filtering so every output pixel reads the same input image. Submission initialises the backdrop’s read halo from the published surface as part of preparing the work buffer. The publication rectangle describes output damage. This ordering is implemented in `c/lib/gfx/core/ol_exec.c`.

`ol_cmd_backdrop_rrect()` adds four corner radii and a choice of one or three box passes. Three passes use a radius of `ceil(blur_radius / 3)` per pass, giving a total reach of three times that value. Two snapshot buffers alternate between passes. Rounded coverage blends the filtered and tinted result at the outline; boundary pixels integrate coverage across 32 subcolumns. See `c/lib/gfx/effects/openlogit_effects.c`.

After closing the list, query `ol_submit_workspace_size()` and provide disjoint storage aligned to eight bytes to `ol_submit_workspace()`. Workspace sizing takes the largest command requirement so commands can reuse the same memory. Submission checks overlap with destination buffers, command storage and referenced resources. It copies rendered damage to the published buffer after the entire list succeeds.

## Glass materials

`ol_display_liquid_glass_ex()` draws glass onto a packed display target. `struct ol_glass_material` selects tint, tint opacity, blur tier, rim style and tint adaptation. The implementation divides preparation, filtering and rendering across these files:

| Stage | Implementation |
|---|---|
| Prepare panel geometry, sampling bounds, scratch and timing | `c/lib/gfx/effects/openlogit_material.c` |
| Copy or blur the backdrop | `c/lib/gfx/effects/glass_blur.c` |
| Calculate edge distance, normals and coverage | `c/lib/gfx/effects/glass_geometry.h` |
| Sample displaced channels and render the panel | `c/lib/gfx/effects/glass_render.c` |
| Calculate lighting and adaptive tint | `c/lib/gfx/effects/glass_shading.h` |
| Build refraction and reflectance tables | `c/lib/gfx/effects/openlogit_glass.c` |

For nonzero tint opacity, the selected tier uses these box radii along each axis:

| Tier | Pass radii | Total blur support |
|---|---|---|
| `OL_GLASS_BLUR_REGULAR` | 6 | 6 pixels |
| `OL_GLASS_BLUR_THIN` | 2, 2, 3 | 7 pixels |
| `OL_GLASS_BLUR_THICK` | 11, 11, 11 | 33 pixels |
| `OL_GLASS_BLUR_ULTRA` | 18, 18, 19 | 55 pixels |

The repeated passes approximate Gaussian blur. During passes that update scratch in place, small rings retain source pixels until they leave the moving window. The table and its variance rationale are in `c/lib/gfx/effects/glass_blur.c`.

Zero tint opacity selects Clear sampling: the backdrop is copied into optical scratch, preserving detail for refraction and reflection. Setting the display’s `reduce_transparency` field draws solid tint using the panel’s coverage geometry. That branch renders before optical scratch allocation in `c/lib/gfx/effects/openlogit_material.c`.

Refraction follows a circular bevel model derived from Snell’s law. Lookup tables contain displacement for red, green and blue, plus Fresnel reflectance indexed by inward edge distance. The refractive indices are 1.44, 1.50 and 1.56. Comments in `c/lib/gfx/effects/openlogit_glass.c` identify this dispersion as an artistic exaggeration and preserve the explanation of the earlier model that used a squared ramp. `GLASS_FRES_MAX`, defined in `c/lib/gfx/include/openlogit_glass.h`, caps reflectance at 190.

`glass_lut_get()` caches tables by edge width for the renderer’s canonical refraction amount. A completed table is published atomically and remains immutable. A caller finding a table under construction computes into its own storage, allowing compositor workers to continue without waiting for that table.

The directional rim uses the surface normal for lighting. The symmetric rim brightens according to edge distance and adjusts reflection strength using backdrop variance. Tint adaptation supports the default threshold rule, a continuous strength ramp through `OL_GLASS_ADAPT_SMOOTH`, and foreground luminance through `OL_GLASS_ADAPT_FG`. The material interface is in `c/lib/gfx/include/openlogit_glass_material.h`.

## Glass sampling, damage and profiling

Glass reads a neighbourhood beyond its visible outline. `ol_display_glass_sample_halo_ex()` adds the material’s blur support to a displacement allowance covering refraction and neighbouring reflection. The renderer samples this halo even when a damage clip restricts its writes. Reconstructing that backdrop neighbourhood prevents a redraw from sampling glass left by the previous frame. See `c/lib/gfx/effects/openlogit_material.c` and the damage expansion in `c/kernel/gui/wm.c`.

Cropping at the target boundary retains the original panel centre and edges. This keeps the lens attached to the moving shape. Radius zero selects square coverage, and the cut flags in `c/lib/gfx/include/openlogit_display.h` suppress the corresponding optical edges.

Scratch belongs to the display context. The glass renderer retains its sample buffer and geometry workspace there; other display primitives use reusable slots through `display_scratch()` in `c/lib/gfx/adapters/display_pixels.h`. `ol_display_destroy()` releases these allocations.

`gfx_set_clock()` installs a nanosecond clock for profiling. The window manager supplies `time_mono_ns()` in `c/kernel/gui/wm.c`. Three clock reads bracket backdrop copying or blur and final rendering. Atomic counters accumulate calls, pixels of the visible panel’s bounding box and stage durations. Pixel counts exclude the sampling halo and keep the bounding box as the unit under rounded coverage or a write clip. Reporting reads each counter atomically. The instrumentation is implemented in `c/lib/gfx/simd/gfx_simd.c`.

## Animation and timing

Animation uses a monotonic timestamp supplied by the caller. `struct ol_timeline` stores an anchor timestamp, local position, playback rate and pause state. Sampling derives local time from that anchor, so a delayed frame advances directly to the corresponding value. The interface is in `c/lib/gfx/include/openlogit_anim.h`; implementation is in `c/lib/gfx/animation/openlogit_anim.c`.

A descriptor supplies immutable keyframes owned by the caller, duration, signed delay, iteration count and direction. An iteration count of zero represents endless playback. Keyframe offsets begin at zero and end at one. Sampling locates the active segment, applies the left keyframe’s easing and interpolates the values. Quaternion mixing chooses the endpoint sign from the dot product, interpolates components and normalises the result.

Pause and rate changes capture the current position before updating the anchor. Seeking sets the anchor position directly. Sampling with reduced motion selects an endpoint using the iteration and direction rules.

Each sample reports values, progress, activity, completion and a suggested next timestamp. Active samples suggest another wake after 16,666,667 nanoseconds; a positive delay can shorten that interval near its end. The caller integrates this suggestion with its event loop. `examples/openlogit/vector/main.c` shares one timestamp across interaction updates in each loop and chooses an idle wait from animation activity.

Two spring interfaces support different timing models:

| Interface | Model |
|---|---|
| `ol_spring_sample()` | Evaluates an analytic damped spring at an absolute timestamp, with underdamped, critical and overdamped cases. At the configured duration it returns the target with zero velocity. |
| `ol_spring256_step()` | Advances integer state by elapsed microseconds, using substeps of at most four milliseconds. Position and velocity retain additional fractional remainders and settle against distance and velocity thresholds. |

Both retarget operations carry the current position and velocity into the new motion. Their implementations are in `c/lib/gfx/animation/openlogit_anim.c` and `c/lib/gfx/animation/openlogit_motion.c`.

`ol_ease256_ex()` accepts progress scaled to 256 and provides cubic, quartic, exponential, back, Bézier and step curves. Bézier inversion uses a safeguarded Newton bracket in `c/lib/gfx/include/openlogit_easing.h`. The motion helpers also interpolate rectangles and mix straight ARGB colours through premultiplied colour.

## The scene layer

`struct ol_scene2d` retains surface references, placement and the last successfully rendered state. Layer index defines paint order. Each layer supplies bounds, opacity and visibility, plus translation in 24.8 pixels, uniform scale in 8.8 units and an effect extent. This supports rendering expensive content into surfaces and animating their placement. See `c/lib/gfx/include/openlogit_scene2d.h`.

`ol_scene2d_render()` compares current layers and surface generations with the previous frame. A changed layer contributes its old and new bounds, rounded outward and expanded by its effect extent. Overlapping damage rectangles merge. Nearby rectangles merge when their union adds less than a quarter of their combined area. If the set exceeds capacity, further merges choose the pair with the least added area.

For each damage rectangle, rendering records a clear, restores the background and composites overlapping layers in paint order. Translation by whole pixels at unit scale uses a layer command. Fractional translation or scaling creates a borrowed image header over the surface pixels and records a sprite with bilinear sampling.

After successful submission, the scene updates its remembered placements and generations. An unchanged frame reports zero commands. The scene exposes its damage rectangles, while submission reports their enclosing output damage rectangle. These mechanisms are in `c/lib/gfx/scene/scene2d.c`.

`ol_scene2d_move()` changes paint order. `ol_scene2d_hit()` searches from the top using transformed bounds and pixel centres. Surfaces and storage owned by the caller must remain alive through detachment and reset of the last referencing command list. Reset that list before `ol_scene2d_release()` destroys borrowed image headers.

## UI helpers

The UI core provides stable numeric control IDs. Each slot stores bounds, enabled state and springs for hover, press and value feedback. The caller supplies layout, model values, event translation, text and command storage. The contract is defined in `c/lib/gfx/include/openlogit_ui.h`.

`ol_ui_event()` handles pointer movement, capture, release, activation and focus traversal. Overlapping hit targets resolve to the highest matching ID. A press captures that control; release activates it when the hit target matches the capture. Focus traversal wraps through enabled slots. Slider dragging maps horizontal pointer position into a value from zero to one. See `c/lib/gfx/ui/interaction.c`.

At frame start, `ol_ui_begin()` supplies the current time and whether reduced motion is on, and clears frame activity and dirty state. Events mark the UI dirty. `ol_ui_feedback()` samples the slot’s springs, and `ol_ui_needs_frame()` combines activity with dirty state. Reduced motion returns feedback targets directly.

Drawing helpers record panels, buttons, toggles, sliders and progress indicators as rounded paths. Buttons use hover colour, press inset and focus geometry. Toggles and sliders derive geometry from the animated value. Labels can be drawn through the caller’s text adapter before presentation. Geometry recording is implemented in `c/lib/gfx/ui/controls.c`.

## Display and window adapters

`ol_display_bind()` borrows a packed target, validates dimensions and nonoverlapping eight-bit colour fields, copies the clip and starts a batch. Initialise display storage to zero and provide allocation and release callbacks for effects. Allocation failure sets the display’s error field to `OL_LIMIT`. See `c/lib/gfx/adapters/openlogit_display.c` and `c/lib/gfx/adapters/display_pixels.h`.

The display adapter family includes:

| Implementation | Role |
|---|---|
| `c/lib/gfx/adapters/display_path.c` | Rasterises coverage in tiles and bands, evaluates paints and composites onto packed pixels. |
| `c/lib/gfx/adapters/display_blit_ex.c` | Filters RGBA images with nearest, bilinear or box sampling; supports source stride, fractional destination geometry and glyph opacity. |
| `c/lib/gfx/adapters/display_shadow_ex.c` | Calculates Gaussian shadows for rounded rectangles, with fractional offset, spread and inset selection. |
| `c/lib/gfx/adapters/display_group.c` | Applies group opacity and rounded clipping through destination snapshots. |

Snapshot groups save the destination, draw children under a rectangular clip, then blend the drawn result against the snapshot using group opacity and rounded coverage. At full opacity, snapshots cover the corner tiles. Lower opacity uses a snapshot of the rectangle. Nested groups require snapshot storage owned by the caller and close from the innermost group outward. See `c/lib/gfx/include/openlogit_display_group.h`.

Window transport lives in `c/apps/gui/openlogit_window.h`. `ol_window_composite()` transfers a tightly packed RGBA surface into the current window through `SYS_GUI_BLIT`. `ol_window_present()` requests presentation through `SYS_GUI_FLUSH`. This lets a caller combine surface content and text into one presented frame.

`ol_window_composite_region()` packs damaged rows into caller scratch because this blit ABI describes a packed source without a stride field. `ol_window_present_region()` reports rectangular damage through `SYS_GUI_FLUSH_RECT`. `examples/openlogit/vector/main.c` demonstrates scene rendering, damage transfer, label drawing and presentation.

## SIMD pixel kernels

The shared kernels operate on packed pixel spans. Arithmetic blends compute each colour byte as:

```text
floor((source * alpha + destination * (255 - alpha)) / 255)
```

The vector implementation widens bytes to 16-bit lanes. For the bounded channel sum, division uses the exact identity `(sum + 1 + (sum >> 8)) >> 8`. Each blend vector processes four pixels, followed by a scalar tail. The implementation is in `c/lib/gfx/simd/gfx_simd.c`.

| Entry point | Operation |
|---|---|
| `gfx_k_blend_cc()` | Constant colour and constant alpha. |
| `gfx_k_blend_cv()` | Constant colour with coverage per pixel. |
| `gfx_k_blend_vc()` | Source pixels with constant alpha. |
| `gfx_k_blend_vv()` | Source pixels with alpha in the high byte. |
| `gfx_k_copy()` | Pixel copy. |
| `gfx_k_fill()` | Pixel fill. |

Kernels that take a constant alpha clear the high byte even when alpha is zero. Kernels that take coverage or an alpha per pixel preserve the entire destination pixel at zero coverage or alpha. At full coverage, `gfx_k_blend_cv()` stores the supplied colour; at full source alpha, `gfx_k_blend_vv()` stores the source colour with its high byte cleared. These distinctions preserve the display callers’ existing byte semantics.

Display callers use the blend kernels when channel positions form a permutation of bits 0, 8 and 16. Other accepted layouts use loops that take the channel positions into account. Eligibility is defined in `c/lib/gfx/simd/gfx_simd.h` and used through `c/lib/gfx/adapters/display_pixels.h`.

`gfx_simd_tier()` chooses scalar or vector execution. The x86-64 probe checks SSE2 through CPUID. Clang vector extensions provide the vector implementation, targeting SSE2 on x86-64 and NEON on Apple Silicon hosts. `gfx_simd_force()` supports comparisons; a negative argument restores automatic selection.

## Kernel verification and test coverage

`gfx_simd_selftest()` calls all six vector implementations directly and compares them with reference implementations. Fixed inputs cover selected lengths from one through 29 pixels, offsets, alpha endpoints and surrounding pixels. A mismatch records the first failing kernel and pins automatic selection to scalar. The scalar blend references disable loop vectorisation to retain a different execution path.

In the kernel, `c/kernel/init/kmain.c` calls `fb_present_bench()` after the initial desktop render. Its SIMD report runs the self-test, prints selection and self-test state, then times each kernel at scalar and vector tiers using the guest clock. The timing report is informational. See `c/kernel/gui/fb/fb.c`.

With `GFX_SIMD_VERIFY`, calls using the four vector blend kernels are compared with scalar calculations in chunks of 64 pixels. Atomic counters record checked calls and differing pixels; `c/kernel/gui/wm.c` reports them. Benchmark calls are excluded from those counters.

The following commands have recipes and assertions in the inspected tree. Run them from the repository root.

| Command | Harness coverage |
|---|---|
| `make test-openlogit-effects` | Premultiplied convolution, shadows, workspace validation, backdrop halos and publication after successful rendering. |
| `make test-ol-backdrop-rounded` | Rounded coverage, direct convolution, blur with three passes, workspace and damage. |
| `make test-openlogit-animation` | Intermediate values, pause, seek, reverse playback, iteration direction, reduced motion and spring continuity. |
| `make test-anim-easing-catalog` | Integer easing curves, including Bézier and back behaviour. |
| `make test-anim-motion-fixed` | Integer spring settling, colour mixing and rectangle interpolation. |
| `make test-scene2d-ext` | Multiple damage regions, fractional placement, scaling, effect extent, ordering and hit testing. |
| `make test-ol-ui-fixes` | Overlapping controls, focus traversal, capture, slider dragging and disabled feedback. |
| `make test-glass-material` | Adaptive tint, neighbouring samples, Clear detail, reduced transparency and fast/general rendering parity. |
| `make test-glass-material-tiers` | Blur profiles, sampling halos, flat inputs and interleaved display contexts. |
| `make test-glass-geometry` | Cropping, square panels, integer square roots, allocation failure and rendering parity. |
| `make test-display-snapshot-group` | Group opacity, rounded corners, nesting, allocation limits and interleaved contexts. |

These targets make negative controls prerequisites. Their recipes require characteristic assertion failures, including disabled blur failing convolution, frozen animation failing the checks on intermediate values, merged scene damage failing the area check and blurred Clear glass failing detail preservation.

## Limitations

- Recorded blur and shadow radii are bounded by `OL_MAX_BLUR_RADIUS`, currently 64. Scene storage holds 32 layers and eight damage rectangles. Packed display dimensions are bounded by `OL_DISPLAY_MAX_SIZE`, currently 8192.
- Timeline descriptor validation accepts easing kinds through `OL_EASE_OUT_CUBIC`. The extended catalogue is available through `ol_ease256_ex()`.
- Scene transforms support translation and uniform scale. Hit testing uses rectangular bounds. `effect_extent` expands damage; callers provide the baked effect pixels.
- Snapshot-group equivalence applies to source-over children. Glass and blur inside a fading group sample the backdrop before the group fade.
- `GFX_SIMD_SSE41` is reserved. Implemented dispatch tiers are scalar and vector. Copy and fill have self-test coverage but are retained for measurement rather than wired into display copy and fill loops. Runtime verification covers the four blend kernels.
- Glass profiling sums elapsed stage durations across calls, including concurrent calls. It measures a different quantity from compositor CPU time.
- Builds, tests, guest execution and physical hardware validation were not run for this review.

## Source map

| File | What it contains |
|---|---|
| `c/lib/gfx/include/openlogit.h` | Recorded effects, layer options and submission workspace contract. |
| `c/lib/gfx/core/ol_record.c` | Effect validation and command recording. |
| `c/lib/gfx/core/ol_exec.c` | Workspace checks, execution, damage and publication. |
| `c/lib/gfx/effects/openlogit_effects.c` | RGBA layer blur, shadows and backdrops. |
| `c/lib/gfx/include/openlogit_glass_material.h` | Glass materials, tiers, rim styles and adaptation flags. |
| `c/lib/gfx/include/openlogit_glass.h` | Optical table interface and reflectance cap. |
| `c/lib/gfx/effects/openlogit_material.c` | Panel preparation, sampling halo, scratch and stage timing. |
| `c/lib/gfx/effects/glass_blur.c` | Blur passes and Clear backdrop copying. |
| `c/lib/gfx/effects/glass_geometry.h` | Coverage, edge distances and normals. |
| `c/lib/gfx/effects/glass_render.c` | Refraction sampling, neighbouring reflection and rendering. |
| `c/lib/gfx/effects/glass_shading.h` | Lighting and adaptive tint. |
| `c/lib/gfx/effects/openlogit_glass.c` | Optical derivation, integer arithmetic and cached tables. |
| `c/lib/gfx/include/openlogit_anim.h` | Timeline, keyframe, sample and analytic spring interfaces. |
| `c/lib/gfx/include/openlogit_easing.h` | Floating-point and integer Bézier inversion. |
| `c/lib/gfx/animation/openlogit_anim.c` | Timeline control, easing and analytic springs. |
| `c/lib/gfx/include/openlogit_motion.h` | Integer motion units and helper contracts. |
| `c/lib/gfx/animation/openlogit_motion.c` | Integer spring integration and interpolation. |
| `c/lib/gfx/include/openlogit_scene2d.h` | Scene storage, placement and resource lifetimes. |
| `c/lib/gfx/scene/scene2d.c` | Damage, composition and hit testing. |
| `c/lib/gfx/include/openlogit_ui.h` | UI slots, events, feedback and drawing interface. |
| `c/lib/gfx/ui/interaction.c` | Capture, focus, activation and feedback. |
| `c/lib/gfx/ui/controls.c` | Control geometry recorded as commands. |
| `c/lib/gfx/include/openlogit_display.h` | Packed targets and display scratch ownership. |
| `c/lib/gfx/adapters/openlogit_display.c` | Display binding, primitives and call sites for the blend kernels. |
| `c/lib/gfx/adapters/display_pixels.h` | Packing, clipping, allocation and kernel eligibility. |
| `c/lib/gfx/adapters/display_path.c` | Path coverage and paint composition onto packed pixels. |
| `c/lib/gfx/adapters/display_blit_ex.c` | Filtered images and glyph opacity. |
| `c/lib/gfx/adapters/display_shadow_ex.c` | Gaussian rounded rectangle shadows. |
| `c/lib/gfx/include/openlogit_display_group.h` | Snapshot-group composition and nesting contract. |
| `c/lib/gfx/adapters/display_group.c` | Snapshot storage, clipping and group blending. |
| `c/lib/gfx/simd/gfx_simd.h` | Interfaces for pixel kernels, selection, self-test and profiling. |
| `c/lib/gfx/simd/gfx_simd.c` | Scalar and vector kernels, dispatch, self-test and runtime comparison. |
| `c/apps/gui/openlogit_window.h` | Window composition, damage transport and presentation. |
| `c/kernel/gui/fb/fb.c` | Drawing contexts and SIMD boot report. |
| `c/kernel/gui/wm.c` | Damage expansion that accounts for sampling, clock installation and reporting. |
| `c/kernel/init/kmain.c` | Initial rendering and framebuffer benchmark invocation. |
| `examples/openlogit/vector/main.c` | Scene, interaction and presentation frame loop. |
| `tests/openlogit.mk` | Effect and timeline test recipes and controls. |
| `tests/ol-backdrop-rounded.mk` | Rounded backdrop tests and corner control. |
| `tests/anim-easing-catalog.mk` | Integer easing tests and control. |
| `tests/anim-motion-fixed.mk` | Integer motion tests and control for the duration cutoff. |
| `tests/scene2d-ext.mk` | Scene extension tests and damage control. |
| `tests/ol-ui-fixes.mk` | Interaction tests and control for disabled hover. |
| `tests/glass_material.mk` | Material tests, controls and rendering parity. |
| `tests/glass-material-tiers.mk` | Blur-tier tests and Gaussian comparison control. |
| `tests/glass_geometry.mk` | Geometry, allocation and controls for square panels. |
| `tests/display-snapshot-group.mk` | Snapshot-group tests and restoration control. |

## See also

- [OpenLogit 2D: geometry, rasterization and canvas](openlogit-2d.md)
- [Graphics and windowing](../architecture/graphics-and-windowing.md)
- [Compositing and effects](../kernel/compositing-and-effects.md)
- [Testing](../architecture/testing.md)
