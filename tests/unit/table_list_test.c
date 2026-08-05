/* Table + list-marker + inline-block layout test: HTML -> DOM -> CSS -> display
 * list, asserting the grid geometry of a 3-col table, bullet/number markers on
 * <li>, and that an inline-block element becomes its own box. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "css.h"
#include "dom.h"

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len*(px/2); }
int res_fetch(const char *u, uint8_t **b, int *l){ (void)u;(void)b;(void)l; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *o){ (void)p;(void)n;(void)o; return -1; }

static int fail;
#define CHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m);fail=1;} else printf("ok: %s\n",m);}while(0)

/* find an IT_TEXT whose bytes start with `prefix`; returns its index or -1 */
static int find_text(const struct item *it, int n, const char *prefix)
{
    int pl = (int)strlen(prefix);
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_TEXT && it[i].len >= pl && !memcmp(it[i].text, prefix, pl)) return i;
    return -1;
}

int main(void)
{
    css_init();
    const char *html =
        "<body>"
        "<table>"
        "<tr><th>Name</th><th>Value</th></tr>"
        "<tr><td>alpha</td><td>22</td></tr>"
        "<tr><td>beta</td><td>333</td></tr>"
        "</table>"
        "<ul><li>bullet one</li><li>bullet two</li></ul>"
        "<ol><li>first</li><li>second</li><li>third</li></ol>"
        "<p>before <span style='display:inline-block;background:#eee'>chip</span> after</p>"
        "</body>";
    struct node *root = dom_parse(html, (int)strlen(html));
    CHECK(root != NULL, "dom_parse");
    css_apply(root, "", 0);
    layout_page(root, 600);
    int n = layout_count();
    const struct item *it = layout_items();
    printf("items=%d height=%d\n", n, layout_height());

    /* table: cells of one row share a y; columns have distinct x; rows descend */
    int i_name = find_text(it, n, "Name"), i_val = find_text(it, n, "Value");
    int i_alpha = find_text(it, n, "alpha"), i_22 = find_text(it, n, "22");
    int i_beta = find_text(it, n, "beta"), i_333 = find_text(it, n, "333");
    CHECK(i_name >= 0 && i_val >= 0 && i_alpha >= 0 && i_22 >= 0 && i_beta >= 0 && i_333 >= 0,
          "all six cells laid out");
    CHECK(it[i_name].y == it[i_val].y, "header row cells share y");
    CHECK(it[i_alpha].y == it[i_22].y, "row 2 cells share y");
    CHECK(it[i_name].x == it[i_alpha].x && it[i_alpha].x == it[i_beta].x, "column 1 aligned");
    CHECK(it[i_val].x == it[i_22].x && it[i_22].x == it[i_333].x, "column 2 aligned");
    CHECK(it[i_val].x > it[i_name].x, "column 2 right of column 1");
    CHECK(it[i_name].y < it[i_alpha].y && it[i_alpha].y < it[i_beta].y, "rows descend");

    /* lists: bullet and number markers exist, numbers run 1..3 in order */
    int i_bullet = find_text(it, n, "\xE2\x80\xA2");
    int i_n1 = find_text(it, n, "1."), i_n2 = find_text(it, n, "2."), i_n3 = find_text(it, n, "3.");
    CHECK(i_bullet >= 0, "ul bullet marker emitted");
    CHECK(i_n1 >= 0 && i_n2 >= 0 && i_n3 >= 0, "ol markers 1. 2. 3. emitted");
    CHECK(it[i_n1].y < it[i_n2].y && it[i_n2].y < it[i_n3].y, "ol numbers in vertical order");
    int i_b1 = find_text(it, n, "bullet");
    CHECK(i_b1 >= 0 && it[i_bullet].x < it[i_b1].x, "bullet left of its li text");
    int i_first = find_text(it, n, "first");
    CHECK(i_first >= 0 && it[i_n1].y == it[i_first].y, "marker on the li's first line");

    /* inline-block chip: its text is on its own line, not fused with 'before' */
    int i_before = find_text(it, n, "before"), i_chip = find_text(it, n, "chip"), i_after = find_text(it, n, "after");
    CHECK(i_before >= 0 && i_chip >= 0 && i_after >= 0, "inline-block content present");
    CHECK(it[i_chip].y > it[i_before].y, "inline-block starts a new line");

    layout_free();
    dom_free(root);
    if (fail) { printf("FAILURES\n"); return 1; }
    printf("ALL PASS\n");
    return 0;
}
