# Compositing and effects

LogitOS composes application canvases, window frames, desktop chrome and overlays into a retained screen back buffer. Damage rectangles select the pixels to rebuild and present. Glass samples nearby backdrop pixels, so its input extents guide both damage expansion and the division of composition work across processors. The frame machinery lives in `c/kernel/gui/wm.c`; the draw2 modules handle extended drawing calls into application canvases.

## Canvases and frame boundaries

Each window has one canvas. Applications draw into the same storage the compositor reads. The first pixel drawing call starts a drawing interval; `SYS_GUI_FLUSH` and `SYS_GUI_FLUSH_RECT` close it and report damage (`c/kernel/gui/wm.c`, `include/abi/logit_abi.h`).

Before composition, the window manager checks each damage rectangle against windows with drawing intervals still open. It defers intersecting rectangles and queues them again after the frame. The retained back buffer preserves the previously composed pixels while the application finishes its update.

Canvas drawing and composition share the graphics mutex in `c/kernel/gui/fb/fb.c`. GUI drawing calls acquire it before the window state lock. The compositor selects deferred rectangles while holding both locks, then releases the state lock for the pixel pass while retaining the graphics mutex. GUI calls that could change the canvases or rendering geometry wait for that pass to finish.

## Damage tracking

Damage uses half-open rectangles in device pixels. `dirty_rect()` clips incoming damage to the screen and requests a frame when the clipped rectangle has area. `dmg_add()` merges overlapping rectangles. Its list holds six entries; when full, it merges the incoming rectangle with the entry whose union adds the least excess area (`c/kernel/gui/wm.c`, `c/kernel/gui/wm_internal.h`).

When multiple entries together exceed three quarters of the screen area, the damage machinery requests a full frame. A single large rectangle retains its bounds because it already requires one traversal and presentation.

| Change | Damage reported |
|---|---|
| `SYS_GUI_FLUSH` | Window frame bounds when drawn at its current position, full size and full opacity |
| `SYS_GUI_FLUSH_RECT` | Requested content rectangle, converted from logical points to device pixels, clamped to the canvas and placed at the content origin |
| Window movement | Old and new footprints, including shadow margins and applicable backdrop sample extents |
| Whole desktop invalidation | Screen rectangle and a recorded reason |

The flush handlers use `dirty_win_content()` and `dirty_win_content_rect()`. Scaled or faded windows use the broader footprint from `dirty_win()`. Flushes on the locked desktop report the whole screen. A rectangle flush whose bounds become empty after clamping takes the whole-window flush path (`c/kernel/gui/wm.c`).

The compositor reconstructs each selected rectangle from its underlying layers. This gives shadows, translucent edges and glass fresh inputs and prevents their effects from accumulating over the previous composite.

## Composition order

`render_layers()` in `c/kernel/gui/wm.c` draws these layers:

1. Cached wallpaper and the Exposé dim layer.
2. Windows from back to front, including shadows, frames and canvases.
3. Closing window images and the snap preview.
4. Menu bar, dock, dropdown menus, overlay panels and window switcher.

`render_top()` then draws the input method candidate bar, notifications and the cursor when cursor composition into the back buffer is selected. These overlays run on the compositor thread because their composition functions can advance state.

`opaque_content_start()` can start rendering at the highest opaque canvas that covers the entire damage rectangle. It checks placement, opacity, canvas dimensions, rounded corners and backdrop regions. Pixels within rounded corner squares or backdrop regions retain their dependency on lower layers (`c/kernel/gui/wm.c`, `c/kernel/gui/wm_cover.h`).

Where corner clipping is needed, `content_group_begin()` and `content_group_end()` wrap the canvas and its backdrop materials in a rounded group. Shadow painting and footprint calculations share the presets and extent rules in `c/kernel/gui/wm_shadow.h` (`c/kernel/gui/wm_chrome.c`).

## Glass and blur materials

Framebuffer drawing adapters call the shared OpenLogit software renderer. A glass material selects tint, opacity, tone, blur tier and rim style. Blur tiers are regular, thin, thick and ultra; rim styles are directional and symmetric. Zero tint opacity selects clear glass, which preserves backdrop detail while keeping lens and rim effects. Reduce Transparency selects a solid panel (`c/kernel/gui/wm_draw2_fx.c`, `c/lib/gfx/effects/openlogit_material.c`).

The drawing target determines the scene sampled by the material:

| Use | Sampled content |
|---|---|
| Desktop chrome | Layers already composed beneath that chrome |
| `SYS_GUI_GLASS2` | Pixels already drawn into the application canvas |
| `SYS_GUI_BACKDROP` | Layers beneath the application window |

`c/kernel/gui/wm_glass.c` centralizes chrome material styles and enumerates chrome glass regions for damage expansion and band splitting. `c/kernel/gui/wm_draw2_fx.c` handles canvas glass. `c/kernel/gui/wm_backdrop.c` stores and composes application backdrop regions.

## Glass dependencies and parallel composition

A glass panel samples a neighborhood around its bounds. The renderer’s sample halo includes blur support and optical displacement. The compositor obtains this extent from the shared material implementation, so its dependency calculations stay tied to the sampling code (`c/lib/gfx/effects/openlogit_material.c`).

Before drawing a partial frame, `dmg_expand()` grows damage that intersects a glass input extent until it contains that extent. It processes chrome panels and application backdrop regions, then merges rectangles brought together by expansion. Expansion repeats for up to eight passes; continued growth selects a full frame. Input extents are clamped to the screen before union so expansion can converge at screen edges (`c/kernel/gui/wm.c`, `c/kernel/gui/wm_backdrop.c`).

Rebuilding the sampled neighborhood prevents a partial update from feeding the previous frame’s frosted output back into the material.

Parallel composition follows the same dependency rule. `c/kernel/gui/wm_bands.h` divides damage into horizontal bands whose boundaries avoid glass sample extents. It assigns greater cost to rows within protected glass ranges when choosing boundaries. Each band renders its layers in order into its portion of the shared back buffer.

Workers have their own target views, clips, displays and material scratch. Context selection follows the worker thread, so its drawing state survives preemption and migration (`c/kernel/gui/fb/fb.c`).

The compositor claims bands alongside the workers and picks up unclaimed work while they run. Bands intersecting the dock or visible window switcher run on the compositor thread because application icon drawing can read storage on a cache miss. After all bands finish, the compositor draws the top overlays and presents each damage rectangle (`par_render()` in `c/kernel/gui/wm.c`).

## Backdrop regions and foreground ink

`SYS_GUI_BACKDROP` records a material region and fills its rounded canvas area with a fallback color carrying `LOGIT_BACKDROP_MARK` in the high byte. `LOGIT_BACKDROP_MAX` provides eight region slots per window. Registration clips the region and marker fill to the canvas bounds (`include/abi/logit_abi.h`, `c/kernel/gui/wm_backdrop.c`).

During composition, `wm_backdrop_composite_at()` renders each region’s material over the layers beneath the window. For the content copy at matching canvas dimensions, `wm_backdrop_blit_content()` skips marked pixels inside active regions. Glass shows through those pixels while application foreground drawing appears above it.

The marker makes the high byte part of the compositor’s bookkeeping. Application colors pass through RGB masking, including `wm_app_rgb()` in `c/kernel/gui/wm_cover.h`. The audit in `c/kernel/gui/wm_backdrop.c` traces legacy drawing calls, draw2 handlers, image copies and canvas resizing to account for every writer that can encounter these markers.

## The draw2 path

Draw2 implements the extended GUI drawing ABI. `wm_gui_dispatch()` binds the caller’s canvas, constructs a `wm2_env`, and calls `wm2_dispatch()`. The environment supplies the display, scale, canvas dimensions, window slot, user memory copying, clipping and allocation services (`c/kernel/gui/wm.c`, `c/kernel/gui/wm_draw2.h`).

`SYS_GUI2_INFO(0)` returns the ABI version shifted left by 16 bits, combined with feature bits supplied by the handler modules. Descriptor arguments begin with a size field. `wm2_copy_struct()` checks that field against the expected size, copies the descriptor through the user memory service, then checks the copied size again (`c/kernel/gui/wm_draw2.c`, `c/kernel/gui/wm_draw2.h`).

| Module | Responsibility |
|---|---|
| `c/kernel/gui/wm_draw2_path.c` | Path validation, fills, strokes, transforms and solid or gradient paints |
| `c/kernel/gui/wm_draw2_state.c` | Clips, groups, snapshots and image blits |
| `c/kernel/gui/wm_draw2_fx.c` | Shadows and glass over the canvas |
| `c/kernel/gui/wm_backdrop.c` | Desktop backdrop region registration and composition |
| `c/kernel/gui/wm_draw2_text.c` | Text runs with alpha and weight |
| `c/kernel/gui/wm_draw2_icon.c` | Vector symbols with size and stroke weight |

Placement coordinates and lengths use 24.8 logical points, where 256 represents one point. Rectangle conversion in `c/kernel/gui/wm2_scale.h` converts both edges before deriving width and height, which keeps adjacent rectangles aligned at fractional scales.

Path geometry retains fractional device coordinates unless snapping is requested. Clip, group and effect handlers round converted edges to whole device pixels. Text and icon handlers floor their converted placement and size to whole device pixels. The corresponding draw2 modules implement these conversions.

Clip and group levels belong to the window slot and span calls. Clips stored on the canvas survive display rebinding. Groups retain snapshots and apply opacity and rounded coverage when closed. A flush closes remaining levels from the inside out and counts each closure through `SYS_GUI2_INFO(1)`. Window creation, destruction and canvas resize reset the slot’s draw2 state (`c/kernel/gui/wm_draw2_state.c`, `c/kernel/gui/wm.c`).

Image blits copy the source rows needed for visible destination bands into kernel scratch before drawing. This bounds temporary storage while supporting source cropping, filtering, opacity and premultiplied input (`c/kernel/gui/wm_draw2_state.c`).

## Presentation and cursor transport

`fb_present_rect()` clips completed rectangles to the screen. A registered native presentation callback can handle a rectangle; its return value selects completion, CPU fallback or framebuffer quarantine. The CPU path copies rows into display backing and can use a registered parallel copy implementation for rectangles at least 128 rows tall. VirtIO presentation follows the copy with `virtio_gpu_flush()` (`c/kernel/gui/fb/fb.c`).

For the linear framebuffer, `fb_init()` requests a write-combining mapping when PAT setup reports it available, otherwise an uncached mapping. It reads back the page table entry for the boot log. CPU row copies use `rep movsb` on x86-64 (`c/kernel/gui/fb/fb.c`, `c/kernel/cpu/power/pat.c`).

The window manager first tries the display cursor plane. A plain linear framebuffer can use a CPU cursor overlay: presentation blends the sprite with retained desktop pixels in scratch storage. Pointer movement presents the old and new cursor boxes from that RAM image. `wm_pointer_sync()` publishes overlay shape changes under the graphics and state locks (`c/kernel/gui/wm.c`, `c/kernel/gui/fb/fb_overlay.c`).

Parallel CPU presentation records acknowledgement waits for its work IPIs. After a timeout, the presenting CPU claims pending copies and waits for copies already running to finish. Four consecutive timeouts, or at least eight timeouts among the most recent 64 parallel presents once 64 calls have been recorded, unregister the parallel callback. Subsequent CPU copies use the local path (`c/kernel/cpu/smp/smp.c`, `c/kernel/cpu/smp/present_policy.c`, `c/kernel/cpu/smp/present_policy.h`).

## Diagnostics and verification hooks

`wm_render()` measures elapsed frame time from before graphics lock acquisition through completion, together with the compositor thread’s CPU time once a scheduler thread exists. For frames taking at least 33 ms, `c/kernel/gui/wm_trace.c` increments a counter and retains the worst frame. Serial reports are limited to one per second and include graphics lock wait, layer rendering, top overlays, presentation, glass time, rectangles and pixels.

Glass time overlaps layer time and accumulates across concurrent bands. Band counters also accumulate all band durations and each frame’s longest band duration. These measurements cover different portions of the frame and can overlap (`c/kernel/gui/wm.c`, `c/kernel/gui/wm_glass.c`).

A build made with `make HWHUD=1` adds a diagnostic panel painted into the back buffer at the end of the window manager loop. It displays the worst recorded slow frame. Menus, overlay panels, the switcher, Exposé, application panel windows, input method candidates and notifications trigger its hiding policy. The panel requests damage over its old rectangle and resumes drawing when that UI closes. Clicking its title row toggles collapse; the window manager consumes the press and release (`Makefile`, `c/kernel/diag/hwhud.c`, `c/kernel/diag/hwhud_panel.h`, `c/kernel/gui/wm.c`).

At boot, `fb_present_bench()` runs the shared pixel kernel self-test and reports implementation selection. With benchmark scratch available, it prints scalar and vector timings. A self-test mismatch pins automatic selection to scalar code. The linear framebuffer path also times a full frame CPU copy (`c/kernel/init/kmain.c`, `c/kernel/gui/fb/fb.c`, `c/lib/gfx/simd/gfx_simd.c`).

The tree defines these checks:

| Target | Scope |
|---|---|
| `test-wm-par` | Guest comparison of parallel layer rendering against serial rendering |
| `test-wm-backdrop-regions` | Host checks for backdrop sampling, markers, region behavior and band dependencies |
| `test-wm-backdrop-regions-guest` | Guest pixel checks for backdrop variation and foreground text |
| `test-wm-draw2-path` | Host checks for path output and malformed input |
| `test-wm-draw2-state` | Host checks for clips across display rebinding, groups and image blits |
| `test-wm-draw2-effects` | Host checks for effects, scaling and Reduce Transparency |
| `test-flash` | Guest pixel checks for partly drawn canvases |
| `test-fb-native-present-host` | Host checks for presentation callbacks, CPU fallback and quarantine |

Their make fragments attach negative controls as prerequisites. The parallel guest checker requires parallel frames, worker participation and compared pixels; its control requires pixel differences when bands cut through glass. Other controls exercise canvas sampling in place of desktop sampling, lost clips during rebinding, unscaled shadow blur, unfinished canvases and CPU overwrites after native presentation (`tests/fluid_perf.mk`, `tests/qmp/qmp_repaint.py`, and the fragments in the source map).

## Limitations

- The unfinished-canvas guard expires after 400 ms, allowing a partially drawn canvas to appear. Locked desktop frames, Exposé and frames with visible windows that fail `win_drawn_direct()` use serial composition (`c/kernel/gui/wm.c`).
- Backdrop foreground fringes blend against the fallback fill. Canvas stretching during resize and opaque scaled thumbnails show that fill; fading canvases blend it over the material. The backdrop alpha-layer flag remains reserved (`c/kernel/gui/wm_backdrop.c`, `include/abi/logit_abi.h`).
- Clearing a backdrop region leaves its fallback pixels until the application repaints. Overlapping regions compose in region ID order, so later regions sample earlier glass (`c/kernel/gui/wm_backdrop.c`).
- Draw2 glass requires equal corner radii. Backdrop regions use the largest requested radius, clamped to half the shorter side, and square a side when both adjoining corner radii are zero (`c/kernel/gui/wm_draw2_fx.c`, `c/kernel/gui/wm_backdrop.c`).
- Draw2 clips and groups have a 16-level stack per window and share a 16-entry framebuffer clip ledger across windows. Interleaved pops that would restore a different clip return `OL_STATE`. Snapshot storage per window is capped at the smaller of twice the canvas byte size and 32 MiB (`c/kernel/gui/wm_draw2_state.c`).
- The CPU cursor overlay accepts sprites up to 64 × 64 pixels on plain linear framebuffers. Source comments mark the write-combining, overlay and SIMD additions as awaiting X79-VG4 hardware verification (`c/kernel/gui/fb/fb.c`, `c/kernel/gui/fb/fb_overlay.h`, `c/lib/gfx/simd/gfx_simd.h`).

## See also

- [Window manager](window-manager.md)
- [GUI events, clipboard and notifications](gui-events-and-services.md)
- [Graphics and windowing](../architecture/graphics-and-windowing.md)
- [Testing](../architecture/testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/kernel/gui/wm.c` | Frame boundaries, damage tracking, composition, band workers, presentation routing and GUI dispatch |
| `c/kernel/gui/wm_internal.h` | Window helpers and rectangle operations |
| `c/kernel/gui/wm_cover.h` | Opaque coverage rules, backdrop placement and application color masking |
| `c/kernel/gui/wm_chrome.c` | Window frames and rounded content groups |
| `c/kernel/gui/wm_shadow.h` | Shadow presets and footprint calculations |
| `c/kernel/gui/wm_glass.c` | Chrome materials, glass regions and compositor glass timing |
| `c/kernel/gui/wm_bands.h` | Band splitting around glass input extents |
| `c/kernel/gui/wm_backdrop.c` | Backdrop registration, markers, dependency calculations and composition |
| `c/kernel/gui/fb/fb.c` | Drawing contexts, graphics locking, framebuffer mapping, presentation and cursor transport |
| `c/kernel/gui/fb/fb_overlay.c` | Cursor box clipping and sprite composition from retained RAM |
| `c/kernel/gui/fb/fb_overlay.h` | Overlay records and sprite size limit |
| `c/kernel/gui/wm_trace.c` | Slow-frame count, worst-frame record, formatting and reporting policy |
| `c/kernel/gui/wm_draw2.c` | Draw2 routing, feature discovery and lifecycle hooks |
| `c/kernel/gui/wm_draw2.h` | Handler environment and descriptor copying |
| `c/kernel/gui/wm_draw2_path.c` | Path validation, transformation, fills and strokes |
| `c/kernel/gui/wm_draw2_state.c` | Clip ledger, groups, snapshots and image copying |
| `c/kernel/gui/wm_draw2_fx.c` | Shadow and canvas glass handlers |
| `c/kernel/gui/wm_draw2_text.c` | Text run handling and coordinate conversion |
| `c/kernel/gui/wm_draw2_icon.c` | Vector symbol handling and coordinate conversion |
| `c/kernel/gui/wm2_scale.h` | Logical point conversion rules |
| `include/abi/logit_abi.h` | GUI calls, descriptors, feature bits and backdrop contract |
| `c/lib/gfx/effects/openlogit_material.c` | Shared material rendering and sample halo queries |
| `c/lib/gfx/simd/gfx_simd.c` | Shared pixel kernel selection, self-test and benchmarks |
| `c/lib/gfx/simd/gfx_simd.h` | Pixel kernel interfaces and verification scope |
| `c/kernel/cpu/power/pat.c` | PAT setup for write-combining mappings |
| `c/kernel/cpu/smp/smp.c` | Parallel CPU presentation, acknowledgement tracking and fallback copies |
| `c/kernel/cpu/smp/present_policy.c` | Rolling timeout policy |
| `c/kernel/cpu/smp/present_policy.h` | Presentation timeout thresholds |
| `c/kernel/diag/hwhud.c` | Diagnostic panel sampling, painting and interaction |
| `c/kernel/diag/hwhud_panel.h` | Panel placement, visibility and collapse policy |
| `c/kernel/init/kmain.c` | Boot invocation of framebuffer benchmarks |
| `Makefile` | Hardware diagnostic panel build option |
| `tests/fluid_perf.mk` | Band splitter and guest composition checks |
| `tests/qmp/qmp_repaint.py` | Guest interaction driver and parallel comparison assertions |
| `tests/wm-backdrop-regions.mk` | Host and guest backdrop checks and controls |
| `tests/wm-draw2-path.mk` | Path checks and validation control |
| `tests/wm-draw2-state.mk` | Clip, group and image blit checks |
| `tests/wm-draw2-effects.mk` | Effect checks and scaling control |
| `tests/flash.mk` | Unfinished-canvas pixel checks |
| `tests/fb_native_present.mk` | Native presentation checks and CPU overwrite control |
