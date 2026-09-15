/* Pointer exclusion is a candidate filter, not inertness or subtree culling.
 * Link the actual cascade, paint, native hit, CSSOM and focus stack. */
#define main cssom_unused_main
#include "cssom_test.c"
#undef main
#include "logit.h"
#include "browser_paint.h"
struct paintop paint_ops[PAINT_MAXOPS]; int paint_nops;
void img_register(img_detect_fn d,img_decode_fn c){(void)d;(void)c;}
static void settle(void){css_apply(g_root,g_sheet,g_sheetlen);css_extra_apply(g_root,g_sheet,g_sheetlen);layout_page(g_root,400);js_dom_clear_dirty();}
static void open_page(const char *body,const char *extra){
 char html[12000];snprintf(html,sizeof html,"<!doctype html><html><head><style>body{margin:0;font:16px sans-serif;line-height:24px}a{color:blue}%s</style></head><body>%s</body></html>",extra?extra:"",body);
 /* css_apply consumes the embedder stylesheet, not raw style nodes. Keep
  * that actual producer input across CSSOM-triggered reflows. */
 snprintf(g_sheet,sizeof g_sheet,"body{margin:0;font-size:16px;line-height:24px}a{color:blue}%s",extra?extra:"");g_sheetlen=strlen(g_sheet);
 g_root=dom_parse(html,strlen(html));css_viewport(400,300);settle();js_page_set_clock(clk);CK(js_page_open(g_root),"apparatus opens real page runtime");ctx=js_page_ctx();js_cssom_set_reflow(settle);
}
static void close_page(void){js_page_close();layout_free();dom_free(g_root);g_root=0;}
static void point(int x,int y,const char *id,const char *href,const char *label){
 /* This direct painter call stands in for the next native event, after the
  * browser's settle_frame. Specified style getters do not perform that commit;
  * relying on the later CSSOM hit query to flush made the two observations
  * describe different frames and manufactured a native-only failure. */
 settle();
 struct node *hit=0;char actual[120],msg[200],src[500];browser_hittest_node_scroll(x,y,0,0,&hit,actual,sizeof actual);
 struct node *want=id?dom_get_element_by_id(g_root->doc,id):0;
 printf("POINT %s at %d,%d native=%s href=%s\n",label,x,y,hit?dom_attr(hit,"id"):"<none>",actual);
 snprintf(msg,sizeof msg,"%s native target",label);CK(id?hit==want:!actual[0],msg);
 snprintf(msg,sizeof msg,"%s native href",label);CK(!strcmp(actual,href?href:""),msg);
 if(id)snprintf(src,sizeof src,"document.elementFromPoint(%d,%d)===document.getElementById('%s')",x,y,id);
 else snprintf(src,sizeof src,"(function(){var n=document.elementFromPoint(%d,%d);while(n){if(n.tagName==='A'&&n.getAttribute('href'))return false;n=n.parentNode}return true})()",x,y);
 snprintf(msg,sizeof msg,"%s CSSOM target",label);CK(eq(src,"true"),msg);
}
#include "focus.h"
static int painted(unsigned col){for(int i=0;i<paint_nops;i++)if((paint_ops[i].kind==OP_RECT||(paint_ops[i].kind==OP_BLIT&&paint_ops[i].solid))&&paint_ops[i].color==col)return 1;return 0;}
int main(void){css_init();
 open_page("<a id=under href=/target style='display:block;width:180px;height:80px'>Under</a><div id=cover><a id=child href=/child>Child</a><button id=key>Key</button></div>","#cover{position:absolute;left:0;top:0;width:180px;height:80px;background:#ee2244;z-index:5;pointer-events:none}#child{display:block;width:80px;height:30px}#key{width:70px;height:25px}");
 CK(eq("[CSS.supports('pointer-events','auto'),CSS.supports('pointer-events','none'),CSS.supports('(pointer-events:none)'),CSS.supports('pointer-events','visiblePainted'),CSS.supports('pointer-events','none auto')].join(',')","true,true,true,false,false"),"capability accepts exactly supported auto none grammar");
 CK(eq("'pointerEvents' in document.getElementById('cover').style","true"),"property enumeration exposes IDL accessor");
 CK(eq("getComputedStyle(document.getElementById('cover')).pointerEvents","none"),"computed none agrees with cascade");
 CK(eq("getComputedStyle(document.getElementById('child')).getPropertyValue('pointer-events')","none"),"unspecified child inherits none");
 point(120,70,"under","/target","none overlay passes through blank region");
 point(20,10,"under","/target","inherited none excludes descendant text");
 paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);CK(painted(0xee2244),"none overlay still paints its red background");
 struct node *key=dom_get_element_by_id(g_root->doc,"key");focus_set(key);CK(focus_current()==key,"pointer none control retains keyboard focus eligibility");focus_reset();focus_advance(g_root,0);focus_advance(g_root,0);CK(focus_current()==dom_get_element_by_id(g_root->doc,"child"),"sequential focus still reaches inherited none link");
 CK(eq("var child=document.getElementById('child');child.style.pointerEvents='auto';getComputedStyle(child).pointerEvents","auto"),"dynamic explicit auto overrides inherited none");point(20,10,"child","/child","auto descendant restores text and blank hit");
 CK(eq("document.elementsFromPoint(20,10).some(function(n){return n.id==='cover'})","false"),"elementsFromPoint excludes none ancestor of auto target");
 CK(eq("var n=0;document.getElementById('cover').addEventListener('click',function(){n++});child.dispatchEvent(new Event('click',{bubbles:true}));n","1"),"none ancestor remains on bubbling event path");
 CK(eq("child.style.pointerEvents='unset';getComputedStyle(child).pointerEvents","none"),"unset inherits rather than restoring initial auto");
 CK(eq("child.style.pointerEvents='initial';getComputedStyle(child).pointerEvents","auto"),"initial restores auto despite none parent");
 CK(eq("child.style.pointerEvents='inherit';getComputedStyle(child).pointerEvents","none"),"explicit inherit resolves parent keyword");
 CK(eq("child.style.pointerEvents='revert';getComputedStyle(child).pointerEvents","none"),"author revert falls through to inherited none");
 CK(eq("child.style.pointerEvents='auto';child.style.pointerEvents='bogus';getComputedStyle(child).pointerEvents","auto"),"invalid dynamic value preserves earlier declaration");
 close_page();focus_reset();
 open_page("<div id=parent style='pointer-events:none'><div id=a class=c style='pointer-events:auto'></div><div id=b class=c style='pointer-events:auto!important'></div><div id=initial></div></div><div id=supported></div><div id=invalid></div>","#a{pointer-events:none!important}.c{pointer-events:auto}#b{pointer-events:none!important}#initial{pointer-events:initial}#supported,#invalid{width:10px;height:10px}@supports(pointer-events:none){#supported{width:31px}}@supports(pointer-events:bogus){#invalid{width:99px}}");
 CK(eq("getComputedStyle(document.getElementById('a')).pointerEvents","none"),"stylesheet important beats ordinary inline auto");
 CK(eq("getComputedStyle(document.getElementById('b')).pointerEvents","auto"),"inline important beats stylesheet important none");
 CK(eq("getComputedStyle(document.getElementById('initial')).pointerEvents","auto"),"explicit initial defeats inherited none in stylesheet");
 CK(eq("document.getElementById('supported').getBoundingClientRect().width","31"),"stylesheet supports accepts supported keyword and applies body");
 CK(eq("document.getElementById('invalid').getBoundingClientRect().width","10"),"stylesheet supports rejects invalid keyword");
 close_page();
 open_page("<a id=under href=/target style='display:block;width:180px;height:80px'>Under</a><div id=cover style='pointer-events:none;display:contents'><div id=middle><a id=child href=/child>Child</a></div></div>","#middle{position:fixed;left:0;top:0;width:180px;height:80px;z-index:3}#child{display:block;width:80px;height:30px}");
 CK(eq("getComputedStyle(document.getElementById('child')).pointerEvents","none"),"inheritance crosses display contents and multiple generations");point(20,10,"under","/target","boxless none ancestor excludes inherited fixed descendant");
 CK(eq("document.getElementById('cover').style.pointerEvents='auto';getComputedStyle(document.getElementById('child')).pointerEvents","auto"),"dynamic ancestor recascade updates inherited descendants");point(20,10,"child","/child","restored inherited auto updates native and CSSOM hit");
 CK(eq("document.getElementById('middle').style.pointerEvents='none';getComputedStyle(document.getElementById('child')).pointerEvents","none"),"nearer none overrides earlier ancestor auto");
 CK(eq("document.getElementById('child').style.pointerEvents='auto';document.getElementById('middle').style.opacity='0';getComputedStyle(document.getElementById('child')).pointerEvents","auto"),"auto escape remains independent of ancestor opacity");point(20,10,"child","/child","zero opacity ancestor does not veto explicit auto target");
 close_page();
 /* A box can paint normally while every real click silently falls through:
  * an inherited none wrapper plus an explicit all dialog. This is an ordinary
  * modal, not a site/challenge fixture. Assert both native and CSSOM targets;
  * testing CSS.supports alone missed the broken user-visible interaction. */
 open_page("<a id=under href=/under style='display:block;width:400px;height:250px'>Under</a><div id=shade></div><div id=frame><div id=dialog><a id=action href=/action>Action</a><button id=key>Key</button></div></div>","#shade{position:fixed;inset:0;background:#112233;z-index:10}#frame{position:fixed;inset:0;pointer-events:none;z-index:11}#dialog{position:absolute;left:80px;top:60px;width:180px;height:100px;background:#aabbcc;pointer-events:all}#action{display:block;width:100px;height:30px}");
 CK(eq("CSS.supports('pointer-events','all')&&CSS.supports('(pointer-events:ALL)')","true"),"all keyword accepted by capability parser");
 CK(eq("getComputedStyle(document.getElementById('dialog')).pointerEvents","all"),"all computed value preserved instead of auto alias");
 CK(eq("getComputedStyle(document.getElementById('action')).pointerEvents","all"),"descendants inherit all from dialog");
 point(90,70,"action","/action","all dialog escapes none wrapper");
 point(245,145,"dialog",0,"all dialog blank box is targetable");
 point(20,20,"shade",0,"none wrapper blank area still targets shade");
 paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);CK(painted(0xaabbcc),"all dialog still paints normally");
 CK(eq("var modal=document.getElementById('dialog');modal.style.pointerEvents='none';getComputedStyle(modal).pointerEvents","none"),"dynamic none disables all subtree");
 point(90,70,"shade",0,"none dialog falls back to shade");
 CK(eq("modal.style.pointerEvents='ALL';getComputedStyle(modal).pointerEvents","all"),"dynamic uppercase all restores canonical computed value");
 point(90,70,"action","/action","dynamic all restores actual descendant hit");
 CK(eq("modal.style.pointerEvents='unset';getComputedStyle(modal).pointerEvents","none"),"unset returns to inherited none");
 point(90,70,"shade",0,"unset all declaration does not leave stale hit");
 CK(eq("modal.style.setProperty('pointer-events','all','important');modal.style.pointerEvents","all"),"inline important all survives CSSOM serialization");
 point(90,70,"action","/action","important all restores actual target");
 CK(eq("modal.style.pointerEvents='all none';getComputedStyle(modal).pointerEvents","all"),"invalid all sequence preserves previous valid declaration");
 CK(eq("modal.style.visibility='hidden';getComputedStyle(modal).visibility","hidden"),"all does not alter HTML visibility");
 /* The native browser commits dirty layout before accepting the next frame's
  * click. getComputedStyle alone only commits style, while item.hidden is a
  * layout snapshot; force that same commit before calling the painter API. */
 CK(eq("modal.getBoundingClientRect().width","180"),"hidden dialog retains committed layout geometry");
 point(90,70,"shade",0,"hidden HTML dialog is not exposed by all");
 close_page();
 printf("pointer-events: %s (%d checks)\n",fails?"FAIL":"PASS",checks);return fails?1:0;
}
