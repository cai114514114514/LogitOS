/* Real parser/style/layout geometry. Intrinsic's deterministic font host is
 * reused; no network image decoding or guest rendering is claimed here. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
int main(void) {
 css_init(); css_viewport(400,600);
 page("<style>body{margin:0}.half{height:50%}.full{height:100%}</style><div style='height:200px'><div id='half' class='half'><div id='full' class='full'></div></div><div style='height:auto'><div id='auto' class='full'><div style='height:37px'></div></div></div></div>",400);
 EQ(height("half"),100,"definite parent resolves percentage height");
 EQ(height("full"),100,"nested percentage chain remains definite");
 EQ(height("auto"),37,"auto ancestor stops percentage chain");
 page("<style>html{height:100%}body{margin:0;height:50%}</style><div id='rootchain' style='height:50%'></div>",400);
 EQ(height("rootchain"),150,"root percentage uses viewport through definite body");
 css_viewport(400,800);layout_page(g_root,400);
 EQ(height("rootchain"),200,"viewport resize recomputes percentage chain");
 css_viewport(400,600);layout_page(g_root,400);
 EQ(height("rootchain"),150,"repeated layout does not reuse previous box height");
 page("<style>body{margin:0}</style><div style='height:200px;padding:10px;border:2px solid;box-sizing:border-box'><div id='content' style='height:50%'></div><div id='calc' style='height:calc(50% - 10px)'></div><div id='minimum' style='height:10%;min-height:50%'></div></div>",400);
 EQ(height("content"),88,"normal percentage excludes parent border and padding");
 EQ(height("calc"),78,"calc height retains percentage addend");
 EQ(height("minimum"),88,"percentage minimum uses same definite basis");
 page("<style>body{margin:0}</style><div style='height:200px'><img id='blockimg' width='20' height='11' style='display:block;width:50%;height:50%'><span><img id='inlineimg' width='20' height='11' style='width:25%;height:25%'></span></div>",400);
 EQ(width("blockimg"),200,"block image percentage width");
 EQ(height("blockimg"),100,"block image percentage height overrides HTML attribute");
 EQ(width("inlineimg"),100,"inline image percentage width");
 EQ(height("inlineimg"),50,"inline image skips non-container inline ancestor");
 page("<style>body{margin:0}</style><div style='position:relative;height:200px;padding:10px;border:2px solid'><div style='height:17px'><div id='overlay' style='position:absolute;top:0;height:50%;width:100px'><div id='overlaychild' style='height:50%'></div></div></div></div>",400);
 EQ(height("overlay"),110,"absolute percentage uses positioned padding box");
 EQ(height("overlaychild"),55,"absolute percentage provides definite child basis");
 page("<style>body{margin:0}</style><div style='position:fixed;top:10px;bottom:30px;width:100px'><div id='stretchchild' style='height:50%'></div></div>",400);
 EQ(height("stretchchild"),280,"opposing fixed insets provide definite content height");
 page("<style>body{margin:0}</style><div style='height:200px'><div id='clip' style='height:50%;overflow:hidden'><div id='ink' style='height:180px;background:red'></div></div></div>",400);
 int clipped=0; const struct item *it=layout_items();
 for(int i=0;i<layout_count();i++) if(it[i].node==ID("ink")&&it[i].type==IT_RECT) clipped=it[i].has_clip&&it[i].clip_h==100;
 CHECK(clipped,"percentage overflow clips at resolved height");
 page("<style>body{margin:0}</style><div id='initial' style='position:absolute;top:0;height:25%;width:10px'></div><div style='position:fixed;top:10px;bottom:30px;width:100px'><div id='nestedabs' style='position:absolute;top:0;height:50%;width:10px'></div></div>",400);
 EQ(height("initial"),150,"absolute initial containing block uses viewport height");
 EQ(height("nestedabs"),280,"absolute child uses stretched parent padding height");
 page("<style>body{margin:0}</style><div style='height:0'><img id='zeroimg' width=40 height=20 style='display:block;height:100%'><img id='zeroinline' width=40 height=20 style='height:100%'></div>",400);
 EQ(height("zeroimg"),0,"definite zero block image never resurrects HTML height");
 EQ(height("zeroinline"),0,"definite zero inline image never resurrects HTML height");
 printf("percentage-height: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
