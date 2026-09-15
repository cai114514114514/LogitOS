/* Ordinary computed transforms resolve through the actual CSSOM/layout path. */
#define main cssom_original_main
#include "cssom_test.c"
#undef main
static void line_check(const char *label,const char *expr)
{
 char *s=evalstr(expr);checks++;
 if(!s||strcmp(s,"true")){printf("FAIL: %s\n",label);fails++;
 }free(s);
}
static void line_reflow(void)
{
 css_apply(g_root,g_sheet,g_sheetlen);css_extra_apply(g_root,g_sheet,g_sheetlen);
 layout_page(g_root,800);
 /* The embedder consumes the dirty episode after its reflow. */
 js_dom_clear_dirty();
}
int main(void)
{
 const char *html="<html><body><div id=target></div><div id=hidden></div><div id=px></div></body></html>";
 g_root=dom_parse(html,(int)strlen(html));css_init();css_viewport(800,600);
 strcpy(g_sheet,"html{font-size:24px;line-height:60px}#target{width:100px;height:40px;font-size:20px;line-height:40px;transform:translate(1lh,1rlh)}#hidden{display:none;line-height:40px;transform:translate(25%,1lh)}#px{width:100px;height:40px;transform:translate(40px,60px)}");g_sheetlen=(int)strlen(g_sheet);
 line_reflow();js_page_set_clock(clk);if(!js_page_open(g_root))return 2;ctx=js_page_ctx();js_cssom_set_reflow(line_reflow);
 line_check("computed line units match absolute reference","(()=>{let a=new DOMMatrix(getComputedStyle(document.getElementById('target')).transform),b=new DOMMatrix(getComputedStyle(document.getElementById('px')).transform);return a.m41===40&&a.m42===60&&a.m41===b.m41&&a.m42===b.m42})()");
 line_check("same-turn element line mutation updates matrix","(()=>{let e=document.getElementById('target');e.style.lineHeight='80px';let m=new DOMMatrix(getComputedStyle(e).transform);return m.m41===80&&m.m42===60})()");
 line_check("same-turn root line mutation updates matrix","(()=>{document.documentElement.style.lineHeight='90px';let m=new DOMMatrix(getComputedStyle(document.getElementById('target')).transform);return m.m41===80&&m.m42===90})()");
 line_check("display-none serializes line lengths but keeps percent","(()=>{let s=getComputedStyle(document.getElementById('hidden')).transform;return s.indexOf('25%')>=0&&s.indexOf('40px')>=0})()");
 line_check("zero used line height is a valid context","(()=>{let e=document.getElementById('target');e.style.lineHeight='0';document.documentElement.style.lineHeight='0';let m=new DOMMatrix(getComputedStyle(e).transform);return m.m41===0&&m.m42===0})()");
 line_check("normal lines retain existing used strut metrics","(()=>{let e=document.getElementById('target');e.style.lineHeight='normal';document.documentElement.style.lineHeight='normal';let m=new DOMMatrix(getComputedStyle(e).transform);return m.m41===25&&m.m42===30})()");
 line_check("CSS supports validates line unit grammar","CSS.supports('transform','translate(1lh,1rlh)')&&!CSS.supports('transform','translateX(1foolh)')");
 line_check("context-free DOMMatrix keeps rejecting relative units","['translateX(1lh)','translateY(1rlh)'].every(s=>{try{new DOMMatrix(s);return false}catch(e){return e.name==='SyntaxError'}})");
 line_check("absolute DOMMatrix remains usable","(()=>{let m=new DOMMatrix('translate(40px,60px)');return m.m41===40&&m.m42===60})()");
 js_page_close();dom_free(g_root);css_init();printf("transform-lineheight-cssom: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
