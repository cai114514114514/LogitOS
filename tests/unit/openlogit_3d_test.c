#include "openlogit_3d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c, n)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            printf("FAIL %s\n", n);                                                                \
            failed++;                                                                              \
        } else                                                                                     \
            printf("PASS %s\n", n);                                                                \
    } while (0)
static const char *vs = "ols 1 vertex\ninput r0 0\nposition r0\ninput r1 1\nvarying 0 r1\n";
static const char *ps = "ols 1 pixel\ninput r0 0\ncolor r0\n";
static struct ol3d_vertex vertex(float x, float y, float z, float w, float r, float g, float b,
                                 float a)
{
    struct ol3d_vertex v = {0};
    float p[4] = {x, y, z, w}, c[4] = {r, g, b, a};
    memcpy(v.attribute[0], p, sizeof p);
    memcpy(v.attribute[1], c, sizeof c);
    return v;
}
int main(void)
{
    int checks = 0, failed = 0;
    struct ol_shader_program vert = {0}, pixel = {0}, bad = {0};
    struct ol_shader_error e;
    CHECK(!ol_shader_compile(vs, strlen(vs), &vert, &e) &&
              !ol_shader_compile(ps, strlen(ps), &pixel, &e),
          "custom vertex and pixel programs compile");
    const char *invalid = "ols 1 pixel\nadd r0 r1 r2\ncolor r0\n";
    CHECK(ol_shader_compile(invalid, strlen(invalid), &bad, &e) != OL_OK &&
              strstr(e.message, "undefined"),
          "undefined shader registers rejected");
    bad = pixel;
    bad.code[0].dst = 99;
    CHECK(ol_shader_validate(&bad, &e) != OL_OK, "edited bytecode validates again");
    invalid = "ols 1 pixel\nloop r0\n";
    CHECK(ol_shader_compile(invalid, strlen(invalid), &bad, &e) != OL_OK,
          "unsupported control flow rejected");
    struct ol3d_context *c = 0;
    CHECK(!ol3d_create(32, 32, &c), "software target created");
    struct ol3d_caps caps;
    ol3d_caps(&caps);
    CHECK((caps.features & OL_CAP_3D) && !(caps.features & OL_CAP_GPU),
          "software 3D capability is distinct from GPU");
    unsigned char image[32 * 32 * 4], previous[sizeof image];
    uint32_t ix[] = {0, 1, 2, 0, 2, 3};
    struct ol3d_vertex v[4] = {vertex(-1, -1, .5, 1, 1, 0, 0, .5),
                               vertex(1, -1, .5, 1, 1, 0, 0, .5), vertex(1, 1, .5, 1, 1, 0, 0, .5),
                               vertex(-1, 1, .5, 1, 1, 0, 0, .5)};
    struct ol3d_draw d = {.pipeline = {&vert, &pixel, 1, 1, 0, 0, 1},
                          .vertices = v,
                          .vertex_count = 4,
                          .indices = ix,
                          .index_count = 6};
    ol3d_begin(c, 0);
    CHECK(!ol3d_draw_indexed(c, &d) && !ol3d_end(c, 0), "indexed quad executes custom shaders");
    ol3d_readback(c, image, sizeof image, 128);
    int once = 1;
    for (int i = 0; i < 32 * 32; i++)
        if (image[4 * i] != 128 || image[4 * i + 1] || image[4 * i + 2])
            once = 0;
    CHECK(once, "shared edge covers every pixel exactly once");
    d.pipeline.blend = 0;
    d.pipeline.depth_test = 1;
    d.pipeline.depth_write = 1;
    ol3d_begin(c, 0);
    for (int i = 0; i < 4; i++) {
        v[i].attribute[0][2] = .25;
        v[i].attribute[1][3] = 1;
    }
    ol3d_draw_indexed(c, &d);
    for (int i = 0; i < 4; i++) {
        v[i].attribute[0][2] = .75;
        v[i].attribute[1][0] = 0;
        v[i].attribute[1][2] = 1;
    }
    ol3d_draw_indexed(c, &d);
    ol3d_end(c, 0);
    ol3d_readback(c, image, sizeof image, 128);
    CHECK(image[16 * 128 + 16 * 4] == 255 && image[16 * 128 + 16 * 4 + 2] == 0,
          "depth keeps near surface over later far draw");
    d.index_count = 3;
    d.pipeline.depth_test = d.pipeline.depth_write = 0;
    v[0] = vertex(-1, -1, -.5, 1, 0, 1, 0, 1);
    v[1] = vertex(1, -1, .5, 1, 0, 1, 0, 1);
    v[2] = vertex(0, 1, .5, 1, 0, 1, 0, 1);
    ol3d_begin(c, 0);
    ol3d_draw_indexed(c, &d);
    ol3d_end(c, 0);
    ol3d_readback(c, image, sizeof image, 128);
    CHECK(image[2 * 128 + 16 * 4 + 1] > 200 && image[29 * 128 + 2 * 4 + 1] == 0,
          "near plane clips geometry before divide");
    v[0] = vertex(-1, -1, .5, 1, 0, 0, 0, 1);
    v[1] = vertex(2, -2, 1, 2, 1, 0, 0, 1);
    v[2] = vertex(0, 1, .5, 1, 0, 0, 0, 1);
    ol3d_begin(c, 0);
    ol3d_draw_indexed(c, &d);
    ol3d_end(c, 0);
    ol3d_readback(c, image, sizeof image, 128);
    /* At screen pixel (16,16), screen barycentrics are 17/64,19/64,28/64.
     * Red belongs to vertex 1 with w=2: (b/2)/(a+b/2+c). */
    double sx = (16.5 / 32) * 2 - 1, sy = 1 - (16.5 / 32) * 2;
    double bc = (sy + 1) / 2, bb = (1 - bc + sx) / 2, ba = 1 - bb - bc;
    int expected = (int)((bb * .5) / (ba + bb * .5 + bc) * 255 + .5);
    CHECK(abs((int)image[16 * 128 + 16 * 4] - expected) <= 1,
          "varyings use perspective correct interpolation");
    memcpy(previous, image, sizeof image);
    ol3d_begin(c, 0xffffff);
    ix[1] = 55;
    CHECK(ol3d_draw_indexed(c, &d) == OL_ARGUMENT && ol3d_end(c, 0) == OL_ARGUMENT,
          "invalid index aborts frame");
    ol3d_readback(c, image, sizeof image, 128);
    CHECK(!memcmp(image, previous, sizeof image), "failed frame preserves published readback");
    ix[1] = 1;
    float matrices[2][16];
    struct ol3d_node nodes[2] = {
        {.parent = -1, .position = {2, 0, 0}, .rotation = {0, 0, 0, 1}, .scale = {1, 1, 1}},
        {.parent = 0, .position = {0, 3, 0}, .rotation = {0, 0, 0, 1}, .scale = {1, 1, 1}}};
    CHECK(!ol3d_scene_matrices(nodes, 2, matrices) && matrices[1][12] == 2 && matrices[1][13] == 3,
          "scene hierarchy composes transforms");
    nodes[0].parent = 1;
    CHECK(ol3d_scene_matrices(nodes, 2, matrices) == OL_ARGUMENT,
          "cyclic or forward parents rejected");
    float p[3] = {1, 1, 1}, weights[4] = {.25, .75, 0, 0}, skinned[3];
    unsigned joints[4] = {0, 1, 0, 0};
    CHECK(!ol3d_skin_vertex(skinned, p, joints, weights, matrices, 2) &&
              fabs(skinned[0] - 3) < 1e-6 && fabs(skinned[1] - 3.25) < 1e-6,
          "four weight skinning uses joint transforms");
    float projection[16];
    CHECK(ol_mat4_perspective(projection, 1, 1, 1, 1) == OL_ARGUMENT,
          "degenerate projection rejected");
    struct ol3d_stats stats;
    ol3d_stats(c, &stats);
    CHECK(stats.frames == 4 && stats.shaded_pixels > 0,
          "statistics describe committed rendered frames");
    /* Actual owned resources; source edits cannot mutate a created pipeline. */
    struct ol3d_buffer *vb=0,*ib=0;struct ol3d_pipeline_object *pipeline=0;
    CHECK(!ol3d_buffer_create(OL3D_VERTEX_BUFFER,v,4,&vb)&&
          !ol3d_buffer_create(OL3D_INDEX_BUFFER,ix,6,&ib)&&
          !ol3d_pipeline_create(&d.pipeline,&pipeline,&e),"owned vertex index and pipeline resources created");
    struct ol3d_buffer_view vv=ol3d_buffer_view(vb),iv=ol3d_buffer_view(ib);
    ol3d_begin(c,0);ol3d_buffer_update(vb,0,1,v);
    CHECK(ol3d_draw_buffers(c,pipeline,&d.bindings,vv,iv,0,3)==OL_STALE_RESOURCE&&
          ol3d_end(c,0)==OL_STALE_RESOURCE,"stale buffer view aborts the whole unpublished frame");
    ol3d_begin(c,0);
    CHECK(!ol3d_draw_buffers(c,pipeline,&d.bindings,ol3d_buffer_view(vb),iv,0,3)&&!ol3d_end(c,0),
          "refreshed buffer views render normally");
    ol3d_pipeline_destroy(pipeline);ol3d_buffer_destroy(vb);ol3d_buffer_destroy(ib);
    struct ol_shader_program texture_program;
    const char *texture_source="ols 1 pixel\ninput r0 0\ntex2d r1 r0 0\ncolor r1\n";
    CHECK(!ol_shader_compile(texture_source,strlen(texture_source),&texture_program,&e),"texture shader compiles");
    unsigned char texels[]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
    struct ol3d_texture texture={texels,2,2,8,sizeof texels,1,0};
    d.pipeline.pixel=&texture_program;d.bindings=(struct ol3d_bindings){.textures=&texture,.texture_count=1};
    for(int i=0;i<4;i++)v[i].attribute[1][0]=v[i].attribute[1][1]=.5f;
    ol3d_begin(c,0);ol3d_draw_indexed(c,&d);ol3d_end(c,0);ol3d_readback(c,image,sizeof image,128);
    int mid=16*128+16*4;
    CHECK(abs(image[mid]-128)<=1&&abs(image[mid+1]-128)<=1&&abs(image[mid+2]-128)<=1,
          "bilinear texture samples independent four-texel average");
    d.bindings.texture_count=0;
    CHECK(ol3d_pipeline_validate(&d.pipeline,&d.bindings,&e)==OL_ARGUMENT&&strstr(e.message,"texture"),
          "missing texture binding diagnosed before executing a shader");
    d.bindings=(struct ol3d_bindings){0};d.pipeline.pixel=&pixel;
    ol3d_readback(c,previous,sizeof previous,128);ol3d_begin(c,0xffffff);
    CHECK(!ol3d_draw_indexed(c,&d),"successful draw precedes a late shader failure");
    const char *zero_divide="ols 1 pixel\nconst r0 0 0 0 0\nrcp r1 r0\ncolor r1\n";
    ol_shader_compile(zero_divide,strlen(zero_divide),&bad,&e);d.pipeline.pixel=&bad;
    CHECK(ol3d_draw_indexed(c,&d)==OL_RENDER_FAILED&&ol3d_end(c,0)==OL_RENDER_FAILED,
          "runtime shader arithmetic failure aborts frame");
    ol3d_readback(c,image,sizeof image,128);
    CHECK(!memcmp(image,previous,sizeof image),"late shader failure publishes none of the earlier draws");
    float model[16],normal[16];ol_mat4_identity(model);model[4]=2;
    CHECK(!ol_mat4_normal(normal,model)&&normal[0]==1&&normal[1]==-2&&normal[4]==0,
          "inverse transpose keeps normals perpendicular under shear");
    ol3d_destroy(c);
    printf("OpenLogit 3D: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
