# Z.ai spacing: author values, layout consumption, and current limits

The new anonymous Z.ai home-page observation exposes a general CSS value gap:
the author requests calculated spacing, but the rendered flex/grid gaps and
logical padding are zero. The current fix consumes those values using the
existing typed length grammar and the element's computed font context. A
related ordinary block-padding percentage basis error is also corrected.
Real-site pixels after this fix remain the root agent's integration check.

## Current site evidence

The inspected image is `build-ai-sites-0913/zai-baseline/zai.late.png`;
`zai.json` and `zai.serial.txt` beside it record the completed anonymous home
page with heading, composer, three card images, and Sign in. The old inline
`aliyun_csp_inline_test_func` ReferenceError remains, but the page mounts and
paints. This is not evidence of an authentication or anti-bot rejection.

The public stylesheet linked by this document is retained at
`build-ai-sites-0913/zai-baseline/public-css/index-CpprTwXp.css`, SHA-256
`d96de011b7f3a18b57de65bd0b022b120ad6ba75daec826c0669a3a01e2d3427`.
It defines `--spacing:.25rem`, uses `calc(var(--spacing)*3)` for `.gap-3`
and `.px-3` logical inline padding, and uses the 2.5 multiplier for `.py-2.5`
logical block padding.

The last box dump at `zai.serial.txt:1271` and `:1292` shows:

| Actual box sequence | Measured gap |
| --- | ---: |
| Grid cards: x118/w316, x434/w315, x749/w315 | 0 px, 0 px |
| Flex category buttons: x118/w99, x217/w190 | 0 px |

These observations identify spacing value loss independently of resource
failures. They do not establish the correctness of every icon, font, animation,
button appearance, or login interaction.

## Product change

`css_extra.c` formerly parsed gap using the px/fr track parser and logical
padding using `one_px`. Both discarded rem/calc values. The new
`css_spacing_math.inc` shares the bounded affine grammar in
`css_inset_math.inc`, validates an entire declaration before publishing any
component, and retains stable source spans until per-element application.
The existing extension cascade merges each axis/edge independently, including
importance and declaration order. The compiled-rule filter and diagnostic
declaration count also consume the new fields, so a gap-only rule is retained.

Logical percentage padding retains an immutable px addend in `css.h`;
`layout.c::resolve_pad` combines it with the containing width and clamps a
calculated negative used value. The normal block branch now resolves padding
before computing the child's border width, using the parent's available width.
Previously a fixed 100 px child resolved 10% against its own width and repeated
layouts could feed old padding back into the next width.

Changed production files are exactly:

- `c/apps/browser/css_extra.c`
- `c/apps/browser/css_extra_cascade.inc`
- `c/apps/browser/css_spacing_math.inc` (new)
- `c/apps/browser/css.h`
- `c/apps/browser/layout.c` (padding resolver and normal block entry only)

The browser/module loader, live VM, verification behavior, and site scripts are
outside this patch. Source hashes are in
`build-zai-spacing-fix/production-hashes.json`.

## Verification

The ordinary local fixture in `tests/unit/css_spacing_math_test.c` uses the
shipping DOM → variable expansion → LibCSS/extension cascade → flex/grid/block
layout path. It contains no site markup, site scripts, images, or network calls.

- Actual pre-change source: **78 checks / 19 failures**; independent 12 px
  literal geometry passes. Evidence: `build-zai-spacing-fix/before.log`.
- Current expanded suite: **144/144**. It checks actual child coordinates,
  rem/em/calc/variables, complete-declaration rejection, importance and axis
  overrides, percentage bases, and repeated resize.
- The same ordinary cases pass **144/144 under ASan/UBSan**, with no reports
  (`detect_leaks=0`, `halt_on_error=1`). Log: `build-zai-spacing-fix/spacing-san.log`.
- Full old-behavior control: **exactly 36 failures**. Padding-basis-only
  control: **exactly 9 failures**. Both separately pass the **12/12 literal**
  control; the checker matches complete failure counters and exit status.
- At 400 → 800 → 800 px containing widths, calculated inline padding is
  **42 → 82 → 82 px**, and block padding **23 → 43 → 43 px**.
- Related normal suites pass: logical margins **76/76**, absolute auto widths
  **99/99**, button content boxes **65/65**, flex used height **75/75**, and
  extension cascade **22/22**. Their existing negative controls remain active.
- `test-mk-wired`: **300 fragments, 299 reachable, 1 declared**.

Primary gate log: `build-zai-spacing-fix/final-gates.log`.
The broad layout-box gate is **51 checks / 2 failures both before and after**,
with identical display-list count assertions and the same 8 painted rectangles
/ 10 box records. The actual saved pre-change source was recompiled with its
matching saved header; evidence is `build-zai-spacing-fix/old-layout-box.log`.
Those assertions were not altered. The comparison, sanitizer result and final
unchanged source hashes are recorded in
`build-zai-spacing-fix/verification-summary.json`.

The first draft fixture had a parameter name colliding with the reused `EQ`
macro's local variable. That harness issue was corrected before the recorded
78-check baseline; it is not counted as a product failure.

## Boundaries and next check

This patch adds nonpercentage typed gaps and typed logical padding. It does
not add percentage-gap cyclic sizing, physical padding calc parsing, vertical
writing logical mapping, cascade layers, or physical/logical competition
across the LibCSS/extension boundary. The normal block percentage basis is
tested here; no general claim is made about every float/grid percentage case.

The next immutable guest should confirm nonzero gaps between the three real
cards and between the category buttons, plus restored composer container
padding. Login dialog geometry and native Sign in/close behavior need their
own current observations. The September 10 report recorded a centered dialog
and a corrected 22×28 close control; its old build directory is absent in this
checkout, so those historical screenshots were not revalidated this turn.

The proposed missing Ctrl+L shortcut was ruled out: current `browser.c` already
normalizes the key into `c`, handles `c == 'l'`, selects the address, and prints
a fixed diagnostic. No shortcut product change was made.

## Release-v1 real-site follow-up: spacing not yet validated

The normal driver finished at `2026-09-13T12:41:51`; its artifacts are in
`build-ai-sites-0913/zai-after-v1/`. The inspected `zai.late.png` contains the
header/sidebar and Sign in, but not the heading, composer, or three card
targets. The result contains 21 late text runs instead of the baseline's 89.
All three diagnostic completion flags are false and the retained serial log
contains no complete boxes group. There are consequently no after coordinates
from which to measure the requested card/tag gaps or composer padding.

Both saved host HTML documents have SHA-256
`8251f5e5ef26f05c11ec88784b0bddca0e66a2e2cfd24975864f2a6d815e9edc`
and reference the same `prod-fe-1.1.93` stylesheet and module filenames. The
after run did not retain a separate stylesheet response body, so an independent
after CSS byte hash is unavailable. The live DOM/paint states are not comparable
even though the host document bytes and linked asset versions match.

`build-ai-sites-0913/zai-spacing-comparison-v1.json` retains the safe extracted
box metrics and version comparison. Baseline gaps are 0/0 px for the cards and
0/0/0/0 px for category buttons; baseline composer logical padding offsets are
also zero. **This run neither validates the spacing correction on Z.ai nor
establishes that spacing caused the missing main content.** No extra navigation,
request, VM connection, or product change was made by this audit.

## Later visible-r1 settlement: spacing measured, button defect separated

The earlier short-driver result above remains valid for its observation window.
The separately retained visible r1 guest eventually mounted the home page.
`build-ai-sites-0913/zai-live-r1/settled.png` and the latest complete
`serial.log:1620` box group (330 shown of 330) now establish these actual values:

| Target | Before | Settled r1 |
| --- | ---: | ---: |
| Card outer gaps | 0 / 0 px | 12 / 12 px |
| Category button outer gaps | 0 / 0 / 0 / 0 px | 12 / 12 / 12 / 12 px |
| Composer bottom child's left/top offsets | 0 / 0 px | 12 / 10 px |
| Textarea wrapper left/right insets | 0 / 0 px | 12 / 12 px |

The page width remains 1126 px; its viewport height is now 642 px rather than
562 px. These horizontal targets are comparable, while full DOM state and
absolute vertical positions differ. The public 1.1.93 stylesheet remains the
retained author-value source; no independent live-r1 CSS response hash was
collected. Safe extracted geometry is in
`build-ai-sites-0913/zai-spacing-settled-r1.json`.

All five category buttons retain their old widths and 24 px height even though
their padding is now applied. Their two text lines end 23 px below the controls.
This is a separate native automatic control-size defect, reproduced and fixed
with ordinary markup in
`docs/engine-plan/button-auto-metrics-fix-2026-09-13.md`. The heading ghost image
is outside that fix. Thus actual spacing now has guest evidence; general page
usability and final button pixels still need the next integrated guest.
