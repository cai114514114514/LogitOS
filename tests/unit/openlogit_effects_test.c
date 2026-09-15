#include "openlogit.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int checks,failed;
#define CHECK(c,s) do{checks++;if(!(c)){failed++;printf("FAIL %s\n",s);}else printf("PASS %s\n",s);}while(0)
static void *allocate(unsigned long size){void *p=calloc(1,size);if(!p)exit(2);return p;}
static void fill(struct ol_list *l,int x,int y,int w,int h,unsigned rgb)
{
    int points[16],subs[2];struct gfx_path p;gfx_path_init(&p,points,8,subs,2);
    gfx_path_rect(&p,GFX_PX(x),GFX_PX(y),GFX_PX(w),GFX_PX(h));
    struct gfx_paint paint;gfx_paint_solid(&paint,rgb,255);
    ol_cmd_fill(l,&p,GFX_NONZERO,&paint,0,8);
}
int main(void)
{
    void *dm=allocate(ol_device_size()),*sm=allocate(ol_surface_size()),*tm=allocate(ol_surface_size());
    void *lm=allocate(65536);struct ol_device *d=0;struct ol_surface *source=0,*target=0;struct ol_list *l=0;
    unsigned char src[3*3*4]={0},sw[sizeof src],front[9*9*4]={0},work[sizeof front],old[sizeof front];
    struct ol_surface_desc sd={sizeof sd,OL_FORMAT_RGBA8_STRAIGHT,3,3,12,src,sw,sizeof src,sizeof sw};
    struct ol_surface_desc td={sizeof td,OL_FORMAT_RGBA8_STRAIGHT,9,9,36,front,work,sizeof front,sizeof work};
    CHECK(!ol_device_create(dm,ol_device_size(),OL_API_VERSION,OL_CAP_LAYER|OL_CAP_BLUR|OL_CAP_BACKDROP,&d)&&
          !ol_surface_create(d,sm,ol_surface_size(),&sd,&source)&&!ol_surface_create(d,tm,ol_surface_size(),&td,&target)&&
          !ol_list_create(d,lm,65536,&l),"native effects capabilities create a usable device");
    ol_cmd_clear(l,0,0);fill(l,0,0,2,3,0xff0000);fill(l,1,0,2,3,0xff0000);ol_list_close(l);
    CHECK(!ol_submit(d,l,source,0),"overlapping children render into an offscreen group");
    ol_list_reset(l);struct ol_layer_options o={.size=sizeof o,.opacity=128};
    CHECK(!ol_cmd_layer(l,source,(struct gfx_rect){3,3,3,3},&o)&&ol_surface_destroy(source)==OL_BUSY,
          "native layer retains its offscreen source");ol_list_close(l);
    CHECK(!ol_submit(d,l,target,0)&&front[4*36+4*4]==255&&front[4*36+4*4+3]==128&&front[4*36+3*4+3]==128,
          "group opacity applies once to overlapping children");
    ol_list_reset(l);unsigned char upload[sizeof src]={0};upload[4*4]=255;upload[4*4+3]=255;
    /* Hidden blue in fully transparent texels must contribute no blue halo. */
    for(int i=0;i<9;i++)if(i!=4)upload[i*4+2]=255;
    ol_surface_upload(source,upload,sizeof upload,12);memset(front,0,sizeof front);
    o.opacity=255;o.blur_radius=1;ol_cmd_layer(l,source,(struct gfx_rect){3,3,3,3},&o);ol_list_close(l);
    unsigned long bytes=0;CHECK(!ol_submit_workspace_size(l,target,&bytes)&&bytes>0&&bytes<=4096,
          "blur queries bounded row workspace rather than a full image");
    void *scratch=allocate(bytes+4096);memcpy(old,front,sizeof old);uint64_t serial=ol_device_completed(d);
    CHECK(ol_submit(d,l,target,0)==OL_LIMIT&&ol_submit_workspace(d,l,target,scratch,bytes-1,0)==OL_LIMIT&&
          !memcmp(front,old,sizeof front)&&ol_device_completed(d)==serial,"missing effect scratch preserves the previous frame");
    CHECK(ol_submit_workspace(d,l,target,work,bytes,0)==OL_ARGUMENT,
          "effect workspace cannot alias target storage");
    struct ol_submit_info info={.size=sizeof info};
    CHECK(!ol_submit_workspace(d,l,target,scratch,bytes,&info),"native blur submits transactionally");
    int correct=1;
    /* Independent direct 3x3 convolution of a one-pixel red impulse: 255/9
     * rounded to 28, full red chroma, zero hidden blue, zero outside support. */
    for(int y=0;y<9;y++)for(int x=0;x<9;x++) {
        unsigned char *p=front+y*36+x*4;int inside=x>=3&&x<=5&&y>=3&&y<=5;
        if(p[3]!=(inside?28:0)||(inside&&(p[0]!=255||p[2]!=0)))correct=0;
    }
    CHECK(correct,"premultiplied blur matches independent convolution without dark or hidden-color fringes");
    CHECK(info.damage.x==2&&info.damage.y==2&&info.damage.w==5&&info.damage.h==5,
          "blur damage includes expanded layer support");
    ol_list_reset(l);memset(front,0,sizeof front);o.blur_radius=0;o.shadow_radius=1;o.shadow_opacity=255;
    o.shadow_dx=2;o.shadow_dy=1;o.shadow_rgb=0x00ff00;
    struct gfx_rect clip={5,3,2,4};o.clip=&clip;
    ol_cmd_layer(l,source,(struct gfx_rect){3,3,3,3},&o);clip=(struct gfx_rect){0,0,0,0};ol_list_close(l);
    ol_submit_workspace_size(l,target,&bytes);
    CHECK(!ol_submit_workspace(d,l,target,scratch,bytes,&info)&&front[5*36+6*4+1]==255&&front[5*36+6*4+3]==28&&front[5*36+7*4+3]==0,
          "shadow uses source coverage and recorded clip after offset and blur");
    CHECK(info.damage.x==5&&info.damage.y==3&&info.damage.w==2&&info.damage.h==4,
          "layer damage is clipped after effect expansion");
    memcpy(old,front,sizeof front);serial=ol_device_completed(d);
    ol_surface_upload(source,upload,sizeof upload,12);
    CHECK(ol_submit_workspace(d,l,target,scratch,bytes,0)==OL_STALE_RESOURCE&&
          !memcmp(front,old,sizeof front)&&serial==ol_device_completed(d),"changed layer version preserves the entire published frame");
    ol_list_reset(l);memset(front,0,sizeof front);
    for(int i=0;i<81;i++)front[i*4+3]=255;front[4*36+4*4]=255;
    memset(work,0xcc,sizeof work);memcpy(old,front,sizeof front);
    ol_cmd_backdrop(l,(struct gfx_rect){3,3,3,3},1,0,0);ol_list_close(l);ol_submit_workspace_size(l,target,&bytes);
    CHECK(!ol_submit_workspace(d,l,target,scratch,bytes,&info),"backdrop snapshots accumulated RGBA before filtering");
    correct=1;
    for(int y=0;y<9;y++)for(int x=0;x<9;x++) {
        unsigned char *p=front+y*36+x*4;int inside=x>=3&&x<=5&&y>=3&&y<=5;
        if(inside?(p[0]!=28||p[1]!=0||p[2]!=0||p[3]!=255):memcmp(p,old+y*36+x*4,4)!=0)correct=0;
    }
    CHECK(correct,"backdrop convolution has no in-place feedback and preserves outside pixels");
    CHECK(info.damage.x==3&&info.damage.w==3&&info.copied_bytes==(25+9)*4,
          "backdrop initializes its read halo while committing only output damage");
    ol_list_reset(l);ol_cmd_clear(l,0x0000ff,255);ol_cmd_backdrop(l,(struct gfx_rect){2,2,5,5},1,0xff0000,128);ol_list_close(l);
    ol_submit_workspace_size(l,target,&bytes);
    CHECK(!ol_submit_workspace(d,l,target,scratch,bytes,0)&&front[4*36+4*4]==128&&front[4*36+4*4+2]==127,
          "glass uses earlier commands in the same frame then applies its tint");
    ol_list_reset(l);memcpy(old,front,sizeof front);serial=ol_device_completed(d);
    ol_cmd_backdrop(l,(struct gfx_rect){0,0,9,9},1,0xffcc00,255);
    int *dense=allocate(600*5*2*sizeof(int)),*contours=allocate(600*sizeof(int));
    struct gfx_path many;gfx_path_init(&many,dense,600*5,contours,600);
    for(int i=0;i<600;i++)gfx_path_rect(&many,GFX_PX(1),0,GFX_PX(5),GFX_PX(8));
    struct gfx_paint paint;gfx_paint_solid(&paint,0x00ff00,255);
    CHECK(!ol_cmd_fill(l,&many,GFX_NONZERO,&paint,0,4)&&!ol_list_close(l),
          "late failure fixture records real geometry after an effect");
    ol_submit_workspace_size(l,target,&bytes);
    CHECK(ol_submit_workspace(d,l,target,scratch,bytes,0)==OL_RENDER_FAILED&&
          !memcmp(front,old,sizeof front)&&serial==ol_device_completed(d),
          "later raster failure cannot publish an earlier successful effect");
    free(dense);free(contours);ol_list_reset(l);
    for(int i=0;i<9;i++){upload[i*4]=255;upload[i*4+1]=upload[i*4+2]=0;upload[i*4+3]=255;}
    ol_surface_upload(source,upload,sizeof upload,12);memset(front,0,sizeof front);
    o=(struct ol_layer_options){.size=sizeof o,.opacity=255,.blur_radius=OL_MAX_BLUR_RADIUS};
    ol_cmd_layer(l,source,(struct gfx_rect){-60,-60,129,129},&o);ol_list_close(l);
    ol_submit_workspace_size(l,target,&bytes);
    CHECK(!ol_submit_workspace(d,l,target,scratch,bytes,0)&&front[4*36+4*4]==255&&front[4*36+4*4+3]==255,
          "maximum blur radius preserves saturated opaque color without sum overflow");
    ol_list_reset(l);o.clip=0;o.blur_radius=OL_MAX_BLUR_RADIUS+1;
    CHECK(ol_cmd_layer(l,source,(struct gfx_rect){0,0,3,3},&o)==OL_ARGUMENT&&ol_list_close(l)==OL_ARGUMENT,
          "out-of-range effect parameters latch the command list error");
    ol_list_destroy(l);ol_surface_destroy(source);ol_surface_destroy(target);ol_device_destroy(d);
    free(scratch);free(dm);free(sm);free(tm);free(lm);
    printf("OpenLogit effects: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
