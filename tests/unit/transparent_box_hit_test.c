/* The guest can read a 180x40 anchor's bounding box while its blank center
 * receives no native click. Exercise the real parser/style/layout, CSSOM and
 * painter hit consumer together; a second hit algorithm would test itself.
 * Deterministic font metrics come from cssom_test.c. This proves host routing,
 * not guest font pixels, QMP delivery, or a network navigation. */
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
 struct node *hit=0;char actual[120],msg[200],src[500];browser_hittest_node_scroll(x,y,0,0,&hit,actual,sizeof actual);
 struct node *want=id?dom_get_element_by_id(g_root->doc,id):0;
 printf("POINT %s at %d,%d native=%s href=%s\n",label,x,y,hit?dom_attr(hit,"id"):"<none>",actual);
 snprintf(msg,sizeof msg,"%s native target",label);CK(id?hit==want:!actual[0],msg);
 snprintf(msg,sizeof msg,"%s native href",label);CK(!strcmp(actual,href?href:""),msg);
 if(id)snprintf(src,sizeof src,"document.elementFromPoint(%d,%d)===document.getElementById('%s')",x,y,id);
 else snprintf(src,sizeof src,"(function(){var n=document.elementFromPoint(%d,%d);while(n){if(n.tagName==='A'&&n.getAttribute('href'))return false;n=n.parentNode}return true})()",x,y);
 snprintf(msg,sizeof msg,"%s CSSOM target",label);CK(eq(src,"true"),msg);
}
static void container(const char *kind,const char *body,const char *css){
 open_page(body,css);struct node *a=dom_get_element_by_id(g_root->doc,"go");int x=0,y=0,w=0,h=0;CK(layout_node_box(a,&x,&y,&w,&h),"apparatus anchor has real layout box");
 printf("BOX %s %d,%d %dx%d items=%d\n",kind,x,y,w,h,layout_count());CK(w==180&&h>=40,"apparatus sized blank region exists");
 char label[120];snprintf(label,sizeof label,"%s blank center",kind);point(x+w/2,y+h/2,"go","/target",label);
 snprintf(label,sizeof label,"%s blank bottom right",kind);point(x+w-8,y+h-8,"go","/target",label);
 char js[240];snprintf(js,sizeof js,"var r=document.getElementById('go').getBoundingClientRect();r.width===%d&&r.height===%d",w,h);CK(eq(js,"true"),"CSSOM measures the actual allocated border box");
 paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);int words=0;for(int i=0;i<paint_nops;i++)if(paint_ops[i].kind==OP_TEXT&&paint_ops[i].len==1&&paint_ops[i].text[0]=='X')words++;CK(words==1,"actual painter still emits link text once");close_page();
}
int main(void){css_init();
 container("block","<a id=go href=/target>X</a>","#go{display:block;width:180px;height:40px}");
 container("painted control","<a id=go href=/target>X</a>","#go{display:block;width:180px;height:40px;background:#ddeeff}");
 container("flex","<div style='display:flex;height:70px'><a id=go href=/target>X</a></div>","#go{width:180px}");
 container("grid","<div style='display:grid;grid-template-columns:180px'><a id=go href=/target>X</a></div>","#go{height:40px}");
 container("absolute","<a id=go href=/target>X</a>","#go{position:absolute;left:20px;top:10px;width:180px;height:40px}");
 container("atomic wrap","<span>prefixprefixprefixprefixprefix</span><a id=go href=/target>X</a>","#go{display:inline-block;width:180px;height:40px}");
 container("atomic vertical-align","<span style='font-size:60px'>Y</span><a id=go href=/target>X</a>","#go{display:inline-block;width:180px;height:40px;vertical-align:bottom}");
 container("table cell","<div style='display:table;border-spacing:0'><div style='display:table-row'><a id=go href=/target>X</a></div></div>","#go{display:table-cell;width:180px;height:40px;padding:0}");
 /* Transparent overlays must remain real targets; following a link below
  * them would turn the missing region into an unintended navigation. */
 open_page("<a id=under href=/target style='display:block;width:180px;height:40px;background:#ddd'>X</a><div id=cover></div>","#cover{position:absolute;left:0;top:0;width:180px;height:40px;z-index:5}");point(90,20,"cover","","transparent overlay blocks underlying anchor");close_page();
 open_page("<a id=under href=/target style='display:block;width:180px;height:40px;background:#ddd'>X</a><div id=cover></div>","#cover{position:absolute;left:0;top:0;width:180px;height:40px;background:red;z-index:5;pointer-events:none}");
 /* This property chain is not part of transparent box emission. An honest
  * refusal is an explicit unsupported observation, never a passing hit test.
  * If capability claims support, its real consumer must pass the same point. */
 char *pe=evalstr("JSON.stringify([CSS.supports('pointer-events','none'),CSS.supports('(pointer-events:none)')])");
 printf("CAPABILITY pointer-events:none %s\n",pe?pe:"<exception>");
 if(pe&&!strcmp(pe,"[false,false]")) {
  struct node *observed=0;char observed_href[120];browser_hittest_node_scroll(90,20,0,0,&observed,observed_href,sizeof observed_href);
  printf("UNSUPPORTED pointer-events:none; CSS.supports refuses declaration and condition; native target=%s href=%s; tracked separately in pointer-events-capability.md\n",observed?dom_attr(observed,"id"):"<none>",observed_href);
 } else {
  CK(pe&&!strcmp(pe,"[true,true]"),"pointer-events capability answers agree");
  point(90,20,"under","/target","pointer-events none claimed capability reaches hit consumer");
 }
 free(pe);close_page();
 open_page("<div style='width:90px;height:40px;overflow:hidden'><a id=go href=/target style='display:block;width:180px;height:40px'>X</a></div>",0);point(70,30,"go","/target","visible blank inside overflow clip");point(100,30,0,"","blank outside overflow clip");close_page();
 open_page("<div id=s style='width:100px;height:40px;overflow:auto'><div style='height:60px'></div><a id=go href=/target style='display:block;width:100px;height:40px'>X</a><div style='height:60px'></div></div>",0);CK(eq("document.getElementById('s').scrollTop=60;document.getElementById('go').getBoundingClientRect().top","0"),"real CSSOM projects scrolled transparent box");point(70,30,"go","/target","scrolled blank uses shared projection");close_page();
 open_page("<a id=go href=/target>json next more</a>","body{width:80px}");point(36,8,"go","/target","inline real whitespace remains clickable");point(76,8,0,"","wrapped inline first unused end");point(36,32,0,"","wrapped inline last unused end");close_page();
 open_page("<a id=go href=/target style='display:block;width:180px;height:40px;visibility:hidden'>X</a>",0);point(90,20,0,"","hidden box stays unhittable");close_page();
 printf("transparent-box-hit: %s (%d checks)\n",fails?"FAIL":"PASS",checks);return fails?1:0;
}
