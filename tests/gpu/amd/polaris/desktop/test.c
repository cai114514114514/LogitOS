/* Desktop wiring test ONLY. Native binding and runtime protocols are explicit
 * stubs here; their real implementations are exercised by separate gates. Each
 * case runs in a fresh process so no test-only reset API enters the product. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amd/polaris/desktop.h"
#include "fb.h"
static unsigned checks,failures,lock_depth,bind_calls,start_calls,present_calls,installs;
static int boot_available=1,bind_error,start_error,present_error;
static uint64_t boot_address=0xc0001000ull,boot_bytes=272*32;
static fb_native_present_fn installed;
static struct polaris_native *native_context;
static struct polaris_runtime *runtime_context;
static uint64_t last_bytes;static uint32_t last_stride,last_rect[4];
static const uint32_t *last_pixels;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL line %d: %s\n",__LINE__,#x);}}while(0)
void fb_graphics_lock(void){++lock_depth;}
void fb_graphics_unlock(void){CHECK(lock_depth>0);--lock_depth;}
uint32_t fb_width(void){return 64;}
uint32_t fb_height(void){return 32;}
int fb_boot_lfb_range(uint64_t *a,uint64_t *b)
{CHECK(lock_depth>0);if(!boot_available)return 0;*a=boot_address;*b=boot_bytes;return 1;}
void fb_set_native_present(fb_native_present_fn fn)
{CHECK(lock_depth>0);CHECK(start_calls==1);CHECK(runtime_context->stage==POLARIS_RUNTIME_ACTIVE);installed=fn;++installs;}
void kprintf(const char *format,...){(void)format;CHECK(lock_depth>0);}
int polaris_native_bind(struct polaris_native *n,const struct polaris_native_resources *r,struct polaris_platform *out)
{
    CHECK(lock_depth>0);++bind_calls;if(bind_error)return -1;
    n->resource=*r;n->bound=1;n->vram_base=UINT64_C(1)<<32;n->vram_bytes=UINT64_C(4)<<30;
    native_context=n;*out=(struct polaris_platform){.opaque=n};return 0;
}
int polaris_runtime_start(struct polaris_runtime *r,const struct polaris_platform *p,const struct polaris_runtime_request *q)
{
    CHECK(lock_depth>0);CHECK(p->opaque==native_context);CHECK(q->pitch==272);++start_calls;runtime_context=r;
    r->attempted=1;r->stage=start_error?POLARIS_RUNTIME_FAILED:POLARIS_RUNTIME_ACTIVE;
    if(start_error){r->quarantined=1;r->error=-5;r->failed_stage=POLARIS_RUNTIME_SMU;return -2;}
    r->queue.submitted=2;r->queue.completed=2;return 0;
}
int polaris_runtime_present(struct polaris_runtime *r,const uint32_t *p,uint64_t bytes,uint32_t stride,
                              uint32_t x,uint32_t y,uint32_t w,uint32_t h)
{
    CHECK(lock_depth>0);CHECK(r==runtime_context);++present_calls;
    last_pixels=p;last_bytes=bytes;last_stride=stride;last_rect[0]=x;last_rect[1]=y;last_rect[2]=w;last_rect[3]=h;
    if(present_error==-2){r->quarantined=1;r->stage=POLARIS_RUNTIME_FAILED;r->failed_stage=POLARIS_RUNTIME_ACTIVE;r->error=-2;}
    if(!present_error){r->present.frames++;r->present.pixels+=(uint64_t)w*h;r->present.uploaded_bytes+=(uint64_t)w*h*4;r->queue.completed++;r->queue.submitted++;}
    return present_error;
}
static void success(struct polaris_native_resources *res,struct polaris_runtime_request *q)
{
    CHECK(!polaris_desktop_start(res,q));CHECK(installed && installs==1 && !lock_depth);
    CHECK(bind_calls==1 && start_calls==1);
    static uint32_t pixels[64*32];
    fb_graphics_lock();CHECK(!installed(pixels,64,2,3,5,7));fb_graphics_unlock();
    CHECK(last_pixels==pixels && last_bytes==8192);
    CHECK(last_stride==256); /* callback pixels -> runtime BYTES, not scanout pitch */
    CHECK(last_rect[0]==2 && last_rect[1]==3 && last_rect[2]==5 && last_rect[3]==7);
    struct polaris_desktop_info info;CHECK(!polaris_desktop_query(&info));
    CHECK(info.stage==POLARIS_RUNTIME_ACTIVE && info.completed==3 && info.frames==1 && info.pixels==35 && info.uploaded_bytes==140);
    present_error=-1;fb_graphics_lock();CHECK(installed(pixels,64,0,0,1,1)==-1);fb_graphics_unlock();
    CHECK(!polaris_desktop_query(&info) && !info.quarantined);
    unsigned calls=present_calls;
    fb_graphics_lock();CHECK(installed(pixels,UINT32_MAX,0,0,1,1)==-1);fb_graphics_unlock();
    CHECK(present_calls==calls);
    present_error=-2;fb_graphics_lock();CHECK(installed(pixels,64,0,0,1,1)==-2);fb_graphics_unlock();
    CHECK(!polaris_desktop_query(&info));CHECK(info.quarantined && info.error==-2 && info.failed_stage==POLARIS_RUNTIME_ACTIVE && info.completed==3);
    CHECK(polaris_desktop_start(res,q)<0 && installs==1 && start_calls==1);
    CHECK(polaris_desktop_query(NULL)<0);CHECK(!lock_depth);
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    struct polaris_native_resources res={.bar0_physical=0xc0000000,.bar0_bytes=256u<<20,
        .arena={0x100200000ull,2u<<20},.scanout={0x100001000ull,272*32}};
    struct polaris_runtime_request q={.width=64,.height=32,.pitch=272};
    q.memory.arena=res.arena;q.memory.scanout=res.scanout;
    q.memory.vram=(struct polaris_memory_range){UINT64_C(1)<<32,UINT64_C(4)<<30};
    q.memory.aperture=(struct polaris_memory_range){UINT64_C(1)<<32,res.bar0_bytes};q.memory.aperture_cpu_base=res.bar0_physical;
    if(!strcmp(argv[1],"success"))success(&res,&q);
    else {
        unsigned expected_bind=0,expected_start=0;
        if(!strcmp(argv[1],"no-boot"))boot_available=0;
        else if(!strcmp(argv[1],"geometry"))q.width++;
        else if(!strcmp(argv[1],"lease"))q.memory.arena.bytes-=4096;
        else if(!strcmp(argv[1],"bind")){bind_error=1;expected_bind=1;}
        else if(!strcmp(argv[1],"address")){boot_address+=4;expected_bind=1;}
        else if(!strcmp(argv[1],"translation")){q.memory.aperture_cpu_base+=4096;expected_bind=1;}
        else if(!strcmp(argv[1],"runtime")){start_error=1;expected_bind=1;expected_start=1;}
        else return 2;
        CHECK(polaris_desktop_start(&res,&q)<0);CHECK(!installed && !installs);
        CHECK(bind_calls==expected_bind && start_calls==expected_start && !lock_depth);
        if(start_error){struct polaris_desktop_info info;CHECK(!polaris_desktop_query(&info));
            CHECK(info.quarantined && info.error==-5 && info.failed_stage==POLARIS_RUNTIME_SMU);}
    }
    printf("DESKTOP %s: %u checks, %u failures (wiring stubs; no protocol or hardware proof)\n",argv[1],checks,failures);
    return failures?1:0;
}
