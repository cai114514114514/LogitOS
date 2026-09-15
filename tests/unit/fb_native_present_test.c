/* Native-present integration, linking the real framebuffer and raster adapter.
 * A callback paints a distinct oracle pattern: reporting GPU success while
 * subsequently doing a CPU copy MUST fail, even if counters look correct.
 * Front-buffer pitch differs from the RAM stride to catch accidental borrowing
 * of an aperture pointer or use of the wrong row size. This proves dispatch,
 * fallback and quarantine, not the GPU engine itself (the RV100 gates do that).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fb.h"
#include "glass.h"
#define W 24
#define H 18
#define FRONT_STRIDE 28
static uint32_t front[FRONT_STRIDE * H], back[W * H], other[W * H];
static uint32_t snapshot[FRONT_STRIDE * H];
static unsigned checks, fails, calls, callback_reentry_ok;
static int callback_result;
static const uint32_t *seen_src;
static uint32_t seen_stride, seen_x, seen_y, seen_w, seen_h;
static void ck(int yes, const char *name)
{ ++checks; if (!yes) { ++fails; printf("FAIL %s\n", name); } }
void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
void vmm_map_range(uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{ (void)a;(void)b;(void)c;(void)d; }
int virtio_gpu_init(void) { return -1; }
uint32_t *virtio_gpu_fb(void) { return NULL; }
uint32_t virtio_gpu_width(void) { return 0; }
uint32_t virtio_gpu_height(void) { return 0; }
int virtio_gpu_present(void) { return 0; }
void virtio_gpu_flush(int x,int y,int w,int h) { (void)x;(void)y;(void)w;(void)h; }
int virtio_gpu_cursor_ready(void) { return 0; }
int virtio_gpu_cursor_define(const uint32_t *a,int w,int h,int hx,int hy)
{ (void)a;(void)w;(void)h;(void)hx;(void)hy;return -1; }
void virtio_gpu_cursor_move(int x,int y) { (void)x;(void)y; }
void text_draw_sz(int x,int y,const char *s,int px,uint32_t c)
{ (void)x;(void)y;(void)s;(void)px;(void)c; }
int text_width_sz(const char *s,int px) { (void)s;(void)px;return 0; }
int text_line_height(int px) { return px; }
/* Effects are not reached by a present. Refusing their optional mask keeps
 * this link faithful without fabricating an exercised graphics capability. */
const unsigned char *gfx_mask_corner(int k,int w,int h,int p)
{ (void)k;(void)w;(void)h;(void)p;return NULL; }
int gfx_shadow_falloff(long d,long b) { (void)d;(void)b;return 0; }
void glass_build_lut(int e,int r) { (void)e;(void)r; }
unsigned gl_isqrt(unsigned long v) { (void)v;return 0; }
int glass_disp[3][GLASS_E_MAX + 1];
unsigned char glass_fres[GLASS_E_MAX + 1];
extern void *fb_dma_test_hold_writer(void);
extern void fb_dma_test_release_writer(void);

static uint32_t gpu_pixel(unsigned x,unsigned y)
{ return 0xa9000000u | (y << 8) | x; }
static void seed(void)
{
    for (unsigned i=0;i<FRONT_STRIDE*H;i++) front[i]=0x5a000000u+i;
    for (unsigned i=0;i<W*H;i++) back[i]=0x12000000u+i;
    memcpy(snapshot,front,sizeof front);
}
static int rect_matches(unsigned x,unsigned y,unsigned w,unsigned h,int gpu)
{
    for (unsigned row=0;row<H;row++) for (unsigned col=0;col<FRONT_STRIDE;col++) {
        int in=col>=x && col<x+w && row>=y && row<y+h;
        uint32_t want=in?(gpu?gpu_pixel(col,row):back[row*W+col]):snapshot[row*FRONT_STRIDE+col];
        if(front[row*FRONT_STRIDE+col]!=want) return 0;
    }
    return 1;
}
static int native(const uint32_t *src,uint32_t stride,uint32_t x,uint32_t y,uint32_t w,uint32_t h)
{
    ++calls;seen_src=src;seen_stride=stride;seen_x=x;seen_y=y;seen_w=w;seen_h=h;
    /* A late AP asks for the same lease while the GPU owns scanout. It must
     * be refused, including during a recursive graphics-mutex call. */
    uint32_t before[FRONT_STRIDE*H];memcpy(before,front,sizeof before);
    fb_copy_rect(0,0,W,H);
    fb_fb_put(1,1,0xffffffffu);
    callback_reentry_ok += memcmp(before,front,sizeof before)==0;
    if(callback_result==0)
        for(unsigned yy=y;yy<y+h;yy++) for(unsigned xx=x;xx<x+w;xx++)
            front[yy*FRONT_STRIDE+xx]=gpu_pixel(xx,yy);
    return callback_result;
}
static void init(void)
{
    /* Alignment is explicit because the real parser reads u64 tag fields. */
    _Alignas(8) uint8_t mbi[128]={0};
    uint8_t *p=mbi+8;
    *(uint32_t *)mbi=sizeof mbi;*(uint32_t *)p=8;*(uint32_t *)(p+4)=40;
    *(uint64_t *)(p+8)=(uintptr_t)front;
    *(uint32_t *)(p+16)=FRONT_STRIDE*4;*(uint32_t *)(p+20)=W;*(uint32_t *)(p+24)=H;
    p[28]=32;p[29]=1;p[32]=16;p[33]=8;p[34]=8;p[35]=8;p[36]=0;p[37]=8;
    ck(fb_init((uintptr_t)mbi)==1,"synthetic LFB initialized");
    fb_set_backbuffer(back);
}
int main(void)
{
    init();seed();fb_set_native_present(native);
    /* A window canvas may be the current raster target; presenting still
     * borrows the full screen back buffer, never this unrelated surface. */
    struct surface off={.px=other,.w=W,.h=H};fb_target(&off);
    uint64_t px=fb_present_px(), n=fb_present_calls();
    fb_present_rect(-3,-2,10,8);
    ck(calls==1,"native called once for visible damage");
    ck(seen_src==back && seen_stride==W,"native borrows screen RAM and pixel stride");
    ck(seen_x==0 && seen_y==0 && seen_w==7 && seen_h==6,"native receives clamped negative damage");
    ck(rect_matches(0,0,7,6,1),"GPU success preserves callback pixels without CPU overwrite");
    ck(callback_reentry_ok==1,"front writers refused during native callback");
    ck(fb_present_px()==px+42 && fb_present_calls()==n+1,"GPU completion accounts clamped damage");
    fb_target(NULL);
    seed();callback_result=-1;px=fb_present_px();n=fb_present_calls();
    fb_present_rect(W-3,H-2,10,10);
    ck(calls==2 && seen_x==W-3 && seen_y==H-2 && seen_w==3 && seen_h==2,"far edge damage clamped before callback");
    ck(rect_matches(W-3,H-2,3,2,0),"declined native present uses CPU with correct pitches");
    ck(fb_present_px()==px+6 && fb_present_calls()==n+1,"CPU fallback counted once");
    unsigned before=calls;px=fb_present_px();n=fb_present_calls();
    fb_present_rect(2,2,0,5);fb_present_rect(W+1,H+1,4,4);
    ck(calls==before && fb_present_px()==px && fb_present_calls()==n,"empty damage never submitted or counted");
    seed();callback_result=0;
    void *lease=fb_dma_test_hold_writer();ck(lease==front,"AP obtained front writer lease");
    fb_present_rect(2,3,4,5);
    ck(calls==before,"outstanding AP lease prevents GPU submission");
    ck(rect_matches(2,3,4,5,0),"AP drain refusal retains CPU fallback");
    fb_dma_test_release_writer();
    seed();callback_result=-2;px=fb_present_px();n=fb_present_calls();
    fb_present_rect(1,1,5,5);
    ck(calls==before+1,"native timeout reached callback after AP release");
    ck(memcmp(front,snapshot,sizeof front)==0,"in-flight failure does not CPU fallback");
    ck(fb_present_px()==px && fb_present_calls()==n,"in-flight failure not counted as presented");
    before=calls;callback_result=0;
    fb_present();fb_copy_rect(0,0,W,H);fb_fb_put(0,0,0xffffffffu);
    ck(calls==before && memcmp(front,snapshot,sizeof front)==0,"quarantine freezes present band and cursor writers");
    fb_set_native_present(NULL);
    fb_present();fb_copy_rect(0,0,W,H);fb_fb_put(W-1,H-1,0xffffffffu);
    ck(memcmp(front,snapshot,sizeof front)==0,"unregister does not clear front quarantine");
    fb_set_native_present(native);fb_present();
    ck(calls==before && memcmp(front,snapshot,sizeof front)==0,"reregister does not clear front quarantine");
    ck(fb_present_px()==px && fb_present_calls()==n,"quarantined presents never inflate counters");
    printf("FB_NATIVE_PRESENT: %u checks, %u failures\n",checks,fails);
    return fails?1:0;
}
