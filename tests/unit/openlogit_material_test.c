#include "openlogit_material.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks, failed;
#define CHECK(c,n) do { checks++; if (!(c)) { failed++; printf("FAIL %s\n",n); } else printf("PASS %s\n",n); } while (0)
static struct ol_material *compile(const char *text)
{
    struct ol_shader_program program;
    struct ol_shader_error error;
    struct ol_material *m = 0;
    if (ol_shader_compile(text,strlen(text),&program,&error) || ol_material_create(&program,&m,&error)) {
        fprintf(stderr,"compile: %s\n",error.message); exit(2);
    }
    return m;
}
int main(void)
{
    struct ol_shader_error error;
    struct ol_material_caps caps = {.size=sizeof caps};
    CHECK(!ol_material_query_caps(&caps) && caps.inputs==2 &&
          (caps.features&OL_CAP_PIXEL_MATERIAL) && !(caps.features&OL_CAP_GPU), "material capabilities report software pixel pass");
    caps.size=0;
    CHECK(ol_material_query_caps(&caps)==OL_ARGUMENT,"short capability descriptor rejected");
    void *dm=malloc(ol_device_size()), *sm=malloc(ol_surface_size()), *lm=malloc(4096);
    struct ol_device *dev=0; struct ol_surface *surface=0; struct ol_list *list=0;
    unsigned char front[80],work[80],before[80]; memset(front,77,sizeof front);
    struct ol_surface_desc desc={sizeof desc,OL_FORMAT_RGBA8_STRAIGHT,4,4,20,front,work,80,80};
    CHECK(!ol_device_create(dm,ol_device_size(),OL_API_VERSION,0,&dev) &&
          !ol_surface_create(dev,sm,ol_surface_size(),&desc,&surface) &&
          !ol_list_create(dev,lm,4096,&list),"padded native target created");
    if(failed)return 2;
    struct ol_caps core={.size=sizeof core}; ol_device_caps(dev,&core);
    CHECK(!(core.features&(OL_CAP_PIXEL_MATERIAL|OL_CAP_3D|OL_CAP_GPU)),"kernel-capable core does not claim userspace pipeline support");
    struct ol_material_context *ctx=0;
    CHECK(ol_material_context_create(0,4,&ctx)==OL_ARGUMENT && !ctx &&
          ol_material_context_create(4097,4,&ctx)==OL_ARGUMENT && !ctx,"context dimensions bounded");
    CHECK(!ol_material_context_create(4,4,&ctx),"reusable material context created");
    struct ol_material *uv=compile("ols 1 pixel\ninput r0 0\ncolor r0\n");
    CHECK(!ol_material_render(ctx,uv,0,surface,&error),"pixel-center material executes");
    int match=1;
    const unsigned char centers[]={32,96,159,223};
    for(int y=0;y<4;y++) for(int x=0;x<4;x++)
        if(front[y*20+x*4]!=centers[x] || front[y*20+x*4+1]!=centers[y] || front[y*20+x*4+2]!=0 || front[y*20+x*4+3]!=255) match=0;
    CHECK(match,"material UV centers and top-left orientation match independent oracle");
    CHECK(front[16]==77 && front[36]==77 && front[76]==77,"material publication preserves target row padding");
    struct ol_material *coords=compile("ols 1 pixel\ninput r0 1\nconst r1 .25 .25 .125 .125\nmul r2 r0 r1\ncolor r2\n");
    CHECK(!ol_material_render(ctx,coords,0,surface,&error) && front[0]==32 && front[1]==32 && front[2]==128 && front[3]==128 && front[72]==223,
          "pixel coordinates expose target extent independently of normalized UV");
    struct ol_shader_program p; struct ol_material *bad=0;
    const char *src="ols 1 vertex\ninput r0 0\nposition r0\n";
    ol_shader_compile(src,strlen(src),&p,&error);
    CHECK(ol_material_create(&p,&bad,&error)==OL_ARGUMENT && !bad && strstr(error.message,"pixel stage"),"vertex program rejected as material");
    src="ols 1 pixel\ninput r0 2\ncolor r0\n"; ol_shader_compile(src,strlen(src),&p,&error);
    CHECK(ol_material_create(&p,&bad,&error)==OL_ARGUMENT && error.line==1,"unavailable material input rejected at creation");
    src="ols 1 pixel\nconst r0 1 .25 -.5 2\ncolor r0\n"; ol_shader_compile(src,strlen(src),&p,&error);
    struct ol_material *immutable=0; ol_material_create(&p,&immutable,&error); p.code[0].literal[0]=0;
    CHECK(!ol_material_render(ctx,immutable,0,surface,&error) && front[0]==255 && front[1]==64 && front[2]==0 && front[3]==255,"immutable program snapshot and RGBA clamping");
    struct ol_material *uniform=compile("ols 1 pixel\nuniform r0 0\ncolor r0\n");
    struct ol_surface_view v; ol_surface_view(surface,&v); uint64_t version=v.generation;
    memcpy(before,front,80);
    CHECK(ol_material_render(ctx,uniform,0,surface,&error)==OL_ARGUMENT && error.line==1 && strstr(error.message,"uniform"),"missing uniform has instruction diagnostic");
    float values[4][4]={{.2f,.4f,.6f,.8f}};
    struct ol3d_bindings bindings={.uniforms=values,.uniform_count=1}; values[0][0]=NAN;
    CHECK(ol_material_render(ctx,uniform,&bindings,surface,&error)==OL_ARGUMENT && strstr(error.message,"nonfinite"),"nonfinite uniform rejected before execution");
    ol_surface_view(surface,&v);
    CHECK(!memcmp(before,front,80) && v.generation==version,"binding failure preserves target bytes and generation");
    values[0][0]=.2f;
    CHECK(!ol_material_render(ctx,uniform,&bindings,surface,&error) && front[0]==51 && front[1]==102 && front[2]==153 && front[3]==204,"uniform color and straight alpha preserved");
    struct ol_material *tex=compile("ols 1 pixel\ninput r0 0\ntex2d r1 r0 0\ncolor r1\n");
    CHECK(ol_material_render(ctx,tex,0,surface,&error)==OL_ARGUMENT && error.line==2,"missing texture rejected before pixel pass");
    unsigned char pixels[]={255,0,0,255, 0,255,0,255, 88,88,88,88, 0,0,255,255, 255,255,255,255};
    struct ol3d_texture texture={pixels,2,2,12,sizeof pixels,0,0};
    bindings=(struct ol3d_bindings){.textures=&texture,.texture_count=1};
    CHECK(!ol_material_render(ctx,tex,&bindings,surface,&error) && front[0]==255 && front[8]==0 && front[9]==255 && front[40+2]==255 && front[48]==255,"padded texture uses same nearest sampler as mesh shaders");
    texture.bytes--;
    CHECK(ol_material_render(ctx,tex,&bindings,surface,&error)==OL_ARGUMENT && strstr(error.message,"storage"),"truncated texture storage rejected"); texture.bytes++;
    texture.bilinear=1;
    CHECK(!ol_material_render(ctx,tex,&bindings,surface,&error) && front[24]==159 && front[25]==64 && front[26]==64,"bilinear texture matches independent weighted texels");
    /* Feedback reads the previous whole frame, not newly shaded neighbors. */
    ol_surface_view(surface,&v); memcpy(before,front,80);
    texture=(struct ol3d_texture){v.pixels,4,4,20,sizeof front,0,0};
    CHECK(!ol_material_render(ctx,tex,&bindings,surface,&error) && !memcmp(before,front,80),"sampling the output as texture observes the complete previous frame");
    struct ol_material *late=compile("ols 1 pixel\ninput r0 1\nswizzle r1 r0 0\nconst r2 3.5 3.5 3.5 3.5\nsub r3 r1 r2\nrcp r4 r3\ncolor r4\n");
    ol_surface_view(surface,&v);version=v.generation;memcpy(before,front,80);
    CHECK(ol_material_render(ctx,late,0,surface,&error)==OL_RENDER_FAILED && strstr(error.message,"(3,0)"),"late execution failure reports the failing pixel");
    ol_surface_view(surface,&v);
    CHECK(!memcmp(before,front,80) && v.generation==version,"late shader failure publishes neither partial pixels nor generation");
    struct ol_material_context *wrong=0;ol_material_context_create(3,4,&wrong);
    CHECK(ol_material_render(wrong,uv,0,surface,&error)==OL_ARGUMENT && !memcmp(before,front,80),"mismatched target cannot publish");
    ol_material_context_destroy(wrong);
    /* A material publication is a real resource update, so earlier lists must
     * fail their ordinary generation check rather than silently change art. */
    ol_cmd_image(list,surface,(struct gfx_rect){0,0,4,4},255,0);ol_list_close(list);
    ol_material_render(ctx,uv,0,surface,&error);
    void *sm2=malloc(ol_surface_size());unsigned char f2[80],w2[80];struct ol_surface *target;
    desc.front=f2;desc.work=w2;ol_surface_create(dev,sm2,ol_surface_size(),&desc,&target);
    CHECK(ol_submit(dev,list,target,0)==OL_STALE_RESOURCE,"material upload invalidates previously recorded resource versions");
    ol_list_reset(list);ol_cmd_clear(list,0,255);
    values[0][0]=1;values[0][1]=values[0][2]=0;values[0][3]=.5f;
    bindings=(struct ol3d_bindings){.uniforms=values,.uniform_count=1};ol_material_render(ctx,uniform,&bindings,surface,&error);
    ol_cmd_image(list,surface,(struct gfx_rect){0,0,4,4},255,0);ol_list_close(list);
    CHECK(!ol_submit(dev,list,target,0) && f2[0]==128 && f2[1]==0 && f2[3]==255,"programmable material composes with native source-over layer semantics");
    ol_list_destroy(list);ol_surface_destroy(target);free(sm2);
    ol_material_destroy(uv);ol_material_destroy(coords);ol_material_destroy(immutable);ol_material_destroy(uniform);
    ol_material_destroy(tex);ol_material_destroy(late);ol_material_context_destroy(ctx);
    ol_surface_destroy(surface);ol_device_destroy(dev);free(sm);free(dm);free(lm);
    printf("material: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
