#include "ols_util.h"
#include <stdlib.h>
#include <string.h>
/* Instruction and binding validation is shared by LSL, legacy IR and both render paths. */
static int reads(const struct ols_instruction *c, unsigned r)
{
    switch (c->op) {
    case OLS_MOV:
    case OLS_RCP:
    case OLS_RSQRT:
    case OLS_SIN:
    case OLS_COS:
    case OLS_TEX2D:
    case OLS_SWIZZLE:
    case OLS_MAT4:
    case OLS_POSITION:
    case OLS_VARYING:
    case OLS_COLOR:
        return r == c->a;
    case OLS_ADD:
    case OLS_SUB:
    case OLS_MUL:
    case OLS_DOT3:
    case OLS_DOT4:
    case OLS_MIN:
    case OLS_MAX:
        return r == c->a || r == c->b;
    case OLS_MAD:
    case OLS_SELECT:
        return r == c->a || r == c->b || r == c->c;
    default:
        return 0;
    }
}
int ol_shader_validate(const struct ol_shader_program *p, struct ol_shader_error *e)
{
    if (!p || p->magic != OLS_MAGIC || p->version != 1 ||
        (p->stage != OLS_VERTEX && p->stage != OLS_PIXEL) || !p->count ||
        p->count > OLS_INSTRUCTIONS)
        return ols_error(e, 0, "invalid OLS-IR v1 header");
    uint64_t initialized = 0;
    unsigned outputs = 0;
    for (unsigned i = 0; i < p->count; i++) {
        const struct ols_instruction *c = &p->code[i];
        if (c->op > OLS_COLOR || c->dst >= OLS_REGISTERS || c->a >= OL3D_MAX_UNIFORMS ||
            c->b >= OL3D_MAX_UNIFORMS || c->c >= OL3D_MAX_UNIFORMS)
            return ols_error(e, i + 1, "operand outside declared limits");
        for (unsigned r = 0; r < OL3D_MAX_UNIFORMS; r++)
            if (reads(c, r) && (r >= OLS_REGISTERS || !(initialized & (1ull << r))))
                return ols_error(e, i + 1, "read of undefined register");
        if (c->op == OLS_CONST && !ols_finite4(c->literal))
            return ols_error(e, i + 1, "constant must be finite");
        if (c->op == OLS_INPUT && c->a >= OL3D_MAX_ATTRIBUTES)
            return ols_error(e, i + 1, "input index exceeds limit");
        if (c->op == OLS_MAT4 && c->b > OL3D_MAX_UNIFORMS - 4)
            return ols_error(e, i + 1, "matrix uniforms exceed limit");
        if (c->op == OLS_TEX2D && (c->b >= OL3D_MAX_TEXTURES || p->stage != OLS_PIXEL))
            return ols_error(e, i + 1, "texture sampling requires pixel stage and valid slot");
        if (c->op == OLS_SWIZZLE && c->b > 255)
            return ols_error(e, i + 1, "swizzle must contain four 2-bit components");
        if (c->op == OLS_POSITION) {
            if (p->stage != OLS_VERTEX)
                return ols_error(e, i + 1, "position output requires vertex stage");
            outputs++;
        } else if (c->op == OLS_COLOR) {
            if (p->stage != OLS_PIXEL)
                return ols_error(e, i + 1, "color output requires pixel stage");
            outputs++;
        } else if (c->op == OLS_VARYING) {
            if (p->stage != OLS_VERTEX || c->dst >= OL3D_MAX_VARYINGS)
                return ols_error(e, i + 1, "invalid varying output");
        } else
            initialized |= 1ull << c->dst;
    }
    if (outputs != 1)
        return ols_error(e, 0, "exactly one stage output is required");
    if (e)
        *e = (struct ol_shader_error){0};
    return OL_OK;
}
int ols_bindings_validate(const struct ol_shader_program *p, const struct ol3d_bindings *b,
                         struct ol_shader_error *e)
{
    if (!b || b->uniform_count > OL3D_MAX_UNIFORMS || b->texture_count > OL3D_MAX_TEXTURES ||
        (b->uniform_count && !b->uniforms) || (b->texture_count && !b->textures))
        return ols_error(e, 0, "invalid binding table");
    for (unsigned i = 0; i < b->uniform_count; i++)
        if (!ols_finite4(b->uniforms[i])) return ols_error(e, 0, "nonfinite uniform");
    for (unsigned i = 0; i < b->texture_count; i++) {
        const struct ol3d_texture *t = &b->textures[i];
        if (!t->pixels || !t->width || !t->height || t->width > 4096 || t->height > 4096 ||
            t->stride < t->width * 4 ||
            t->bytes < (size_t)(t->height - 1) * t->stride + t->width * 4)
            return ols_error(e, 0, "invalid texture storage");
    }
    for (unsigned i = 0; i < p->count; i++) {
        const struct ols_instruction *c = &p->code[i];
        if ((c->op == OLS_UNIFORM && c->a >= b->uniform_count) ||
            (c->op == OLS_MAT4 && c->b + 4 > b->uniform_count))
            return ols_error(e, i + 1, "missing uniform binding");
        if (c->op == OLS_TEX2D && c->b >= b->texture_count)
            return ols_error(e, i + 1, "missing texture binding");
    }
    if (e) memset(e, 0, sizeof *e);
    return OL_OK;
}
