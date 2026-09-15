/* Sky Islands -- a complete, editable OpenLogit SDK consumer.
 * Guest build: tcc island.c -I/usr/include/openlogit -lopenlogit -o /tmp/island
 * Run /tmp/island. No private rasterizer, timer thread or shader shortcut.
 * The simulation uses fixed steps; all visual curves sample one shared time.
 * Frame timings below are guest render/submit time, NOT displayed frame rate. */
#include "openlogit_3d.h"
#include "openlogit_window.h"
#include "island_game.h"
#include "island_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "example_log.h"

static struct ol3d_context *renderer;
static struct ol_device *device;
static struct ol_surface *scene;
static void *device_mem,*surface_mem;
static unsigned char *front,*work;
static struct ol_shader_program vertex_program,pixel_program;
static struct ol3d_vertex cube[24];
static uint32_t indices[36];
static struct ol3d_pipeline_object *pipeline;
static struct ol3d_buffer *vertices_buffer,*index_buffer;
static struct ol3d_bindings bindings;
static float uniforms[11][4],view_projection[16];
static struct ol3d_texture checker;
static unsigned char texels[]={255,255,255,255,220,228,238,255,220,228,238,255,255,255,255,255};
static struct ol_timeline sway,walk,bob,pulse;
static struct ol_spring jump_pose;
static int pose_grounded=1;
static struct ol_keyframe sway_keys[2]={
    {0,{-1,0,0,0},{.kind=OL_EASE_INOUT}}, {1,{1,0,0,0},{.kind=OL_LINEAR}}};
static struct ol_keyframe walk_keys[2]={
    {0,{-.28f,0,0,.96f},{.kind=OL_EASE_INOUT}}, {1,{.28f,0,0,.96f},{.kind=OL_LINEAR}}};
static struct ol_keyframe bob_keys[2]={
    {0,{0,0,0,0},{.kind=OL_EASE_INOUT}}, {1,{.045f,0,0,0},{.kind=OL_LINEAR}}};
static struct ol_keyframe pulse_keys[2]={
    {0,{.1f,0,0,0},{.kind=OL_EASE_INOUT}}, {1,{.55f,0,0,0},{.kind=OL_LINEAR}}};
static int width=640,height=360,window_w=800,window_h=540;
static uint64_t frames,render_ns[120];
static int trace_enabled;

static int target(int low)
{
    unsigned w=low?320:640,h=low?180:360;
    struct ol3d_context *next=0;
    unsigned char *f=calloc(1,w*h*4),*b=malloc(w*h*4);
    void *sm=malloc(ol_surface_size());struct ol_surface *s=0;
    if(!f||!b||!sm||ol3d_create(w,h,&next)){free(f);free(b);free(sm);return 0;}
    struct ol_surface_desc desc={sizeof desc,OL_FORMAT_RGBA8_STRAIGHT,w,h,w*4,f,b,w*h*4,w*h*4};
    if(ol_surface_create(device,sm,ol_surface_size(),&desc,&s)) {
        free(f);free(b);free(sm);ol3d_destroy(next);return 0;
    }
    if(scene)ol_surface_destroy(scene);
    ol3d_destroy(renderer);free(front);free(work);free(surface_mem);
    renderer=next;scene=s;front=f;work=b;surface_mem=sm;width=w;height=h;
    return 1;
}
static int init(void)
{
    device_mem=malloc(ol_device_size());
    if(!device_mem||ol_device_create(device_mem,ol_device_size(),OL_API_VERSION,OL_CAP_IMAGE,&device))return 0;
    if(!target(0))return 0;
    struct ol_shader_error error;
    if(ol_shader_compile(island_vertex_shader,strlen(island_vertex_shader),&vertex_program,&error)||
       ol_shader_compile(island_pixel_shader,strlen(island_pixel_shader),&pixel_program,&error)) {
        fprintf(stderr,"shader:%u: %s\n",error.line,error.message);return 0;
    }
    /* Each face has its own vertices for discontinuous flat normals and UVs.
     * Winding is CCW from outside; the SDK performs clipping/culling/depth. */
    static const float corners[8][3]={{-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,.5f,-.5f},{-.5f,.5f,-.5f},
        {-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f}};
    static const unsigned face[6][4]={{4,5,6,7},{1,0,3,2},{0,4,7,3},{5,1,2,6},{3,7,6,2},{0,1,5,4}};
    static const float normals[6][3]={{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    static const float uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
    for(int f=0;f<6;f++) {
        for(int j=0;j<4;j++) {
            memcpy(cube[f*4+j].attribute[0],corners[face[f][j]],3*sizeof(float));
            cube[f*4+j].attribute[0][3]=1;
            memcpy(cube[f*4+j].attribute[2],normals[f],3*sizeof(float));
            memcpy(cube[f*4+j].attribute[3],uv[j],2*sizeof(float));
        }
        unsigned local[6]={0,1,2,0,2,3};for(int j=0;j<6;j++)indices[f*6+j]=f*4+local[j];
    }
    checker=(struct ol3d_texture){texels,2,2,8,sizeof texels,0,1};
    struct ol3d_pipeline state={&vertex_program,&pixel_program,2,1,1,1,0};
    if(ol3d_pipeline_create(&state,&pipeline,&error)||
       ol3d_buffer_create(OL3D_VERTEX_BUFFER,cube,24,&vertices_buffer)||
       ol3d_buffer_create(OL3D_INDEX_BUFFER,indices,36,&index_buffer))return 0;
    bindings=(struct ol3d_bindings){uniforms,11,&checker,1};
    ol_spring_init(&jump_pose,0,0,0,5,1,0,300000000);
    struct ol_animation_desc a={sizeof a,OL_SCALAR,2,OL_ALTERNATE,0,3000000000ull,0,sway_keys};
    if(ol_animation_init(&sway,&a,0))return 0;
    a.kind=OL_QUATERNION;a.duration_ns=260000000;a.keys=walk_keys;
    if(ol_animation_init(&walk,&a,0))return 0;
    a.kind=OL_SCALAR;a.duration_ns=900000000;a.keys=bob_keys;
    if(ol_animation_init(&bob,&a,0))return 0;
    a.duration_ns=700000000;a.keys=pulse_keys;
    return ol_animation_init(&pulse,&a,0)==OL_OK;
}
static int box_matrix(const float model[16],unsigned rgb,float glow,float hit)
{
    float mvp[16];ol_mat4_mul(mvp,view_projection,model);memcpy(uniforms,mvp,sizeof mvp);
    const float light[3]={.35f,.85f,.39f};
    /* Inverse rotation maps light into the cube's local normal space. Each
     * model column has its own scale; remove that before the dot product. */
    for(int j=0;j<3;j++) {
        float n=sqrtf(model[j*4]*model[j*4]+model[j*4+1]*model[j*4+1]+model[j*4+2]*model[j*4+2]);
        uniforms[4][j]=n>0?(model[j*4]*light[0]+model[j*4+1]*light[1]+model[j*4+2]*light[2])/n:0;
    }
    uniforms[4][3]=0;
    uniforms[5][0]=glow+hit;uniforms[5][1]=glow*.72f;uniforms[5][2]=glow*.22f;uniforms[5][3]=0;
    uniforms[6][0]=GFX_R(rgb)/255.f;uniforms[6][1]=GFX_G(rgb)/255.f;
    uniforms[6][2]=GFX_B(rgb)/255.f;uniforms[6][3]=1;
    if(ol_mat4_normal(&uniforms[7][0],model))return OL_ARGUMENT;
    return ol3d_draw_buffers(renderer,pipeline,&bindings,ol3d_buffer_view(vertices_buffer),
                              ol3d_buffer_view(index_buffer),0,36);
}
static int box(float x,float y,float z,float w,float h,float d,unsigned color,float glow)
{
    float m[16],p[3]={x,y,z},q[4]={0,0,0,1},s[3]={w,h,d};ol_mat4_trs(m,p,q,s);
    return box_matrix(m,color,glow,0);
}
static void label(int x,int y,int size,unsigned color,const char *text)
{ol_window_text_run(x,y,size,0,color,text,(int)strlen(text));}
static int render(struct island_game *g,int moving,uint64_t time_ns,int reduced)
{
    uint64_t start=monotonic_ns();
    float eye[3]={g->x+sin(g->yaw)*8,g->y+2.2f+g->pitch*6,g->z+cos(g->yaw)*8};
    float center[3]={g->x,g->y+.7f,g->z-1},up[3]={0,1,0},v[16],p[16];
    if(ol_mat4_look_at(v,eye,center,up)||ol_mat4_perspective(p,.95f,16.f/9,.15f,90))return 0;
    ol_mat4_mul(view_projection,p,v);
    if(ol3d_begin(renderer,0x17263f))return 0;
    int error=0;
    for(int i=0;i<4&&!error;i++) {
        struct island_platform *b=&g->platforms[i];
        error=box(b->x,b->y,b->z,b->w,b->h,b->d,i==1?0x41bbd1:0x4bbd91,0);
    }
    struct ol_anim_sample breathing,stepping,glowing;
    ol_animation_sample(&bob,time_ns,reduced,&breathing);
    ol_animation_sample(&walk,time_ns,reduced,&stepping);
    ol_animation_sample(&pulse,time_ns,reduced,&glowing);
    for(int i=0;i<3&&!error;i++)if(!g->collected[i])
        error=box(0,island_collect_y[i]+breathing.value[0],island_collect_z[i],.38f,.65f,.38f,0xffcb57,glowing.value[0]);
    struct ol3d_node nodes[7]={
        {-1,{g->x,g->y,g->z},{0,0,0,1},{1,1,1}},
        {0,{0,.7f,0},{0,0,0,1},{.55f,.65f,.4f}},
        {0,{0,1.23f,0},{0,0,0,1},{.45f,.42f,.42f}},
        {0,{-.18f,.22f,0},{0,0,0,1},{.21f,.45f,.25f}},
        {0,{.18f,.22f,0},{0,0,0,1},{.21f,.45f,.25f}},
        {0,{-.4f,.72f,0},{0,0,0,1},{.18f,.5f,.22f}},
        {0,{.4f,.72f,0},{0,0,0,1},{.18f,.5f,.22f}}};
    nodes[1].position[1]+=breathing.value[0];
    if(pose_grounded!=g->grounded) {
        ol_spring_retarget(&jump_pose,time_ns,g->grounded?0:1);
        pose_grounded=g->grounded;
    }
    float jump_mix=reduced?0:ol_spring_sample(&jump_pose,time_ns,0,0);
    for(int i=3;i<7;i++) {
        if(moving&&g->grounded&&!reduced)memcpy(nodes[i].rotation,stepping.value,4*sizeof(float));
        if(i&1)nodes[i].rotation[0]=-nodes[i].rotation[0];
        float pose[4]={i<5?.28f:-.5f,0,0,i<5?.96f:.8660254f};
        ol_quat_mix(nodes[i].rotation,nodes[i].rotation,pose,jump_mix);
    }
    float world[7][16];if(ol3d_scene_matrices(nodes,7,world))error=1;
    for(int i=1;i<7&&!error;i++)error=box_matrix(world[i],i==2?0xffd5a4:i<3?0x758dff:0xdceaff,0,g->hit);
    int end=ol3d_end(renderer,scene);if(error||end)return 0;
    ol_window_clear(0x101c30);
    int rw=window_w,rh=rw*9/16;
    if(rh>window_h-100){rh=window_h-100;rw=rh*16/9;}
    if(rw<1||rh<1)return 0;
    if(ol_window_composite(scene,(window_w-rw)/2,70,rw,rh))return 0;
    label(22,14,22,0xeaf4ff,"SKY ISLANDS");
    char status[128];snprintf(status,sizeof status,"Crystals  %d / 3     %d x %d    P: quality",g->score,width,height);
    label(22,43,13,0x91accb,status);
    label(20,window_h-23,12,0xb6cbe5,"WASD move   Arrows camera   Space jump   Esc pause   R restart");
    for(int i=0;i<3;i++)ol_window_rrect(window_w-98+i*25,23,15,20,4,g->collected[i]?0xffd064:0x334968);
    if(g->paused||g->won) {
        int x=(window_w-360)/2,y=(window_h-154)/2;
        ol_window_glass(x,y,360,154,16,14,25,45,205);
        label(x+25,y+26,25,0xffffff,g->won?"ISLANDS COMPLETE":"PAUSED");
        label(x+25,y+74,15,0xbcd0e9,g->won?"All three crystals collected.":"Take a breath. Your progress is saved.");
        label(x+25,y+112,14,0xffd064,g->won?"R  Play again":"Esc  Resume     R  Restart");
    }
    if(ol_window_present())return 0;
    render_ns[frames%120]=monotonic_ns()-start;frames++;
    if(trace_enabled){example_log("ISLAND frame=%llu\n",(unsigned long long)frames);}
    if(frames%120==0) {
        uint64_t sorted[120];memcpy(sorted,render_ns,sizeof sorted);
        for(int i=1;i<120;i++){uint64_t x=sorted[i];int j=i;while(j&&sorted[j-1]>x){sorted[j]=sorted[j-1];j--;}sorted[j]=x;}
        example_log("ISLAND submitted=%llu render_us_p50=%llu render_us_p95=%llu x=%.2f y=%.2f z=%.2f score=%d\n",
            (unsigned long long)frames,(unsigned long long)(sorted[59]/1000),(unsigned long long)(sorted[113]/1000),g->x,g->y,g->z,g->score);
        
    }
    return 1;
}
int main(int argc,char **argv)
{
    if(!init()){fprintf(stderr,"OpenLogit initialization failed\n");return 1;}
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--low"))target(1);
        if(!strcmp(argv[i],"--trace"))trace_enabled=1;
    }
    gui_create("Sky Islands",window_w,window_h);
    _sys(SYS_GUI_WIN_MIN,(480L<<16)|360,0,0);
    struct island_game game;island_reset(&game);
    uint64_t previous=monotonic_ns(),simulation=0;double accumulator=0;int last_score=0,last_deaths=0;uint64_t trace_at=0;
    int dirty=1,jump_request=0,low=width==320,reduced=setting_int("ui.reduce_motion",0)!=0;
    example_log("ISLAND READY api=1.1 backend=software shaders=custom source=SDK\n");
    for(;;) {
        struct logit_event event;
        while(poll_event(&event)) {
            if(event.type==EV_CLOSE)goto done;
            if(event.type==EV_RESIZE){window_w=event.a;window_h=event.b;dirty=1;}
            if(event.type==EV_WINDOW_FOCUS||event.type==EV_THEME){dirty=1;reduced=setting_int("ui.reduce_motion",0)!=0;}
            if(event.type==EV_KEY) {
                if(event.a==27&&!game.won){game.paused=!game.paused;dirty=1;example_log("ISLAND pause=%d\n",game.paused);}
                if(event.a=='r'||event.a=='R'){island_reset(&game);simulation=0;ol_spring_init(&jump_pose,0,0,0,5,1,0,300000000);pose_grounded=1;accumulator=0;last_score=last_deaths=0;dirty=1;example_log("ISLAND restart\n");}
                if(event.a=='p'||event.a=='P'){if(target(!low)){low=!low;dirty=1;example_log("ISLAND quality=%dx%d\n",width,height);}}
                if(event.a==' ')jump_request=1;
            }
        }
        uint64_t now=monotonic_ns();double dt=(now-previous)*1e-9;previous=now;if(dt>.25)dt=.25;
        int visible=ol_window_visible();
        if(!visible){jump_request=0;accumulator=0;wait_idle(0);previous=monotonic_ns();continue;}
        int xaxis=ol_window_key_down('d')-ol_window_key_down('a');
        int zaxis=ol_window_key_down('s')-ol_window_key_down('w');
        if(!game.paused&&!game.won) {
            game.yaw+=(ol_window_key_down(KEY_RIGHT)-ol_window_key_down(KEY_LEFT))*dt*1.6f;
            game.pitch+=(ol_window_key_down(KEY_UP)-ol_window_key_down(KEY_DOWN))*dt*.8f;
            if(game.pitch<.1f)game.pitch=.1f;if(game.pitch>1.1f)game.pitch=1.1f;
            accumulator+=dt;
            while(accumulator>=1.0/120) {
                simulation+=8333333;struct ol_anim_sample motion;
                /* A moving platform is gameplay, so reduced motion affects
                 * decoration/camera only; it must not change level geometry. */
                ol_animation_sample(&sway,simulation,0,&motion);
                island_step(&game,1.f/120,xaxis,zaxis,jump_request,motion.value[0]);
                jump_request=0;accumulator-=1.0/120;
            }
            dirty=1;
        } else {accumulator=0;jump_request=0;}
        if(game.score!=last_score){last_score=game.score;example_log("ISLAND collected=%d\n",game.score);if(game.won)example_log("ISLAND VICTORY\n");}
        if(game.deaths!=last_deaths){last_deaths=game.deaths;example_log("ISLAND fall_reset=%d\n",game.deaths);}
        if(trace_enabled&&(xaxis||zaxis||!game.grounded||now-trace_at>=1000000000ull)) {
            trace_at=now;
            example_log("ISLAND state x=%.3f y=%.3f z=%.3f grounded=%d score=%d deaths=%d\n",
                   game.x,game.y,game.z,game.grounded,game.score,game.deaths);
        }
        if(dirty){if(!render(&game,xaxis||zaxis,simulation,reduced)){fprintf(stderr,"OpenLogit frame failed; last scene preserved\n");goto done;}dirty=0;}
        wait_idle(game.paused||game.won?0:16);
    }
done:
    ol3d_pipeline_destroy(pipeline);ol3d_buffer_destroy(vertices_buffer);ol3d_buffer_destroy(index_buffer);
    ol3d_destroy(renderer);ol_surface_destroy(scene);ol_device_destroy(device);
    free(surface_mem);free(device_mem);free(front);free(work);return 0;
}
