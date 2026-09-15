/* Native control children must use computed border/padding, including resets.
 * Percentage spans exercise the content box without depending on SVG parsing. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
static int pos(const char *id,int k){int v[4];CHECK(layout_node_box(ID(id),v,v+1,v+2,v+3),"button fixture box exists");return v[k];}
static void button(const char *rule,const char *outer){
 char html[2048];snprintf(html,sizeof html,"<style>body{margin:0;font-size:14px}button{width:24px;height:24px;padding:0;border:0;box-sizing:border-box}#icon{display:inline-block;width:100%%;height:4px;background:red}%s</style>%s<button id=button><span id=icon></span></button></div>",rule,outer);page(html,240);
}
int main(void){
 css_init();css_viewport(240,200);
 const char *modes[]={"","button{display:block}","button{display:inline-block}","#host{display:flex}","#host{display:grid;grid-template-columns:24px}"};
 for(int i=0;i<5;i++){
  button(modes[i],"<div id=host>");
  EQ(width("button"),24,"authored native button border width retained");
  EQ(width("icon"),24,"zero-padding button exposes full content width");
  EQ(pos("icon",0),pos("button",0),"zero-padding button has no hidden horizontal inset");
  EQ(pos("icon",1),pos("button",1),"zero-padding button has no hidden vertical inset");
 }
 button("button{width:12px;height:12px}","<div>");EQ(width("icon"),12,"small button does not collapse child to one pixel");
 button("button{width:60px;height:40px;padding:3px 7px 5px 11px;border:2px solid}","<div>");
 EQ(width("icon"),38,"asymmetric padding subtracts each actual edge once");
 EQ(pos("icon",0)-pos("button",0),13,"content origin includes left padding and border");
 EQ(pos("icon",1)-pos("button",1),5,"content origin includes top padding and border");
 button("button{width:100px;height:60px;padding:5%;border:0}","<div>");
 EQ(width("icon"),76,"percentage control padding resolves against containing width");
 EQ(pos("icon",0)-pos("button",0),12,"percentage padding origin uses containing block");
 printf("button-content-box: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
