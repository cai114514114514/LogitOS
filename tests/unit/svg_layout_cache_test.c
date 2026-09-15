/* Actual flex/layout trial lifecycle with a counted deterministic decoder.
 * The guest benchmark separately uses the real SVG rasteriser. These asserts
 * prove that sharing avoids repeated work without freeing final pixels or
 * retaining old source data across a new layout pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layout.h"
#include "dom.h"
#include "css.h"
void *kmalloc(unsigned long n){return malloc(n);}
void kfree(void *p){free(p);}
int text_measure(const char *s,int n,int px,int face){(void)s;(void)face;return n*px/2;}
int res_fetch(const char *s,unsigned char **b,int *n){(void)s;(void)b;(void)n;return -1;}
static int decodes,live,checks,fails;
int img_decode(const unsigned char *p,int n,struct image *im){
 decodes++;
 if(n<4 || memcmp(p,"<svg",4))return -1;
 im->w=im->h=16;im->rgba=malloc(16*16*4);memset(im->rgba,0x7c,16*16*4);live++;return 0;
}
void img_free(struct image *im){if(im&&im->rgba){free(im->rgba);im->rgba=0;live--;}}
#define CHECK(c,m) do{checks++;if(!(c)){fails++;printf("FAIL: %s\n",m);}}while(0)
static const char html[]="<!doctype html><body><div class=row><div class=row><div class=row><span><svg width=16 height=16 viewBox='0 0 16 16'><path d='M0 0H16V16Z'/></svg></span><span>label</span></div></div></div></body>";
static const char css[]="body{margin:0}.row{display:flex;align-items:center}span{display:block}";
void layout_images_reset(void);
int main(void){
 css_init();struct node *root=dom_parse(html,sizeof html-1);
 css_apply(root,css,sizeof css-1);css_extra_apply(root,css,sizeof css-1);
 layout_page(root,640);
 CHECK(decodes==1,"one SVG decode across nested flex measurement trials");
 int images=0;const struct item *its=layout_items();
 for(int i=0;i<layout_count();i++)if(its[i].img){images++;CHECK(its[i].img->rgba[0]==0x7c,"final painted bitmap remains live");CHECK(its[i].w==16&&its[i].h==16,"cached SVG keeps geometry");}
 CHECK(images==1,"one final SVG image is emitted");CHECK(live==1,"trials share one bitmap owner");
 /* A new layout formerly required a second decode. The new cache still
  * re-serializes source but reuses pixels for byte-identical input. */
 layout_page(root,480);CHECK(decodes==1,"unchanged new layout rechecks input and reuses pixels");CHECK(live==1,"reflow has one retained bitmap owner");
 layout_free();layout_images_reset();CHECK(live==0,"document teardown releases retained bitmap");
 dom_free(root);
 /* More distinct sources than cache entries exercise the item-owned fallback
  * alongside borrowed entries in the SAME final display list. */
 char *many=malloc(30000);int used=0;used+=sprintf(many+used,"<!doctype html><body><div class=row>");
 /* These must have distinct inputs: formerly node identity made identical
  * markup distinct, but content cache sharing should coalesce identical SVG. */
 for(int i=0;i<150;i++)used+=sprintf(many+used,"<span><svg id='icon-%d' width=16 height=16 viewBox='0 0 16 16'><path d='M0 0H16V16Z'/></svg></span>",i);
 used+=sprintf(many+used,"</div></body>");root=dom_parse(many,used);
 css_apply(root,css,sizeof css-1);css_extra_apply(root,css,sizeof css-1);layout_page(root,4000);
 images=0;its=layout_items();for(int i=0;i<layout_count();i++)if(its[i].img)images++;
 CHECK(images==150,"cache entry limit preserves all final SVG images");CHECK(live==150,"mixed owned and borrowed images remain live");
 layout_free();layout_images_reset();CHECK(live==0,"cache overflow fallback and shared entries both free once");dom_free(root);free(many);
 printf("svg layout cache: %d checks, %d failures; %d decodes\n",checks,fails,decodes);return fails?1:0;
}
