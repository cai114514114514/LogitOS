/* The decoder oracle supplies exact dimensions, not a PNG/JPEG compatibility
 * claim. Real DOM, CSS, layout, cache ownership and box/item agreement are the
 * product under test; native fixtures use actual decoded image bytes. */
#define img_decode intrinsic_unused_img_decode
#define img_free intrinsic_unused_img_free
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
#undef img_decode
#undef img_free
int img_decode(const uint8_t *p,int n,struct image *im)
{
 if(n!=8)return -1;memcpy(&im->w,p,4);memcpy(&im->h,p+4,4);
 if(im->w<1||im->w>1024||im->h<1||im->h>1024)return -1;
 im->rgba=calloc((size_t)im->w*im->h,4);return im->rgba?0:-1;
}
void img_free(struct image *im){free(im->rgba);im->rgba=0;}
static void decoded(const char *src,int w,int h)
{ int data[2]={w,h};CHECK(layout_img_store(src,(const unsigned char*)data,sizeof data),"dimension oracle populated real image cache"); }
static void fresh(const char *body)
{
 char html[4096];layout_free();if(g_root)dom_free(g_root);g_root=0;
 snprintf(html,sizeof html,"<style>body{margin:0;font-size:16px;line-height:20px}</style>%s",body);page(html,800);
}
static void size(const char *id,int w,int h)
{
 int x,y,bw=0,bh=0;struct node *n=ID(id);
 CHECK(layout_node_box(n,&x,&y,&bw,&bh),"decoded image box exists");EQ(bw,w,"decoded intrinsic border width");EQ(bh,h,"decoded intrinsic border height");
 int found=0;const struct item *it=layout_items();
 for(int i=0;i<layout_count();i++)if(it[i].node==n&&it[i].type==IT_IMAGE){found++;EQ(it[i].w,w,"image item width agrees with box");EQ(it[i].h,h,"image item height agrees with box");CHECK(it[i].img!=0,"real cache pixels attached");}
 EQ(found,1,"exactly one image item for replaced element");
}
int main(void)
{
 css_init();css_viewport(800,600);decoded("landscape",320,160);decoded("portrait",90,180);
 fresh("<img id=i src=landscape style='display:block'><div id=next>Title</div>");size("i",320,160);
 int x,y,w,h;layout_node_box(ID("next"),&x,&y,&w,&h);EQ(y,160,"following title uses decoded image height before placement");
 fresh("<span><img id=i src=landscape></span>");size("i",320,160);
 fresh("<img id=i src=landscape style='display:block;width:80px;height:auto'>");size("i",80,40);
 fresh("<img id=i src=landscape style='display:block;width:auto;height:100px'>");size("i",200,100);
 fresh("<img id=i src=landscape style='display:block;width:80px;height:30px'>");size("i",80,30);
 fresh("<div style='display:flex;justify-content:center;width:400px'><picture id=p style='display:block'><img id=i src=landscape style='display:block;width:auto;height:100px'></picture></div>");
 size("i",200,100);EQ(width("p"),200,"flex picture max-content comes from decoded ratio not 24px");
 fresh("<div style='display:grid;grid-template-columns:max-content'><picture id=p><img id=i src=portrait style='display:block;height:120px'></picture></div>");size("i",60,120);EQ(width("p"),60,"grid picture intrinsic width uses decoded ratio");
 fresh("<img id=i src=landscape style='display:block;max-width:100px'>");size("i",100,50);
 fresh("<img id=i src=landscape style='display:block;width:50%;height:auto'>");size("i",400,200);
 fresh("<div style='height:200px'><img id=i src=landscape style='display:block;height:50%;width:auto'></div>");size("i",200,100);
 fresh("<img id=i src=landscape style='display:block;width:0;height:auto'>");size("i",0,0);
 fresh("<img id=i src=landscape style='float:left;width:120px;height:auto'><div style='clear:both' id=next>Title</div>");size("i",120,60);
 layout_node_box(ID("next"),&x,&y,&w,&h);EQ(y,60,"float exclusion uses decoded height");
 fresh("<div style='display:flex'><img id=i src=landscape style='width:120px;height:auto'></div>");size("i",120,60);
 fresh("<div style='display:grid;grid-template-columns:160px'><img id=i src=landscape style='width:100%;height:auto'></div>");size("i",160,80);
 fresh("<img id=i src=landscape style='display:block;max-height:60px'>");size("i",120,60);
 fresh("<img id=i src=landscape style='display:block;min-width:400px'>");size("i",400,200);
 fresh("<img id=i src=landscape style='display:block;min-width:400px;max-height:60px'>");size("i",400,60);
 fresh("<img id=i src=late style='display:block'><div id=next>Title</div>");
 CHECK(!layout_image_geometry_pending(),"layout consumes existing image generation");
 decoded("late",200,50);CHECK(layout_image_geometry_pending(),"decode without DOM mutation invalidates geometry");
 layout_page(g_root,800);CHECK(!layout_image_geometry_pending(),"one reflow consumes decoded generation");size("i",200,50);
 layout_node_box(ID("next"),&x,&y,&w,&h);EQ(y,50,"late decoded size moves following title");
 CHECK(!layout_img_store("broken",(const unsigned char*)"bad",3),"bad decoder bytes are not a successful image");
 CHECK(!layout_image_geometry_pending(),"negative cache entry does not invent intrinsic geometry");
 printf("image-intrinsic: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
