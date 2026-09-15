/* Reduced from an opacity-hidden overlay whose descendants have opaque
 * computed colors. This runs the actual painter; geometry alone cannot detect
 * the leak because opacity must preserve layout. No partial-alpha compositing
 * accuracy is claimed by this zero-alpha gate. */
#define main cssom_original_main
#include "cssom_test.c"
#undef main
#include "logit.h"
#include "browser_paint.h"
struct paintop paint_ops[PAINT_MAXOPS];int paint_nops;
void img_register(img_detect_fn d,img_decode_fn c){(void)d;(void)c;}
static int max_colored(unsigned col){for(int i=0;i<paint_nops;i++)if((paint_ops[i].kind==OP_RECT||(paint_ops[i].kind==OP_BLIT&&paint_ops[i].solid))&&paint_ops[i].color==col)return paint_ops[i].y;return -999;}
static void opacity_reflow(void){css_apply(g_root,0,0);css_extra_apply(g_root,0,0);layout_page(g_root,400);js_dom_clear_dirty();}
static void opacity_page(const char *html) {
 g_root=dom_parse(html,strlen(html));css_viewport(400,600);css_apply(g_root,0,0);css_extra_apply(g_root,0,0);layout_page(g_root,400);
 paint_nops=0;browser_paint_scroll(0,0,400,600,0,0);
}
static void opacity_close(void){layout_free();dom_free(g_root);}
int main(void){
 css_init();
 opacity_page("<body style='margin:0'><div id='group' style='opacity:0'><div id='child' style='height:50px;background:#ee2244'>SECRET</div></div><div style='height:10px;background:#112233'></div></body>");
 CK(max_colored(0xee2244)==-999,"zero-opacity ancestor suppresses descendant ink");
 CK(max_colored(0x112233)==50,"zero-opacity ancestor preserves flow height");
 struct node *hit=0;char href[32];browser_hittest_node(10,30,0,&hit,href,sizeof href);
 CK(hit==dom_get_element_by_id(g_root->doc,"child"),"transparent group descendants remain pointer targets");
 js_page_set_clock(clk);js_page_open(g_root);ctx=js_page_ctx();js_cssom_set_reflow(opacity_reflow);
 CK(eq("(document.getElementById('group').style.opacity='1',document.getElementById('child').getBoundingClientRect().height)","50"),"CSSOM opacity change keeps child geometry");
 paint_nops=0;browser_paint_scroll(0,0,400,600,0,0);
 CK(max_colored(0xee2244)==0,"zero to one CSSOM transition restores descendant ink");
 CK(eq("(document.getElementById('group').style.opacity='0',document.getElementById('child').getBoundingClientRect().height)","50"),"CSSOM return to zero keeps child geometry");
 paint_nops=0;browser_paint_scroll(0,0,400,600,0,0);
 CK(max_colored(0xee2244)==-999,"one to zero CSSOM transition hides descendant ink again");
 js_page_close();
 opacity_close();
 opacity_page("<body style='margin:0'><div style='opacity:0;transition:opacity .2s'><div style='position:fixed;top:0;width:100px;height:50px;background:#ee2244'>OVERLAY</div></div></body>");
 CK(max_colored(0xee2244)==-999,"opacity transition parent suppresses fixed descendant ink");
 opacity_close();
 opacity_page("<body style='margin:0'><div style='opacity:0'><div style='opacity:1;visibility:visible;height:50px;background:#ee2244'></div></div></body>");
 CK(max_colored(0xee2244)==-999,"opaque child cannot escape transparent group");
 opacity_close();
 opacity_page("<body style='margin:0'><div style='visibility:hidden'><div style='visibility:visible;height:50px;background:#ee2244'></div></div></body>");
 CK(max_colored(0xee2244)==0,"visibility visible child can override inherited hidden");
 opacity_close();
 opacity_page("<body style='margin:0'><div style='opacity:1'><div style='height:50px;background:#ee2244'></div></div></body>");
 CK(max_colored(0xee2244)==0,"opaque group paints normally");
 opacity_close();
 opacity_page("<body style='margin:0'><div style='display:contents;opacity:0'><div style='height:50px;background:#ee2244'></div></div></body>");
 CK(max_colored(0xee2244)==0,"boxless contents wrapper creates no opacity group");opacity_close();
 printf("opacity-group: %s (%d checks)\n",fails?"FAIL":"PASS",checks);return fails?1:0;
}
