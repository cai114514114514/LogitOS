/* A real guest caret fixture exposed a missing consumer: direct flex controls
 * had border rectangles and working DOM values, but no IT_CONTROL. Check the
 * display list AND actual painted value; arithmetic-only caret gates cannot
 * detect a control that never reaches forms.c at all. */
#define main caret_geometry_unused_main
#include "modal_paint_test.c"
#undef main
#include "forms.h"
static void container(const char *display,int stretch) {
 char html[2048],css[1024];
 snprintf(html,sizeof html,"<!doctype html><div id=row><input id=f><textarea id=t></textarea><select id=s><option>Chosen</option><option>Unchosen</option></select><button id=b><span>Button</span></button></div>");
 snprintf(css,sizeof css,"body{margin:0}#row{display:%s;width:700px;gap:8px;align-items:center;grid-template-columns:repeat(4,160px)}input,textarea,select,button{width:140px;height:36px;padding:5px;border:2px solid green;font-size:16px}%s",display,stretch?"#row{align-items:stretch;height:90px}input,textarea,select,button{height:auto}":"");
 struct node *root=dom_parse(html,strlen(html));css_viewport(800,300);css_apply(root,css,strlen(css));css_extra_apply(root,css,strlen(css));layout_page(root,800);
 const char *ids[]={"f","t","s","b"};
 for(int k=0;k<4;k++) {
  struct node *n=dom_get_element_by_id(root->doc,ids[k]);int count=0,x,y,w,h;layout_node_box(n,&x,&y,&w,&h);
  for(int i=0;i<layout_count();i++){const struct item *it=layout_items()+i;if(it->node==n&&it->type==IT_CONTROL){count++;CHECK(it->x==x&&it->y==y&&it->w==w&&it->h==h,"control paint box agrees with final layout box");}}
  printf("container %s id=%s controls=%d box=%d,%d,%d,%d\n",display,ids[k],count,x,y,w,h);
  CHECK(count==1,"direct container child emits exactly one IT_CONTROL");
 }
 struct node *f=dom_get_element_by_id(root->doc,"f"),*t=dom_get_element_by_id(root->doc,"t");
 fc_set_value(f,"NativeValue",11);fc_set_value(t,"TextAreaValue",13);focus_set(f);paint_nops=0;browser_paint_scroll(0,0,800,300,0,0);
 int count;text_index("NativeValue",&count);CHECK(count==1,"direct container input value reaches real painter");
 text_index("TextAreaValue",&count);CHECK(count==1,"direct container textarea value reaches real painter");
 text_index("Unchosen",&count);CHECK(count==0,"select option children never leak into page text");
 text_index("Button",&count);CHECK(count==1,"button child markup paints exactly once");
 fc_reset();focus_reset();layout_free();dom_free(root);
}
int main(void){container("flex",0);container("grid",0);container("block",0);container("flex",1);container("grid",1);puts(fail?"form-control-container: FAIL":"form-control-container: PASS");return fail?1:0;}
