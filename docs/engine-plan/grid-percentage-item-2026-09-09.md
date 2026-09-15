# Grid item percentage widths, 2026-09-09

Bilibili's captured navigation has `repeat(9,1fr)` and content-box links with
`width:100%` and 1px borders. The former bridge resolved each link against the
whole 676px grid before intrinsic track sizing, so every fractional track grew
to roughly the whole grid's width. The three-column reduction reproduced this
as a 320px grid whose links were 322px each.

Percentage preferred widths now remain indefinite during column sizing. A
late callback resolves against the final grid area, including spanning gaps,
then supplies that border-box width to row measurement and self alignment.
It reuses the existing calc/box-sizing/min/max conversion instead of copying
CSS representations into the solver. Fixed widths remain definite and auto
widths preserve stretching.

`make BUILD=build test-grid-percentage-item`: 18 checks pass. Its prerequisite
negative control restores whole-container resolution and fails 6 checks:
322px versus 102px links; half-width 160px versus 50px; spanning calc 310px
versus 200px; min-width and border-box padding cases. The gate includes fixed
and auto controls. Host geometry is not a real-site success claim; root agent
owns disk rebuilding and guest replay. Card cover heights remain a separate
used-height absolute-containing-block defect.
