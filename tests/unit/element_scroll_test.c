#define main cssom_original_main
#include "cssom_test.c"
#undef main
#include "logit.h"
#include "browser_paint.h"
struct paintop paint_ops[PAINT_MAXOPS];int paint_nops;
void img_register(img_detect_fn d,img_decode_fn c){(void)d;(void)c;}
static void settle(void){css_apply(g_root,0,0);css_extra_apply(g_root,0,0);layout_page(g_root,400);js_dom_clear_dirty();}
static int colored_y(unsigned col){for(int i=0;i<paint_nops;i++)if((paint_ops[i].kind==OP_RECT || (paint_ops[i].kind==OP_BLIT && paint_ops[i].solid)) && paint_ops[i].color==col)return paint_ops[i].y;return -999;}
int main(void){
 const char *html="<!doctype html><html><body style='margin:0'><div id='s' style='width:120px;height:80px;overflow:auto;background:#112233'><div id='pad' style='height:100px'></div><div id='target' style='height:40px;background:#ee2244'>TARGET</div><div style='height:100px'></div></div><div id='outside' style='height:80px;background:#55aa77'>OUT</div><div id='b' style='width:120px;height:80px;overflow:auto'><div style='height:60px'></div><div id='inner' style='height:80px;overflow:auto'><div id='leaf' style='height:200px;background:#abcdef'></div></div><div style='height:60px'></div></div></body></html>";
 css_init();g_root=dom_parse(html,strlen(html));css_viewport(400,300);settle();js_page_set_clock(clk);js_page_open(g_root);ctx=js_page_ctx();js_cssom_set_reflow(settle);
 {struct node *n=dom_get_element_by_id(g_root->doc,"s");int x,y,w,h,sw,sh;layout_node_box(n,&x,&y,&w,&h);layout_node_scroll(n,&sw,&sh);printf("APPARATUS: items=%d box=%d,%d,%d,%d overflow=%d,%d css-overflow=%d style=%s\n",layout_count(),x,y,w,h,sw,sh,n->style?((struct cstyle *)n->style)->overflow_y:-1,dom_attr(n,"style"));}
 CK(eq("var s=document.getElementById('s'),t=document.getElementById('target'),events=0;s.addEventListener('scroll',function(){events++});var y=t.getBoundingClientRect().top;s.scrollTop=100;y-t.getBoundingClientRect().top","100"),"element scroll moves client geometry");
 CK(eq("events","0"),"scroll event waits for rendering step");
 CK(js_cssom_dispatch_element_scroll()==1,"native rendering step observes scroll");
 CK(eq("events","1"),"element scroll event delivered once");
 CK(js_cssom_dispatch_element_scroll()==0,"unchanged scroll emits no event");
 CK(eq("document.elementFromPoint(10,10)===t","true"),"CSSOM hit follows scrolled descendant");
 struct node *target=dom_get_element_by_id(g_root->doc,"target"),*scroller=dom_get_element_by_id(g_root->doc,"s"),*hit=0;
 browser_hittest_node_scroll(10,10,0,0,&hit,0,0);CK(hit==target,"trusted hit follows scrolled descendant");
 paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);
 CK(colored_y(0xee2244)==0,"real painter moves descendant to visible origin");
 CK(colored_y(0x112233)==0,"scroller background stays stationary");
 CK(eq("(s.scrollTop=1e9,s.scrollTop===s.scrollHeight-s.clientHeight)","true"),"scroll clamp uses actual layout overflow");
 CK(eq("(s.scrollTop=Infinity,s.scrollTop)","0"),"nonfinite scroll normalizes before integer cast");
 CK(js_cssom_scroll_element_by(target,0,40)==1,"native wheel consumes inner scroller");
 CK(eq("s.scrollTop","40"),"wheel and JS share one scroll offset");
 CK(eq("(s.scrollTop=0,s.scrollTop=20,s.scrollTop=30,events)","1"),"multiple writes remain queued");js_cssom_dispatch_element_scroll();CK(eq("events","2"),"multiple writes coalesce");
 CK(eq("var b=document.getElementById('b'),inner=document.getElementById('inner'),leaf=document.getElementById('leaf');var ly=leaf.getBoundingClientRect().top;b.scrollTop=50;inner.scrollTop=40;ly-leaf.getBoundingClientRect().top","90"),"nested client geometry sums both scrollers");
 {struct node *leaf=dom_get_element_by_id(g_root->doc,"leaf"),*b=dom_get_element_by_id(g_root->doc,"b");int bx,by,bw,bh;layout_node_box(b,&bx,&by,&bw,&bh);const struct item *it=layout_items();int found=0;for(int i=0;i<layout_count();i++)if(it[i].node==leaf && it[i].type==IT_RECT){struct item e=it[i];js_cssom_project_item(&e);CK(e.y==by-30 && e.clip_y==by+10 && e.clip_h==70,"nested clips move inner edge and retain outer edge");found=1;break;}CK(found,"nested clip assertion reached actual paint item");}
 js_cssom_dispatch_element_scroll();
 CK(eq("var order='';s.addEventListener('scroll',function(){order+='s'});b.addEventListener('scroll',function(){order+='b'});b.scrollTop=40;s.scrollTop=40;order",""),"scroll dispatch remains asynchronous");js_cssom_dispatch_element_scroll();CK(eq("order","bs"),"scroll event order follows first enqueue not slot creation");
 CK(eq("order='';s.addEventListener('scroll',function once(){s.removeEventListener('scroll',once);b.scrollTop=30});s.scrollTop=50;true","true"),"listener creates another scroll");js_cssom_dispatch_element_scroll();CK(eq("order","s"),"listener-created scroll waits next rendering step");CK(js_cssom_element_scroll_pending(),"listener-created scroll keeps idle loop awake");js_cssom_dispatch_element_scroll();CK(eq("order","sb"),"next rendering step delivers queued scroll");
 CK(eq("(s.scrollTop=100,t.scrollIntoView(),s.scrollTop)","100"),"repeated scrollIntoView uses current projected position");
 CK(eq("(s.style.height='500px',s.getBoundingClientRect().height,s.scrollTop)","0"),"resize reconciles previously legal scroll offset");
 CK(eq("(s.scrollTop=40,s.remove(),true)","true"),"remove pending scroller");CK(js_cssom_dispatch_element_scroll()==0,"removed pending target is safely discarded");
 js_page_close();layout_free();dom_free(g_root);printf("element-scroll: %s (%d checks)\n",fails?"FAIL":"PASS",checks);return fails?1:0;
}
