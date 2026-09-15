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

static int opx(const char *s){for(int i=0;i<paint_nops;i++)if(paint_ops[i].kind==OP_TEXT&&paint_ops[i].len==(int)strlen(s)&&!memcmp(paint_ops[i].text,s,strlen(s)))return paint_ops[i].x;return -999;}
int main(void){
 const char *html="<body><div id='a'>ab cd ef gh ij</div><div id='b'>uv wx</div><div id='c'>h<span>ello</span> world</div><div id='d'>xx<span>ab</span></div><div id='e'><span>xy zz</span></div><div id='f'><span id='sized'>AA BB</span></div><div id='scroller'><div></div><div></div><div></div></div><div id='following'>end</div></body>";
 const char *css="body{margin:0;font-size:20px;line-height:16px}#a{width:100px;text-indent:20px;text-transform:uppercase;letter-spacing:2px;word-spacing:5px}#b{word-spacing:7px}#c{text-transform:capitalize}#d span{text-transform:capitalize}#e span{background:yellow;letter-spacing:4px;text-transform:uppercase}#f{display:flex}#sized{letter-spacing:3px;word-spacing:7px}#scroller{width:120px;height:80px;overflow:auto}#scroller div{height:80px}";
 struct node *root=dom_parse(html,(int)strlen(html));css_apply(root,css,(int)strlen(css));css_extra_apply(root,css,(int)strlen(css));layout_page(root,300);
 const struct item *it=layout_items();int first=-1,second=-1;struct node *a=dom_get_element_by_id(root->doc,"a");
 for(int i=0;i<layout_count();i++)if(it[i].type==IT_TEXT&&it[i].node->parent==a){if(first<0)first=i;else if(it[i].y>it[first].y&&second<0)second=i;}
 CHECK(first>=0&&it[first].x==20,"formatter applies first-line indent");
 CHECK(second>=0&&it[second].x==0,"formatter resets indent after first line");
 CHECK(first>=0&&second>=0&&it[second].y-it[first].y==16,"formatter honors explicit sub-em line height");
 CHECK(first>=0&&it[first].text[0]=='A',"paint list contains transformed text");
 CHECK(a->first_child->text[0]=='a',"text transform leaves authored DOM unchanged");
 paint_nops=0;browser_paint_scroll(0,0,300,300,0,0);
 CHECK(opx("B")-opx("A")==12,"painter consumes letter spacing");
 CHECK(opx("D")-opx("C")==12,"spacing continues after wrapping");
 CHECK(opx("wx")-opx("uv")==37,"painter consumes word spacing");
 CHECK(opx("H")>=0&&opx("ello")>=0,"capitalize spans form one word");
 CHECK(opx("ab")>=0&&opx("Ab")==-999,"untransformed span preserves capitalization boundary");
 CHECK(opx("Y")-opx("X")==14,"decorated inline fallback paints transformed spaced glyphs");
 struct node *sized=dom_get_element_by_id(root->doc,"sized");int bx,by,bw,bh;
 layout_node_box(sized,&bx,&by,&bw,&bh);
 CHECK(bw==72,"flex intrinsic width consumes letter and word spacing");
 struct node *scroller=dom_get_element_by_id(root->doc,"scroller"),*following=dom_get_element_by_id(root->doc,"following");int sw,sh,fy;
 layout_node_box(scroller,&bx,&by,&bw,&bh);layout_node_scroll(scroller,&sw,&sh);layout_node_box(following,0,&fy,0,0);
 CHECK(bh==80,"specified height remains scroll viewport height");
 CHECK(sh==240,"overflow extent retains full descendant height");
 CHECK(fy==by+80,"following flow uses specified box height");
 layout_page(root,300);paint_nops=0;browser_paint_scroll(0,0,300,300,0,0);
 CHECK(opx("B")-opx("A")==12,"relayout replaces owned transformed text safely");
 layout_free();dom_free(root);puts(fail?"text wiring failed":"text wiring passed");return fail?1:0;
}
