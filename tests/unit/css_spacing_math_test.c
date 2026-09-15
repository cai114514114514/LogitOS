/* Ordinary CSS values through the shipping DOM, variable expansion, cascade
 * and flex/grid layout. The literal declarations are the independent control;
 * no captured site markup, scripts, images or requests are used. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main

static void finish_page(void)
{
    layout_free(); dom_free(g_root); g_root=0;
}
static int coord(const char *id,int axis)
{
    int box[4]={0};
    CHECK(layout_node_box(ID(id),box,box+1,box+2,box+3),"spacing fixture has a laid out box");
    return box[axis];
}
static void spacing(const char *name,const char *value,int want_px)
{
    char html[4096];
    snprintf(html,sizeof html,
        "<style>:root{--space:.25rem}html{font-size:16px}body{margin:0}"
        "#row{display:flex;gap:%s;width:120px}#grid{display:grid;grid-template-columns:20px 20px;gap:%s}"
        ".child{width:20px;height:10px}#pad{width:100px;padding-inline:%s;padding-block:%s}"
        "</style><div id=row><div id=a class=child></div><div id=b class=child></div></div>"
        "<div id=grid><div id=c class=child></div><div id=d class=child></div></div>"
        "<div id=pad><div id=inside class=child></div></div>",value,value,value,value);
    page(html,400);
    char label[160];
    snprintf(label,sizeof label,"%s: flex gap reaches actual second child",name);
    EQ(coord("b",0)-coord("a",0)-20,want_px,label);
    snprintf(label,sizeof label,"%s: grid gap reaches actual second track",name);
    EQ(coord("d",0)-coord("c",0)-20,want_px,label);
    snprintf(label,sizeof label,"%s: logical inline padding reaches content origin",name);
    EQ(coord("inside",0)-coord("pad",0),want_px,label);
    snprintf(label,sizeof label,"%s: logical block padding reaches content origin",name);
    EQ(coord("inside",1)-coord("pad",1),want_px,label);
    finish_page();
}
static void cascade(const char *label,const char *rules,const char *style,int want_px)
{
    char html[4096];
    snprintf(html,sizeof html,
        "<style>html{font-size:16px}body{margin:0}.row{display:flex;width:120px}.child{width:20px;height:10px}%s</style>"
        "<div id=row class=row style='%s'><div id=a class=child></div><div id=b class=child></div></div>",rules,style);
    page(html,400);
    EQ(coord("b",0)-coord("a",0)-20,want_px,label);
    finish_page();
}
static void padding_case(const char *label,const char *rules,int want_x,int want_y)
{
    char html[4096];
    snprintf(html,sizeof html,
        "<style>html{font-size:16px}body{margin:0}#pad{width:100px;%s}.child{width:20px;height:10px}</style>"
        "<div id=pad><div id=inside class=child></div></div>",rules);
    page(html,400);
    EQ(coord("inside",0)-coord("pad",0),want_x,label);
    EQ(coord("inside",1)-coord("pad",1),want_y,label);
    finish_page();
}
static void percentage_resize(void)
{
    const char *html="<style>body{margin:0}#pad{width:100px;padding-inline:calc(10% + 2px);padding-block:calc(5% + 3px)}"
        "#inside{width:20px;height:10px}</style><div id=pad><div id=inside></div></div>";
    page(html,400);
    EQ(coord("inside",0)-coord("pad",0),42,"mixed padding uses containing width and px addend");
    EQ(coord("inside",1)-coord("pad",1),23,"vertical percentage padding also uses containing width");
    layout_page(g_root,800);
    EQ(coord("inside",0)-coord("pad",0),82,"resize recomputes logical inline percentage padding");
    EQ(coord("inside",1)-coord("pad",1),43,"resize recomputes logical block percentage padding");
    layout_page(g_root,800);
    EQ(coord("inside",0)-coord("pad",0),82,"repeated layout does not accumulate inline padding");
    EQ(coord("inside",1)-coord("pad",1),43,"repeated layout does not accumulate block padding");
    finish_page();
}
int main(int argc,char **argv)
{
    css_init();css_viewport(400,240);
    spacing("literal control","12px",12);
    if(argc==2 && !strcmp(argv[1],"literal")) {
        printf("css-spacing-math: %d checks, %d failures\n",checks,fails);
        return fails?1:0;
    }
    if(argc!=1)return 2;
    spacing("root relative length",".75rem",12);
    spacing("length multiplication","calc(4px * 3)",12);
    spacing("expanded variable multiplication","calc(var(--space)*3)",12);
    spacing("mixed length units","calc(2px + .625rem)",12);
    cascade("later ordinary literal replaces earlier calc",".row{gap:calc(4px * 3);gap:7px}","",7);
    cascade("later calc replaces earlier ordinary literal",".row{gap:7px;gap:calc(4px * 3)}","",12);
    cascade("invalid auto gap retains earlier valid value",".row{gap:7px;gap:auto}","",7);
    cascade("invalid later token retains earlier valid value",".row{gap:7px;gap:calc(4px * 3) nope}","",7);
    cascade("invalid literal suffix retains earlier valid value",".row{gap:7px;gap:12px nope}","",7);
    cascade("negative gap literal retains earlier valid value",".row{gap:7px;gap:-12px}","",7);
    cascade("normal gap resets earlier value",".row{gap:7px;gap:normal}","",0);
    cascade("negative calculated gap is range clamped",".row{gap:7px;gap:calc(2px - 5px)}","",0);
    cascade("important calc beats normal inline",".row{gap:calc(4px * 3)!important}","gap:5px",12);
    cascade("element font resolves em after matching",".row{font-size:20px;gap:calc(.5em * 2)}","",20);
    cascade("column longhand replaces one shorthand component",".row{gap:12px;column-gap:calc(3px * 2)}","",6);
    cascade("later shorthand replaces earlier column longhand",".row{column-gap:5px;gap:calc(3px * 2) calc(4px * 2)}","",8);
    padding_case("literal logical padding clears old physical percentage","padding:10%;padding-inline:12px;padding-block:7px",12,7);
    padding_case("invalid logical suffix keeps earlier complete declaration","padding-inline:7px;padding-block:9px;padding-inline:12px nope",7,9);
    padding_case("negative logical literal keeps earlier declaration","padding-inline:7px;padding-block:9px;padding-inline:-2px",7,9);
    padding_case("logical padding percentage is relative to containing width","padding-inline:10%;padding-block:5%",40,20);
    padding_case("negative percentage calc resolves before range clamp","padding-inline:calc(50px - 10%);padding-block:calc(5px - 10%)",10,0);
    percentage_resize();
    printf("css-spacing-math: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
