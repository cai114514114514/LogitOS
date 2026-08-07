/* Host test for the PAINTER (c/apps/browser/browser_paint.c).
 *
 * layout_test asserts on `struct item`, which stops one step short of the
 * claim that matters: a box can carry the right geometry, the right colour and
 * the right alpha in the display list and still be drawn wrong -- or not drawn
 * at all. This test runs the real pipeline (parse -> style -> layout) and then
 * the real painter, with the five GUI syscall wrappers replaced by recorders
 * (tests/unit/painthost/logit.h), and asserts on the draw ops that come out.
 *
 * What is under test here is exactly the set of properties that used to reach
 * `struct item` and die there: opacity, background-color alpha, the two
 * text-decoration lines that are not underline, the non-solid border styles,
 * and the overflow clip. */
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

/* --- stubs (same set layout_test uses) --- */
void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len * (px/2); }
int res_fetch(const char *url, uint8_t **buf, int *len){ (void)url;(void)buf;(void)len; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *out){ (void)p;(void)n;(void)out; return -1; }

static int fail;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fail=1; } else printf("ok: %s\n", msg); }while(0)

/* Lay a page out and paint it into a 400x600 viewport at scroll 0, so that
 * device coordinates equal document coordinates and every assertion below can
 * be read straight off the fixture. */
static void render(const char *html, const char *css)
{
    struct node *root = dom_parse(html, (int)strlen(html));
    css_apply(root, css, css ? (int)strlen(css) : 0);
    layout_page(root, 400);
    paint_nops = 0;
    browser_paint(0, 0, 400, 600, 0);
}

/* The blended fill covering (x,y): a 1x1 BLIT is how the painter does alpha. */
static const struct paintop *blend_at(int x, int y)
{
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind != OP_BLIT || !o->solid) continue;
        if (x >= o->x && x < o->x + o->w && y >= o->y && y < o->y + o->h) return o;
    }
    return 0;
}

/* border-radius reaches cstyle through css_extra on real pages; this test
 * drives the painter, so it patches the value straight onto every styled
 * element that has a background. */
static void set_radius(struct node *n, int r)
{
    if (n->type == N_ELEM && n->style) {
        struct cstyle *st = n->style;
        if (st->has_bg) st->radius = r;
    }
    for (struct node *c = n->first_child; c; c = c->next) set_radius(c, r);
}

static const struct paintop *text_op(const char *s)
{
    int n = (int)strlen(s);
    for (int i = 0; i < paint_nops; i++)
        if (paint_ops[i].kind == OP_TEXT && paint_ops[i].len == n &&
            !memcmp(paint_ops[i].text, s, (size_t)n)) return &paint_ops[i];
    return 0;
}

/* Count RECT/BLIT ops whose whole body lies in the horizontal band [y0,y1). */
static int ops_in_band(int y0, int y1)
{
    int c = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *o = &paint_ops[i];
        if (o->kind != OP_RECT && o->kind != OP_BLIT) continue;
        if (o->y >= y0 && o->y + o->h <= y1) c++;
    }
    return c;
}

int main(void)
{
    /* ---- background-color alpha ----
     * rgba() used to paint fully opaque: the alpha byte reached `struct item`
     * and the painter called the opaque fill. Now it must reach the blended
     * path with the alpha intact. */
    render("<body><div class='a'>x</div></body>",
           "body{background:#ffffff;margin:0}"
           ".a{background:rgba(255,0,0,0.5);width:200px;height:50px}");
    const struct paintop *o = blend_at(10, 10);
    CHECK(o != 0, "rgba background went through the blended fill path");
    CHECK(o && o->color == 0xFF0000, "the blended fill keeps the authored RGB");
    CHECK(o && o->alpha >= 126 && o->alpha <= 129,
          "alpha 0.5 reaches the painter as ~128/255, not 255");
    CHECK(o && o->w == 200 && o->h == 50, "the blended fill covers the whole box");

    /* An opaque background must NOT take the blend path -- one syscall, not a
     * scaled blit. */
    render("<body><div class='a'>x</div></body>",
           "body{background:#ffffff;margin:0}.a{background:#ff0000;width:200px;height:50px}");
    CHECK(blend_at(10, 10) == 0, "an opaque background still uses the plain fill");

    /* ---- opacity ----
     * opacity used to be binary: 0 hid the box and everything else painted
     * fully opaque. */
    render("<body><div class='a'>x</div></body>",
           "body{background:#ffffff;margin:0}"
           ".a{background:#ff0000;opacity:0.5;width:200px;height:50px}");
    o = blend_at(10, 10);
    CHECK(o && o->alpha >= 126 && o->alpha <= 129,
          "opacity:0.5 on an opaque background blends at ~128");

    /* ...and the two alphas MULTIPLY: rgba(.5) inside opacity:.5 is 25% ink. */
    render("<body><div class='a'>x</div></body>",
           "body{background:#ffffff;margin:0}"
           ".a{background:rgba(255,0,0,0.5);opacity:0.5;width:200px;height:50px}");
    o = blend_at(10, 10);
    CHECK(o && o->alpha >= 62 && o->alpha <= 66,
          "background alpha and opacity multiply to ~64/255");

    /* Text opacity is folded into the colour against the backdrop, which is
     * exact for the glyph blend the kernel does. Black at 50% over the white
     * page background is mid grey. */
    render("<body><p class='t'>zz</p></body>",
           "body{background:#ffffff;margin:0}.t{color:#000000;opacity:0.5}");
    o = text_op("zz");
    CHECK(o != 0, "the faded text run was still drawn");
    { int r = o ? (int)((o->color >> 16) & 0xFF) : -1;
      CHECK(r >= 120 && r <= 136,
            "opacity:0.5 black text is drawn mid-grey against the white backdrop"); }

    /* The backdrop is the nearest opaque box underneath, not always the page:
     * the same text over a black card must fade toward BLACK, not toward
     * white. This is the whole reason backdrop_at walks the list backwards. */
    render("<body><div class='card'><p class='t'>zz</p></div></body>",
           "body{background:#ffffff;margin:0}"
           ".card{background:#000000;width:300px;height:60px}"
           ".t{color:#ffffff;opacity:0.5;margin:0}");
    o = text_op("zz");
    { int r = o ? (int)((o->color >> 16) & 0xFF) : -1;
      CHECK(r >= 120 && r <= 136,
            "white text at 50% over a black card fades toward the card, not the page"); }

    /* A rounded box with a TRANSLUCENT background cannot go through gui_rrect
     * (that syscall has no alpha), so the painter bands it: one blended strip
     * per row of the two corner zones plus one for the straight middle. That is
     * also the only exercise the corner arithmetic gets. border-radius comes
     * from css_extra on real pages, so it is patched onto the cstyle here. */
    {
        const char *h = "<body><div class='r'>x</div></body>";
        const char *c = "body{background:#ffffff;margin:0}"
                        ".r{background:rgba(0,0,254,0.5);width:100px;height:60px}";
        struct node *root = dom_parse(h, (int)strlen(h));
        css_apply(root, c, (int)strlen(c));
        set_radius(root, 10);
        layout_page(root, 400);
        paint_nops = 0;
        browser_paint(0, 0, 400, 600, 0);
        int strips = 0, rr = 0, widest = 0, narrowest = 1 << 30;
        for (int i = 0; i < paint_nops; i++) {
            const struct paintop *p = &paint_ops[i];
            if (p->kind == OP_RRECT) rr++;
            if (p->kind != OP_BLIT || !p->solid) continue;
            strips++;
            if (p->w > widest) widest = p->w;
            if (p->w < narrowest) narrowest = p->w;
        }
        CHECK(rr == 0, "a translucent rounded box never uses the opaque rounded-rect call");
        CHECK(strips == 2 * 10 + 1,
              "it is banded into 2*radius corner rows plus one middle strip");
        CHECK(widest == 100, "the middle strip spans the full box width");
        /* first corner row: inset = 10 - isqrt(100 - 81) = 10 - 4 = 6 each side */
        CHECK(narrowest == 100 - 2 * 6,
              "the outermost corner row is inset by the circle's own arithmetic");
    }

    /* ---- text-decoration ----
     * Only the underline bit was drawn. All three lines are 1px at 16px, and
     * each sits at its own offset from the top of the em box. */
    render("<body><p class='u'>ab</p><p class='s'>cd</p><p class='o'>ef</p></body>",
           "body{background:#ffffff;margin:0}p{margin:0;font-size:16px}"
           ".u{text-decoration:underline}.s{text-decoration:line-through}"
           ".o{text-decoration:overline}");
    const struct paintop *tu = text_op("ab"), *ts = text_op("cd"), *to = text_op("ef");
    CHECK(tu && ts && to, "all three decorated runs were drawn");
    int nu = 0, ns = 0, no = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *p = &paint_ops[i];
        if (p->kind != OP_RECT || p->h != 1 || p->w != 16) continue;    /* 2 glyphs * 8px */
        if (tu && p->y == tu->y + 16 + 2) nu++;
        if (ts && p->y == ts->y + 16 + 2 - 16 * 30 / 100) ns++;   /* 0.3em above it */
        if (to && p->y == to->y) no++;
    }
    CHECK(nu == 1, "underline drawn just below the em box");
    CHECK(ns == 1, "line-through drawn 0.3em above the underline, i.e. through the glyphs");
    CHECK(no == 1, "overline drawn on the top edge of the em box");

    /* ---- border styles ----
     * Every non-none style used to paint solid. Each pattern is checked by the
     * NUMBER of fills its top edge takes, which is the only observable that
     * distinguishes them without a framebuffer.
     *
     * box-sizing:border-box makes the painted box exactly 200px wide so the
     * dash arithmetic is round; the box starts at (8,8) because <body> falls
     * back to an 8px margin. Only the TOP edge is given a border, so the counts
     * below are that one edge's pattern and nothing else -- with all four edges
     * present the left edge's first dot lands on the same pixel as the top
     * edge's and the totals stop being readable. */
#define BORDER_CSS(rest) \
    "body{background:#ffffff;margin:0}" \
    ".b{box-sizing:border-box;width:200px;height:40px;" rest "}"

    render("<body><div class='b'>x</div></body>", BORDER_CSS("border-top:2px solid #123456"));
    CHECK(ops_in_band(8, 10) == 1, "a solid 2px top border is one fill");

    render("<body><div class='b'>x</div></body>", BORDER_CSS("border-top:2px dotted #123456"));
    /* dots are `thick` long with a `thick` gap: 200 / 4 = 50 */
    CHECK(ops_in_band(8, 10) == 50, "a dotted 2px top border is 50 square dots over 200px");

    render("<body><div class='b'>x</div></body>", BORDER_CSS("border-top:2px dashed #123456"));
    /* dashes are 3*thick on, 2*thick off: ceil(200 / 10) = 20 */
    CHECK(ops_in_band(8, 10) == 20, "a dashed 2px top border is 20 dashes over 200px");

    render("<body><div class='b'>x</div></body>", BORDER_CSS("border-top:6px double #123456"));
    CHECK(ops_in_band(8, 14) == 2, "a double 6px top border is two 2px lines with a gap");

    /* The same patterns run along the OTHER axis on the left/right edges. */
    render("<body><div class='b'>x</div></body>", BORDER_CSS("border-left:6px double #123456"));
    { int nv = 0;
      for (int i = 0; i < paint_nops; i++) {
          const struct paintop *p = &paint_ops[i];
          if (p->kind == OP_RECT && p->w == 2 && p->h == 40) nv++;
      }
      CHECK(nv == 2, "a double 6px LEFT border is two 2px columns, i.e. the pattern rotates"); }

    /* groove splits the thickness into two tones -- two fills, and they must
     * differ from each other and from the authored colour. */
    render("<body><div class='b'>x</div></body>", BORDER_CSS("border-top:6px groove #808080"));
    unsigned g1 = 0, g2 = 0; int gn = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *p = &paint_ops[i];
        if (p->kind != OP_RECT || p->y < 8 || p->y + p->h > 14 || p->w != 200) continue;
        if (gn == 0) g1 = p->color; else if (gn == 1) g2 = p->color;
        gn++;
    }
    CHECK(gn == 2, "a groove top border is two half-thickness bands");
    CHECK(gn == 2 && g1 != g2 && g1 != 0x808080 && g2 != 0x808080,
          "the two groove bands are a lighter and a darker shade of the border colour");
#undef BORDER_CSS

    /* ---- overflow ----
     * The clip has to be programmed for the clipped subtree and released
     * afterwards, and the painter must not re-program it per box. */
    render("<body><div class='clip'>xx</div><div class='free'>yy</div></body>",
           "body{background:#ffffff;margin:0}"
           ".clip{overflow:hidden;width:120px;height:30px}.free{width:120px}");
    int clip_set = 0, clip_full = 0;
    for (int i = 0; i < paint_nops; i++) {
        const struct paintop *p = &paint_ops[i];
        if (p->kind != OP_CLIP) continue;
        if (p->w == 120 && p->h == 30) clip_set++;
        if (p->w == 400 && p->h == 600) clip_full++;
    }
    CHECK(clip_set == 1, "the overflow:hidden box programmed its clip exactly once");
    CHECK(clip_full >= 2, "the clip was restored to the viewport for the unclipped sibling");
    { const struct paintop *t = text_op("yy");
      CHECK(t != 0, "content outside the clipping box is still painted"); }

    /* An item whose clip does not intersect the viewport is skipped entirely
     * rather than drawn with a degenerate clip (gui_clip(0,0,0,0) CLEARS the
     * clip in the kernel, so emitting one would have leaked). */
    render("<body><div class='clip'><div class='in'>zz</div></div></body>",
           "body{background:#ffffff;margin:0}"
           ".clip{overflow:hidden;width:120px;height:0}.in{height:40px}");
    CHECK(text_op("zz") == 0, "content clipped to a zero-height box is not drawn at all");

    printf(fail ? "\nPAINT TEST FAILED\n" : "\nPAINT TEST PASSED\n");
    return fail;
}
