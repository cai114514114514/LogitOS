# display:contents and box-tree flattening — 2026-09-09

Apple guest baseline `build/site-general/baseline/apple/apple.png` shows Store/Mac/iPad/etc stacked vertically through the hero. Fetched authored CSS has `.globalnav-menu{display:contents}` and nested flyout/menu-list wrappers using `display:inherit`. DOM nesting puts the menu links several wrappers below the flex list. LibCSS had no contents keyword/enum/cascade value, so the wrapper retained its ordinary box and the child links never became sibling flex items.

Production adds contents to the actual LibCSS display parser and cascade, synchronizes the checked-in generated parser with properties.gen, and maps the computed value to DISP_CONTENTS. Layout flattens only its box-tree iterators (before / authored children / after), preserving real DOM parents and selector inheritance. Flex, grid, normal flow and intrinsic sizing use the same traversal. Percentage height skips contents ancestors because those do not establish a containing box.

Spec: https://www.w3.org/TR/css-display-3/#box-generation

Host `make BUILD=build test-display-contents`: initial **26/26 pass**, with prerequisite `LAYOUT_CONTENTS_LEGACY` **26 checks / 11 failures**. Concrete old geometry: first/nested 30px flex items x=109/109 from wrapper padding; new x=0/30, following sibling x=60. Grid children occupy separate 30px/50px tracks, no principal wrapper geometry, DOM parent and direct-child selector unchanged, before pseudo participates, display:none still wins its cascade. Logs: `build/site-general/layout/contents-baseline.log`, `contents-final.log`.

Guest fixture `tests/fixtures/engine-expansion/display-contents.html`: `DISPLAY-CONTENTS PASS item offsets=80,160 wrapper=0x0`. Root controls the shared disk build and real-site guest after image. Host geometry does not establish real Apple rendering or behavior.

Follow-up during integration: inspect uncommon replaced/control and root-element computed-display adjustments, nested before/after cases and boxless effects against CSS Display Appendix B. No site name, hostname, UA spoof or script-recognition branch is added to production.

## Final integration gate

After the initial 26 checks, the fixture added generated after as well as before, inherited contents chains, ordinary button children, and replaced/select subtree suppression. Final **32 checks / 0 failures**, prerequisite **32 checks / 14 failures**. The button's principal box is removed while authored children remain, matching Appendix B; it is deliberately not classified with replaced controls.

`layout-final-suite.log` records all four new positive/negative gates. `layout-regressions-final.log` records layout-box51, generated-content54, inline-flex36, intrinsic65, Grid142+84+40+52, max-height18 and test-mk-wired green. The generated-content 48-failure and 2-failure runs are its mandatory controls; its positive run is54/54.

This patch targets the ordinary nested contents boxes evidenced by navigation/layout wrappers. Root/body's manually constructed principal boxes and complete unusual-element computed-value serialization have not been independently validated and are not represented by these checks.

Guest consumer correction: the earlier 32 layout assertions checked
`layout_node_box`, but the real guest `getBoundingClientRect` returned the
children's 160x40 ink union for the absent wrapper. `js_cssom.c` now excludes
contents from both rectangle producers and preserves the zero origin after
scroll. The companion QuickJS gate is a prerequisite of `test-display-contents`:
9 assertions pass; restoring the fallback produces 5 assertion failures,
including the exact 160x40 guest result. This host correction still requires a
rebuilt guest replay of `display-contents.html`.
