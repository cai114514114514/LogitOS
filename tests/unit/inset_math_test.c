/* Real LibCSS + extension producer + layout; deterministic host glyphs only.
 * These are not timing measurements or proof that a site's dialog works. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
static int xy(const char *id,int axis){int x,y,w,h;CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"inset box exists");return axis?y:x;}
static void release_page(void){dom_free(g_root);g_root=NULL;}
int main(void)
{
 css_init();css_viewport(400,300);
 page("<!doctype html><style>:root{--space:.25rem;font-size:20px}body{margin:0}#n{position:fixed;inset:calc(var(--space)*0)}</style><div style='height:90px'></div><div id=n></div>",400);
 EQ(xy("n",1),0,"calculated zero inset anchors overlay above flow");
 EQ(width("n"),400,"calculated zero inset stretches viewport width");
 EQ(height("n"),300,"calculated zero inset stretches viewport height");release_page();
 page("<!doctype html><style>html{font-size:20px}body{margin:0}#n{font-size:30px;position:fixed;inset:calc(.25rem * 2) calc(1em / 2) calc(10% + 5px) calc(5vw + 3px)}</style><div id=n></div>",400);
 EQ(xy("n",0),23,"viewport length and pixel sum");EQ(xy("n",1),10,"rem uses computed root font");EQ(width("n"),362,"em uses matched element font");EQ(height("n"),255,"percent and pixels retain different bases");
 release_page();
 page("<!doctype html><style>body{margin:0}#n{position:fixed;inset:calc(10% + 3px) calc(20% - 2px)}</style><div id=n></div>",400);
 EQ(xy("n",0),78,"horizontal percentage uses width");EQ(xy("n",1),33,"vertical percentage uses height");
 css_viewport(800,600);layout_page(g_root,800);
 EQ(xy("n",0),158,"mixed horizontal percent recomputes at layout resize");EQ(xy("n",1),63,"mixed vertical percent recomputes at layout resize");release_page();css_viewport(400,300);
 page("<!doctype html><style>body{margin:0}#n{position:fixed;inset:CALC((1e1px + calc(6px / 2)) * -2) 0 auto}</style><div id=n style='height:20px'></div>",400);
 EQ(xy("n",1),-26,"nested arithmetic exponent and negative multiplier");EQ(height("n"),20,"auto edge does not constrain explicit height");release_page();
 page("<!doctype html><style>body{margin:0}#n{position:fixed;inset:9px;inset-block-start:calc(2px + 3px)!important;inset:7px}</style><div id=n></div>",400);
 EQ(xy("n",1),5,"logical important math survives later shorthand");EQ(xy("n",0),7,"normal shorthand still updates independent edges");release_page();
 page("<!doctype html><style>body{margin:0}#n{position:fixed;inset:10%;inset:auto}</style><div style='height:37px'></div><div id=n style='width:20px;height:20px'></div>",400);
 EQ(xy("n",1),37,"auto shorthand clears prior percentage kind");release_page();
 const char *bad[]={"1px nope","1px 2px 3px 4px 5px","calc(1px + 2)","calc(0)","calc(0 * 1px * 1px)","calc(2px / 0)","calc(1px+ 2px)","calc(1px +2px)","calc(1px)junk","calc(1px +)","3","calc(1s * 2)","calc(1px / 2px)","calc(1e999px)","calc(1px/**/+ 2px)"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){
  char html[2048];snprintf(html,sizeof html,"<!doctype html><style>body{margin:0}#n{position:fixed;inset:9px;inset:%s}</style><div id=n></div>",bad[i]);page(html,400);
  EQ(xy("n",1),9,"invalid entire shorthand preserves previous value");EQ(xy("n",0),9,"invalid shorthand cannot partially replace edges");release_page();
 }
 page("<!doctype html><style>body{margin:0}#n{position:fixed;inset:calc(1px + /*comment*/ 2px) 0}</style><div id=n></div>",400);
 EQ(xy("n",1),3,"comments inside arithmetic do not become numeric tokens");release_page();
 page("<!doctype html><style>body{margin:0}#n{position:fixed;inset:1in 72pt 2.54cm 25.4mm}</style><div id=n></div>",400);
 EQ(xy("n",0),96,"absolute length units share CSS reference pixel");EQ(xy("n",1),96,"inches use CSS not physical display dpi");EQ(width("n"),208,"absolute shorthand right conversion");EQ(height("n"),108,"absolute shorthand bottom conversion");release_page();
 page("<!doctype html><style>body{margin:0}.n{position:fixed;inset:calc(.25em * 4)}#a{font-size:20px}#b{font-size:30px}</style><div class=n id=a></div><div class=n id=b></div>",400);
 EQ(xy("a",0),20,"shared compiled span uses first element font");EQ(xy("b",0),30,"shared compiled span uses second element font");release_page();
 page("<!doctype html><style>body{margin:0}#n{position:fixed;inset:calc(.25px * 4) calc(10vmin) calc(10vmax)}</style><div id=n></div>",400);
 EQ(xy("n",1),1,"subpixel arithmetic rounds only after multiplication");EQ(xy("n",0),30,"vmin chooses viewport smaller dimension");EQ(height("n"),259,"three component shorthand maps bottom independently");release_page();
 for(int kind=0;kind<2;kind++){
  char html[8192],expr[4096];int used=0;
  if(!kind){for(int j=0;j<40;j++)used+=snprintf(expr+used,sizeof expr-used,"calc(");used+=snprintf(expr+used,sizeof expr-used,"1px");for(int j=0;j<40;j++)expr[used++]=')';expr[used]=0;}
  else {used=snprintf(expr,sizeof expr,"calc(1px");for(int j=0;j<270;j++)used+=snprintf(expr+used,sizeof expr-used," + 1px");snprintf(expr+used,sizeof expr-used,")");}
  snprintf(html,sizeof html,"<!doctype html><style>body{margin:0}#n{position:fixed;inset:9px;inset:%s}</style><div id=n></div>",expr);page(html,400);
  EQ(xy("n",1),9,"expression depth and work limits preserve earlier declaration");release_page();
 }
 printf("inset-math: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
