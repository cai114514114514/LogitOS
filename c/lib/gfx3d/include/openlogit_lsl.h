#ifndef OPENLOGIT_LSL_H
#define OPENLOGIT_LSL_H
#include "openlogit_3d.h"

/* LSL 1 is the native typed source language. Stages are vertex / fragment;
 * OLS-IR remains the versioned execution format for existing binaries.
 * Shader objects own immutable code and reflection. Compile failure publishes
 * no object; diagnostics use source lines. No compiler code enters the kernel. */
enum lsl_stage {
    LSL_VERTEX = OLS_VERTEX,
    LSL_FRAGMENT = OLS_PIXEL
};

enum lsl_type {
    LSL_FLOAT = 1,
    LSL_VEC2 = 2,
    LSL_VEC3 = 3,
    LSL_VEC4 = 4,
    LSL_MAT4 = 5,
    LSL_SAMPLER2D = 6,
    LSL_BOOL = 7
};

enum lsl_binding_storage {
    LSL_INPUT = 1,
    LSL_OUTPUT = 2,
    LSL_UNIFORM = 3
};

struct ol_lsl_binding {
    char name[48];
    unsigned storage;
    unsigned type;
    unsigned location;
};

struct ol_lsl_shader;

/* Compile and pipeline creation return OL_OK or an OL_* error code. */
int ol_lsl_compile(const char *source, size_t bytes,
                   struct ol_lsl_shader **out_shader,
                   struct ol_shader_error *error);
void ol_lsl_destroy(struct ol_lsl_shader *shader);
const struct ol_shader_program *ol_lsl_ir(const struct ol_lsl_shader *shader);
unsigned ol_lsl_binding_count(const struct ol_lsl_shader *shader);
/* Reflection queries return 1 when found, 0 for invalid or absent bindings. */
int ol_lsl_binding(const struct ol_lsl_shader *shader, unsigned index,
                   struct ol_lsl_binding *out_binding);
int ol_lsl_find_binding(const struct ol_lsl_shader *shader, const char *name,
                        struct ol_lsl_binding *out_binding);

/* Typed stage linkage checks locations and uniform ranges before creating the
 * existing immutable 3D pipeline. Destruction of the shaders is then safe. */
struct ol_lsl_raster_state {
    int cull_back;
    int depth_test;
    int depth_write;
    int blend;
};

int ol_lsl_pipeline_create(const struct ol_lsl_shader *vertex,
                           const struct ol_lsl_shader *fragment,
                           const struct ol_lsl_raster_state *state,
                           struct ol3d_pipeline_object **out_pipeline,
                           struct ol_shader_error *error);

#endif
