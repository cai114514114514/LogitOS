# Grid rem/em font bases — 2026-09-09

The css-compat audit recorded the single-font parser contract: grid breadth parsed both em and rem using the grid container's font_px. This is now independently reproduced through DOM/CSS/layout rather than a pure parser call. Root20px/grid32px with tracks `2rem 2em 1fr` in 300px formerly measured 64/64/172; it must be 40/64/196. Implicit auto rows root24px/grid12px formerly gave a 24px 2rem row; it must be 48px.

Production adds `grid_parse_template_units` and `grid_parse_tracklist_units` with separate element/root bases. The scanner carries root_px through nested track parsing. Browser grid bridge reads the actual documentElement cstyle font; rem is not hard-coded to 16. Existing pure-parser entry points intentionally retain their same-font contract and delegate to the new API, preserving their callers. Other unsupported units remain outside this patch.

`make BUILD=build test-grid-font-units`: **10 checks / 0 failures**; prerequisite `LAYOUT_GRID_REM_AS_EM` restores the one-font bridge and prints **10 checks / 3 failures**. Logs: `build/site-general/layout/grid-font-baseline.log`, `grid-font-final.log`. Guest fixture `tests/fixtures/engine-expansion/grid-font-units.html` prints `GRID-FONT-UNITS PASS rem=40 em=64 fr=196`; guest rendering remains root's integration check.
