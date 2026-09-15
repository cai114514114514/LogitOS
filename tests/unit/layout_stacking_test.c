/* Real parser -> CSS -> display list -> native paint and hit tests. */
#define main prior_modal_main
#include "modal_paint_test.c"
#undef main
static int checks,failures;
#define EXPECT(c,what) do{checks++;if(!(c)){failures++;printf("FAIL: %s: %s\n",scene,what);}}while(0)
static const char *scene;
static struct node *page(const char *html,const char *css)
{struct node *r=dom_parse(html,(int)strlen(html));css_viewport(500,400);css_apply(r,css,(int)strlen(css));css_extra_apply(r,css,(int)strlen(css));layout_page(r,500);paint_nops=0;browser_paint_scroll(0,0,500,400,0,0);return r;}
static int rect(uint32_t color)
{for(int i=paint_nops-1;i>=0;i--)if(paint_ops[i].kind==OP_RECT&&paint_ops[i].color==color)return i;return -1;}
static int text(const char *s){int count;return text_index(s,&count);}
static int hit(struct node *r,const char *id,int x,int y)
{struct node *n=0;char href[100];browser_hittest_node_scroll(x,y,0,0,&n,href,sizeof href);return n==dom_get_element_by_id(r->doc,id);}
static void done(struct node *r){layout_free();dom_free(r);}
static void own_background(const char *name,const char *parent,const char *child)
{
 scene=name;char css[900];snprintf(css,sizeof css,"body{margin:0}#p{width:200px;height:100px;background:#fedcba;%s}#c{position:relative;%s}",parent,child);
 struct node *r=page("<!doctype html><div id=p><div id=c>VISIBLE</div></div>",css);
 EXPECT(text("VISIBLE")>rect(0xfedcba)&&rect(0xfedcba)>=0,"context background precedes its descendant ink");EXPECT(hit(r,"c",2,2),"frontmost visible child receives the hit");done(r);
}
static void trap(const char *name,const char *parent,int trapped)
{
 scene=name;char css[1100];snprintf(css,sizeof css,"body{margin:0}#p{width:150px;height:80px;%s}#c{position:absolute;left:0;top:0;width:100px;height:60px;background:#112233;z-index:999999}#o{position:absolute;left:0;top:0;width:100px;height:60px;background:#aabbcc;z-index:2}",parent);
 struct node *r=page("<!doctype html><div id=p><div id=c>CHILD</div></div><div id=o>OTHER</div>",css);
 EXPECT(text("CHILD")>=0&&text("OTHER")>=0,"both real text items reach paint");EXPECT(trapped?text("CHILD")<rect(0xaabbcc):text("CHILD")>rect(0xaabbcc),"descendant context obeys its actual ancestor boundary");EXPECT(hit(r,trapped?"o":"c",2,2),"reverse hit agrees with the visible context");done(r);
}
static void negative(void)
{
 scene="negative-non-context";struct node *r=page("<!doctype html><div id=p><div id=c>NEGATIVE</div></div>","body{margin:0}#p{width:100px;height:60px;background:#fedcba}#c{position:absolute;z-index:-3;top:0;left:0}");
 EXPECT(text("NEGATIVE")<rect(0xfedcba),"negative context sits behind ordinary block background");EXPECT(hit(r,"p",2,2),"covered negative content does not receive hits");done(r);
}
static void flex(const char *display)
{
 scene=display;char css[700];snprintf(css,sizeof css,"body{margin:0}#f{display:%s;width:200px;height:100px;grid-template-columns:100px 100px}#p{z-index:8;width:100px;height:100px;background:#fedcba}#c{position:relative;z-index:1}",display);
 struct node *r=page("<!doctype html><div id=f><div id=p><div id=c>FLEX</div></div></div>",css);
 EXPECT(text("FLEX")>rect(0xfedcba),"unpositioned non-auto z item owns its descendant context");EXPECT(hit(r,"c",2,2),"item descendant hit stays above its own background");done(r);
}
static void order(void)
{
 scene="order-modified-tree";struct node *r=page("<!doctype html><div id=f><div id=a><div id=c>A</div></div><div id=b>B</div></div>","body{margin:0}#f{display:flex}#a{order:2;width:100px}#b{order:1;width:100px;position:relative;z-index:0}#c{position:relative;z-index:0}");
 EXPECT(text("A")>text("B"),"equal z descendants follow order-modified flex subtrees");done(r);
}
static void deep(void)
{
 scene="deep-tree";char html[15000],css[300];int at=snprintf(html,sizeof html,"<!doctype html><div id=p style='position:relative;z-index:1'>");
 for(int i=0;i<80;i++)at+=snprintf(html+at,sizeof html-at,"<div style='position:relative;z-index:%d'>",10+i);
 at+=snprintf(html+at,sizeof html-at,"<span id=c>DEEP</span>");for(int i=0;i<81;i++)at+=snprintf(html+at,sizeof html-at,"</div>");snprintf(html+at,sizeof html-at,"<div id=o>FRONT</div>");
 snprintf(css,sizeof css,"body{margin:0}#o{position:absolute;top:0;left:0;width:100px;height:60px;background:#aabbcc;z-index:2}");struct node *r=page(html,css);
 EXPECT(text("DEEP")>=0&&text("DEEP")<rect(0xaabbcc),"eighty nested contexts cannot escape the outer context");EXPECT(hit(r,"o",2,2),"deep nesting keeps the same frontmost hit");done(r);
}
static void markers(void)
{
 scene="marker-backing";struct node *r=page("<!doctype html><ol><li>ONE</li><li>TWO</li></ol>","body{margin:0}li{position:relative;z-index:1}");
 const struct item *it=layout_items();int seen=0;for(int i=0;i<layout_count();i++)if(it[i].type==IT_TEXT&&it[i].len==2&&it[i].text&&it[i].text[1]=='.'){EXPECT(it[i].text==it[i].marker,"permuted list marker retains its own inline text backing");seen++;}
 EXPECT(seen==2,"both numbered list markers remain readable");done(r);
}
int main(void)
{
 css_init();own_background("parent8-child1","position:relative;z-index:8","z-index:1");own_background("negative-in-context","position:relative;z-index:8","z-index:-3");
 trap("nested-context","position:relative;z-index:1",1);trap("auto-not-a-boundary","position:relative",0);trap("zero-is-a-boundary","position:relative;z-index:0",1);
 trap("opacity-boundary","opacity:.5",1);trap("near-opaque-boundary","opacity:.999",1);trap("transform-boundary","transform:translateX(0px)",1);trap("transform-none","transform:none",0);trap("ignored-static-z","z-index:1",0);
 trap("fixed-auto-boundary","position:fixed",1);trap("sticky-auto-boundary","position:sticky",1);
 negative();flex("flex");flex("grid");order();deep();markers();
 printf("layout-stacking: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
