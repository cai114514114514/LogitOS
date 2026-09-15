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

## Still required for the full plan

- [x] Native command-list layers/effects with retained sources and whole-frame
      failure preservation. The first-batch limitation above is superseded by
      `ol_cmd_layer`, `ol_cmd_backdrop` and explicit submit workspace APIs.
- [ ] Inventory and finish remaining menu/page/close/scroll animation consumers.
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
