/* SPDX-License-Identifier: MIT
 * A plain legacy search form, with no site bundle or site selectors. Box
 * relationships are checked at three widths; image checks inspect actual
 * compositor calls rather than merely accepting the image's border box. */
#define img_decode unused_home_decode
#define img_free unused_home_free
#define main unused_home_main
#include "modal_paint_test.c"
#undef main
#undef img_free
#undef img_decode
#include "forms.h"

int img_decode(const uint8_t *p,int n,struct image *im)
{
    if(n!=8)return -1;
    memcpy(&im->w,p,4);memcpy(&im->h,p+4,4);
    if(im->w<1||im->w>512||im->h<1||im->h>512)return -1;
    im->rgba=calloc((size_t)im->w*im->h,4);return im->rgba?0:-1;
}
void img_free(struct image *im){free(im->rgba);im->rgba=0;}
static int checks,failures;
#undef CHECK
#define CHECK(c,m) do{checks++;if(!(c)){failures++;printf("FAIL: %s\n",m);}}while(0)
static struct node *doc;
static struct node *id(const char *s){return dom_get_element_by_id(doc->doc,s);}
static int dim(const char *s,int k)
{int v[4]={0};CHECK(layout_node_box(id(s),v,v+1,v+2,v+3),"box exists");return v[k];}
static void page(const char *html,const char *css,int w)
{
    if(doc){fc_reset();focus_reset();layout_free();dom_free(doc);}
    doc=dom_parse(html,(int)strlen(html));css_viewport(w,600);
    css_apply(doc,css,(int)strlen(css));css_extra_apply(doc,css,(int)strlen(css));layout_page(doc,w);
}
static void form(int w)
{
    const char *html="<!doctype html><center id=outer><div><img id=logo src=tile width=120 height=40 style='padding:11px 0 7px'></div>"
        "<form><table><tr valign=top><td id=left width='25%'>&nbsp;</td><td id=cell align=CeNtEr nowrap>"
        "<div><input id=query name=q style='width:300px'></div><br>"
        "<span class=wrap><input id=go type=submit value=Search></span> <span class=wrap><input id=other type=submit value=Explore></span>"
        "</td><td id=right align=left width='25%' nowrap><a id=advanced>Advanced search</a></td></tr></table></form>"
        "<p id=foot>FOOTER</p></center>";
    page(html,"body{margin:0;font-size:16px;line-height:20px}.wrap{display:inline-block;margin:3px}input{box-sizing:border-box}table{width:100%}",w);
    CHECK(dim("logo",0)==(w-120)/2,"center aligns inline image");
    printf("logo box=%d,%d %dx%d align=%d\n",dim("logo",0),dim("logo",1),dim("logo",2),dim("logo",3),((struct cstyle*)id("logo")->style)->text_align);
    int qx=dim("query",0),qw=dim("query",2),cx=dim("cell",0),cw=dim("cell",2);
    printf("width=%d cell=%d,%d input=%d,%d buttons_y=%d,%d\n",w,cx,cw,qx,qw,dim("go",1),dim("other",1));
    CHECK(qw>=300&&qx>=cx&&qx+qw<=cx+cw,"table cell contains its input");
    CHECK(dim("advanced",0)>=qx+qw,"adjacent cell does not overlap input");
    CHECK(dim("go",1)==dim("other",1),"inline buttons share a line");
    CHECK(((struct cstyle*)id("cell")->style)->text_align==ALIGN_CENTER,"HTML align is case insensitive");
    CHECK(((struct cstyle*)id("cell")->style)->white_space==WS_NOWRAP,"HTML nowrap reaches computed style");
    if(w>=800){CHECK(dim("left",2)==w/4,"percentage cell width respected");CHECK(dim("right",2)==w/4,"second percentage cell width respected");}
    paint_nops=0;browser_paint_scroll(0,0,w,600,0,0);
    int found=0;
    for(int i=0;i<paint_nops;i++){struct paintop *p=paint_ops+i;if(p->kind==OP_BLIT&&p->sw==120&&p->sh==40){
        found++;printf("image blit=%d,%d %dx%d\n",p->x,p->y,p->w,p->h);CHECK(p->x==dim("logo",0)&&p->y==dim("logo",1)+11&&p->w==120&&p->h==40,"image pixels exclude padding and border");}}
    CHECK(found==1,"decoded image actually reaches compositor");
}
static void cascade(void)
{
    page("<center id=c style='text-align:right'><table><tr><td id=t align=center nowrap width=200 style='text-align:left;white-space:normal;width:90px'>one two</td></tr></table></center>","body{margin:0}",400);
    struct cstyle *s=id("t")->style;
    CHECK(s->text_align==ALIGN_LEFT&&s->white_space==WS_NORMAL&&s->has_w&&s->width==90,"author CSS overrides HTML hints");
    CHECK(((struct cstyle*)id("c")->style)->text_align==ALIGN_RIGHT,"author CSS overrides center default");
    page("<div align=right id=d>text</div><span align=center id=s>text</span>","",400);
    CHECK(((struct cstyle*)id("d")->style)->text_align==ALIGN_RIGHT,"block align hint applies");
    CHECK(((struct cstyle*)id("s")->style)->text_align!=ALIGN_CENTER,"unsupported align on span stays inert");
    page("<table><tr><td id=cell width=80 nowrap><input id=query style='width:300px;box-sizing:border-box'></td><td>next</td></tr></table>","body{margin:0}",400);
    CHECK(dim("cell",2)>=300&&dim("query",2)==300,"small cell width cannot erase control intrinsic minimum");
}
static void image_edges(void)
{
    const char *html="<img id=i src=tile>";
    const char *css="body{margin:0}img{display:block;width:120px;height:40px;padding:3px 5px 7px 11px;border:2px solid}";
    page(html,css,400);paint_nops=0;browser_paint_scroll(0,0,400,200,0,0);
    CHECK(dim("i",2)==140&&dim("i",3)==54,"image border box reserves asymmetric edges once");
    int found=0;
    for(int i=0;i<paint_nops;i++){struct paintop *p=paint_ops+i;if(p->kind==OP_BLIT&&p->sw==120&&p->sh==40){
        found++;CHECK(p->x==13&&p->y==5&&p->w==120&&p->h==40,"asymmetric padding and border keep bitmap at content origin");}}
    CHECK(found==1,"asymmetric image reaches compositor");
    browser_paint_scroll(0,0,400,200,0,0);int x,y,w,h;
    CHECK(!browser_paint_dirty_rect(&x,&y,&w,&h),"unchanged image is not dirty");
    const char *next="body{margin:0}img{display:block;width:120px;height:40px;padding:3px 11px 7px 5px;border:2px solid}";
    css_apply(doc,next,strlen(next));css_extra_apply(doc,next,strlen(next));layout_page(doc,400);
    browser_paint_scroll(0,0,400,200,0,0);
    CHECK(browser_paint_dirty_rect(&x,&y,&w,&h),"same outer box with moved image pixels is dirty");
}
int main(void)
{
    css_init();int data[2]={120,40};CHECK(layout_img_store("tile",(const unsigned char*)data,8),"test image decoded");
    form(800);form(1126);form(360);cascade();image_edges();
    fc_reset();focus_reset();layout_free();dom_free(doc);
    printf("legacy-home: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
