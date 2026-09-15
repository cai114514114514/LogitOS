# OpenLogit 2D batch — 2026-09-15

This batch adds reusable 2D drawing state, atlas sprites, nine-slice panels,
cached layer composition and a native three-page Vector Studio consumer. Public
1.0/1.1 descriptor layouts remain unchanged. New source is separated into
`c/lib/gfx/canvas`, `c/lib/gfx/scene` and `examples/openlogit/vector`.

## Delivered behavior

- Save/restore transform, device clip, A8 coverage and opacity; vector fill,
  stroke and rounded rectangle recorders share the transactional command list.
- Versioned image crops with affine rotation/reflection; exact rectangular
  coverage; clamped premultiplied bilinear filtering; nine-slice composition.
  Legacy filter modes preserve their previous numerical behavior.
- Transparent rectangle replacement, ordered cached surfaces and source-version
  invalidation. Movement and hiding restore old/new bounds and recompose all
  overlapping layers. Failed recording does not advance displayed placements.
- Partial window transport packs damaged rows and clips native labels to the
  same region. Unchanged scenes skip submission. Shared timelines and UI
  feedback stop drawing while paused, minimized or reduced motion is active.
- Installed public headers, archive, `/bin/vector-studio`, three source files
  and guest build instructions. The Vector Studio link does not require 3D.

The consumer offers editable vector strokes/coverage, rotating atlas crops,
resizable nine-slice panels, draggable cached layers and layer opacity. It is a
fixed-size SDK workbench; it is not a new document editor or full browser Canvas
implementation. Font parsing/layout, image decoding and application state keep
their existing owners.

## Functional evidence

Artifact root: `/Users/wangzhe/system/openlogit-2d-0915`.

- `canvas-final.log`: 23 host assertions, repeated under ASan/UBSan. Independent
  pixel expectations cover state restoration, clipping, atlas crop edges,
  reflection, nine-slice seams, stale resources, layer movement/alpha/idle,
  failed-frame retry and aligned/odd-stride copies with padding sentinels.
- Disabling old-position damage produces the required visible
  `FAIL moving layer clears its previous position` negative control.
- `vector-guest/result.json`: 19 guest checks. TCC compiles the installed three
  sources; ordinary keyboard/mouse input changes screenshot pixels, exact
  restoration clears previous content, and pause/minimize/reduced motion stop
  continuous redraw. Screenshots include actual intermediate rotation frames.
- `scene-guest/result.json`: all 22 existing 3D Scene Studio guest checks pass
  with the final archive, including guest compilation, shader controls, picking,
  animation, pause/minimize and reduced motion. Its disk/archive hashes match
  the final Vector Studio and successful performance artifacts.
- `final-build.log`: SDK and a freestanding Clock link pass; effects, materials,
  scene/UI, gameplay, 1.1 and source-route tests pass with their negative controls.
  `regression-final.log` records the broader OpenLogit and GFX checks.

The source-route fence reports 53 files and zero violations. Its injected
legacy-renderer and missing-animation-wake controls fail as intended. This
static result does not certify every system screen at runtime.

## Performance acceptance, run after implementation

The benchmark uses the same compiled application object linked against the
preserved previous Scene SDK archive and the new archive. Both binaries run in
one fixed QEMU guest: x86-64 CPU max, four vCPUs, 1 GiB RAM, multithreaded TCG,
virtio GPU, 1280x800 desktop, no network. Each 600x400 client is moved through
ordinary titlebar input to the same desktop position (160,90). Run order
alternates between workloads.

Each run has 48 input-driven frames; eight warmups are retained but excluded,
leaving 40 timing samples. p95 uses nearest rank. Guest monotonic timestamps
bracket command recording, rendering, window copy, labels and present request.
Host pacing/screenshot time is excluded. Every displayed frame carries a unique
marker; surface checksums and complete client screenshot hashes match between
the archives for all 288 observed frames.

| Existing 2D workload | Baseline p95 | Final p95 | Change |
| --- | ---: | ---: | ---: |
| 24 gradient rounded controls | 20.000 ms | 16.587 ms | -17.07% |
| 24 scaled bilinear image tiles | 24.484 ms | 24.874 ms | +1.59% |
| Cached background and moving layer | 16.002 ms | 16.025 ms | +0.14% |

All three meet the +10% budget. Process RSS was 1047 pages in both control/image
runs, and 1047 versus 1048 pages in the cached-layer run (4 KiB pages).
Leading-clear frame initialization/publication traffic drops from 1,920,000 to
960,000 bytes per frame in the first two workloads. Cached damage traffic
remains bounded and equal across the two archives; raw rectangles are recorded.

### Failure retained and fixes

The first run remains in `perf-guest`: cached-layer p95 was 11.331 versus
15.587 ms (+37.56%), so that acceptance failed. That run also exposed an
apparatus error: automatic window cascading gave each variant a different
desktop position. It is retained as a failed diagnostic, not a valid isolated
estimate of SDK regression.

The final run fixes positioning and changes the real hot paths: word-sized
freestanding copies with an unaligned byte fallback; no old-frame initialization
before a leading full clear; direct opaque pixel replacement for unscaled,
unblurred cached layers. Partial-alpha blending and whole-list commit semantics
remain covered by independent pixel and failure tests. Because measurement
conditions and implementation both changed, the difference between the two
runs cannot be attributed solely to the optimizations.

`perf-optimized/result.json`, `artifacts.json`, `command.json`, raw guest serial
and six final screenshots retain the successful paired run. `tests/bench/`
contains the common application; `tests/boot/run-openlogit-2d-perf.py` drives the
display checks and p95 gate. Build the binaries with `openlogit-2d-perf-binaries`
and an explicit `OPENLOGIT_BASELINE_ARCHIVE`. The artifact `repack_sdk.py`
records how both `/bin/bench2d-*` binaries and the SDK were put into this disk.

### Evidence limits

The timings measure guest rendering/window submission after input dispatch,
not physical input latency or vblank completion. The 288 observed frames are
not a sustained FPS claim. Global compositor counters are included as raw
diagnostics, including existing late/torn counters; this batch does not claim
to eliminate compositor tearing. The three workloads compare this batch with
the previous Scene SDK, not an unavailable original pre-migration baseline.
Browser, media, all multiwindow scenes and real hardware need separate gates.

Whole-current-tree image builds encountered concurrent AS include and browser
compile failures (retained in `build.log` and `build-retry.log`). To keep the
comparison controlled, this run uses the previously verified Scene kernel/ISO
and overlays the newly built SDK onto its disk. `sdk-repack.json` proves 369
non-SDK files were preserved byte-for-byte and records the benchmark binaries.
This is a new SDK acceptance on that fixed kernel, not a successful build or
performance certification of the entire current shared operating-system tree.
