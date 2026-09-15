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

static int text_op(const char *word) {
    int len=(int)strlen(word);
    for(int i=0;i<paint_nops;i++)
        if(paint_ops[i].kind==OP_TEXT && paint_ops[i].len==len &&
           !memcmp(paint_ops[i].text,word,len)) return 1;
    return 0;
}
static void hitpage(const char *html, const char *extra, int width) {
    char css[4096];
    snprintf(css,sizeof css,"body{margin:0;font-size:16px;line-height:24px}a{color:blue}%s",extra?extra:"");
    struct node *root=dom_parse(html,(int)strlen(html));
    css_apply(root,css,(int)strlen(css)); css_extra_apply(root,css,(int)strlen(css));
    layout_page(root,width);
}
static int linkat(int x,int y) {
    char href[80]; struct node *node=0;
    browser_hittest_node(x,y,0,&node,href,sizeof href);
    return !strcmp(href,"/target");
}
int main(void) {
    hitpage("<a href=/target>json next</a>","",400);
    CHECK(linkat(12,8),"control: glyph navigates");
    CHECK(linkat(36,8),"plain collapsed space navigates");
    hitpage("<a href=/target><code>json</code> next</a>","code{background:#eee;padding:0 1px}",400);
    CHECK(linkat(38,8),"space after nested code navigates");
    hitpage("<a href=/target><span>json next</span></a>","",400);
    CHECK(linkat(36,8),"unpainted nested span space navigates");
    hitpage("<a href=/target>  json next  </a>","",400);
    CHECK(linkat(4,8),"collapsed leading space does not indent glyph");
    CHECK(!linkat(76,8),"collapsed trailing space does not create a box");
    hitpage("x <a href=/target>json next</a>","",400);
    CHECK(!linkat(12,8),"outside leading space stays outside anchor");
    CHECK(linkat(52,8),"inside space after external debt navigates");
    hitpage("<a href=/target>json </a>next","",400);
    CHECK(linkat(36,8),"rendered trailing space keeps its source anchor");
    hitpage("<a href=/target>json next more</a>","",80);
    CHECK(linkat(36,8),"first wrapped line space navigates");
    CHECK(!linkat(76,8),"wrapped line unused end is not clickable");
    CHECK(!linkat(36,32),"last line unused end is not clickable");
    hitpage("<a href=/target>json next</a>","a{white-space:pre}",400);
    CHECK(linkat(36,8),"preserved space still navigates");
    hitpage("<a href=/target>json\tnext</a>","a{white-space:pre}",400);
    CHECK(linkat(44,8),"preserved tab hit region navigates");
    hitpage("<div><a href=/target>json next</a></div>","div{width:36px;overflow:hidden}a{white-space:nowrap}",400);
    CHECK(!linkat(38,8),"clipped half of space is not clickable");
    CHECK(linkat(34,8),"visible half of clipped space navigates");
    hitpage("<a href=/target>json next</a><div></div>","div{position:absolute;left:32px;top:0;width:8px;height:24px;background:red;z-index:5}",400);
    CHECK(!linkat(36,8),"higher nonlink overlay blocks link navigation");
    { char h[80]; CHECK(!browser_hittest(36,8,0,h,sizeof h),"legacy link query also respects overlay"); }
    hitpage("<a href=/target>json next</a><div></div>","div{position:absolute;left:0;top:0;width:24px;height:24px;background:red;z-index:5}",400);
    CHECK(!linkat(12,8),"higher nonlink overlay blocks painted glyph too");
    hitpage("<a href=/target>json next</a>","a{visibility:hidden}",400);
    CHECK(!linkat(36,8),"hidden whitespace cannot navigate");
    hitpage("<a href=/target>json next</a>","",400);
    paint_nops=0; browser_paint(0,0,400,100,0);
    CHECK(text_op("json") && text_op("next"),"hit regions leave actual text painting intact");
    hitpage("<a href=/target>json next</a>","body{text-align:center}",400);
    CHECK(linkat(200,8),"center alignment moves the space with its words");
    hitpage("<a href=/target>json next more</a>","body{text-align:justify}",100);
    CHECK(linkat(50,8),"justification expands actual space hit region");
    printf("inline-hit: %s\n",fail?"FAIL":"PASS"); return fail;
}
