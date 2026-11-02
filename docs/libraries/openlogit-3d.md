# OpenLogit 3D: the software renderer

OpenLogit 3D renders indexed triangles on the CPU through programmable vertex and fragment stages, clipping, depth testing, texture sampling and blending. Its typed shading language, LSL, compiles into OLS-IR instructions executed by an interpreter. Applications own geometry, shaders and rendering storage, then upload completed images into OpenLogit 2D surfaces for composition and window presentation.

## Kernel and user space

The kernel source selection in `Makefile` includes the 2D and animation code under `c/lib/gfx/` and filters out `c/lib/gfx3d/`. The triangle renderer, shader compiler, interpreter, scene helpers and programmable image materials run in user space.

`tests/openlogit_sdk.mk` compiles the 3D sources with application flags and combines them with the 2D objects in the SDK archive. `tools/mkopenlogit_sdk.py` installs that archive, public headers, shader compiler programs and examples into the guest sysroot.

Applications execute shaders and produce RGBA images in their own storage. The kernel window manager handles application canvases and desktop composition. The window transport in `c/apps/gui/openlogit_window.h` submits rendered pixels through GUI system calls handled by `c/kernel/gui/wm.c`.

## Triangle pipeline

The public interface is `c/lib/gfx3d/include/openlogit_3d.h`; the pipeline implementation is `c/lib/gfx3d/render/ol3d.c`.

A draw supplies pipeline state, resource bindings, vertices and triangle indices. Each `ol3d_vertex` contains eight attributes, each holding four floats. The vertex program produces a homogeneous position and varying values. The fragment program consumes interpolated varyings and produces RGBA color.

| Stage | Operation |
| --- | --- |
| Validation | Check pipeline state, shader instructions, resource bindings, counts and every index before vertex execution. |
| Vertex shading | Execute the vertex program for every supplied vertex and cache its position and varyings. |
| Clipping | Clip against six homogeneous planes, interpolate new vertices and triangulate the resulting polygon as a fan. |
| Projection and culling | Divide by homogeneous w, map to the viewport and apply face culling. |
| Coverage | Evaluate integer edge equations at pixel centers using a top-left inclusion rule. |
| Depth and interpolation | Apply the selected depth comparison and interpolate varyings with perspective correction. |
| Fragment shading | Execute the fragment program, clamp and round its color to RGBA8, blend and apply the configured depth write. |

Matrices use column major storage. Clip coordinates satisfy −w ≤ x ≤ w, −w ≤ y ≤ w and 0 ≤ z ≤ w. Front faces are counterclockwise in normalized device coordinates; viewport mapping flips the vertical axis.

Coverage coordinates have eight fractional bits. The top-left rule assigns shared edges consistently, allowing adjacent triangles to cover each pixel once. This matters for blending, where drawing an edge twice changes its color. Depth interpolates clip z divided by w across the screen; varying interpolation adjusts the barycentric weights by reciprocal w.

Raw pipeline state controls face culling, depth comparison, depth writes and blending. Depth comparisons include less, less-or-equal, always, never and greater. Fragment blending reuses the 2D composition functions. A nondefault composition operator takes precedence over the pipeline’s blend flag; with the default operator, that flag selects source-over blending or direct color replacement.

## Frames and resource ownership

An `ol3d_context` owns a published RGBA image, an RGBA work image and a floating point depth buffer. `ol3d_begin` clears the work image to an opaque RGB color and initializes depth to 1. Draws execute synchronously.

`ol3d_end` checks the stored frame error, uploads the work image to an optional destination surface, copies it into the published image and increments the frame counter. Invalid indices and shader execution failures abort publication for the frame. Staging protects the published image even when a fragment fails after earlier draws have completed. `ol3d_readback` reads the published image. These operations are implemented in `c/lib/gfx3d/render/ol3d.c`.

The resource API offers borrowed draw data and owned buffers:

- `ol3d_draw_indexed` borrows vertex, index and binding storage for the duration of its call.
- `ol3d_buffer_create` copies geometry into a CPU buffer. Changing its contents increments its version. `ol3d_draw_buffers` compares buffer views with the current versions and reports `OL_STALE_RESOURCE` for an outdated view.

`ol3d_pipeline_create` copies both shader programs and raster state into a pipeline object. The source shader objects can then be destroyed. Uniform arrays and texture bytes remain caller supplied resources. Buffer and pipeline handling lives in `c/lib/gfx3d/render/ol3d_resources.c`.

## LSL and its compiler

LSL programs begin with `lsl 1 vertex` or `lsl 1 fragment`, followed by interface declarations and `void main()`. The compiler and reflection interfaces are declared in `c/lib/gfx3d/include/openlogit_lsl.h`.

Declarations assign numeric locations to inputs, outputs and uniforms. Samplers use binding indices. Vertex programs assign `gl_Position`; fragment programs assign a `vec4` output at location 0. A uniform `mat4` occupies four consecutive uniform slots.

The language supports initialized locals, assignments, arithmetic, scalar float comparisons, numeric constructors and component swizzles. Numeric values use `float`, `vec2`, `vec3` and `vec4`; local conditions use `bool`. Matrices and samplers are uniform bindings. Arithmetic supports scalar and vector operands, while assignments require matching types.

Builtins include `texture`, `dot`, `normalize`, `sin`, `cos`, `abs`, `min`, `max`, `step`, `clamp`, `mix` and `select`. The arguments to `select` are evaluated eagerly. Its candidate expressions must both execute successfully, including arithmetic and texture operations.

The frontend scans bounded source text, parses declarations and expressions, resolves symbols and checks types before emitting OLS-IR. The implementation is divided among the files under `c/lib/gfx3d/shader/lsl/`, including `c/lib/gfx3d/shader/lsl/lexer.c`, `c/lib/gfx3d/shader/lsl/parser.c`, `c/lib/gfx3d/shader/lsl/expressions.c`, `c/lib/gfx3d/shader/lsl/symbols.c`, `c/lib/gfx3d/shader/lsl/types.c` and `c/lib/gfx3d/shader/lsl/emit.c`.

Emission accounts for the interpreter’s four-component registers. Scalars are replicated across the components. Short vectors receive padding masks, and division pads unused divisor components with one before taking reciprocals. This prevents an unused zero component from making a valid vector division fail. Temporary registers are reused; reassignment releases the register holding the previous value. These rules appear in `c/lib/gfx3d/shader/lsl/emit.c`, `c/lib/gfx3d/shader/lsl/operators.c` and `c/lib/gfx3d/shader/lsl/symbols.c`.

`ol_lsl_compile` returns an object owning instructions and reflected bindings. `ol_lsl_pipeline_create` matches fragment inputs with vertex outputs by location and type, then checks overlapping uniform ranges across stages. Its `ol_lsl_raster_state` exposes backface culling, depth testing, depth writes and blending. Programs obtained through `ol_lsl_ir` can also be used with the raw pipeline interface.

Source diagnostics use lines beginning at 1. Typed linkage errors and failures in the compiler’s final IR validation use line 0. Direct IR validation reports instruction numbers beginning at 1 for instruction errors. The distinction is implemented in `c/lib/gfx3d/shader/lsl/compiler.c`, `c/lib/gfx3d/shader/lsl/link.c` and `c/lib/gfx3d/shader/ir/ols_validate.c`.

## OLS-IR execution and textures

LSL and the legacy text assembler produce version 1 `ol_shader_program` objects. The assembler in `c/lib/gfx3d/shader/ir/ols_compile.c` accepts `ols 1 vertex` and `ols 1 pixel` programs. The guest compiler programs in `c/apps/coreutils/lslcc.c` and `c/apps/coreutils/olscc.c` write the resulting program structure to a bytecode file.

The shared validator checks instruction operands, register initialization, stage rules and the required position or color output. Draw validation also checks bindings. The interpreter in `c/lib/gfx3d/shader/runtime/ols_vm.c` executes instructions in order over four-component registers. A reciprocal of zero, a reciprocal square root of a nonpositive value or a nonfinite instruction result produces a rendering error.

Textures describe caller supplied RGBA8 bytes, dimensions, stride and storage size. Sampling supports nearest or bilinear filtering, with repeat or edge clamping. Mip chains also occupy caller storage; `ol3d_texture_build_mips` generates smaller images by averaging source texels.

Triangle rendering estimates texture detail from neighboring positions. When a bound texture requests mip filtering and neighbor interpolation succeeds, the interpreter executes the fragment program at the horizontal and vertical neighbors to record each texture instruction’s coordinates. The target pixel then uses those coordinate differences to select mip levels. This includes coordinate calculations performed inside the shader.

Mip filtering selects the nearest level or blends adjacent levels. Combining level blending with bilinear sampling produces trilinear filtering. The implementation is in `c/lib/gfx3d/shader/runtime/ols_vm.c`.

## Scenes, lighting and deformation

Scene helpers produce matrices and geometry consumed by the renderer.

`ol3d_scene_matrices` evaluates arrays of `ol3d_node` values containing translation, quaternion rotation, scale and a parent index. A parent must precede its children, enabling world matrices to be computed in one forward traversal. Matrix operations, perspective projection, camera orientation and normal transforms live in `c/lib/gfx3d/scene/ol3d_transform.c`.

The interface in `c/lib/gfx3d/include/openlogit_scene.h` adds sphere, torus, tube and plane generation, an orbit camera, viewport rays, nearest triangle picking, planar shadow projection, pose blending and mesh skinning. Generated meshes place position at attribute 0, normal at attribute 2 and UV coordinates at attribute 3.

`ol3d_skin_mesh` accepts four joint influences per vertex, transforms normals with each bone’s inverse transpose and normalizes the result. It stages the output mesh before copying it to the caller. A bad weight or cancelling normal discovered late in deformation returns an error before that copy. The implementation is `c/lib/gfx3d/scene/skin.c`.

Applications assemble lighting and surface appearance through shaders and uniforms. In `examples/openlogit/studio/shaders.h`, the vertex shader computes diffuse and specular terms, and the fragment shader combines interpolated lighting with a texture and fog. Computing lighting at vertices reduces the software fragment workload. `examples/openlogit/studio/scene.c` connects these shaders to generated meshes, pose blending, buffer updates and projected shadow draws.

## Programmable image materials

An `ol_material` represents a programmable pass over an image. It shares the OLS-IR fragment interpreter and resource bindings with triangle rendering. Its inputs describe the current pixel:

| Input | Value |
| --- | --- |
| 0 | `(u, v, 0, 1)`, normalized coordinates sampled at pixel centers, with the origin at the top left. |
| 1 | `(x + 0.5, y + 0.5, width, height)`, in target pixels. |

`ol_material_create` validates and copies the program. A material context allocates a reusable RGBA staging image. `ol_material_render` validates bindings, shades the image and uploads it to a matching OpenLogit surface after the pass succeeds.

Staging permits the output surface’s previous image to serve as a texture input: every invocation samples that image before publication. Colors use straight RGBA8 components, clamped and rounded during conversion. The API and implementation are `c/lib/gfx3d/include/openlogit_material.h` and `c/lib/gfx3d/material/ol_material.c`.

## Rendering into 2D surfaces

`ol3d_end` accepts an `ol_surface` with matching dimensions. Publication calls `ol_surface_upload`, which copies the pixels and advances the surface generation in `c/lib/gfx/core/ol_device.c`.

The application can record the surface as a layer with `ol_cmd_layer`, applying a 2D transform, clipping and opacity. Record commands after uploading their source image: commands retain the surface generation, and a later upload causes submission of the earlier reference to return `OL_STALE_RESOURCE`. Recording and generation checks live in `c/lib/gfx/core/ol_record.c` and `c/lib/gfx/core/ol_exec.c`.

For window presentation, `ol_window_composite` submits the surface through `SYS_GUI_BLIT`; `ol_window_present` issues `SYS_GUI_FLUSH`. The kernel draws the submitted image into the application canvas and marks the window’s content as damaged on flush. The wrappers are in `c/apps/gui/openlogit_window.h`, and the syscall handlers are in `c/kernel/gui/wm.c`. `examples/openlogit/island.c` uses this path to combine a rendered scene with window text and interface drawing.

## Performance

Rendering cost follows vertex count, projected triangle area and shader work. Each draw allocates a transformed-vertex cache and executes the vertex program for every supplied vertex. Rasterization visits each triangle’s screen bounding rectangle, checks coverage and depth, then shades surviving fragments. Mip filtering can add two complete fragment-program evaluations per shaded pixel.

Frame storage costs 12 bytes per pixel for two RGBA images and a depth buffer. At 4096 × 4096, these arrays total 192 MiB, calculated from the allocations in `c/lib/gfx3d/render/ol3d.c`. Surface storage, textures, geometry and temporary draw caches add to that amount. Beginning a frame clears the full target; successful publication copies the full image into the context’s published storage and, when supplied, the destination surface.

`ol3d_stats` exposes cumulative counts for input triangles processed, triangles produced by clipping, covered fragments before depth rejection, successful fragment shading and completed frames. The work counters also accumulate work performed during frames that later fail; the frame count advances on successful `ol3d_end`.

`examples/openlogit/island.c` records guest durations spanning scene rendering, interface drawing and window submission, then reports median and 95th percentile durations over 120 submissions.

## Limitations

- The backend executes on the CPU. The window transport provides no GPU or display-completion fence; frame counters and submission timings do not measure frames shown.
- Render targets and base texture dimensions are bounded at 4096 per axis. A draw accepts up to 65,536 vertices and 196,608 indices, with eight attributes, eight varyings, eight textures and 256 uniform slots.
- Each shader has 64 registers and up to 256 instructions. LSL accepts up to 128 KiB of source, 128 symbols, 64 reflected bindings and 64 expression nesting levels.
- LSL implements a restricted grammar without loops, user functions, recursion, arrays, conditional statements, ternary expressions or swizzle assignments. Matrix multiplication supports a uniform `mat4` multiplied by a `vec4`.
- Material passes use base-level texture sampling. Color conversion clamps components without an sRGB transfer.

These bounds are defined in `c/lib/gfx3d/include/openlogit_3d.h`, `c/lib/gfx3d/shader/lsl/lsl_internal.h` and the renderer, parser, interpreter and material implementations.

## Test coverage

The tree defines the following checks. Their assertions and recipes describe coverage; no builds or tests were run for this documentation review.

| Target | Coverage in source |
| --- | --- |
| `test-openlogit-3d` | Shared-edge coverage, clipping, depth, perspective interpolation, texture sampling, buffer versions and publication after failures, in `tests/unit/openlogit_3d_test.c`. |
| `test-openlogit-lsl` | Numeric execution, reflection, typed linkage and diagnostics, in `tests/unit/openlogit_lsl_test.c`. |
| `test-openlogit-material` | Pixel coordinates, sampling, staged publication and 2D composition, in `tests/unit/openlogit_material_test.c`. |
| `test-openlogit-scene` | Mesh generation, camera rays, picking, shadows, deformation and example shaders, in `tests/unit/openlogit_scene_test.c`. |
| `test-gfx3d-raster-state` | Depth comparisons, culling, depth write masking and composition operators, in `tests/unit/ol3d_state_test.c`. |
| `test-gfx3d-tex-mipmaps` | Mip generation, level selection, level blending and reduced aliasing, in `tests/unit/ol3d_mip_test.c`. |

The recipes require negative controls that disable or alter the behavior under test. Guest example harnesses are wired through `tests/openlogit_sdk.mk`.

## Source map

| File | What it contains |
| --- | --- |
| `Makefile` | Kernel source selection and application graphics objects. |
| `c/lib/gfx3d/include/openlogit_3d.h` | Triangle, shader, resource, texture and transform interfaces and bounds. |
| `c/lib/gfx3d/render/ol3d.c` | Frame storage, clipping, rasterization, depth, interpolation and publication. |
| `c/lib/gfx3d/render/ol3d_resources.c` | Owned buffers, pipeline snapshots, binding checks and mip generation. |
| `c/lib/gfx3d/include/openlogit_lsl.h` | LSL compilation, reflection and pipeline linkage interfaces. |
| `c/lib/gfx3d/shader/lsl/parser.c`, `c/lib/gfx3d/shader/lsl/expressions.c` | Declaration, statement and expression grammar. |
| `c/lib/gfx3d/shader/lsl/lexer.c`, `c/lib/gfx3d/shader/lsl/lsl_internal.h` | Source scanning, diagnostics and compiler budgets. |
| `c/lib/gfx3d/shader/lsl/types.c`, `c/lib/gfx3d/shader/lsl/symbols.c` | Type compatibility, names and assigned values. |
| `c/lib/gfx3d/shader/lsl/emit.c`, `c/lib/gfx3d/shader/lsl/operators.c`, `c/lib/gfx3d/shader/lsl/builtins.c` | Register allocation and expression lowering. |
| `c/lib/gfx3d/shader/lsl/compiler.c`, `c/lib/gfx3d/shader/lsl/link.c` | Compilation, reflection and stage compatibility checks. |
| `c/lib/gfx3d/shader/ir/ols_compile.c`, `c/lib/gfx3d/shader/ir/ols_validate.c` | Legacy text assembly and instruction and binding validation. |
| `c/lib/gfx3d/shader/runtime/ols_vm.c` | Instruction execution, texture sampling and mip selection. |
| `c/lib/gfx3d/include/openlogit_scene.h` | Mesh, camera, picking, shadow and deformation interfaces. |
| `c/lib/gfx3d/scene/mesh.c`, `c/lib/gfx3d/scene/camera.c` | Mesh generation, orbit camera, picking and planar shadows. |
| `c/lib/gfx3d/scene/ol3d_transform.c`, `c/lib/gfx3d/scene/skin.c` | Matrices, hierarchy evaluation, pose blending and skinning. |
| `c/lib/gfx3d/include/openlogit_material.h`, `c/lib/gfx3d/material/ol_material.c` | Programmable image passes and staged publication. |
| `c/lib/gfx/core/ol_device.c`, `c/lib/gfx/core/ol_record.c`, `c/lib/gfx/core/ol_exec.c` | Surface upload, layer recording and generation checks. |
| `c/apps/gui/openlogit_window.h`, `c/kernel/gui/wm.c` | Pixel transport and window presentation handling. |
| `c/apps/coreutils/lslcc.c`, `c/apps/coreutils/olscc.c` | Guest shader compiler programs. |
| `tests/openlogit_sdk.mk`, `tools/mkopenlogit_sdk.py` | SDK archive construction, installation and guest example targets. |
| `examples/openlogit/studio/scene.c`, `examples/openlogit/studio/shaders.h` | Scene assembly, deformation and programmable lighting. |
| `examples/openlogit/island.c` | Rendering, window composition and guest submission timing. |

## See also

- [OpenLogit 2D: geometry, rasterization and canvas](openlogit-2d.md)
- [Using OpenLogit from applications](openlogit-sdk.md)
- [Graphics and windowing](../architecture/graphics-and-windowing.md)