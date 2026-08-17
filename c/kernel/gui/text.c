#include "text.h"
#include "ttf.h"
#include "utf8.h"
#include "shape.h"
#include "script.h"
#include "bidi.h"
#include "fb.h"
#include "vfs.h"
#include "kheap.h"
#include "kprintf.h"
#include "spinlock.h"

void *memcpy(void *, const void *, size_t);

/* Loaded fonts, in fallback order after whichever one was asked for.
 *   UI    proportional, Latin + CJK (a Noto Sans SC subset)
 *   MONO  the Terminal's fixed-pitch Latin
 *   TEXT  DejaVu Sans, vendored unmodified: the only one of the three with
 *         Arabic and Hebrew, and the only one with GSUB/GPOS at all -- the two
 *         Noto subsets lost their layout tables to subsetting, so without this
 *         font the shaper has nothing to apply. See third_party/fonts/README.md.
 */
enum { F_UI = 0, F_MONO = 1, F_TEXT = 2, NFONT = 3 };
static struct ttf_font fonts[NFONT];
static int font_ok[NFONT];

static int load_font(const char *path, int idx)
{
    int sz = vfs_size(path);
    if (sz <= 0) { kprintf("[text] %s: not found\n", path); return -1; }
    uint8_t *buf = kmalloc(sz);
    if (!buf) { kprintf("[text] %s: oom\n", path); return -1; }
    if (vfs_read(path, buf, sz) != sz) { kprintf("[text] %s: read fail\n", path); kfree(buf); return -1; }
    if (ttf_parse(buf, sz, &fonts[idx]) != 0) { kprintf("[text] %s: parse fail\n", path); kfree(buf); return -1; }
    /* success: fonts[idx] points into buf, so buf is intentionally NOT freed. */
    font_ok[idx] = 1;
    kprintf("[text] %s: %d glyphs, upem=%d\n", path, fonts[idx].num_glyphs, fonts[idx].units_per_em);
    return 0;
}

void text_init(void)
{
    load_font("/fonts/ui.ttf", F_UI);
    load_font("/fonts/mono.ttf", F_MONO);
    load_font("/fonts/text.ttf", F_TEXT);
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

/* ---------------------------------------------------------------- layout --
 *
 * Everything below goes through shape_line() -- ONE function, whether it is
 * measuring or drawing. That is not tidiness, it is the invariant the display
 * line stated: text_measure and text_draw_run must agree at the same px,
 * because wm.c measures at S(px) and divides the answer back. Shaping breaks
 * any measurement that walks characters: "fi" is one glyph, "AV" is narrower
 * than A plus V. A separate measuring path would drift a few pixels per line
 * in a way nobody could reproduce, so there is not one.
 *
 * The scratch is file scope because the whole UI runs under the big kernel
 * lock, and because a 32 KiB kernel stack has no room for it. */

#define TL_CP    1024
#define TL_GLYPH 2048
#define TL_RUN     64

static uint32_t tl_cps[TL_CP];
static uint8_t  tl_levels[TL_CP];
static int      tl_order[TL_CP];
static struct shape_glyph tl_glyphs[TL_GLYPH];
static struct text_run    tl_runs[TL_RUN];
/* bidi_scratch_size is 26n + slack; 32 KiB covers TL_CP with room to spare. */
static uint8_t  tl_bidi[32 * 1024];

/* The blit callback. The pixel size rides in the context because shape_emit
 * does not pass it -- the shaper has no business knowing what a raster is.
 *
 * `map` translates the shaper's font-set index into an index into fonts[].
 * They are NOT the same number: the set is built in preference order for this
 * particular call, so set slot 1 is the second CHOICE, not fonts[1]. Feeding
 * the set index straight to the glyph cache rasterizes the right glyph id out
 * of the wrong font, which renders as nothing at all when that font is a
 * 97-glyph subset. */
struct emit_ctx { int base, px; uint32_t color; const int *map; };

static void emit_blit(void *ud, int fidx, int gid, int x, int y_off)
{
    struct emit_ctx *e = (struct emit_ctx *)ud;
    struct gentry *g = glyph_get(e->map[fidx], gid, e->px);
    if (g->cov)
        fb_blit_glyph(x + g->ox, e->base - y_off - g->oy, g->cov, g->w, g->h, e->color);
}

static void tl_scratch(struct shape_scratch *sc)
{
    sc->cps = tl_cps; sc->levels = tl_levels; sc->order = tl_order;
    sc->glyphs = tl_glyphs; sc->runs = tl_runs; sc->bidi = tl_bidi;
    sc->ncp_cap = TL_CP; sc->nglyph_cap = TL_GLYPH; sc->nrun_cap = TL_RUN;
    sc->bidi_cap = (int)sizeof tl_bidi;
}

/* Font preference order for a run: the requested font, then UI, then the
 * shaping font, then mono. A code point none of them covers renders as .notdef
 * in the first, which is visible rather than silently missing. */
static void tl_fonts(struct shape_font_set *fs, int prefer, int *map)
{
    static const int order[NFONT] = { F_UI, F_TEXT, F_MONO };
    fs->n = 0;
    if (font_ok[prefer]) { map[fs->n] = prefer; fs->f[fs->n++] = &fonts[prefer]; }
    for (int i = 0; i < NFONT && fs->n < SHAPE_MAX_FONTS; i++)
        if (order[i] != prefer && font_ok[order[i]]) {
            map[fs->n] = order[i];
            fs->f[fs->n++] = &fonts[order[i]];
        }
}

/* THE TEXT LOCK, AND WHY IT IS ONE LOCK AND NOT THREE.
 *
 * The comment above the scratch says the quiet part: "the whole UI runs under
 * the big kernel lock". That is the only thing keeping ~45 KiB of file-scope
 * shaping scratch (tl_cps, tl_levels, tl_order, tl_glyphs, tl_runs, tl_bidi),
 * the 40 KiB rasteriser scratch, and the 2048-entry glyph cache from being torn
 * by two cores at once -- and step 2 of the BKL removal takes that away.
 *
 * One lock covers all three because they are all reached through ONE function.
 * layout() is the single entry every public text call funnels into, for a
 * reason stated further up: measuring and drawing must agree at the same px, so
 * there is deliberately no second path. Splitting this into a shaping lock and
 * a cache lock would buy nothing (nobody takes one without the other) and cost
 * an ordering rule that only exists to be got wrong.
 *
 * IT ALSO SETTLES THE GLYPH CACHE'S UGLY CASE FOR FREE. glyph_get returns a
 * POINTER INTO the cache table, and its caller reads e->cov after it returns --
 * so a lock inside glyph_get would protect the probe and leave the use
 * unguarded, with eviction free to kfree() the coverage buffer under a blit.
 * Held out here, the entry cannot be evicted while it is being used, because
 * the only code that evicts is inside the same lock.
 *
 * ORDERING: BKL -> text_lock -> kheap_lock. Text is never entered from an
 * interrupt (input is deferred to the WM thread -- see wm.c's input-deferral
 * note), so this is the bare spin_lock and not the irqsave form; and nothing
 * under this lock takes the BKL, so the order cannot invert.
 *
 * TODAY THIS LOCK IS NEVER CONTENDED, on purpose. The BKL still serialises
 * everything above it, so adding it changes no behaviour and can be verified
 * against test-desktop-look's 16 recorded values byte for byte. It starts doing
 * work the moment the compositor stops holding the BKL across its pixel pass,
 * which is the next piece. Introducing it a step early is what makes that step
 * a small change instead of a flag day.
 */
static spinlock_t text_lock = SPINLOCK_INIT;

/* The one layout entry point. `draw` = 0 measures, 1 draws. Returns end x. */
static int layout_locked(int x, int y, const char *s, int len, int prefer, int px,
                         int cell, uint32_t color, int draw)
{
    if (!font_ok[F_UI] && !font_ok[F_MONO] && !font_ok[F_TEXT]) return x;
    if (len <= 0) return x;

    struct shape_font_set fs;
    int map[SHAPE_MAX_FONTS];
    tl_fonts(&fs, prefer, map);
    if (fs.n == 0) return x;

    struct shape_scratch sc;
    tl_scratch(&sc);

    if (!draw) return shape_line(&fs, s, len, px, cell, x, 0, &sc);

    struct emit_ctx ec = { y + ascent_px(map[0], px), px, color, map };
    struct shape_emit em = { emit_blit, &ec };
    return shape_line(&fs, s, len, px, cell, x, &em, &sc);
}

static int layout(int x, int y, const char *s, int len, int prefer, int px,
                  int cell, uint32_t color, int draw)
{
    spin_lock(&text_lock);
    int r = layout_locked(x, y, s, len, prefer, px, cell, color, draw);
    spin_unlock(&text_lock);
    return r;
}

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

int text_draw_sz(int x, int y, const char *utf8, int px, uint32_t color)
{ return layout(x, y, utf8, slen(utf8), F_UI, px, 0, color, 1); }

int text_draw(int x, int y, const char *utf8, uint32_t color)
{ return layout(x, y, utf8, slen(utf8), F_UI, TEXT_UI_PX, 0, color, 1); }

int text_draw_mono(int x, int y, const char *utf8, int cell_w, uint32_t color)
{ return layout(x, y, utf8, slen(utf8), F_MONO, TEXT_UI_PX, cell_w, color, 1); }

/* Same, at an explicit pixel size. The Terminal picks its cell width in points
 * and the WM scales BOTH the cell and the glyph size, so a 2x display draws
 * genuinely larger glyphs rather than the same glyphs in wider cells. */
int text_draw_mono_sz(int x, int y, const char *utf8, int px, int cell_w, uint32_t color)
{ return layout(x, y, utf8, slen(utf8), F_MONO, px, cell_w, color, 1); }

int text_width_sz(const char *utf8, int px)
{ return layout(0, 0, utf8, slen(utf8), F_UI, px, 0, 0, 0); }

int text_width(const char *utf8)
{ return layout(0, 0, utf8, slen(utf8), F_UI, TEXT_UI_PX, 0, 0, 0); }

/* Measure a length-delimited UTF-8 run at `px`, in the mono or UI font (for the
 * layout engine's word-wrap). Same function as the draw below, one argument
 * apart -- see the note above. */
int text_measure(const char *s, int len, int px, int mono)
{ return layout(0, 0, s, len, mono ? F_MONO : F_UI, px, 0, 0, 0); }

/* Draw a length-delimited UTF-8 run at (x, y=top) in the UI or mono font at
 * `px`, returning the end x. For the layout engine's display list. */
int text_draw_run(int x, int y, const char *s, int len, int px, int mono, uint32_t color)
{ return layout(x, y, s, len, mono ? F_MONO : F_UI, px, 0, color, 1); }

int text_line_height(int px)
{
    int f = font_ok[F_UI] ? F_UI : F_MONO;
    if (!font_ok[f]) return px + px/4;
    return (int)(((long)(fonts[f].ascent - fonts[f].descent + fonts[f].line_gap) * px) / fonts[f].units_per_em);
}
