/* The layout-only gate cannot see CSSOM's ink-union fallback. This companion
 * executes the same DOM API that failed in the guest navigation fixture. */
#define main cssom_original_main
#include "cssom_test.c"
#undef main
static void contents_reflow(void) {
 css_apply(g_root,0,0);css_extra_apply(g_root,0,0);layout_page(g_root,400);js_dom_clear_dirty();
}
int main(void) {
 css_init();css_viewport(400,600);
 const char *html="<body style='margin:0'><nav style='display:flex'><div id='wrap' style='display:contents'><div style='display:inherit'><div id='a' style='width:80px;height:40px;background:red'></div><div id='b' style='width:80px;height:40px;background:blue'></div></div></div><div id='c' style='width:80px;height:40px;background:green'></div></nav><div style='height:2000px'></div></body>";
 g_root=dom_parse(html,strlen(html));contents_reflow();js_page_set_clock(clk);js_page_open(g_root);ctx=js_page_ctx();js_cssom_set_reflow(contents_reflow);
 CK(eq("var wrap=document.getElementById('wrap');var a=document.getElementById('a');var b=document.getElementById('b');var c=document.getElementById('c');[b.getBoundingClientRect().x-a.getBoundingClientRect().x,c.getBoundingClientRect().x-a.getBoundingClientRect().x].join(',')","80,160"),"contents children retain flex geometry");
 CK(eq("var r=wrap.getBoundingClientRect();[r.x,r.y,r.width,r.height].join(',')","0,0,0,0"),"contents bounding rect is zero not child union");
 CK(eq("wrap.getClientRects().length","0"),"contents fragment list is empty");
 CK(eq("String(wrap.getClientRects().item(0))","null"),"empty contents rect list retains item method");
 CK(eq("window.scrollTo(0,100);window.scrollY","100"),"viewport scroll control actually moves");
 CK(eq("r=wrap.getBoundingClientRect();[r.x,r.y,r.width,r.height].join(',')","0,0,0,0"),"boxless rect origin remains zero after scroll");
 CK(eq("a.getBoundingClientRect().y","-100"),"real child rect follows viewport scroll");
 CK(eq("wrap.style.display='block';wrap.getBoundingClientRect().width>0","true"),"contents to block restores own geometry");
 CK(eq("wrap.style.display='contents';wrap.getClientRects().length","0"),"block to contents removes rect fragments");
 js_page_close();layout_free();dom_free(g_root);
 printf("display-contents-cssom: %s (%d checks)\n",fails?"FAIL":"PASS",checks);return fails?1:0;
}
