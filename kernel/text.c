#include "text.h"
#include "ttf.h"
#include "utf8.h"
#include "fb.h"
#include "vfs.h"
#include "kheap.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);

/* Loaded fonts. UI = proportional CJK (Heiti subset); MONO = Menlo. Glyph
 * lookup falls back from a requested font to UI so the Terminal's mono font can
 * borrow CJK glyphs. */
enum { F_UI = 0, F_MONO = 1, NFONT = 2 };
static struct ttf_font fonts[NFONT];
static int font_ok[NFONT];

static int load_font(const char *path, int idx)
{
    int sz = vfs_size(path);
    if (sz <= 0) { kprintf("[text] %s: not found\n", path); return -1; }
    uint8_t *buf = kmalloc(sz);
    if (!buf || vfs_read(path, buf, sz) != sz) { kprintf("[text] %s: read fail\n", path); return -1; }
    if (ttf_parse(buf, sz, &fonts[idx]) != 0) { kprintf("[text] %s: parse fail\n", path); return -1; }
    font_ok[idx] = 1;
    kprintf("[text] %s: %d glyphs, upem=%d\n", path, fonts[idx].num_glyphs, fonts[idx].units_per_em);
    return 0;
}

void text_init(void)
{
    load_font("/fonts/ui.ttf", F_UI);
    load_font("/fonts/mono.ttf", F_MONO);
}

/* Resolve a code point to (font, gid), trying `prefer` then UI. */
static int resolve(uint32_t cp, int prefer, int *fidx)
{
    if (font_ok[prefer]) { int g = ttf_glyph_id(&fonts[prefer], cp); if (g) { *fidx = prefer; return g; } }
    if (prefer != F_UI && font_ok[F_UI]) { int g = ttf_glyph_id(&fonts[F_UI], cp); if (g) { *fidx = F_UI; return g; } }
    *fidx = font_ok[prefer] ? prefer : F_UI;
    return 0;                                  /* .notdef */
}

/* --- glyph cache (open addressing with hash-slot eviction) --- */
#define CACHE_N 2048
struct gentry { int used, fidx, gid, px; uint8_t *cov; int w, h, ox, oy, adv; };
static struct gentry cache[CACHE_N];
static uint8_t rastbuf[200 * 200];             /* scratch for one glyph rasterization */

static struct gentry *glyph_get(int fidx, int gid, int px)
{
    unsigned h0 = ((unsigned)fidx * 131u + (unsigned)gid * 2654435761u + (unsigned)px * 97u) % CACHE_N;
    for (int probe = 0; probe < 8; probe++) {
        struct gentry *e = &cache[(h0 + probe) % CACHE_N];
        if (e->used && e->fidx == fidx && e->gid == gid && e->px == px) return e;
        if (!e->used) { h0 = (h0 + probe) % CACHE_N; goto fill; }
    }
    h0 = h0 % CACHE_N;                          /* table full at probe window: evict slot */
    if (cache[h0].used && cache[h0].cov) kfree(cache[h0].cov);
fill: ;
    struct gentry *e = &cache[h0];
    int w, h, ox, oy;
    if (text_raster(&fonts[fidx], gid, px, rastbuf, (int)sizeof rastbuf, &w, &h, &ox, &oy) != 0) {
        w = h = 0; ox = oy = 0;
    }
    uint8_t *cov = 0;
    if (w > 0 && h > 0) { cov = kmalloc(w * h); if (cov) memcpy(cov, rastbuf, w * h); }
    e->used = 1; e->fidx = fidx; e->gid = gid; e->px = px;
    e->cov = cov; e->w = cov ? w : 0; e->h = h; e->ox = ox; e->oy = oy;
    e->adv = (int)(((long)ttf_advance(&fonts[fidx], gid) * px) / fonts[fidx].units_per_em);
    return e;
}

static int ascent_px(int fidx, int px) { return (int)(((long)fonts[fidx].ascent * px) / fonts[fidx].units_per_em); }

/* Draw `utf8` at (x, y=top). `prefer` font, `px` size, snapping advance to
 * `cell` when cell>0 (monospace: narrow=cell, wide CJK=2*cell). Returns end x. */
static int draw(int x, int y, const char *utf8, int prefer, int px, int cell, uint32_t color)
{
    if (!font_ok[F_UI] && !font_ok[F_MONO]) return x;
    int base = y + ascent_px(font_ok[prefer] ? prefer : F_UI, px);
    uint32_t cp;
    for (const char *s = utf8; *s; ) {
        s = utf8_next(s, &cp);
        if (!cp) break;
        int fidx, gid = resolve(cp, prefer, &fidx);
        struct gentry *g = glyph_get(fidx, gid, px);
        int adv = g->adv;
        if (cell > 0) adv = (g->adv > cell * 3 / 2) ? cell * 2 : cell;   /* mono cells */
        if (g->cov) fb_blit_glyph(x + g->ox + (cell ? (adv - g->w) / 2 : 0), base - g->oy,
                                  g->cov, g->w, g->h, color);
        x += adv;
    }
    return x;
}

static int measure(const char *utf8, int prefer, int px, int cell)
{
    uint32_t cp; int x = 0;
    for (const char *s = utf8; *s; ) {
        s = utf8_next(s, &cp); if (!cp) break;
        int fidx, gid = resolve(cp, prefer, &fidx);
        struct gentry *g = glyph_get(fidx, gid, px);
        x += cell > 0 ? ((g->adv > cell * 3 / 2) ? cell * 2 : cell) : g->adv;
    }
    return x;
}

int text_draw_sz(int x, int y, const char *utf8, int px, uint32_t color){ return draw(x,y,utf8,F_UI,px,0,color); }
int text_draw(int x, int y, const char *utf8, uint32_t color){ return draw(x,y,utf8,F_UI,TEXT_UI_PX,0,color); }
int text_draw_mono(int x, int y, const char *utf8, int cell_w, uint32_t color){ return draw(x,y,utf8,F_MONO,TEXT_UI_PX,cell_w,color); }
int text_width_sz(const char *utf8, int px){ return measure(utf8,F_UI,px,0); }
int text_width(const char *utf8){ return measure(utf8,F_UI,TEXT_UI_PX,0); }

/* Measure a length-delimited UTF-8 run at `px`, in the mono or UI font (for the
 * layout engine's word-wrap). */
int text_measure(const char *s, int len, int px, int mono)
{
    int prefer = mono ? F_MONO : F_UI;
    uint32_t cp; int x = 0; const char *e = s + len;
    for (const char *p = s; p < e; ) {
        p = utf8_next(p, &cp); if (!cp) break;
        int fi, g = resolve(cp, prefer, &fi);
        x += glyph_get(fi, g, px)->adv;
    }
    return x;
}
/* Draw a length-delimited UTF-8 run at (x, y=top) in the UI or mono font at
 * `px`, returning the end x. For the layout engine's display list. */
int text_draw_run(int x, int y, const char *s, int len, int px, int mono, uint32_t color)
{
    int prefer = mono ? F_MONO : F_UI;
    if (!font_ok[F_UI] && !font_ok[F_MONO]) return x;
    int base = y + ascent_px(font_ok[prefer] ? prefer : F_UI, px);
    uint32_t cp; const char *e = s + len;
    for (const char *p = s; p < e; ) {
        p = utf8_next(p, &cp); if (!cp) break;
        int fi, gid = resolve(cp, prefer, &fi);
        struct gentry *g = glyph_get(fi, gid, px);
        if (g->cov) fb_blit_glyph(x + g->ox, base - g->oy, g->cov, g->w, g->h, color);
        x += g->adv;
    }
    return x;
}

int text_line_height(int px)
{
    int f = font_ok[F_UI] ? F_UI : F_MONO;
    if (!font_ok[f]) return px + px/4;
    return (int)(((long)(fonts[f].ascent - fonts[f].descent + fonts[f].line_gap) * px) / fonts[f].units_per_em);
}
