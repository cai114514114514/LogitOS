#include "openlogit_material.h"
#include "ols_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ol_material { struct ol_shader_program program; };
struct ol_material_context { unsigned width, height; unsigned char pixels[]; };

static int diagnostic(struct ol_shader_error *e, int status, const char *message)
{
    if (e) { e->line = 0; snprintf(e->message, sizeof e->message, "%s", message); }
    return status;
}
int ol_material_query_caps(struct ol_material_caps *c)
{
    if (!c || c->size < sizeof *c) return OL_ARGUMENT;
    *c = (struct ol_material_caps){sizeof *c, OL_API_VERSION, OL_BACKEND_SOFTWARE,
                                  OL_CAP_PIXEL_MATERIAL | OL_CAP_ATOMIC_FRAME, 4096, 4096, 2};
    return OL_OK;
}
int ol_material_create(const struct ol_shader_program *p, struct ol_material **out,
                       struct ol_shader_error *e)
{
    if (!out) return diagnostic(e, OL_ARGUMENT, "missing material output");
    *out = 0;
    int status = ol_shader_validate(p, e);
    if (status) return status;
    if (p->stage != OLS_PIXEL) return diagnostic(e, OL_ARGUMENT, "material requires pixel stage");
    for (unsigned i = 0; i < p->count; i++)
        if (p->code[i].op == OLS_INPUT && p->code[i].a >= 2) {
            diagnostic(e, OL_ARGUMENT, "material input must be 0 (UV) or 1 (pixel coordinates)");
            if (e) e->line = i + 1;
            return OL_ARGUMENT;
        }
    struct ol_material *m = malloc(sizeof *m);
    if (!m) return diagnostic(e, OL_LIMIT, "material allocation failed");
    memcpy(&m->program, p, sizeof *p);
    *out = m;
    return OL_OK;
}
void ol_material_destroy(struct ol_material *m) { free(m); }
int ol_material_context_create(unsigned w, unsigned h, struct ol_material_context **out)
{
    if (!out) return OL_ARGUMENT;
    *out = 0;
    if (!w || !h || w > 4096 || h > 4096) return OL_ARGUMENT;
    struct ol_material_context *c = malloc(sizeof *c + (size_t)w * h * 4);
    if (!c) return OL_LIMIT;
    c->width = w; c->height = h; *out = c;
    return OL_OK;
}
void ol_material_context_destroy(struct ol_material_context *c) { free(c); }
int ol_material_render(struct ol_material_context *c, const struct ol_material *m,
                       const struct ol3d_bindings *bindings, struct ol_surface *output,
                       struct ol_shader_error *e)
{
    struct ol_surface_view view;
    if (e) memset(e, 0, sizeof *e);
    if (!c || !m || ol_surface_view(output, &view) || view.width != c->width ||
        view.height != c->height || view.format != OL_FORMAT_RGBA8_STRAIGHT)
        return diagnostic(e, OL_ARGUMENT, "material target dimensions or format mismatch");
    const struct ol3d_bindings empty = {0};
    if (!bindings) bindings = &empty;
    int status = ols_bindings_validate(&m->program, bindings, e);
    if (status) return status;
    /* Immutable bytecode was validated at creation, and bindings once above.
     * Publishing inside this loop would expose a partial UI on a late RCP or
     * texture failure, and make sampling the previous target order-dependent. */
    for (unsigned y = 0; y < c->height; y++)
        for (unsigned x = 0; x < c->width; x++) {
            float input[2][4] = {{(x + .5f) / c->width, (y + .5f) / c->height, 0, 1},
                                 {x + .5f, y + .5f, c->width, c->height}};
            struct ols_output result;
            status = ols_run(&m->program, input, 2, bindings, &result);
            if (status) {
                if (e) snprintf(e->message, sizeof e->message, "pixel (%u,%u): %s", x, y,
                                ol_status_string(status));
                return status;
            }
            for (unsigned k = 0; k < 4; k++) {
                float v = result.color[k];
                c->pixels[((size_t)y * c->width + x) * 4 + k] =
                    v <= 0 ? 0 : v >= 1 ? 255 : (unsigned char)(v * 255 + .5f);
            }
        }
    status = ol_surface_upload(output, c->pixels, (size_t)c->width * c->height * 4, c->width * 4);
    if (status) diagnostic(e, status, "material target upload failed");
    return status;
}
