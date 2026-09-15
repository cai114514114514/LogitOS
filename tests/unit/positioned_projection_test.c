/* Real painter + native hit + JS CSSOM consumer of fixed geometry. A layout
 * box alone remains unscrolled under either implementation and cannot prove
 * any of these presentation boundaries. */
#define main cssom_original_main
#include "cssom_test.c"
#undef main
#include "logit.h"
#include "browser_paint.h"
#include "focus.h"
#include "top_layer.h"
struct paintop paint_ops[PAINT_MAXOPS];int paint_nops;
void img_register(img_detect_fn d,img_decode_fn c){(void)d;(void)c;}
static int color_y(unsigned color){for(int i=0;i<paint_nops;i++)if((paint_ops[i].kind==OP_RECT||(paint_ops[i].kind==OP_BLIT&&paint_ops[i].solid))&&paint_ops[i].color==color)return paint_ops[i].y;return -999;}
static void project_reflow(void){css_apply(g_root,0,0);css_extra_apply(g_root,0,0);layout_page(g_root,400);js_dom_clear_dirty();}
int main(void){
 css_init();css_viewport(400,300);
 const char *html="<body style='margin:0;width:1000px'><div id='box' style='position:fixed;top:20%;left:0;right:0;margin:auto;width:200px;height:52px;z-index:3'><form style='display:flex;align-items:center;width:100%;height:100%;background:#ee2244'><input id='query' style='width:190px;height:28px;padding:0;border:0;background:transparent'></form></div><a id='nav' href='/videos' style='position:fixed;left:100px;top:0;width:100px;height:39px;z-index:4;background:#112233'>Videos</a><div style='height:400px'></div><div id='flow' style='height:40px;background:#228844'></div><div style='height:1000px;width:1000px'></div></body>";
 g_root=dom_parse(html,strlen(html));project_reflow();js_page_set_clock(clk);js_page_open(g_root);ctx=js_page_ctx();js_cssom_set_reflow(project_reflow);
 CK(eq("document.getElementById('query').getBoundingClientRect().y","72"),"fixed percent input geometry is below navigation");
 CK(eq("document.elementFromPoint(150,80).id","query"),"actual elementFromPoint on search area excludes navigation");
 CK(eq("document.elementFromPoint(150,20).id","nav"),"navigation remains independently targetable");
 struct node *hit=0;char href[40];browser_hittest_node(150,80,0,&hit,href,sizeof href);
 CK(hit==dom_get_element_by_id(g_root->doc,"query")&&!href[0],"native point on control does not navigate underlying link");
 paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);CK(color_y(0xee2244)==60,"fixed background paints at percentage top before scroll");
 CK(eq("window.scrollTo(50,100);[scrollX,scrollY].join(',')","50,100"),"real page scroll changes both axes");
 CK(eq("var r=document.getElementById('query').getBoundingClientRect();[r.x,r.y].join(',')","100,72"),"fixed CSSOM rect ignores both page scroll axes");
 CK(eq("document.getElementById('query').getClientRects()[0].y","72"),"fixed CSSOM fragment ignores page scroll");
 CK(eq("document.getElementById('flow').getBoundingClientRect().y","300"),"ordinary flow CSSOM still subtracts page scroll");
 CK(eq("document.elementFromPoint(150,80).id","query"),"fixed elementFromPoint still hits control after scroll");
 browser_hittest_node_scroll(150,80,50,100,&hit,href,sizeof href);CK(hit==dom_get_element_by_id(g_root->doc,"query")&&!href[0],"native point remains on fixed control after page scroll");
 paint_nops=0;browser_paint_scroll(0,0,400,300,50,100);CK(color_y(0xee2244)==60,"real painter keeps fixed background after page scroll");
 CK(eq("document.getElementById('query').scrollIntoView();[scrollX,scrollY].join(',')","50,100"),"scrollIntoView on fixed control preserves the page position");
 js_page_close();layout_free();dom_free(g_root);
 /* A promoted dialog escapes an intervening DOM scroller even when a
  * more distant ancestor happens to be fixed. Exercise the real top layer. */
 const char *modal="<body style='margin:0'><div style='position:fixed;left:0;top:0;width:200px;height:200px'><div id='outer' style='overflow:auto;height:50px'><div style='height:200px'></div><dialog open id='dialog' style='width:100px;height:100px;margin:0;padding:0;border:0'><div id='modalchild' style='height:40px;background:#aa6633'>Child</div></dialog><div style='height:200px'></div></div></div></body>";
 g_root=dom_parse(modal,strlen(modal));project_reflow();
 struct node *dialog=dom_get_element_by_id(g_root->doc,"dialog");
 CK(top_layer_push(dialog),"real top-layer dialog is promoted inside fixed ancestor");
 project_reflow();js_page_open(g_root);ctx=js_page_ctx();js_cssom_set_reflow(project_reflow);
 CK(eq("var outer=document.getElementById('outer'),child=document.getElementById('modalchild');var before=child.getBoundingClientRect().y;outer.scrollTop=40;[outer.scrollTop,child.getBoundingClientRect().y-before].join(',')","40,0"),"nearer top layer escapes outer fixed panel scroller");
 CK(eq("child.scrollIntoView();outer.scrollTop","40"),"top-layer scrollIntoView does not move outer fixed panel scroller");
 CK(eq("document.elementFromPoint(155,105).id","modalchild"),"top-layer hit ignores intervening outer scroll clip");
 paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);
 CK(color_y(0xaa6633)==100,"top-layer paint ignores intervening outer scroll clip");
 js_page_close();top_layer_reset();focus_reset();layout_free();dom_free(g_root);
 printf("positioned-projection: %s (%d checks)\n",fails?"FAIL":"PASS",checks);return fails?1:0;
}
