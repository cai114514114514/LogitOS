/* Normal shipped-font runs through the actual kernel text engine, glyph masks,
 * SDK wrapper and WM measure branch. Host usercopy uses valid local objects;
 * framebuffer ownership is represented by the real blit callback's pixels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "logit_abi.h"
#include "text.h"
#include "ttf.h"

_Static_assert(sizeof(struct logit_text_metrics) == 44, "metrics ABI");
_Static_assert(sizeof(struct logit_text_metrics_query) == 40, "query ABI");
_Static_assert(offsetof(struct logit_text_metrics_query, out) == 32, "query out ABI");

static const char *root;
static int checks, fails, draws, copies_in, copies_out, scale = 100;
static int ink_on, ix0, iy0, ix1, iy1;
static uint64_t pixel_digest;
static void check(int ok, const char *name)
{ ++checks; if (!ok) ++fails; printf("%s %s\n", ok ? "PASS" : "FAIL", name); }
void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
void kprintf(const char *fmt, ...) { (void)fmt; }
struct spinlock;
void spin_lock(struct spinlock *p) { (void)p; }
void spin_unlock(struct spinlock *p) { (void)p; }
static FILE *font_file(const char *path)
{ char name[2048]; snprintf(name, sizeof name, "%s%s", root, path); return fopen(name,"rb"); }
int vfs_size(const char *path)
{ FILE *f=font_file(path); if(!f)return -1; fseek(f,0,SEEK_END); int n=(int)ftell(f); fclose(f);return n; }
int vfs_read(const char *path, void *buf, int max)
{ FILE *f=font_file(path);if(!f)return -1;int n=(int)fread(buf,1,(size_t)max,f);fclose(f);return n; }
void fb_blit_glyph(int x, int y, const unsigned char *cov, int w, int h, uint32_t color)
{
    (void)color; ++draws;
    for(int yy=0; yy<h; ++yy) for(int xx=0; xx<w; ++xx) if(cov[yy*w+xx]) {
        int a=x+xx,b=y+yy;
        pixel_digest = pixel_digest*1099511628211ULL + (unsigned)(a*31+b*7+cov[yy*w+xx]);
        if(!ink_on){ix0=a;iy0=b;ix1=a+1;iy1=b+1;ink_on=1;}
        else {if(a<ix0)ix0=a;if(b<iy0)iy0=b;if(a+1>ix1)ix1=a+1;if(b+1>iy1)iy1=b+1;}
    }
}
static void reset_pixels(void) { draws=ink_on=ix0=iy0=ix1=iy1=0; pixel_digest=0; }
static int S(int x) {return x>=0?x*scale/100:-((-x*scale+99)/100);}
static int PT(int x) {int a=100*(x+1);return (a>=0?(a+scale-1)/scale:-((-a)/scale))-1;}
static int fb_scale(void) {return scale;}
static int user_copy_from(void *d,const void *s,unsigned long n)
{ ++copies_in; memcpy(d,s,n); return 0; }
static int user_copy_to(void *d,const void *s,unsigned long n)
{ ++copies_out; memcpy(d,s,n); return 0; }
#define USER_TEXT_MAX 1024
static long fixture_sys(long n,long a,long b,long c)
{
    if(n != SYS_TEXT_MEASURE) return -1;
#include "text_measure_dispatch.inc"
}
#define _sys fixture_sys
#include "../../c/apps/text_metrics_wiring.inc"
#undef _sys

static void run_case(const char *s,int px,int face,int pct)
{
    scale=pct; int len=(int)strlen(s), old_draws=draws;
    struct logit_text_metrics m;
    int rc=text_run_metrics_px(s,len,px,face,&m);
    char name[160];
#define CASECHECK(ok,what) do { snprintf(name,sizeof name,"%s px=%d face=%d scale=%d len=%d",what,px,face,pct,len);check((ok),name); } while(0)
    CASECHECK(rc==1,"query supported"); if(rc!=1)return;
    CASECHECK(draws==old_draws,"query leaves drawing untouched");
    int width=(int)fixture_sys(SYS_TEXT_MEASURE,(long)s,len,((long)px<<2)|face);
    CASECHECK(width==PT(m.advance),"width ABI and shaped advance agree");
    CASECHECK(m.scale_percent==pct && m.baseline==m.ascent && m.ascent>0 && m.descent<=0,"font metrics and display scale present");
    reset_pixels(); int end=text_draw_run(37,29,s,len,S(px),face,0xffffff);
    CASECHECK(end==37+m.advance,"paint and query advance agree");
    CASECHECK(m.has_ink==ink_on && (!ink_on || (m.ink_left+37==ix0 && m.ink_top+29==iy0 && m.ink_right+37==ix1 && m.ink_bottom+29==iy1)),"tight ink equals actual glyph pixels");
    uint64_t digest=pixel_digest;int actual_draws=draws;
    struct logit_text_metrics warm;
    CASECHECK(text_run_metrics_px(s,len,px,face,&warm)==1 && !memcmp(&m,&warm,sizeof m) && draws==actual_draws && pixel_digest==digest,"warm query stable and read only");
    if(!len || !strcmp(s,"   ")) CASECHECK(!m.has_ink && m.ink_left==0 && m.ink_top==0 && m.ink_right==0 && m.ink_bottom==0,"empty ink retains font strut");
#undef CASECHECK
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;root=argv[1];text_init();
    struct logit_text_metrics out,unchanged;
    memset(&out,0x5a,sizeof out);unchanged=out;
    copies_in=copies_out=draws=0;
    int rc=text_run_metrics_px("Hg",2,14,0,&out);
    int before_in=copies_in,before_out=copies_out;
    check(draws==0,"initial query never paints");
    if(rc==0) {
        check(!memcmp(&out,&unchanged,sizeof out),"unsupported query leaves output untouched");
        check(before_in==0 && before_out==0,"old width branch rejects query before usercopy");
        check(fixture_sys(SYS_TEXT_MEASURE,(long)"Hg",2,14<<2)==text_measure("Hg",2,14,0),"old width ABI remains available");
        check(0,"vertical metrics capability exists");
    } else {
        check(rc==1,"vertical metrics capability exists");
        check(before_in==2 && before_out==1,"versioned query copies ordinary request text and result");
        const char *runs[]={"ASCII 123", "Hg", "g", "中文", "" , "   "};
        const int sizes[]={10,14,24};
        const int faces[]={0,LOGIT_FACE_MONO,LOGIT_FACE_BOLD,LOGIT_FACE_MONO|LOGIT_FACE_BOLD};
        const int scales[]={100,150,200};
        for(unsigned f=0;f<sizeof faces/sizeof faces[0];f++)
            for(unsigned i=0;i<sizeof runs/sizeof runs[0];i++)
                run_case(runs[i],sizes[i%3],faces[f],scales[i%3]);
        struct logit_text_metrics a,b;
        scale=100; text_run_metrics_px("H",1,14,0,&a);text_run_metrics_px("g",1,14,0,&b);
        check(a.baseline==b.baseline && a.ink_bottom<b.ink_bottom,"baseline is stable while descender ink changes");
        printf("normal-g-14: scale=%d baseline=%d ascent=%d descent=%d ink=%d,%d,%d,%d\n",
               b.scale_percent,b.baseline,b.ascent,b.descent,b.ink_left,b.ink_top,b.ink_right,b.ink_bottom);
    }
    printf("text-vertical-metrics: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
