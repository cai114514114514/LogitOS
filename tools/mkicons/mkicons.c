/* mkicons.c -- the app icon masters (design_language.md 3.7.4, 3.7.5).
 *
 *   mkicons [--sheet SHEET.png [--sheet-only]] [--only STEM] OUTDIR
 *
 * Writes OUTDIR/<stem>.png, 256 x 256 RGBA, for every app that has a glyph in
 * glyphs_svg.inc (files settings browser clock monitor preview studio assistant
 * terminal textedit gallery widgets) plus OUTDIR/logit.png, the 3.7.5 mark tile.
 * Colours, ink and the glyph key come from c/include/ui/logit_apps.h, so the
 * table the Dock and Spotlight read is also the table the art is drawn from.
 *
 * One tile (3.7.4):
 *   - a squircle of r = 0.225 * size (smoothing 154/256), full-bleed gradient
 *     (2 stops, vertical; the Logit tiles use the 135 degree 3-stop gradient of 3.1.6);
 *   - the glyph at 56 % of the tile, white or #1D1D23 on a light tile;
 *   - a soft bottom shade, #000000 @ 20 over the bottom 6 %;
 *   - a 1 dev px inner rim, #FFFFFF @ 90 at the top fading to @ 0 at 50 %;
 *   - NO drop shadow: the Dock draws the contact shadow.
 * --sheet also writes the contact sheet: every icon at 256, 64 and 32 on light
 * #F4F4F8 and dark #1E1E24.
 *
 * Rasterised by c/lib/gfx (the same coverage engine the OS draws with) and
 * encoded by rust/src/pngenc.rs (the in-tree PNG encoder tests/pngenc.mk
 * checks; stored deflate, so no zlib). Deterministic: same bytes every run.
 * -DUI_ART_NEGCTL drops the rim, which `test-ui-art-assets-negctl` watches
 * fail. Host tool: not part of the OS image.
 *
 * Build: see tests/ui-art-assets.mk. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "ui/logit_apps.h"

/* The Rust staticlib allocates through the host's kmalloc/kfree. */
void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
/* rust/src/{bmp,ico}.rs register their decoders at start-up; this tool decodes nothing. */
void img_register(void *d, void *f) { (void)d; (void)f; }
unsigned char *png_encode_rgba(const unsigned char *px, int w, int h, int *out_len);
void png_encode_free(unsigned char *p);

#define TILE 256
#define GLYPH_PCT 56            /* 3.7.4: glyph at 56 % of the size */

enum { R_INK = 0, R_ACCENT, R_RGB };
struct glyph_layer {
    const char *d;
    unsigned char role, evenodd, grad, fade, alpha, alpha2;
    unsigned rgb, rgb2;
    double stroke, rot, sc, tx, ty, gy0, gy1;
};
#include "svgpath.c"
#include "glyphs_svg.inc"

/* ------------------------------------------------------------ surface */
static unsigned char g_px[TILE * TILE * 4];
static int g_pts[16384 * 2], g_subs[256];
static int g_opts[65536 * 2], g_osubs[1024];

static void path_init(struct gfx_path *p) { gfx_path_init(p, g_pts, 16384, g_subs, 256); }
static void out_init(struct gfx_path *p) { gfx_path_init(p, g_opts, 65536, g_osubs, 1024); }

static void fill_path(struct gfx_surface *s, const struct gfx_path *p, int rule, const struct gfx_paint *paint)
{
    struct gfx_rect clip = { 0, 0, s->w, s->h };
    if (!gfx_fill_subs(s, p, rule, paint, &clip, 8)) {
        fprintf(stderr, "mkicons: gfx_fill refused a path (overflow %d)\n", p->overflow);
        exit(1);
    }
}
static unsigned rgb_of(unsigned c) { return c & 0xFFFFFF; }

/* ------------------------------------------------------------ the tile */
static void tile_paint(struct gfx_paint *p, const struct logit_app *a, int size)
{
    if (a->flags & LOGIT_APP_F_BRAND_GRADIENT) {            /* 3.1.6: 135 deg, 3 stops */
        gfx_paint_linear(p, 0, 0, GFX_PX(size), GFX_PX(size));
        gfx_paint_stop(p, 0, 0x7C5CFF, 255);
        gfx_paint_stop(p, 36045, 0x4F7BFF, 255);             /* 55 % */
        gfx_paint_stop(p, 65536, 0x38A8FF, 255);
    } else {
        gfx_paint_linear(p, 0, 0, 0, GFX_PX(size));
        gfx_paint_stop(p, 0, rgb_of(a->grad_top), 255);
        gfx_paint_stop(p, 65536, rgb_of(a->grad_bottom), 255);
    }
    p->flags |= GFX_PAINT_DITHER;
}

static void squircle(struct gfx_path *p, int inset, int size)
{
    /* r = 0.225 * size; the 1 px inset used by the rim keeps r - inset */
    int r = (int)floor(0.225 * size * 256.0 + 0.5) - inset * 256;
    gfx_path_squircle(p, GFX_PX(inset), GFX_PX(inset), GFX_PX(size - 2 * inset), GFX_PX(size - 2 * inset), r, 154);
}

/* ------------------------------------------------------------ glyph layers */
static unsigned layer_colour(const struct glyph_layer *l, const struct logit_app *a)
{
    if (l->role == R_ACCENT) return a->glyph_accent ? a->glyph_accent : a->glyph_ink;
    if (l->role == R_RGB) return l->rgb;
    return a->glyph_ink;
}

static void draw_layer(struct gfx_surface *s, const struct glyph_layer *l, const struct logit_app *a, double gx, double gy, double gs)
{
    double r = l->rot * 3.14159265358979323846 / 180.0, c = cos(r), sn = sin(r), sc = l->sc ? l->sc : 1.0;
    /* box -> tile: rotate and scale about (50,50), translate, then place the box */
    struct svg_xf xf;
    xf.a = c * sc * gs;  xf.b = sn * sc * gs;
    xf.c = -sn * sc * gs; xf.d = c * sc * gs;
    xf.e = gx + gs * (50.0 + l->tx - sc * (c * 50.0 - sn * 50.0));
    xf.f = gy + gs * (50.0 + l->ty - sc * (sn * 50.0 + c * 50.0));
    struct gfx_path src, out;
    path_init(&src);
    if (svg_path_build(&src, l->d, &xf)) { fprintf(stderr, "mkicons: bad path in a %s layer\n", a->stem); exit(1); }
    unsigned col = layer_colour(l, a);
    int alpha = l->alpha ? l->alpha : 255;
    struct gfx_paint paint;
    if (l->grad) {
        gfx_paint_linear(&paint, XFX(&xf, 50.0, l->gy0), XFY(&xf, 50.0, l->gy0), XFX(&xf, 50.0, l->gy1), XFY(&xf, 50.0, l->gy1));
        unsigned c0 = l->rgb ? l->rgb : col;
        gfx_paint_stop(&paint, 0, rgb_of(c0), alpha);
        gfx_paint_stop(&paint, 65536, rgb_of(l->rgb2 ? l->rgb2 : c0), l->alpha2 ? l->alpha2 : (l->fade ? 0 : alpha));
    } else {
        gfx_paint_solid(&paint, rgb_of(col), alpha);
    }
    if (l->stroke > 0) {
        struct gfx_stroke st;
        memset(&st, 0, sizeof st);
        st.width = svg_fix(l->stroke * sc * gs);
        st.cap = GFX_CAP_ROUND; st.join = GFX_JOIN_ROUND; st.miter_limit = 4 << 16;
        out_init(&out);
        if (!gfx_stroke_path(&out, &src, &st)) { fprintf(stderr, "mkicons: stroke refused in a %s layer\n", a->stem); exit(1); }
        fill_path(s, &out, GFX_NONZERO, &paint);
    } else {
        fill_path(s, &src, l->evenodd ? GFX_EVENODD : GFX_NONZERO, &paint);
    }
}

static const struct glyph_layer *glyph_layers(int id)
{
    for (unsigned i = 0; i < sizeof GLYPH_DEFS / sizeof GLYPH_DEFS[0]; i++)
        if (GLYPH_DEFS[i].id == id) return GLYPH_DEFS[i].layers;
    return NULL;
}

/* Renders one app tile into g_px (straight RGBA). */
static int render_tile(const struct logit_app *a)
{
    const struct glyph_layer *layers = glyph_layers(a->glyph);
    if (!layers) return 0;
    struct gfx_surface s;
    gfx_surface_init(&s, g_px, TILE, TILE, TILE * 4);
    memset(g_px, 0, sizeof g_px);
    struct gfx_path p;
    struct gfx_paint paint;

    path_init(&p);
    squircle(&p, 0, TILE);
    tile_paint(&paint, a, TILE);
    fill_path(&s, &p, GFX_NONZERO, &paint);

    /* soft bottom shade: black, 0 -> 20/255 over the bottom 6 %, kept inside the squircle */
    {
        static unsigned char cov[TILE * TILE];
        memset(cov, 0, sizeof cov);
        gfx_fill_mask_subs(&p, GFX_NONZERO, cov, TILE, TILE, 0, 0, 8);
        struct gfx_clip_mask cm = { cov, TILE, TILE, 0, 0 };
        struct gfx_path band;
        path_init(&band);
        int y0 = TILE - (int)(TILE * 0.06 + 0.5);
        gfx_path_rect(&band, 0, GFX_PX(y0), GFX_PX(TILE), GFX_PX(TILE - y0));
        gfx_paint_linear(&paint, 0, GFX_PX(y0), 0, GFX_PX(TILE));
        gfx_paint_stop(&paint, 0, 0x000000, 0);
        gfx_paint_stop(&paint, 65536, 0x000000, 20);
        struct gfx_rect clip = { 0, 0, TILE, TILE };
        gfx_fill_clipped(&s, &band, GFX_NONZERO, &paint, &clip, 8, &cm);
    }

    /* the glyph, 56 % of the tile, centred */
    double gs = TILE * GLYPH_PCT / 100.0 / 100.0, gx = (TILE - 100.0 * gs) / 2.0, gy = gx;
    for (const struct glyph_layer *l = layers; l->d; l++) draw_layer(&s, l, a, gx, gy, gs);

#ifndef UI_ART_NEGCTL
    /* 1 dev px inner rim: white @ 90 at the top fading to @ 0 at 50 % of the height */
    path_init(&p);
    squircle(&p, 0, TILE);
    squircle(&p, 1, TILE);
    gfx_paint_linear(&paint, 0, 0, 0, GFX_PX(TILE / 2));
    gfx_paint_stop(&paint, 0, 0xFFFFFF, 90);
    gfx_paint_stop(&paint, 65536, 0xFFFFFF, 0);
    fill_path(&s, &p, GFX_EVENODD, &paint);
#endif
    return 1;
}

/* ------------------------------------------------------------ files */
static int write_png(const char *path, const unsigned char *px, int w, int h)
{
    int n = 0;
    unsigned char *png = png_encode_rgba(px, w, h, &n);
    if (!png || n <= 0) { fprintf(stderr, "mkicons: png_encode_rgba failed for %s\n", path); return 0; }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); png_encode_free(png); return 0; }
    int ok = fwrite(png, 1, (size_t)n, f) == (size_t)n;
    ok = fclose(f) == 0 && ok;
    png_encode_free(png);
    if (!ok) fprintf(stderr, "mkicons: short write to %s\n", path);
    return ok;
}

/* ------------------------------------------------------------ contact sheet */
struct sheet { unsigned char *px; int w, h; };

static void over(struct sheet *sh, int x, int y, const unsigned char *src, int sw, int sh_)
{
    for (int j = 0; j < sh_; j++)
        for (int i = 0; i < sw; i++) {
            if (x + i < 0 || y + j < 0 || x + i >= sh->w || y + j >= sh->h) continue;
            const unsigned char *s = src + (j * sw + i) * 4;
            unsigned char *d = sh->px + ((y + j) * sh->w + x + i) * 4;
            unsigned a = s[3];
            for (int k = 0; k < 3; k++) d[k] = (unsigned char)((s[k] * a + d[k] * (255 - a) + 127) / 255);
            d[3] = 255;
        }
}
/* Area average of a TILE x TILE RGBA into (TILE/f)^2, premultiplied, so soft edges stay soft. */
static void box_down(const unsigned char *src, unsigned char *dst, int f)
{
    int n = TILE / f;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            unsigned long r = 0, g = 0, b = 0, a = 0;
            for (int v = 0; v < f; v++)
                for (int u = 0; u < f; u++) {
                    const unsigned char *s = src + ((j * f + v) * TILE + i * f + u) * 4;
                    r += (unsigned long)s[0] * s[3]; g += (unsigned long)s[1] * s[3]; b += (unsigned long)s[2] * s[3]; a += s[3];
                }
            unsigned char *d = dst + (j * n + i) * 4;
            if (a) { d[0] = (unsigned char)((r + a / 2) / a); d[1] = (unsigned char)((g + a / 2) / a); d[2] = (unsigned char)((b + a / 2) / a); }
            else d[0] = d[1] = d[2] = 0;
            d[3] = (unsigned char)((a + (unsigned)(f * f) / 2) / (unsigned)(f * f));
        }
}
static void fillrect(struct sheet *sh, int x0, int y0, int w, int h, unsigned rgb)
{
    for (int j = y0; j < y0 + h; j++)
        for (int i = x0; i < x0 + w; i++) {
            unsigned char *d = sh->px + (j * sh->w + i) * 4;
            d[0] = (rgb >> 16) & 255; d[1] = (rgb >> 8) & 255; d[2] = rgb & 255; d[3] = 255;
        }
}

#define SHEET_COLS 5
#define CELL_W (TILE + 20 + 64 + 28)
#define CELL_H (TILE + 28)
#define PAD 28

static int write_sheet(const char *path, const struct logit_app **apps, unsigned char (*tiles)[TILE * TILE * 4], int n)
{
    int rows = (n + SHEET_COLS - 1) / SHEET_COLS;
    int panel_w = PAD * 2 + SHEET_COLS * CELL_W, panel_h = PAD * 2 + rows * CELL_H;
    struct sheet sh = { 0, panel_w * 2, panel_h };
    sh.px = calloc((size_t)sh.w * sh.h, 4);
    if (!sh.px) return 0;
    static unsigned char s64[64 * 64 * 4], s32[32 * 32 * 4];
    (void)apps;
    for (int panel = 0; panel < 2; panel++) {
        fillrect(&sh, panel * panel_w, 0, panel_w, panel_h, panel ? 0x1E1E24 : 0xF4F4F8);
        for (int i = 0; i < n; i++) {
            int cx = panel * panel_w + PAD + (i % SHEET_COLS) * CELL_W, cy = PAD + (i / SHEET_COLS) * CELL_H;
            over(&sh, cx, cy, tiles[i], TILE, TILE);
            box_down(tiles[i], s64, 4);
            over(&sh, cx + TILE + 20, cy, s64, 64, 64);
            box_down(tiles[i], s32, 8);
            over(&sh, cx + TILE + 20, cy + 64 + 20, s32, 32, 32);
        }
    }
    int ok = write_png(path, sh.px, sh.w, sh.h);
    free(sh.px);
    return ok;
}

/* ------------------------------------------------------------ main */
int main(int argc, char **argv)
{
    const char *outdir = NULL, *sheet = NULL, *only = NULL;
    int sheet_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sheet") && i + 1 < argc) sheet = argv[++i];
        else if (!strcmp(argv[i], "--sheet-only")) sheet_only = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (argv[i][0] != '-' && !outdir) outdir = argv[i];
        else { fprintf(stderr, "usage: mkicons [--sheet SHEET.png [--sheet-only]] [--only STEM] OUTDIR\n"); return 2; }
    }
    if (!outdir) { fprintf(stderr, "usage: mkicons [--sheet SHEET.png [--sheet-only]] [--only STEM] OUTDIR\n"); return 2; }

    static unsigned char tiles[16][TILE * TILE * 4];
    const struct logit_app *apps[16];
    int n = 0;
    /* the 12 app tiles in table order, then the Logit mark tile (3.7.5) */
    for (int i = 0; i < LOGIT_APP_COUNT && n < 16; i++) {
        const struct logit_app *a = &LOGIT_APPS[i];
        if (!strcmp(a->stem, "ch") || !strcmp(a->stem, "greeter") || !strcmp(a->stem, "spotlight")) continue;
        if (only && strcmp(only, a->stem)) continue;
        if (!render_tile(a)) { fprintf(stderr, "mkicons: no glyph for %s\n", a->stem); return 1; }
        char path[512];
        snprintf(path, sizeof path, "%s/%s.png", outdir, a->stem);
        if (!sheet_only && !write_png(path, g_px, TILE, TILE)) return 1;
        memcpy(tiles[n], g_px, sizeof g_px); apps[n++] = a;
    }
    const struct logit_app *mark = logit_app_lookup("greeter");
    if (mark && (!only || !strcmp(only, "logit"))) {
        if (!render_tile(mark)) return 1;
        char path[512];
        snprintf(path, sizeof path, "%s/logit.png", outdir);
        if (!sheet_only && !write_png(path, g_px, TILE, TILE)) return 1;
        memcpy(tiles[n], g_px, sizeof g_px); apps[n++] = mark;
    }
    if (sheet && !write_sheet(sheet, apps, tiles, n)) return 1;
    printf("mkicons: %s %d icons%s%s\n", sheet_only ? "drew" : "wrote", n, sheet_only ? "" : " to ", sheet_only ? "" : outdir);
    return 0;
}
