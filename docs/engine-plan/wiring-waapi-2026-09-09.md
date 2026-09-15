# WAAPI presentation wiring — 2026-09-09

The previous `js_anim.c` prelude interpolated values behind a
`getComputedStyle()` proxy. It never advanced a playing Animation's local time
or submitted those values to the painter; `finished` resolved immediately.
The old interpolation gate's margin-left assertions proved math, not rendered
motion. Its historical comment is retained beside the correction.

`css_anim_active/next_due/tick` now drives WAAPI through the existing page
queue. `js_page_run_due` budgets the sampler as a JS entry. Native frames write
`cstyle.opacity` and `xraw[XR_TRANSFORM]`; the existing
`css_anim_needs_layout` consumer rebuilds opacity's display-list snapshot or
repaints the live transform. No second timer, browser loop, or inline-style
mutation was introduced.

`css_anim_snapshot` removes our overlay before cascade. `css_anim_note` restores
unaffected scoped styles before CSS sampling, then replays the last native WAAPI
frame without entering JS. This matters: a first test with an unrelated scoped
restyle restored opacity 128 instead of 255 on cancel and left an invalid
transform pointer. Comparing the current style allocation and replaying from
owned values fixes both. Entries retain JS wrappers and resolve their live
nodes afresh; close removes borrowed transform pointers and frees references
before `js_dom_cleanup` and runtime teardown.

## Accepted subset and explicit boundaries

- Numeric opacity and absolute 2D transforms: translate X/Y, scale X/Y, rotate,
  skew, matrix, and `none`. Keyframes require explicit 0 and 1 endpoints for each
  animated property. Timing supports duration, delay/endDelay, iterations,
  easing, direction, seek, pause/play, reverse, playback rate, finish and cancel.
- `finished` settles at completion; cancel rejects an outstanding promise with
  `AbortError`. Basic finish/cancel listeners and handlers are queued after
  sampling. This is not complete EventTarget/options or pending-play-task
  conformance. `ready` resolves because play/pause are synchronous in this
  implementation.
- Other properties, relative/percentage transforms, 3D transforms, unresolved
  keywords, pseudo targets, implicit underlying keyframes and non-replace
  composition throw `NotSupportedError`. `commitStyles` also throws instead of
  its former no-op success. Full compositor stacking, scroll timelines, and
  effect replacement semantics are outside this change.
- Native capacity is 128 target entries and 2047-byte sampled transforms.
  Capacity/value overflow leaves CSS base styling and emits one diagnostic.
  These bounds are not a promise to paint an unbounded animation list.

Timing and effect-stack reference: [W3C Web Animations Level 1](https://www.w3.org/TR/web-animations-1/), especially
[timing model](https://www.w3.org/TR/web-animations-1/#timing-model) and
[applying the composited result](https://www.w3.org/TR/web-animations-1/#applying-the-composited-result).

## Reproduction and evidence

```sh
make BUILD=build test-waapi-paint-negctl
make BUILD=build test-waapi-paint
make BUILD=build test-waapi-paint-asan
make BUILD=build test-css-anim test-page-runtime test-mk-wired
```

The positive target requires its negative control. `JS_WAAPI_NO_PAINT` retains
real JS timing but disables native commit: 34 checks, 7 failures, including
`WAAPI midpoint reaches display-list opacity` and
`WAAPI midpoint reaches painter transform`. The positive runs 34 checks with
zero failures. The display list and painter's parsed transform are independent
of the computed-style proxy. Host glyph advances are approximate and no host
screenshot is claimed.

`test-css-anim` now measures the same seven easing inputs using supported
transform values, including extrapolation -0.3 and 1.5. Its clamping control
still fails those two cases. It additionally checks unsupported-property
refusals. It reports 20 checks, zero positive failures. `test-page-runtime`
reports 45 checks, zero failures.

Logs are under `build/wiring-next/waapi*.log`; raw negative output is
`build/wiring-next/waapi/negctl.log`. A combined ASan/UBSan exploratory build
reported pre-existing QuickJS diagnostics (signed left shift in
`quickjs.c:33791`, infinity-to-int in `quickjs.h:577`), so that run is not called
UBSan-clean. The dedicated address-sanitizer target disables leak detection on
Darwin, which does not supply it.

Guest page: `tests/fixtures/engine-expansion/waapi.html`. It moves a blue box
240 px over 2400 ms and fades it from .15 to 1. Automatically emits
`WAAPI-MID`, `WAAPI-FINISHED`, then `WAAPI-WIRING checks=4 failures=0` based on
actual time/state assertions. `WAAPI-CANCEL-BUTTON clientX=... clientY=...`
provides a content-coordinate click target. Pause/resume, reverse, cancel and
replay buttons permit independent interaction checks. The markers do not prove
painting: guest screenshots must independently show midpoint/end positions
and cancellation restoring the opaque box to the left. Root owns that guest
run and the production ELF/disk build; neither is claimed by these host gates.

### Guest-discovered checkpoint correction

The first guest run reached `WAAPI-MID` but never delivered the completion
marker. The old `run_js("a.finish()")` host assertion drained jobs itself and
masked that automatic clock completion had no checkpoint. A new constant-value
animation with zero timers now requires both its finished reaction and finish
handler, and requires a nonzero work result so its DOM change is settled.
Without the fix it produced exactly two failures. `js_page_run_due` now runs
`js_dom_run_jobs` after `css_anim_tick`, inside the interrupt slice, regardless
of pixel changes. `test-waapi-checkpoint-negctl` is a prerequisite of the
positive test. Final count is 39 positive checks; pure ASan also passes 39.

Unified guest confirmation: automatic completion reaches
`WAAPI-WIRING checks=4 failures=0`; a native mouse click on Cancel restores the
box's start position and opacity. Both screenshots and the serial sequence are
retained in [the integration record](general-browser-progress-2026-09-09.md).
