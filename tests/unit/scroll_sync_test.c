/* Shared CSSOM gate setup; the live layout and QuickJS implementation are
 * linked, while only the browser window's scroll callback is a host seam.
 * Actual guest mouse/paint motion remains the integration gate. */
#define main cssom_existing_main
#include "cssom_test.c"
#undef main

#ifndef SCROLL_SYNC_BEFORE
static int embed_x,embed_y,embed_calls;
static void embed_scroll(int x,int y) {
    embed_calls++;
    if(x>300)x=300; if(y>400)y=400;
    if(x<0)x=0;if(y<0)y=0;
    int changed=x!=embed_x || y!=embed_y;
    embed_x=x;embed_y=y;js_dom_set_scroll(x,y);
    /* Same contract as browser.c: clamp, publish, then one event per change. */
    if(changed){struct js_event_init e={0};js_dom_dispatch(g_root,"scroll",&e);}
}
#endif
int main(void) {
    const char *html="<!doctype html><body><div id=box>x</div></body>";
    const char *sheet="body{margin:0;width:1200px;height:1200px;background:white}#box{position:absolute;left:360px;top:240px;width:80px;height:40px;background:red}";
    css_init();css_viewport(400,300);
    g_root=dom_parse(html,(int)strlen(html));css_apply(g_root,sheet,(int)strlen(sheet));
    css_extra_apply(g_root,sheet,(int)strlen(sheet));layout_page(g_root,400);
    js_page_set_clock(clk);CK(js_page_open(g_root),"page opened");ctx=js_page_ctx();
    js_dom_clear_dirty();
    js_dom_set_scroll(100,80);
    CK(eq("[scrollX,scrollY,pageXOffset,pageYOffset].join(',')","100,80,100,80"),"native offset reaches all window getters");
    CK(eq("[document.documentElement.scrollLeft,document.documentElement.scrollTop].join(',')","100,80"),"root offsets read native position");
    CK(eq("(function(){var r=document.getElementById('box').getBoundingClientRect();return [r.x,r.y].join(',')})()","260,160"),"bounding rect uses native offset");
    CK(eq("(function(){var r=document.getElementById('box').getClientRects()[0];return [r.x,r.y].join(',')})()","260,160"),"client rects use native offset");
    CK(eq("document.elementFromPoint(270,170).id","box"),"elementFromPoint follows native offset");
    CK(eq("(function(){var e=new MouseEvent('click',{clientX:7,clientY:9});return [e.clientX,e.clientY,e.pageX,e.pageY].join(',')})()","7,9,107,89"),"page coordinates add scroll without changing client coordinates");
    CK(eq("(window.__p='',document.addEventListener('click',function(e){__p=[e.clientX,e.clientY,e.pageX,e.pageY].join(',')}),true)","true"),"native event listener installed");
    {struct js_event_init e={0};e.client_x=13;e.client_y=17;js_dom_dispatch(g_root,"click",&e);}
    CK(eq("__p","13,17,113,97"),"native event page coordinates include scroll");
#ifndef SCROLL_SYNC_BEFORE
    js_dom_set_scroll(0,0);embed_x=embed_y=embed_calls=0;
    js_cssom_set_scroll_handler(embed_scroll);
    CK(eq("(window.__scrolls=0,document.addEventListener('scroll',function(){__scrolls++}),true)","true"),"scroll listener installed");
    CK(eq("(scrollTo(900,900),[scrollX,scrollY,__scrolls].join(','))","300,400,1"),"JS scroll requests actual embedder clamp and one event");
    CK(embed_calls==1 && embed_x==300 && embed_y==400,"JS call moves embedder position");
    CK(eq("(scrollTo(900,900),__scrolls)","1"),"unchanged clamped offset does not redispatch");
    CK(eq("(scrollBy(-25,-40),[scrollX,scrollY,__scrolls].join(','))","275,360,2"),"scrollBy starts at actual position");
    CK(eq("(document.documentElement.scrollTo(20,30),[scrollX,scrollY,__scrolls].join(','))","20,30,3"),"root scrollTo updates two axes in one event");
    CK(eq("(document.body.scrollLeft=40,[scrollX,scrollY,__scrolls].join(','))","40,30,4"),"body scrollLeft uses same embedder hook");
    CK(eq("(scrollTo({top:50}),[scrollX,scrollY].join(','))","40,50"),"dictionary preserves unspecified native axis");
    CK(eq("(scrollTo(0,0),document.getElementById('box').scrollIntoView(),scrollY>0)","true"),"scrollIntoView moves actual viewport");
    CK(embed_y>0,"scrollIntoView calls embedder");
    CK(eq("(scrollTo(100,80),scrollBy(Infinity,NaN),[scrollX,scrollY].join(','))","100,80"),"nonfinite relative deltas normalize before addition");
    CK(eq("(scrollTo(Infinity,-Infinity),[scrollX,scrollY].join(','))","0,0"),"nonfinite absolute requests normalize to zero");
    CK(eq("(window.__frozen='',document.addEventListener('mousemove',function(e){scrollTo(80,90);__frozen=[e.pageX,e.pageY].join(',')}),document.dispatchEvent(new MouseEvent('mousemove',{clientX:7,clientY:9})),__frozen)","7,9"),"scroll inside a handler does not move the dispatched event position");
        js_cssom_set_scroll_handler(0);
    js_dom_set_scroll(0,0);
    /* The standalone doc_size fallback sees painted right edge 440, hence
     * only 40px of horizontal range. Use a request INSIDE that known range;
     * the hooked cases above deliberately prove the real owner can exceed it. */
    CK(eq("(scrollTo(20,80),[scrollX,scrollY].join(','))","20,80"),"standalone host fallback still scrolls");
#endif
    js_page_close();dom_free(g_root);
    printf("scroll-sync: %s (%d checks)\n",fails?"FAIL":"PASS",checks);return fails;
}
