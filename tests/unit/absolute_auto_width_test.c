/* Positioned auto widths through the real DOM/CSS/layout path. Fixed glyphs
 * isolate geometry; native image bytes, painting and clicks are separate. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main

static int coord(const char *id,int axis)
{ int x,y,w,h;CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"positioned box exists");return axis?y:x; }
static void clear_page(void){layout_free();if(g_root)dom_free(g_root);g_root=NULL;}
static void specimen(const char *rules,const char *markup)
{
 char html[8192];clear_page();
 snprintf(html,sizeof html,"<!doctype html><style>body{margin:0;font-size:16px;line-height:20px}%s</style>%s",rules,markup);
 page(html,400);
}
int main(void)
{
 css_init();css_viewport(400,300);
 specimen("#a{position:absolute;left:10px}","<div id=a>small tag</div>");
 EQ(width("a"),72,"one-inset auto box uses max-content not containing width");
 EQ(coord("a",0),10,"left anchored shrink box retains its inset");
 specimen("#a{position:fixed;right:17px}","<div id=a>small tag</div>");
 EQ(width("a"),72,"right anchored fixed box also shrinks to content");
 EQ(coord("a",0),311,"right inset uses the computed shrink width");
 specimen("#a{position:absolute;left:10px;right:17px}","<div id=a>small tag</div>");
 EQ(width("a"),373,"nonreplaced opposing insets still stretch auto width");
 specimen("#a{position:absolute;left:10px;right:17px;width:100px}","<div id=a>small tag</div>");
 EQ(width("a"),100,"explicit positioned width is unchanged");
 specimen("#wrap{margin-left:50px;width:300px}#a{position:absolute}","<div id=wrap><div style='height:20px'></div><div id=a>small tag</div></div>");
 EQ(width("a"),72,"all-auto positioned box measures its own content");
 EQ(coord("a",0),50,"all-auto width preserves ordinary static x");
 EQ(coord("a",1),20,"all-auto width preserves preceding flow y");
 specimen("#wrap{position:relative;width:120px}#a{position:absolute;left:50px}","<div id=wrap><div id=a>longer words</div></div>");
 EQ(width("a"),70,"shrink width is bounded by available space above min-content");
 specimen("#wrap{position:relative;width:120px}#a{position:absolute;left:110px}","<div id=wrap><div id=a>unbreakable</div></div>");
 EQ(width("a"),88,"min-content may overflow narrower available space");
 specimen("#a{position:absolute;left:10px;padding:10px;border:2px solid}","<div id=a>word</div>");
 EQ(width("a"),56,"positioned preferred size counts border and padding once");
 specimen("#a{position:absolute;left:10px;padding:0 10%}","<div id=a>word</div>");
 EQ(width("a"),112,"percentage padding uses containing width before shrinking");
 layout_page(g_root,800);EQ(width("a"),192,"positioned percentage padding is recomputed on resize");
 layout_page(g_root,400);EQ(width("a"),112,"resize does not accumulate used percentage padding");
 specimen("#a{position:absolute;left:10px;width:100px;padding:0 10%}","<div id=a>word</div>");
 EQ(width("a"),180,"explicit content width adds padding resolved against its container");
 specimen("#wrap{position:relative;width:150px}#a{position:absolute;left:10px;margin:0 30px 0 20px}","<div id=wrap><div id=a>longer words</div></div>");
 EQ(width("a"),90,"available shrink width subtracts both fixed margins");
 EQ(coord("a",0),30,"left margin offsets the final shrink box");
 specimen("#a{position:absolute;left:300px;min-width:50%}","<div id=a>small tag</div>");
 EQ(width("a"),200,"min-width percentage uses containing block not leftover space");
 specimen("#a{position:absolute;right:17px;max-width:10%}","<div id=a>small tag</div>");
 EQ(width("a"),40,"max-width applies after preferred width calculation");
 EQ(coord("a",0),343,"right anchor is solved after max-width constraint");
 specimen("#a{position:absolute;left:10px}#child{position:absolute;width:1000px}","<div id=a>ok<div id=child>ignored</div></div>");
 EQ(width("a"),16,"only the measured out-of-flow root participates in intrinsic width");
 specimen("#a{position:absolute;left:10px}#child{display:none;width:1000px}","<div id=a>ok<div id=child>ignored</div></div>");
 EQ(width("a"),16,"hidden descendants never increase positioned preferred width");
 const char *tags[]={"<button id=%s>Close</button>","<input id=%s size=8>","<textarea id=%s cols=8></textarea>","<select id=%s><option>Choice</option></select>","<input id=%s type=checkbox>"};
 for(unsigned i=0;i<sizeof tags/sizeof *tags;i++) {
  char ref[256],abs[256],html[600];snprintf(ref,sizeof ref,tags[i],"ref");snprintf(abs,sizeof abs,tags[i],"a");snprintf(html,sizeof html,"%s%s",ref,abs);
  specimen("#ref{display:block}#a{position:fixed;right:17px;top:70px}",html);
  EQ(width("a"),width("ref"),"positioned auto control retains the ordinary intrinsic width");
  EQ(coord("a",0),400-17-width("a"),"positioned control right edge is independently anchored");
  specimen("#ref{display:block}#a{position:fixed;left:10px;right:17px;top:70px}",html);
  EQ(width("a"),width("ref"),"opposing insets do not stretch a replaced control");
 }
 specimen("#wrap{position:relative;width:150px}#a{position:absolute;right:0}","<div id=wrap><img id=a width=96 height=48></div>");
 EQ(width("a"),96,"positioned image preserves explicit intrinsic width attribute");
 EQ(coord("a",0),54,"positioned image right edge uses its intrinsic width");
 specimen("#a{position:fixed;left:10px;right:10px}","<canvas id=a width=96></canvas>");
 EQ(width("a"),96,"external replaced box uses HTML width without fallback children");
 specimen("#a{position:fixed;right:10px}","<canvas id=a></canvas>");
 EQ(width("a"),300,"external replaced box retains default intrinsic width");
 clear_page();printf("absolute-auto-width: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
