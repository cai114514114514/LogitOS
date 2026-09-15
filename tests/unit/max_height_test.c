#define main cssom_original_main
#include "cssom_test.c"
#undef main
#include "logit.h"
#include "browser_paint.h"
struct paintop paint_ops[PAINT_MAXOPS];int paint_nops;
void img_register(img_detect_fn d,img_decode_fn c){(void)d;(void)c;}
static void settle_max(void){css_apply(g_root,0,0);css_extra_apply(g_root,0,0);layout_page(g_root,400);js_dom_clear_dirty();}
static int max_colored(unsigned col){for(int i=0;i<paint_nops;i++)if((paint_ops[i].kind==OP_RECT||(paint_ops[i].kind==OP_BLIT&&paint_ops[i].solid))&&paint_ops[i].color==col)return paint_ops[i].y;return -999;}
int main(void){
 const char *html="<body style='margin:0'><div id='s' style='width:120px;max-height:80px;overflow:auto;border:2px solid black;background:#112233'><div style='height:80px'></div><div id='target' style='height:80px;background:#ee2244'>TARGET</div><div style='height:80px'></div></div><div id='short' style='max-height:80px;overflow:hidden'><div id='shortchild' style='height:40px;background:#ddeeff'></div></div><div id='minimum' style='min-height:100px;max-height:80px;overflow:hidden'><div id='minchild' style='height:120px;background:#ddffaa'></div></div><div id='borderbox' style='box-sizing:border-box;max-height:80px;border:2px solid black;padding:4px;overflow:auto'><div style='height:120px'></div></div><div id='fixed' style='height:50px;max-height:80px;overflow:auto'><div style='height:120px'></div></div><div id='flex' style='display:flex;max-height:80px;overflow:auto'><div style='height:120px'>FLEX</div></div><div id='grid' style='display:grid;grid-template-columns:1fr;max-height:80px;overflow:auto'><div style='height:120px'>GRID</div></div></body>";
 css_init();g_root=dom_parse(html,strlen(html));css_viewport(400,1000);settle_max();js_page_set_clock(clk);js_page_open(g_root);ctx=js_page_ctx();js_cssom_set_reflow(settle_max);
 CK(eq("var s=document.getElementById('s'),t=document.getElementById('target');s.getBoundingClientRect().height","84"),"auto max-height limits border box");
 CK(eq("s.clientHeight","80"),"auto max-height sets client viewport");
 CK(eq("s.scrollHeight","240"),"auto max-height preserves scroll extent");
 CK(eq("document.getElementById('short').getBoundingClientRect().height","40"),"short content keeps natural height");
 CK(eq("document.getElementById('minimum').getBoundingClientRect().height","100"),"min-height wins over max-height");
 CK(eq("document.getElementById('borderbox').getBoundingClientRect().height","80"),"max-height respects border-box sizing");
 CK(eq("document.getElementById('fixed').getBoundingClientRect().height","50"),"specified smaller height stays unchanged");
 CK(eq("document.getElementById('flex').getBoundingClientRect().height","80"),"flex container max-height limits viewport");
 CK(eq("document.getElementById('grid').getBoundingClientRect().height","80"),"grid container max-height limits viewport");
 const struct item *items=layout_items();struct node *t=dom_get_element_by_id(g_root->doc,"target");int bounded=0;
 for(int i=0;i<layout_count();i++)if(items[i].node==t&&items[i].type==IT_RECT)bounded=items[i].has_clip&&items[i].clip_h==80;
 CK(bounded,"auto max-height stamps final descendant clip");
 struct node *shortchild=dom_get_element_by_id(g_root->doc,"shortchild"),*minchild=dom_get_element_by_id(g_root->doc,"minchild");int shortclip=0,minclip=0;
 for(int i=0;i<layout_count();i++)if(items[i].type==IT_RECT){if(items[i].node==shortchild)shortclip=items[i].has_clip&&items[i].clip_h==40;if(items[i].node==minchild)minclip=items[i].has_clip&&items[i].clip_h==100;}
 CK(shortclip,"short content clip uses natural height rather than maximum");
 CK(minclip,"clip uses min-height when minimum exceeds maximum");
 CK(eq("document.getElementById('borderbox').clientHeight","76"),"padding box clip excludes border width");
 paint_nops=0;browser_paint_scroll(0,0,400,1000,0,0);CK(max_colored(0xee2244)==-999,"real painter clips overflowing target before scrolling");
 CK(eq("(s.scrollTop=80,s.scrollTop)","80"),"auto max-height has actual scroll range");
 paint_nops=0;browser_paint_scroll(0,0,400,1000,0,0);CK(max_colored(0xee2244)==2,"real painter reveals target after scrolling");
 CK(max_colored(0x112233)==0,"auto max-height background stays stationary");
 CK(eq("document.elementFromPoint(10,10)===t","true"),"scroll hit uses max-height viewport");
 js_page_close();layout_free();dom_free(g_root);printf("max-height: %s (%d checks)\n",fails?"FAIL":"PASS",checks);return fails?1:0;
}
