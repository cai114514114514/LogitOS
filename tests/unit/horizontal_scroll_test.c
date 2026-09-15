/* Real parser/style/layout/paint output, with a nonzero viewport origin so
 * moving the document cannot accidentally move its clip too. Guest input and
 * JS scroll synchronization have separate gates; no syscall is simulated here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "logit.h"
#include "layout.h"
#include "css.h"
#include "dom.h"
#include "browser_paint.h"
struct paintop paint_ops[PAINT_MAXOPS]; int paint_nops;
void *kmalloc(unsigned long n){return malloc(n);} void kfree(void *p){free(p);}
int text_measure(const char *s,int n,int px,int face){(void)s;(void)face;return n*(px/2);}
int res_fetch(const char*u,uint8_t**b,int*n){(void)u;(void)b;(void)n;return -1;}
void img_free(struct image*i){(void)i;}
int img_decode(const uint8_t*b,int n,struct image*i){(void)b;(void)n;(void)i;return -1;}
void img_register(img_detect_fn d,img_decode_fn c){(void)d;(void)c;}
static int failures, checks;
static struct node *current_root;
#define CHECK(c,m) do {checks++;if(!(c)){failures++;printf("FAIL: %s\n",m);}}while(0)
static void page(const char *html,const char *css){
 struct node*r=dom_parse(html,(int)strlen(html));
 current_root=r;
 css_apply(r,css,(int)strlen(css));css_extra_apply(r,css,(int)strlen(css));layout_page(r,200);
}
static int text_x(void){for(int i=0;i<paint_nops;i++)if(paint_ops[i].kind==OP_TEXT && paint_ops[i].len==5 && !memcmp(paint_ops[i].text,"RIGHT",5))return paint_ops[i].x;return -999;}
static void paint(int sx){paint_nops=0;browser_paint_scroll(7,11,200,100,sx,0);}
int main(void){
 const char *html="<div><a href='/right'>RIGHT</a></div>";
 const char *css="body{margin:0;font-size:16px}div{width:400px}a{margin-left:300px}";
 page(html,css);paint(0);int before=text_x();
 CHECK(before>=300,"control: right link is outside initial viewport");
 CHECK(browser_content_width(current_root,200)>=400,"fixed width creates document overflow");
 paint(250);CHECK(text_x()==before-250,"horizontal scroll translates actual text");
 CHECK(paint_nops>0 && paint_ops[0].kind==OP_CLIP && paint_ops[0].x==7 && paint_ops[0].y==11 && paint_ops[0].w==200,"document translation preserves viewport clip");
 char href[40];browser_hittest(text_x()-7+250+2,4,0,href,sizeof href);
 CHECK(!strcmp(href,"/right"),"painted point maps back to the same document link");
 int x,y,w,h;int d=browser_paint_dirty_rect(&x,&y,&w,&h);
 CHECK(d!=0,"scroll invalidates painted pixels");
 CHECK(d<0 || (x>=7 && y>=11 && x+w<=207 && y+h<=111),"dirty damage stays inside viewport");
 paint(250);CHECK(browser_paint_dirty_rect(&x,&y,&w,&h)==0,"stationary viewport does not invent damage");
 page(html,"body{margin:0;font-size:16px}div{width:400px;transform:translateX(10px)}a{margin-left:300px}");
 paint(0);before=text_x();paint(250);
 CHECK(text_x()==before-250,"transform origin follows horizontal document translation");
 page("<div><span>RIGHT</span></div>","body{margin:0}div{width:100px;overflow:hidden}span{display:block;width:400px}");
 CHECK(browser_content_width(current_root,200)==200,"clipped descendants cannot create page overflow");
 page("<div>RIGHT</div>","body{margin:0}div{display:none;width:400px}");
 CHECK(browser_content_width(current_root,200)==200,"display none creates no scroll range");
 page("<div>RIGHT</div>","body{margin:0}div{width:400px;visibility:hidden}");
 CHECK(browser_content_width(current_root,200)>=400,"visibility hidden retains layout overflow");
 page("<div></div>","body{margin:0}div{width:2000px;height:20px}");
 CHECK(browser_content_width(current_root,200)>=2000,"empty boxes contribute overflow without paint items");
 page("<div>RIGHT</div>","body{margin:0}div{width:100px;transform:translateX(600px)}");
 CHECK(browser_content_width(current_root,200)>=700,"transformed content contributes reachable overflow");
 printf("horizontal-scroll: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
