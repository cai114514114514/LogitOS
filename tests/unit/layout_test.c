/* Host test for the layout engine (net/layout.c). Compile with dom.c + css.c;
 * stub the kernel-only deps (heap, text metrics, image/resource fetch). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "css.h"
#include "dom.h"

/* --- stubs --- */
void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
/* memset/memcpy come from libc; declare matching protos used by modules */

/* fake proportional metrics: each glyph ~ px/2 px wide (ASCII-only test) */
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len * (px/2); }
int res_fetch(const char *url, uint8_t **buf, int *len){ (void)url;(void)buf;(void)len; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *out){ (void)p;(void)n;(void)out; return -1; }

static int fail;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fail=1; } else printf("ok: %s\n", msg); }while(0)

static struct node *find_display(struct node *n, int disp)
{
    if (n->type == N_ELEM) {
        struct cstyle *st = n->style;
        if (st && st->display == disp) return n;
    }
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find_display(c, disp);
        if (r) return r;
    }
    return NULL;
}

int main(void)
{
    const char *html =
        "<body style='width:200px'>"
        "<h1>Hi</h1>"
        "<p>one two three four five six seven</p>"
        "</body>";
    struct node *root = dom_parse(html, (int)strlen(html));
    CHECK(root != NULL, "dom_parse");
    css_apply(root, 0, 0);

    layout_page(root, 200);
    int n = layout_count();
    int h = layout_height();
    const struct item *it = layout_items();
    printf("items=%d height=%d\n", n, h);
    CHECK(n > 0, "produced items");

    /* count distinct line tops among the 16px (paragraph) text items */
    int ys[64], ny = 0;
    int h1_bottom = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT) continue;
        if (it[i].font_px == 32) {
            int b = it[i].y + it[i].h;
            if (b > h1_bottom) h1_bottom = b;
        }
        if (it[i].font_px == 16) {
            int seen = 0;
            for (int j = 0; j < ny; j++) if (ys[j] == it[i].y) seen = 1;
            if (!seen && ny < 64) ys[ny++] = it[i].y;
        }
    }
    printf("h1_bottom=%d  paragraph_lines=%d\n", h1_bottom, ny);
    CHECK(h1_bottom > 0, "h1 laid out (32px run present)");

    /* The page starts AT <body>, so <body>'s 8px UA margin is the top offset
     * and is applied exactly ONCE.
     *
     * This is the one geometry change the spec parser caused, and it is worth
     * pinning because it moves every real page. Before the switch, "<body>..."
     * parsed to #document -> <body>, and layout_page's hand-rolled two-level
     * scan looked for <body> among the document's GRANDchildren -- so it never
     * found it, started at #document instead, and then laid <body> out as an
     * ordinary block inside that. The 8px margin was charged twice, once as
     * the document's own top offset and again as the body block's margin, top
     * and bottom: every document was 16px taller than it should be and all its
     * content sat 8px too low. The spec tree is #document -> <html> -> <body>,
     * <body> is found, and the double count is gone. */
    int h1_top = 0x7fffffff;
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_TEXT && it[i].font_px == 32 && it[i].y < h1_top) h1_top = it[i].y;
    CHECK(h1_top == 8 + 14, "h1 top = body margin (8) + h1 margin (14), body margin counted once");
    CHECK(ny >= 2, "long paragraph wrapped into >=2 lines within width 200");

    /* paragraph must sit below the h1, and total height exceeds h1 + 1 line */
    int first_p_y = 0x7fffffff, p_lh = 0;
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_TEXT && it[i].font_px == 16) {
            if (it[i].y < first_p_y) first_p_y = it[i].y;
            p_lh = it[i].h;
        }
    CHECK(first_p_y >= h1_bottom, "paragraph starts below the h1");
    CHECK(h > h1_bottom + p_lh, "document height > h1 + one paragraph line");

    /* flex container with buttons holding bare text: the text must survive as
     * an anonymous flex item, on the same row as the other button's text */
    const char *html2 =
        "<body><div class='bar'>"
        "<button class='b'>Platform</button>"
        "<button class='c'>Solutions</button>"
        "</div></body>";
    const char *css2 = ".bar{display:flex}.b{display:flex}";
    struct node *r2 = dom_parse(html2, (int)strlen(html2));
    CHECK(r2 != NULL, "flex dom_parse");
    css_apply(r2, css2, (int)strlen(css2));
    layout_page(r2, 400);
    n = layout_count();
    it = layout_items();
    int py = -1, sy = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT) continue;
        if (it[i].len == 8 && !memcmp(it[i].text, "Platform", 8)) py = it[i].y;
        if (it[i].len == 9 && !memcmp(it[i].text, "Solutions", 9)) sy = it[i].y;
    }
    CHECK(py >= 0, "bare text inside flex button emitted");
    CHECK(sy >= 0, "second button text emitted");
    CHECK(py >= 0 && sy >= 0 && py == sy, "both button labels share the flex row");

    /* A width:100% flex item must not starve its auto siblings (GitHub's
     * header nav vanished because the cta container's width:100% claimed the
     * whole row); it takes only the leftover. A flex-grow item absorbs the
     * remaining space. */
    const char *html3 =
        "<body><div class='row'>"
        "<div class='nv'><span>NavLabel</span></div>"
        "<div class='cta'><span>CtaLabel</span></div>"
        "</div>"
        "<div class='gr'><div class='g'><span>x</span></div></div></body>";
    const char *css3 =
        ".row{display:flex;width:400px}.cta{width:100%}"
        ".gr{display:flex;width:400px}.g{flex-grow:1;background:#102030}";
    struct node *r3 = dom_parse(html3, (int)strlen(html3));
    CHECK(r3 != NULL, "flex pct/grow dom_parse");
    css_apply(r3, css3, (int)strlen(css3));
    layout_page(r3, 800);
    n = layout_count();
    it = layout_items();
    int nwx = -1, ctx = -1, groww = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type == IT_TEXT && it[i].len == 8 && !memcmp(it[i].text, "NavLabel", 8)) nwx = it[i].x;
        if (it[i].type == IT_TEXT && it[i].len == 8 && !memcmp(it[i].text, "CtaLabel", 8)) ctx = it[i].x;
        if (it[i].type == IT_RECT && it[i].has_bg) groww = it[i].w;
    }
    CHECK(nwx >= 0, "auto item next to width:100% item keeps its content width");
    CHECK(ctx > nwx, "width:100% item claims only the leftover, after auto siblings");
    CHECK(groww == 400, "flex-grow item absorbs the row's leftover space");

    /* A spaceless CJK title in a flex item wraps between characters instead of
     * overflowing the row (Bilibili video rail titles).
     *
     * The fixture is real CJK, not the ASCII stand-in it used to be, because
     * flex items now get CSS's automatic minimum size (min-width:auto = the
     * min-content width). A spaceless ASCII run has no break opportunity in
     * it, so that minimum is the whole run and the row overflows -- which is
     * exactly what a real browser does, and is asserted separately below. CJK
     * DOES have a break opportunity between any two ideographs, so its
     * min-content width is one character and the item still compresses. The
     * old ASCII fixture only wrapped because nothing floored the item at all. */
    const char *html4 =
        "<body><div class='row'>"
        "<div class='pic'></div>"
        "<div class='info'><p>\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B"
        "\xE4\xBA\x94\xE5\x85\xAD\xE4\xB8\x83\xE5\x85\xAB\xE4\xB9\x9D\xE5\x8D\x81"
        "\xE7\x99\xBE\xE5\x8D\x83\xE4\xB8\x87\xE4\xBA\xBF\xE5\x85\x86"
        "\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B\xE4\xBA\x94</p></div>"
        "</div></body>";
    const char *css4 =
        ".row{display:flex;width:220px}.pic{width:60px}"
        ".info p{font-size:16px}";
    struct node *r4 = dom_parse(html4, (int)strlen(html4));
    CHECK(r4 != NULL, "flex wrap dom_parse");
    css_apply(r4, css4, (int)strlen(css4));
    layout_page(r4, 400);
    n = layout_count();
    it = layout_items();
    int wrap_ys[8], nwy = 0, max_edge = 0, wtexts = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT) continue;
        wtexts++;
        if (it[i].x + it[i].w > max_edge) max_edge = it[i].x + it[i].w;
        int seen = 0;
        for (int j = 0; j < nwy; j++) if (wrap_ys[j] == it[i].y) seen = 1;
        if (!seen && nwy < 8) wrap_ys[nwy++] = it[i].y;
    }
    /* 20 ideographs * 24px (3 UTF-8 bytes * px/2 in the stub metrics) = 480px
     * of text in a 220px row: 3 lines, none of them past the row's edge. */
    CHECK(wtexts == 3, "CJK title split into 3 text items");
    CHECK(nwy == 3, "CJK title wrapped to 3 lines within the flex item");
    CHECK(max_edge <= 220 + 8, "no text item overflows the flex row width");

    /* The other half of the same rule: an unbreakable ASCII run IS the flex
     * item's automatic minimum, so the item keeps that width (capped at the
     * container) rather than being shredded a character at a time. */
    const char *html4b =
        "<body><div class='row'>"
        "<div class='pic'></div>"
        "<div class='info'><p>abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJ</p></div>"
        "</div></body>";
    struct node *r4b = dom_parse(html4b, (int)strlen(html4b));
    css_apply(r4b, css4, (int)strlen(css4));
    layout_page(r4b, 400);
    n = layout_count();
    it = layout_items();
    int longest = 0;
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_TEXT && it[i].w > longest) longest = it[i].w;
    /* min-content of the 46-char word is 368px, capped at the 220px row. */
    CHECK(longest >= 200, "unbreakable ASCII flex item floored at its min-content width");

    /* Minimal grid: 4 equal columns (repeat(4,1fr)) with a 20px gap lay their
     * items out side by side, wrapping to a second row (Bilibili card grid).
     * The tracks come from css_extra on real pages; here we patch the cstyle
     * directly to test the layout side. */
    const char *html5 =
        "<body><div class='g'>"
        "<div class='c'><span>a1</span></div><div class='c'><span>a2</span></div>"
        "<div class='c'><span>a3</span></div><div class='c'><span>a4</span></div>"
        "<div class='c'><span>a5</span></div>"
        "</div></body>";
    const char *css5 = ".g{display:grid}.c{background:#102030}";
    struct node *r5 = dom_parse(html5, (int)strlen(html5));
    CHECK(r5 != NULL, "grid dom_parse");
    css_apply(r5, css5, (int)strlen(css5));
    /* find the grid container and give it repeat(4,1fr) + gap 20 */
    struct node *grid = find_display(r5, DISP_GRID);
    CHECK(grid != NULL, "display:grid mapped to DISP_GRID");
    if (grid) {
        struct cstyle *gs = grid->style;
        gs->grid_cols = 4;
        for (int i = 0; i < 4; i++) gs->grid_tracks[i] = -10;   /* 1fr each */
        gs->grid_gap_x = gs->grid_gap_y = 20;
    }
    layout_page(r5, 436);
    n = layout_count();
    it = layout_items();
    int colx[8], ncol = 0, rowy[8], nrw = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT) continue;
        int sx = 0, sy = 0;
        for (int j = 0; j < ncol; j++) if (colx[j] == it[i].x) sx = 1;
        if (!sx && ncol < 8) colx[ncol++] = it[i].x;
        for (int j = 0; j < nrw; j++) if (rowy[j] == it[i].y) sy = 1;
        if (!sy && nrw < 8) rowy[nrw++] = it[i].y;
    }
    /* columns must be 4 evenly spaced x positions (col width + 20px gap) */
    CHECK(ncol == 4, "grid items occupy 4 distinct column x positions");
    for (int i = 0; i < ncol; i++)
        for (int j = i + 1; j < ncol; j++)
            if (colx[j] < colx[i]) { int t = colx[i]; colx[i] = colx[j]; colx[j] = t; }
    CHECK(ncol == 4 && colx[1] - colx[0] == colx[2] - colx[1] &&
          colx[2] - colx[1] == colx[3] - colx[2] && colx[1] - colx[0] > 20,
          "grid columns evenly spaced by width+gap");
    CHECK(nrw == 2, "fifth grid item wrapped to a second row");

    /* <img> without src falls back to data-src (lazy-loaded images, Bilibili
     * card covers). */
    const char *html6 = "<body><img data-src='cover.png' width='30' height='30'></body>";
    struct node *r6 = dom_parse(html6, (int)strlen(html6));
    CHECK(r6 != NULL, "img data-src dom_parse");
    css_apply(r6, 0, 0);
    layout_page(r6, 200);
    n = layout_count();
    it = layout_items();
    const char *isrc = 0;
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_IMAGE) isrc = it[i].imgsrc;
    CHECK(isrc && !strcmp(isrc, "cover.png"), "img data-src used when src is absent");

    /* ---- box-sizing ----
     * The same authored width under both models. content-box paints a box of
     * width + padding + border and lays its content out at `width`; border-box
     * paints exactly `width` and squeezes the content into what is left. Every
     * number below is the border-box rect the painter receives. */
    const char *html7 =
        "<body><div class='cb'>x</div><div class='bb'>y</div></body>";
    const char *css7 =
        ".cb{width:200px;padding:10px;border:5px solid #000;background:#111111;box-sizing:content-box}"
        ".bb{width:200px;padding:10px;border:5px solid #000;background:#222222;box-sizing:border-box}";
    struct node *r7 = dom_parse(html7, (int)strlen(html7));
    css_apply(r7, css7, (int)strlen(css7));
    layout_page(r7, 600);
    n = layout_count();
    it = layout_items();
    int cbw = -1, bbw = -1, cbx = -1, bbx = -1, cb_tx = -1, bb_tx = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type == IT_RECT && it[i].bg == 0x111111) { cbw = it[i].w; cbx = it[i].x; }
        if (it[i].type == IT_RECT && it[i].bg == 0x222222) { bbw = it[i].w; bbx = it[i].x; }
        if (it[i].type == IT_TEXT && it[i].len == 1 && it[i].text[0] == 'x') cb_tx = it[i].x;
        if (it[i].type == IT_TEXT && it[i].len == 1 && it[i].text[0] == 'y') bb_tx = it[i].x;
    }
    CHECK(cbw == 230, "content-box: painted width = 200 + 2*10 padding + 2*5 border");
    CHECK(bbw == 200, "border-box: painted width = the authored 200");
    /* content origin = border-box origin + border + padding, in both models */
    CHECK(cb_tx == cbx + 15, "content-box: text inset by border+padding");
    CHECK(bb_tx == bbx + 15, "border-box: text inset by border+padding");

    /* max-width + margin:auto -- the centering idiom the whole web uses. */
    const char *html8 = "<body><div class='wrap'>z</div></body>";
    const char *css8 = ".wrap{max-width:300px;margin:0 auto;background:#333333}";
    struct node *r8 = dom_parse(html8, (int)strlen(html8));
    css_apply(r8, css8, (int)strlen(css8));
    layout_page(r8, 800);           /* body margin 8 -> 784px of content */
    n = layout_count(); it = layout_items();
    int wx = -1, ww2 = -1;
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_RECT && it[i].bg == 0x333333) { wx = it[i].x; ww2 = it[i].w; }
    CHECK(ww2 == 300, "max-width clamps an auto-width block");
    CHECK(wx == 8 + (784 - 300) / 2, "margin:0 auto centres the clamped block");

    /* min-height grows a short box; min-width widens a narrow one. */
    const char *html9 = "<body><div class='mh'>q</div></body>";
    const char *css9 = ".mh{min-height:80px;min-width:400px;width:50px;background:#444444}";
    struct node *r9 = dom_parse(html9, (int)strlen(html9));
    css_apply(r9, css9, (int)strlen(css9));
    layout_page(r9, 600);
    n = layout_count(); it = layout_items();
    int mhh = -1, mhw = -1;
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_RECT && it[i].bg == 0x444444) { mhh = it[i].h; mhw = it[i].w; }
    CHECK(mhw == 400, "min-width overrides a smaller width");
    CHECK(mhh == 80, "min-height grows a one-line box");

    /* ---- white-space ----
     * <pre> must keep its newlines, its runs of spaces and its tab stops. */
    const char *html10 = "<body><pre>a b\n  c\n\nd\te</pre></body>";
    struct node *r10 = dom_parse(html10, (int)strlen(html10));
    css_apply(r10, 0, 0);
    layout_page(r10, 600);
    n = layout_count(); it = layout_items();
    int pys[16], npy = 0, twospace = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT) continue;
        int seen = 0;
        for (int j = 0; j < npy; j++) if (pys[j] == it[i].y) seen = 1;
        if (!seen && npy < 16) pys[npy++] = it[i].y;
        if (it[i].len == 3 && !memcmp(it[i].text, "  c", 3)) twospace = 1;
    }
    /* Three lines carry text ("a b", "  c", "d<TAB>e") but there are FOUR line
     * boxes: the empty line between them still advances the pen, which is the
     * whole point of preserving newlines. 20px per line at the UA's 16px
     * monospace, so the last line sits 3 lines down, not 2. */
    CHECK(npy == 3, "pre produced 3 lines that carry text");
    CHECK(twospace, "pre kept the two leading spaces in the run");
    { int dy = -1, dx = -1, ex = -1, firstpre = 0x7fffffff;
      for (int i = 0; i < n; i++) {
          if (it[i].type != IT_TEXT) continue;
          if (it[i].y < firstpre) firstpre = it[i].y;
          if (it[i].len != 1) continue;
          if (it[i].text[0] == 'd') { dx = it[i].x; dy = it[i].y; }
          if (it[i].text[0] == 'e') ex = it[i].x;
      }
      CHECK(dy == firstpre + 60, "the blank line consumed a line box of its own");
      /* the tab advanced the pen to the 8-space stop: space = 8px -> 64px */
      CHECK(dx == 8 && ex == 8 + 64, "pre tab advanced to the next 8-space tab stop");
    }
    /* the same markup with white-space:normal collapses to a single line */
    const char *css10b = "pre{white-space:normal}";
    struct node *r10b = dom_parse(html10, (int)strlen(html10));
    css_apply(r10b, css10b, (int)strlen(css10b));
    layout_page(r10b, 600);
    n = layout_count(); it = layout_items();
    int nys = 0, ys2[16];
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT) continue;
        int seen = 0;
        for (int j = 0; j < nys; j++) if (ys2[j] == it[i].y) seen = 1;
        if (!seen && nys < 16) ys2[nys++] = it[i].y;
    }
    CHECK(nys == 1, "white-space:normal collapses the same text onto one line");

    /* ---- flex: wrap, justify-content, align-items, order, column ---- */
    const char *html11 =
        "<body><div class='f'>"
        "<div class='i a'>A</div><div class='i b'>B</div><div class='i c'>C</div>"
        "</div></body>";
    const char *css11 =
        ".f{display:flex;width:250px;flex-wrap:wrap;justify-content:flex-end}"
        ".i{width:100px;flex-shrink:0;background:#555555}";
    struct node *r11 = dom_parse(html11, (int)strlen(html11));
    css_apply(r11, css11, (int)strlen(css11));
    layout_page(r11, 400);
    n = layout_count(); it = layout_items();
    int ax = -1, bx2 = -1, cx2 = -1, ay = -1, cy2 = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT || it[i].len != 1) continue;
        if (it[i].text[0] == 'A') { ax = it[i].x; ay = it[i].y; }
        if (it[i].text[0] == 'B') bx2 = it[i].x;
        if (it[i].text[0] == 'C') { cx2 = it[i].x; cy2 = it[i].y; }
    }
    /* two 100px items fit on a 250px line, the third wraps. flex-end pushes
     * the first line's 50px of slack to the left of item A. */
    CHECK(ax == 8 + 50, "flex-wrap: first line right-aligned by justify-content:flex-end");
    CHECK(bx2 == ax + 100, "second item follows the first on the same line");
    CHECK(cy2 > ay, "third item wrapped to a second flex line");
    CHECK(cx2 == 8 + 150, "wrapped line's single item is also flex-end aligned");

    /* order reverses the visual sequence without touching the DOM */
    const char *css12 =
        ".f{display:flex;width:400px}.i{width:100px;flex-shrink:0}"
        ".a{order:3}.b{order:2}.c{order:1}";
    struct node *r12 = dom_parse(html11, (int)strlen(html11));
    css_apply(r12, css12, (int)strlen(css12));
    layout_page(r12, 400);
    n = layout_count(); it = layout_items();
    int oa = -1, ob = -1, oc = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT || it[i].len != 1) continue;
        if (it[i].text[0] == 'A') oa = it[i].x;
        if (it[i].text[0] == 'B') ob = it[i].x;
        if (it[i].text[0] == 'C') oc = it[i].x;
    }
    CHECK(oc == 8 && ob == 108 && oa == 208, "flex `order` reorders items C,B,A");

    /* justify-content:space-between spreads the slack into the gaps only */
    const char *css13 = ".f{display:flex;width:400px;justify-content:space-between}"
                        ".i{width:100px;flex-shrink:0}";
    struct node *r13 = dom_parse(html11, (int)strlen(html11));
    css_apply(r13, css13, (int)strlen(css13));
    layout_page(r13, 400);
    n = layout_count(); it = layout_items();
    int sa = -1, sb = -1, sc = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT || it[i].len != 1) continue;
        if (it[i].text[0] == 'A') sa = it[i].x;
        if (it[i].text[0] == 'B') sb = it[i].x;
        if (it[i].text[0] == 'C') sc = it[i].x;
    }
    /* 400 - 300 = 100 slack over 2 gaps = 50 each */
    CHECK(sa == 8 && sb == 8 + 150 && sc == 8 + 300, "space-between spreads 100px over 2 gaps");

    /* align-items:center vertically centres a short item against a tall one */
    const char *html14 =
        "<body><div class='f'>"
        "<div class='tall'>T</div><div class='short'>S</div>"
        "</div></body>";
    const char *css14 =
        ".f{display:flex;width:400px;align-items:center}"
        ".tall{width:100px;height:100px}.short{width:100px}";
    struct node *r14 = dom_parse(html14, (int)strlen(html14));
    css_apply(r14, css14, (int)strlen(css14));
    layout_page(r14, 400);
    n = layout_count(); it = layout_items();
    int ty = -1, sy2 = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT || it[i].len != 1) continue;
        if (it[i].text[0] == 'T') ty = it[i].y;
        if (it[i].text[0] == 'S') sy2 = it[i].y;
    }
    /* line cross size 100, short item is one 20px line -> offset (100-20)/2 */
    CHECK(ty == 8, "tall flex item sits at the top of the line");
    CHECK(sy2 == 8 + 40, "align-items:center offsets the short item by (100-20)/2");

    /* flex-direction:column stacks its items and honours the column gap */
    const char *css15 = ".f{display:flex;flex-direction:column;width:400px;gap:12px}";
    struct node *r15 = dom_parse(html11, (int)strlen(html11));
    css_apply(r15, css15, (int)strlen(css15));
    { struct node *fx = find_display(r15, DISP_FLEX);
      if (fx) { struct cstyle *fs = fx->style; fs->grid_gap_x = fs->grid_gap_y = 12; } }
    layout_page(r15, 400);
    n = layout_count(); it = layout_items();
    int ka = -1, kb = -1, kc = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT || it[i].len != 1) continue;
        if (it[i].text[0] == 'A') ka = it[i].y;
        if (it[i].text[0] == 'B') kb = it[i].y;
        if (it[i].text[0] == 'C') kc = it[i].y;
    }
    CHECK(ka == 8 && kb == 8 + 32 && kc == 8 + 64,
          "flex column stacks items 20px tall with a 12px gap");

    /* ---- list-style-type ---- */
    const char *html16 =
        "<body><ol class='r'><li>a</li><li>b</li><li>c</li><li>d</li></ol>"
        "<ol class='al' start='2'><li>e</li></ol>"
        "<ul class='sq'><li>f</li></ul>"
        "<ul class='no'><li>g</li></ul></body>";
    const char *css16 = ".r{list-style-type:lower-roman}.al{list-style-type:upper-alpha}"
                        ".sq{list-style-type:square}.no{list-style-type:none}";
    struct node *r16 = dom_parse(html16, (int)strlen(html16));
    css_apply(r16, css16, (int)strlen(css16));
    layout_page(r16, 400);
    n = layout_count(); it = layout_items();
    int have_iv = 0, have_B = 0, have_sq = 0, markers = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT) continue;
        if (it[i].text == it[i].marker) markers++;
        if (it[i].len == 3 && !memcmp(it[i].text, "iv.", 3)) have_iv = 1;
        if (it[i].len == 2 && !memcmp(it[i].text, "B.", 2)) have_B = 1;
        if (it[i].len == 3 && !memcmp(it[i].text, "\xE2\x96\xAA", 3)) have_sq = 1;
    }
    CHECK(have_iv, "lower-roman numbers the 4th item 'iv.'");
    CHECK(have_B, "<ol start=2> + upper-alpha numbers the first item 'B.'");
    CHECK(have_sq, "list-style-type:square uses U+25AA");
    CHECK(markers == 6, "list-style-type:none emits no marker (6 markers, not 7)");

    /* ---- z-index ---- */
    const char *html17 =
        "<body><div class='under'>u</div><div class='over'>o</div></body>";
    const char *css17 =
        ".under{position:relative;z-index:5;background:#666666}"
        ".over{background:#777777}";
    struct node *r17 = dom_parse(html17, (int)strlen(html17));
    css_apply(r17, css17, (int)strlen(css17));
    layout_page(r17, 400);
    n = layout_count(); it = layout_items();
    int i_under = -1, i_over = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_RECT) continue;
        if (it[i].bg == 0x666666) i_under = i;
        if (it[i].bg == 0x777777) i_over = i;
    }
    CHECK(i_under > i_over,
          "z-index:5 moved the earlier box after the later one in paint order");

    /* ---- position:relative ---- */
    const char *html18 = "<body><div class='rel'>r</div></body>";
    const char *css18 = ".rel{position:relative;left:30px;top:7px;background:#888888}";
    struct node *r18 = dom_parse(html18, (int)strlen(html18));
    css_apply(r18, css18, (int)strlen(css18));
    layout_page(r18, 400);
    n = layout_count(); it = layout_items();
    int rx = -1, ry = -1, rtx = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type == IT_RECT && it[i].bg == 0x888888) { rx = it[i].x; ry = it[i].y; }
        if (it[i].type == IT_TEXT && it[i].len == 1 && it[i].text[0] == 'r') rtx = it[i].x;
    }
    CHECK(rx == 8 + 30 && ry == 8 + 7, "position:relative offsets the painted box");
    CHECK(rtx == 8 + 30, "the relative box's content moved with it");

    /* ---- text-align:justify ---- */
    const char *html19 = "<body><p class='j'>aa bb cc dd ee ff gg hh ii jj kk ll</p></body>";
    const char *css19 = ".j{text-align:justify;width:200px}";
    struct node *r19 = dom_parse(html19, (int)strlen(html19));
    css_apply(r19, css19, (int)strlen(css19));
    layout_page(r19, 400);
    n = layout_count(); it = layout_items();
    int firsty = 0x7fffffff;
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_TEXT && it[i].y < firsty) firsty = it[i].y;
    int rightmost = 0, lasty = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT) continue;
        if (it[i].y == firsty && it[i].x + it[i].w > rightmost) rightmost = it[i].x + it[i].w;
        if (it[i].y > lasty) lasty = it[i].y;
    }
    CHECK(rightmost == 8 + 200, "justified line stretched exactly to the measure");
    { int lastright = 0;
      for (int i = 0; i < n; i++)
          if (it[i].type == IT_TEXT && it[i].y == lasty && it[i].x + it[i].w > lastright)
              lastright = it[i].x + it[i].w;
      CHECK(lastright < 8 + 200, "the last line is not justified"); }

    /* ---- floats ----
     *
     * The stub metrics make every number here checkable by hand: a glyph is
     * px/2 wide, so a 16px 3-letter word is 24px and an inter-word space is
     * 8px, and a line box is 16*5/4 = 20px tall. `layout_page(root, 400)` gives
     * <body> an 8px margin all round, i.e. a content box of 384px starting at
     * x=8, y=8.
     *
     * Twenty 3-letter words is deliberate: a 200px line holds six of them
     * (24 + 5*32 = 184, a seventh would need 216), so three narrowed lines use
     * eighteen and the rest lands on the first line BELOW the float, where the
     * measure is the full 300px again. That is the whole float contract in one
     * fixture -- narrow while beside it, full width once past it. */
    const char *words20 =
        "aaa bbb ccc ddd eee fff ggg hhh iii jjj "
        "kkk lll mmm nnn ooo ppp qqq rrr sss ttt";
    char fbuf[512];

    snprintf(fbuf, sizeof fbuf,
             "<body><div class='f'></div><div class='t'>%s</div></body>", words20);
    const char *cssF1 = ".f{float:left;width:100px;height:60px;background:#0a0b0c}"
                        ".t{width:300px}";
    struct node *rf1 = dom_parse(fbuf, (int)strlen(fbuf));
    css_apply(rf1, cssF1, (int)strlen(cssF1));
    layout_page(rf1, 400);
    n = layout_count(); it = layout_items();
    int flx = -1, fly = -1, flw = -1, flh = -1;
    int beside_min_x = 1 << 30, beside_max_r = 0, below_min_x = 1 << 30, nbelow = 0;
    int ftops[16], nftop = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].type == IT_RECT && it[i].bg == 0x0a0b0c)
            { flx = it[i].x; fly = it[i].y; flw = it[i].w; flh = it[i].h; }
        if (it[i].type != IT_TEXT) continue;
        int seen = 0;
        for (int j = 0; j < nftop; j++) if (ftops[j] == it[i].y) seen = 1;
        if (!seen && nftop < 16) ftops[nftop++] = it[i].y;
        if (it[i].y < 68) {
            if (it[i].x < beside_min_x) beside_min_x = it[i].x;
            if (it[i].x + it[i].w > beside_max_r) beside_max_r = it[i].x + it[i].w;
        } else {
            nbelow++;
            if (it[i].x < below_min_x) below_min_x = it[i].x;
        }
    }
    CHECK(flx == 8 && fly == 8 && flw == 100 && flh == 60,
          "float:left placed at the content origin at its declared size");
    CHECK(beside_min_x == 108,
          "text beside a left float starts at the float's right edge (8+100)");
    CHECK(beside_max_r <= 308, "text beside the float still ends at the block's right edge");
    CHECK(nftop == 4, "20 words -> 3 narrowed lines beside the float + 1 below");
    CHECK(nbelow == 2 && below_min_x == 8,
          "the line below the float regains the full 300px measure (2 words at x=8)");

    /* float:right, declared INSIDE the block whose text wraps around it -- the
     * inline-context path, where flow_node takes the float out of flow without
     * breaking the line. Forty words this time so the first line past the float
     * is FULL (9 words, 24 + 8*32 = 280px) and its right edge can be checked
     * against the float's left edge; twenty would have left it two words long
     * and proved nothing. */
    snprintf(fbuf, sizeof fbuf,
             "<body><div class='t'><div class='f'></div>%s %s</div></body>", words20, words20);
    const char *cssF2 = ".t{width:300px}"
                        ".f{float:right;width:100px;height:60px;background:#0a0b0c}";
    struct node *rf2 = dom_parse(fbuf, (int)strlen(fbuf));
    css_apply(rf2, cssF2, (int)strlen(cssF2));
    layout_page(rf2, 400);
    n = layout_count(); it = layout_items();
    int frx = -1, frw = -1, r_beside_max = 0, r_beside_minx = 1 << 30, r_below_max = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].type == IT_RECT && it[i].bg == 0x0a0b0c) { frx = it[i].x; frw = it[i].w; }
        if (it[i].type != IT_TEXT) continue;
        if (it[i].y < 68) {
            if (it[i].x + it[i].w > r_beside_max) r_beside_max = it[i].x + it[i].w;
            if (it[i].x < r_beside_minx) r_beside_minx = it[i].x;
        } else if (it[i].x + it[i].w > r_below_max) r_below_max = it[i].x + it[i].w;
    }
    CHECK(frx == 208 && frw == 100, "float:right sits against the block's right edge (308-100)");
    CHECK(r_beside_minx == 8, "text beside a right float still starts at the left edge");
    CHECK(r_beside_max <= 208, "no text beside the right float crosses into it");
    CHECK(r_below_max > 208, "the line below the right float uses the full measure again");

    /* Two floats, one per side, narrowing the SAME line: 300 - 60 - 60 = 180px,
     * which holds five words (24 + 4*32 = 152; a sixth needs 184). */
    snprintf(fbuf, sizeof fbuf,
             "<body><div class='t'><div class='fl'></div><div class='fr'></div>%s</div></body>",
             words20);
    const char *cssF3 = ".t{width:300px}"
                        ".fl{float:left;width:60px;height:40px;background:#010101}"
                        ".fr{float:right;width:60px;height:40px;background:#020202}";
    struct node *rf3 = dom_parse(fbuf, (int)strlen(fbuf));
    css_apply(rf3, cssF3, (int)strlen(cssF3));
    layout_page(rf3, 400);
    n = layout_count(); it = layout_items();
    int flx3 = -1, frx3 = -1, first_y = 1 << 30, nfirst = 0, f3_minx = 1 << 30, f3_maxr = 0;
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_TEXT && it[i].y < first_y) first_y = it[i].y;
    for (int i = 0; i < n; i++) {
        if (it[i].type == IT_RECT && it[i].bg == 0x010101) flx3 = it[i].x;
        if (it[i].type == IT_RECT && it[i].bg == 0x020202) frx3 = it[i].x;
        if (it[i].type != IT_TEXT || it[i].y != first_y) continue;
        nfirst++;
        if (it[i].x < f3_minx) f3_minx = it[i].x;
        if (it[i].x + it[i].w > f3_maxr) f3_maxr = it[i].x + it[i].w;
    }
    CHECK(flx3 == 8 && frx3 == 248, "left and right floats take opposite edges of the block");
    CHECK(f3_minx == 68 && f3_maxr <= 248,
          "the line between two floats runs from 68 to at most 248");
    CHECK(nfirst == 5, "the 180px band between the two floats holds exactly 5 words");

    /* Three same-side floats in a 300px block. The two 100px ones sit side by
     * side in the first band (8..108 and 108..208). The 150px third does not
     * fit in the 100px left over, so it drops to the first band where it does:
     * past the SHALLOWER of the two (b, bottom 48), where only a still excludes
     * it and 200px are free from x=108. Landing at (108,48) rather than (8,68)
     * is the whole point -- a naive "drop below everything" placement would
     * waste the band beside the deeper float. */
    snprintf(fbuf, sizeof fbuf,
             "<body><div class='t'><div class='a'></div><div class='b'></div>"
             "<div class='c'></div>%s</div></body>", words20);
    const char *cssF8 = ".t{width:300px}"
                        ".a{float:left;width:100px;height:60px;background:#0a0a0a}"
                        ".b{float:left;width:100px;height:40px;background:#0b0b0b}"
                        ".c{float:left;width:150px;height:20px;background:#0c0c0c}";
    struct node *rf8 = dom_parse(fbuf, (int)strlen(fbuf));
    css_apply(rf8, cssF8, (int)strlen(cssF8));
    layout_page(rf8, 400);
    n = layout_count(); it = layout_items();
    int ax8 = -1, ay8 = -1, bx8 = -1, by8 = -1, cx8 = -1, cy8 = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_RECT) continue;
        if (it[i].bg == 0x0a0a0a) { ax8 = it[i].x; ay8 = it[i].y; }
        if (it[i].bg == 0x0b0b0b) { bx8 = it[i].x; by8 = it[i].y; }
        if (it[i].bg == 0x0c0c0c) { cx8 = it[i].x; cy8 = it[i].y; }
    }
    CHECK(ax8 == 8 && ay8 == 8, "the first left float takes the content origin");
    CHECK(bx8 == 108 && by8 == 8, "the second left float sits beside it in the same band");
    CHECK(cx8 == 108 && cy8 == 48,
          "the third drops past the shallower float only, and lands beside the deeper one");

    /* A float declared MID-LINE cannot narrow words that were already measured
     * against the wider band, so it starts at the next line box instead. The
     * first line therefore keeps the full measure. */
    snprintf(fbuf, sizeof fbuf,
             "<body><div class='t'>xxx yyy<div class='f'></div>%s</div></body>", words20);
    const char *cssF9 = ".t{width:300px}"
                        ".f{float:left;width:100px;height:60px;background:#0d0d0d}";
    struct node *rf9 = dom_parse(fbuf, (int)strlen(fbuf));
    css_apply(rf9, cssF9, (int)strlen(cssF9));
    layout_page(rf9, 400);
    n = layout_count(); it = layout_items();
    int mfy = -1, mfx = -1, first_row_minx = 1 << 30, second_row_minx = 1 << 30;
    for (int i = 0; i < n; i++) {
        if (it[i].type == IT_RECT && it[i].bg == 0x0d0d0d) { mfx = it[i].x; mfy = it[i].y; }
        if (it[i].type != IT_TEXT) continue;
        if (it[i].y == 8 && it[i].x < first_row_minx) first_row_minx = it[i].x;
        if (it[i].y == 28 && it[i].x < second_row_minx) second_row_minx = it[i].x;
    }
    CHECK(mfx == 8 && mfy == 28, "a mid-line float starts at the NEXT line box, not this one");
    CHECK(first_row_minx == 8, "the line the float appeared on keeps its full measure");
    CHECK(second_row_minx == 108, "the following line is narrowed by it");

    /* `clear` drops a following block below the floats on the named side. The
     * two floats end at different depths on purpose (58 and 88), so each value
     * of `clear` lands somewhere different. One cleared box per run, so the
     * numbers are independent instead of chaining through each other's
     * heights. */
    const char *htmlF4 =
        "<body><div class='fl'></div><div class='fr'></div><div class='c'>x</div></body>";
    static const char *const clear_css[4] = {
        ".fl{float:left;width:60px;height:50px;background:#010101}"
        ".fr{float:right;width:60px;height:80px;background:#020202}"
        ".c{background:#033333}",
        ".fl{float:left;width:60px;height:50px;background:#010101}"
        ".fr{float:right;width:60px;height:80px;background:#020202}"
        ".c{clear:left;background:#033333}",
        ".fl{float:left;width:60px;height:50px;background:#010101}"
        ".fr{float:right;width:60px;height:80px;background:#020202}"
        ".c{clear:right;background:#033333}",
        ".fl{float:left;width:60px;height:50px;background:#010101}"
        ".fr{float:right;width:60px;height:80px;background:#020202}"
        ".c{clear:both;background:#033333}",
    };
    int cleary[4] = { -1, -1, -1, -1 };
    for (int k = 0; k < 4; k++) {
        struct node *rf4 = dom_parse(htmlF4, (int)strlen(htmlF4));
        css_apply(rf4, clear_css[k], (int)strlen(clear_css[k]));
        layout_page(rf4, 400);
        n = layout_count(); it = layout_items();
        for (int i = 0; i < n; i++)
            if (it[i].type == IT_RECT && it[i].bg == 0x033333) cleary[k] = it[i].y;
    }
    CHECK(cleary[0] == 8, "without `clear` the block starts beside the floats, at the top");
    CHECK(cleary[1] == 58, "clear:left drops to the left float's bottom (8+50)");
    CHECK(cleary[2] == 88, "clear:right drops to the right float's bottom (8+80)");
    CHECK(cleary[3] == 88, "clear:both drops past the deeper of the two");

    /* A float taller than its container. A BFC root (overflow:hidden here)
     * GROWS to contain its floats -- the classic clearfix -- while an ordinary
     * block does not and the float simply overflows it. The control box is what
     * makes this an assertion about containment and not about the float. */
    const char *htmlF5 =
        "<body><div class='box'><div class='f'></div>hi</div>"
        "<div class='plain'><div class='f'></div>hi</div></body>";
    const char *cssF5 =
        ".box{overflow:hidden;width:300px;background:#040404}"
        ".plain{width:300px;background:#060606}"
        ".f{float:left;width:80px;height:120px;background:#050505}";
    struct node *rf5 = dom_parse(htmlF5, (int)strlen(htmlF5));
    css_apply(rf5, cssF5, (int)strlen(cssF5));
    layout_page(rf5, 400);
    n = layout_count(); it = layout_items();
    int boxh = -1, plainh = -1, boxy = -1, plainy = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_RECT) continue;
        if (it[i].bg == 0x040404) { boxh = it[i].h; boxy = it[i].y; }
        if (it[i].bg == 0x060606) { plainh = it[i].h; plainy = it[i].y; }
    }
    CHECK(boxh == 120, "overflow:hidden grows to contain a 120px float (clearfix)");
    CHECK(plainh == 20, "a plain block keeps its one-line height and lets the float overflow");
    CHECK(boxy == 8 && plainy == 128,
          "the contained float pushed the next block down; the overflowing one did not");

    /* ---- overflow clip ----
     * The clip is stamped onto every item the subtree emits, in document
     * coordinates, and is the ancestor's PADDING box. With an auto height there
     * is nothing below the content to cut, so only the horizontal bounds are
     * finite; a definite height bounds it both ways. */
    const char *htmlF6 =
        "<body><div class='auto'>hi</div><div class='sized'>hi</div>"
        "<div class='vis'>hi</div></body>";
    const char *cssF6 =
        ".auto{overflow:hidden;width:300px;padding:10px}"
        ".sized{overflow:hidden;width:200px;height:40px}"
        ".vis{width:200px}";
    struct node *rf6 = dom_parse(htmlF6, (int)strlen(htmlF6));
    css_apply(rf6, cssF6, (int)strlen(cssF6));
    layout_page(rf6, 400);
    n = layout_count(); it = layout_items();
    int a_clip = -1, a_cx = -1, a_cw = -1, a_cy = -1;
    int s_clip = -1, s_ch = -1, s_cw = -1;
    int v_clip = -1;
    int seen_txt = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT) continue;
        if (seen_txt == 0) { a_clip = it[i].has_clip; a_cx = it[i].clip_x;
                             a_cw = it[i].clip_w; a_cy = it[i].clip_y; }
        if (seen_txt == 1) { s_clip = it[i].has_clip; s_ch = it[i].clip_h; s_cw = it[i].clip_w; }
        if (seen_txt == 2) v_clip = it[i].has_clip;
        seen_txt++;
    }
    CHECK(a_clip == 1 && a_cx == 8 && a_cw == 320 && a_cy == 8,
          "overflow:hidden clips content to the padding box (8..328 with 10px padding)");
    CHECK(s_clip == 1 && s_cw == 200 && s_ch == 40,
          "a definite height bounds the clip vertically as well");
    CHECK(v_clip == 0, "overflow:visible leaves its content unclipped");

    /* A floated <img> is a replaced element, not an empty block box: it has to
     * reserve its declared size and exclude the text just like a floated div. */
    snprintf(fbuf, sizeof fbuf,
             "<body><div class='t'><img src='a.png' width='80' height='60' class='fi'>%s</div></body>",
             words20);
    const char *cssF7 = ".t{width:300px}.fi{float:left}";
    struct node *rf7 = dom_parse(fbuf, (int)strlen(fbuf));
    css_apply(rf7, cssF7, (int)strlen(cssF7));
    layout_page(rf7, 400);
    n = layout_count(); it = layout_items();
    int imx = -1, imy = -1, imw = -1, imh = -1, i_minx = 1 << 30;
    for (int i = 0; i < n; i++) {
        if (it[i].type == IT_IMAGE) { imx = it[i].x; imy = it[i].y; imw = it[i].w; imh = it[i].h; }
        if (it[i].type == IT_TEXT && it[i].y < 68 && it[i].x < i_minx) i_minx = it[i].x;
    }
    CHECK(imx == 8 && imy == 8 && imw == 80 && imh == 60,
          "a floated <img> reserves its declared box out of flow");
    CHECK(i_minx == 88, "text wraps beside the floated image (8+80)");

    printf(fail ? "\nLAYOUT TEST FAILED\n" : "\nLAYOUT TEST PASSED\n");
    return fail;
}
