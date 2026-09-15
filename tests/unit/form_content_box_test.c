#define main unused_modal_main
#include "modal_paint_test.c"
#undef main
#include "forms.h"
static int checks, failures;
#define EXPECT(c,m) do{checks++;if(!(c)){failures++;printf("FAIL: %s: %s\n",name,m);}}while(0)
static void sample(const char *name,const char *rule,int want_l,int want_t,int want_r,int want_b,int focused)
{
 const char *html="<!doctype html><input id=f placeholder=Placeholder>";
 char css[2048];snprintf(css,sizeof css,"body{margin:20px;font-size:14px;line-height:20px}input{width:160px;box-sizing:border-box}%s",rule);
 struct node *root=dom_parse(html,strlen(html));css_viewport(400,200);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,400);
 struct node *n=dom_get_element_by_id(root->doc,"f");int x,y,w,h;EXPECT(layout_node_box(n,&x,&y,&w,&h),"input box exists");
 if(focused)focus_set(n);
 paint_nops=0;browser_paint_scroll(0,0,400,200,0,0);
 int clip[4]={0},count=0;
 for(int i=0;i<paint_nops;i++){
  struct paintop *p=paint_ops+i;
  if(p->kind==OP_CLIP){clip[0]=p->x;clip[1]=p->y;clip[2]=p->w;clip[3]=p->h;}
  if(p->kind!=OP_TEXT||p->len!=11||memcmp(p->text,"Placeholder",11))continue;
  count++;
  printf("case=%s box=%dx%d content=%d,%d,%d,%d text_top=%d font=%d em_fits=%d\n",name,w,h,clip[0]-x,clip[1]-y,clip[2],clip[3],p->y-y,p->px,p->y>=clip[1]&&p->y+p->px<=clip[1]+clip[3]);
  EXPECT(clip[0]-x==want_l && clip[1]-y==want_t && clip[2]==w-want_l-want_r && clip[3]==h-want_t-want_b,"paint uses declared content edges");
  EXPECT(p->y>=clip[1]&&p->y+p->px<=clip[1]+clip[3],"placeholder em fits vertical clip");
  EXPECT(p->x==x+want_l,"placeholder starts at content origin");
 }
 EXPECT(count==1,"one empty placeholder reaches painter");
 fc_reset();focus_reset();layout_free();dom_free(root);
}
static int input_origin(struct node *n)
{
#ifdef FC_COMPUTED_CONTENT
 return fc_content_insets(n).left;
#else
 (void)n;return FC_BORDER+FC_PAD_X;
#endif
}
static void sizes(const char *name,int large)
{
 const char *html="<!doctype html><input id=i><textarea id=t rows=2 cols=20></textarea><select id=s><option>Choice</option></select>";
 char css[1024];snprintf(css,sizeof css,"body{margin:20px;font-size:16px}input,textarea,select{display:block}%s",large?"input,textarea,select{padding:12px 11px 9px 17px;border:2px solid}":"");
 struct node *root=dom_parse(html,strlen(html));css_viewport(500,300);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,500);
 const char *ids[]={"i","t","s"};int ww[]={large?192:172,large?192:172,large?98:78};int hh[]={large?45:28,large?65:48,large?45:28};
 for(int j=0;j<3;j++){struct node *n=dom_get_element_by_id(root->doc,ids[j]);int x,y,w,h;EXPECT(layout_node_box(n,&x,&y,&w,&h),"default control box exists");
  EXPECT(w==ww[j],"natural width includes content and edges once");EXPECT(h==hh[j],"natural height includes content and edges once");
  printf("case=%s kind=%d box=%dx%d expected=%dx%d\n",name,j,w,h,ww[j],hh[j]);}
 fc_reset();focus_reset();layout_free();dom_free(root);
}
static void editing(void)
{
 const char *name="short-selection";const char *html="<!doctype html><input id=f>";
 const char *css="body{margin:20px;font-size:14px}input{width:160px;height:32px;box-sizing:border-box;padding:2px 11px 4px 17px;border:2px solid}";
 struct node *root=dom_parse(html,strlen(html));css_viewport(400,200);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,400);
 struct node *n=dom_get_element_by_id(root->doc,"f");int x,y,w,h;layout_node_box(n,&x,&y,&w,&h);fc_set_value(n,"ABCD",4);focus_set(n);fc_set_selection(n,1,3);
 paint_nops=0;browser_paint_scroll(0,0,400,200,0,0);int selection=0,caret=0;
 for(int i=0;i<paint_nops;i++){struct paintop *p=paint_ops+i;
  if(p->kind==OP_RECT&&p->color==0xb4d5fe){selection++;EXPECT(p->x==x+19+7&&p->w==14,"selection uses computed content origin");}
  if(p->kind==OP_RECT&&p->w==1&&p->h>=14&&p->x==x+19+21)caret++;
 }
 EXPECT(selection==1,"one ordinary short selection painted");EXPECT(caret==1,"caret shares the selection content origin");
 EXPECT(fc_offset_at_px(n,19+7-input_origin(n),14,0)==1,"click uses the same computed origin");
 fc_reset();focus_reset();layout_free();dom_free(root);
}
static void overflow_value(void)
{
 const char *name="short-inner-scroll";const char *html="<!doctype html><input id=f>";
 const char *css="body{margin:20px;font-size:14px}input{width:80px;height:32px;box-sizing:border-box;padding:2px 11px 4px 17px;border:2px solid}";
 struct node *root=dom_parse(html,strlen(html));css_viewport(400,200);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,400);
 struct node *n=dom_get_element_by_id(root->doc,"f");int x,y,w,h;layout_node_box(n,&x,&y,&w,&h);fc_set_value(n,"ABCDEFGHIJKLMNOP",16);focus_set(n);fc_edit_end(n,0);
 paint_nops=0;browser_paint_scroll(0,0,400,200,0,0);int caret=-1;
 for(int i=0;i<paint_nops;i++){struct paintop *p=paint_ops+i;if(p->kind==OP_RECT&&p->w==1&&p->h>=14&&p->x>=x&&p->x<x+w)caret=p->x;}
 EXPECT(caret==x+w-13-2,"End caret reserves space at computed right edge");
 EXPECT(caret>=x+19&&caret<x+w-13,"End caret stays inside declared content clip");
 EXPECT(fc_offset_at_px(n,caret-x-input_origin(n),14,0)==16,"painted End position maps to last byte");
 fc_edit_home(n,0);paint_nops=0;browser_paint_scroll(0,0,400,200,0,0);struct fpaint fp;fc_paint_state(n,14,0,48,&fp);
 EXPECT(fp.scroll_x==0&&fp.caret_x==0,"Home clears ordinary inner scroll");
 fc_reset();focus_reset();layout_free();dom_free(root);
}
static void dirty_padding(void)
{
 const char *name="padding-repaint";const char *html="<!doctype html><input id=f placeholder=Placeholder>";
 const char *a="body{margin:20px;font-size:14px}input{width:160px;height:40px;box-sizing:border-box;border:1px solid;padding:3px 5px}";
 const char *b="body{margin:20px;font-size:14px}input{width:160px;height:40px;box-sizing:border-box;border:1px solid;padding:3px 17px 3px 5px}";
 struct node *root=dom_parse(html,strlen(html));css_viewport(400,200);css_apply(root,a,strlen(a));css_extra_apply(root,a,strlen(a));layout_page(root,400);
 browser_paint_scroll(0,0,400,200,0,0);browser_paint_scroll(0,0,400,200,0,0);int x,y,w,h;
 EXPECT(browser_paint_dirty_rect(&x,&y,&w,&h)==0,"unchanged field produces no dirty rectangle");
 css_apply(root,b,strlen(b));css_extra_apply(root,b,strlen(b));layout_page(root,400);browser_paint_scroll(0,0,400,200,0,0);
 EXPECT(browser_paint_dirty_rect(&x,&y,&w,&h)==1&&w>0&&h>0,"right padding change participates in paint signature");
 fc_reset();focus_reset();layout_free();dom_free(root);
}
int main(void)
{
 sample("reset16","input{height:16px;padding:0;border:0}",0,0,0,0,0);
 sample("focused-reset16","input{height:16px;padding:0;border:0}",0,0,0,0,1);
 sample("reset20","input{height:20px;padding:0;border:0}",0,0,0,0,0);
 sample("small-padding","input{height:20px;padding:2px 6px;border:1px solid}",7,3,7,3,0);
 sample("asymmetric","input{height:28px;padding:2px 11px 4px 7px;border:2px solid}",9,4,13,6,0);
 sample("default","",6,4,6,4,0);
 sample("percent240","body{width:240px}input{width:100px;height:50px;padding:5%;border:0}",12,12,12,12,0);
 sample("percent320","body{width:320px}input{width:100px;height:50px;padding:5%;border:0}",16,16,16,16,0);
 sizes("default-natural",0);sizes("authored-natural",1);editing();overflow_value();dirty_padding();
 printf("form-content-box: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
