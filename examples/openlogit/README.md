# OpenLogit SDK 1.1

OpenLogit is LogitOS's drawing API. The kernel links the 2D, display and animation
core. Software 3D and OLS-IR execution are user-space library modules. GPU
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
vertex/pixel shaders, a node hierarchy, SDK quaternion/spring animation and SDK
window composition. Physics is game code. It does not contain a rasterizer.

For an editable 2D motion/materials example:

    /bin/tcc /usr/share/openlogit/effects.c -I/usr/include/openlogit -lopenlogit -o /tmp/effects
    /tmp/effects

Space retargets the moving card, B toggles blur, Esc closes. It uses a retained
offscreen group, shared spring, translucent layer, shadow and backdrop glass.
Its background is cached; each frame restores old/new effect bounds before
drawing the new position. Completed/hidden animation stops rendering.

## Which header

- `openlogit.h`: transactional 2D devices, surfaces, lists, images, damage.
- `openlogit_draw.h`, `openlogit_bitmap.h`: checked borrowed pixel/mask adapters.
- `openlogit_display.h`: opaque native display targets, scaled composition,
  glyphs, shadows, blur and glass. Caller serializes borrowed targets.
- `openlogit_anim.h`: caller-clock timelines, curves, quaternion interpolation,
  analytic springs and continuous retargeting; no internal event loop.
- `openlogit_3d.h`: software render targets, owned buffers/pipelines, textures,
  depth, indexed drawing, camera/scene/skin transforms, OLS compiler.
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

## Animation ownership

Supply one monotonic nanosecond timestamp per frame. Timelines implement delay,
finite/infinite repeats, directions, pause/play, seek, negative rates and cancel.
Colors/positions/scales use vector tracks; quaternion tracks use normalized
shortest-arc interpolation. Springs preserve position and velocity when
retargeted. Stop animation wakeups when samples are inactive. Hidden windows
must not keep drawing. `ui.reduce_motion` is the system accessibility setting;
AUI, window transitions and the browser media query consume it. Game collision
and moving-platform simulation continue; decorative motion can be suppressed.

## OLS-IR v1

    /bin/olscc /usr/share/openlogit/passthrough.ols /tmp/passthrough.olsb

A program starts with `ols 1 vertex` or `ols 1 pixel`. Registers are r0..r63;
constants are four finite floats. Instructions include const/input/uniform,
mov/add/sub/mul/mad, dot3/dot4, min/max, rcp/rsqrt, sin/cos, select, swizzle,
mat4, tex2d, position/varying/color. Examples in `island_shaders.h` are editable
text programs. `mat4 r0 r1 0` reads four uniform vec4 columns starting at slot 0;
`tex2d r0 r1 0` samples texture slot 0 using r1.xy. `select r0 r1 r2 r3` chooses
r2 or r3 componentwise according to r1>=0. `swizzle` uses an integer containing
four 2-bit component selectors. Texture bindings choose nearest/bilinear and
clamp/repeat. Pixel inputs must be linked to written vertex varyings.

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
