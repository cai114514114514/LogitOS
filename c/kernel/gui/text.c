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
#include "logit_abi.h"   /* LOGIT_FACE_MONO / LOGIT_FACE_BOLD -- see face_font() */

void *memcpy(void *, const void *, size_t);

/* Loaded fonts, in fallback order after whichever one was asked for.
 *   UI    proportional, Latin + CJK (a Noto Sans SC subset)
 *   MONO  the Terminal's fixed-pitch Latin
 *   TEXT  DejaVu Sans, vendored unmodified: the only one of the three with
 *         Arabic and Hebrew, and the only one with GSUB/GPOS at all -- the two
 *         Noto subsets lost their layout tables to subsetting, so without this
 *         font the shaper has nothing to apply. See third_party/fonts/README.md.
 *   UI_B  the wght=700 instance of the same Noto Sans SC source as UI, subset
 *   MONO_B  to exactly the same codepoints as its regular twin
 *
 * There is no italic entry, and that is the asset's fact rather than this
 * file's choice: neither vendored source has an `ital` or a `slnt` axis, so
 * there is nothing to instance. fsroot/fonts/README.md carries the argument
 * for not shearing the regular outlines instead.
 *
 * DejaVu has no bold twin here either -- it is vendored unmodified as the one
 * font with Arabic and Hebrew, and we do not ship a second copy of it. Bold
 * Arabic therefore falls through to regular DejaVu, which is the same glyphs
 * at the wrong weight rather than no glyphs at all. */
enum { F_UI = 0, F_MONO = 1, F_TEXT = 2, F_UI_B = 3, F_MONO_B = 4, NFONT = 5 };
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
    /* A MISSING BOLD FACE IS NOT AN ERROR HERE, and that is what keeps this
     * change reversible. load_font leaves font_ok[] zero when the file is not
     * there; tl_fonts skips every !font_ok entry; so on an image built before
     * these two files existed -- and in tests/unit/refhost, which maps only
     * /fonts/ui.ttf and /fonts/mono.ttf to host paths -- a bold request
     * degrades to exactly the font set a regular request gets, which is
     * byte-for-byte what this file did before it learned about weight. */
    load_font("/fonts/ui-bold.ttf", F_UI_B);
    load_font("/fonts/mono-bold.ttf", F_MONO_B);
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
 * in the first, which is visible rather than silently missing.
 *
 * TWO ORDERS, AND THE REGULAR ONE IS UNTOUCHED ON PURPOSE. A regular run walks
 * exactly the list it walked before bold existed -- the bold faces are not
 * appended to it -- so every recorded pixel this tree compares against (the
 * WPT reftest baselines, test-desktop-look's sixteen values, the site
 * scoreboard's text runs) is a claim about the same font set it was recorded
 * with. A bold run tries the bold faces first and then falls through the same
 * regular list, so a codepoint the bold subsets do not carry is drawn at the
 * wrong weight rather than as .notdef.
 *
 * THE BOLD LIST IS PER-PITCH, and this is a bug a negative control caught
 * rather than a precaution. There was one bold order,
 * { F_UI_B, F_UI, F_TEXT, F_MONO_B, F_MONO }, for both pitches. Ask for bold
 * MONO on a machine that has ui-bold.ttf but no mono-bold.ttf and the walk
 * skips the missing F_MONO_B, reaches F_UI, and draws the Terminal's <code> in
 * a PROPORTIONAL face -- measured, with the bold files moved aside: the mono
 * bold run came back 324 px wide and byte-identical to the UI regular run
 * instead of mono regular's 399. Losing the weight is a degradation; losing
 * the pitch is a different font. So a bold-mono request falls back through
 * mono first, and a bold-proportional request through the proportional faces
 * first, which is the same rule the regular path has always had.
 *
 * The lists are longer than SHAPE_MAX_FONTS (4) can hold and the loop below
 * stops at that cap, which is why the entries are in preference order and not
 * merely present. */
static void tl_fonts(struct shape_font_set *fs, int prefer, int face, int *map)
{
    static const int order_reg[3]       = { F_UI, F_TEXT, F_MONO };
    static const int order_bold[5]      = { F_UI_B, F_UI, F_TEXT, F_MONO_B, F_MONO };
    static const int order_bold_mono[5] = { F_MONO_B, F_MONO, F_UI_B, F_UI, F_TEXT };
    int bold = (face & LOGIT_FACE_BOLD) != 0;
    const int *order = !bold ? order_reg
                     : (face & LOGIT_FACE_MONO) ? order_bold_mono : order_bold;
    int n = bold ? 5 : 3;
    fs->n = 0;
    if (font_ok[prefer]) { map[fs->n] = prefer; fs->f[fs->n++] = &fonts[prefer]; }
    for (int i = 0; i < n && fs->n < SHAPE_MAX_FONTS; i++)
        if (order[i] != prefer && font_ok[order[i]]) {
            map[fs->n] = order[i];
            fs->f[fs->n++] = &fonts[order[i]];
        }
}

/* THE ONE PLACE A FACE MASK BECOMES A FONT INDEX.
 *
 * `face` is LOGIT_FACE_MONO | LOGIT_FACE_BOLD (include/abi/logit_abi.h), the
 * same two bits SYS_TEXT_MEASURE packs into its third argument and
 * SYS_GUI_TEXT_RUN spells as the `mono` and `bold` fields of struct logit_run.
 * A caller that asks for bold and gets it must MEASURE at the same weight it
 * DRAWS at -- bold advances are wider -- and the way this file guarantees that
 * is the way it already guaranteed px agreement: measuring and drawing are the
 * same function, one argument apart, so there is no second path to disagree
 * with.
 *
 * The fall-back to the regular face when the bold file is absent happens in
 * tl_fonts (via font_ok), NOT here: returning F_UI for a bold request would
 * lose the bold ORDER as well as the bold face, and then a machine with
 * ui-bold.ttf but no mono-bold.ttf would draw bold <code> in regular UI rather
 * than in bold UI.
 *
 * The two bits are LOGIT_FACE_* from the ABI header and are NOT respelled
 * here. This tree has paid three times for a constant spelled in two places
 * (see CLAUDE.md, "One jar, TWO doors"); a private FACE_BOLD 0x2 beside the
 * ABI's LOGIT_FACE_BOLD 0x2 would be the fourth, and it would fail silently --
 * a mismatch draws the wrong weight, not an error. */
static int face_font(int face)
{
    if (face & LOGIT_FACE_BOLD) return (face & LOGIT_FACE_MONO) ? F_MONO_B : F_UI_B;
    return (face & LOGIT_FACE_MONO) ? F_MONO : F_UI;
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
static int layout_locked(int x, int y, const char *s, int len, int face, int px,
                         int cell, uint32_t color, int draw)
{
    if (!font_ok[F_UI] && !font_ok[F_MONO] && !font_ok[F_TEXT]) return x;
    if (len <= 0) return x;

    int prefer = face_font(face);
    struct shape_font_set fs;
    int map[SHAPE_MAX_FONTS];
    tl_fonts(&fs, prefer, face, map);
    if (fs.n == 0) return x;

    struct shape_scratch sc;
    tl_scratch(&sc);

    if (!draw) return shape_line(&fs, s, len, px, cell, x, 0, &sc);

    struct emit_ctx ec = { y + ascent_px(map[0], px), px, color, map };
    struct shape_emit em = { emit_blit, &ec };
    return shape_line(&fs, s, len, px, cell, x, &em, &sc);
}

static int layout(int x, int y, const char *s, int len, int face, int px,
                  int cell, uint32_t color, int draw)
{
    spin_lock(&text_lock);
    int r = layout_locked(x, y, s, len, face, px, cell, color, draw);
    spin_unlock(&text_lock);
    return r;
}

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

int text_draw_sz(int x, int y, const char *utf8, int px, uint32_t color)
{ return layout(x, y, utf8, slen(utf8), 0, px, 0, color, 1); }

int text_draw(int x, int y, const char *utf8, uint32_t color)
{ return layout(x, y, utf8, slen(utf8), 0, TEXT_UI_PX, 0, color, 1); }

int text_draw_mono(int x, int y, const char *utf8, int cell_w, uint32_t color)
{ return layout(x, y, utf8, slen(utf8), LOGIT_FACE_MONO, TEXT_UI_PX, cell_w, color, 1); }

/* Same, at an explicit pixel size. The Terminal picks its cell width in points
 * and the WM scales BOTH the cell and the glyph size, so a 2x display draws
 * genuinely larger glyphs rather than the same glyphs in wider cells. */
int text_draw_mono_sz(int x, int y, const char *utf8, int px, int cell_w, uint32_t color)
{ return layout(x, y, utf8, slen(utf8), LOGIT_FACE_MONO, px, cell_w, color, 1); }

int text_width_sz(const char *utf8, int px)
{ return layout(0, 0, utf8, slen(utf8), 0, px, 0, 0, 0); }

int text_width(const char *utf8)
{ return layout(0, 0, utf8, slen(utf8), 0, TEXT_UI_PX, 0, 0, 0); }

/* Measure a length-delimited UTF-8 run at `px`, in the face `face` selects (for
 * the layout engine's word-wrap). Same function as the draw below, one argument
 * apart -- see the note above.
 *
 * `face` IS THE OLD `mono` PARAMETER WIDENED, not a new one: bit 0 still means
 * exactly what it meant, so the 23 host test files that define their own
 * `int text_measure(const char *, int, int, int)` stub for the browser -- every
 * one of them ignoring this argument and answering len * (px/2) -- keep
 * compiling and keep answering what they answered. Widening beat adding a fifth
 * argument for that reason alone. */
int text_measure(const char *s, int len, int px, int face)
{ return layout(0, 0, s, len, face, px, 0, 0, 0); }

/* Draw a length-delimited UTF-8 run at (x, y=top) in the face `face` selects at
 * `px`, returning the end x. For the layout engine's display list. */
int text_draw_run(int x, int y, const char *s, int len, int px, int face, uint32_t color)
{ return layout(x, y, s, len, face, px, 0, color, 1); }

int text_line_height(int px)
{
    int f = font_ok[F_UI] ? F_UI : F_MONO;
    if (!font_ok[f]) return px + px/4;
    return (int)(((long)(fonts[f].ascent - fonts[f].descent + fonts[f].line_gap) * px) / fonts[f].units_per_em);
}
