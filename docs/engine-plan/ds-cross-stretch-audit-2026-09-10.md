# DS central blank area: nested row stretch and overflow audit

Measured 2026-09-10. This is a diagnosis with local controls; **no layout product algorithm was changed by this audit**. The passive-document context work is separate.

Correction/update later on 2026-09-10: the user then authorized implementation. The generic used-height and sidebar fix, old-behavior controls and independent guest acceptance are recorded in [ds-flex-used-height-fix-2026-09-10.md](ds-flex-used-height-fix-2026-09-10.md). The audit observations below remain the pre-fix evidence.

The central blank area has a reproduced browser mechanism: the final stretched height is applied to a row flex item's outer box after its descendants have been laid out. Nested content therefore builds its absolute-position containing block and overflow clip from an indefinite/zero height. Its text remains in the display list and is then correctly culled by the incorrect zero-height clip.

## Live evidence and generic host reproduction

The parent task's existing live log, `build-ds-render-audit/live/ds-serial.log`, records a 865×562 row (`._7780f2e`), a 865×14 column (`._765a5cd`) and a 865×0 absolute child (`._660ca72`). Visible-range welcome/input items exist below those boxes. This subtask did not navigate or operate that logged-in VM.

The audit removes all site names, classes, scripts and user data. Its HTML contains an outer fixed-height flex row, a growing inner row, a `flex:1` column with `position:relative;overflow:hidden`, and a child with `position:absolute;inset:0`. The child holds the synthetic labels `VISIBLE` and `INPUT`.

The host harness links the shipping DOM parser, CSS engine/extra/vars, layout and browser painter; its GUI recorder counts actual text draw calls. It does not substitute a copied layout or paint algorithm.

| Scenario | Outer/inner row | Column | Absolute child | Retained VISIBLE item | Item clip height | Text paint calls |
|---|---:|---:|---:|---:|---:|---:|
| Original nested stretch + hidden overflow | 865×562 | 865×14 | 865×0 | 1 | 0 | 0 |
| Only change overflow to visible | 865×562 | 865×14 | 865×0 | 1 | none | 1 |
| Only add explicit column height 562px | 865×562 | 865×562 | 865×562 | 1 | 562 | 1 |

The two controls distinguish clipping from missing DOM/text/font output. The explicit-height control also restores the welcome block's requested height: 60px instead of the 20px natural result under the zero-height flex container. These measurements strongly connect the reproduced mechanism to the live geometry; they do not claim every blank/overlap problem on DS has one cause.

Reproduce the host result:

```sh
make BUILD=build-iframe-layout-fix -f build-ds-render-audit/cross_stretch_probe.mk ds-cross-stretch-audit
```

Sources and output: `build-ds-render-audit/cross_stretch_probe.c`, `.mk`, `.log`. The harness exits 0 only when the original has one retained text item and zero text draws, while both controls draw the retained text.

## Independent real guest

`build-ds-render-audit/cross_stretch_guest.py` boots its own QEMU with a unique QMP socket and the frozen `build-terms-layout-evidence/snapshot-final/{logit.iso,disk.img}`. It uses `-snapshot`, serves only the generic fixture over a loopback HTTP server, and terminates that new QEMU after capture. It never connects to the logged-in VM. The fixture has no geometry-reading JavaScript; its only script emits a ready marker, avoiding a CSSOM-triggered reflow that could alter initial rendering.

Three 320×400 panels are shown side by side at 1280×800 so the complete comparison fits the guest browser window. Visual inspection of `page.png` confirms: left is an empty white panel; middle shows `VISIBLE` and `INPUT` after only overflow is made visible; right shows both with the intended taller welcome area after only the column height is supplied.

![Independent guest: blank original, two visible controls](../../build-ds-render-audit/cross-stretch-guest/page.png)

Evidence: `build-ds-render-audit/cross-stretch-guest/{fixture.html,serial.log,results.json,page.png}`. The ready marker was observed, screenshot inspected, QEMU shut down, and both source-image SHA-256 hashes were verified unchanged afterward:

- ISO: `89466926e7e34e117f538e08fd5772cc18eab02eda4ed78b1d93c96f4e00c58b`
- Disk: `e65fb6d2a45654b297494a5d929710a5c54c49e02da32e03cb73e2c0350c856d`

## Code path and boundary

In `c/apps/browser/layout.c`, `flex_place_impl()` applies a temporary definite used height before laying out descendants only when `exact_h` is true (around line 4317). The row bridge instead calls `flex_place()` with `exact_h=0` (around lines 4382 and 4625). That path sets the positioned containing-block height from the style, calls `layout_block()` (around line 4356), and only afterward raises the returned outer height to `forced_h` (around line 4365). Thus the stretched row's final box can be 562px while nested layout still received no definite cross-size. The painter later sees a real zero-height overflow clip and culls the retained text; disabling that cull globally would hide this geometry defect and break intentional clipping.

The separate flat z-index/stacking-context audit explains a sidebar overpainting mechanism and is not merged into this conclusion. Neither audit changes site CSS, injects a site workaround, or establishes that the authenticated application is completely rendered correctly.
