/* Normal label controls only: no file selection, site or event submission. */
#define main unused_modal_main
#include "modal_paint_test.c"
#undef main
#include "forms.h"
static int checks, failures;
#define EXPECT(c,m) do{checks++;if(!(c)){failures++;printf("FAIL: %s/%s: %s\n",kind,name,m);}}while(0)
static void sample(const char *kind,const char *name,const char *rule,int edges_w,int edges_h,int explicit_size)
{
 char html[256],css[512];snprintf(html,sizeof html,"<!doctype html><input id=f type=%s value=OK>",kind);
 snprintf(css,sizeof css,"body{margin:20px;font-size:16px}input{display:block}%s",rule);
 struct node *root=dom_parse(html,strlen(html));css_viewport(400,200);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,400);
 struct node *n=dom_get_element_by_id(root->doc,"f");int x=0,y=0,w=0,h=0;EXPECT(layout_node_box(n,&x,&y,&w,&h),"box exists");
 const char *label=!strcmp(kind,"file")?"Choose File":"OK";int len=strlen(label),want_w=explicit_size?160:len*8+edges_w+10,want_h=explicit_size?80:20+edges_h;
 EXPECT(w==want_w&&h==want_h,"natural or explicit dimensions include edges once");
 paint_nops=0;browser_paint_scroll(0,0,400,200,0,0);int found=0,fits=0,cl[4]={0};
 for(int i=0;i<paint_nops;i++){struct paintop *p=paint_ops+i;if(p->kind==OP_CLIP){cl[0]=p->x;cl[1]=p->y;cl[2]=p->w;cl[3]=p->h;}if(p->kind==OP_TEXT&&p->len==len&&!memcmp(p->text,label,len)){found++;fits=cl[2]>=len*8&&cl[3]>=20;}}
 EXPECT(fits,"label has positive natural content dimensions");
 EXPECT(found==1,"native label painted exactly once");
 printf("case=%s/%s box=%dx%d expected=%dx%d label_runs=%d\n",kind,name,w,h,want_w,want_h,found);
 fc_reset();focus_reset();layout_free();dom_free(root);
}
int main(void)
{
 const char *types[]={"submit","reset","button","image","file"};
 for(int i=0;i<5;i++){
  sample(types[i],"default","",12,8,0);
  sample(types[i],"large","input{padding:16px;border:1px solid}",34,34,0);
  sample(types[i],"zero","input{padding:0;border:0}",0,0,0);
  sample(types[i],"explicit","input{padding:16px;border:1px solid;box-sizing:border-box;width:160px;height:80px}",34,34,1);
 }
 printf("form-native-button-edges: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
