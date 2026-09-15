/* Real parser/style/layout/painter hit test. Use the paint recorder and host
 * font contract, not a copied hit algorithm. Guest glyphs and mouse delivery
 * are deliberately a separate fixture gate. */
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

/* SVG links for the shared paint pipeline's colour evaluator; no decoder runs. */
void img_register(img_detect_fn d, img_decode_fn c){ (void)d; (void)c; }

static int fail;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fail=1; } else printf("ok: %s\n", msg); }while(0)

#include "focus.h"
#include "top_layer.h"
static int text_index(const char *word,int *count) {
    int at=-1;*count=0;for(int i=0;i<paint_nops;i++)if(paint_ops[i].kind==OP_TEXT&&paint_ops[i].len==(int)strlen(word)&&!memcmp(paint_ops[i].text,word,strlen(word))){at=i;(*count)++;}return at;
}
int main(void) {
    const char *html="<!doctype html><button id=outside>OUTSIDE</button><dialog open id=d><span>MODAL</span><button id=first>FIRST</button><button id=last>LAST</button></dialog><div id=page>PAGE</div>";
    const char *css="body{margin:0;font-size:16px;line-height:20px}dialog{width:180px;height:100px;border:0;padding:0;margin:0}#page{position:relative;z-index:2147483647;height:30px}#outside{display:block}";
    struct node *root=dom_parse(html,(int)strlen(html));css_viewport(500,400);
    css_apply(root,css,(int)strlen(css));css_extra_apply(root,css,(int)strlen(css));
    struct node *d=dom_get_element_by_id(root->doc,"d"),*outside=dom_get_element_by_id(root->doc,"outside"),*first=dom_get_element_by_id(root->doc,"first"),*last=dom_get_element_by_id(root->doc,"last"),*page=dom_get_element_by_id(root->doc,"page");
    focus_set(outside);CHECK(top_layer_push(d),"native modal stack accepts open connected dialog");focus_set(first);
    layout_page(root,500);int x,y,w,h;layout_node_box(page,&x,&y,&w,&h);
    CHECK(y<100,"modal is excluded from ordinary document flow");
    layout_node_box(d,&x,&y,&w,&h);printf("modal box %d %d %d %d items %d\n",x,y,w,h,layout_count());CHECK(x==160&&y==150&&w==180&&h==100,"modal box is centered against viewport");
    paint_nops=0;browser_paint_scroll(0,0,500,400,0,0);
    int cm,cp;int mi=text_index("MODAL",&cm),pi=text_index("PAGE",&cp);printf("modal painted %d page %d counts %d %d\n",mi,pi,cm,cp);
    CHECK(cm==1,"modal text paints exactly once");CHECK(mi>pi&&pi>=0,"modal paints above maximum page z index");
    int backdrop=0;for(int i=0;i<paint_nops;i++)if(paint_ops[i].kind==OP_BLIT&&paint_ops[i].solid&&paint_ops[i].w==500&&paint_ops[i].h==400&&paint_ops[i].alpha==100)backdrop++;
    CHECK(backdrop==1,"modal backdrop is a real full viewport paint operation");
    struct node *hit=0;char href[40];browser_hittest_node_scroll(2,2,80,120,&hit,href,sizeof href);CHECK(hit==d,"backdrop consumes hit instead of targeting background");
    browser_hittest_node_scroll(x+2,y+2,80,120,&hit,href,sizeof href);CHECK(top_layer_owner(hit)==d,"modal hit geometry stays fixed while page scrolls");
    focus_set(outside);CHECK(focus_current()==first,"modal rejects programmatic background focus");
    focus_advance(root,0);CHECK(focus_current()==last,"Tab advances inside modal");focus_advance(root,0);CHECK(focus_current()==first,"Tab wraps inside modal");focus_advance(root,1);CHECK(focus_current()==last,"Shift Tab wraps inside modal");
    top_layer_remove(d);CHECK(focus_current()==outside,"close restores prior focused element");
    layout_page(root,500);layout_node_box(page,&x,&y,&w,&h);CHECK(y>=100,"nonmodal open dialog remains in ordinary flow");
    const char *wide="body{margin:0}dialog{width:800px;height:100px;border:0;padding:0;margin:0}";
    css_apply(root,wide,(int)strlen(wide));top_layer_push(d);layout_page(root,500);
    CHECK(browser_content_width(root,500)==500,"oversized modal does not enlarge document scroll width");
    top_layer_reset();focus_reset();layout_free();dom_free(root);
    puts(fail?"modal paint failed":"modal paint passed");return fail?1:0;
}
