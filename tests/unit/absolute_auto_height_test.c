/* Actual card-shaped rules, with no image request or video playback. The
 * decoded pixels are independent of the geometry reserved before fetching. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
static int ypos(const char *id){int x,y,w,h;CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"position box exists");return y;}
int main(void){
 css_init();css_viewport(400,600);
 page("<style>body{margin:0}.card{width:320px}.image{position:relative}.wrap{padding-top:56.25%}.cover{position:absolute;top:0;left:0;width:100%;height:100%;overflow:hidden}.cover img{display:block;width:100%;height:100%}.stats{position:absolute;bottom:0;left:0;width:100%;height:38px}.info{margin-top:8px;height:44px}</style><div class='card'><a><div id='image' class='image'><div class='wrap'><picture id='cover' class='cover'><img id='img' src='unfetched.png'></picture></div><div id='stats' class='stats'>STATS</div></div></a><div id='info' class='info'>TITLE</div></div>",400);
 EQ(height("image"),180,"ratio padding reserves natural cover height");
 EQ(height("cover"),180,"absolute percent uses auto containing block used height");
 EQ(height("img"),180,"nested image percentage uses resolved cover height");
 EQ(ypos("stats"),142,"bottom anchored statistics fit inside auto card");
 EQ(ypos("info"),188,"title follows ratio frame plus authored margin");
 int image_count=0,clip=0;const struct item *it=layout_items();
 for(int i=0;i<layout_count();i++)if(it[i].type==IT_IMAGE&&it[i].node==ID("img")){image_count++;clip=it[i].has_clip&&it[i].clip_h==180;}
 EQ(image_count,1,"measurement leaves exactly one image item");CHECK(clip,"resolved absolute cover clips descendant image");
 page("<style>body{margin:0}#outer{position:relative;padding:10px;border:2px solid;width:200px}#flow{height:100%}#abs{position:absolute;top:0;height:100%;width:10px}#fixed{position:fixed;top:7px;height:50%;width:10px}</style><div id='outer'><div id='flow'><div style='height:40px'></div></div><div><div id='abs'></div><div id='fixed'><div id='fixedchild' style='height:50%'></div></div></div></div>",400);
 EQ(height("flow"),40,"normal flow percent still stops at auto ancestor");EQ(height("outer"),64,"outer natural border box excludes absolute children");EQ(height("abs"),60,"absolute height uses used padding box excluding borders");EQ(height("fixed"),300,"nested fixed uses viewport height");EQ(height("fixedchild"),150,"fixed descendant retains definite percentage basis");EQ(ypos("fixed"),7,"nested fixed retains viewport top");
 page("<style>body{margin:0}#outer{position:relative}#inner{position:relative}#a,#b{position:absolute;bottom:0;height:50%;width:10px}</style><div id='outer'><div style='height:20px'></div><div id='inner'><div style='height:60px'></div><div id='b'></div></div><div id='a'></div></div>",400);
 EQ(height("a"),40,"outer absolute child uses outer natural height");EQ(height("b"),30,"nested positioning establishes independent used height");EQ(ypos("a"),40,"outer bottom anchor uses full normal flow");EQ(ypos("b"),50,"nested bottom anchor uses inner origin");
 page("<style>body{margin:0}#outer{position:relative;height:auto;max-height:50px;overflow:hidden}#a{position:absolute;top:0;height:100%;width:20px;background:red}#b{position:absolute;top:0;height:20px;width:20px;background:blue;z-index:2}</style><div id='outer'><div style='height:100px'></div><div id='a'></div><div id='b'></div></div>",400);
 EQ(height("a"),50,"auto maximum constrains absolute percentage basis");
 it=layout_items();int ai=-1,bi=-1;for(int i=0;i<layout_count();i++)if(it[i].type==IT_RECT){if(it[i].node==ID("a"))ai=i;if(it[i].node==ID("b"))bi=i;}
 CHECK(ai>=0&&bi>ai,"absolute sibling stacking survives measurement rollback");CHECK(ai>=0&&it[ai].has_clip&&it[ai].clip_h==50,"auto max-height clipping applies to absolute ink");
 layout_page(g_root,400);EQ(height("a"),50,"repeated layout derives height from current flow");
 page("<style>body{margin:0}#outer{position:relative;overflow:hidden}#ink{position:absolute;top:0;height:80px;width:20px;background:red}</style><div id='outer'><div style='height:40px'></div><div id='ink'></div></div>",400);
 it=layout_items();clip=0;for(int i=0;i<layout_count();i++)if(it[i].node==ID("ink")&&it[i].type==IT_RECT)clip=it[i].has_clip&&it[i].clip_h==40;
 CHECK(clip,"auto containing block clips absolute overflow at natural height");
 /* A full-width absolute flex layer must retain a nested picture's fallback
  * image, even before decoded pixels exist. Count actual image items, not the
  * picture box or a successful resource call. This isolates layout ownership
  * from responsive source selection and from real-site animation callbacks. */
 const char *gaps[]={"", "\n\t", "<!-- formatting -->\n "};
 for(int gi=0;gi<3;gi++) {
 char specimen[1800];
 snprintf(specimen,sizeof specimen,"<style>body{margin:0}#outer{position:relative;display:flex;justify-content:center;align-items:flex-start;width:100%%}#copy{position:relative;padding-top:160px;padding-bottom:20px;display:flex;flex-direction:column;align-items:center}#layer{position:absolute;inset:0;display:flex;justify-content:center}picture,img{display:block;width:auto;height:100%%}img{height:200px}</style><div id=outer><div id=copy>Title</div>%s<div id=layer><picture id=picture><source srcset=alternate.png media='(width<200px)'><img id=hero src=fallback.png></picture></div></div>",gaps[gi]);
 page(specimen,400);
 image_count=0;it=layout_items();
 for(int i=0;i<layout_count();i++)if(it[i].type==IT_IMAGE&&it[i].node==ID("hero"))image_count++;
 EQ(image_count,1,"whitespace cannot consume absolute flex image layer");
 EQ(width("layer"),400,"absolute flex image layer fills opposing insets");
 EQ(height("hero"),200,"nested fallback image keeps authored pixel height");
 }
 printf("absolute-auto-height: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
