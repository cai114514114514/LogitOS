# Text rendering: fonts, shaping and line breaking

LogitOS shares font parsing, bidirectional text processing, shaping, and glyph rasterization through `c/lib/text/`. The kernel text service adds filesystem loading, font fallback, and cached coverage masks for desktop and application drawing. GUI paragraph layout runs in applications, while browser canvas text loads fonts into application memory and draws their outlines through its own callbacks.

## Components and ownership

The pipeline converts text into positioned glyphs, then converts glyph outlines into coverage:

| Representation | Contents | Interface |
|---|---|---|
| Font face | Borrowed font bytes, metrics, and table offsets | `struct ttf_font` in `c/lib/text/ttf.h` |
| Logical text | Unicode code points and resolved bidi levels | `c/lib/text/bidi.h` |
| Text run | A span with one script, bidi level, and font | `struct text_run` in `c/lib/text/script.h` |
| Shaped glyphs | Glyph IDs, source clusters, advances, and offsets | `struct shape_glyph` in `c/lib/text/shape.h` |
| Glyph outline | Moves, lines, quadratic curves, and cubic curves | `struct fp_path` in `c/lib/text/fontpath.h` |
| Coverage mask | One coverage byte per pixel and bitmap bearings | `c/lib/text/glyphras.h` |

Shaping and bidi resolution receive working storage from their callers. The kernel supplies static arrays in `c/kernel/gui/text.c`; browser canvas supplies arrays in `c/apps/gui/browser/js/graphics/canvas.c`. Large buffers stay off the kernel stack.

The kernel’s `text_lock` serializes access to shaping scratch, rasterizer scratch, and cached glyphs. Drawing holds this lock through the bitmap blit because a cache entry contains a pointer to coverage that eviction can free. Drawing and caret queries use `spin_lock_irqsave()` to keep the thread running while it holds the lock. The locking rationale and call sites are in `c/kernel/gui/text.c`.

## Font loading and fallback

Kernel initialization calls `text_init()`, which attempts to load seven faces. `load_font()` reads each complete file into kernel heap memory and calls `ttf_parse()`. A successful parse retains the allocation because the parsed face points into those bytes. See `c/kernel/init/kmain.c` and `c/kernel/gui/text.c`.

`Makefile` installs these guest files:

| Guest files | Repository inputs | Use |
|---|---|---|
| `/fonts/ui.ttf`, `/fonts/ui-medium.ttf`, `/fonts/ui-semibold.ttf`, `/fonts/ui-bold.ttf` | Corresponding files in `fsroot/fonts/` | Proportional UI weights 400, 500, 600, and 700 |
| `/fonts/mono.ttf`, `/fonts/mono-bold.ttf` | Corresponding files in `fsroot/fonts/` | Monospaced weights 400 and 700 |
| `/fonts/text.ttf` | `third_party/fonts/DejaVuSans.ttf` | Additional coverage, including Arabic and Hebrew |

`tools/mkfont.py` generates the UI and monospaced subsets from vendored Noto variable fonts. It fixes the requested weight before subsetting. UI coverage combines GB2312 characters with Latin and punctuation; the monospaced subset contains printable ASCII and a nonbreaking space.

Older comments in `c/kernel/gui/text.c` say both Noto subsets lost their layout tables. The current generator preserves the `kern` feature in UI GPOS tables and drops GSUB; monospaced subsets drop GSUB, GPOS, and GDEF. This matters because UI Latin text can receive kerning through the shared shaper.

`tl_fonts()` assembles a preference list for each request. A regular proportional request tries UI, DejaVu Sans, then mono. A regular monospaced request tries mono, UI, then DejaVu Sans. `shape_line()` selects the first face with a glyph for each code point, so terminal text can obtain Chinese glyphs from UI.

Numeric UI weights resolve to the nearest baked weight, with ties going upward. Monospaced requests resolve to regular or bold. Missing faces are skipped, and each request holds up to four faces. If every candidate lacks a character, shaping uses glyph zero from the first face. Selection lives in `c/kernel/gui/text.c` and `c/lib/text/shape.c`.

## Font parsing and outlines

`ttf_parse()` reads the OpenType table directory, horizontal metrics, units per em, ascent, descent, line gap, and glyph count. Character mapping uses cmap formats 4 and 12, preferring format 12. `ttf_advance()` returns a glyph’s horizontal advance in font units. These operations live in `c/lib/text/ttf.c`.

The parser selects an outline source:

| Font data | Handling |
|---|---|
| TrueType `glyf` and `loca` | Simple contours and composite components with transforms and coordinate offsets |
| CFF | Type 2 charstrings producing cubic paths |
| CFF2 | Type 2 charstrings through the CFF interpreter |
| CBDT or sbix bitmap face | Character mapping and metrics, with embedded images available through colour-font readers |

TrueType extraction decodes contour flags and coordinate deltas, then resolves implied points between adjacent quadratic control points. The CFF interpreter handles local and global subroutines and selects private dictionaries for CID glyphs. See `c/lib/text/ttf.c` and `c/lib/text/cff.c`.

Both outline backends feed `ttf_glyph_path()`. Its output is a `struct fp_path` in font units, with y increasing upward. The shared representation retains quadratic and cubic commands, preserving the curves supplied by each format. `c/lib/text/fontpath.h` explains this design.

Character mapping has a cache below shaping. `ttf_glyph_id()` uses a 512-entry memo keyed by the font data address, code point, and parse generation. Every `ttf_parse()` attempt advances the generation, including failed attempts. This prevents a reused allocation from inheriting glyph mappings from its previous font contents. The implementation and failure scenario are documented in `c/lib/text/ttf.c`.

## Bidirectional text and shaping

For proportional text, `shape_line()` performs five stages:

1. Decode UTF-8 into code points.
2. Resolve bidi levels using automatic paragraph direction.
3. Split text into runs with one script, level, and font.
4. Derive the runs’ visual order.
5. Shape each run and emit positioned glyphs from left to right.

`c/lib/text/bidi.c` handles embeddings, overrides, isolates, weak and neutral classes, paired brackets, implicit levels, and visual reordering. It keeps original character classes alongside working classes: formatting structure still needs the original properties after directional resolution has rewritten the working classes.

Callers size storage with `bidi_scratch_size()`. `bidi_resolve()` accepts automatic, left-to-right, or right-to-left paragraph direction. `bidi_l1_line()` resets separators and trailing whitespace to the paragraph level; `bidi_reorder()` produces logical indices in visual order. Their contracts are in `c/lib/text/bidi.h`.

`script_runs()` segments the text. `script_arabic_joining()` chooses contextual joining forms, treating transparent marks as characters through which surrounding letters can join. Both live in `c/lib/text/script.c`.

`shape_run()` maps characters to glyphs, applies OpenType glyph substitution tables (GSUB), assigns advances, then applies glyph positioning tables (GPOS). Supported operations include ligatures, contextual substitutions, pair positioning, cursive attachment, and mark attachment. Arabic joining forms receive feature masks on the corresponding glyphs. For right-to-left runs, character mirroring consults glyph availability before substitution. Feature selection and application live in `c/lib/text/shape.c`; table readers live in `c/lib/text/otlayout.c`.

Legacy `kern` adjustments are enabled when the font lacks a usable GPOS table containing lookups. This avoids applying kerning from both sources. Shaped output carries source cluster indices, advances, and placement offsets in font units.

Drawing and advance measurement use `shape_line()`: a glyph callback consumes positions for drawing, while a null callback measures the final pen position. Ligatures and positioning adjustments therefore participate in width calculation.

## Rasterization and the glyph cache

`text_raster()` obtains a path through `ttf_glyph_path()`, scales it into device coordinates, and asks OpenLogit for an eight-bit coverage mask. The bridge in `c/lib/text/glyphras.c` uses nonzero winding, 16 sub-scanlines per pixel row, and a curve flattening tolerance of 1/32 pixel.

Each coordinate is scaled with full integer division. The source explains the reason: rounding a scale matrix first would multiply that error by the outline coordinates. The bitmap bounds include padding around the scaled path.

The rasterizer returns width, height, and horizontal and vertical bearings. The bitmap’s top left is placed at the pen x plus the horizontal bearing, and the baseline minus the vertical bearing. Empty outlines succeed with a zero-sized bitmap. `text_raster_extent()` obtains bounds, and `text_raster_at()` renders into a caller-chosen box.

The kernel cache in `c/kernel/gui/text.c` keys coverage by font slot, glyph ID, pixel size, and horizontal phase. It has 4,096 entries and a default coverage budget of 4 MiB. Lookup probes eight slots; replacing an occupied probe window evicts its initial hash slot. Budget eviction walks the table in round-robin order.

Proportional drawing retains a 24.8 fixed-point pen and rounds horizontal placement to quarter-pixel phases. Each phase has its own cached mask. Foreground colour is applied during blitting, allowing coverage to serve multiple colours.

Cache filling uses a 200 × 200-byte scratch buffer for small masks. Larger masks can be rasterized into heap allocations counted against the coverage budget. Large glyphs that cannot obtain a cached buffer use horizontal bands through the scratch buffer. Cache entries also retain nonzero coverage bounds for ink measurements.

## Colour fonts

`c/lib/text/fontcolor.c` exposes colour layers, palettes, and embedded bitmap images:

| Data | Interface |
|---|---|
| COLR layer records | `colr_layers()` returns glyph IDs and palette indices |
| CPAL palettes | `cpal_color()` returns a colour as ARGB |
| CBLC/CBDT strikes | `cbdt_lookup()` locates image bytes and strike metrics |
| sbix strikes | `sbix_lookup()` locates image bytes and origin offsets |
| Combined bitmap lookup | `font_bitmap_lookup()` tries CBDT, then sbix |

Bitmap lookup prefers the smallest strike at or above the requested pixels per em, falling back to the largest available strike. Returned image data borrows the font allocation. For sbix, the origin offset describes the image’s bottom left; the caller uses the decoded height to derive its top bearing.

COLR layers share a coordinate system. A caller can union their bounds from `text_raster_extent()`, then rasterize each layer into that common box with `text_raster_at()`. `tests/unit/font_color_test.c` follows this procedure for layer coverage and checks layer records, palette values, bitmap metrics, and image-byte checksums against reference data.

## Line breaking and paragraphs

`c/lib/text/linebreak_core.inc` contains the shared Unicode line-break property lookup and rule engine. Its generated tables identify Unicode 16.0.0. Both the public wrapper in `c/lib/text/linebreak.c` and browser layout in `c/apps/gui/browser/layout/text.c` include this core.

`tl_break_utf8()` returns prohibited, allowed, and mandatory break opportunities indexed by UTF-8 byte offset. Callers can use these offsets to slice the original string. `c/lib/text/linebreak.h` exposes normal and loose behaviour, plus break-all and keep-all options.

`tl_para_layout()` combines break opportunities with measured caret stops. It greedily fills each line, excludes trailing spaces and line endings from measured width, and cuts an overlong word at a supplied cluster boundary. Options select width, alignment, maximum line count, and head, middle, or tail ellipsis. Line height is carried for the caller’s vertical placement.

Measurement goes through `struct tl_measure_cb`. The layout requests caret positions once for the text and once more for U+2026 when an ellipsis is needed. Wrapping then operates on local arrays. Output records source byte ranges, alignment offsets, and the range replaced by the ellipsis. The contracts and implementation are in `c/lib/text/para.h` and `c/lib/text/para.c`.

`Makefile` excludes the public line-break wrapper and paragraph implementation from kernel compilation and packages them into an application archive. `c/apps/gui/aui_text.inc` connects paragraph measurement to kernel caret queries and assembles each output line into a text run for drawing.

## Desktop and application integration

The window manager draws titles and labels through `fb_text()` and measures them through `fb_text_width()`. These wrappers use the display’s scaled UI font size. `fb_text()` splits newline-delimited text and advances vertically by `text_line_height()`. See `c/kernel/gui/fb/fb.c` and title callers in `c/kernel/gui/wm_chrome.c`.

Applications call `text_measure_px()`, `gui_text_run()`, and `gui_text_run_w()` through `c/apps/logit.h`. The GUI dispatcher copies application text into kernel memory, selects the application surface, scales coordinates and font size, and invokes the text service. The legacy drawing path is in `c/kernel/gui/wm.c`.

`SYS_GUI_TEXT_RUN2` carries global alpha and numeric weight. Its handler in `c/kernel/gui/wm_draw2_text.c` converts 24.8 logical positions and sizes into whole device pixels and resolves the requested weight to a baked face. `SYS_TEXT_MEASURE` supplies advance, metrics, and caret queries through `c/kernel/gui/text_measure_dispatch.inc`. Metrics include advance, baseline, signed font metrics, and ink bounds; caret positions use 24.8 device coordinates.

The terminal calls `gui_text_mono()` from `c/apps/gui/terminal/terminal.c`. A positive cell width in `shape_line()` selects grid placement. Each code point occupies one cell, or two when its scaled glyph advance exceeds one and a half cells. Font fallback supplies the selected glyph.

Browser document layout obtains native measurements through `c/apps/gui/browser/app/runtime.c`, and `c/apps/gui/browser/paint/text.inc` submits text runs for painting. Canvas text loads font bytes into application memory, calls `shape_line()` for measurement and placement, and converts glyph outlines into paths for filling or stroking. That integration lives in `c/apps/gui/browser/js/graphics/canvas.c`.

## Limitations

- Font collections require extracting a face before `ttf_parse()`. TrueType hint instructions and CFF hints are skipped; composite placement by matching points is unsupported. CFF2 blending retains default values. See `c/lib/text/ttf.c` and `c/lib/text/cff.c`.
- The shaper lacks Unicode normalization, Indic/Khmer/Myanmar syllable reordering, Arabic presentation-form fallback, and vertical shaping. Terminal grid placement bypasses shaping and bidi resolution. See `c/lib/text/shape.h` and `c/lib/text/shape.c`.
- Proportional drawing retains fractional advances, while width and caret queries accumulate whole-pixel advances. Legacy measurement accepts regular/bold face bits, while numeric drawing weights also select medium and semibold faces. These differences can affect placement and fitting. See `c/kernel/gui/text.c`, `c/kernel/gui/text_measure_dispatch.inc`, and `c/apps/gui/aui_text.inc`.
- Colour-font readers are used by font tests and the fuzzer; desktop text drawing uses monochrome outline coverage. COLR version 1 paint graphs and SVG glyph rendering are absent. Embedded bitmap data requires decoding by the caller. See `c/lib/text/fontcolor.c` and `c/kernel/gui/text.c`.
- Caret clustering omits spacing combining marks and Hangul jamo sequences. Paragraph layout rejects decreasing caret x positions, restricting bidi use; browser paragraph fragments also await paragraph bidi resolution and reordering per line. Strict line breaking currently matches normal behaviour. See `c/kernel/gui/text.c`, `c/lib/text/para.c`, `c/lib/text/linebreak.h`, and `c/apps/gui/browser/layout/text.c`.
- Working storage has fixed limits. Kernel shaping holds 1,024 code points, 2,048 glyphs, and 64 runs; `shape_line()` truncates input and run counts to scratch capacities and skips runs whose shaping fails. GUI drawing payloads are limited to 1,023 bytes. The line-break wrapper accepts up to 2,048 code points, paragraph layout accepts up to 8,192 bytes, and both require serialized calls because their scratch is static. See `c/kernel/gui/text.c`, `c/kernel/gui/wm.c`, `c/lib/text/shape.c`, `c/lib/text/linebreak.h`, and `c/lib/text/para.h`.

## Source map

| File | What it contains |
|---|---|
| `c/lib/text/ttf.h`, `c/lib/text/ttf.c` | Font representation, parsing, character mapping, metrics, and TrueType outlines |
| `c/lib/text/cff.h`, `c/lib/text/cff.c` | CFF/CFF2 parsing and Type 2 charstring interpretation |
| `c/lib/text/fontpath.h` | Shared outline commands and bounds |
| `c/lib/text/glyphras.h`, `c/lib/text/glyphras.c` | Outline conversion, coverage masks, bearings, and raster limits |
| `c/lib/text/fontcolor.h`, `c/lib/text/fontcolor.c` | Colour layers, palettes, and embedded bitmap lookup |
| `c/lib/text/bidi.h`, `c/lib/text/bidi.c` | Bidi levels, whitespace reset, mirroring, and visual order |
| `c/lib/text/script.h`, `c/lib/text/script.c` | Script segmentation and contextual joining |
| `c/lib/text/otlayout.c` | OpenType substitution, positioning, glyph definition, and kerning table readers |
| `c/lib/text/shape.h`, `c/lib/text/shape.c` | Feature plans, shaping, font fallback, and line placement |
| `c/lib/text/linebreak_core.inc`, `c/lib/text/linebreak_data.inc` | Shared line-break rules and generated Unicode properties |
| `c/lib/text/linebreak.h`, `c/lib/text/linebreak.c` | Public UTF-8 break-opportunity interface |
| `c/lib/text/para.h`, `c/lib/text/para.c` | Greedy wrapping, alignment, and ellipsis |
| `c/kernel/init/kmain.c` | Text-service initialization |
| `c/kernel/gui/text.c` | Font loading, selection, glyph cache, drawing, metrics, and carets |
| `c/kernel/gui/fb/fb.c`, `c/kernel/gui/wm_chrome.c` | Desktop text wrappers and window-title callers |
| `c/kernel/gui/wm.c`, `c/kernel/gui/wm_draw2_text.c` | Application text dispatch and weighted drawing with alpha |
| `c/kernel/gui/text_measure_dispatch.inc`, `include/abi/logit_abi.h` | Measurement dispatch, query structures, and coordinate contracts |
| `c/apps/logit.h`, `c/apps/gui/aui_text.inc` | Application text calls and paragraph integration |
| `c/apps/gui/terminal/terminal.c` | Terminal text-grid drawing |
| `c/apps/gui/browser/layout/text.c` | Browser line breaking and fragment placement |
| `c/apps/gui/browser/app/runtime.c`, `c/apps/gui/browser/paint/text.inc` | Browser native measurement and document text painting |
| `c/apps/gui/browser/js/graphics/canvas.c` | Application font storage and canvas text paths |
| `tools/mkfont.py`, `Makefile` | Font generation, installation, and text-library build selection |
| `tests/unit/font_color_test.c` | Colour-layer coverage, palette, and embedded bitmap checks |

## See also

- [Text and input](../architecture/text-and-input.md)
- [Window manager](../kernel/window-manager.md)
- [Layout: block, flex, grid and text](../browser/layout.md)
- [Libraries](overview.md)