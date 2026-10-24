/* Reuse the pixel recorder from the companion border/gradient gate. Its
 * decoder refusal is renamed: this host gate supplies deterministic RGBA to
 * exercise cache/load/paint, while the guest gate uses the shipping BMP codec.
 * No API-presence or draw-count assertion stands in for the sampled pixels. */
#define main background_companion_main
#define img_decode background_unused_decode
#define img_free background_unused_free
#include "css_background_test.c"
#undef img_free
#undef img_decode
#undef main
#include "css/background.h"
#include "../../include/weaksym.h"
int layout_img_cached(const char *);
void layout_images_reset(void);
#include "../../c/apps/gui/browser/app/background.c"

int img_decode(const uint8_t *data,int len,struct image *out)
{
    if(len!=1||data[0]!=42)return -1;
    memset(out,0,sizeof *out);out->w=4;out->h=2;out->rgba=malloc(4*4*2);
    if(!out->rgba)return -1;
    for(int y=0;y<2;y++)for(int x=0;x<4;x++){
        unsigned color=x<1?0xe72939:x<3?0x1fbf67:0x2549df;
        unsigned char *p=out->rgba+4*(y*4+x);p[0]=color>>16;p[1]=color>>8;p[2]=color;p[3]=255;
    }
    return 0;
}
void img_free(struct image *image){free(image->rgba);image->rgba=0;}
static int fetches,pending;
static int parse_url(const char *text,char *url,int capacity)
{return css_background_url(text,(int)strlen(text),url,capacity);}
static int fetch_bitmap(const char *url,unsigned char **data,int *len)
{
    fetches++;check(!strcmp(url,"tile.bmp"),"loader uses the captured CSS URL");
    if(pending){pending=0;return 1;}
    *data=malloc(1);(*data)[0]=42;*len=1;return 0;
}
static void render_page(struct node *root,const char *css)
{
    css_apply(root,css,(int)strlen(css));css_extra_apply(root,css,(int)strlen(css));layout_page(root,W);
    gui_clip(0,0,W,H);for(int y=0;y<H;y++)for(int x=0;x<W;x++)pixels[y][x]=0xffffff;
    browser_paint(0,0,W,H,0);
}
static void grammar(void)
{
    char url[80];
    check(parse_url("url('a)b.bmp')",url,sizeof url)&&!strcmp(url,"a)b.bmp"),"quoted closing parenthesis belongs to the URL");
    check(parse_url("url(a\\29 b.bmp)",url,sizeof url)&&!strcmp(url,"a)b.bmp"),"CSS hex escape is decoded without truncation");
    /* The one-URL decoder still refuses a list. Correction: image lists now
     * belong to css_background_layers, which consumes every URL separately. */
    check(!parse_url("url(a),url(b)",url,sizeof url),"one URL token reader refuses an image list");
    check(!parse_url("url(a",url,sizeof url),"unclosed URL is refused");
    check(!parse_url("url(abcdef)",url,4),"URL capacity refuses the whole value");
    check(!css_background_supports("background-size",15,"1.px",4),"a trailing decimal point is not a CSS number");
    check(!css_background_supports("background-position",19,"50% left",8),"numeric center is not an exchangeable center keyword");
    check(!css_background_supports("background-position",19,"top 50%",7),"vertical keyword cannot precede a horizontal numeric component");
    check(!css_background_supports("background-size",15,"auto0",5),"one identifier cannot become two size components");
    check(!css_background_supports("background-size",15,"1px2px",6),"one dimension cannot become two size components");
    const char *properties[]={"background-size","background-position","background-repeat"};
    const char *valid[]={"contain","bottom right","repeat-x"};
    const char *invalid[]={"-1px 2px","left right","repeat banana"};
    for(int i=0;i<3;i++){
        check(css_background_supports(properties[i],strlen(properties[i]),valid[i],strlen(valid[i])),"supported bitmap geometry parses fully");
        check(!css_background_supports(properties[i],strlen(properties[i]),invalid[i],strlen(invalid[i])),"invalid bitmap geometry is refused atomically");
    }
}
static void grammar_edges(void)
{
    const char *positions[]={"center right 10px","center bottom 10px"};
    const char *support_labels[]={"center-first horizontal edge position is supported",
                                  "center-first vertical edge position is supported"};
    const char *geometry_labels[]={"center-first right offset keeps the vertical center",
                                   "center-first bottom offset keeps the horizontal center"};
    for(int i=0;i<2;i++){
        struct cstyle style={0};struct bg_geometry geometry;
        style.font_px=16;style.xraw[XR_BG_POSITION]=positions[i];
        style.xrawlen[XR_BG_POSITION]=(int)strlen(positions[i]);
        check(css_background_supports("background-position",19,positions[i],strlen(positions[i])),support_labels[i]);
        int edge=i==0?0:1,center=1-edge;
        check(css_background_geometry(&style,&geometry)&&geometry.position[edge].percent==1&&
              geometry.position[edge].px==-10&&geometry.position[center].percent==.5&&
              geometry.position[center].px==0,geometry_labels[i]);
    }
    check(css_background_supports("background-position",19,"right 10px center",17)&&
          css_background_supports("background-position",19,"bottom 10px center",18),
          "center-last edge positions retain their existing grammar");
    const char *invalid[]={"left right 10px","top bottom 10px","center left top","center 10px right"};
    for(int i=0;i<4;i++)check(!css_background_supports("background-position",19,invalid[i],strlen(invalid[i])),
                            "same-axis or unattached-offset positions remain invalid");

    struct bg_declaration declaration;
    const char *spellings[]={" inherit ","/**/InHeRiT/**/","\tinherit /* trailing */ \n"};
    int longhand=1,shorthand=1;
    for(int i=0;i<3;i++){
        int ok=css_background_declaration("background-size",15,spellings[i],strlen(spellings[i]),&declaration);
        longhand&=ok&&declaration.value[BG_SIZE]&&declaration.len[BG_SIZE]==7&&
                  !memcmp(declaration.value[BG_SIZE],"inherit",7)&&!declaration.shorthand_bits;
        ok=css_background_declaration("background",10,spellings[i],strlen(spellings[i]),&declaration);
        shorthand&=ok&&!declaration.shorthand_bits;
        for(int slot=0;slot<BG_SLOTS;slot++)shorthand&=ok&&declaration.value[slot]&&
            declaration.len[slot]==7&&!memcmp(declaration.value[slot],"inherit",7);
    }
    check(longhand,"longhand inherit spelling publishes one stable canonical keyword");
    check(shorthand,"shorthand inherit spelling publishes canonical keywords for every slot");
    check(!css_background_declaration("background-size",15,"inh/**/erit",11,&declaration),
          "comments cannot join two identifiers into the inherit keyword");
    check(css_background_declaration("background-size",15," /**/ UnSeT /**/ ",17,&declaration)&&
          declaration.len[BG_SIZE]==4&&!memcmp(declaration.value[BG_SIZE],"auto",4),
          "commented mixed-case unset still publishes the initial size");

    /* These are production CSS -> layout -> gradient samples; a parser flag
     * alone cannot prove the center was assigned to the other axis. */
    const char *html="<div id=right></div><div id=bottom></div>";
    struct node *root=dom_parse(html,(int)strlen(html));
    render_page(root,"body{margin:0}div{position:absolute;top:0;width:60px;height:60px;background:red;"
                     "background-image:linear-gradient(blue,blue);background-size:20px 10px;background-repeat:no-repeat}"
                     "#right{left:0;background-position:center right 10px}"
                     "#bottom{left:80px;background-position:center bottom 10px}");
    pixel(35,27,0x0000ff,"center-first right position paints its horizontal edge offset");
    pixel(45,27,0x0000ff,"center-first right position paints the centered vertical band");
    pixel(105,42,0x0000ff,"center-first bottom position paints its vertical edge offset");
    pixel(95,42,0xff0000,"center-first bottom position preserves the horizontal gap");
    layout_free();dom_free(root);

    html="<div id=parent><div id=long></div><div id=short></div></div>";
    root=dom_parse(html,(int)strlen(html));
    render_page(root,"body{margin:0}#parent{position:absolute;left:0;top:80px;width:240px;height:90px;font-size:20px;"
                     "background:linear-gradient(blue,blue) 1em 2em / 2em 1em no-repeat red}"
                     "#long,#short{position:absolute;top:0;width:100px;height:80px;font-size:10px}"
                     "#long{left:0;background-color:white;background-image:inherit/**/;background-size:/**/INHERIT;"
                     "background-position: InHeRiT /**/;background-repeat:/**/inherit/**/}"
                     "#short{left:110px;background:/**/InHeRiT/**/}");
    struct bg_geometry geometry;const struct cstyle *style=find(root,"long")->style;
    check(css_background_geometry(style,&geometry)&&geometry.size[0].px==40&&geometry.size[1].px==20,
          "commented longhand inherit retains the parent computed size");
    check(css_background_geometry(style,&geometry)&&geometry.position[0].px==20&&geometry.position[1].px==40,
          "commented longhand inherit retains the parent computed position");
    style=find(root,"short")->style;
    check(css_background_geometry(style,&geometry)&&geometry.size[0].px==40&&geometry.position[0].px==20,
          "commented shorthand inherit retains the parent geometry and font basis");
    pixel(25,125,0x0000ff,"commented longhand inherit paints the parent image over its own color");
    pixel(135,125,0x0000ff,"commented shorthand inherit paints the parent image in the child box");
    layout_free();dom_free(root);layout_images_reset();
}
int main(int argc,char **argv)
{
    css_init();css_viewport(W,H);
    if(argc==2&&!strcmp(argv[1],"grammar")){
        grammar_edges();
        printf("css-background-grammar: %d checks, %d failures\n",checks,failures);
        return failures?1:0;
    }
    if(argc!=1)return 2;
    grammar();grammar_edges();
    const char *html="<div id=cover></div><div id=contain></div><div id=repeat></div>";
    struct node *root=dom_parse(html,(int)strlen(html));
    const char *css="body{margin:0}div{position:absolute;width:60px;height:60px;background-color:#dedc71;"
        "background-image:url(tile.bmp);background-repeat:no-repeat;background-position:center}"
        "#cover{left:10px;top:10px;background-size:cover}"
        "#contain{left:80px;top:10px;background-size:contain}"
        "#repeat{left:150px;top:10px;background-size:20px 10px;background-position:0 0;background-repeat:repeat}";
    render_page(root,css);pending=1;
    check(background_images_wants("tile.bmp"),"active CSS background retains its asynchronous request");
    check(background_images_pending(),"background-only page keeps the frame loader awake");
    check(background_images_load(1,fetch_bitmap)==0&&!layout_img_cached("tile.bmp"),"pending background does not create a negative cache entry");
    check(background_images_pending(),"pending background keeps its request alive until completion");
    check(background_images_load(1,fetch_bitmap)==1&&layout_img_get("tile.bmp"),"completed background uses the actual shared decoded cache");
    check(!background_images_pending(),"completed backgrounds release their frame drain");
    check(fetches==2,"one pending poll and one completion service all three boxes");
    check(background_images_load(8,fetch_bitmap)==0&&fetches==2,"repeated discovery reuses the cache without IO");
    render_page(root,css);
    pixel(12,40,0x1fbf67,"cover center crop samples the intrinsic picture");
    pixel(110,12,0xdedc71,"contain preserves the top background band");
    pixel(110,40,0x1fbf67,"contain paints its centered image");
    pixel(110,68,0xdedc71,"contain preserves the bottom background band");
    pixel(152,12,0xe72939,"repeat paints its first tile");
    pixel(172,22,0xe72939,"repeat paints the corresponding pixel in later tiles");
    pixel(162,42,0x1fbf67,"repeat keeps the authored tile scale");
    render_page(root,"body{margin:0}#cover{position:absolute;left:10px;top:10px;width:60px;height:60px;"
                     "background-image:url(tile.bmp);background-size:20px 10px;transform:scale(2);transform-origin:0 0}");
    pixel(12,12,0xe72939,"axis scale keeps the first image tile color");
    pixel(17,12,0xe72939,"axis scale enlarges authored bitmap tile lengths");
    pixel(37,12,0x1fbf67,"axis scale does not insert extra repeated tiles");
    render_page(root,"body{margin:0}#cover{position:absolute;left:10px;top:10px;width:60px;height:60px;"
                     "background-image:url(tile.bmp);mask-image:url(mask.svg)}");
    check(((const struct cstyle *)find(root,"cover")->style)->mask_refused,
          "unsupported mask retains the existing decoration refusal");
    check(!background_images_wants("tile.bmp"),"refused masked decoration does not retain bitmap IO");
    pixel(12,40,0xffffff,"cached bitmap does not revive an unimplemented masked icon");
    const char *reset="body{margin:0}div{width:60px;height:60px;background-image:url(tile.bmp)}#cover{background:red}#contain{background:none}";
    render_page(root,reset);
    struct node *cover=find(root,"cover"),*contain=find(root,"contain");char source[80];
    const struct cstyle *cover_style=cover->style,*contain_style=contain->style;
    check(!css_background_url(cover_style->xraw[XR_BG_IMAGE],cover_style->xrawlen[XR_BG_IMAGE],source,sizeof source),"later color shorthand removes a bitmap declaration");
    check(!css_background_url(contain_style->xraw[XR_BG_IMAGE],contain_style->xrawlen[XR_BG_IMAGE],source,sizeof source),"background shorthand resets the earlier image");
    layout_free();dom_free(root);layout_images_reset();
    const char *inherited="<div id=parent><div id=child></div></div>";
    root=dom_parse(inherited,(int)strlen(inherited));
    render_page(root,"#parent{font-size:20px;background-size:2em 3em;background-position:1em 2em}"
                     "#child{font-size:10px;background-size:inherit;background-position:inherit}");
    struct bg_geometry geometry;const struct cstyle *child_style=find(root,"child")->style;
    check(css_background_geometry(child_style,&geometry)&&geometry.size[0].px==40&&geometry.size[1].px==60,
          "inherited background size retains its parent computed font units");
    check(geometry.position[0].px==20&&geometry.position[1].px==40,
          "inherited background position retains its parent computed font units");
    layout_free();dom_free(root);
    printf("css-background-image: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
