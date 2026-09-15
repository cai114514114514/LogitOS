/* Ordinary two-sided cards, through real CSS/layout/paint and native hit.
 * Expected visibility is fixed by simple rotations before implementation is
 * consulted. The legacy build ignores facing in both real consumers. */
#define main prior_modal_main
#include "modal_paint_test.c"
#undef main

static int checks,failures,dirty;
static unsigned long long mutation;
int js_dom_dirty(void){return dirty;}
unsigned long long js_dom_mutation_generation(void){return mutation;}
static const char *scene;
#define EXPECT(c,msg) do{checks++;if(!(c)){failures++;printf("FAIL: %s: %s\n",scene,msg);}}while(0)
static int count_text(const char *s){int n;text_index(s,&n);return n;}
static int count_color(unsigned color){int n=0;for(int i=0;i<paint_nops;i++)if((paint_ops[i].kind==OP_RECT||paint_ops[i].kind==OP_RRECT)&&paint_ops[i].color==color)n++;return n;}
static int desc(struct node *n,struct node *p){for(;n;n=n->parent)if(n==p)return 1;return 0;}
static struct node *page(const char *html,const char *css)
{
 dirty=0;struct node *r=dom_parse(html,(int)strlen(html));css_viewport(400,300);
 css_apply(r,css,(int)strlen(css));css_extra_apply(r,css,(int)strlen(css));layout_page(r,400);
 paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);return r;
}
static void done(struct node *r){top_layer_reset();focus_reset();layout_free();dom_free(r);}
static void card(const char *name,const char *xf,const char *vis,const char *extra,int visible)
{
 scene=name;char css[2048];snprintf(css,sizeof css,"body{margin:0}#under{position:absolute;left:50px;top:30px;width:100px;height:60px;background:#112233}#face{position:absolute;left:50px;top:30px;width:100px;height:60px;background:#abcdef;transform:%s;%s}#child{display:block}%s",xf,vis,extra);
 struct node *r=page("<!doctype html><a id=under href=/under>UNDER</a><div id=face>FACE<span id=child>CHILD</span></div>",css);
 struct node *f=dom_get_element_by_id(r->doc,"face"),*hit=0;int x,y,w,h;char href[40];
 EXPECT(layout_node_box(f,&x,&y,&w,&h)&&x==50&&y==30&&w==100&&h==60,"face keeps layout geometry");
 EXPECT(count_text("FACE")==visible,"face ink matches facing");EXPECT(count_text("CHILD")==visible,"ordinary descendant shares face visibility");
 EXPECT(count_color(0xabcdef)==visible,"face background follows same visibility");
 browser_hittest_node_scroll(100,60,0,0,&hit,href,sizeof href);
 EXPECT(visible?desc(hit,f):!desc(hit,f),"native hit agrees with visible plane");
 EXPECT(visible?!href[0]:!strcmp(href,"/under"),"covered link only returns when face is absent");
 paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);EXPECT(count_text("FACE")==visible,"full repaint repeats current facing");done(r);
}
static void nested(const char *name,const char *parent,const char *child,int visible)
{
 scene=name;char css[2048];snprintf(css,sizeof css,"body{margin:0}#under{position:absolute;left:0;top:0;width:100px;height:60px;background:#112233}#parent{position:relative;width:100px;height:60px;%s}#face{position:absolute;left:0;top:0;width:100px;height:60px;background:#abcdef;%s}",parent,child);
 struct node *r=page("<!doctype html><a id=under href=/under>UNDER</a><div id=parent><div id=face>FACE</div></div>",css);
 struct node *f=dom_get_element_by_id(r->doc,"face"),*hit=0;char href[40];
 EXPECT(count_text("FACE")==visible,"nested plane facing matches accumulated transform");
 browser_hittest_node_scroll(50,30,0,0,&hit,href,sizeof href);EXPECT(visible?desc(hit,f):!desc(hit,f),"nested hit shares plane ownership");
 done(r);
}
static void cascade(void)
{
 scene="cascade";const char *html="<!doctype html><div id=p><div id=a class=x></div><div id=b style='backface-visibility:inherit;transform-style:inherit'></div><div id=c style='backface-visibility:initial;transform-style:unset'></div><div id=d style='backface-visibility:hidden;backface-visibility:banana'></div><div id=e style='backface-visibility:visible'></div><div id=f></div></div>";
 struct node *r=page(html,"#p{backface-visibility:hidden;transform-style:preserve-3d}#a{backface-visibility:hidden}.x{backface-visibility:visible}#e{backface-visibility:hidden!important}#f{backface-visibility:hidden;backface-visibility:visible}");
 const char *ids[]={"p","a","b","c","d","e","f"};const char *want[]={"hidden","hidden","hidden","visible","hidden","hidden","visible"};char out[60];
 for(int i=0;i<7;i++){struct node *n=dom_get_element_by_id(r->doc,ids[i]);css_computed_text(n,CSSP_BACKFACE_VISIBILITY,out,sizeof out);EXPECT(!strcmp(out,want[i]),"backface default/inherit/invalid/specificity/important computed value");}
 struct node *b=dom_get_element_by_id(r->doc,"b"),*c=dom_get_element_by_id(r->doc,"c");css_computed_text(b,CSSP_TRANSFORM_STYLE,out,sizeof out);EXPECT(!strcmp(out,"preserve-3d"),"explicit transform-style inheritance resolves");
 css_computed_text(c,CSSP_TRANSFORM_STYLE,out,sizeof out);EXPECT(!strcmp(out,"flat"),"unset transform-style resets non-inherited property");
 EXPECT(css_prop_lookup("backfaceVisibility",-1)==CSSP_BACKFACE_VISIBILITY,"camelCase computed lookup uses same property");
 EXPECT(css_supports_decl("backface-visibility",-1,"hidden",-1),"public CSS parser accepts supported value");
 EXPECT(!css_supports_decl("backface-visibility",-1,"banana",-1),"public CSS parser rejects invalid value");
 done(r);
}
static void synchronous(void)
{
 scene="synchronous-cssom";css_set_post_pass(css_extra_apply);
 const char *html="<!doctype html><div id=p style='backface-visibility:hidden;transform-style:preserve-3d'></div>";
 struct node *r=dom_parse(html,(int)strlen(html));
 struct node *p=dom_get_element_by_id(r->doc,"p");char out[80];dirty=0;
 css_computed_text(p,CSSP_BACKFACE_VISIBILITY,out,sizeof out);EXPECT(!strcmp(out,"hidden"),"first computed read flushes extension cascade");
 dom_set_attr(p,"style","backface-visibility:visible;transform-style:flat");dirty=1;mutation++;
 css_computed_text(p,CSSP_BACKFACE_VISIBILITY,out,sizeof out);EXPECT(!strcmp(out,"visible"),"same-turn computed read sees style mutation");
 dom_set_attr(p,"style","backface-visibility:hidden;transform-style:preserve-3d");mutation++;
 css_computed_text(p,CSSP_BACKFACE_VISIBILITY,out,sizeof out);EXPECT(!strcmp(out,"hidden"),"same-turn repeated mutation reestablishes hidden");
 css_computed_text(p,CSSP_TRANSFORM_STYLE,out,sizeof out);EXPECT(!strcmp(out,"preserve-3d"),"same-turn transform-style read sees extension");
 dirty=0;dom_free(r);
}
static void top_layer(void)
{
 scene="top-layer-boundary";struct node *r=page("<!doctype html><div id=p><dialog id=d open>MODAL</dialog></div>","body{margin:0}#p{backface-visibility:hidden;transform:rotateY(180deg)}dialog{width:100px;height:60px;border:0;padding:0;margin:0}");
 struct node *d=dom_get_element_by_id(r->doc,"d");top_layer_push(d);layout_page(r,400);paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);
 EXPECT(count_text("MODAL")==1,"top layer leaves ordinary hidden ancestor plane");struct node *hit=0;char href[10];browser_hittest_node_scroll(200,150,0,0,&hit,href,sizeof href);EXPECT(desc(hit,d),"top layer remains native hit target");done(r);
}
static void clipped(void)
{
 scene="overflow-clipping";const char *html="<!doctype html><a id=under href=/under>UNDER</a><div id=clip><div id=face>FACE</div></div>";
 const char *styles[]={"rotateY(0deg)","rotateY(180deg)"};
 for(int i=0;i<2;i++) {
  char css[700];snprintf(css,sizeof css,"body{margin:0}#under{position:absolute;width:200px;height:100px}#clip{position:relative;overflow:hidden;width:40px;height:40px}#face{position:absolute;top:0;left:0;width:100px;height:60px;background:#abcdef;backface-visibility:hidden;transform:%s}",styles[i]);
  struct node *r=page(html,css),*f=dom_get_element_by_id(r->doc,"face"),*hit=0;char href[20];
  browser_hittest_node_scroll(10,10,0,0,&hit,href,sizeof href);EXPECT(i?!desc(hit,f):desc(hit,f),"inside clip obeys backface ownership");
  browser_hittest_node_scroll(70,10,0,0,&hit,href,sizeof href);EXPECT(!desc(hit,f)&&!strcmp(href,"/under"),"outside clip never hits clipped face");
  EXPECT(count_text("FACE")==!i,"clipped full paint still culls turned face");done(r);
 }
}
int main(void)
{
 css_init();css_set_post_pass(css_extra_apply);
 synchronous();
 card("x-zero","rotateX(0deg)","backface-visibility:hidden","",1);
 card("y-zero","rotateY(0deg)","backface-visibility:hidden","",1);
 card("x-back","rotateX(180deg)","backface-visibility:hidden","",0);
 card("y-back","rotateY(180deg)","backface-visibility:hidden","",0);
 card("x-quarter","rotateX(90deg)","backface-visibility:hidden","",0);
 card("y-quarter","rotateY(90deg)","backface-visibility:hidden","",0);
 card("default-visible","rotateY(180deg)","","",1);
 card("explicit-visible","rotateX(180deg)","backface-visibility:visible","",1);
 card("mirror-is-front","scaleX(-1)","backface-visibility:hidden","",1);
 card("two-d-rotation","rotate(180deg)","backface-visibility:hidden","",1);
 card("visibility-hidden","none","backface-visibility:visible;visibility:hidden","",0);
 card("child-visible-cannot-reveal-plane","rotateY(180deg)","backface-visibility:hidden","#child{backface-visibility:visible}",0);
 nested("preserve-parent-turn","transform-style:preserve-3d;transform:rotateY(180deg)","backface-visibility:hidden",0);
 nested("flat-parent-turn","transform:rotateY(180deg)","backface-visibility:hidden",1);
 nested("two-half-turns","transform-style:preserve-3d;transform:rotateY(180deg)","transform:rotateY(180deg);backface-visibility:hidden",1);
 nested("hidden-parent-independent-child","transform-style:preserve-3d;transform:rotateY(180deg);backface-visibility:hidden","transform:rotateY(180deg);backface-visibility:hidden",1);
 nested("hidden-parent-none-child","transform-style:preserve-3d;transform:rotateY(180deg);backface-visibility:hidden","transform:none",0);
 nested("hidden-parent-two-d-child","transform-style:preserve-3d;transform:rotateY(180deg);backface-visibility:hidden","transform:translateX(0px)",0);
 nested("overflow-forces-flat","transform-style:preserve-3d;transform:rotateY(180deg);overflow:hidden","backface-visibility:hidden",1);
 nested("opacity-forces-flat","transform-style:preserve-3d;transform:rotateY(180deg);opacity:.5","backface-visibility:hidden",1);
 cascade();top_layer();clipped();
 printf("backface: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
