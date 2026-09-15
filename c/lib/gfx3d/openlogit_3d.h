#ifndef OPENLOGIT_3D_H
#define OPENLOGIT_3D_H
#include "openlogit.h"
#include "openlogit_anim.h"
#include <stddef.h>

/* User-space software extension. 3D is real here; GPU acceleration is absent.
 * Matrices are column-major, clip volume -w<=x,y<=w and 0<=z<=w, front faces
 * are counter-clockwise in NDC. The viewport alone flips y for screen pixels.
 * A frame is unpublished until end succeeds, including late shader failures. */
#define OL3D_MAX_ATTRIBUTES 8
#define OL3D_MAX_VARYINGS 8
#define OL3D_MAX_TEXTURES 8
#define OL3D_MAX_UNIFORMS 256
#define OLS_REGISTERS 64
#define OLS_INSTRUCTIONS 256
#define OLS_MAGIC 0x31534c4fu
enum { OLS_VERTEX = 1, OLS_PIXEL = 2 };
enum ols_opcode {
    OLS_CONST,
    OLS_INPUT,
    OLS_UNIFORM,
    OLS_MOV,
    OLS_ADD,
    OLS_SUB,
    OLS_MUL,
    OLS_MAD,
    OLS_DOT3,
    OLS_DOT4,
    OLS_MIN,
    OLS_MAX,
    OLS_RCP,
    OLS_RSQRT,
    OLS_SIN,
    OLS_COS,
    OLS_SELECT,
    OLS_TEX2D,
    OLS_SWIZZLE,
    OLS_MAT4,
    OLS_POSITION,
    OLS_VARYING,
    OLS_COLOR
};
struct ols_instruction {
    unsigned op, dst, a, b, c;
    float literal[4];
};
struct ol_shader_program {
    unsigned magic, version, stage, count;
    struct ols_instruction code[OLS_INSTRUCTIONS];
};
struct ol_shader_error {
    unsigned line;
    char message[96];
};
int ol_shader_compile(const char *text, size_t bytes, struct ol_shader_program *,
                      struct ol_shader_error *);
int ol_shader_validate(const struct ol_shader_program *, struct ol_shader_error *);

struct ol3d_texture {
    const unsigned char *pixels;
    unsigned width, height, stride;
    size_t bytes;
    int bilinear, repeat;
};
struct ol3d_bindings {
    const float (*uniforms)[4];
    unsigned uniform_count;
    const struct ol3d_texture *textures;
    unsigned texture_count;
};
struct ol3d_vertex {
    float attribute[OL3D_MAX_ATTRIBUTES][4];
};
struct ol3d_pipeline {
    const struct ol_shader_program *vertex, *pixel;
    unsigned varying_count;
    int cull_back, depth_test, depth_write, blend;
};
struct ol3d_draw {
    struct ol3d_pipeline pipeline;
    struct ol3d_bindings bindings;
    const struct ol3d_vertex *vertices;
    size_t vertex_count;
    const uint32_t *indices;
    size_t index_count;
};
struct ol3d_context;
struct ol3d_stats {
    uint64_t triangles, clipped_triangles, fragments, shaded_pixels, frames;
};
struct ol3d_caps {
    unsigned version, backend, features, max_texture_size, max_registers, max_instructions;
};
void ol3d_caps(struct ol3d_caps *);
int ol3d_create(unsigned width, unsigned height, struct ol3d_context **);
void ol3d_destroy(struct ol3d_context *);
int ol3d_begin(struct ol3d_context *, unsigned clear_rgb);
int ol3d_draw_indexed(struct ol3d_context *, const struct ol3d_draw *);
int ol3d_end(struct ol3d_context *, struct ol_surface *output);
int ol3d_readback(const struct ol3d_context *, unsigned char *rgba, size_t bytes, unsigned stride);
void ol3d_stats(const struct ol3d_context *, struct ol3d_stats *);
/* Owned immutable pipeline snapshots and versioned CPU buffers. Draw calls
 * execute synchronously: no retained raw pointer survives the call. A view's
 * version must be refreshed after update, so stale frame assembly is explicit. */
enum { OL3D_VERTEX_BUFFER=1, OL3D_INDEX_BUFFER=2 };
struct ol3d_buffer;
struct ol3d_pipeline_object;
struct ol3d_buffer_view { const struct ol3d_buffer *buffer; uint64_t version; };
int ol3d_buffer_create(unsigned kind, const void *data, size_t count, struct ol3d_buffer **);
int ol3d_buffer_update(struct ol3d_buffer *, size_t first, size_t count, const void *data);
struct ol3d_buffer_view ol3d_buffer_view(const struct ol3d_buffer *);
void ol3d_buffer_destroy(struct ol3d_buffer *);
int ol3d_pipeline_create(const struct ol3d_pipeline *, struct ol3d_pipeline_object **,
                         struct ol_shader_error *);
void ol3d_pipeline_destroy(struct ol3d_pipeline_object *);
int ol3d_pipeline_validate(const struct ol3d_pipeline *, const struct ol3d_bindings *,
                           struct ol_shader_error *);
int ol3d_draw_buffers(struct ol3d_context *, const struct ol3d_pipeline_object *,
                       const struct ol3d_bindings *, struct ol3d_buffer_view vertices,
                       struct ol3d_buffer_view indices, size_t first_index, size_t index_count);
/* Abort the current unpublished frame; subsequent end returns the error. */
int ol3d_abort(struct ol3d_context *, int error);

void ol_mat4_identity(float m[16]);
int ol_mat4_normal(float out[16], const float model[16]);
void ol_mat4_mul(float out[16], const float a[16], const float b[16]);
int ol_mat4_perspective(float out[16], float fovy_radians, float aspect, float near_z, float far_z);
int ol_mat4_look_at(float out[16], const float eye[3], const float center[3], const float up[3]);
void ol_mat4_trs(float out[16], const float position[3], const float rotation[4],
                 const float scale[3]);
struct ol3d_node {
    int parent;
    float position[3], rotation[4], scale[3];
};
int ol3d_scene_matrices(const struct ol3d_node *, unsigned count, float (*world)[16]);
int ol3d_skin_vertex(float out[3], const float position[3], const unsigned joints[4],
                     const float weights[4], const float (*skin)[16], unsigned joint_count);
#endif
