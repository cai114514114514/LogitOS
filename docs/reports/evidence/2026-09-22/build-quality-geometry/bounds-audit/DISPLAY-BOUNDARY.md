# Display target limit review

Recommendation: keep the packed-pixel display target limit separate from
`GFX_MAX_W`, with an explicit per-axis display limit of 8192. No reviewed
`ol_display_*` call requires the entire destination to fit the rasterizer's
4096-pixel coverage row. This review does not recommend accepting arbitrary
positive dimensions merely because their product fits `int`.

## Fixed storage and call boundaries

- `gfx_raster.c:83–87` owns fixed coverage, clip, and accumulator rows of
  `GFX_MAX_W`. Its `raster` function refuses a fill wider than that limit at
  line 356. This limit should remain 4096.
- The display adapter's only path rasterization dependency is
  `gfx_mask_corner` (`openlogit_display.c:308,541`). Its cache accepts tiles
  no larger than `GFX_MASK_MAX` (72); `gfx_mask.c:149` refuses larger tiles
  before writing the cache. It never sends the screen width to the path
  rasterizer. The shadow adapter already bounds its corner tile to this cache.
- Glyph blending (`openlogit_display.c:403–422`) reads the caller's coverage
  bitmap using its glyph width and writes target rows using target width.
  It has no fixed screen-wide row. Its offsets are widened to `long`.
- Ordinary blur owns allocator-backed `blur_scratch`, sized to its cropped
  visible rectangle. Glass owns allocator-backed `glass_buf` and `glass_line`.
  Glass's only fixed pixel storage is the seven-pixel ring in its vertical
  blur, independent of panel or display dimensions.
- Cursor blending and animated layer alpha use `long` pixel offsets. The
  actual WM wrappers at `wm.c:4462–4469` and `4560–4568` check bind failure;
  they benefit from accepting 5K targets without changing their source arrays.

## Arithmetic with an 8192-pixel per-axis target

| Expression | Bound with normal source and destination dimensions ≤ 8192 |
| --- | --- |
| Target pixel area / signed pixel index | 67,108,864 pixels, below INT_MAX |
| Effect buffer byte size | 268,435,456 bytes; allocation uses unsigned long |
| Nearest scaling `i * src->w`, `j * src->h` | at most 67,108,864 |
| Bilinear `src_dimension << 16` | at most 536,870,912, fits uint32_t |
| RGBA source byte offset | below 268,435,456 |
| Gradient channel delta × row index | at most 255 × 8192 |
| Clamped corner fallback squared distances | at most 2 × 4096² |

Geometry retains signed 8.8 distances, while its square/root input is unsigned
long. At this size the signed distance and normal calculations also fit their
existing types. `glass_line` needs at most 8192 integers (32 KiB); the blur
radius remains six, so running color sums and reciprocal products do not grow
with the screen area.

The GUI packed rectangle positions/sizes are at most 65535 points, and the
scale is capped at 400%. Those conversions and the subsequent intersections
with an 8192 target do not approach signed-int overflow. GUI rounded rectangle
and glass radii are eight bits before scaling. The actual `SYS_GUI_BLIT`
handler separately restricts RGBA source dimensions to 4096 at `wm.c:2999`.
General direct callers still own the validity of their independently supplied
source buffers and geometry; changing the target limit should not silently
claim arbitrary source input validation.

## Why area-only acceptance would be a larger change

The saved `thin-scale.c` probe emulates the proposed area-only target policy
without modifying production. Both source and destination are valid, tiny
allocations in pixel-count terms:

- 50000 × 1 nearest scaling overflows `42950 * 50000` at
  `openlogit_display.c:112`; UBSan stops the probe.
- 65536 × 1 bilinear scaling silently wraps the source-width shift at line
  182. A 1:1 copy produces 65,535 wrong pixels; its last pixel is 0 instead of
  65,535.

See `thin-scale-{nearest,bilinear}.log` and `thin-scale-command.txt`. These are
not blockers for the proposed 8192 cap and are not part of the current fix.
They demonstrate why a per-axis contract should be explicit instead of relying
on pixel area alone. No 5K guest, hardware, or arbitrary-source robustness claim
follows from this source review.

## Allocation failure ownership review

The root agent's OOM change stops before blur or rendering. `display_alloc`
sets `OL_LIMIT`; subsequent successful allocations do not erase it. A new
successful bind starts a batch and clears that error. Each replaced scratch
allocation is freed first, then its pointer and capacity are updated together;
the context retains any successful allocation for reuse or destruction.

`ownership.c` adds a read-only scratch test with explicit allocation tracking:
12 sequences cover cold first/second allocation failure, both buffers growing,
only pixel storage growing, only column storage growing, then either immediate
destruction or allocation recovery. All sequences finish with zero failures
and zero live allocations, with no unknown/double releases or sanitizer
diagnostics (`ownership.log`). The 1067 formal checks validate no writes on
failure and the next batch's complete recovered raster. They do not themselves
count leaks because their ASan invocation disables leak detection; this tracked
probe closes that specific evidence gap without changing formal files.
