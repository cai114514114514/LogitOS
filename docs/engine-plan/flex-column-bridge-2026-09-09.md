# Definite column flex sizing reaches the DOM layout

The saved Deepseek specimen uses a fixed, inset-zero column menu with a
`flex:1;overflow-y:auto` body. In `build/site-general/layout/deepseek-00.css`,
line 3, zero-based Unicode char 29275 starts `.ds-mobile-menu`; char 29654
starts `.ds-mobile-menu-body`. The mobile menu may be hidden on desktop; this
is evidence of authored use, not a claim that the old desktop screenshot's
overlap was caused by column shrink.

The actual DOM consumer previously routed only rows into `layout_flex_run`.
Its column fallback stacked natural heights and enlarged boxes when spare
height existed. It never resolved flex-basis or negative free space. The pure
solver already tested columns, so those tests could pass while the real DOM
path ignored their results. The new DOM fixture observed a 400px scrolling
body where a 200px column minus a 40px header required a 160px scrollport.

For definite-height, nowrap columns, `layout.c` now establishes the available
cross width, measures each item's actual block content at that width, and
feeds those contributions to the existing flex solver. Its final used main
size is scoped through the item's layout before descendants are laid out.
That permits shrinking, correct clipping, nested columns, and descendant
percentages; merely resizing the background afterwards cannot do that.
Computed height is restored after this synchronous used-size scope.

The sizing rules follow [Flexbox section 9](https://www.w3.org/TR/css-flexbox-1/#layout-algorithm)
and its [definite-size rules](https://www.w3.org/TR/css-flexbox-1/#definite-sizes).
The old ROWS ONLY claim remains beside its correction in the code. Auto-height
and wrapping columns retain the existing fallback; orthogonal layout and
preferred aspect-ratio transfer are not newly claimed. There is one real trial
per item for content contributions, with the existing image/display-list
rollback. This is a correctness change, not a measured performance win.

## Evidence and integration boundary

`tests/flex_column_bridge.mk` defines `test-flex-column-bridge`, whose required
prerequisite is `test-flex-column-bridge-negctl`. The latter compiles the real
old column path with `LAYOUT_FLEX_COLUMN_LEGACY` and requires its scrollport
assertion to fail. It is not a mock solver or a fixture-specific production
branch. Initial unmodified behavior produced 41 checks / 13 failures; the
expanded longhand-based fixture initially produced 52 checks / 16 failures in
its legacy build and 52 / 0 in its positive build. After adding computed-style
restoration, repeat-layout and nested-column checks, the final result is
**59 checks / 0 failures; legacy 59 checks / 19 failures**, recorded in
`build/site-general/continue/flex-column/{test,legacy}.log`.

Coverage includes weighted growth and shrink, zero basis, min/max freezing,
shrink zero, padding/borders, percentage descendants, column-reverse, gap,
auto margins, zero definite height, automatic content minima, nested columns,
absolute descendants, clipping, and measurement rollback. Existing positive
DOM regressions pass: percentage-height 41, absolute-auto-height 39,
grid-percentage-item 18, inline-flex 36.

The first compile was blocked by a concurrent external diagnostic edit in
LibCSS `stylesheet.c` that dereferenced a void pointer. Per root's direction,
the existing host archive was copied to
`build/site-general/continue/libcss-host-validated.a`; its SHA-256 is
`6c7ab32ce19a567de5a70c572db5d2a70fde770dd847555cf387a512379a1456`.
The saved commands were derived from `make -n` and replace only that archive
path. **These results do not establish a successful build of all live sources.**
Root must finish the ordinary wired target, app/disk build, and guest check
after the external compile issue is resolved.

`tests/fixtures/engine-expansion/flex-column-bridge.html` emits
`FLEX-COLUMN-BRIDGE PASS` only after checking final CSSOM sizes and an
`elementFromPoint` hit at its input's center. It reports that native coordinate
and emits `FLEX-COLUMN-NATIVE <value>` on real input. This document does not
claim that guest fixture or actual menu interaction has run yet.

An adjacent parser defect was kept separate: on the saved archive,
`flex:1 1 0%` was discarded (computed grow 0, has_fb 0), while `flex:1` worked.
The layout gate uses equivalent explicit longhands to test basis consumption;
the runtime agent owns the shorthand parser investigation and its independent
negative control. No networking, `browser.c`, `browser_rt`, or `bfetch` changes
belong to this patch.
