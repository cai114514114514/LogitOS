/* Host test for the LibCSS-backed CSS engine (user/css_engine.c).
 * Compiles css_engine.c + net/dom.c + all of LibCSS natively and checks that
 * the produced struct cstyle matches expectations (UA + author + inline). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "css.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static struct cstyle *st(struct node *n) { return (struct cstyle *)n->style; }
static struct node *find(struct node *n, const char *tag)
{
    if (n->type == N_ELEM && strcmp(n->tag, tag) == 0) return n;
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find(c, tag);
        if (r) return r;
    }
    return NULL;
}

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails = 1; } else printf("ok: %s\n", m); } while (0)

int main(void)
{
    const char *html =
        "<html><head><style>.hi{color:#ff0000} p{font-size:20px}</style></head>"
        "<body><h1>Title</h1><p class='hi'>para</p>"
        "<a href='/y'>link</a>"
        "<div style='background:#00ff00;width:200px'>box</div>"
        "<pre>code</pre></body></html>";
    struct node *root = dom_parse(html, (int)strlen(html));
    if (!root) { printf("FAIL: dom_parse\n"); return 1; }

    css_init();
    const char *author = ".hi{color:#ff0000} p{font-size:20px}";
    css_apply(root, author, (int)strlen(author));

    struct node *h1 = find(root, "h1"), *p = find(root, "p"),
                *a = find(root, "a"), *div = find(root, "div"), *pre = find(root, "pre");

    CHECK(h1 && st(h1), "h1 has computed style");
    CHECK(st(h1)->display == DISP_BLOCK, "h1 display:block (UA)");
    CHECK(st(h1)->font_px == 32, "h1 font-size 32 (UA)");
    CHECK(st(h1)->bold, "h1 bold (UA)");
    CHECK(st(p) && st(p)->display == DISP_BLOCK, "p display:block (UA)");
    CHECK(st(p)->font_px == 20, "p font-size 20 (author)");
    CHECK(st(p)->color == 0xff0000, "p.hi color red (author class)");
    CHECK(st(a) && st(a)->color == 0x1a0dab, "a link color blue (UA)");
    CHECK(st(a)->underline, "a underline (UA)");
    CHECK(st(div) && st(div)->has_bg && st(div)->background == 0x00ff00, "div bg green (inline)");
    CHECK(st(div)->has_w && st(div)->width == 200, "div width 200px (inline)");
    CHECK(st(pre) && st(pre)->mono, "pre monospace (UA)");

    printf(fails ? "\nCSS ENGINE TEST FAILED\n" : "\nCSS ENGINE TEST PASSED\n");
    return fails;
}
