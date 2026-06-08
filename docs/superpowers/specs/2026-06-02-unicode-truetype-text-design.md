# Aether OS — Unicode + from-scratch TrueType anti-aliased text (M14)

## Problem

Text today is `font8x16[ch & 0x7F]` (`kernel/fb.c`): **7-bit ASCII only, 1-bit
(no anti-aliasing), a fixed 8×16 cell**, and `net/html.c` reads raw bytes with no
UTF-8 decoding. The most-used written language in the world — Chinese — cannot
be shown at all (a CJK glyph needs far more than 8×16 pixels, and there are
thousands of them), and even Latin text is blocky. To render Chinese web pages
and to look like macOS, we need real Unicode handling and a real, **scalable,
anti-aliased** font.

Two hard requirements from the user:
1. A complete, real Unicode + font stack (not a bitmap hack).
2. Genuine anti-aliasing (it's an imitation of macOS — the type must be smooth).

The framebuffer already alpha-blends (`fb_blend_rect`), so compositing
anti-aliased (grayscale-coverage) glyphs onto the back buffer is supported.

## Goals / non-goals

**Goals**
- Decode UTF-8 → Unicode code points throughout the text path.
- Parse a real `.ttf` from scratch (no libc, no font libraries) and rasterize
  glyph outlines to **grayscale coverage with anti-aliasing** at any pixel size.
- Render CJK (GB2312 coverage) + Latin across the **whole UI** (menu bar, Finder,
  Dock, window titles, all apps, browser), plus a monospace path for the Terminal.
- Proportional layout (per-glyph advances; CJK full-width, Latin proportional).

**Non-goals (v1)**
- No hinting (macOS-style light/smooth rendering; hinting omitted on purpose).
- No subpixel/LCD AA (grayscale coverage only).
- No bidi/shaping/ligatures/kerning (left-to-right, one glyph per code point).
- No Traditional/Japanese/Korean coverage in v1 (GB2312 Simplified subset; the
  engine can load more fonts later).

## Architecture

Single coherent subsystem (a text-rendering engine), built in layers. Modules,
each with one job and a clean interface:

| Module | Responsibility | Depends on |
|--------|----------------|------------|
| `lib/utf8.c` (`include/utf8.h`) | `utf8_next(s, &cp)`: bytes → code point. Pure, no deps. | — |
| `lib/ttf.c` (`include/ttf.h`) | From-scratch TrueType parse: `cmap` (codepoint→glyph id), `glyf` (outline), `loca`, `hmtx` (advance), `head`/`hhea` (units_per_em, ascent/descent). **Parse only — never touches the screen.** Exposes a glyph's outline (lines + quadratic Béziers) and metrics. | — |
| `kernel/text.c` (`include/text.h`) | Rasterizer (outline → AA coverage bitmap), glyph cache, layout, and bridge to `fb`. Multi-font + fallback chain. | `ttf`, `utf8`, `fb`, `kheap` |
| `kernel/fb.c` | New `fb_blit_glyph(x, y, cov, w, h, color)`: treat the coverage bitmap as alpha, blend onto the back buffer. | — |

**Data flow:** `UTF-8 string → utf8_next → codepoint → cmap → glyph id →
(cache hit?) → ttf outline → rasterizer → AA coverage → fb_blit_glyph (alpha
blend)`.

## Anti-aliasing technique

The rasterizer is where the "effort on anti-aliasing" goes.

**Scanline polygon fill with 4× vertical oversampling + fractional horizontal
coverage:**
- Flatten each quadratic Bézier into short line segments (subdivision adapted to
  the pixel size).
- For each pixel row, take **4 sub-scanlines**. For each sub-scanline, compute
  the filled spans using the **nonzero winding rule**; at each span edge the
  partial pixel is weighted by the **fractional x coverage** within that pixel.
- Average the 4 sub-scanlines → a **0–255 grayscale alpha** per pixel. Smooth
  edges, far better than 16-level 4×4 supersampling, with much less code than a
  full signed-area rasterizer (FreeType-style). Upgradeable later.

No hinting: scale outline by `px_size / units_per_em`, rasterize, done.

## Fonts

- **UI / proportional:** Noto Sans CJK SC (思源黑体), SIL OFL — close to PingFang.
  Subset to **GB2312 (~6763 hanzi) + Latin + CJK punctuation** with `pyftsubset`
  → ~3–5 MB `.ttf`.
- **Monospace (Terminal):** DejaVu Sans Mono (free), shipped whole (small);
  CJK in the Terminal **falls back** to Noto (full-width / 2 cells).
- The engine loads multiple fonts and resolves a glyph through a **fallback
  chain** (Terminal = [mono, noto]; everything else = [noto]).
- Both `.ttf` files live on the **AetherFS disk** (packed by `tools/mkfs.py`),
  loaded into memory **after fs mount, before the desktop comes up**. If loading
  fails, text renders as missing-glyph boxes (should not happen in practice).
- QEMU RAM bumped to **`-m 512M`** (the pmm sizes itself from the multiboot map;
  font + glyph cache is ~10 MB, comfortable, and leaves room for full CJK later).

## API and system-wide migration

- New engine API (in `text.h`):
  - `int text_draw(int x, int y, const char *utf8, uint32_t color)` — default UI
    size, proportional, returns the end x (sum of advances). `y` = top.
  - `int text_width(const char *utf8)` — measure.
  - `int text_draw_sz(int x, int y, const char *utf8, int px, uint32_t color)` —
    explicit pixel size (titles, menu bar).
  - `int text_draw_mono(int x, int y, const char *utf8, int cell_w, uint32_t color)`
    — Terminal: snap each glyph to a fixed cell (narrow = 1 cell, wide = 2).
- **`fb_text` / `fb_text_width` stay as thin wrappers** over the new engine at the
  default size, so most call sites keep their signatures and get AA Unicode for
  free.
- The invasive part: call sites that compute layout as `chars × 8` / `n*FONT_W`
  (menu bar, Dock labels, Finder rows, window titles). These switch to
  `text_width()`. Audited and fixed per call site during implementation.
- The whole UI moves to the engine — no half-migration (consistent crisp type).

## Files to remove (obsolete after this work)

These are the 8×16 bitmap path, fully replaced by the TTF engine:
- `include/font8x16.h`
- `tools/genfont.py`

**They cannot be deleted up front** — `kernel/fb.c` `#include`s `font8x16.h`, so
removing it before the engine exists breaks the build. Deletion happens in the
implementation phase, once `text.c` is the sole text path (the build-order step
that drops `font8x16.h` from `fb.c` also removes these files). Early-boot text
uses the serial console, not framebuffer glyphs, so no bitmap fallback is needed
once the disk font loads.

## Build order (each layer independently verifiable)

- **L1** `lib/utf8.c` + `lib/ttf.c`: parse a TTF; extract a glyph outline +
  metrics + `cmap`. *Verify on host:* look up `'中'` and `'A'`, check glyph id +
  advance against `fonttools`/`ttx`.
- **L2** `kernel/text.c` rasterizer: outline → AA coverage bitmap. *Verify on
  host:* rasterize `'A'`/`'中'` to a PGM, eyeball the anti-aliasing.
- **L3** glyph cache + `fb_blit_glyph` + `text_draw`/`text_width`; load the TTF
  from disk at boot. *Verify in QEMU:* draw `"Hello 你好世界"` on the desktop,
  screenshot.
- **L4** UTF-8 in `net/html.c` + Browser. *Verify in QEMU:* load a Chinese web
  page, screenshot (the key milestone).
- **L5** migrate system UI (menu bar, Finder, Dock, titles) to AA; fix width math.
- **L6** Terminal monospace (DejaVu Mono + CJK fallback). Remove `font8x16.h` /
  `genfont.py`.
- Throughout: `tools/mkfont.py` subsetting + `mkfs.py` packing + QEMU `-m 512M`.

## Testing

- **Host:** TTF parse (glyph id / advance vs `fonttools`); rasterizer output
  (PGM, eyeball AA); UTF-8 decode unit checks.
- **QEMU:** screenshots — a Chinese web page in the Browser, the AA system UI,
  the Terminal — eyeballed for correctness and AA quality.
- `make test` (AETHER_BOOT_OK) stays green throughout.

## Risks

- **Rasterizer correctness** (winding, Bézier flattening, coverage at edges) — the
  hardest part; validate per-glyph on host before wiring to the screen.
- **Performance** in QEMU TCG: rasterizing is expensive; the glyph cache makes it
  a one-time cost per (codepoint, size). A page of Chinese is a few hundred unique
  glyphs.
- **Layout ripple:** proportional widths break fixed-cell assumptions; caught by
  auditing `FONT_W`/`*8` call sites.
- **Font size on disk:** GB2312 subset keeps it to a few MB; grow the disk image
  in `mkfs.py` if needed.
