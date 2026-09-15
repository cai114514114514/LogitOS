# OpenLogit 2D and animation core

| Directory | Responsibility |
| --- | --- |
| `include/` | Public SDK headers and stable API structures |
| `internal/` | Private software-backend declarations |
| `core/` | Devices, surfaces, command lists and frame submission |
| `canvas/` | Saved drawing state, affine canvas and nine-slice composition |
| `scene/` | Retained 2D layers, generation tracking and old/new damage |
| `ui/` | Shared control input, focus and animated visual feedback |
| `geometry/` | Fixed-point math, paths and stroke construction |
| `raster/` | Coverage rasterization, masks, paint and compositing |
| `effects/` | Blur, shadows and glass rendering |
| `animation/` | Caller-clock timelines, easing, springs and interpolation |
| `adapters/` | Drawing, image and display adapters; one-way legacy forwarding |

The software backend writes internal pixel targets. Compatibility entry points
forward to the SDK; they must not become a second rasterizer or be called back
from the backend. The user-space 3D compiler, VM and triangle pipeline live in
`../gfx3d` and are excluded from the kernel source list.

Use `GFX_SRC`, `GFX_HEADERS` and `GFX_INC` in build fragments. They discover
translation units and include directories recursively. Object rules create each
object's parent directory; listing only root-level C files would silently omit
the engine after this split. The installed SDK still exposes headers by basename,
so consumers include `openlogit.h` rather than repository-relative paths.
