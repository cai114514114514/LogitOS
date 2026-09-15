/* Native button chrome with ordinary inner flex layout. Glyph advances are
 * deterministic; inline SVG is decoded by the real image/gfx pipeline. */
#define main intrinsic_original_main
#define img_decode intrinsic_unused_img_decode
#define img_free intrinsic_unused_img_free
#include "intrinsic_test.c"
#undef img_free
#undef img_decode
#undef main

static void finish(void) { layout_free(); dom_free(g_root); g_root=0; }
static int desc(struct node *n,struct node *p) { for(;n;n=n->parent)if(n==p)return 1;return 0; }
static void box(const char *id,int *b) { CHECK(layout_node_box(ID(id),b,b+1,b+2,b+3),"element box exists"); }
static void scene(const char *host,const char *rule,int svg,int raw)
{
    char html[4096];
    snprintf(html,sizeof html,"<style>body{margin:0;font-size:13px;line-height:20px}"
        "#host{width:500px;%s}button{display:inline-flex;font-size:13px;line-height:20px;"
        "padding:6px 12px;border:1px solid;box-sizing:border-box;gap:8px;align-items:center;justify-content:flex-start;%s}"
        "#icon{width:16px;height:16px;flex:0 0 auto}#label{white-space:nowrap}"
        "</style><div id=host><button id=b>%s%s</button></div>",host,rule,
        svg?"<svg id=icon width=16 height=16 viewBox='0 0 16 16'><rect width=16 height=16 fill='#00ff00'/></svg>":"<span id=icon></span>",
        raw?"Alpha Beta":"<span id=label>Alpha Beta</span>");
    page(html,600);
}
static void control(const char *id)
{
    int b[4],n=0;box(id,b);struct node *node=ID(id);
    const struct item *list=layout_items();
    for(int i=0;i<layout_count();i++)if(list[i].type==IT_CONTROL&&list[i].node==node){
        n++;EQ(list[i].x,b[0],"native control x matches box");EQ(list[i].y,b[1],"native control y matches box");
        EQ(list[i].w,b[2],"native control width matches box");EQ(list[i].h,b[3],"native control height matches box");
    }
    EQ(n,1,"exactly one native control");
}
static void text_inside(void)
{
    int b[4],n=0,first=-1,last=-1;box("b",b);struct node *node=ID("b");
    const struct item *list=layout_items();
    for(int i=0;i<layout_count();i++)if(list[i].type==IT_TEXT&&desc(list[i].node,node)){
        const struct item *it=list+i;n++;if(first<0)first=it->y;last=it->y;
        CHECK(it->x>=b[0]+13&&it->x+it->w<=b[0]+b[2]-13,"label inside horizontal content box");
        CHECK(it->y>=b[1]+7&&it->y+it->h<=b[1]+b[3]-7,"label inside vertical content box");
    }
    CHECK(n>0,"real label text emitted");EQ(first,last,"label remains on one line");
}
static void svg_image(void)
{
    const struct item *list=layout_items();int count=0,pixels=0,bad=0;
    for(int i=0;i<layout_count();i++)if(list[i].type==IT_IMAGE&&list[i].node==ID("icon")){
        count++;EQ(list[i].w,16,"SVG display item width");EQ(list[i].h,16,"SVG display item height");
        const struct image *img=list[i].img;CHECK(img&&img->rgba,"SVG real raster exists");
        if(img&&img->rgba)for(int p=0;p<img->w*img->h;p++){
            const unsigned char *q=img->rgba+4*p;
            if(q[3]){pixels++;if(q[0]||q[1]!=255||q[2]||q[3]!=255)bad++;}
        }
    }
    EQ(count,1,"exactly one SVG image item");EQ(pixels,256,"SVG rectangle has 256 opaque pixels");EQ(bad,0,"SVG rectangle uses real green pixels");
}
static void geometry(int column,int svg)
{
    int b[4],icon[4],label[4];box("b",b);box("icon",icon);box("label",label);
    EQ(b[2],column?86:110,"inner flex intrinsic button width");
    EQ(b[3],column?58:34,"inner flex natural button height");
    EQ(icon[2],16,"icon keeps 16px width");EQ(icon[3],16,"icon keeps 16px height");
    EQ(label[2],60,"label keeps full max-content width");EQ(label[3],20,"label keeps one line-height");
    if(column){EQ(icon[0]-b[0],35,"column icon cross-axis center");EQ(icon[1]-b[1],7,"column icon content start");
        EQ(label[0]-b[0],13,"column label cross-axis center");EQ(label[1]-icon[1]-icon[3],8,"column authored item gap");}
    else{EQ(icon[0]-b[0],13,"row icon content start");EQ(icon[1]-b[1],9,"row icon cross-axis center");
        EQ(label[0]-icon[0]-icon[2],8,"row authored item gap");EQ(label[1]-b[1],7,"row label cross-axis center");}
    control("b");text_inside();if(svg)svg_image();
}
int main(void)
{
    css_init();css_viewport(600,400);
    const char *hosts[]={"","display:flex;align-items:flex-start","display:grid;grid-template-columns:max-content;align-items:start"};
    for(int svg=0;svg<2;svg++)for(int column=0;column<2;column++)for(int host=0;host<4;host++){
        printf("case host=%d column=%d svg=%d\n",host,column,svg);
        scene(hosts[host==3?0:host],host==3?(column?"display:flex;flex-direction:column":"display:flex"):(column?"flex-direction:column":""),svg,0);geometry(column,svg);finish();
    }
    for(int host=0;host<3;host++) {
        scene(hosts[host],"flex-direction:column;max-height:40px",0,0);
        EQ(height("b"),40,"auto column honors max-height in each parent");control("b");finish();
        scene(hosts[host],"flex-direction:column;min-height:70px",0,0);
        EQ(height("b"),70,"auto column honors min-height in each parent");control("b");finish();
    }
    scene("","flex-direction:column;height:100%",0,0);
    EQ(height("b"),58,"indefinite percentage height remains natural auto");control("b");finish();
    for(int cb=0;cb<2;cb++){
        scene("display:flex;align-items:flex-start",cb?"width:120px;height:40px;box-sizing:content-box":"width:146px;height:54px",1,0);
        EQ(width("b"),146,"explicit CSS width counts edges once");EQ(height("b"),54,"explicit CSS height preserved");control("b");text_inside();svg_image();finish();
    }
    scene("display:flex;height:90px;align-items:stretch","",0,0);
    EQ(height("b"),90,"parent flex allocated height retained");
    {int b[4],i[4],l[4];box("b",b);box("icon",i);box("label",l);EQ(i[1]-b[1],37,"parent stretch centers icon");EQ(l[1]-b[1],35,"parent stretch centers label");}control("b");finish();
    page("<style>body{margin:0;font-size:16px}#host{display:flex}#i{width:140px;height:36px;padding:3px;border:2px solid;box-sizing:border-box}</style><div id=host><input id=i value=Hello><input id=j size=20></div>",600);
    EQ(width("i"),140,"text input explicit width unchanged");EQ(height("i"),36,"text input explicit height unchanged");EQ(width("j"),172,"text input auto width unchanged");control("i");control("j");finish();
    printf("button-inner-flex: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
