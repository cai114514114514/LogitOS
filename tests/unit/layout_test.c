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

    printf(fail ? "\nLAYOUT TEST FAILED\n" : "\nLAYOUT TEST PASSED\n");
    return fail;
}
