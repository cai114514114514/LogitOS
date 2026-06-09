/* ASan/UBSan stress harness for the ring-3 render pipeline (dom + css_engine +
 * layout + LibCSS). Feeds real (and truncated-to-64KB, matching the browser's
 * bodybuf) HTML through the full pipeline repeatedly; host malloc gets ASan
 * redzones so any heap overflow/use-after-free is caught at its exact source.
 *
 * Build: clang -O1 -g -fsanitize=address,undefined -DWITHOUT_ICONV_FILTER \
 *   -Isrc/apps/browser -Isrc/lib/image -I<libcss includes> \
 *   tools/t/pipeline_stress.c src/apps/browser/{css_engine,dom,layout}.c \
 *   $(find third_party/css -name '*.c' ! -name css_property_parser_gen.c) -o /tmp/pstress
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dom.h"
#include "css.h"
#include "layout.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
int   text_measure(const char *s, int len, int px, int mono) { (void)s; (void)mono; return len * (px > 1 ? px / 2 : 1); }
int   res_fetch(const char *u, uint8_t **b, int *l) { (void)u; (void)b; (void)l; return -1; }
void  img_free(struct image *o) { (void)o; }
int   img_decode(const uint8_t *p, int n, struct image *o) { (void)p; (void)n; (void)o; return -1; }

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

static char *slurp(const char *path, int *len)
{
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(sz + 1); fread(b, 1, sz, f); b[sz] = 0; fclose(f); *len = (int)sz; return b;
}

int main(int argc, char **argv)
{
    css_init();
    for (int a = 1; a < argc; a++) {
        int len; char *html = slurp(argv[a], &len);
        if (!html) { printf("skip %s\n", argv[a]); continue; }
        if (len > 65535) len = 65535;                 /* match browser bodybuf truncation */
        for (int rep = 0; rep < 3; rep++) {           /* repeat to catch cross-load issues */
            struct node *root = dom_parse(html, len);
            if (!root) { printf("%s: parse NULL\n", argv[a]); break; }
            static char css[16384];
            int cl = collect_style(root, css, 0, sizeof css); css[cl] = 0;
            css_apply(root, css, cl);
            layout_page(root, 760);
            layout_load_images(0);
            printf("%s rep%d: items=%d h=%d\n", argv[a], rep, layout_count(), layout_height());
            layout_free();
            dom_free(root);
        }
        free(html);
    }
    printf("PIPELINE STRESS DONE\n");
    return 0;
}
