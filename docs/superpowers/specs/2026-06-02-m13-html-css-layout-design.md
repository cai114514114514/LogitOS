# Aqua OS — M13: HTML/CSS layout + images (the browser's body)

## Problem

The browser today fetches a page over from-scratch TLS, then `net/html.c`
**strips tags to a flat text stream** with clickable links. There is no DOM, no
CSS, no box model, no images — every page looks like the same monospace-ish
text dump. To be a real browser (and to set up JavaScript, M16, which needs a
DOM to manipulate), we need a real rendering engine: parse HTML into a DOM,
apply CSS, lay it out with the box model, and paint it with styles and **images**
(a hard requirement — a logo like Google's is the soul of a page).

This is part of a three-milestone arc the user approved:
- **M13** HTML/CSS layout + images — *this spec* (written from scratch).
- **M15** prerequisites for JS — enable SSE/FPU in the kernel (CR0/CR4 + FXSAVE
  on context switch), a small libc shim, heap room (JS needs IEEE doubles, and
  the kernel is currently `-mno-sse`).
- **M16** JavaScript — port a mature engine (borrowed) + write DOM bindings.

M13 stays **integer-only** (no SSE yet); image decoders are chosen/written to be
integer.

## Goals / non-goals

**Goals**
- Parse HTML → a real DOM (element + text nodes, attributes).
- Parse CSS (UA default sheet + page `<style>` + inline `style=`); cascade with
  specificity + inheritance → a computed style per node.
- Lay out with the **block + inline + inline-block** box model (vertical block
  stacking; inline text flow with word wrapping); produce a box tree with
  absolute geometry, text runs, and image references.
- **Images**: `<img>` (and `data:` URIs) fetched, decoded to RGBA, and painted
  with alpha. A **pluggable codec registry** (so JPEG/WebP slot in later) with a
  reusable DEFLATE module; PNG and GIF written from scratch this milestone.
- Paint a scrolled viewport of the box tree into the browser window; clickable
  links via hit-testing.

**Non-goals (v1)**
- No float / flex / grid / table layout, no `position`/z-index, no overflow
  scroll boxes, no external `.css`, no media queries, no animations/transitions.
- No JPEG / WebP / SVG (JPEG needs floats → after M15; SVG is a separate vector
  renderer). Such images render as a placeholder box, never a crash.
- No JavaScript (M16).

## Architecture (kernel-side pipeline, viewport paint — chosen "A")

The kernel runs the whole pipeline (it already does http/tls/render); the
browser app stays a thin shell that draws its own chrome (address bar) and asks
the kernel to paint the page viewport, scroll, and hit-test.

**Data flow:** `SYS_HTTP_GET(url) → http fetch bytes → dom_parse → css (UA +
page) → layout (fetch + decode <img>) → box tree stored`. Then each frame the
app calls `SYS_PAGE_RENDER(scroll)` to paint the viewport slice into its window
surface.

### Modules (one responsibility each)

| Module | Responsibility | Depends on |
|--------|----------------|------------|
| `lib/inflate.c` / `inflate.h` | DEFLATE/zlib decompress (RFC 1951, fixed + dynamic Huffman), integer. Reusable (PNG now; gzip HTTP later). | — |
| `lib/img.c` / `img.h` | Codec registry + uniform result `struct image { int w,h; uint8_t *rgba; }` (straight 8-bit RGBA). `img_decode(bytes,len,&out)` picks a decoder by signature. | inflate, kheap |
| `lib/png.c` | PNG decode (color types 0/2/3/4/6, 8-bit, palette + `tRNS`, non-interlaced; Adam7 if cheap) → registers with `img`. | inflate |
| `lib/gif.c` | GIF decode (LZW, palette, first frame) → registers with `img`. | — |
| `net/dom.c` / `dom.h` | HTML tokenizer + tree builder → DOM (`struct node`: element/text, tag id, attrs, children/sibling/parent). | — |
| `net/css.c` / `css.h` | CSS parser (selectors + declarations), built-in UA sheet, cascade + specificity + inheritance → computed style per node. | dom |
| `net/layout.c` / `layout.h` | block + inline + inline-block layout → box tree with geometry, text runs, image refs; fetches/decodes `<img>` via `res_fetch` + `img`. | dom, css, text(M14), img, http |
| `net/paint.c` / `paint.h` | paint a viewport slice (background, border, text via M14, image alpha-blit) into the target surface; link hit-test. | layout, fb, text |
| `net/http.c` | add `res_fetch(url, &buf, &len)`: a self-contained dns+tcp+tls+GET into a fresh kmalloc'd buffer (independent of the page buffer), for sub-resources (images). | tcp, tls, dns |
| `kernel/wm.c` | new syscalls below; `SYS_HTTP_GET` now drives the full pipeline. | layout, paint |
| `user/browser.c` | thin shell: load → render viewport → scroll → click-navigate; draws its own address bar. | — |

`net/html.c` (the de-tag renderer) and the text-stream API (`http_read`,
`http_link`, `html_render`) are **retired** — replaced by the DOM pipeline and
the page-render syscalls.

## Image codec subsystem (the "modern / extensible" requirement)

- **Uniform output:** every decoder yields straight 8-bit **RGBA** (`image.rgba`,
  `w*h*4` bytes, kmalloc'd). The compositor alpha-blends it (reusing the M14
  blend). Upper layers never see format details.
- **Registry:** `img_register(detect, decode)`; `img_decode` runs `detect` on the
  header bytes to pick the decoder. Adding JPEG later = one new file + one
  `img_register`; layout/paint unchanged.
- **`inflate` is its own module** so PNG and (later) gzip-encoded HTTP both use
  it — the deliberate split that avoids future pain.
- **PNG:** parse IHDR/PLTE/tRNS/IDAT/IEND; zlib-wrapped DEFLATE via `inflate`;
  per-scanline unfilter (None/Sub/Up/Average/Paeth); expand color types 0/2/3/4/6
  (8-bit) + palette/`tRNS` to RGBA. Adam7 interlace if it falls out cheaply,
  else non-interlaced only (the common case).
- **GIF:** logical screen + global/local palette, LZW image data, first frame to
  RGBA (transparent index honored).

## CSS support (broad, practical subset)

- **Selectors:** type, `.class`, `#id`, descendant (space), comma grouping;
  cascade by specificity (id > class > type) then source order.
- **Properties:** `display` (block/inline/inline-block/none); `color`;
  `background`/`background-color`; `font-size`, `font-weight`
  (normal/bold/100–900), `font-style`, `font-family` (parsed, mapped to our UI
  vs mono font); `margin`, `padding` (1–4-value shorthand, `auto`); `width`,
  `height`; `text-align`; `line-height`; `border` (solid color); `border-radius`;
  `text-decoration` (underline); `list-style`; `white-space`.
- **Colors:** `#rgb`, `#rrggbb`, `rgb()/rgba()`, ~140 named colors. **Units:**
  px, %, em (relative to font-size).
- **Inheritance:** `color` and `font-*` inherit; box-model properties do not.
- **UA default stylesheet (built in):** body margin; h1–h6 sizes + bold + margins;
  p margins; `a { color: #1a0dab; text-decoration: underline }`; b/strong bold;
  i/em italic; ul/ol/li indent + marker; pre/code monospace.

## Layout (block + inline + inline-block)

- **Block formatting:** block boxes stack vertically. Width = containing block
  content width − horizontal margins, or an explicit `width` (`margin: auto`
  centers). Height = sum of children (or explicit `height`). x/y assigned
  top-down; margins applied (no margin-collapsing in v1).
- **Inline formatting:** inline-level items (text runs, inline elements,
  inline-blocks, images) flow left→right into line boxes within the content
  width, **wrapping at word boundaries** (word widths measured with the M14
  engine at the computed font-size). Line height = max content height on the
  line. Inline styles set color/weight/style/underline.
- **inline-block / `<img>`:** an atomic inline box with intrinsic size (decoded
  image w/h, or styled width/height), placed in the line flow.
- **Output:** a box tree where each box carries absolute `x/y/w/h`, background/
  border style, optional text runs (text + font + size + color + position), and
  optional image ref + dest rect; `<a>` ancestry attaches an `href` for
  hit-testing. Layout canvas width = the browser window's content width.

## Syscalls + browser app

- `SYS_HTTP_GET(url)` (id 26, semantics expanded): fetch + dom + css + layout;
  returns 0 ok / `<0` error.
- `SYS_PAGE_RENDER` (new id 31): `(scroll_y, (vx<<16)|vy, (vw<<16)|vh)` — paint
  the document rows `[scroll_y, scroll_y+vh)` into the window surface at viewport
  rect (vx,vy,vw,vh) (below the app's address bar). Layout uses `vw` as canvas
  width.
- `SYS_PAGE_HEIGHT` (new id 32): `()` → total laid-out document height (px).
- `SYS_PAGE_HITTEST` (new id 33): `((x<<16)|y, scroll_y, buf)` — the link URL at
  viewport-local (x,y) given scroll, copied into `buf` (≤511), or `-1`.
- **Browser app:** `load(url)` → `SYS_HTTP_GET`, `scroll=0`, read
  `SYS_PAGE_HEIGHT`. Each frame: draw the address bar (its own chrome), call
  `SYS_PAGE_RENDER(scroll, viewport)`, `gui_flush`. Up/Down/PageUp/Down/wheel
  adjust `scroll` (clamped). Click → `SYS_PAGE_HITTEST` → if a URL, navigate
  (load it). Address bar still accepts a typed URL + Enter.

## Testing

- **Host:** `inflate` vs python `zlib` (compress known data, decompress with
  ours, compare). PNG/GIF decode vs `sips`/PIL RGBA on sample images (incl.
  palette + alpha). DOM tree structure on sample HTML. CSS computed style on
  sample rules (specificity, inheritance). Layout box geometry on a small page
  (assert x/y/w/h of a few boxes). Build each as `tools/t/*_test.c` linking the
  pure modules.
- **QEMU:** load a styled page (zh.wikipedia) and a page with a PNG logo;
  screenshot — eyeball styled blocks, fonts/colors, wrapping, and the image.
- `make test` (AQUA_BOOT_OK) stays green throughout.

## Build order (layered, each verifiable)

- **L1** `lib/inflate.c` — vs zlib vectors (host).
- **L2** `lib/img.c` + `lib/png.c` + `lib/gif.c` — decode sample PNG/GIF vs
  reference RGBA (host); write PPM to eyeball.
- **L3** `net/dom.c` — tokenize + build tree; assert structure on samples (host).
- **L4** `net/css.c` — parse + UA sheet + cascade; assert computed styles (host).
- **L5** `net/layout.c` — block/inline/inline-block geometry + `res_fetch`/`<img>`
  decode; assert box geometry on a small page (host).
- **L6** `net/paint.c` + syscalls + `user/browser.c` rewrite; retire `html.c`
  text API. QEMU: styled page + image screenshot.

## Risks

- **Scope:** this is a large engine. The layered build keeps each piece
  host-testable before integration. Block+inline only (no float/flex) bounds it.
- **Sub-resource fetch:** images need re-entrant fetching independent of the page
  buffer — `res_fetch` uses its own kmalloc'd buffers; bounded count/size.
- **Memory:** DOM + box tree + decoded images can be large; cap image dimensions
  and total image memory; free a page's trees/images on navigation.
- **Integer-only:** all decoders + layout avoid floats (kernel is `-mno-sse`
  until M15); percentages/em use integer math.
- **`now`/TLS** for image HTTPS reuses the M12 path (RTC time, IF=1 in the
  syscall).
