#include "lsl_internal.h"

#include <stdio.h>

static int link_error(struct ol_shader_error *error, const char *message)
{
    if (error) {
        error->line = 0;
        snprintf(error->message, sizeof error->message, "%s", message);
    }
    return OL_ARGUMENT;
}

static int link_varyings(const struct ol_lsl_shader *vertex,
                         const struct ol_lsl_shader *fragment,
                         struct ol_shader_error *error)
{
    for (unsigned input = 0; input < fragment->binding_count; input++) {
        const struct ol_lsl_binding *required = &fragment->bindings[input];
        if (required->storage != LSL_INPUT)
            continue;
        int matched = 0;
        for (unsigned output = 0; output < vertex->binding_count; output++) {
            const struct ol_lsl_binding *provided = &vertex->bindings[output];
            if (provided->storage != LSL_OUTPUT || provided->location != required->location)
                continue;
            if (provided->type != required->type)
                return link_error(error, "vertex and fragment varying types differ");
            matched = 1;
            break;
        }
        if (!matched)
            return link_error(error, "fragment input has no vertex output");
    }
    return OL_OK;
}

static int link_uniforms(const struct ol_lsl_shader *vertex,
                         const struct ol_lsl_shader *fragment,
                         struct ol_shader_error *error)
{
    for (unsigned first = 0; first < vertex->binding_count; first++) {
        const struct ol_lsl_binding *left = &vertex->bindings[first];
        if (left->storage != LSL_UNIFORM)
            continue;
        unsigned left_width = left->type == LSL_MAT4 ? 4 : 1;
        for (unsigned second = 0; second < fragment->binding_count; second++) {
            const struct ol_lsl_binding *right = &fragment->bindings[second];
            if (right->storage != LSL_UNIFORM ||
                (left->type == LSL_SAMPLER2D) != (right->type == LSL_SAMPLER2D))
                continue;
            unsigned right_width = right->type == LSL_MAT4 ? 4 : 1;
            int overlap = left->location < right->location + right_width &&
                          right->location < left->location + left_width;
            if (overlap && (left->location != right->location || left->type != right->type))
                return link_error(error, "stage uniform ranges or types conflict");
        }
    }
    return OL_OK;
}

int ol_lsl_pipeline_create(const struct ol_lsl_shader *vertex,
                           const struct ol_lsl_shader *fragment,
                           const struct ol_lsl_raster_state *state,
                           struct ol3d_pipeline_object **out_pipeline,
                           struct ol_shader_error *error)
{
    if (out_pipeline)
        *out_pipeline = NULL;
    if (!vertex || !fragment || !state || !out_pipeline ||
        vertex->ir.stage != LSL_VERTEX || fragment->ir.stage != LSL_FRAGMENT)
        return link_error(error, "pipeline requires vertex and fragment shaders");
    if (link_varyings(vertex, fragment, error) != OL_OK ||
        link_uniforms(vertex, fragment, error) != OL_OK)
        return OL_ARGUMENT;

    unsigned varying_count = 0;
    for (unsigned index = 0; index < vertex->binding_count; index++) {
        const struct ol_lsl_binding *binding = &vertex->bindings[index];
        if (binding->storage == LSL_OUTPUT && binding->location >= varying_count)
            varying_count = binding->location + 1;
    }
    struct ol3d_pipeline pipeline = {
        .vertex = &vertex->ir,
        .pixel = &fragment->ir,
        .varying_count = varying_count,
        .cull_back = state->cull_back,
        .depth_test = state->depth_test,
        .depth_write = state->depth_write,
        .blend = state->blend
    };
    return ol3d_pipeline_create(&pipeline, out_pipeline, error);
}
