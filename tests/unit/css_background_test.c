/* The fault boundary is actual source-over pixels from parser -> cascade ->
 * layout -> the shipping painter. A parsed gradient or a counted draw call
 * cannot prove its transparent area retains the box's own background color.
 * The HTML fixture is also the guest acceptance page; this host run makes no
 * browser performance claim and uses no host wall-clock measurement. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "logit.h"
#include "layout/block.h"
#include "css/style.h"
#include "dom/tree.h"
#include "paint/renderer.h"

/* Companion gradient gates reuse this immediate GUI blend recorder. Keep the
 * original fixture defaults: changing its viewport moves the old apparatus,
 * while copying it would let host/guest blend expectations drift apart. */
#ifndef CSS_BACKGROUND_WIDTH
#define CSS_BACKGROUND_WIDTH 400
#define CSS_BACKGROUND_HEIGHT 440
#define CSS_BACKGROUND_CHECKS 26
#define CSS_BACKGROUND_HTML "tests/fixtures/engine-expansion/css-background.html"
#define CSS_BACKGROUND_CSV "tests/fixtures/engine-expansion/css-background-pixels.csv"
#define CSS_BACKGROUND_NAME "css-background"
#endif
enum { W=CSS_BACKGROUND_WIDTH, H=CSS_BACKGROUND_HEIGHT };
static unsigned pixels[H][W];
static int cx,cy,cw=W,ch=H,checks,failures;
void *kmalloc(unsigned long n){return malloc(n);}
void kfree(void *p){free(p);}
int text_measure(const char *s,int n,int px,int mono){(void)s;(void)mono;return n*(px/2);}
int res_fetch(const char *u,uint8_t **p,int *n){(void)u;(void)p;(void)n;return -1;}
void img_free(struct image *p){(void)p;}
int img_decode(const uint8_t *p,int n,struct image *o){(void)p;(void)n;(void)o;return -1;}
void img_register(img_detect_fn a,img_decode_fn b){(void)a;(void)b;}

static void put(int x,int y,unsigned color,int a)
{
    if(x<0||y<0||x>=W||y>=H||x<cx||y<cy||x>=cx+cw||y>=cy+ch)return;
    unsigned old=pixels[y][x],out=0;
    for(int shift=0;shift<=16;shift+=8){
        unsigned v=(((color>>shift)&255)*a+((old>>shift)&255)*(255-a)+127)/255;
        out|=v<<shift;
    }
    pixels[y][x]=out;
}
void gui_clip(int x,int y,int w,int h){cx=x;cy=y;cw=w?w:W;ch=h?h:H;}
void gui_rect(int x,int y,int w,int h,unsigned c)
{for(int j=0;j<h;j++)for(int i=0;i<w;i++)put(x+i,y+j,c,255);}
void gui_rrect(int x,int y,int w,int h,int r,unsigned c)
{
    for(int j=0;j<h;j++)for(int i=0;i<w;i++){
        int dx=i<r?r-i:i>=w-r?i-(w-r):0;
        int dy=j<r?r-j:j>=h-r?j-(h-r):0;
        if(!dx||!dy||dx*dx+dy*dy<=r*r)put(x+i,y+j,c,255);
    }
}
void gui_blit(int x,int y,int w,int h,const unsigned char *p,int sw,int sh)
{
    if(w<=0||h<=0||sw<=0||sh<=0||!p)return;
    for(int j=0;j<h;j++)for(int i=0;i<w;i++){
        const unsigned char *s=p+4*((long long)j*sh/h*sw+(long long)i*sw/w);
        put(x+i,y+j,((unsigned)s[0]<<16)|((unsigned)s[1]<<8)|s[2],s[3]);
    }
}
void gui_text_run_w(int x,int y,int px,int mono,unsigned c,const char *s,int n,int b)
{(void)x;(void)y;(void)px;(void)mono;(void)c;(void)s;(void)n;(void)b;}
static void check(int c,const char *label)
{checks++;if(!c){failures++;printf("FAIL: %s\n",label);}else printf("ok: %s\n",label);}
static void pixel(int x,int y,unsigned expected,const char *label)
{
    unsigned actual=pixels[y][x];int ok=1;
    for(int shift=0;shift<=16;shift+=8){int d=(int)((actual>>shift)&255)-(int)((expected>>shift)&255);if(d < -2 || d > 2)ok=0;}
    if(!ok)printf("pixel(%d,%d)=%06x expected=%06x\n",x,y,actual,expected);
    check(ok,label);
}
static struct node *find(struct node *n,const char *id)
{
    const char *s=n->type==N_ELEM?dom_attr(n,"id"):0;if(s&&!strcmp(s,id))return n;
    for(struct node *c=n->first_child;c;c=c->next){struct node *f=find(c,id);if(f)return f;}return 0;
}
int main(int argc,char **argv)
{
    const char *name=CSS_BACKGROUND_HTML;
    FILE *f=fopen(name,"rb");if(!f){perror(name);return 2;}
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    char *html=malloc((size_t)size+1);if(!html||fread(html,1,(size_t)size,f)!=(size_t)size)return 2;fclose(f);html[size]=0;
    struct node *root=dom_parse(html,(int)size);
    char *css=strstr(html,"<style>")+7,*end=strstr(css,"</style>");
    css_apply(root,css,(int)(end-css));css_extra_apply(root,css,(int)(end-css));layout_page(root,W);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++)pixels[y][x]=0xffffff;
    browser_paint(0,0,W,H,0);
    /* The guest driver reads this exact table too. Duplicating the colors and
     * coordinates in Python would let one door silently keep a stale oracle. */
    FILE *oracle=fopen(CSS_BACKGROUND_CSV,"r");
    if(!oracle){perror(CSS_BACKGROUND_CSV);return 2;}
    char line[512],label[256],id[64];
    while(fgets(line,sizeof line,oracle)){
        int x,y,w,h;unsigned expected;
        if(sscanf(line,"pixel,%d,%d,%x,%255[^,\n]",&x,&y,&expected,label)==4){
            if(x<0||x>=W||y<0||y>=H){fclose(oracle);return 2;}
            pixel(x,y,expected,label);
        }else if(sscanf(line,"box,%63[^,],%d,%d,%d,%d,%255[^\n]",id,&x,&y,&w,&h,label)==6){
            int ax,ay,aw,ah;struct node *target=find(root,id);
            check(target&&layout_node_box(target,&ax,&ay,&aw,&ah)&&ax==x&&ay==y&&aw==w&&ah==h,label);
        }else{fprintf(stderr,"malformed CSS background oracle row\n");fclose(oracle);return 2;}
    }
    fclose(oracle);if(checks!=CSS_BACKGROUND_CHECKS)return 2;
    FILE *ppm=argc>1?fopen(argv[1],"wb"):0;
    if(ppm){fprintf(ppm,"P6\n%d %d\n255\n",W,H);for(int j=0;j<H;j++)for(int i=0;i<W;i++){unsigned p=pixels[j][i];unsigned char rgb[3]={(unsigned char)(p>>16),(unsigned char)(p>>8),(unsigned char)p};fwrite(rgb,1,3,ppm);}fclose(ppm);}
    printf(CSS_BACKGROUND_NAME ": %d checks, %d failures\n",checks,failures);return failures?1:0;
}
