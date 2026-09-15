# OpenLogit SDK 1.1

OpenLogit is LogitOS's native graphics platform: devices, resources, render
targets, command submission, programmable 2D/3D and shared animation. Its role
is comparable to DirectX/OpenGL on other systems; its public ABI is OpenLogit,
with no DirectX/OpenGL binary or shader-language compatibility implied. UI
toolkits and applications consume it and keep their own layout/input/state.
The kernel links the 2D, display and animation core. Software 3D, screen-space
materials, the LSL compiler and OLS-IR execution are user-space library modules. GPU
acceleration is unavailable; query capabilities instead of assuming it exists.

## Compile and play inside LogitOS

    /bin/mkdir /tmp
    /bin/tcc /usr/share/openlogit/island.c -I/usr/include/openlogit -lopenlogit -o /tmp/island
    /tmp/island

The installed `/bin/island` is the same example built by the host toolchain.
`--low` selects 320x180; default is 640x360. P switches quality. WASD moves,
arrows orbit the camera, Space jumps, Esc pauses/resumes, R restarts. Collect
all three crystals. Jump at the ends of the first and middle islands. Falling
returns the player to the start while retaining collected crystals.

The example uses versioned vertex/index buffers, an immutable pipeline, custom
LSL vertex/fragment shaders, a node hierarchy, SDK quaternion/spring animation and SDK
window composition. Physics is game code. It does not contain a rasterizer.

For an editable 2D motion/materials example:

    /bin/tcc /usr/share/openlogit/effects.c -I/usr/include/openlogit -lopenlogit -o /tmp/effects
    /tmp/effects

Space retargets the moving card, B toggles blur, Esc closes. It uses a retained
offscreen group, shared spring, translucent layer, shadow and backdrop glass.
Its background is cached; each frame restores old/new effect bounds before
drawing the new position. Completed/hidden animation stops rendering.

For a programmable UI material workbench, run `/bin/materials`, or compile:

    /bin/tcc /usr/share/openlogit/materials.c -I/usr/include/openlogit -lopenlogit -o /tmp/materials
    /tmp/materials

The workbench has a progress control, pausable loading shimmer, press ripple,
texture tint toggle, warmth slider, reset and keyboard focus. Mouse controls
and P/L/Space/C/R shortcuts act on the same state; Tab/Enter and slider arrows
are also supported. Edit `ui_shaders.h` and rebuild to change the actual pixel
programs. Every animation samples the shared SDK timeline at one caller time.
Unchanged materials are cached; idle, minimized and reduced-motion UI does not
continuously render. The example has a fixed 760x540 logical-point layout.

## Which header

For native 2D drawing and cached composition, run `/bin/vector-studio`.
`vector/README.md` documents guest compilation, the vector/atlas/layers pages,
and the state, clipping and damage contracts.

For an interactive 3D scene and three-page inspector, run `/bin/scene-studio`.
The installed `studio/README.md` contains guest build instructions and controls.
Its sphere, torus and skinned mesh use LSL lighting, fog, texture sampling,
projected shadows, picking and shared animation. Source files are organized in
the `studio/` subdirectory and installed with that structure preserved.

- `openlogit.h`: transactional 2D devices, surfaces, lists, images, damage.
- `openlogit_canvas.h`: saved drawing state, affine geometry, cropped sprites,
  clamped atlas filtering, nine-slice panels and transparent rectangle clearing.
- `openlogit_scene2d.h`: ordered cached layers, old/new damage restoration and
  unchanged-frame suppression. Caller owns layer resources and their lifetime.
- `openlogit_draw.h`, `openlogit_bitmap.h`: checked borrowed pixel/mask adapters.
- `openlogit_display.h`: opaque native display targets, scaled composition,
  glyphs, shadows, blur and glass. Caller serializes borrowed targets.
- `openlogit_anim.h`: caller-clock timelines, curves, quaternion interpolation,
  analytic springs and continuous retargeting; no internal event loop.
- `openlogit_3d.h`: software render targets, owned buffers/pipelines, textures,
  depth, indexed drawing, camera/scene/skin transforms, OLS compiler.
- `openlogit_scene.h`: parametric meshes, orbit rays, triangle picking, planar
  shadows, pose blending and mesh skinning with transformed normals.
- `openlogit_ui.h`: reusable pointer capture, focus, spring feedback and native
  button/toggle/slider/progress geometry; caller owns layout and text.
- `openlogit_lsl.h`: typed LSL source compilation, reflection and stage linkage.
- `openlogit_material.h`: immutable pixel programs, reusable screen-space pass
  contexts and atomic output into ordinary 2D surfaces, using the OLS VM.
- `openlogit_window.h`: LogitOS logical-point window transport and presentation.

## Resource and frame contract

API 1.0 descriptor layouts and entry points remain accepted by 1.1. Opaque
object storage sizes are queried at runtime. `ol_surface` has separate front
and work RGBA8 buffers. A list retains resource references and recorded
versions; reset/destroy releases them. An image update invalidates old recorded
versions. Submission rejects stale resources before modifying the visible
front. `ol_submit_damage` reports the actual union copied; `ol_damage_move`
includes old and new bounds plus effect expansion.

RGBA8 uses straight alpha and byte order R,G,B,A. A8 contains coverage, no
color. Display targets are opaque 32-bit native color with explicit channel
positions, commonly BGRX. Cursor packing explicitly adds high-byte alpha.
A reusable image is not a render target. Borrowed raster/display calls are
immediate and do not provide a transactional command list; use native surfaces
when a producer needs last-frame preservation.

Original first-batch limitation: effects only used the borrowed display API.
Correction (2026-09-13): `ol_cmd_layer` and `ol_cmd_backdrop` now provide native
transactional effects. A layer retains its source surface and applies opacity
once after children have been composed. Blur/shadow use a box kernel of radius
0..64 destination pixels, premultiplied filtering, then straight RGBA output.
Clip is applied after effect expansion. Backdrop samples preceding commands in
the same frame and snapshots its read halo before writing filtered pixels.

Call `ol_submit_workspace_size` on a closed list and target, then allocate that
many aligned bytes and pass them to `ol_submit_workspace`. No allocation occurs
inside the core. Insufficient/aliased scratch or stale sources leave front and
completion unchanged. Ordinary `ol_submit` remains valid for scratch-free
lists; it returns `OL_LIMIT` when an effect needs workspace. Submission reports
front/work transfer bytes, excluding effect scratch and display-driver traffic.

`ol3d_begin` clears an unpublished work/depth target. Draw calls are synchronous;
borrowed bindings must stay valid for the call. `ol3d_end` publishes only after
every draw succeeds. Failure, including a late shader arithmetic failure,
retains the previous readable/output frame. Buffer updates require refreshing
the versioned view. Pipeline creation copies validated programs. Depth range is
0..1; clip volume is -w..w on x/y, 0..w on z. NDC front faces are CCW; screen y
is inverted by viewport mapping. Matrices are column-major. A node's parent
must precede it. Skinning accepts up to four normalized weights per vertex.

## Programmable 2D material passes

Query `ol_material_query_caps` with `caps.size = sizeof caps`. The software
extension reports `OL_CAP_PIXEL_MATERIAL` and atomic publication; core device
capabilities do not report this user-space feature. Existing 1.0/1.1 descriptor
layouts are unchanged. This pass and the OLS VM link independently of the 3D
triangle/depth context; the kernel does not link either user-space module.

Compile an OLS pixel program, create its immutable `ol_material` snapshot, and
create an `ol_material_context` matching the output surface dimensions. The
context allocates one reusable RGBA staging image once. `ol_material_render`
validates bindings, executes the program for the entire target, then uploads
once. It accepts `NULL` bindings for a program without resources. Targets range
from 1x1 to 4096x4096; padded RGBA8 target rows preserve padding. A material is a
reusable program, while the context and output surface are separate objects.

`input 0` is `(u,v,0,1)`, with pixel centers and a top-left origin. `input 1` is
`(x+.5,y+.5,width,height)` in target pixels. Outputs clamp and round to straight
RGBA8 without an implicit sRGB conversion. Uniform/matrix and RGBA texture
bindings use the same ABI, validation and sampler as mesh shaders. Binding
errors identify the instruction; arithmetic failure identifies the pixel.
Any failure leaves the previous target pixels and generation intact. Sampling
the old target as a texture is supported, because upload happens after every
pixel finishes. Keep all borrowed binding storage stable and exclusively own
the context/target throughout the synchronous call.

Compose the result via `ol_cmd_layer` with other surfaces, glyphs, clip,
transforms and opacity. Publication increments the ordinary resource version;
a list recorded before the update must be reset and recorded again. This is a
full-target pixel pass: use small effect targets and cache unchanged materials
as the workbench does. It is not a GPU queue, vblank fence or widget system.

## Animation ownership

Supply one monotonic nanosecond timestamp per frame. Timelines implement delay,
finite/infinite repeats, directions, pause/play, seek, negative rates and cancel.
Colors/positions/scales use vector tracks; quaternion tracks use normalized
shortest-arc interpolation. Springs preserve position and velocity when
retargeted. Stop animation wakeups when samples are inactive. Hidden windows
must not keep drawing. `ui.reduce_motion` is the system accessibility setting;
AUI, window transitions and the browser media query consume it. Game collision
and moving-platform simulation continue; decorative motion can be suppressed.

## LSL 1

`island_shaders.h` contains the game's editable typed source. Stages begin with
`lsl 1 vertex` or `lsl 1 fragment`. Use `layout(location=N)` for input/output and
uniform slots, `layout(binding=N)` for samplers, and `void main()` for execution.
Vertex code writes `gl_Position`; fragment code writes a `vec4` output at location 0.

    /bin/lslcc /usr/share/openlogit/passthrough.lsl /tmp/fragment.olsb

The first frontend supports numeric locals, assignments, scalar/vector arithmetic,
read swizzles, column-major `mat4 * vec4`, texture sampling and numeric built-ins.
`select(bool, yes, no)` evaluates both values. Loops, user functions, arrays, `if`
and ternary expressions are rejected. This is not full GLSL compatibility.
`ol_lsl_compile` and `ol_lsl_pipeline_create` return `OL_OK` on success and publish
no object on failure. Typed linkage checks varying types and uniform overlap.
Rendering still uses the same validated native instruction executor as OLS-IR.

## OLS-IR v1

    /bin/olscc /usr/share/openlogit/passthrough.ols /tmp/passthrough.olsb

A program starts with `ols 1 vertex` or `ols 1 pixel`. Registers are r0..r63;
constants are four finite floats. Instructions include const/input/uniform,
mov/add/sub/mul/mad, dot3/dot4, min/max, rcp/rsqrt, sin/cos, select, swizzle,
mat4, tex2d, position/varying/color. `ui_shaders.h` retains legacy IR examples.
`mat4 r0 r1 0` reads four uniform vec4 columns starting at slot 0;
`tex2d r0 r1 0` samples texture slot 0 using r1.xy. `select r0 r1 r2 r3` chooses
r2 or r3 componentwise according to r1>=0. `swizzle` uses an integer containing
four 2-bit component selectors. Texture bindings choose nearest/bilinear and
clamp/repeat. Mesh pixel inputs link to written vertex varyings; material
passes instead expose the two documented screen-space inputs.

There are no loops, recursion, arbitrary addresses or native callbacks.
Creation validates instructions and register use; drawing validates bindings.
Diagnostics include a source/instruction line. Invalid arithmetic aborts the
unpublished frame. The `.olsb` file is the fixed little-endian v1 program
structure for this x86_64 OS, not SPIR-V or a GPU binary.

## Validation boundary

Host numerical tests and guest display/play tests are separate gates. Submitted
frames are not displayed frames. The game's render-time log is not a display
FPS measurement. Whole-system migration/performance acceptance is tracked in
`docs/OPENLOGIT_SDK_IMPLEMENTATION.md` in the source repository.
