# Unicode + from-scratch TrueType anti-aliased text (M14) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render Unicode text — including Chinese — across the whole UI with a from-scratch, scalable TrueType rasterizer that produces genuine grayscale anti-aliasing, replacing the 8×16 ASCII bitmap font.

**Architecture:** `lib/utf8.c` decodes UTF-8 → code points. `lib/ttf.c` parses a real `.ttf` (cmap/glyf/loca/hmtx/head/hhea) into glyph outlines + metrics, screen-agnostic. `kernel/text.c` rasterizes outlines to grayscale coverage (scanline fill, 4× vertical oversampling + fractional horizontal coverage), caches glyphs by (font,codepoint,size), lays out runs, and blits via a new `fb_blit_glyph`. Two fonts on the AquaFS disk (Noto Sans CJK SC subset for UI, DejaVu Sans Mono for the Terminal) load at boot; a fallback chain resolves missing glyphs.

**Tech Stack:** C (freestanding, `clang --target=x86_64-elf`), nasm, QEMU; host-side `clang` + Python (`fonttools`/`pyftsubset`) for tooling and tests.

---

## File structure

| File | Responsibility | New/Modify |
|------|----------------|------------|
| `include/utf8.h`, `lib/utf8.c` | `utf8_next(s,&cp)` decoder | new |
| `include/ttf.h`, `lib/ttf.c` | TrueType parse: load font, `cmap` lookup, glyph outline + metrics | new |
| `include/text.h`, `kernel/text.c` | rasterizer + glyph cache + layout + font registry/fallback | new |
| `kernel/fb.c`, `include/fb.h` | add `fb_blit_glyph`; `fb_text`/`fb_text_width` become wrappers over `text.c` | modify |
| `tools/mkfont.py` | subset Noto Sans CJK SC → `fsroot/fonts/ui.ttf`; copy DejaVu Mono → `fsroot/fonts/mono.ttf` | new |
| `tools/mkfs.py`, `Makefile` | pack `fonts/*.ttf` into the disk; `-m 512M`; `$(DISK)` deps | modify |
| `kernel/kmain.c` | `text_init()` (load fonts from disk) after fs mount, before `wm_init` | modify |
| `net/html.c` | decode UTF-8 (keep codepoints; pass through multibyte) + numeric entities → UTF-8 | modify |
| `user/terminal.c`, `kernel/wm.c` etc. | width math: `chars*8` → `text_width`; Terminal uses `text_draw_mono` | modify (L5/L6) |
| `include/font8x16.h`, `tools/genfont.py` | **delete** at L6 | remove |

### Interfaces (locked here; later tasks must match exactly)

```c
/* include/utf8.h */
const char *utf8_next(const char *s, uint32_t *cp);   /* returns ptr past the char; *cp=0xFFFD on bad byte */

/* include/ttf.h */
struct ttf_font {
    const uint8_t *data; int len;
    int units_per_em, ascent, descent, line_gap;
    uint32_t off_cmap, off_glyf, off_loca, off_hmtx, off_hhea, off_maxp, off_head;
    int loca_long;          /* 1 = 32-bit loca, 0 = 16-bit */
    int num_glyphs, num_hmetrics;
};
int  ttf_parse(const uint8_t *data, int len, struct ttf_font *f);   /* 0 ok */
int  ttf_glyph_id(const struct ttf_font *f, uint32_t codepoint);    /* 0 if absent (.notdef) */
int  ttf_advance(const struct ttf_font *f, int gid);               /* advance width in font units */
/* Emit the glyph outline as flattened contours in font units (y up). Calls back
 * move/line; quadratics are flattened by the caller-provided step. Returns 0 ok. */
struct ttf_outline { /* filled by ttf_glyph_outline */
    /* points in font units; contours delimited by `on`/end markers — see ttf.c */
    short *x, *y; uint8_t *on; int *contour_end; int npts, ncontours;
};
int  ttf_glyph_outline(const struct ttf_font *f, int gid, struct ttf_outline *out, void *scratch, int scratchlen);

/* include/text.h */
void text_init(void);                                  /* load /fonts/ui.ttf + /fonts/mono.ttf from disk */
int  text_draw(int x, int y, const char *utf8, uint32_t color);              /* default UI px; returns end x */
int  text_draw_sz(int x, int y, const char *utf8, int px, uint32_t color);
int  text_draw_mono(int x, int y, const char *utf8, int cell_w, uint32_t color);
int  text_width(const char *utf8);                     /* default UI px */
int  text_width_sz(const char *utf8, int px);
int  text_line_height(int px);                         /* ascent+descent+gap scaled */

/* include/fb.h (added) */
void fb_blit_glyph(int x, int y, const uint8_t *cov, int w, int h, uint32_t color); /* cov = 8-bit alpha */
```

Default UI pixel size: `#define TEXT_UI_PX 16` (in `text.h`).

---

## Task 1: UTF-8 decoder (L1)

**Files:** Create `include/utf8.h`, `lib/utf8.c`, `tools/t/utf8_test.c`.

- [ ] **Step 1: Write `include/utf8.h`** with the `utf8_next` prototype above and an include guard.

- [ ] **Step 2: Write the failing host test** `tools/t/utf8_test.c`:

```c
#include <stdio.h>
#include <stdint.h>
#include "utf8.h"
static int fail;
static void chk(const char*s,uint32_t want,int wlen){
  uint32_t cp; const char*p=utf8_next(s,&cp);
  if(cp!=want||(int)(p-s)!=wlen){printf("FAIL %s: cp=%x len=%ld want %x/%d\n",s,cp,(long)(p-s),want,wlen);fail=1;}
}
int main(void){
  chk("A",0x41,1); chk("\xc3\xa9",0xE9,2); chk("\xe4\xbd\xa0",0x4F60,3); /* 你 */
  chk("\xf0\x9f\x98\x80",0x1F600,4);                                    /* 😀 */
  chk("\xff",0xFFFD,1);                                                 /* bad byte */
  printf(fail?"SOME FAILED\n":"ALL PASS\n"); return fail;
}
```

- [ ] **Step 3: Run, verify it fails** (link error / no `utf8.c`):
`clang -Iinclude tools/t/utf8_test.c lib/utf8.c -o /tmp/utf8t && /tmp/utf8t`
Expected: compile error (utf8_next undefined) → after stub, FAIL lines.

- [ ] **Step 4: Implement `lib/utf8.c`** — standard UTF-8 decode: 1/2/3/4-byte forms by leading-byte pattern, validate continuation bytes (`0x80..0xBF`), emit `0xFFFD` and advance 1 on any malformed lead/continuation. No libc.

- [ ] **Step 5: Run, verify PASS:** `clang -Iinclude tools/t/utf8_test.c lib/utf8.c -o /tmp/utf8t && /tmp/utf8t` → `ALL PASS`.

- [ ] **Step 6: Commit** `git add include/utf8.h lib/utf8.c tools/t/utf8_test.c && git commit -m "utf8: UTF-8 -> codepoint decoder (M14 L1)"`

---

## Task 2: TrueType parser — header, cmap, advance (L1)

**Files:** Create `include/ttf.h`, `lib/ttf.c`, `tools/t/ttf_test.c`. Need a font on the host: `tools/mkfont.py` (Task 8) produces `/tmp/ui.ttf`; for this task generate it first or use any system `.ttf`.

- [ ] **Step 1:** Write `include/ttf.h` with the structs/prototypes from the interface block.

- [ ] **Step 2:** Generate a test font for the host:
`python3 tools/mkfont.py /tmp/ui.ttf /tmp/mono.ttf` (Task 8 must exist; if running L1 first, temporarily point at a system font). Also dump references:
`python3 -c "from fontTools.ttLib import TTFont; f=TTFont('/tmp/ui.ttf'); print(f.getBestCmap().get(0x4F60)); print(f['hmtx']['cid00000'] if 0 else '')"` — use `fonttools` to get the glyph name + advance for `你`(0x4F60) and `A`(0x41) as the expected values.

- [ ] **Step 3: Write failing host test** `tools/t/ttf_test.c`: read `/tmp/ui.ttf` into a buffer, `ttf_parse`, then assert `ttf_glyph_id(f,0x41)!=0`, `ttf_glyph_id(f,0x4F60)!=0`, and `ttf_advance` for each equals the fonttools reference (pass references as argv or hardcode after Step 2). Print PASS/FAIL.

- [ ] **Step 4: Run, verify fails.** `clang -Iinclude tools/t/ttf_test.c lib/ttf.c -o /tmp/ttft && /tmp/ttft /tmp/ui.ttf`

- [ ] **Step 5: Implement `lib/ttf.c` parse + cmap + advance:**
  - Big-endian readers `rd16/rd32`. Parse the table directory (offset 4 = numTables; records: tag[4], checksum, offset, length).
  - `head`: units_per_em (off 18), indexToLocFormat (off 50) → `loca_long`.
  - `maxp`: numGlyphs (off 4). `hhea`: ascent(4)/descent(6)/lineGap(8), numberOfHMetrics(34).
  - `cmap`: find a Unicode subtable — prefer format 4 (platform 3 enc 1) and format 12 (platform 3 enc 10) for BMP+astral. Implement format 4 (segmented) and format 12 (groups) lookup → glyph id.
  - `hmtx`: advance = entry `min(gid, numHMetrics-1)` (×4 bytes, advance is the u16 at that offset).

- [ ] **Step 6: Run, verify PASS.** Expected: glyph ids non-zero, advances match fonttools.

- [ ] **Step 7: Commit** `git add include/ttf.h lib/ttf.c tools/t/ttf_test.c && git commit -m "ttf: parse header/cmap/hmtx, codepoint->gid + advance (M14 L1)"`

---

## Task 3: TrueType glyph outlines (L1)

**Files:** Modify `lib/ttf.c`, `tools/t/ttf_test.c`.

- [ ] **Step 1:** Extend `ttf_test.c`: for `'A'` and `'你'`, call `ttf_glyph_outline`, assert `ncontours>=1`, `npts>0`, and that all points fall within `[xmin..xmax]×[ymin..ymax]` from the glyph header. Dump npts/ncontours.

- [ ] **Step 2: Run, verify fails** (ttf_glyph_outline undefined).

- [ ] **Step 3: Implement `ttf_glyph_outline`:**
  - `loca` → glyph offset/length (16-bit entries are ×2). Empty length = blank glyph (ncontours 0).
  - `glyf` record: numberOfContours (s16). If <0 → **composite**: parse components (flags, glyphIndex, args, optional 2×2 transform), recurse, apply transform + offset, concatenate contours. If ≥0 → simple glyph: endPtsOfContours[n], instructionLength (skip), flags (with repeat), x-coords (delta, SHORT/SAME bits), y-coords likewise. Reconstruct absolute points; `on`-curve bit from flags.
  - Write points into caller `scratch` (lay out `x[],y[],on[],contour_end[]` from it; bounds-check against `scratchlen`, return -1 if too small).

- [ ] **Step 4: Run, verify PASS.**

- [ ] **Step 5: Commit** `git commit -am "ttf: simple + composite glyph outline extraction (M14 L1)"`

---

## Task 4: Anti-aliased rasterizer (L2)

**Files:** Create `include/text.h`, `kernel/text.c` (rasterizer portion only), `tools/t/raster_test.c`.

- [ ] **Step 1:** Add to `text.h` the prototype for the internal rasterizer (exposed for the host test):
```c
/* Rasterize a glyph outline (font units, y up) into an 8-bit coverage bitmap at
 * px pixel size. *w,*h,*ox,*oy describe the bitmap and its top-left offset from
 * the pen origin (oy is distance above baseline). cov is caller-owned >= w*h. */
int text_raster(const struct ttf_font *f, int gid, int px,
                uint8_t *cov, int covcap, int *w, int *h, int *ox, int *oy);
```

- [ ] **Step 2: Write failing host test** `tools/t/raster_test.c`: parse `/tmp/ui.ttf`, raster `'A'` and `'你'` at px=48, assert `*w>0 && *h>0`, count non-zero AND intermediate (1..254) coverage pixels — assert there ARE intermediate values (proves anti-aliasing, not 1-bit). Write each to a PGM (`P5`) at `/tmp/A.pgm`, `/tmp/ni.pgm` for eyeballing.

- [ ] **Step 3: Run, verify fails.**

- [ ] **Step 4: Implement `text_raster` in `kernel/text.c`:**
  - Scale `s = px / units_per_em`. Compute glyph bbox in pixels → `w,h,ox,oy`.
  - Flatten quadratics: for each off-curve point between two on-curve points, subdivide into `k` segments (`k` ≈ `clamp(px/8, 2, 16)`); implied on-curve midpoints per TrueType. Produce edge list (x0,y0,x1,y1) in pixel space (y down, baseline-relative via `oy`).
  - For each pixel row `r` (0..h-1): for each of 4 sub-scanlines at `y=r+(k+0.5)/4`: collect edge crossings, sort, apply **nonzero winding** to get filled spans; for each span `[xa,xb)` add coverage to pixels — full 1.0 for interior pixels, **fractional** for the partial pixels at `xa`/`xb`. Accumulate into a float/`uint16` row, then `cov[r*w+c] = clamp(acc*255/4)`.
  - Return -1 if `w*h > covcap`.

- [ ] **Step 5: Run, verify PASS** and inspect: `python3 -c "..."` or open `/tmp/A.pgm`. Confirm smooth edges + intermediate gray values present.

- [ ] **Step 6: Commit** `git add include/text.h kernel/text.c tools/t/raster_test.c && git commit -m "text: anti-aliased scanline rasterizer (M14 L2)"`

---

## Task 5: Glyph cache + font registry + layout + fb integration (L3)

**Files:** Modify `kernel/text.c`, `kernel/fb.c`, `include/fb.h`, `kernel/kmain.c`. Disk font load needs Tasks 8–9.

- [ ] **Step 1: `fb_blit_glyph` in `fb.c`** (+ declaration in `fb.h`):
```c
void fb_blit_glyph(int x, int y, const uint8_t *cov, int w, int h, uint32_t color){
    for(int r=0;r<h;r++) for(int c=0;c<w;c++){
        uint8_t a=cov[r*w+c]; if(!a) continue;
        fb_blend_px(x+c, y+r, color, a);   /* reuse the per-pixel blend used by fb_blend_rect */
    }
}
```
(If no per-pixel blend exists, factor one out of `fb_blend_rect`.)

- [ ] **Step 2: Font registry + `text_init`** in `text.c`: a small array of `struct ttf_font` (slots: UI, MONO). `text_init()` reads `/fonts/ui.ttf` and `/fonts/mono.ttf` via `vfs_read` into kmalloc'd buffers and `ttf_parse`s them. Fallback chain helper `resolve(cp, primary)` → returns (font*, gid), trying primary then UI.

- [ ] **Step 3: Glyph cache** in `text.c`: hash table keyed by `(font_idx, gid, px)` → cached `{cov ptr, w,h,ox,oy,advance_px}`. On miss, `text_raster` + `kmalloc` the coverage, store. Fixed capacity (e.g. 1024 entries); evict oldest on full (simple ring or LRU).

- [ ] **Step 4: Layout + public API** in `text.c`: implement `text_draw_sz`/`text_width_sz` — iterate `utf8_next`, resolve glyph, fetch from cache, `fb_blit_glyph` at `(x+ox, y+ascent_px-oy)`, advance `x += advance_px`. `text_draw`/`text_width` call the `_sz` form with `TEXT_UI_PX`. `text_line_height`. `text_draw_mono`: snap advance to `cell_w` (×2 if the glyph's natural advance > 1.5×cell, i.e. wide CJK).

- [ ] **Step 5: Call `text_init()`** in `kmain.c` after `vfs_mount()` succeeds and before `wm_init()`.

- [ ] **Step 6: Temp in-kernel smoke test:** in `kmain.c` after `text_init`, `text_draw(40, 80, "Hello \xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c", rgb(0,0,0));` then `fb_present()`. Build, run headless with QMP screenshot; verify "Hello 你好世界" renders anti-aliased. Remove the temp draw after verifying.

- [ ] **Step 7: Commit** `git commit -am "text: glyph cache + font registry + layout + fb_blit_glyph; draws 你好世界 (M14 L3)"`

---

## Task 6: Route `fb_text` through the engine + UTF-8 in HTML/Browser (L4)

**Files:** Modify `kernel/fb.c` (wrappers), `net/html.c`.

- [ ] **Step 1:** Replace `fb_text`/`fb_text_width` bodies with `return text_draw(x,y,s,color);` / `return text_width(s);`. Keep `fb_char` only if still referenced; otherwise drop it. Remove `#include "font8x16.h"` from `fb.c` **only after** nothing else uses it (defer actual file delete to L6).

- [ ] **Step 2:** `net/html.c`: stop masking to ASCII. Where it copies body bytes into the text buffer, pass multibyte UTF-8 through unchanged (it already strips tags; just don't `&0x7F` or drop high bytes). Numeric entities `&#NNNN;`/`&#xHHHH;` → encode the codepoint as UTF-8 (1–4 bytes) into the output. Named entities unchanged.

- [ ] **Step 3: Verify in QEMU:** rebuild; via QMP open a Chinese page in the Browser (e.g. `https://zh.wikipedia.org/` or a small known UTF-8 page) and screenshot. Expected: Chinese text renders, anti-aliased. (`make test` still green.)

- [ ] **Step 4: Commit** `git commit -am "fb/html: route text through the TTF engine; UTF-8 in HTML render; Chinese pages render (M14 L4)"`

---

## Task 7: Migrate system UI width math (L5)

**Files:** audit + modify call sites that assume an 8px cell.

- [ ] **Step 1:** Find them: `grep -rnE "FONT_W|\* *8\b|8 *\*|fb_text_width" kernel/ user/ include/`. List each (menu bar clock in `wm.c`, Dock labels, Finder rows, window titles, app headers).

- [ ] **Step 2:** For each, replace char-count×8 width/centering with `text_width(s)` (or `text_width_sz`). Pick UI sizes per element (menu bar/titles maybe `TEXT_UI_PX`, smaller labels 13–14px). One commit per coherent group (wm chrome; then per app).

- [ ] **Step 3:** After each group: rebuild, QMP screenshot the desktop/app, verify alignment looks right (no overlap/clipping).

- [ ] **Step 4: Commit** each group, e.g. `git commit -am "wm: proportional text width for menu bar/dock/titles (M14 L5)"`

---

## Task 8: Font subsetting tool (supports L1 onward)

**Files:** Create `tools/mkfont.py`; add `fsroot/fonts/` to the repo (with a LICENSE note).

- [ ] **Step 1: Write `tools/mkfont.py`:**
  - Input: a full Noto Sans CJK SC `.ttf/.otf` (path via arg or a vendored copy) + DejaVu Sans Mono.
  - Subset Noto with `fontTools.subset`: unicodes = `0x20..0x7E` (Latin) + GB2312 hanzi set + CJK punctuation (`0x3000..0x303F`, `0xFF00..0xFFEF`). Output `ui.ttf`. Copy DejaVu Mono → `mono.ttf`.
  - GB2312 hanzi set: enumerate GB2312 level-1+2 → Unicode (via `codecs` `gb2312`), collect the mapped codepoints.
  - Usage: `python3 tools/mkfont.py <out_ui.ttf> <out_mono.ttf>`.

- [ ] **Step 2:** Vendor the source fonts (or document the download) under `tools/fonts/` and emit the subset into `fsroot/fonts/ui.ttf` + `fsroot/fonts/mono.ttf`. Add SIL OFL / DejaVu license files.

- [ ] **Step 3:** Run it; confirm `ui.ttf` is ~3–5 MB and `ttf_test`/`raster_test` pass against it.

- [ ] **Step 4: Commit** `git add tools/mkfont.py fsroot/fonts && git commit -m "tools: subset Noto Sans CJK SC (GB2312) + DejaVu Mono for the disk (M14)"`

---

## Task 9: Pack fonts on disk + QEMU RAM (supports L3)

**Files:** Modify `Makefile`, `tools/mkfs.py`.

- [ ] **Step 1:** `mkfs.py`/`Makefile`: pack `fsroot/fonts/ui.ttf`→`/fonts/ui.ttf` and `mono.ttf`→`/fonts/mono.ttf` into `$(DISK)` (extend the `FS_FILES`/packing list). Grow the disk image block count if the 16 MB image can't hold the fonts + apps.

- [ ] **Step 2:** Add `-m 512M` to the QEMU command in the `run` target and in `scripts/run-test.sh`.

- [ ] **Step 3:** Build the disk; boot; confirm `vfs` lists `/fonts/ui.ttf` (Terminal `ls /fonts`) and `text_init` loads it (serial: print font load OK temporarily).

- [ ] **Step 4: Commit** `git commit -am "build: pack /fonts on the AquaFS disk; QEMU -m 512M (M14)"`

---

## Task 10: Terminal monospace + remove the bitmap font (L6)

**Files:** Modify `user/terminal.c` (and the WM terminal backend if rendering is kernel-side), then delete `include/font8x16.h`, `tools/genfont.py`.

- [ ] **Step 1:** Terminal text: render each cell with `text_draw_mono(x, y, ch_utf8, CELL_W, color)` against the MONO font (CJK falls back to UI, occupying 2 cells). Keep the existing column/row grid; CELL_W = mono advance at the chosen px.

- [ ] **Step 2:** Verify in QEMU: open Terminal, run `ls`, type ASCII + a Chinese string; screenshot — monospace Latin grid intact, Chinese double-width and readable.

- [ ] **Step 3:** Confirm nothing references `font8x16` (`grep -rn font8x16 .`); remove `#include "font8x16.h"` if any remains, then `git rm include/font8x16.h tools/genfont.py`.

- [ ] **Step 4:** `make clean && make test` → green; full clean build.

- [ ] **Step 5: Commit** `git commit -am "terminal: monospace AA text + CJK fallback; remove 8x16 bitmap font (M14 L6)"`

---

## Self-review

- **Spec coverage:** UTF-8 (T1) · TTF parse/cmap/advance (T2) · outlines incl composite (T3) · AA rasterizer with the 4×-oversample/fractional-coverage technique (T4) · cache + registry + fallback + layout + `fb_blit_glyph` (T5) · `fb_text` wrappers + HTML UTF-8 + Browser (T6) · whole-UI migration (T7) · font subsetting/fonts-on-disk + `-m 512M` (T8/T9) · Terminal monospace + delete `font8x16.h`/`genfont.py` (T10). All spec sections mapped.
- **Placeholders:** none — algorithms and test code specified; the rasterizer step gives the exact method (flatten quadratics, 4 sub-scanlines, nonzero winding, fractional edge coverage).
- **Type consistency:** `struct ttf_font`, `ttf_parse/ttf_glyph_id/ttf_advance/ttf_glyph_outline`, `text_init/text_draw/text_draw_sz/text_draw_mono/text_width/text_width_sz/text_line_height`, `text_raster`, `fb_blit_glyph` — names/signatures consistent across tasks.
- **Note:** Tasks 8–9 (font asset + disk packing) are prerequisites for the host TTF tests (T2+) and the in-kernel load (T5); do T8 early (right after T1) even though it's numbered later, or generate a temp host font.
