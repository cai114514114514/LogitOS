/* Diagnostic: dump computed font_px/display/color per element for an HTML file.
 * Build with css_engine.c + net/dom.c + LibCSS; run on a saved page. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"
#include "css.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static int tag_is(const char *t, const char *l) { int i = 0; for (; l[i]; i++) if (t[i] != l[i]) return 0; return t[i] == 0; }
static int collect_style(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && tag_is(n->tag, "style"))
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
    for (struct node *c = n->first_child; c; c = c->next) o = collect_style(c, out, o, max);
    return o;
}
static void dump(struct node *n, int d)
{
    if (n->type == N_ELEM) {
        struct cstyle *s = n->style;
        for (int i = 0; i < d; i++) printf("  ");
        if (s) printf("%-8s font_px=%d disp=%d color=%06x bold=%d mt=%d ml=%d w=%d(%d)\n",
                      n->tag, s->font_px, s->display, s->color, s->bold, s->mt, s->ml, s->width, s->has_w);
        else   printf("%-8s (no style)\n", n->tag);
    }
    for (struct node *c = n->first_child; c; c = c->next) dump(c, d + 1);
}
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1); fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);
    struct node *root = dom_parse(buf, (int)sz);
    css_init();
    static char css[16384]; int n = collect_style(root, css, 0, sizeof css); css[n] = 0;
    printf("=== author CSS (%d bytes): %.200s\n", n, css);
    css_apply(root, css, n);
    dump(root, 0);
    return 0;
}
