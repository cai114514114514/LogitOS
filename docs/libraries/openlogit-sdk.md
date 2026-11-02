# Using OpenLogit from applications

OpenLogit provides drawing commands, surfaces, animation, and software 3D rendering for LogitOS applications. An application creates rendering resources, records a frame, submits it to a surface, and composites the result into its window. The application controls layout, events, and frame scheduling. The public interfaces live in `c/lib/gfx/include/`, `c/lib/gfx3d/include/`, and `c/apps/gui/openlogit_window.h`.

## Linking an application

The SDK static archive combines the 2D library with the software 3D modules. `Makefile` discovers the 2D sources recursively; `tests/openlogit.mk` discovers the 3D sources. `tests/openlogit_sdk.mk` constructs the archive and links the example applications with the C startup object, libc, and math objects.

From the repository root:

```sh
make openlogit-sdk
make build/disk.img
```

With the default build directory, the first command installs the SDK into the build sysroot. The second rebuilds the guest disk containing that installation. SDK installation follows libc sysroot generation because regenerating the sysroot replaces its contents. `tests/openlogit_sdk.mk` records this ordering and adds the SDK installation stamp to the disk prerequisites.

`tools/mkopenlogit_sdk.py` installs these guest locations:

| Guest location | Contents |
|---|---|
| `/usr/include/openlogit` | Selected public 2D and 3D headers, window helpers, and their ABI dependencies, installed by basename. |
| `/usr/lib/libopenlogit.a` | Static SDK archive. |
| `/usr/share/openlogit` | Example sources and supporting files, preserving their directory structure. |
| `/bin/scene-studio` | Compiled 3D scene example. |
| `/bin/vector-studio` | Compiled 2D drawing example. |

Inside LogitOS, compile and run the installed effects example with:

```sh
/bin/mkdir /tmp
/bin/tcc /usr/share/openlogit/effects.c -I/usr/include/openlogit -lopenlogit -o /tmp/effects
/tmp/effects
```

This invocation is documented in `examples/openlogit/README.md` and exercised by the harness in `tests/boot/run-openlogit-effects.py`. The include directory allows source files to include public headers by basename.

## Choosing public headers

| Repository path | Role |
|---|---|
| `c/lib/gfx/include/openlogit.h` | Device creation, capability queries, surfaces, images, command lists, effects, and submission. |
| `c/lib/gfx/include/gfx.h` | Geometry and paint umbrella included by the core API header. |
| `c/apps/gui/openlogit_window.h` | Surface composition, regional updates, presentation, and window state queries. |
| `c/apps/logit.h` | Window creation, events, clocks, settings, and syscall wrappers. |
| `c/lib/gfx/include/openlogit_canvas.h` | Saved drawing state, transforms, clipping, sprites, and nine slice drawing. |
| `c/lib/gfx/include/openlogit_scene2d.h` | Cached surface layers, placement, and damage tracking. |
| `c/lib/gfx/include/openlogit_anim.h` | Timelines, easing, interpolation, and springs sampled at caller timestamps. |
| `c/lib/gfx/include/openlogit_motion.h` | Integer springs advanced by elapsed steps, rectangle interpolation, and color mixing. |
| `c/lib/gfx/include/openlogit_ui.h` | Control geometry, pointer capture, focus, and animated feedback. |
| `c/lib/gfx3d/include/openlogit_3d.h` | Software 3D contexts, shaders, buffers, pipelines, and transforms. |
| `c/lib/gfx3d/include/openlogit_scene.h` | Mesh generation, orbit cameras, picking, pose blending, and skinning. |
| `c/lib/gfx3d/include/openlogit_lsl.h` | Typed shader compilation, reflection, and stage linkage. |
| `c/lib/gfx3d/include/openlogit_material.h` | Programmable pixel passes that publish into 2D surfaces. |

For integration with existing pixel storage, `c/lib/gfx/include/openlogit_draw.h` exposes raster operations, `c/lib/gfx/include/openlogit_bitmap.h` exposes bitmap operations, and `c/lib/gfx/include/openlogit_display.h` exposes drawing into native packed display targets.

Include the window helper for an application that combines SDK surfaces with window drawing. It includes the core API and `c/apps/logit.h`. The latter includes `c/lib/gfx/include/openlogit_wire.h` after declaring the syscall wrapper, supplying the transport helpers with their ABI types and syscall entry point.

## Initialising resources

The 2D core uses storage owned by the caller. Query object sizes with `ol_device_size()` and `ol_surface_size()`. Use `ol_list_min_size()` as the lower bound for a command arena, then reserve enough additional space for the frame’s commands and copied geometry. Object storage requires alignment to eight bytes and must remain allocated through destruction. The libc allocator in `c/apps/libc/src/malloc.c` provides suitably aligned storage.

Create a device with `ol_device_create()`, passing `OL_API_VERSION` and the required capability bits. The current API version is 1.1; device creation also accepts 1.0. To inspect capabilities, initialise the `size` field of `struct ol_caps` before calling `ol_device_caps()`. `c/lib/gfx/core/ol_device.c` implements these checks.

A surface uses two pixel buffers described by `struct ol_surface_desc`:

- `front` holds the published image.
- `work` holds the image under construction.

Supply disjoint buffers, their byte counts, dimensions, format, and row stride. `OL_FORMAT_RGBA8_STRAIGHT` stores straight alpha with bytes in R, G, B, A order. Submission renders into the work buffer and copies the affected pixels into the front buffer after all commands succeed. This preserves the preceding published frame when rendering fails; see `c/lib/gfx/core/ol_exec.c`.

Command recording copies path coordinates and paint values into the list. Commands that sample surfaces or images retain resource references and record their generations. For the next frame, reset the list, update source resources, and record commands against their current generations. Recording and reference release are implemented in `c/lib/gfx/core/ol_record.c`; submission checks generations in `c/lib/gfx/core/ol_exec.c`.

Destroy lists before the resources they retain, then destroy the device and free the backing allocations. `examples/openlogit/effects.c` demonstrates this order.

## Drawing a frame into a window

For a frame rendered into an application surface:

1. Reset the list with `ol_list_reset()`.
2. Record drawing commands and close the list with `ol_list_close()`.
3. Submit it to the target surface with `ol_submit()`.
4. Composite the published surface into the window with `ol_window_composite()`.
5. Draw any window text or additional transport primitives.
6. Call `ol_window_present()`.

Path coordinates use 24.8 fixed point; `GFX_PX()` converts whole pixels into that representation. Matrix coefficients use 16.16. These units are defined in `c/lib/gfx/include/gfx_math.h`.

The window bridge takes its destination rectangle in logical points and reads the surface in pixels. The `SYS_GUI_BLIT` handler in `c/kernel/gui/wm.c` validates the image and transfers it into the window backing surface, scaling to the destination’s device coordinates. Presentation marks the window’s content as damaged for the compositor.

This ordering allows surface drawing and window text to form one update. `examples/openlogit/studio/ui.c` submits its frame surface, composites it, draws labels, and presents.

Effects that require scratch storage use `ol_submit_workspace_size()` after the list is closed. Allocate the returned number of bytes in disjoint storage aligned to eight bytes, then pass it to `ol_submit_workspace()`. Its optional `struct ol_submit_info` reports damage and submission statistics.

For regional transport, `ol_window_composite_region()` packs a surface rectangle into scratch provided by the caller, and `ol_window_present_region()` reports window damage. The composite helper places the copied rectangle at the same window coordinates as its surface coordinates. Account for the conversion between surface pixels and window logical points when choosing this path; both helpers are defined in `c/apps/gui/openlogit_window.h`.

## Minimal complete example

The following static application derives its resource setup and event loop from `examples/openlogit/effects.c`. It renders a rectangle once and composites the published image when processing window events.

```c
#include "openlogit_window.h"
#include <stdlib.h>

int main(void)
{
    struct ol_device *device = NULL;
    struct ol_surface *surface = NULL;
    struct ol_list *list = NULL;
    void *device_storage = malloc(ol_device_size());
    void *surface_storage = malloc(ol_surface_size());
    void *list_storage = malloc(32768);
    unsigned char *front = calloc(1, 320 * 224 * 4);
    unsigned char *work = malloc(320 * 224 * 4);
    int result = 1;

    if (!device_storage || !surface_storage || !list_storage ||
        !front || !work)
        goto done;

    if (ol_device_create(device_storage, ol_device_size(),
                         OL_API_VERSION,
                         OL_CAP_PATH_FILL | OL_CAP_ATOMIC_FRAME,
                         &device) != OL_OK)
        goto done;

    struct ol_surface_desc desc = {
        .size = sizeof desc,
        .format = OL_FORMAT_RGBA8_STRAIGHT,
        .width = 320, .height = 224, .stride = 320 * 4,
        .front = front, .work = work,
        .front_bytes = 320 * 224 * 4,
        .work_bytes = 320 * 224 * 4
    };
    if (ol_surface_create(device, surface_storage, ol_surface_size(),
                          &desc, &surface) != OL_OK ||
        ol_list_create(device, list_storage, 32768, &list) != OL_OK)
        goto done;

    int points[16], subs[2];
    struct gfx_path path;
    struct gfx_paint paint;
    gfx_path_init(&path, points, 8, subs, 2);
    gfx_path_rect(&path, GFX_PX(40), GFX_PX(40),
                  GFX_PX(160), GFX_PX(128));
    gfx_paint_solid(&paint, 0x63ecd2, 255);

    /* The clear defines every pixel before the first publication. */
    if (ol_cmd_clear(list, 0x111c31, 255) != OL_OK ||
        ol_cmd_fill(list, &path, GFX_NONZERO, &paint, NULL, 4) != OL_OK ||
        ol_list_close(list) != OL_OK ||
        ol_submit(device, list, surface, NULL) != OL_OK)
        goto done;

    gui_create("OpenLogit", 320, 224);
    for (;;) {
        struct logit_event event;
        while (poll_event(&event) > 0) {
            if (event.type == EV_CLOSE ||
                (event.type == EV_KEY && event.a == 27)) {
                result = 0;
                goto done;
            }
        }
        if (ol_window_visible()) {
            ol_window_clear(0x111c31);
            if (ol_window_composite(surface, 0, 0, 320, 224) != OL_OK ||
                ol_window_present() != OL_OK)
                goto done;
        }
        wait_idle(0);
    }

done:
    /* Drop recorded references before releasing resource storage. */
    if (list) ol_list_destroy(list);
    if (surface) ol_surface_destroy(surface);
    if (device) ol_device_destroy(device);
    free(list_storage);
    free(surface_storage);
    free(device_storage);
    free(front);
    free(work);
    return result;
}
```

The fresh list is ready for recording after creation. The rectangle uses eight slots of point storage, with two integers per slot. The fill records its own copies of the geometry and paint. `wait_idle(0)` waits for an event and keeps it pending, so the next iteration drains events through `poll_event()`. These contracts are implemented in `c/lib/gfx/geometry/gfx_path.c`, `c/lib/gfx/core/ol_record.c`, and `c/kernel/gui/wm.c`.

## Animation and frame scheduling

Create a `struct ol_timeline` with `ol_animation_init()` and sample it with `ol_animation_sample()`. The descriptor supports scalar, vector, and quaternion tracks, delay, repetition, and direction. The caller keeps the keyframe array alive for as long as the timeline uses it. The declarations are in `c/lib/gfx/include/openlogit_anim.h`.

Use one `monotonic_ns()` timestamp for related animation samples in a frame. `struct ol_anim_sample` returns the value, activity state, and next sampling time. The application translates this into a wait timeout and renders the final transition into the inactive state.

For movement that can change target during playback, `ol_spring_init()`, `ol_spring_sample()`, and `ol_spring_retarget()` provide an analytic damped spring. Retargeting samples the current position and velocity before establishing the new target. `c/lib/gfx/animation/openlogit_anim.c` evaluates the motion from elapsed time.

The effects example reads `ui.reduce_motion`, suspends rendering while minimized, and uses `wait_idle(active ? 16 : 0)` to sleep between frames. It retains the preceding activity state so the final resting frame is drawn. `examples/openlogit/studio/main.c` applies the same scheduling pattern to scene playback and control feedback.

## Adding software 3D and materials

Query 3D capabilities with `ol3d_caps()` and create a context with `ol3d_create()`. A frame begins with `ol3d_begin()`, draws through `ol3d_draw_indexed()` or `ol3d_draw_buffers()`, and finishes with `ol3d_end()`. Passing a 2D surface with matching dimensions to `ol3d_end()` publishes the image there for composition. The context allocates its color and depth storage during creation; release it with `ol3d_destroy()`. See `c/lib/gfx3d/render/ol3d.c`.

`ol3d_buffer_create()` copies vertex or index data into an owned buffer. After updating a buffer, obtain a current view with `ol3d_buffer_view()`. Pipeline creation copies validated shader programs into the pipeline object. These resource operations are implemented in `c/lib/gfx3d/render/ol3d_resources.c`.

For typed shader sources, use `ol_lsl_compile()` followed by `ol_lsl_pipeline_create()`. Compilation validates the generated program, and linkage checks stage inputs and uniform ranges before creating the pipeline. `examples/openlogit/studio/scene.c` demonstrates shader setup, buffer drawing, and publication into a surface. Its event loop in `examples/openlogit/studio/main.c` resets the preceding 2D list before publishing the next scene image.

The 3D coordinate conventions in `c/lib/gfx3d/include/openlogit_3d.h` use column-major matrices, a clip volume of `-w..w` on x and y and `0..w` on z, and counterclockwise front faces in normalized device coordinates.

For programmable 2D pixel effects, use `ol_material_query_caps()`, `ol_material_create()`, and `ol_material_context_create()`, then render with `ol_material_render()`. The context dimensions must match the output surface. The implementation in `c/lib/gfx3d/material/ol_material.c` stages the complete pass before uploading it, allowing the resulting surface to enter the same composition path.

## Limitations

- The reported 2D and 3D backends are software implementations. GPU acceleration is absent from their capability results; see `c/lib/gfx/core/ol_internal.h` and `c/lib/gfx3d/render/ol3d.c`.
- `ol_window_composite()` requires straight RGBA8 with a row stride equal to width times four. The regional helper repacks rows because the window blit transport carries source dimensions without a stride; see `c/apps/gui/openlogit_window.h`.
- `ol_window_visible()` queries minimization state. It provides no occlusion test.
- Submission completion records surface publication. Window presentation schedules compositor work; neither operation supplies a fence tied to the display or the vertical blank.
- The minimal example was checked against declarations, implementations, and existing callers. It was not compiled or executed for this page.

## Source map

| File | What it contains |
|---|---|
| `Makefile` | 2D source discovery, application compilation flags, and disk construction. |
| `tests/openlogit.mk` | Software 3D source discovery and graphics checks. |
| `tests/sysroot.mk` | Sysroot locations, libc installation, and SDK fragment inclusion. |
| `tests/openlogit_sdk.mk` | SDK archive construction, example linking, and installation dependencies. |
| `tools/mkopenlogit_sdk.py` | Header, archive, executable, and example installation. |
| `c/lib/gfx/include/openlogit.h` | Resource ownership, capabilities, recording, and submission interfaces. |
| `c/lib/gfx/include/gfx.h` | Geometry, paint, and raster header umbrella. |
| `c/lib/gfx/include/gfx_math.h` | Coordinate units, rectangles, and matrices. |
| `c/lib/gfx/include/openlogit_canvas.h` | Drawing state, clipping, sprites, and layer recording. |
| `c/lib/gfx/include/openlogit_scene2d.h` | Cached surface composition and damage contracts. |
| `c/lib/gfx/include/openlogit_ui.h` | Control interaction and geometry interfaces. |
| `c/lib/gfx/core/ol_internal.h` | Software feature set and object internals. |
| `c/lib/gfx/core/ol_device.c` | Device and surface creation, capability queries, and destruction. |
| `c/lib/gfx/core/ol_record.c` | Command copying, resource references, list reset, and closure. |
| `c/lib/gfx/core/ol_exec.c` | Validation, workspace sizing, rendering, and publication. |
| `c/lib/gfx/geometry/gfx_path.c` | Path construction and caller storage handling. |
| `c/apps/gui/openlogit_window.h` | Surface transfer, regional updates, and presentation helpers. |
| `c/apps/logit.h` | Window creation, events, settings, and clock wrappers. |
| `c/lib/gfx/include/openlogit_wire.h` | Window drawing syscall helpers. |
| `c/kernel/gui/wm.c` | Window transfer, presentation damage, and event waiting. |
| `c/apps/libc/src/malloc.c` | Allocation and alignment used by the examples. |
| `c/lib/gfx/include/openlogit_anim.h` | Timeline and spring interfaces. |
| `c/lib/gfx/include/openlogit_motion.h` | Integer motion interfaces. |
| `c/lib/gfx/animation/openlogit_anim.c` | Timeline sampling and analytic springs. |
| `c/lib/gfx3d/include/openlogit_3d.h` | 3D interfaces and coordinate conventions. |
| `c/lib/gfx3d/include/openlogit_scene.h` | Mesh, camera, picking, and skinning helpers. |
| `c/lib/gfx3d/render/ol3d.c` | Software rendering context and frame publication. |
| `c/lib/gfx3d/render/ol3d_resources.c` | Owned buffers, versioned views, and pipeline snapshots. |
| `c/lib/gfx3d/include/openlogit_lsl.h` | Typed shader compiler and linkage interfaces. |
| `c/lib/gfx3d/shader/lsl/compiler.c` | Shader compilation and generated program validation. |
| `c/lib/gfx3d/shader/lsl/link.c` | Typed stage linkage and pipeline creation. |
| `c/lib/gfx3d/include/openlogit_material.h` | Programmable pixel pass interfaces. |
| `c/lib/gfx3d/material/ol_material.c` | Material staging and surface upload. |
| `examples/openlogit/effects.c` | Resource setup, effects, animation scheduling, and cleanup. |
| `examples/openlogit/studio/main.c` | Scene Studio initialisation and event loop. |
| `examples/openlogit/studio/scene.c` | Shader setup, buffer drawing, and scene publication. |
| `examples/openlogit/studio/ui.c` | Surface composition, labels, and presentation. |
| `tests/boot/run-openlogit-effects.py` | Guest compilation and interaction harness for the effects example. |

## See also

- [OpenLogit 2D: geometry, rasterization and canvas](openlogit-2d.md)
- [OpenLogit effects, animation and SIMD kernels](openlogit-effects-and-animation.md)
- [OpenLogit 3D: the software renderer](openlogit-3d.md)
- [Graphics and windowing](../architecture/graphics-and-windowing.md)
- [Applications and AEX packages](../architecture/applications-aex.md)
- [Build system](../architecture/build-system.md)
