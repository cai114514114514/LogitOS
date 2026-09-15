#include "lsl_internal.h"

#include <stdlib.h>
#include <string.h>

int ol_lsl_compile(const char *source, size_t bytes,
                   struct ol_lsl_shader **out_shader,
                   struct ol_shader_error *error)
{
    if (out_shader)
        *out_shader = NULL;
    if (error)
        memset(error, 0, sizeof *error);

    struct lsl_parser parser = {
        .source = source,
        .source_bytes = bytes,
        .line = 1,
        .token_line = 1,
        .error = error
    };
    if (!source || !out_shader || !bytes || bytes > LSL_MAX_SOURCE_BYTES) {
        lsl_fail(&parser, "invalid source or source size");
        return OL_ARGUMENT;
    }
    parser.shader = calloc(1, sizeof *parser.shader);
    if (!parser.shader) {
        lsl_fail(&parser, "shader allocation failed");
        return OL_LIMIT;
    }
    parser.shader->ir.magic = OLS_MAGIC;
    parser.shader->ir.version = 1;

    if (!lsl_parse_program(&parser)) {
        free(parser.shader);
        return OL_ARGUMENT;
    }
    /* A compiler bug must not publish malformed native code. Native validation
     * numbers instructions, so report its failures at line 0, not a fake source line. */
    if (ol_shader_validate(&parser.shader->ir, error) != OL_OK) {
        if (error)
            error->line = 0;
        free(parser.shader);
        return OL_ARGUMENT;
    }
    *out_shader = parser.shader;
    return OL_OK;
}

void ol_lsl_destroy(struct ol_lsl_shader *shader)
{
    free(shader);
}

const struct ol_shader_program *ol_lsl_ir(const struct ol_lsl_shader *shader)
{
    return shader ? &shader->ir : NULL;
}

unsigned ol_lsl_binding_count(const struct ol_lsl_shader *shader)
{
    return shader ? shader->binding_count : 0;
}

int ol_lsl_binding(const struct ol_lsl_shader *shader, unsigned index,
                   struct ol_lsl_binding *out_binding)
{
    if (!shader || !out_binding || index >= shader->binding_count)
        return 0;
    *out_binding = shader->bindings[index];
    return 1;
}

int ol_lsl_find_binding(const struct ol_lsl_shader *shader, const char *name,
                        struct ol_lsl_binding *out_binding)
{
    if (!shader || !name || !out_binding)
        return 0;
    for (unsigned index = 0; index < shader->binding_count; index++) {
        if (strcmp(name, shader->bindings[index].name) == 0)
            return ol_lsl_binding(shader, index, out_binding);
    }
    return 0;
}
