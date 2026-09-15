/* Actual cascade -> layout -> paint. A transparent reset and no styling both
 * used to yield has_bg=0/border=0, so the old painter restored system chrome.
 * The UA sheet must provide defaults through CSS and leave author resets final.
 * Count real primitive coverage at the control centre to catch duplicate fills
 * and the white interior formerly used to fake a transparent border ring. */
#define main unused_modal_main
#include "modal_paint_test.c"
#undef main
#include "forms.h"
static void specimen(const char *name,const char *style,int disabled,int focused,int expected_fills,unsigned expected_bg){
 char html[512],css[1024];snprintf(html,sizeof html,"<!doctype html><div class=row><button id=b %s><span>Button</span></button></div>",disabled?"disabled":"");
 snprintf(css,sizeof css,"body{margin:20px;background:#446644}.row{display:flex;align-items:center}button{width:120px;height:36px}%s",style);
 struct node *root=dom_parse(html,strlen(html));css_viewport(400,200);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,400);
 struct node *b=dom_get_element_by_id(root->doc,"b");int x,y,w,h;layout_node_box(b,&x,&y,&w,&h);if(focused)focus_set(b);
 paint_nops=0;browser_paint_scroll(0,0,400,200,0,0);
 int centre_fills=0,label=0,control=0,ring=0;
 for(int i=0;i<layout_count();i++)if(layout_items()[i].node==b&&layout_items()[i].type==IT_CONTROL)control++;
 for(int i=0;i<paint_nops;i++){
  struct paintop *p=&paint_ops[i];if(p->kind==OP_TEXT){if(p->len==6&&!memcmp(p->text,"Button",6))label++;continue;}
  if(p->kind==OP_CLIP)continue;
  if(p->color==0x2f6feb&&p->x<x+w+2&&p->y<y+h+2&&p->x+p->w>x-2&&p->y+p->h>y-2)ring++;
  if(p->x>=x&&p->y>=y&&p->x+p->w<=x+w&&p->y+p->h<=y+h&&p->x<=x+w/2&&p->y<=y+h/2&&p->x+p->w>x+w/2&&p->y+p->h>y+h/2){
   centre_fills++;CHECK(p->color==expected_bg,"control centre uses computed background colour");
  }
 }
 printf("chrome %s centre=%d expected=%d controls=%d focusring=%d\n",name,centre_fills,expected_fills,control,ring);
 CHECK(centre_fills==expected_fills,"computed reset and transparent borders never restore native fill");
 CHECK(control==1&&label==1,"one control and one button label reach real painter");
 if(focused)CHECK(ring>0,"focused transparent control retains an outside focus ring");
 fc_reset();focus_reset();layout_free();dom_free(root);
}
int main(void){
 specimen("default","",0,0,1,0xf6f7f8);
 specimen("reset","button{background:transparent;border:0;border-radius:0}",0,0,0,0);
 specimen("none shorthand","button{background:none;border:none}",0,0,0,0);
 specimen("custom","button{background:#123456;border:3px solid #aabbcc;border-radius:0}",0,0,1,0x123456);
 specimen("transparent border","button{background:transparent;border:3px solid #aabbcc;border-radius:8px}",0,0,0,0);
 specimen("disabled default","",1,0,1,0xf1f2f4);
 specimen("disabled reset","button:disabled{background:transparent;border:0}",1,0,0,0);
 specimen("focused reset","button{background:transparent;border:0}",0,1,0,0);
 puts(fail?"control-chrome: FAIL":"control-chrome: PASS");return fail?1:0;
}
