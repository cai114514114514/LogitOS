/* Finite line-relative transforms through the shipping style/layout/painter.
 * Labels and expected coordinates are independent of transform parsing. The
 * px twins preserve the same paint/hit oracle in the old behavior control. */
#define main prior_modal_main
#include "modal_paint_test.c"
#undef main

static int checks,failures,dirty,known_hit_gap;
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

static void shifted(const char *name,const char *font,const char *rootline,
                    const char *line,const char *transform,int dx,int dy)
{
 scene=name;char css[1800];snprintf(css,sizeof css,
  "html{font-size:24px;line-height:%s}body{margin:0}"
  "#under{position:absolute;left:0;top:0;width:390px;height:280px}"
  "#face{position:absolute;left:50px;top:30px;width:80px;height:40px;"
  "background:#abcdef;font-size:%s;line-height:%s;transform:%s}"
  "#word{line-height:20px}",rootline,font,line,transform);
 struct node *r=page("<!doctype html><a id=under href=/under></a><div id=face><span id=word>LABEL</span></div>",css);
 struct node *f=dom_get_element_by_id(r->doc,"face"),*hit=0;
 int x,y,w,h;char href[40];int count=0,at=text_index("LABEL",&count);
 if(!known_hit_gap){
 EXPECT(layout_node_box(f,&x,&y,&w,&h)&&x==50&&y==30&&w==80&&h==40,"transform preserves layout box");
 EXPECT(count==1,"label paints once");
 EXPECT(at>=0&&paint_ops[at].x==50+dx,"text x uses containing element line context");
 EXPECT(at>=0&&paint_ops[at].y==30+dy,"text y uses document root line context");
 }else{
 browser_hittest_node_scroll(50+dx+40,30+dy+20,0,0,&hit,href,sizeof href);
 EXPECT(desc(hit,f),"native hit follows resolved translation");
 }
 if(!known_hit_gap){
 paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);at=text_index("LABEL",&count);
 EXPECT(at>=0&&paint_ops[at].x==50+dx&&paint_ops[at].y==30+dy,"repaint does not accumulate resolved lengths");
 }
 done(r);
}
static void stacking_line(void)
{
 scene="line-transform-stacking";
 struct node *r=page("<!doctype html><div id=p><span id=c>INNER</span></div><div id=s>OUTER</div>",
  "html{line-height:40px}body{margin:0}#p{width:100px;height:40px;transform:translateZ(1lh)}#c{position:relative;z-index:99}#s{position:relative;z-index:1}");
 int a,b;int ai=text_index("INNER",&a),bi=text_index("OUTER",&b);
 EXPECT(a==1&&b==1,"both stacking labels paint");
 EXPECT(ai>=0&&bi>ai,"line-relative transform establishes stacking context");done(r);
}
static void root_change(void)
{
 scene="root-line-change";
 const char *html="<!doctype html><div id=face><span>LABEL</span></div>";
 const char *one="html{font-size:24px;line-height:30px}body{margin:0}#face{position:absolute;left:50px;top:30px;width:80px;height:40px;line-height:20px;transform:translateX(1rlh)}";
 const char *two="html{font-size:24px;line-height:70px}body{margin:0}#face{position:absolute;left:50px;top:30px;width:80px;height:40px;line-height:20px;transform:translateX(1rlh)}";
 struct node *r=page(html,one);int count,at=text_index("LABEL",&count);
 EXPECT(count==1&&at>=0&&paint_ops[at].x==80,"initial rlh resolves root line not root font");
 css_apply(r,two,(int)strlen(two));css_extra_apply(r,two,(int)strlen(two));layout_page(r,400);
 paint_nops=0;browser_paint_scroll(0,0,400,300,0,0);at=text_index("LABEL",&count);
 EXPECT(count==1&&at>=0&&paint_ops[at].x==120,"root line style update reaches transform");
 layout_page(r,600);paint_nops=0;browser_paint_scroll(0,0,600,300,0,0);at=text_index("LABEL",&count);
 EXPECT(count==1&&at>=0&&paint_ops[at].x==120,"resize keeps line units independent of viewport");done(r);
}
static void syntax(void)
{
 scene="line-transform-syntax";
 EXPECT(css_supports_decl("transform",-1,"translateZ(.5lh)",-1),"supports accepts element-line units");
 EXPECT(css_supports_decl("transform",-1,"translateY(1rlh)",-1),"supports accepts root-line units");
 EXPECT(css_supports_decl("transform",-1,"translateZ(20px)",-1),"supports retains px reference");
 EXPECT(!css_supports_decl("transform",-1,"translateZ(.5foolh)",-1),"unknown suffix stays invalid");
 EXPECT(!css_supports_decl("transform",-1,"translateZ(2%)",-1),"z-axis percentage remains invalid");
 EXPECT(!css_supports_decl("transform",-1,"rotateX(1lh)",-1),"line length is not an angle");
 EXPECT(!css_supports_decl("transform",-1,"translateX(1lh) junk",-1),"whole transform syntax remains checked");
}
int main(int argc,char **argv)
{
 known_hit_gap=argc==2&&!strcmp(argv[1],"--known-translated-hit");
 css_init();css_set_post_pass(css_extra_apply);
 shifted("explicit-lines","20px","60px","40px","translate(1lh,1rlh)",40,60);
 shifted("explicit-px","20px","60px","40px","translate(40px,60px)",40,60);
 shifted("fractional-lines","20px","50px","46px","translate(.5lh,1rlh)",23,50);
 shifted("normal-lines","20px","normal","normal","translate(1lh,1rlh)",25,30);
 shifted("zero-lines","20px","0","0","translate(1lh,1rlh)",0,0);
 shifted("zero-px","20px","0","0","translate(0px,0px)",0,0);
 shifted("inherited-line","20px","52px","inherit","translateX(1lh)",52,0);
 if(known_hit_gap){printf("transform-lineheight-hit-gap: %d checks, %d failures\n",checks,failures);return failures?1:0;}
 card("lh-back","rotateX(180deg) translateZ(.5lh)","backface-visibility:hidden","#face{font-size:24px;line-height:40px}",0);
 card("lh-quarter","rotateX(-90deg) translateZ(.5lh)","backface-visibility:hidden","#face{font-size:24px;line-height:40px}",0);
 card("rlh-back","rotateY(180deg) translateZ(.5rlh)","backface-visibility:hidden","html{line-height:60px}",0);
 card("px-quarter","rotateX(-90deg) translateZ(20px)","backface-visibility:hidden","",0);
 card("lh-front","translateZ(.5lh)","backface-visibility:hidden","",1);
 card("invalid-unit","rotateX(180deg) translateZ(.5foolh)","backface-visibility:hidden","",1);
 syntax();stacking_line();root_change();
 printf("transform-lineheight: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
