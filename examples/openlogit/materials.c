/* Guest: tcc materials.c -I/usr/include/openlogit -lopenlogit -o /tmp/materials
 * Material Studio is a small graphics-tool consumer: editable OLS programs,
 * live uniforms, texture inspection and motion controls. Hit testing, focus,
 * labels and state belong here; shading, rasterization, animation and final
 * composition belong to OpenLogit. No private pixel loop or animation engine.
 */
#include "openlogit_window.h"
#include "openlogit_material.h"
#include "example_log.h"
#include "ui_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct image {struct ol_surface *surface;void *storage;unsigned char *front,*work;};
struct material_view {struct image image;struct ol_material *material;struct ol_material_context *context;float last;};
static struct ol_device *device;static struct ol_list *list;static void *dm,*lm;
static struct image frame,background,texture;
static struct material_view progress_view,shimmer_view,ripple_view,texture_view;
static struct ol_timeline progress,shimmer,ripple;
static int busy,tint=1,gain=55,focus,pressed=-1,dirty=1,reduced,dragging;
static unsigned sequence,passes;
static int image_create(struct image *im,int w,int h)
{
    im->storage=malloc(ol_surface_size());im->front=calloc(1,w*h*4);im->work=malloc(w*h*4);
    if(!im->storage||!im->front||!im->work)return 0;
    struct ol_surface_desc desc={sizeof desc,OL_FORMAT_RGBA8_STRAIGHT,w,h,w*4,im->front,im->work,w*h*4,w*h*4};
    return !ol_surface_create(device,im->storage,ol_surface_size(),&desc,&im->surface);
}
static void image_destroy(struct image *im)
{if(im->surface)ol_surface_destroy(im->surface);free(im->storage);free(im->front);free(im->work);}
static int material_create(struct material_view *v,const char *src,int w,int h)
{
    struct ol_shader_program program;struct ol_shader_error error;v->last=-100;
    if(ol_shader_compile(src,strlen(src),&program,&error)||ol_material_create(&program,&v->material,&error)) {
        example_log("MATERIAL ERROR %s\n",error.message);return 0;
    }
    return !ol_material_context_create(w,h,&v->context)&&image_create(&v->image,w,h);
}
static void material_destroy(struct material_view *v)
{ol_material_destroy(v->material);ol_material_context_destroy(v->context);image_destroy(&v->image);}
static int shade(struct material_view *v,float value,const struct ol3d_bindings *bindings)
{
    if(v->last==value)return 1;
    struct ol_shader_error error;
    if(ol_material_render(v->context,v->material,bindings,v->image.surface,&error)) {
        example_log("MATERIAL ERROR %s\n",error.message);return 0;
    }
    v->last=value;passes++;return 1;
}
static void rect(int x,int y,int w,int h,int radius,unsigned rgb)
{
    int pts[1024],subs[8];struct gfx_path p;struct gfx_paint paint;
    gfx_path_init(&p,pts,512,subs,8);gfx_path_rrect(&p,GFX_PX(x),GFX_PX(y),GFX_PX(w),GFX_PX(h),GFX_PX(radius));
    gfx_paint_solid(&paint,rgb,255);ol_cmd_fill(list,&p,GFX_NONZERO,&paint,0,4);
}
static void layer(struct image *im,int x,int y,int w,int h)
{
    struct ol_layer_options options={.size=sizeof options,.opacity=255,.bilinear=1};
    ol_cmd_layer(list,im->surface,(struct gfx_rect){x,y,w,h},&options);
}
static void label(int x,int y,int size,unsigned rgb,const char *s)
{ol_window_text_run(x,y,size,0,rgb,s,(int)strlen(s));}
static const struct gfx_rect controls[]={{44,218,132,38},{192,218,132,38},{44,286,280,64},
                                       {408,330,140,36},{568,330,140,36},{408,390,300,20}};
static int hit(int x,int y)
{
    for(int i=0;i<6;i++){struct gfx_rect r=controls[i];if(x>=r.x&&y>=r.y&&x<r.x+r.w&&y<r.y+r.h)return i;}
    return -1;
}
static void begin_motion(struct ol_timeline *t,uint64_t now)
{ol_animation_seek(t,now,0);ol_animation_play(t,now);}
static void activate(int id,uint64_t now)
{
    if(id==0)begin_motion(&progress,now);
    if(id==1){busy=!busy;if(busy)ol_animation_play(&shimmer,now);else ol_animation_pause(&shimmer,now);}
    if(id==2)begin_motion(&ripple,now);
    if(id==3)tint=!tint;
    if(id==4){gain=55;tint=1;busy=0;ol_animation_pause(&shimmer,now);ol_animation_seek(&shimmer,now,0);
        ol_animation_pause(&progress,now);ol_animation_seek(&progress,now,0);
        ol_animation_pause(&ripple,now);ol_animation_seek(&ripple,now,ripple.desc.duration_ns);}
    dirty=1;
}
static void set_gain(int x)
{gain=(x-408)*100/300;if(gain<0)gain=0;if(gain>100)gain=100;dirty=1;}
static int draw(float p,float s,float r,int active)
{
    /* Only changed materials rerun their pixel pass. Static textures and
     * labels survive motion in another panel; all panels publish together. */
    float u[3][4]={{p,p,p,p},{.29f,.86f,.76f,1},{.13f,.20f,.30f,1}};
    struct ol3d_bindings bindings={.uniforms=u,.uniform_count=3};
    if(!shade(&progress_view,p,&bindings))return 0;
    u[0][0]=u[0][1]=u[0][2]=u[0][3]=s;
    bindings.uniform_count=1;if(!shade(&shimmer_view,s,&bindings))return 0;
    u[0][0]=u[0][1]=u[0][2]=u[0][3]=r*5.2f;
    u[1][0]=u[1][1]=u[1][2]=0;u[1][3]=(1-r)*.7f;
    bindings.uniform_count=2;if(!shade(&ripple_view,r,&bindings))return 0;
    float amount=tint?gain/100.f:0;
    u[0][0]=1+amount*.25f;u[0][1]=1-amount*.25f;u[0][2]=1-amount*.65f;u[0][3]=1;
    struct ol3d_texture sampler={texture.front,64,64,256,64*64*4,1,0};
    bindings=(struct ol3d_bindings){u,1,&sampler,1};
    if(!shade(&texture_view,amount,&bindings))return 0;
    ol_list_reset(list);layer(&background,0,0,760,540);
    for(int i=0;i<5;i++) {
        struct gfx_rect c=controls[i];
        if(focus==i)rect(c.x-2,c.y-2,c.w+4,c.h+4,9,0x66daca);
        rect(c.x,c.y,c.w,c.h,7,pressed==i?0x38566e:0x283e54);
    }
    layer(&progress_view.image,44,176,280,18);
    layer(&ripple_view.image,44,286,280,64);
    layer(&texture_view.image,408,170,300,136);
    layer(&shimmer_view.image,44,374,200,10);
    layer(&shimmer_view.image,44,394,280,10);
    layer(&shimmer_view.image,44,414,164,10);
    rect(408,396,300,6,3,0x34495d);rect(408,396,gain*3,6,3,0x72dccc);
    if(focus==5)rect(408+gain*3-8,389,16,20,7,0xcaf6ec);
    else rect(408+gain*3-5,391,10,16,5,0xe3f1f7);
    if(ol_list_close(list)||ol_submit(device,list,frame.surface,0)||
       ol_window_composite(frame.surface,0,0,760,540))return 0;
    label(24,20,27,0xf1f6fb,"OpenLogit / Material Studio");
    label(26,61,13,0x9aafc5,"Programmable UI   /   OLS-IR v1   /   Software backend");
    label(44,128,18,0xe8f0f8,"Motion controls");label(408,128,18,0xe8f0f8,"Texture inspector");
    label(65,228,14,0xe7f5fb,"Run progress");label(205,228,14,0xe7f5fb,busy?"Stop loading":"Start loading");
    label(108,309,16,0xe7f5fb,"Press ripple");
    label(432,340,14,0xe7f5fb,tint?"Tint enabled":"Tint disabled");label(613,340,14,0xe7f5fb,"Reset");
    char status[160];snprintf(status,sizeof status,"Progress %d%%",(int)(p*100+.5f));label(44,152,12,0x91adbf,status);
    snprintf(status,sizeof status,"Warmth %d%%",gain);label(408,371,12,0x91adbf,status);
    label(44,355,12,0x91adbf,busy?"Loading preview / shared timeline":"Loading paused");
    snprintf(status,sizeof status,"%s   |   %s   |   %u material passes",active?"Animating":"Idle",reduced?"Reduced motion":"Full motion",passes);
    label(42,474,13,0xc8e5de,status);
    label(26,514,11,0x8ca2b8,"P progress    L loading    Space ripple    C tint    R reset    Tab focus / Enter    Esc close");
    if(ol_window_present())return 0;
    example_log("MATERIAL FRAME n=%u p=%d ripple=%d busy=%d tint=%d gain=%d active=%d passes=%u\n",
        ++sequence,(int)(p*1000+.5f),(int)(r*1000+.5f),busy,tint,gain,active,passes);
    return 1;
}
int main(void)
{
    int result=1;dm=malloc(ol_device_size());lm=malloc(65536);
    if(!dm||!lm||ol_device_create(dm,ol_device_size(),OL_API_VERSION,OL_CAP_LAYER,&device)||
       ol_list_create(device,lm,65536,&list)||!image_create(&frame,760,540)||!image_create(&background,760,540)||
       !image_create(&texture,64,64))goto done;
    if(!material_create(&progress_view,ui_progress_shader,280,18)||!material_create(&shimmer_view,ui_shimmer_shader,200,10)||
       !material_create(&ripple_view,ui_ripple_shader,280,64)||!material_create(&texture_view,ui_texture_shader,300,136))goto done;
    ol_cmd_clear(list,0x111d2c,255);rect(24,108,324,338,14,0x1b2c3f);rect(384,108,352,338,14,0x1b2c3f);
    rect(24,462,712,38,10,0x203a43);
    if(ol_list_close(list)||ol_submit(device,list,background.surface,0))goto done;
    ol_list_reset(list);ol_cmd_clear(list,0x5389b5,255);
    rect(6,8,14,14,7,0xe9db9a);rect(0,37,64,27,0,0x275970);rect(0,46,64,18,0,0x388b8c);
    rect(29,20,22,34,3,0xcee0cf);rect(33,25,6,8,1,0x326687);rect(42,25,5,8,1,0x326687);
    rect(33,37,14,4,1,0x729897);
    if(ol_list_close(list)||ol_submit(device,list,texture.surface,0))goto done;
    ol_list_reset(list);
    uint64_t now=monotonic_ns();
    static const struct ol_keyframe keys[]={{.offset=0,.value={0},.easing={.kind=OL_EASE_INOUT}},
                                           {.offset=1,.value={1}}};
    static const struct ol_keyframe linear[]={{.offset=0,.value={0}},{.offset=1,.value={1}}};
    struct ol_animation_desc desc={sizeof desc,OL_SCALAR,2,OL_FORWARD,1,2200000000,0,keys};
    if(ol_animation_init(&progress,&desc,now)||ol_animation_pause(&progress,now))goto done;
    desc.keys=linear;desc.duration_ns=1000000000;
    if(ol_animation_init(&ripple,&desc,now)||ol_animation_pause(&ripple,now)||
       ol_animation_seek(&ripple,now,desc.duration_ns))goto done;
    desc.iterations=0;desc.duration_ns=1800000000;
    if(ol_animation_init(&shimmer,&desc,now)||ol_animation_pause(&shimmer,now))goto done;
    gui_create("OpenLogit Material Studio",760,540);_sys(SYS_GUI_WIN_MIN,(760L<<16)|540,0,0);
    reduced=setting_int("ui.reduce_motion",0)!=0;
    example_log("MATERIAL READY api=1.1 pixel_material=1 gpu=0\n");
    int was_active=0;result=0;
    for(;;) {
        struct logit_event e;
        while(poll_event(&e)) {
            now=monotonic_ns();
            if(e.type==EV_CLOSE||(e.type==EV_KEY&&e.a==27))goto done;
            if(e.type==EV_THEME||e.type==EV_WINDOW_FOCUS||e.type==EV_RESIZE) {
                dirty=1;reduced=setting_int("ui.reduce_motion",0)!=0;
                example_log("MATERIAL MOTION reduced=%d\n",reduced);
            }
            if(e.type==EV_MOUSE&&e.button!=EV_BTN_RIGHT){pressed=hit(e.a,e.b);if(pressed>=0)focus=pressed;
                dragging=pressed==5;if(dragging)set_gain(e.a);dirty=1;}
            if(e.type==EV_MOUSE_MOVE&&dragging)set_gain(e.a);
            if(e.type==EV_MOUSE_UP&&e.button!=EV_BTN_RIGHT){if(pressed>=0&&pressed==hit(e.a,e.b))activate(pressed,now);
                pressed=-1;dragging=0;dirty=1;}
            if(e.type==EV_KEY) {
                if(e.a=='\t'){focus=(focus+1)%6;dirty=1;}
                if(e.a=='\n'||e.a=='\r')activate(focus,now);
                if(e.a=='p'||e.a=='P')activate(0,now);
                if(e.a=='l'||e.a=='L')activate(1,now);
                if(e.a==' ')activate(2,now);
                if(e.a=='c'||e.a=='C')activate(3,now);
                if(e.a=='r'||e.a=='R')activate(4,now);
                if(focus==5&&(e.a==KEY_LEFT||e.a==KEY_RIGHT))set_gain(408+(gain+(e.a==KEY_RIGHT?5:-5))*3);
            }
        }
        if(!ol_window_visible()){wait_idle(0);continue;}
        /* One monotonic sample instant for the complete UI. The infinite
         * loading track is dormant when paused/reduced/hidden. A final redraw
         * clears the ripple; stopping at active==false would leave its halo. */
        now=monotonic_ns();struct ol_anim_sample ps,ss,rs;
        ol_animation_sample(&progress,now,reduced,&ps);ol_animation_sample(&shimmer,now,0,&ss);
        ol_animation_sample(&ripple,now,reduced,&rs);
        float s=reduced?.5f:ss.value[0];int active=ps.active||rs.active||(!reduced&&busy&&ss.active);
        if(dirty||active||was_active){if(!draw(ps.value[0],s,rs.value[0],active)){result=1;goto done;}dirty=0;}
        was_active=active;wait_idle(active?16:0);
    }
done:
    if(list)ol_list_destroy(list);
    material_destroy(&progress_view);material_destroy(&shimmer_view);material_destroy(&ripple_view);material_destroy(&texture_view);
    image_destroy(&frame);image_destroy(&background);image_destroy(&texture);
    if(device)ol_device_destroy(device);free(dm);free(lm);return result;
}
