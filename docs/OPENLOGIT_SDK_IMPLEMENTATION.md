# OpenLogit SDK implementation ledger

User-approved scope (2026-09-13): all normal graphics consumers, one-way
compatibility, shared animation, programmable software 3D, playable third-person
game, guest-buildable SDK. **Whole-system acceptance remains incomplete.** GPU
is not available and is not reported as available.

## Artifact ownership and lost baseline

Original claim: build/output ownership was `build-openlogit-sdk`, before-state
snapshots were in `before/` and `baseline-sources.json`, and older API 1.0
artifacts were in `build-openlogit-0913`.

Correction (2026-09-13): those directories disappeared during parallel workspace
activity; their old images/logs cannot be used as current evidence. Sources were
retained. This task's rebuilt artifacts now live outside the scratch-build
pattern, at `/Users/wangzhe/system/openlogit-sdk-work`. `before-display/` preserves
only the framebuffer/material implementation before that individual migration
batch. It is **not** the complete original system/performance baseline.

## Implemented and exercised

- API 1.1 accepts 1.0 descriptors and entry points. Added versioned image/A8
  resources, transformed/opacity fills, path strokes, glyph masks, clipping,
  regional updates, damage unions and transactional regional submission.
- Native lists now retain offscreen groups with one group opacity, rectangular
  effect clipping, box blur, coverage-derived shadows and backdrop glass. Blur
  filters premultiplied color before returning straight RGBA. Backdrop snapshots
  earlier list results and initializes its external read halo before filtering.
  Caller-sized scratch is shared across commands; missing/aliased scratch,
  changed source versions or a later draw failure publish none of the frame.
- Raster backends take private targets/workspaces. Legacy `gfx_fill*`/clear
  functions are one-way SDK adapters. Glyph rasterization, SVG, icons, Canvas
  and browser paths use the SDK raster interfaces.
- Opaque display drawing, image scaling, glyph mixing, gradient/shape drawing,
  shadows, blur and glass now live under `c/lib/gfx`. Framebuffer entry points
  forward to them. WM resize/initialization, faded windows and cursor composition
  also use SDK pixel operations. Display/boot/panic transport remains lower-level.
- Normal GUI syscall transport in `logit.h` forwards through `openlogit_wire.h`.
  AUI/apps/browser/video callers thus enter the SDK without a per-widget full
  window copy. A frame still becomes visible on flush/present.
- Shared animation supports caller monotonic clocks, keyframes, scalar/vector/
  quaternion samples, directions, delay, repeats, pause/play/seek/rate/cancel,
  cubic/step easing and analytic springs with continuous retargeting. AUI,
  WM curves and CSS/WAAPI easing use it. Reduced motion reaches AUI, window
  transitions, Settings, CSS rules and existing MediaQueryList change listeners.
  Minimized AUI windows stop their animation deadline wakes.
  Preference changes invalidate the active CSS cache; activating a previously
  inactive document also refreshes its own cascade revision. Repeating the same
  preference causes no redundant style pass.
- User-space software 3D includes homogeneous clipping, top-left coverage,
  culling, perspective interpolation, depth and blending, texture sampling,
  indexed buffers with versions, immutable pipeline snapshots, camera/normal
  matrices, parent-first TRS hierarchy and four-weight skinning.
- OLS-IR v1 compiler/VM supports bounded straight-line vertex/pixel programs.
  Stage interfaces, registers, resource bindings and finite arithmetic are
  checked. Runtime failure retains the previous published frame, even after an
  earlier successful draw in the same frame.
- `Sky Islands` uses those actual APIs for the scene, character motion and HUD.
  It has moving platforms/collision/jumps/collectibles/fall reset/win/pause/restart.
  Vertex shaders do flat lighting; pixel shaders sample texture and apply glow
  and hit feedback. Both 320x180 and 640x360 targets are usable.
- Guest package contains public headers, `libopenlogit.a`, `/bin/olscc`,
  `/bin/island`, editable example sources and README. It is installed after the
  libc sysroot stamp so sysroot regeneration cannot silently delete the SDK.
- `effects.c` is a second guest-buildable consumer: a translucent card with
  shadow/blur, a cached backdrop and interruptible spring movement. Background
  restoration includes old/new effect bounds. Static gradients are rendered
  once, and unscaled native layers avoid general resampling. Its timing log
  measures guest render/submit work only. Both examples write diagnostic lines
  with one syscall so kernel output cannot split individual printf fragments.

## Current evidence

All paths below are relative to `/Users/wangzhe/system/openlogit-sdk-work`.
Negative-control FAIL lines in combined logs are expected; positive tests must
separately exit successfully.

| Evidence | Result / precise scope |
|---|---|
| `round2-build.log` | 25 native runtime, 22 animation, 18 API 1.1 and 28 3D numeric checks pass; API 1.1 and 3D also pass ASan/UBSan (leak sanitizer unavailable on this host) |
| `round2-host.log` | Canvas 112 checks and external PNG/base64 13 checks pass |
| `motion-final.log` | 13 checks: actual CSS cascade + JS MQL change behavior; missing-motion matcher control visibly fails both paths |
| `motion-context.log`, `motion-cache-neg.log` | 4 isolated-document cache checks; removed invalidation visibly fails both the active and previously inactive document assertions |
| `round2-final-build.log` | Full kernel/app/disk rebuild; clearRect saved-clip negative control fails the public Canvas readback assertion |
| `fb-bind-cache.log` | Framebuffer clipped drawing/presentation checks pass; same borrowed target is no longer rebound per wallpaper pixel |
| `consumer-gate.log`, `openlogit-consumers.json` | 39 files inventoried, forbidden old raster/backend entry points fenced; injected old call visibly rejected; make fragments wired |
| `island-guest/result.json`, `game-effects-regression.log` | 12 guest checks passed and complete: guest OLS/TCC compilation, actual scene, pause idle, resume, zero-death normal-input full game, restart, quality switch, minimized continuous-game render idle, normal window-switch restore, SDK compositor counter |
| `island-guest/victory.png`, `quality-640.png` | Real QEMU scanout, visually inspected; victory and changed-resolution scene |
| `openlogit-guest/result.json`, `clock-effects-regression.log` | 8 guest checks on the effects build: 100%/150% Clock display, moving hand, actual resize and new surface |
| `round2-usb-build.log` | USB held-key snapshots join common input; host composite-device/removal checks pass; USB guest run is blocked by xHCI Enable Slot timeout (before keyboard binding) |
| `motion-settings-guest/result.json` | 3 manual guest checks: actual Settings switch writes the preference, switch pixels change, Clock opens with preference enabled |
| `effects-final-build.log` | Latest host integration: 25 native runtime, 22 animation, 18 API 1.1, 21 native effects and 28 software 3D checks pass; effects/API/3D sanitizer runs pass; 4 motion-cache checks; negative controls visibly fail; 41 consumer files, 0 route violations; 313 make fragments audited |
| `effects-serial-build.log` | Kernel/app SDK integration retained; updated game and effects source compile and install, guest disk rebuilt with 398 files |
| `effects-guest/result.json`, `effects-guest.log` | 10 guest checks passed and complete: guest compilation, real native effects display, spring intermediate/endpoints, old-area cleanup, idle, blur on/off exact restoration, reverse/no-trails, live Settings reduced-motion propagation and endpoint-only rendering |
| `effects-guest/intermediate.png`, `moved.png`, `blur.png`, `reduced-motion.png` | Actual scanout artifacts; intermediate and reduced-motion endpoint visually inspected; synchronization uses the card's displayed position, not a submitted-frame counter |
| `effects-guest/metrics.json` | 27 guest render/submit samples, p50 92,273 us, p95 229,108 us; front/work transfers 274,720..805,120 bytes. Game verification was running concurrently. These are not display FPS, isolated performance results or evidence for the requested migration p95 criterion |

Input actions, QEMU command, serial output and image/archive hashes accompany
the game results. Submission and SDK invocation counters are **not** displayed
FPS. The game render-time samples are guest rendering timings, not a controlled
whole-system benchmark. The first exploration captured inactive output; only
later successful scanout artifacts above count as visual evidence.

Final route recheck still inventories 41 files with zero violations. A subsequent
`test-mk-wired` run passes 314 fragments / 313 reachable / 1 declared after another
workspace task added a fragment; the earlier build log's 313 count is historical.

## 2026-09-15: AUI scrolling and application wake integration

This batch lives in `/Users/wangzhe/system/openlogit-scroll-0915`; the September
13 artifacts above remain in their original directory. Before-source copies
for AUI, Chat and Monitor are in this batch's `before/`. This is a functional
scrolling comparison, **not** a replacement for the missing migration baseline.
`acceptance.json` binds the final source/image/client hashes to this batch's
21 scrolling, 28 Chat and 23 Gallery guest assertions. Gallery's retained
temporary serial/capture files are also copied into `gallery-guest/`.

- `aui_scroll_begin/end` now uses the SDK's caller-time pixel transition over
  180 ms. AUI retains pointer identity and separate displayed/target offsets;
  rapid notches accumulate, reversing starts at the displayed position, and
  changed offsets, drags, content/viewport changes and reduced motion cancel or
  finish the pending leg. Missing containers request no wakes and latch their
  displayed state when they reappear. Sixteen bounded slots retain no ownership
  of caller memory; applications must keep offset addresses stable.
- Scrollbar input is handled before content painting, its final thumb after
  painting. The gutter is reserved for the scrollbar. Previous clipped geometry
  routes a nested wheel event to one innermost container, including its gutter.
- Existing direct consumers are Chat transcripts, Monitor process tables,
  Settings' key table and Gallery lists/tables. Chat and Monitor were missing
  animation deadline handling; both now merge SDK wakes with their existing
  work deadlines. Chat's pending transcript updates remain subject to its
  40 ms budget; pure animation frames are separately counted without changing
  the meaning of the existing total `repaints` field. Monitor retains its
  one-second data refresh and no longer polls every 100 ms at rest.
- The source fence now rejects a direct AUI scroll/list/table consumer lacking
  the animation due/wait calls. Its injected missing-Chat-wake control visibly
  fails. This is a wiring check, not proof of arbitrary application-loop logic.
- A standalone GUI link exposed a compiler-emitted `memcpy` from copying the
  large paint descriptor. `ol_cmd_fill` now uses its existing bounded internal
  copy; both test clients link the real SDK and AUI without libc.

| Evidence relative to this batch directory | Result / scope |
|---|---|
| `final-host.log`, `final-gates.log` | 25 scroll state checks plus ASan/UBSan pass; 25 native, 22 animation, 18 API 1.1, 21 effects checks and AUI mask regression pass. Instant-scroll, frame-commit, blur, route and missing-wake controls visibly fail. 42 route files, zero violations; latest make audit: 340 fragments / 339 reachable / 1 declared |
| `scroll-final-guest.log`, `scroll-guest/result.json` | 21 guest checks: actual intermediate/endpoint scanout, instant-control failure, accumulated/reverse input, exact old-area restoration, idle, wheel interrupted by held thumb drag, short content, tiny clipped viewport, nested gutter routing, hide while moving/restore and live Settings reduced motion |
| `scroll-guest/artifacts.json`, `scroll-guest/command.json`, `scroll-guest/actions.json` | Image/client/source hashes, fixed QEMU command and normal input actions. The gate opts into test clients with `OPENLOGIT_SCROLL_GUEST=1` in the selected build |
| `chat-wrapper-final.log`, `ch-shots/` | 28 real Chat assertions pass: mock-response streaming, exact reply bytes and transcript repaint budget; idle Chat shows a translated intermediate transcript, completes a 48-pixel wheel leg without further input and restores exact pixels on reverse |
| `gallery-final.log`, `gallery-*.png` | 23 existing Gallery visual assertions pass on `disk_gallery.img`, including keyboard tab navigation, list/table page painting, shapes, focus, hover, shadows and modal scrim |

The first Gallery attempt used the normal disk, which intentionally omits that
demo; its missing-dock-app error was an apparatus error. The corrected run uses
the existing `GALLERY_AEX` mechanism and an independent Gallery disk. The initial
Chat assertion also mixed spinner frames with transcript work; the corrected
gate reports both and preserves the token/time limits for transcript frames.
The Chat wrapper now puts screenshots beside the selected disk rather than in
shared `build/ch-shots`.

Monitor's changed loop is built and included in the source-wake gate; its
one-second cadence is not a measured performance result. These runs do not prove
every scroll surface, every hidden application, display FPS or the p95 migration
criterion.

## Still required for the full plan

- [x] Native command-list layers/effects with retained sources and whole-frame
      failure preservation. The first-batch limitation above is superseded by
      `ol_cmd_layer`, `ol_cmd_backdrop` and explicit submit workspace APIs.
- [ ] Inventory and finish remaining menu/page/close animation consumers and
      scrolling outside the shared AUI container. AUI scrolling is covered by
      the September 15 batch above.
      Actual Settings-to-effects reduced-motion propagation and minimized game
      idle/restore are now verified; this does not prove every hidden consumer.
- [ ] Broader guest browser/Canvas/font/SVG/media, multi-window and scale coverage;
      the source route fence and Clock/game checks do not prove every consumer.
- [ ] USB guest input: common held-key API now includes USB and host tests pass,
      but current xHCI enumeration times out before binding. PS/2 guest gameplay
      passed; USB gameplay must not be reported as verified.
- [ ] Additional independent 3D acceptance for every clip plane, texture/address
      modes and skeletal animation, with corresponding negative controls.
- [ ] Fixed-QEMU guest frame-time distributions, actual displayed-frame/input/
      memory/damage measurements and a defensible pre-migration comparison.
      The requested 2D p95 <= +10% criterion has **not been established** because
      the original comparison artifacts were lost.

A completed module or gameplay gate does not mean this entire checklist passed.

## Programmable UI material batch — 2026-09-15

The owner's clarified positioning is a native OS graphics platform, comparable
in role to DirectX plus OpenGL. OpenLogit owns device/resource/target/pipeline/
submission/animation mechanisms. AUI and apps own widget identity, layout,
focus and events. This is not a DirectX/OpenGL compatibility claim.

`openlogit_material.h` adds a user-space, programmable screen-space pass. An
immutable pixel program shares OLS-IR, the VM, binding validation, uniforms,
matrices and texture samplers with software 3D. Built-in inputs are pixel-center
UV and device-pixel coordinates/target extent. A reusable context allocates
staging once; a complete pass uploads to an ordinary RGBA8 surface once. Late
execution failure retains prior pixels/generation. Sampling the previous output
as a texture is supported; recorded native lists observe ordinary generation
invalidation. Core 2D descriptors and layouts remain unchanged, and core device
caps do not claim the separate user-space material feature. No GPU is reported.

`/bin/materials` and editable `materials.c` / `ui_shaders.h` now provide a UI
material workbench: progress, pausable loading shimmer, press ripple, texture
tint, warmth slider, reset and Tab/Enter/arrow-key interaction. All shading and
composition use SDK paths; all motion uses SDK timelines with one frame time.
Each material caches its last uniform value, and continuous rendering stops
when idle/minimized/reduced. It uses a fixed 760x540 logical-point layout.

Original SDK packaging copied `logit.h` alone. Correction: a later text metrics
addition made that header depend on `text_metrics_wiring.inc`, but the SDK
installer had not included it. Guest TCC compilation exposed this in this batch.
The include is now packaged and tracked as a build dependency; installation
checks every quoted public include before copying anything into the sysroot.

Artifacts for this batch are `/Users/wangzhe/system/openlogit-materials-0915`.
`build.log` is its successful first full ISO/app/disk build. During incremental
repack, an active AetherScript change temporarily left the host compiler with
unresolved `at_class_*` symbols (`rebuild-sdk.log`); this task did not edit its
sources. `repack_sdk.py` uses the existing fsck snapshot/lifecycle guard and
atomic serializer to apply only the SDK manifest to that first built disk.
`sdk-repack.json` verifies 37 SDK paths and 368 byte-identical other files.
`disk-before-sdk.img` preserves the checked base. This tests the current SDK on
that isolated OS build, not all concurrent language edits in the shared tree.

Host evidence: `material-final.log` has 27 numerical and 27 ASan/UBSan checks,
with a disabled-VM control visibly failing the independent pixel-center oracle.
LeakSanitizer is excluded because Apple ASan does not support it. The positive
host binary links only the material pass, VM and 2D core, so a hidden dependency
on the triangle/depth module would fail the link. `module-boundaries.json` also
checks actual kernel/UI ELF symbols: the kernel excludes the user-space VM;
the UI includes the VM and material pass but excludes the triangle/depth context.
`host-final.log` preserves 25 runtime, 22 animation, 18 API 1.1, 21 effects,
25 scroll and 28 software 3D checks, their existing controls and sanitizer runs.
`routes-final.log` reports 43 source-route consumers with zero violations and
350 make fragments wired (349 reachable, one declared wrapper).

The shared-VM game regression (`game-final-rerun.log`, `island-guest/result.json`)
passes 12 guest checks, including OLS compilation, SDK source compilation,
normal-input zero-death victory, pause/resume/restart, both render resolutions
and minimize/restore. The first run exposed another existing packaging/tooling
trap: `olscc` used fragmented printf output, and boot diagnostics split its
instruction count inside a successful compiler summary. `olscc` now writes
formatted diagnostics/summary as one buffered line. `island-guest-split-log`
retains the interrupted apparatus failure; it is not a 3D render failure.

`material-guest/result.json` and `material-guest-final.log` complete 20 guest
checks: installed-source compilation, actual progress intermediates/endpoints,
shader shimmer pause/idle, ripple cleanup, pointer and keyboard uniform edits,
tint round-trip, reset, focus activation, minimize/restore and live reduced
motion. PNGs are real scanout; `initial-client.png` was visually inspected.
Earlier missing-header and one-step window-switch apparatus runs are preserved
in separate directories, not included in the passing result. `acceptance.json`
binds the positive reports and current artifact/source hashes to this batch.

Whole-system migration, all consumer runtime coverage and the original 2D p95
performance comparison remain incomplete. Material pass counts and application
frame logs are not displayed FPS or input-latency measurements.
