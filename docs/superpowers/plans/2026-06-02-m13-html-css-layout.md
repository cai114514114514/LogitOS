# M13 — HTML/CSS layout + images — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the de-tag-to-text renderer with a real from-scratch engine: HTML→DOM, CSS cascade, block/inline box-model layout, and `<img>` images (PNG/GIF), painted into a scrolled viewport with clickable links.

**Architecture:** Kernel-side pipeline (the kernel already does http/tls); the browser app is a thin shell. `SYS_HTTP_GET` runs fetch→dom→css→layout (fetching/decoding `<img>` on the way); `SYS_PAGE_RENDER(scroll,viewport)` paints the viewport slice into the app window; `SYS_PAGE_HITTEST` returns link URLs. A pluggable image-codec registry over a reusable integer DEFLATE module keeps formats extensible. Integer-only (kernel is `-mno-sse` until M15).

**Tech Stack:** C (freestanding, `clang --target=x86_64-elf`, `-mno-sse`), QEMU; host `clang` + Python (`zlib`, `sips`/PIL) for tests.

---

## Interfaces (locked here; later tasks must match)

```c
/* include/inflate.h */
int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen); /* RFC1951; 0 ok */
int zlib_decompress(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen); /* skips 2-byte zlib hdr + adler */

/* include/img.h */
struct image { int w, h; uint8_t *rgba; };              /* straight 8-bit RGBA, kmalloc'd */
typedef int (*img_detect_fn)(const uint8_t *p, int n);  /* 1 if this decoder handles it */
typedef int (*img_decode_fn)(const uint8_t *p, int n, struct image *out); /* 0 ok */
void img_register(img_detect_fn d, img_decode_fn f);
int  img_decode(const uint8_t *p, int n, struct image *out);  /* 0 ok, -1 unsupported */
void img_free(struct image *im);
void png_register(void);  void gif_register(void);       /* called from img_init */
void img_init(void);

/* include/dom.h */
enum { N_ELEM, N_TEXT };
struct attr { char name[32]; char val[256]; };
struct node {
    int type; char tag[16];                              /* lowercased tag (N_ELEM) */
    struct attr *attrs; int nattr;
    char *text; int textlen;                             /* N_TEXT */
    struct node *parent, *first_child, *next;
    void *style;                                         /* struct cstyle* (css) */
};
struct node *dom_parse(const char *html, int len);       /* returns root (<html>/document) */
const char *dom_attr(const struct node *n, const char *name);  /* value or NULL */
void dom_free(struct node *root);

/* include/css.h */
struct cstyle {                                          /* computed style (px ints) */
    int display;        /* DISP_BLOCK/INLINE/INLINE_BLOCK/NONE */
    uint32_t color, background; int has_bg;
    int font_px, bold, italic, mono;
    int mt, mr, mb, ml, pt, pr, pb, pl;                  /* margins, paddings */
    int width, height, has_w, has_h;                     /* -1 = auto */
    int text_align;     /* 0 left 1 center 2 right */
    int line_px;
    int border_w; uint32_t border_color;
    int radius; int underline; int list_item;
};
void css_init(void);                                     /* build UA default sheet */
void css_apply(struct node *root, const char *page_css, int css_len); /* sets node->style for all */

/* include/layout.h */
void layout_page(struct node *root, int canvas_w);       /* builds the box tree, fetches <img> */
int  layout_height(void);                                /* total document px */
void layout_free(void);

/* include/paint.h */
void paint_viewport(int vx, int vy, int vw, int vh, int scroll_y);   /* into current fb target */
int  paint_hittest(int x, int y, int scroll_y, char *url, int max);  /* 0 + url, or -1 */

/* net/http.h additions */
int res_fetch(const char *url, uint8_t **buf, int *len);  /* sub-resource GET; *buf kmalloc'd; 0 ok */
```

New syscalls (`include/aqua_abi.h`): `SYS_PAGE_RENDER 31`, `SYS_PAGE_HEIGHT 32`, `SYS_PAGE_HITTEST 33`.

---

## Task 1: DEFLATE / zlib inflate (L1)

**Files:** Create `include/inflate.h`, `lib/inflate.c`, `tools/t/inflate_test.c`.

- [ ] **Step 1:** Write `include/inflate.h` (prototypes above).
- [ ] **Step 2: Failing host test** `tools/t/inflate_test.c`: read a raw-DEFLATE blob + its expected output from argv files, call `inflate_raw`, compare bytes; also a zlib blob through `zlib_decompress`. Generate fixtures:
  `python3 -c "import zlib,sys; d=open('/tmp/orig','rb').read(); open('/tmp/zl','wb').write(zlib.compress(d,9)); import struct; co=zlib.compressobj(9,zlib.DEFLATED,-15); open('/tmp/raw','wb').write(co.compress(d)+co.flush())"` (raw = `-15` window = no zlib header).
- [ ] **Step 3:** Run, verify fails (no `inflate_raw`). `clang -Iinclude tools/t/inflate_test.c lib/inflate.c -o /tmp/inf && /tmp/inf`
- [ ] **Step 4: Implement `lib/inflate.c`:** bit reader (LSB-first); block loop: BFINAL/BTYPE; stored (type 0: copy LEN bytes); fixed Huffman (type 1: built-in lit/len + dist tables); dynamic Huffman (type 2: read HLIT/HDIST/HCLEN, code-length code lengths, build the two Huffman trees via canonical codes). LZ77 back-references (length 3–258, distance 1–32768) copy from `out`. Canonical Huffman decode by first-code-per-length tables. `zlib_decompress` skips the 2-byte header and ignores the trailing adler32. All integer.
- [ ] **Step 5:** Run, verify PASS (decompressed == original for both raw and zlib, on a few KB of mixed text + repeats).
- [ ] **Step 6: Commit** `git commit -m "inflate: from-scratch DEFLATE/zlib decompress (M13 L1)"`

---

## Task 2: Image codecs — registry + PNG + GIF (L2)

**Files:** Create `include/img.h`, `lib/img.c`, `lib/png.c`, `lib/gif.c`, `tools/t/img_test.c`.

- [ ] **Step 1:** Write `include/img.h` (above). `lib/img.c`: a small registry (array of `{detect,decode}`); `img_init` calls `png_register`+`gif_register`; `img_decode` runs each `detect` on the first 16 bytes, calls the match; `img_free` frees `rgba`.
- [ ] **Step 2: Failing host test** `tools/t/img_test.c`: decode `/tmp/t.png` and `/tmp/t.gif` (created below) via `img_decode`; assert w/h match a reference (from `sips -g pixelWidth -g pixelHeight`), and spot-check a few pixels against a reference RGBA (PIL: `python3 -c "from PIL import Image;im=Image.open('/tmp/t.png').convert('RGBA');print(im.size, im.getpixel((1,1)))"`). Write decoded RGBA to `/tmp/out.ppm` to eyeball. Create fixtures with PIL (a small image with palette+alpha for PNG, and a GIF).
- [ ] **Step 3:** Run, verify fails.
- [ ] **Step 4a: `lib/png.c`:** verify 8-byte signature; walk chunks (length, type, data, CRC — CRC may be skipped). IHDR: width/height/bitdepth(==8)/colortype(0,2,3,4,6)/interlace. PLTE/tRNS for palette + transparency. Concatenate IDAT, `zlib_decompress` → filtered scanlines (each prefixed by a filter byte; bytes-per-pixel from color type). Unfilter None/Sub/Up/Average/Paeth. Expand each color type to RGBA (palette index → PLTE+tRNS; gray → r=g=b; add alpha 255 or from type/tRNS). Adam7 interlace: if `interlace==1`, either implement the 7-pass de-interleave or (acceptable v1) return -1 for interlaced. `png_register(detect= bytes start \x89PNG)`.
- [ ] **Step 4b: `lib/gif.c`:** parse header (`GIF87a`/`GIF89a`), logical screen descriptor, global color table; skip extensions; image descriptor (+ local color table); LZW decode (variable code width, clear/EOI codes, dictionary) → palette indices → RGBA (transparent index from a Graphic Control Extension if present). First image only. `gif_register(detect= "GIF8")`.
- [ ] **Step 5:** Run, verify PASS (w/h + sampled pixels match; eyeball `/tmp/out.ppm`).
- [ ] **Step 6: Commit** `git commit -m "img: pluggable codec registry + from-scratch PNG + GIF -> RGBA (M13 L2)"`

---

## Task 3: HTML → DOM (L3)

**Files:** Create `include/dom.h`, `net/dom.c`, `tools/t/dom_test.c`.

- [ ] **Step 1:** Write `include/dom.h` (above).
- [ ] **Step 2: Failing host test** `tools/t/dom_test.c`: `dom_parse` a sample (`"<html><body><h1 id=t>Hi</h1><p class='a b'>x<a href=/y>z</a></p><img src=p.png></body></html>"`); assert the tree: html>body>{h1>text("Hi"), p>{text("x"), a(href=/y)>text("z")}, img(src=p.png)}; check `dom_attr` returns id/class/href/src; check void elements (`img`, `br`, `hr`, `meta`, `link`, `input`) have no children.
- [ ] **Step 3:** Run, verify fails.
- [ ] **Step 4: Implement `net/dom.c`:** tokenizer — text runs; tags `<name attrs>`, `</name>`, self-closing `/>`, comments `<!-- -->`, `<!doctype>`. Parse attributes (`name`, `name=value`, quoted/unquoted). Lowercase tag names. Tree builder with an open-element stack: push on start tag, pop on matching end tag (tolerate mismatches by popping to the nearest match); **void elements** never push; **`<script>`/`<style>`** consume raw text until their close tag (content kept as a text child for css/js). Decode entities in text (reuse the entity logic from the old html.c: named + numeric → UTF-8). Allocate nodes/attrs from kheap; arena-style free in `dom_free`.
- [ ] **Step 5:** Run, verify PASS.
- [ ] **Step 6: Commit** `git commit -m "dom: HTML tokenizer + tree builder -> DOM (M13 L3)"`

---

## Task 4: CSS parse + cascade (L4)

**Files:** Create `include/css.h`, `net/css.c`, `tools/t/css_test.c`. Modify `net/dom.c` only if `node->style` wiring needs a hook (it's a `void*`, set by css).

- [ ] **Step 1:** Write `include/css.h` (`struct cstyle` + prototypes above) and the display/text-align enums.
- [ ] **Step 2: Failing host test** `tools/t/css_test.c`: build a small DOM, `css_init()`, `css_apply(root, "h1{color:#f00;font-size:32px} .a{font-weight:bold} #t{margin:10px}", len)`; assert computed styles: h1 → color red, font_px 32; element with class a → bold; `#t` → ml/mr/mt/mb 10; that `color` inherits to children; that a UA rule (e.g. `a` underline+blue) applies; specificity (id rule beats class beats type on conflict).
- [ ] **Step 3:** Run, verify fails.
- [ ] **Step 4: Implement `net/css.c`:**
  - Parser: split into rules `selector-list { decl; decl }`; selectors = simple selectors joined by descendant combinator (space); each simple selector = optional type + `.class`* + `#id`. Declarations `prop: value`.
  - Value parsing: colors (`#rgb`/`#rrggbb`/`rgb()/rgba()`/named-table), lengths (px/%/em → resolved to px at use, % vs containing/font), keywords.
  - UA default sheet: a string compiled at `css_init` (body/h1–h6/p/a/b/strong/i/em/ul/ol/li/pre/code…).
  - Cascade: for each node, gather matching rules from (UA, then page, then inline `style=`), sort by (origin, specificity, source order), apply in order into a fresh `cstyle`; then resolve inherited props (`color`, `font-*`) from the parent's computed style. Store `cstyle*` in `node->style` (kmalloc).
  - Selector match walks ancestors for descendant combinator.
- [ ] **Step 5:** Run, verify PASS.
- [ ] **Step 6: Commit** `git commit -m "css: parser + UA default sheet + cascade/specificity/inheritance (M13 L4)"`

---

## Task 5: Layout — block/inline/inline-block + images (L5)

**Files:** Create `include/layout.h`, `net/layout.c`, `tools/t/layout_test.c`. Modify `net/http.c` (+ `http.h`) to add `res_fetch`.

- [ ] **Step 1:** Write `include/layout.h` (above) and the box-tree types in `net/layout.c` (`struct box { int x,y,w,h; struct cstyle *st; struct textrun *runs; int nrun; struct image *img; int img_w,img_h; char href[256]; struct box *kids; int nkid; }`, `struct textrun { int x,y,font_px,bold,italic,mono; uint32_t color; int underline; char *text; int len; }`).
- [ ] **Step 2:** Add `res_fetch` to `net/http.c`: like `fetch_once` but into a freshly `kmalloc`'d buffer returned to the caller (caller frees); reuses dns/tcp/tls + `now_unix`. Handle `data:` URIs (base64 decode inline, no network).
- [ ] **Step 3: Failing host test** `tools/t/layout_test.c`: build DOM+CSS for `"<body style='width:200px'><h1>Hi</h1><p>one two three four five six seven</p></body>"`, `layout_page(root, 200)`, then walk the box tree (exposed via a test hook) and assert: h1 box y=0-ish below body margin, p below h1, the long `<p>` text wraps into ≥2 line boxes within width 200, `layout_height()` > h1+one line. (Image fetch is network — skip in host test; covered in QEMU.)
- [ ] **Step 4: Implement `net/layout.c`:**
  - Recursive block layout: assign each block its content width (canvas/containing − margins/padding, or explicit), place children top-down accumulating y, apply margins; height = children extent or explicit.
  - Inline formatting context: collect inline-level descendants of a block into line boxes; measure words with the M14 text engine at the computed font_px (need a `text_measure_px(s,len,px,mono)` helper — add to `text.c`); wrap at width; emit `textrun`s with positions; line height from max font/line on the line; inline-block/img placed as atomic boxes.
  - `<img>`: resolve `src` against the page URL; `res_fetch` → `img_decode`; intrinsic w/h (or styled); emit an image box (placeholder box if decode fails). Cap dimensions/total image memory.
  - `layout_height` returns the body's bottom; `layout_free` frees boxes + decoded images.
- [ ] **Step 5:** Run host test, verify PASS (wrapping + stacking geometry).
- [ ] **Step 6: Commit** `git commit -m "layout: block/inline/inline-block box model + <img> fetch/decode; res_fetch (M13 L5)"`

---

## Task 6: Paint + syscalls + browser app; retire the text renderer (L6)

**Files:** Create `include/paint.h`, `net/paint.c`. Modify `kernel/wm.c`, `include/aqua_abi.h`, `user/aqua.h`, `user/browser.c`. Remove `net/html.c` + its API once unused.

- [ ] **Step 1: `net/paint.c`:** `paint_viewport(vx,vy,vw,vh,scroll)` walks the box tree; for boxes intersecting `[scroll, scroll+vh)`: fill `background` (rounded if `radius`), draw `border`, draw each `textrun` via `text_draw_sz`/`_mono` at `(vx + run.x, vy + run.y - scroll)` in its color (+ underline line), `fb_blit` images (alpha) scaled to dest rect. Clip to the viewport rect. `paint_hittest(x,y,scroll,buf,max)` finds the topmost box with an `href` containing `(x, y+scroll-vy)` → copies its href.
- [ ] **Step 2:** `aqua_abi.h`: add `SYS_PAGE_RENDER 31`, `SYS_PAGE_HEIGHT 32`, `SYS_PAGE_HITTEST 33`. `user/aqua.h`: `page_render(scroll,vx,vy,vw,vh)`, `page_height()`, `page_hittest(x,y,scroll,buf,max)` inline wrappers (pack args as the abi comments specify).
- [ ] **Step 3:** `kernel/wm.c`: `SYS_HTTP_GET` → `http_get` (now: fetch → `dom_parse` → `css_apply` → `layout_page(viewport_w)`; store root + box tree; free the previous page first). Add cases `SYS_PAGE_RENDER` (set `fb_target(&w->surf)`, `paint_viewport(...)`, target NULL), `SYS_PAGE_HEIGHT` (`return layout_height()`), `SYS_PAGE_HITTEST` (`paint_hittest` into the user buffer). The pipeline runs with IF=1 (same as the M12 SYS_HTTP_GET fix) because `res_fetch` blocks on the net.
- [ ] **Step 4:** Rewrite `user/browser.c`: keep the address bar (typed URL + Enter → `load`). `load(url)`: `http_get(url)`; on ok `scroll=0`, `ph=page_height()`. Main loop: `gui_clear`; draw address bar; `page_render(scroll, 0, BARH, WINW, WINH-BARH)`; `gui_flush`. Keys: Up/Down/PageUp/PageDown adjust `scroll` (clamp `0..max(0,ph-(WINH-BARH))`). Mouse click in the page area → `page_hittest(mx, my-BARH, scroll, buf)` → if URL, `load(buf)`. Remove the old `http_read`/`http_link` text rendering.
- [ ] **Step 5:** Remove `net/html.c` and the `http_read`/`http_link`/`html_render` declarations + the now-unused `SYS_HTTP_READ/STATUS/LINK` handling (or keep STATUS). Ensure nothing references them (`grep`). `make clean && make test` green.
- [ ] **Step 6: QEMU verify:** via QMP, load `https://zh.wikipedia.org/` (styled text, wrapping, colors) and a page with a PNG logo; screenshot — eyeball layout + image. 
- [ ] **Step 7: Commit** `git commit -m "paint+wm+browser: render the box tree to a scrolled viewport, clickable links, images; retire the de-tag text renderer (M13 L6)"`

---

## Self-review

- **Spec coverage:** inflate (T1) · pluggable codecs + PNG + GIF → RGBA (T2) · HTML→DOM (T3) · CSS parse/UA/cascade/inheritance (T4) · block/inline/inline-block layout + `<img>` fetch/decode + `res_fetch`/data: (T5) · paint viewport + hit-test + the three new syscalls + browser rewrite + retire html.c (T6). All spec sections mapped.
- **Placeholders:** none — each task names exact files, the algorithm, host fixtures, and commands. Adam7 interlace is explicitly "implement or return -1" (bounded).
- **Type consistency:** `struct image`/`img_decode`/`img_free`/`png_register`/`gif_register`; `struct node`/`dom_parse`/`dom_attr`/`dom_free`; `struct cstyle`/`css_init`/`css_apply`; `layout_page`/`layout_height`/`layout_free`; `paint_viewport`/`paint_hittest`; `res_fetch`; syscalls 31/32/33 — consistent across tasks.
- **New helper noted:** `text_measure_px(text,len,px,mono)` added to `kernel/text.c` in T5 for word-wrap measurement (sibling of `text_width_sz`).
