#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ime_ui.h"
#include "ime_learn.h"
#include "fb.h"
#include "text.h"
#include "vfs.h"
#include "kheap.h"
#include "kprintf.h"
#include "wm.h"
static int checks,failures,focus=0,damages,draws;
static const char *dictpath;static int extra_mode;
static struct {int x,y;char text[768];} rows[32];
#define CHECK(c,l) do {checks++;if(!(c)){failures++;printf("FAIL: %s\n",l);}else printf("ok: %s\n",l);}while(0)
void *kmalloc(size_t n){return malloc(n);} void kfree(void *p){free(p);}
void kprintf(const char *f,...){(void)f;}
static const char *host_path(const char *p){if(strstr(p,"qwen"))return extra_mode==1?dictpath:NULL;return "fsroot/ime/pinyin.dat";}
int vfs_size(const char *p){if(strstr(p,"qwen")&&extra_mode==2)return 4; const char *s=host_path(p);if(!s)return -1;FILE*f=fopen(s,"rb");if(!f)return -1;fseek(f,0,SEEK_END);int n=ftell(f);fclose(f);return n;}
int vfs_pread(const char*p,void*out,int n,long long off){if(strstr(p,"qwen")&&extra_mode==2){memcpy(out,"bad!",4);return 4;}const char*s=host_path(p);if(!s)return -1;FILE*f=fopen(s,"rb");if(!f)return -1;fseek(f,off,SEEK_SET);int got=fread(out,1,n,f);fclose(f);return got;}
uint32_t fb_width(void){return 1280;}uint32_t fb_height(void){return 800;}
int fb_pt(int x){return x;}int fb_ui_px(void){return 16;}
uint32_t fb_rgb(uint8_t r,uint8_t g,uint8_t b){return ((unsigned)r<<16)|((unsigned)g<<8)|b;}
void fb_blend_round_rect(int x,int y,int w,int h,int rad,uint8_t r,uint8_t g,uint8_t b,uint8_t a){(void)x;(void)y;(void)w;(void)h;(void)rad;(void)r;(void)g;(void)b;(void)a;}
int text_line_height(int px){return px+4;}
int text_width_sz(const char*s,int px){int n=0;for(;*s;s++)if(((unsigned char)*s&192)!=128)n+=((unsigned char)*s<128)?px/2:px;return n;}
int text_draw_sz(int x,int y,const char*s,int px,uint32_t color){(void)color;if(draws<32){rows[draws].x=x;rows[draws].y=y;snprintf(rows[draws].text,768,"%s",s);}draws++;return text_width_sz(s,px);}
void wm_damage(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;damages++;}
void wm_damage_menubar(void){damages++;}
int wm_dark(void){return 0;}
int wm_ime_anchor(int *wi,int *x,int *y,int *w,int *h){if(focus<0)return 0;*wi=focus;*x=100;*y=100;*w=600;*h=30;return 1;}
static uint32_t out[IME_UI_MAXCP];
static int key(int c,int mods){return ime_ui_key(focus,c,mods,out,IME_UI_MAXCP);}
static void type(const char*s){while(*s)key(*s++,0);}
static void paint(void){draws=0;ime_ui_compose();}
static int equals(int n,const char*text){char b[256];int at=0;for(int i=0;i<n;i++){unsigned c=out[i];if(c<128)b[at++]=c;else if(c<2048){b[at++]=192|(c>>6);b[at++]=128|(c&63);}else{b[at++]=224|(c>>12);b[at++]=128|((c>>6)&63);b[at++]=128|(c&63);}}b[at]=0;return !strcmp(b,text);}
int main(int argc,char**argv){if(argc!=3)return 2;dictpath=argv[1];extra_mode=atoi(argv[2]);
 CHECK(ime_ui_init()==1&&ime_ui_available(),"dictionary loads, including missing/corrupt optional fallback");
 CHECK(key('n',0)==-1,"English mode passes ASCII through");
 CHECK(key(' ',IME_TOGGLE_MOD|EV_MOD_CTRL)==-1&&!ime_ui_enabled(0),"extra modifiers cannot accidentally toggle IME");
 CHECK(key(' ',IME_TOGGLE_MOD)==0&&ime_ui_enabled(0),"toggle enables focused window");
 type("nihao");CHECK(equals(key(',',0),"你好，")&&!ime_ui_composing(),"punctuation commits preedit instead of dropping it");
 type("nihaom");CHECK(equals(key('1',0),"你好")&&ime_ui_composing(),"partial selection keeps suffix open in the actual UI");
 paint();CHECK(draws>=1&&!strncmp(rows[0].text,"m",1),"remaining m is visible");
 key('a',0);CHECK(equals(key(' ',0),"吗"),"suffix continues and commits");
 type("nihao");CHECK(key(19,EV_MOD_CTRL)==-1&&ime_ui_composing(),"Ctrl+S preserves unsent composition");
 CHECK(equals(key(' ',0),"你好"),"preserved composition remains committable after shortcut");
 type("nihao");CHECK(key('9',0)==0&&ime_ui_composing(),"missing numeric slot leaves preedit intact");key(27,0);
 type("nihao");CHECK(equals(key('X',EV_MOD_SHIFT),"nihaoX"),"uppercase escape keeps all raw letters");
 type("nihao");focus=1;ime_ui_focus(1);paint();CHECK(draws==0&&!ime_ui_enabled(1),"switching to English window hides old popup without a key");
 key(' ',IME_TOGGLE_MOD);type("beijing");focus=0;ime_ui_focus(0);CHECK(equals(key(' ',0),"你好"),"returning focus restores original spelling");
 focus=1;ime_ui_focus(1);CHECK(equals(key(' ',0),"北京"),"other window retains its own composition");
 type("xian");paint();CHECK(draws==10,"all nine choices are rendered as separate visible rows");
 int visible=1;for(int i=1;i<draws;i++)if(rows[i].y<=rows[i-1].y||rows[i].y>=800||rows[i].x+text_width_sz(rows[i].text,16)>1280)visible=0;
 CHECK(visible,"numbered candidates fit the screen with distinct hit rows");
 int chosen=ime_ui_click(1,rows[9].x+5,rows[9].y+2,out,IME_UI_MAXCP);
 CHECK(equals(chosen,"西安"),"click selects the same numbered candidate that was drawn");
 type("nihao");ime_ui_win_gone(1);CHECK(!ime_ui_enabled(1)&&!ime_ui_composing(),"closed window cannot retain stale IME state");
 CHECK(damages>0,"state transitions request repaint");
 printf("%d checks, %d failed\n",checks,failures);return failures?1:0;}
