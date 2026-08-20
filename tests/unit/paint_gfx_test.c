/* Host gate for the PAINT declarations reaching the SCREEN.
 *
 * WHAT THIS IS FOR, and why it is not tests/unit/paint_test.c.
 *
 * paint_test asserts on the draw ops of the shipped pipeline, and its link
 * line carries neither css_extra.c nor css_interp.c -- which is exactly right
 * for what it measures and makes it structurally unable to see `transform`,
 * `box-shadow` or a gradient, because those three have no producer without
 * css_extra's raw-declaration scan. tests/cssdecl.mk closes the other half:
 * it proves the declaration becomes the right VALUES. Between the two there
 * was nothing, and "the value is right" and "the pixels are right" are the two
 * failures this milestone can actually have -- CLAUDE.md's WPT note is the
 * same shape one subsystem over: *LINKING A TRANSLATION UNIT IS NOT RUNNING
 * IT.* A painter that links css_extra.c and never calls css_shadow_parse
 * scores identically to one that does.
 *
 * So this binary links the whole road -- parse, cascade, css_extra capture,
 * layout, the real browser_paint.c, the real c/lib/gfx -- and asserts on the
 * ops that come out the far end. Every assertion below names a pixel or a
 * count; none of them says "it looks right".
 *
 * THE ORACLE PROBLEM, stated plainly. 2D coverage has an independent oracle
 * and tests/unit/gfx_raster_test.c is it, at 16x16 supersampling against each
 * shape's own analytic predicate. That is the engine's bar and it is already
 * met. What this file measures is one level up and is a DIFFERENT question:
 * given a declaration, does the painter ask the engine for the right shape, in
 * the right place, with the right colour, in the right order? That is answered
 * by geometry and arithmetic that can be computed by hand in the test, which
 * is what every CHECK here does -- and the numbers are computed from the CSS,
 * never copied from a run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "logit.h"                 /* the recorder, via -Itests/unit/painthost */
#include "layout.h"
#include "css.h"
#include "dom.h"
#include "browser_paint.h"

struct paintop paint_ops[PAINT_MAXOPS];
int paint_nops;

/* --- stubs (the same set paint_test uses) --- */
void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len * (px/2); }
int res_fetch(const char *url, uint8_t **buf, int *len){ (void)url;(void)buf;(void)len; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *out){ (void)p;(void)n;(void)out; return -1; }
/* svg.c is linked for ONE function, img_css_color -- this tree's single CSS
 * colour evaluator. Its svg_register() registers the SVG decoder with img.c's
 * table, and pulling in img.c to satisfy that would drag every codec in for a
 * registration nothing here ever calls. Stubbed, not omitted: leaving it
 * undefined is a link error, and defining it as a real registry would be
 * building a decoder table for a test that decodes nothing. */
void img_register(img_detect_fn detect, img_decode_fn decode){ (void)detect; (void)decode; }

static int fail;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fail=1; } else printf("ok: %s\n", msg); }while(0)

/* The viewport. 600x600 at scroll 0, so window coordinates ARE document
 * coordinates and every expectation below can be read straight off the CSS. */
#define VW 600
#define VH 600

static void render(const char *html, const char *css)
{
    struct node *root = dom_parse(html, (int)strlen(html));
    css_apply(root, css, css ? (int)strlen(css) : 0);
    /* css_extra_apply is what puts the four XR_* spans on the style. Without
     * this line every transform/shadow/gradient row below would be measuring
     * an absent capture rather than the painter, and would pass or fail for
     * the wrong reason. */
    css_extra_apply(root, css, css ? (int)strlen(css) : 0);
    layout_page(root, VW);
    paint_nops = 0;
    browser_paint(0, 0, VW, VH, 0);
}

/* ---- op queries ---------------------------------------------------------- */

/* Any RECT or 1x1-solid BLIT of exactly this colour covering (x,y). */
static const struct paintop *solid_at(int x, int y, unsigned color)
{
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind == OP_RECT) { if (o->color != color) continue; }
        else if (o->kind == OP_BLIT && o->solid) { if (o->color != color) continue; }
        else continue;
        if (x >= o->x && x < o->x + o->w && y >= o->y && y < o->y + o->h) return o;
    }
    return 0;
}

/* Any op at all -- rect, solid blit or tile -- whose body covers (x,y). */
static int any_cover(int x, int y)
{
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind != OP_RECT && o->kind != OP_BLIT && o->kind != OP_RRECT) continue;
        if (x >= o->x && x < o->x + o->w && y >= o->y && y < o->y + o->h) return 1;
    }
    return 0;
}

/* The first multi-pixel BLIT that is a 1-D RAMP: 1 x n (replicated across x)
 * or n x 1 (replicated down y). That is the gradient/shadow-strip shape. */
static const struct paintop *ramp_op(int want_vertical)
{
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind != OP_BLIT || o->solid) continue;
        if (want_vertical && o->sw == 1 && o->sh > 1) return o;
        if (!want_vertical && o->sh == 1 && o->sw > 1) return o;
    }
    return 0;
}

static int ramp_count(void)
{
    int c = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind != OP_BLIT || o->solid) continue;
        if ((o->sw == 1 && o->sh > 1) || (o->sh == 1 && o->sw > 1)) c++;
    }
    return c;
}

/* A RECT of this colour whose top-left is exactly (x,y) and whose size is
 * exactly (w,h). Geometry, not coverage: this is what separates "the top edge
 * spans the whole box" (the square border path) from "the top edge spans the
 * box minus two corner radii" (the rounded one). */
static const struct paintop *rect_exact(int x, int y, int w, int h, unsigned color)
{
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind != OP_RECT || o->color != color) continue;
        if (o->x == x && o->y == y && o->w == w && o->h == h) return o;
    }
    return 0;
}

/* Corner tiles: a BLIT whose source is a square-ish TILE (both dimensions > 1)
 * rather than a ramp or a single pixel. */
static int tile_count(void)
{
    int c = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind == OP_BLIT && !o->solid && o->sw > 1 && o->sh > 1) c++;
    }
    return c;
}

static const struct paintop *text_op(const char *s)
{
    int n = (int)strlen(s);
    for (int i = 0; i < paint_nops; i++)
        if (paint_ops[i].kind == OP_TEXT && paint_ops[i].len == n &&
            !memcmp(paint_ops[i].text, s, (size_t)n)) return &paint_ops[i];
    return 0;
}

static int near(int a, int b, int tol) { int d = a - b; if (d < 0) d = -d; return d <= tol; }

/* ==========================================================================
 * 1. box-shadow
 * ========================================================================== */

/* A shadow ring on a TRANSPARENT box. CSS: an outer shadow is clipped inside
 * the border box, so `0 0 0 4px red` on a box with no background is a 4px ring
 * OUTSIDE it and NOTHING over the box itself. This is the case a naive
 * implementation gets wrong invisibly -- over an opaque card the difference
 * cannot be seen at all, and on a transparent input it is the whole feature.
 * PAINT_NEGCTL_SHADOW_NO_CLIP is exactly that implementation. */
static void t_shadow_ring(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:100px;top:100px;"
           "width:200px;height:100px;box-shadow:0 0 0 4px #ff0000}");
    /* Just outside the left edge: inside the ring. */
    CHECK(solid_at(97, 150, 0xFF0000) != 0,
          "shadow ring: `0 0 0 4px` paints 4px outside the left edge");
    CHECK(solid_at(150, 97, 0xFF0000) != 0,
          "shadow ring: ... and 4px above the top edge");
    /* 5px out is past the spread. */
    CHECK(solid_at(95, 150, 0xFF0000) == 0,
          "shadow ring: nothing 5px out -- the spread is 4, not more");
    /* The interior must be untouched: this is the clip. */
    CHECK(solid_at(200, 150, 0xFF0000) == 0,
          "shadow ring: NOTHING over a transparent caster's own box (CSS clips "
          "the outer shadow inside the border edge)");
}

/* The same declaration on an OPAQUE box. The clip is then unobservable -- the
 * background covers it -- and the ring must still be outside, in the right
 * colour, with the background on top. */
static void t_shadow_over_bg(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:100px;top:100px;"
           "width:200px;height:100px;background:#ffffff;box-shadow:0 0 0 4px #ff0000}");
    CHECK(solid_at(97, 150, 0xFF0000) != 0,
          "shadow over a background: the ring is still painted outside");
    /* The background is painted AFTER the shadow, so it is the last op
     * covering the centre. Find the last covering op and require it white. */
    const struct paintop *last = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind != OP_RECT && !(o->kind == OP_BLIT && o->solid)) continue;
        if (200 >= o->x && 200 < o->x + o->w && 150 >= o->y && 150 < o->y + o->h)
            last = o;
    }
    CHECK(last && last->color == 0xFFFFFF,
          "shadow order: the background is painted OVER the shadow, not under it");
}

/* An offset shadow with no blur, on a transparent box: the offset part shows
 * and the caster's own footprint does not. */
static void t_shadow_offset(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:100px;top:100px;"
           "width:200px;height:100px;box-shadow:10px 10px 0 #0000ff}");
    CHECK(solid_at(305, 205, 0x0000FF) != 0,
          "offset shadow: the 10,10 offset shape reaches past the bottom-right");
    CHECK(solid_at(200, 150, 0x0000FF) == 0,
          "offset shadow: still nothing over the transparent caster");
    CHECK(solid_at(105, 105, 0x0000FF) == 0,
          "offset shadow: and nothing up-left of it -- the shape MOVED, it did "
          "not grow");
}

/* A blurred shadow. The falloff arrives as 1-D ramp blits (the four edges)
 * plus tiles (the four corners); the assertion is the RAMP's shape, which is
 * what says the blur is a blur and not a hard edge. */
static void t_shadow_blur(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:100px;top:100px;"
           "width:200px;height:100px;background:#ffffff;box-shadow:0 8px 16px #000000}");
    const struct paintop *v = ramp_op(1);
    CHECK(v != 0, "blurred shadow: the vertical edges arrive as a 1 x n ramp");
    if (v) {
        int a0 = v->samp[0][3], aN = v->samp[v->nsamp - 1][3];
        /* One end touches the caster (opaque-ish) and the other is the outer
         * limit of the blur (transparent). Which end is which depends on the
         * edge; the claim is that they DIFFER by most of the range, i.e. the
         * strip is a falloff and not a flat band. */
        int lo = a0 < aN ? a0 : aN, hi = a0 < aN ? aN : a0;
        CHECK(lo <= 8 && hi >= 200,
              "blurred shadow: the ramp runs from ~0 to ~255 -- a falloff, not "
              "a flat band");
        CHECK(tile_count() >= 4,
              "blurred shadow: four blurred CORNER tiles, so the halo has no gap");
    }
}

/* A blurred INSET shadow is refused, and the refusal is COUNTED. Not painting
 * it is a decision; not being able to say how often is the bug. */
static void t_shadow_inset(void)
{
    int b0, t0, b1, t1;
    browser_paint_shadow_stats(&b0, &t0);
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:100px;top:100px;"
           "width:200px;height:100px;box-shadow:inset 0 0 12px #000000}");
    browser_paint_shadow_stats(&b1, &t1);
    CHECK(b1 == b0 + 1,
          "inset+blur: refused and COUNTED exactly once (browser_paint_shadow_stats)");

    /* The hard inset ring IS painted, and exactly: `inset 0 0 0 3px` is a 3px
     * band just inside the border edge and nothing in the middle. */
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:100px;top:100px;"
           "width:200px;height:100px;box-shadow:inset 0 0 0 3px #00aa00}");
    CHECK(solid_at(101, 150, 0x00AA00) != 0,
          "inset ring: `inset 0 0 0 3px` paints just INSIDE the left edge");
    CHECK(solid_at(105, 150, 0x00AA00) == 0,
          "inset ring: and stops 3px in -- it is a ring, not a fill");
    CHECK(solid_at(97, 150, 0x00AA00) == 0,
          "inset ring: nothing outside the box (that would be the OUTER shape)");
}

/* Two shadows: CSS paints the FIRST-listed on top, so the second must be
 * emitted first. Both are outer, so both are under the background. */
static void t_shadow_order(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:100px;top:100px;"
           "width:200px;height:100px;box-shadow:0 0 0 4px #ff0000,0 0 0 10px #0000ff}");
    int red = -1, blue = -1;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        unsigned c = (o->kind == OP_RECT || (o->kind == OP_BLIT && o->solid)) ? o->color : 0xDEAD;
        if (c == 0xFF0000 && red < 0) red = i;
        if (c == 0x0000FF && blue < 0) blue = i;
    }
    CHECK(blue >= 0 && red >= 0 && blue < red,
          "two shadows: the SECOND listed is painted first, so the first ends "
          "up on top (CSS paint order)");
    CHECK(solid_at(93, 150, 0x0000FF) != 0,
          "two shadows: the 10px spread reaches 10px out and is the blue one");
}

/* ==========================================================================
 * 2. linear-gradient
 * ========================================================================== */

static void t_grad_vertical(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:0;top:0;width:200px;height:100px;"
           "background:linear-gradient(#ff0000,#0000ff)}");
    const struct paintop *v = ramp_op(1);
    CHECK(v != 0, "gradient: no direction means `to bottom` -- a 1 x n ramp");
    if (v) {
        CHECK(v->samp[0][0] > 240 && v->samp[0][2] < 16,
              "gradient: the first sample is the first stop (#ff0000)");
        CHECK(v->samp[v->nsamp-1][2] > 240 && v->samp[v->nsamp-1][0] < 16,
              "gradient: the last sample is the last stop (#0000ff)");
        CHECK(v->w == 200 && v->h == 100,
              "gradient: the ramp covers the whole border box");
    }
}

static void t_grad_horizontal(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:0;top:0;width:200px;height:100px;"
           "background:linear-gradient(to right,#ff0000,#0000ff)}");
    const struct paintop *h = ramp_op(0);
    CHECK(h != 0, "gradient: `to right` is an n x 1 ramp, replicated down y");
    if (h) CHECK(h->samp[0][0] > 240 && h->samp[h->nsamp-1][2] > 240,
                 "gradient: `to right` starts red on the LEFT");
}

/* 90deg IS `to right` -- CSS angles are clockwise from `to top`. Getting the
 * convention backwards produces a gradient that looks fine and runs the wrong
 * way, which no "does it paint" check can see. */
static void t_grad_angle(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:0;top:0;width:200px;height:100px;"
           "background:linear-gradient(90deg,#ff0000,#0000ff)}");
    const struct paintop *h = ramp_op(0);
    CHECK(h != 0, "gradient: 90deg is axis-aligned (an n x 1 ramp), not diagonal");
    if (h) CHECK(h->samp[0][0] > 240 && h->samp[h->nsamp-1][2] > 240,
                 "gradient: 90deg runs LEFT to RIGHT (clockwise from `to top`)");

    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:0;top:0;width:200px;height:100px;"
           "background:linear-gradient(270deg,#ff0000,#0000ff)}");
    h = ramp_op(0);
    CHECK(h && h->samp[0][2] > 240 && h->samp[h->nsamp-1][0] > 240,
          "gradient: 270deg runs RIGHT to LEFT -- the other way round");
}

/* THE PERCENTAGE. A stop at 25% must land a quarter of the way along, and the
 * ramp's samples are evenly spaced, so sample 4 of 16 is at 4/15 = 26.7% and
 * must already be past the step. This is the shape of the units bug CLAUDE.md
 * records (`padding-top:56.25%` stored as 56 pixels): nothing is zero, nothing
 * overflows, the gradient still has both its colours and only the POSITION
 * moves. */
static void t_grad_stop_pos(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:0;top:0;width:200px;height:160px;"
           "background:linear-gradient(#000000 0%,#000000 25%,#ffffff 25%,#ffffff 100%)}");
    const struct paintop *v = ramp_op(1);
    CHECK(v != 0, "stop position: the hard-step gradient paints");
    if (v) {
        /* samp[k] is at k/(nsamp-1) of the ramp. The step is at 25%: samples
         * 0..3 (0, 6.7, 13.3, 20%) are black, 5..15 (33%+) are white. Sample 4
         * (26.7%) is the first past the step and is not asserted, because it
         * sits inside one device row of it. */
        CHECK(v->samp[0][0] < 16 && v->samp[3][0] < 16,
              "stop position: below 25% is the first colour");
        CHECK(v->samp[5][0] > 240 && v->samp[15][0] > 240,
              "stop position: above 25% is the second -- so 25% is a QUARTER, "
              "not 25 pixels");
    }
}

/* A refused value must fall back to the background COLOUR, not to something
 * approximate and not to nothing. */
static void t_grad_refused(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:0;top:0;width:200px;height:100px;"
           "background:#123456;background-image:radial-gradient(#ff0000,#0000ff)}");
    CHECK(ramp_count() == 0,
          "refusal: radial-gradient paints NO ramp (the engine has the paint, "
          "the parser does not produce it yet)");
    CHECK(solid_at(100, 50, 0x123456) != 0,
          "refusal: ... and the background COLOUR is painted exactly as before");
}

/* A diagonal gradient cannot be a 1-D ramp and goes through a surface: one
 * blit whose source is a TILE, covering the box. */
static void t_grad_diagonal(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:0;top:0;width:120px;height:80px;"
           "background:linear-gradient(to bottom right,#ff0000,#0000ff)}");
    CHECK(ramp_count() == 0,
          "diagonal: `to bottom right` is NOT painted as a 1-D ramp");
    const struct paintop *t = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind == OP_BLIT && !o->solid && o->sw > 1 && o->sh > 1 &&
            o->w == 120 && o->h == 80) t = o;
    }
    CHECK(t != 0, "diagonal: it is a surface blit covering the whole box");
    if (t) {
        /* samp[] walks the source in row-major order, so sample 0 is the
         * top-left pixel and the last is the bottom-right -- which for
         * `to bottom right` are the two ENDS of the gradient line. */
        CHECK(t->samp[0][0] > 200 && t->samp[t->nsamp-1][2] > 200,
              "diagonal: red at the top-left corner, blue at the bottom-right");
    }
}

/* ==========================================================================
 * 3. border-radius with no background  (the has_bg guard)
 * ========================================================================== */

/* The bug: `border:2px solid; border-radius:12px` with no background fell
 * through to the SQUARE border path. The discriminator is geometry, not
 * colour: the square path's top edge spans the whole box (w = 200); the
 * rounded one spans the box minus two radii (w = 200 - 24 = 176) and adds four
 * corner tiles. */
static void t_round_border_nobg(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{box-sizing:border-box;position:absolute;left:100px;top:100px;"
           "width:200px;height:100px;border:2px solid #ff0000;border-radius:12px}");
    CHECK(rect_exact(112, 100, 176, 2, 0xFF0000) != 0,
          "rounded border, no background: the top edge spans w-2r (176), which "
          "is the rounded path");
    CHECK(rect_exact(100, 100, 200, 2, 0xFF0000) == 0,
          "rounded border, no background: it does NOT span the full width, "
          "which is the square path this used to take");
    CHECK(tile_count() >= 4,
          "rounded border, no background: four ring corner tiles are blitted");
    /* And the middle is still empty -- the fix must not fill the box. */
    CHECK(!any_cover(200, 150) || solid_at(200, 150, 0xFF0000) == 0,
          "rounded border, no background: the interior is not filled with the "
          "border colour");

    /* A NON-uniform border keeps the square path on purpose: the rounded path
     * collapses four edges into one ring and would invent three of them. */
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{box-sizing:border-box;position:absolute;left:100px;top:100px;"
           "width:200px;height:100px;border-bottom:2px solid #ff0000;border-radius:12px}");
    CHECK(rect_exact(100, 198, 200, 2, 0xFF0000) != 0,
          "non-uniform rounded border: still the square path -- one bottom edge, "
          "full width, and no ring round the other three sides");
}

/* The case that must NOT change: a rounded box WITH a background still goes
 * down the fill-then-inset path it always did. */
static void t_round_border_bg(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:100px;top:100px;"
           "width:200px;height:100px;background:#ffffff;"
           "border:2px solid #ff0000;border-radius:12px}");
    CHECK(solid_at(200, 150, 0xFFFFFF) != 0,
          "rounded box with a background: the fill is still painted");
    CHECK(solid_at(101, 150, 0xFF0000) != 0,
          "rounded box with a background: the border ring is still painted");
}

/* ==========================================================================
 * 4. transform
 * ========================================================================== */

/* translate(-50%,-50%) is the centring idiom, and the percentage is of the
 * element's OWN border box -- so the SAME declaration on two different boxes
 * must move them by different amounts. One row alone cannot tell a percentage
 * from a pixel count; the pair can. */
static void t_xf_translate_pct(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:300px;top:300px;"
           "width:200px;height:100px;background:#ff0000;"
           "transform:translate(-50%,-50%)}");
    CHECK(solid_at(205, 255, 0xFF0000) != 0 && solid_at(195, 255, 0xFF0000) == 0,
          "translate(-50%,-50%) on 200x100: the box's left edge lands at 200");
    CHECK(solid_at(300, 251, 0xFF0000) != 0 && solid_at(300, 249, 0xFF0000) == 0,
          "translate(-50%,-50%) on 200x100: ... and its top edge at 250");

    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:300px;top:300px;"
           "width:400px;height:300px;background:#ff0000;"
           "transform:translate(-50%,-50%)}");
    CHECK(solid_at(105, 155, 0xFF0000) != 0 && solid_at(95, 155, 0xFF0000) == 0,
          "translate(-50%,-50%) on 400x300: the SAME declaration moves it "
          "twice as far -- a percentage, not a stored pixel count");
}

/* A transform applies to the whole SUBTREE, and the display list is flat. */
static void t_xf_subtree(void)
{
    render("<body><div id=a><span>HELLO</span></div></body>",
           "body{margin:0} #a{position:absolute;left:0;top:0;width:200px;height:50px;"
           "transform:translate(120px,60px)}");
    const struct paintop *t = text_op("HELLO");
    CHECK(t != 0, "transformed subtree: the text is still painted");
    if (t) CHECK(t->x >= 120 && t->y >= 60,
                 "transformed subtree: a DESCENDANT's text moves with its "
                 "transformed ancestor");
}

/* scale() changes the box AND the type size. Getting only the box is the
 * plausible half-implementation: the block moves and grows and the words
 * inside stay the size they were. */
static void t_xf_scale(void)
{
    render("<body><div id=a><span>HELLO</span></div></body>",
           "body{margin:0} #a{position:absolute;left:0;top:0;width:100px;height:40px;"
           "background:#ff0000;font-size:20px;transform:scale(2);"
           "transform-origin:0 0}");
    CHECK(solid_at(190, 70, 0xFF0000) != 0,
          "scale(2) about 0,0: the 100x40 box now reaches 200x80");
    CHECK(solid_at(210, 70, 0xFF0000) == 0,
          "scale(2) about 0,0: ... and stops there");
    const struct paintop *t = text_op("HELLO");
    CHECK(t && t->px == 40,
          "scale(2): the FONT SIZE doubles too -- 20px type in a scaled block "
          "is 40px type");

    /* The SAME scale with the DEFAULT origin (50% 50%). A 100x40 box at
     * (200,200) grows about its own centre (250,220), so it spans x 150..350
     * and y 180..260 -- LEFT of where it started, which is the whole
     * observable difference between the two origins and what
     * PAINT_NEGCTL_XF_NO_ORIGIN cannot produce. */
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:200px;top:200px;width:100px;"
           "height:40px;background:#ff0000;transform:scale(2)}");
    CHECK(solid_at(160, 220, 0xFF0000) != 0,
          "scale(2) about the default centre: the box grows LEFT of its origin");
    CHECK(solid_at(340, 220, 0xFF0000) != 0,
          "scale(2) about the default centre: ... and right of it, symmetrically");
    CHECK(solid_at(390, 220, 0xFF0000) == 0,
          "scale(2) about the default centre: ... stopping at 350, not at 400 "
          "(which is where a top-left origin would put it)");
}

/* transform-origin. rotate(90deg) about the default centre and about 0 0 put
 * the box in visibly different places, and PAINT_NEGCTL_XF_NO_ORIGIN is the
 * implementation that cannot tell them apart. */
static void t_xf_origin(void)
{
    /* 200x100 at (200,200). Rotated 90deg about its own centre (300,250) the
     * bounding box becomes 100x200 centred there: x 250..350, y 150..350. */
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:200px;top:200px;"
           "width:200px;height:100px;background:#ff0000;transform:rotate(90deg)}");
    CHECK(solid_at(300, 160, 0xFF0000) != 0 || any_cover(300, 160),
          "rotate(90deg) about the centre: the box now reaches y=160");
    CHECK(!any_cover(210, 250),
          "rotate(90deg) about the centre: ... and no longer reaches x=210");

    /* About 0 0 the same rotation swings the box down-left of its origin:
     * (200,200)+(x,y) -> (200-y, 200+x), so it spans x 100..200, y 200..400. */
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:200px;top:200px;"
           "width:200px;height:100px;background:#ff0000;transform:rotate(90deg);"
           "transform-origin:0 0}");
    CHECK(any_cover(150, 300),
          "rotate(90deg) about 0 0: the box swings to the LEFT of its origin");
    CHECK(!any_cover(300, 160),
          "rotate(90deg) about 0 0: ... and not up and to the right, which is "
          "where the centre origin put it");
}

/* A rotated rect is a rasterized PATH, not an axis-aligned fill: the giveaway
 * is a tile blit whose bounding box is the rotated one (w*sqrt2 for 45deg). */
static void t_xf_rotate_path(void)
{
    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:200px;top:200px;"
           "width:60px;height:60px;background:#ff0000;transform:rotate(45deg)}");
    const struct paintop *t = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind == OP_BLIT && !o->solid && o->sw > 1 && o->sh > 1) t = o;
    }
    CHECK(t != 0, "rotate(45deg): the box is a rasterized path, one tile blit");
    if (t) CHECK(near(t->w, 85, 2) && near(t->h, 85, 2),
                 "rotate(45deg): its bounding box is 60*sqrt(2) = 85 a side");
    /* The corners of the bounding box must be EMPTY -- that is what says a
     * diamond was drawn and not a square. */
    if (t) {
        int a = 255;
        if (t->nsamp > 0) a = t->samp[0][3];
        CHECK(a == 0,
              "rotate(45deg): the top-left of the bounding box has zero "
              "coverage -- a diamond, not a square");
    }
}

/* Nested transforms compose. */
static void t_xf_nested(void)
{
    render("<body><div id=a><div id=b></div></div></body>",
           "body{margin:0} #a{position:absolute;left:0;top:0;width:400px;height:400px;"
           "transform:translate(50px,0)} "
           "#b{position:absolute;left:0;top:0;width:60px;height:60px;"
           "background:#ff0000;transform:translate(0,70px)}");
    CHECK(solid_at(80, 100, 0xFF0000) != 0,
          "nested transforms compose: 50 across from the parent and 70 down "
          "from the child");
    CHECK(solid_at(20, 100, 0xFF0000) == 0,
          "nested transforms compose: the parent's translate is not lost");
}

/* ==========================================================================
 * 5. the ROUNDED overflow clip -- path clipping's first consumer
 * ========================================================================== */

/* `overflow:hidden` + `border-radius` on the ancestor. A child background
 * filling the card must follow the card's ARC, not its bounding rectangle.
 *
 * The discriminator is the corner PIXEL: at (2,2) inside a 24px-radius corner
 * the arc has not reached yet, so nothing of the child may be painted there;
 * at (20,20) it has, so the child must be. A rectangular clip paints both. */
static void t_rclip_rect(void)
{
    int a0, r0, a1, r1;
    browser_paint_rclip_stats(&a0, &r0);
    render("<body><div id=c><div id=k></div></div></body>",
           "body{margin:0} #c{box-sizing:border-box;position:absolute;left:0;top:0;"
           "width:300px;height:200px;border-radius:24px;overflow:hidden} "
           "#k{position:absolute;left:0;top:0;width:300px;height:200px;"
           "background:#ff0000}");
    browser_paint_rclip_stats(&a1, &r1);
    CHECK(a1 > a0 && r1 == r0,
          "rounded clip: the path clip was APPLIED, not refused "
          "(browser_paint_rclip_stats)");
    /* The corner blit is a TILE whose alpha is the arc's coverage. Find the
     * one at the clip's top-left and read its first sample: the tile's (0,0)
     * pixel is outside the arc, so it must be fully transparent. */
    const struct paintop *t = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind == OP_BLIT && !o->solid && o->sw > 1 && o->sh > 1 &&
            o->x == 0 && o->y == 0) t = o;
    }
    CHECK(t != 0, "rounded clip: a corner TILE is blitted at the clip's corner");
    if (t) {
        CHECK(t->samp[0][3] == 0,
              "rounded clip: the tile's outermost pixel has ZERO alpha -- the "
              "child does not reach outside the arc");
        CHECK(t->samp[t->nsamp - 1][3] == 255 &&
              t->samp[t->nsamp - 1][0] == 0xFF,
              "rounded clip: its innermost pixel is the child's colour at full "
              "alpha -- the clip multiplies coverage, it does not erase");
    }
    /* And no plain RECT of the child's colour may start at the clip's corner:
     * that is precisely the rectangular clip this replaces. */
    CHECK(solid_at(2, 2, 0xFF0000) == 0,
          "rounded clip: nothing solid at (2,2), which a rectangular clip "
          "would have painted");
    CHECK(solid_at(150, 100, 0xFF0000) != 0,
          "rounded clip: the middle of the card is still painted, in one band");
}

/* A clipper with a BORDER. CSS clips overflow to the PADDING box, and the
 * padding edge's curvature is the border-radius REDUCED BY THE BORDER WIDTH --
 * so a 24px radius inside a 4px border leaves a 20px arc, starting at (4,4)
 * and not at (0,0). Using the border-box radius there cuts a visible bite out
 * of the content along the whole arc, and it is the exact mistake the naive
 * reading makes. */
static void t_rclip_border(void)
{
    render("<body><div id=c><div id=k></div></div></body>",
           "body{margin:0} #c{box-sizing:border-box;position:absolute;left:0;top:0;"
           "width:300px;height:200px;border:4px solid #0000ff;border-radius:24px;"
           "overflow:hidden} "
           "#k{position:absolute;left:0;top:0;width:300px;height:200px;"
           "background:#ff0000}");
    /* The clipper ITSELF now draws a rounded outline (it has a border and no
     * background), so the viewport holds ring tiles at (0,0) size 24 as well.
     * The assertion is therefore for a tile at an EXACT position and size, not
     * for "the top-left tile" -- which would find the clipper's own corner and
     * pass or fail for a reason that has nothing to do with the clip. */
    const struct paintop *pad = 0, *bord = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind != OP_BLIT || o->solid || o->sw <= 1 || o->sh <= 1) continue;
        if (o->x == 4 && o->y == 4 && o->w == 20 && o->h == 20) pad = o;
        if (o->x == 0 && o->y == 0 && o->w == 24 && o->h == 24) bord = o;
    }
    CHECK(bord != 0,
          "clip with a border: the clipper's own 24px outline corner is drawn");
    CHECK(pad != 0,
          "clip with a border: the CHILD's arc is a 20x20 tile at (4,4) -- the "
          "padding edge, with the curvature the 4px border leaves (24 - 4), "
          "and not the border box's 24 at (0,0)");
}

/* An UNROUNDED scroller must cost nothing: the whole mechanism is skipped. */
static void t_rclip_square(void)
{
    int a0, r0, a1, r1;
    browser_paint_rclip_stats(&a0, &r0);
    render("<body><div id=c><div id=k></div></div></body>",
           "body{margin:0} #c{position:absolute;left:0;top:0;"
           "width:300px;height:200px;overflow:hidden} "
           "#k{position:absolute;left:0;top:0;width:300px;height:200px;"
           "background:#ff0000}");
    browser_paint_rclip_stats(&a1, &r1);
    CHECK(a1 == a0 && r1 == r0,
          "square clip: an overflow:hidden box with no radius does not touch "
          "the path clip at all");
    CHECK(solid_at(2, 2, 0xFF0000) != 0,
          "square clip: ... and its child still paints into the corner");
}

/* ==========================================================================
 * 6. the shipped path still paints what it painted
 * ========================================================================== */

/* Guard rows. Everything above adds a branch to the busiest loop in the
 * browser; these are the ordinary boxes that must come out unchanged. */
static void t_unchanged(void)
{
    render("<body><div id=a>text</div></body>",
           "body{margin:0} #a{position:absolute;left:10px;top:10px;width:100px;"
           "height:40px;background:#336699}");
    CHECK(rect_exact(10, 10, 100, 40, 0x336699) != 0,
          "unchanged: a plain background is still ONE rect of exactly its box");
    CHECK(ramp_count() == 0 && tile_count() == 0,
          "unchanged: ... and costs no ramp and no tile");

    render("<body><div id=a></div></body>",
           "body{margin:0} #a{position:absolute;left:10px;top:10px;width:100px;"
           "height:40px;border:3px dashed #ff0000}");
    int dashes = 0;
    for (int i = 0; i < paint_nops; i++)
        if (paint_ops[i].kind == OP_RECT && paint_ops[i].color == 0xFF0000) dashes++;
    CHECK(dashes > 8,
          "unchanged: a dashed border is still drawn dash by dash");
}

int main(void)
{
    t_shadow_ring();
    t_shadow_over_bg();
    t_shadow_offset();
    t_shadow_blur();
    t_shadow_inset();
    t_shadow_order();

    t_grad_vertical();
    t_grad_horizontal();
    t_grad_angle();
    t_grad_stop_pos();
    t_grad_refused();
    t_grad_diagonal();

    t_round_border_nobg();
    t_round_border_bg();

    t_xf_translate_pct();
    t_xf_subtree();
    t_xf_scale();
    t_xf_origin();
    t_xf_rotate_path();
    t_xf_nested();

    t_rclip_rect();
    t_rclip_border();
    t_rclip_square();

    t_unchanged();

    printf(fail ? "paint_gfx_test: FAILED\n" : "paint_gfx_test: ALL PASS\n");
    return fail;
}
