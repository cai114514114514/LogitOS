/* SPDX-License-Identifier: MIT
 * Real stylesheet -> computed margins -> flex placement, with fixed host
 * glyphs. Guest pixels/native activation remain separate integration checks. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
static int coord(const char *id,int axis){int x,y,w,h;CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"margin box exists");return axis?y:x;}
static void release(void){dom_free(g_root);g_root=NULL;}
static struct cstyle *style(const char *id){return ID(id)->style;}
int main(void)
{
 css_init();css_viewport(400,300);
 page("<!doctype html><style>body{margin:0}#wrap{height:300px;display:flex;justify-content:center}#panel{width:100px;height:60px;margin-block:auto}</style><div id=wrap><div id=panel></div></div>",400);
 EQ(style("panel")->margin_auto&5,5,"logical block auto preserves both edge kinds");
 EQ(coord("panel",1),120,"logical auto centers within explicit-height flex");release();
 page("<!doctype html><style>body{margin:0}#wrap{position:fixed;inset:0;display:flex;justify-content:center}#panel{width:100px;height:60px;margin-top:auto;margin-bottom:auto}</style><div id=wrap><div id=panel></div></div>",400);
 EQ(coord("panel",1),120,"opposing insets supply definite flex cross height");EQ(height("wrap"),300,"fixed flex container retains its viewport extent");release();
 page("<!doctype html><style>body{margin:0}#wrap{position:fixed;inset:20px 30px;display:flex;justify-content:center;padding:10px;border:2px solid}#panel{width:100px;height:60px;margin-block:auto}</style><div id=wrap><div id=panel></div></div>",400);
 EQ(coord("panel",0),150,"inset and padded flex horizontal center");EQ(coord("panel",1),120,"logical auto uses definite inset content height");release();
 page("<!doctype html><style>body{margin:0}#wrap{height:300px;display:flex;flex-direction:column}#panel{width:100px;height:60px;margin-inline:auto;margin-block:auto}</style><div id=wrap><div id=panel></div></div>",400);
 EQ(coord("panel",0),150,"logical inline auto centers column cross axis");EQ(coord("panel",1),120,"logical block auto centers column main axis");release();
 page("<!doctype html><style>body{margin:0}#wrap{position:fixed;inset:0;display:flex;flex-direction:column}#panel{width:100px;height:60px;margin-inline:auto;margin-block:auto}</style><div id=wrap><div id=panel></div></div>",400);
 EQ(coord("panel",1),120,"inset-sized column enters definite main-axis solver");release();
 page("<!doctype html><style>body{margin:0}#wrap{position:relative;height:240px}#inner{position:absolute;inset:20px;display:flex;align-items:center}#panel{width:100px;height:60px}</style><div id=wrap><div id=inner><div id=panel></div></div></div>",400);
 EQ(coord("panel",1),90,"absolute flex center uses containing block rather than viewport");release();
 page("<!doctype html><style>body{margin:0}#wrap{display:flex}#panel{width:100px;height:60px;margin-block:auto}</style><div id=wrap><div id=panel></div></div>",400);
 EQ(coord("panel",1),0,"auto-sized flex does not invent viewport free space");EQ(height("wrap"),60,"auto-sized flex remains content-sized");release();
 page("<!doctype html><style>body{margin:0}#wrap{height:0;display:flex;align-items:center}#panel{width:100px;height:60px;margin-block:auto}</style><div id=wrap><div id=panel></div></div>",400);
 EQ(coord("panel",1),0,"overflowing auto margins retain start edge at zero height");EQ(height("wrap"),0,"zero is a definite flex height");release();
 page("<!doctype html><style>body{margin:0}#n{margin-block:12.5% calc(10% + 5px);margin-inline:auto -1px}</style><div id=n></div>",400);
 EQ(css_margin_px(ID("n")->style,0,400),50,"logical block percent resolves against inline width");
 EQ(css_margin_px(ID("n")->style,2,400),45,"mixed logical margin retains percentage and pixel offset");
 EQ(css_margin_px(ID("n")->style,2,800),85,"mixed margin recomputes without accumulating used pixels");
 EQ(style("n")->margin_auto&10,8,"two-value logical inline shorthand keeps auto and negative distinct");
 EQ(css_margin_px(ID("n")->style,1,400),-1,"minus one pixel is not auto");release();
 page("<!doctype html><style>html{font-size:20px}#a,#b{margin-inline:calc(.5em + 1rem) 2vh}#a{font-size:10px}#b{font-size:30px}</style><div id=a></div><div id=b></div>",400);
 EQ(css_margin_px(ID("a")->style,3,400),25,"logical length uses first matched font and root font");
 EQ(css_margin_px(ID("b")->style,3,400),35,"shared raw logical length uses each matched font");
 EQ(css_margin_px(ID("a")->style,1,400),6,"logical viewport units use real viewport");release();
 page("<!doctype html><style>#n{margin-block-start:auto!important;margin-block:12px}#n{margin-block-end:5%;margin-block:7px}</style><div id=n style='margin-block-start:9px'></div>",400);
 EQ(style("n")->margin_auto&1,1,"important logical auto survives normal inline value");EQ(css_margin_px(ID("n")->style,2,400),7,"later shorthand clears earlier percentage kind");release();
 page("<!doctype html><style>#n{margin-inline:auto!important}</style><div id=n></div>",400);
 EQ(style("n")->margin_auto&10,10,"important-only logical rule survives compiled rule admission");release();
 const char *controls[]={"<button id=n>Close</button>","<input id=n>","<textarea id=n></textarea>","<select id=n><option>One</option></select>","<img id=n width=100 height=60>"};
 for(unsigned i=0;i<sizeof controls/sizeof *controls;i++) {
  char html[2048];snprintf(html,sizeof html,"<!doctype html><style>body{margin:0}#n{display:block;width:100px;margin-inline:auto}</style>%s",controls[i]);page(html,400);
  EQ(coord("n",0),(400-width("n"))/2,"block controls and images consume auto margins after sizing");release();
 }
 page("<!doctype html><style>body{margin:0}#n{display:block;width:100px;margin-left:auto;margin-right:20px}</style><button id=n>Close</button>",400);
 EQ(coord("n",0),400-width("n")-20,"one auto margin aligns replaced block against fixed end margin");release();
 const char *bad[]={"auto nope","1px 2px 3px","autofoo","calc(1px + 2)","calc(0)","calc(1px * 2px)","(1px)","1pxjunk","calc(1px / 0)","1px auto extra"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){
  char html[2048];snprintf(html,sizeof html,"<!doctype html><style>#n{margin-inline:9px;margin-inline:%s}</style><div id=n></div>",bad[i]);page(html,400);
  EQ(css_margin_px(ID("n")->style,3,400),9,"invalid complete logical shorthand preserves prior start");
  EQ(css_margin_px(ID("n")->style,1,400),9,"invalid complete logical shorthand preserves prior end");release();
 }
 printf("logical-margin: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
