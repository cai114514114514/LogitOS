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

    printf(fail ? "\nLAYOUT TEST FAILED\n" : "\nLAYOUT TEST PASSED\n");
    return fail;
}
