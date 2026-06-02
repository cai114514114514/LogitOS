/* Integration test: HTML -> DOM -> computed CSS -> layout display list, with an
 * author <style> block, a styled background box, and an <img>. Mirrors the
 * kernel pipeline in wm.c (collect <style> text, css_apply, layout_page). */
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
int img_decode(const uint8_t *p, int n, struct image *o){ (void)p;(void)n;(void)o; return -1; }

/* same as wm.c collect_style */
static int tag_is(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }
static int collect_style(struct node *n, char *out, int o, int max){
    if(!n) return o;
    if(n->type==N_ELEM && tag_is(n->tag,"style"))
        for(struct node *c=n->first_child;c;c=c->next)
            if(c->type==N_TEXT && c->text)
                for(int i=0;i<c->textlen && o<max-1;i++) out[o++]=c->text[i];
    for(struct node *c=n->first_child;c;c=c->next) o=collect_style(c,out,o,max);
    return o;
}

static int fail;
#define CHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m);fail=1;} else printf("ok: %s\n",m);}while(0)

int main(void){
    const char *html =
        "<style>.box{background:#eef0ff;width:300px;padding:10px}</style>"
        "<body><div class='box'>"
        "<h1>Title</h1>"
        "<p>one two three four five six seven eight nine ten eleven twelve</p>"
        "<img src='logo.png' width='40' height='30'>"
        "</div></body>";
    struct node *root = dom_parse(html, (int)strlen(html));
    CHECK(root!=NULL, "dom_parse");
    char css[4096]; int cl = collect_style(root, css, 0, sizeof css);
    printf("author css (%d bytes): %.*s\n", cl, cl, css);
    css_apply(root, css, cl);
    layout_page(root, 400);

    int n = layout_count();
    const struct item *it = layout_items();
    printf("items=%d height=%d\n", n, layout_height());

    int rect_bg=0, img=0, plines=0, ys[64], ny=0;
    for(int i=0;i<n;i++){
        if(it[i].type==IT_RECT && it[i].has_bg) rect_bg++;
        if(it[i].type==IT_IMAGE){ img++;
            CHECK(it[i].w==40 && it[i].h==30, "img sized from width/height attrs"); }
        if(it[i].type==IT_TEXT && it[i].font_px==16){
            int seen=0; for(int j=0;j<ny;j++) if(ys[j]==it[i].y) seen=1;
            if(!seen && ny<64) ys[ny++]=it[i].y;
        }
    }
    plines=ny;
    CHECK(rect_bg>=1, "styled .box produced a background rect");
    CHECK(img==1, "img produced one IMAGE item");
    CHECK(plines>=2, "paragraph wrapped to >=2 lines inside the 300px box");

    printf(fail? "\nPAGE TEST FAILED\n" : "\nPAGE TEST PASSED\n");
    return fail;
}
