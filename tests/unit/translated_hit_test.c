/* Finite ordinary translated controls, page scroll and element clipping.
 * The exact paint recorder is the position oracle; no site code is involved. */
#define main previous_modal_main
#include "modal_paint_test.c"
#undef main
#include "forms.h"
static int checks,failures;static const char *scene;
#define EXPECT(c,m) do{checks++;if(!(c)){failures++;printf("FAIL: %s: %s\n",scene,m);}}while(0)
static int desc(struct node *n,struct node *p){for(;n;n=n->parent)if(n==p)return 1;return 0;}
/* A bounded presentation-offset seam, supplied at the same callback boundary
 * as CSSOM. This fixture checks transform composition after that callback;
 * it does not re-test the JS element-scroll state machine. */
static struct node *inner_owner;static int inner_x,inner_y;
void js_cssom_scroll_offset(const struct node *n,int *x,int *y)
{int in=n&&n!=inner_owner&&desc((struct node *)n,inner_owner);*x=in?inner_x:0;*y=in?inner_y:0;}
void js_cssom_project_item(struct item *e)
{int x,y;js_cssom_scroll_offset(e->node,&x,&y);e->x-=x;e->y-=y;}

static void release(struct node *r){inner_owner=0;inner_x=inner_y=0;top_layer_reset();focus_reset();fc_reset();layout_free();dom_free(r);}
static struct node *page(const char *html,const char *css)
{struct node *r=dom_parse(html,strlen(html));css_viewport(500,400);css_apply(r,css,strlen(css));css_extra_apply(r,css,strlen(css));layout_page(r,500);return r;}
static void rect_at(unsigned color,int *x,int *y,int *w,int *h,int *count)
{
 *count=0;for(int i=0;i<paint_nops;i++){struct paintop *p=paint_ops+i;if((p->kind==OP_RECT||p->kind==OP_RRECT)&&p->color==color){(*count)++;*x=p->x;*y=p->y;*w=p->w;*h=p->h;}}
}
static void probe(const char *label,const char *parent_rule,const char *rule,int dx,int dy,int scroll_x,int scroll_y,int fixed)
{
 scene=label;char css[1600];snprintf(css,sizeof css,
 "body{margin:0;font-size:14px}#under{position:absolute;left:0;top:0;width:490px;height:390px}"
 "#p{position:absolute;left:80px;top:60px;width:120px;height:80px;%s}"
 "#f{display:block;margin:0;padding:0;border:0;width:80px;height:40px;box-sizing:border-box;background:#abcdef;border-radius:0;%s}",parent_rule,rule);
 struct node *r=page("<!doctype html><a id=under href=/under></a><div id=p><input id=f value=ABCDE></div>",css);
 struct node *target=dom_get_element_by_id(r->doc,"f"),*hit=0;int lx,ly,lw,lh,x=0,y=0,w=0,h=0,count=0;char href[50];
 EXPECT(layout_node_box(target,&lx,&ly,&lw,&lh),"target has layout box");
 int ex=lx+dx-(fixed?0:scroll_x),ey=ly+dy-(fixed?0:scroll_y);
 paint_nops=0;browser_paint_scroll(0,0,500,400,scroll_x,scroll_y);
 rect_at(0xabcdef,&x,&y,&w,&h,&count);
 EXPECT(count==1,"visible control background draws once");EXPECT(x==ex&&y==ey&&w==lw&&h==lh,"paint follows expected complete translation");
 browser_hittest_node_scroll(ex+2,ey+2,scroll_x,scroll_y,&hit,href,sizeof href);
 EXPECT(desc(hit,target),"visible translated position targets control");EXPECT(!href[0],"control blocks underlying link");
 if(dx||dy){browser_hittest_node_scroll(lx-(fixed?0:scroll_x)+2,ly-(fixed?0:scroll_y)+2,scroll_x,scroll_y,&hit,href,sizeof href);EXPECT(!desc(hit,target),"old unshifted position does not target control");}
 const struct item *it=layout_items();int found=0;
 for(int i=0;i<layout_count();i++)if(it[i].type==IT_CONTROL&&it[i].node==target){struct item q;browser_input_item_geometry(&it[i],0,0,scroll_x,scroll_y,&q);found=1;
  EXPECT(q.x-scroll_x==ex&&q.y-scroll_y==ey&&q.w==lw&&q.h==lh,"input/selection geometry shares visible border box");
  int rel=ex+14+scroll_x-(q.x+fc_content_insets(target).left);
  EXPECT(fc_offset_at_px(target,rel,14,0)==2,"native caret maps visible point to ordinary byte offset");
  EXPECT(q.y-scroll_y+q.h==ey+lh,"popup anchor follows translated lower edge");break;}
 EXPECT(found,"real native control item tested");
 release(r);
}
static void child_and_clip(void)
{
 scene="translated-child-overflow";
 const char *html="<a id=under href=/under></a><div id=p><button id=f><span id=word>Label</span></button></div>";
 const char *css="body{margin:0;font-size:14px}#under{position:absolute;width:490px;height:390px}#p{position:absolute;left:80px;top:60px;width:120px;height:80px;overflow:hidden}#f{display:block;margin:0;padding:0;border:0;width:80px;height:40px;background:#abcdef;border-radius:0;transform:translate(80px,20px)}";
 struct node *r=page(html,css);struct node *p=dom_get_element_by_id(r->doc,"p"),*f=dom_get_element_by_id(r->doc,"f"),*hit=0;char href[30];
 paint_nops=0;browser_paint_scroll(0,0,500,400,0,0);
 browser_hittest_node_scroll(170,90,0,0,&hit,href,sizeof href);EXPECT(desc(hit,f),"visible clipped portion targets translated child");
 browser_hittest_node_scroll(220,90,0,0,&hit,href,sizeof href);EXPECT(!desc(hit,f),"outside stationary ancestor clip does not target child");
 browser_hittest_node_scroll(85,65,0,0,&hit,href,sizeof href);EXPECT(!desc(hit,f),"old child area has no ghost target");
 int text=0;for(int i=0;i<paint_nops;i++)if(paint_ops[i].kind==OP_TEXT&&paint_ops[i].len==5&&!memcmp(paint_ops[i].text,"Label",5))text++;
 EXPECT(text==1,"child-content button still paints one label");
 (void)p;release(r);
}
static void text_geometry(void)
{
 scene="translated-source-text";const char *html="<div id=p contenteditable=true><span id=t>ABCDE</span></div>";
 const char *css="body{margin:0;font-size:14px}#p{position:absolute;left:80px;top:60px;transform:translate(40px,60px)}";
 struct node *r=page(html,css);paint_nops=0;browser_paint_scroll(0,0,500,400,17,23);
 int at,count;at=text_index("ABCDE",&count);EXPECT(count==1,"ordinary source text paints once");
 const struct item *it=layout_items();int found=0;for(int i=0;i<layout_count();i++)if(it[i].type==IT_TEXT&&it[i].len==5&&!memcmp(it[i].text,"ABCDE",5)){struct item q;browser_input_item_geometry(&it[i],0,0,17,23,&q);found=1;
  EXPECT(at>=0&&q.x-17==paint_ops[at].x&&q.y-23==paint_ops[at].y,"text selection rectangle matches translated paint");
  EXPECT(q.text==it[i].text&&q.len==it[i].len&&q.font_px==it[i].font_px,"translation preserves source bytes and advance contract");break;}
 EXPECT(found,"source text geometry consumer reached");release(r);
}
static void style_change(void)
{
 scene="before-repaint-style-change";const char *html="<a id=under href=/under></a><input id=f value=ABCDE>";
 const char *a="body{margin:0}#under{position:absolute;width:490px;height:390px}#f{position:absolute;left:60px;top:40px;width:80px;height:40px;padding:0;border:0;transform:translate(40px,60px)}";
 const char *b="body{margin:0}#under{position:absolute;width:490px;height:390px}#f{position:absolute;left:60px;top:40px;width:80px;height:40px;padding:0;border:0;transform:translate(100px,20px)}";
 struct node *r=page(html,a),*f=dom_get_element_by_id(r->doc,"f"),*hit=0;char href[20];paint_nops=0;browser_paint_scroll(0,0,500,400,0,0);
 css_apply(r,b,strlen(b));css_extra_apply(r,b,strlen(b));layout_page(r,500);
 browser_hittest_node_scroll(165,65,0,0,&hit,href,sizeof href);EXPECT(desc(hit,f),"new style hit works before another paint");
 browser_hittest_node_scroll(105,105,0,0,&hit,href,sizeof href);EXPECT(!desc(hit,f),"old cached transform no longer targets input");release(r);
}
static void inner_scroll(void)
{
 scene="inner-offset-plus-page-scroll";
 const char *html="<div id=p><div style='height:100px'></div><input id=f value=ABCDE></div>";
 const char *css="body{margin:0;font-size:14px}#p{position:absolute;left:80px;top:60px;width:120px;height:80px;overflow:hidden}#f{display:block;width:80px;height:40px;padding:0;border:0;border-radius:0;background:#abcdef;transform:translate(40px,20px)}";
 struct node *r=page(html,css),*f=dom_get_element_by_id(r->doc,"f"),*hit=0;inner_owner=dom_get_element_by_id(r->doc,"p");inner_x=10;inner_y=100;
 paint_nops=0;browser_paint_scroll(0,0,500,400,17,23);int x=0,y=0,w=0,h=0,count=0;
 rect_at(0xabcdef,&x,&y,&w,&h,&count);EXPECT(count==1&&x==93&&y==57,"paint composes inner offset translation and page scroll once");
 browser_hittest_node_scroll(95,59,17,23,&hit,0,0);EXPECT(hit==f,"trusted hit shares composed visible origin");
 browser_hittest_node_scroll(190,59,17,23,&hit,0,0);EXPECT(hit!=f,"stationary ancestor clip still limits hit");
 const struct item *it=layout_items();int found=0;for(int i=0;i<layout_count();i++)if(it[i].type==IT_CONTROL&&it[i].node==f){struct item q;browser_input_item_geometry(&it[i],0,0,17,23,&q);found=1;EXPECT(q.x-17==93&&q.y-23==57,"native consumer has same composed point");break;}
 EXPECT(found,"inner-scroll native control reached");release(r);
}
static void fractional_window_origin(void)
{
 scene="fractional-window-origin";
 const char *html="<input id=f value=ABCDE>";
 const char *css="body{margin:0;font-size:14px}#f{position:absolute;left:0;top:0;width:80px;height:14px;padding:0;border:0;border-radius:0;background:#abcdef;transform:translate(-0.5px,-0.5px)}";
 struct node *r=page(html,css),*f=dom_get_element_by_id(r->doc,"f"),*hit=0;
 paint_nops=0;browser_paint_scroll(53,80,500,400,0,0);
 int x=0,y=0,w=0,h=0,count=0;rect_at(0xabcdef,&x,&y,&w,&h,&count);
 EXPECT(count==1&&x==53&&y==80&&w==80&&h==14,"paint rounds half edges at actual positive window origin");
 const struct item *it=layout_items();int found=0;for(int i=0;i<layout_count();i++)if(it[i].type==IT_CONTROL&&it[i].node==f){struct item q;
  browser_input_item_geometry(&it[i],53,80,0,0,&q);found=1;
  EXPECT(q.x==x-53&&q.y==y-80&&q.w==w&&q.h==h,"input geometry uses identical window rounding phase");break;}
 EXPECT(found,"fractional native control reached");
 browser_hittest_node_viewport(53,80,0,0,0,0,&hit,0,0);EXPECT(hit==f,"painted top left hits at nonzero window origin");
 browser_hittest_node_viewport(53,80,0,14,0,0,&hit,0,0);EXPECT(hit!=f,"painted bottom excludes adjacent row");
 release(r);
}
int main(void)
{
 probe("own-px","","transform:translate(40px,60px)",40,60,0,0,0);
 probe("ancestor-px","transform:translate(40px,60px)","",40,60,0,0,0);
 probe("nested-px","transform:translate(30px,40px)","transform:translate(10px,20px)",40,60,0,0,0);
 probe("own-percent","","transform:translate(50%,150%)",40,60,0,0,0);
 probe("ancestor-percent","transform:translate(50%,75%)","",60,60,0,0,0);
 probe("nested-percent","transform:translate(50%,75%)","transform:translate(-25%,50%)",40,80,0,0,0);
 probe("page-scroll","transform:translate(40px,60px)","",40,60,17,23,0);
 probe("fixed-control","","position:fixed;left:80px;top:60px;transform:translate(40px,60px)",40,60,17,23,1);
 probe("fixed-parent","position:fixed;left:80px;top:60px;transform:translate(40px,60px)","",40,60,17,23,1);
 probe("untranslated-control","","",0,0,17,23,0);
 child_and_clip();text_geometry();style_change();inner_scroll();fractional_window_origin();
 printf("translated-hit: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
