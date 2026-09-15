# Clock GUI

| Module | Responsibility |
| --- | --- |
| `layout.c` | Point-based responsive layout and device-pixel storage budget |
| `dial.c`, `geometry.h` | Dial paths, cached art, hands and SDK composition |
| `motion.c` | Wall-second to SDK transition adapter, wrap and resume semantics |
| `view.c`, `view.h` | Read-only application-state presentation and action reporting |

OpenLogit remains the only rasterizer and motion sampler. Static face art and
minute marks are recorded together and cached until resize/theme changes. One
transactional list restores that cached layer and draws all hands directly into
the composed target. One completed dial is sent through window transport,
and its bounds join AUI's label/control damage before presentation.
Intermediate hand samples update and present only the dial rectangle. Time
readout changes, input, control feedback, theme changes and resize use the full
UI recording path. This avoids clearing the whole window between tick endpoints.

This replaces the older Clock's multiple per-part clear/submit/blit sequences.
The 384 device-pixel face cap and its independently tested geometry budget are
retained. Rendering at the destination's device size preserves antialiasing at
fractional scales. The two caller-owned double-buffered targets require
2.25 MiB of pixel storage; they avoid transient allocation during resize.
The first revision used a separate hands target plus retained-scene composition.
It was replaced because that content changes every frame: caching it adds a
second full-face publication without reducing rasterization work.
The application applies this GUI's 240x240-point minimum through the normal
window-management API; the layout tests cover that bound through 300% scale.

The application passes wall time and a monotonic instant. A normal second tick
uses `ol_transition256`; 59 to zero moves clockwise through one tick. Missed
seconds or clock corrections snap to current time. The event loop publishes one
final frame when motion stops, so the hand never remains on the penultimate
sample. With seconds hidden, minute/hour changes still update the clock.

The GUI returns an action instead of writing clock preferences or scheduling
work. No alarm, timer, timezone service or application persistence lives here.
