/* Guest: tcc effects.c -I/usr/include/openlogit -lopenlogit -o /tmp/effects
 * A small interactive SDK consumer: one offscreen card, native group opacity,
 * shadow/blur/backdrop commands, and a continuously retargetable SDK spring.
 * All buffers are owned here; the kernel needs no effect allocator or 3D VM. */
#include "openlogit_window.h"
#include "openlogit_anim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "example_log.h"
struct image {struct ol_surface *surface;void *storage;unsigned char *front,*work;};
static struct ol_device *device;static struct ol_list *list;
static void *dm,*lm,*scratch;static unsigned long scratch_bytes;
static struct image frame,card,background;
static int last_x=40;
static int image_create(struct image *im,int w,int h)
{
    im->storage=malloc(ol_surface_size());im->front=calloc(1,w*h*4);im->work=malloc(w*h*4);
    if(!im->storage||!im->front||!im->work)return 0;
    struct ol_surface_desc desc={sizeof desc,OL_FORMAT_RGBA8_STRAIGHT,w,h,w*4,
        im->front,im->work,w*h*4,w*h*4};
    return !ol_surface_create(device,im->storage,ol_surface_size(),&desc,&im->surface);
}
static void image_destroy(struct image *im)
{if(im->surface)ol_surface_destroy(im->surface);free(im->storage);free(im->front);free(im->work);}
static int shape(int x,int y,int w,int h,int radius,unsigned color,unsigned color2)
{
    int points[1024],subs[8];struct gfx_path p;gfx_path_init(&p,points,512,subs,8);
    gfx_path_rrect(&p,GFX_PX(x),GFX_PX(y),GFX_PX(w),GFX_PX(h),GFX_PX(radius));
    struct gfx_paint paint;gfx_paint_linear(&paint,0,GFX_PX(y),0,GFX_PX(y+h));
    gfx_paint_stop(&paint,0,color,255);gfx_paint_stop(&paint,65536,color2,255);
    return ol_cmd_fill(list,&p,GFX_NONZERO,&paint,0,4);
}
static int draw(float position,int blur)
{
    uint64_t started=monotonic_ns();int x=(int)(40+position*390);
    ol_list_reset(list);
    /* The decorative background and its glass are immutable. Restore only
     * old/new card bounds (including the downward shadow) from that surface,
     * instead of rerasterizing seven gradients and glass every motion frame. */
    struct gfx_rect damage=ol_damage_move((struct gfx_rect){last_x,116,160,128},
        (struct gfx_rect){x,116,160,128},21,640,320);
    struct ol_layer_options restore={.size=sizeof restore,.opacity=255,.clip=&damage};
    ol_cmd_layer(list,background.surface,(struct gfx_rect){0,0,640,320},&restore);
    struct ol_layer_options effect={.size=sizeof effect,.opacity=220,.bilinear=1,
        .blur_radius=blur?4:0,.shadow_radius=9,.shadow_dy=12,.shadow_opacity=175,.shadow_rgb=0x000009};
    ol_cmd_layer(list,card.surface,(struct gfx_rect){x,116,160,128},&effect);
    int r=ol_list_close(list);unsigned long needed=0;
    if(!r)r=ol_submit_workspace_size(list,frame.surface,&needed);
    if(!r&&needed>scratch_bytes) {
        void *next=malloc(needed);if(!next)return 0;
        free(scratch);scratch=next;scratch_bytes=needed;
    }
    struct ol_submit_info info={.size=sizeof info};
    if(!r)r=ol_submit_workspace(device,list,frame.surface,scratch,scratch_bytes,&info);
    if(r){fprintf(stderr,"EFFECTS FAIL %s\n",ol_status_string(r));return 0;}
    ol_window_clear(0x101827);
    if(ol_window_composite(frame.surface,10,80,640,320))return 0;
    const char *title="OpenLogit Motion & Materials";
    ol_window_text_run(26,16,24,0,0xeaf3ff,title,(int)strlen(title));
    const char *help="Space: move card     B: blur     Esc: close";
    ol_window_text_run(26,49,13,0,0x9ab2cf,help,(int)strlen(help));
    const char *caption="One group. One opacity. Shared spring animation.";
    ol_window_text_run(46,116,16,0,0xf2f7ff,caption,(int)strlen(caption));
    if(ol_window_present())return 0;
    last_x=x;
    example_log("EFFECTS FRAME x=%d blur=%d scratch=%lu copied=%llu render_us=%llu\n",x,blur,
           scratch_bytes,(unsigned long long)info.copied_bytes,(unsigned long long)((monotonic_ns()-started)/1000));
    return 1;
}
int main(void)
{
    dm=malloc(ol_device_size());lm=malloc(32768);
    if(!dm||!lm||ol_device_create(dm,ol_device_size(),OL_API_VERSION,OL_CAP_LAYER|OL_CAP_BACKDROP,&device)||
       ol_list_create(device,lm,32768,&list)||!image_create(&frame,640,320)||!image_create(&card,160,128)||
       !image_create(&background,640,320))return 1;
    ol_cmd_clear(list,0,0);shape(0,0,160,128,20,0x63ecd2,0x4273e6);
    shape(22,24,74,12,6,0xe0fff4,0xc5e8f9);shape(22,48,114,8,4,0x84cedb,0x98bded);
    shape(22,64,96,8,4,0x84cedb,0x98bded);shape(22,92,46,16,8,0xe0fff4,0xd5eef9);
    if(ol_list_close(list)||ol_submit(device,list,card.surface,0))return 1;
    ol_list_reset(list);ol_cmd_clear(list,0x111c31,255);
    for(int i=0;i<7;i++)shape(40+i*88,16,28,290,12,i&1?0x254763:0x324267,0x172438);
    ol_cmd_backdrop(list,(struct gfx_rect){20,18,600,52},8,0x516989,95);
    if(ol_list_close(list)||ol_submit_workspace_size(list,background.surface,&scratch_bytes))return 1;
    scratch=malloc(scratch_bytes);
    if(!scratch||ol_submit_workspace(device,list,background.surface,scratch,scratch_bytes,0)||
       ol_surface_upload(frame.surface,background.front,640*320*4,640*4))return 1;
    ol_list_reset(list);gui_create("OpenLogit Effects",660,420);
    _sys(SYS_GUI_WIN_MIN,(660L<<16)|420,0,0);
    struct ol_spring spring;uint64_t now=monotonic_ns();ol_spring_init(&spring,0,0,0,1.5f,.85f,now,1300000000);
    int goal=0,blur=0,dirty=1,reduced=setting_int("ui.reduce_motion",0)!=0,was_active=0,result=0;
    example_log("EFFECTS READY api=1.1 native_layers=1\n");
    for(;;) {
        struct logit_event e;
        while(poll_event(&e)) {
            if(e.type==EV_CLOSE||(e.type==EV_KEY&&e.a==27))goto done;
            if(e.type==EV_KEY&&e.a==' '){goal=!goal;ol_spring_retarget(&spring,monotonic_ns(),goal);dirty=1;}
            if(e.type==EV_KEY&&(e.a=='b'||e.a=='B')){blur=!blur;dirty=1;}
            if(e.type==EV_THEME||e.type==EV_WINDOW_FOCUS||e.type==EV_RESIZE){
                dirty=1;reduced=setting_int("ui.reduce_motion",0)!=0;
                example_log("EFFECTS MOTION reduced=%d\n",reduced);
            }
        }
        if(!ol_window_visible()){wait_idle(0);continue;}
        int active=0;float position=reduced?goal:ol_spring_sample(&spring,monotonic_ns(),0,&active);
        int drew=dirty||active||was_active;
        if(drew) {if(!draw(position,blur)){result=1;goto done;}dirty=0;}
        if(drew&&!active){example_log("EFFECTS IDLE x=%d\n",goal?430:40);}
        was_active=active;wait_idle(active?16:0);
    }
done:
    ol_list_destroy(list);image_destroy(&card);image_destroy(&background);image_destroy(&frame);ol_device_destroy(device);
    free(dm);free(lm);free(scratch);return result;
}
