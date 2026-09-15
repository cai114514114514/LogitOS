#include <stdio.h>
#include <string.h>
#include "logit.h"
struct paintop paint_ops[PAINT_MAXOPS];int paint_nops;
static char url[600];static int ulen,ucaret,usel,win_w=240;
enum { TABH=30,BARH=30 };
static int bookmark_find(const char *s){(void)s;return -1;}
/* A proportional, UTF-8-aware font apparatus. The control's byte*8 cannot
 * accidentally agree with this as it did with the former len*8 test font. */
int text_measure(const char *s,int n,int px,int face)
{
    (void)px;(void)face;int w=0;
    for(int p=0;p<n;){unsigned char c=s[p];w+=c>=128?16:(c=='i'?3:c=='W'?13:7);p++;while(p<n&&((unsigned char)s[p]&0xc0)==0x80)p++;}
    return w;
}
static void gui_glass(int x,int y,int w,int h,int radius,int r,int g,int b,int a){(void)radius;(void)a;gui_rect(x,y,w,h,(r<<16)|(g<<8)|b);}
static void gui_text(int x,int y,unsigned c,const char*s){gui_text_run(x,y,16,0,c,s,strlen(s));}
static unsigned rgb(int r,int g,int b){return (r<<16)|(g<<8)|b;}
#include "../../c/apps/browser/address_geometry.inc"
static int failures,checks;
#define CHECK(x,msg) do{checks++;if(!(x)){printf("FAIL: %s\n",msg);failures++;}}while(0)
static int caret_x(void){for(int i=paint_nops-1;i>=0;i--)if(paint_ops[i].kind==OP_RECT&&paint_ops[i].color==rgb(90,150,240))return paint_ops[i].x;return -1;}
static void set(const char*s){strcpy(url,s);ulen=strlen(url);ucaret=usel=ulen;addr_hscroll=0;paint_nops=0;draw_address_bar(1);}
int main(void)
{
    set("Wiii");CHECK(caret_x()==14+22,"proportional ASCII caret uses painted font");
    set("Wi中文i");CHECK(caret_x()==14+51,"mixed UTF8 caret follows characters, not bytes");
    CHECK(addr_hit(14+16)==2,"mouse maps glyph boundary back to UTF8 byte offset");
    memset(url,'W',200);url[200]=0;ulen=ucaret=usel=200;paint_nops=0;draw_address_bar(1);
    CHECK(caret_x()==win_w-38,"long address keeps end caret within clip");
    CHECK(addr_hscroll>0,"long address scrolls its text viewport");
    CHECK(addr_hit(caret_x())==ulen,"scrolled mouse hit returns end byte offset");
    ucaret=usel=0;paint_nops=0;draw_address_bar(1);CHECK(caret_x()==14&&addr_hscroll==0,"Home brings start back into view");
    set("Wi中文i");ucaret=2;usel=8;paint_nops=0;draw_address_bar(1);
    int selection=0;for(int i=0;i<paint_nops;i++)if(paint_ops[i].color==rgb(140,180,250)&&paint_ops[i].w==32)selection=1;
    CHECK(selection,"selection width uses the same measured prefix pair");
    printf("address geometry: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
