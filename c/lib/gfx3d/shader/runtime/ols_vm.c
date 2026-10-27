#include "ols_util.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* Straight-line vec4 execution and texture sampling; no source parser is linked by this file. */
static void texel(const struct ol3d_texture *t, int x, int y, float out[4])
{
    if (t->repeat) {
        x %= (int)t->width;
        y %= (int)t->height;
        if (x < 0)
            x += t->width;
        if (y < 0)
            y += t->height;
    } else {
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (x >= (int)t->width)
            x = t->width - 1;
        if (y >= (int)t->height)
            y = t->height - 1;
    }
    const unsigned char *p = t->pixels + (size_t)y * t->stride + x * 4;
    for (int k = 0; k < 4; k++)
        out[k] = p[k] / 255.0f;
}
static int sample_level(const struct ol3d_texture *t, float u, float v, float out[4])
{
    if (!t || !t->pixels || !t->width || !t->height || t->width > 4096 || t->height > 4096 ||
        t->stride < t->width * 4 || t->bytes < (size_t)(t->height - 1) * t->stride + t->width * 4 ||
        !isfinite(u) || !isfinite(v))
        return OL_ARGUMENT;
    if (t->repeat) {
        u -= floorf(u);
        v -= floorf(v);
    } else {
        u = fmin(1, fmax(0, u));
        v = fmin(1, fmax(0, v));
    }
    float x = u * t->width - .5f, y = v * t->height - .5f;
    if (!t->bilinear) {
        texel(t, (int)floorf(x + .5f), (int)floorf(y + .5f), out);
        return OL_OK;
    }
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float a[4], b[4], c[4], d[4], fx = x - ix, fy = y - iy;
    texel(t, ix, iy, a);
    texel(t, ix + 1, iy, b);
    texel(t, ix, iy + 1, c);
    texel(t, ix + 1, iy + 1, d);
    for (int k = 0; k < 4; k++)
        out[k] = (a[k] + (b[k] - a[k]) * fx) * (1 - fy) + (c[k] + (d[k] - c[k]) * fx) * fy;
    return OL_OK;
}
/* UV deltas to the neighbours at x+1 and y+1 for one TEX2D, or null for LOD 0. */
struct uv_delta {
    float dx[2], dy[2];
};
#ifndef OL3D_MIP_FORCE_LOD0
static int level_view(const struct ol3d_texture *t, unsigned level, struct ol3d_texture *v)
{
    *v = *t;
    v->mips = 0;
    v->mip_count = 0;
    v->mip_filter = 0;
    if (!level)
        return 0;
    const struct ol3d_mip_level *m = &t->mips[level - 1];
    unsigned w = t->width >> level, h = t->height >> level;
    if (!m->pixels || m->width != (w ? w : 1) || m->height != (h ? h : 1) ||
        m->stride < m->width * 4 || m->bytes < (size_t)(m->height - 1) * m->stride + m->width * 4)
        return -1;
    v->pixels = m->pixels;
    v->width = m->width;
    v->height = m->height;
    v->stride = m->stride;
    v->bytes = m->bytes;
    return 0;
}
#endif
static int texture(const struct ol3d_texture *t, float u, float v, const struct uv_delta *d,
                   float out[4])
{
#ifndef OL3D_MIP_FORCE_LOD0
    if (t && t->mip_filter) {
        if (t->mip_filter >= OL3D_MIP_FILTER_COUNT || !t->mips || !t->mip_count ||
            t->mip_count > 15 || !t->width || !t->height || t->width > 4096 || t->height > 4096)
            return OL_ARGUMENT;
        float lod = 0;
        if (d) {
            float ax = d->dx[0] * t->width, bx = d->dx[1] * t->height, ay = d->dy[0] * t->width,
                  by = d->dy[1] * t->height;
            float rho = fmax(sqrtf(ax * ax + bx * bx), sqrtf(ay * ay + by * by));
            if (isfinite(rho) && rho > 1)
                lod = (float)log2(rho);
            else if (!isfinite(rho))
                lod = (float)t->mip_count;
        }
        if (lod > (float)t->mip_count)
            lod = (float)t->mip_count;
        if (t->mip_filter == OL3D_MIP_NEAREST || lod >= (float)t->mip_count) {
            struct ol3d_texture lv;
            if (level_view(t, (unsigned)(lod + .5f), &lv))
                return OL_ARGUMENT;
            return sample_level(&lv, u, v, out);
        }
        unsigned l0 = (unsigned)lod;
        float f = lod - l0, a[4], c[4];
        struct ol3d_texture v0, v1;
        if (level_view(t, l0, &v0) || level_view(t, l0 + 1, &v1) ||
            sample_level(&v0, u, v, a) || sample_level(&v1, u, v, c))
            return OL_ARGUMENT;
        for (int k = 0; k < 4; k++)
            out[k] = a[k] + (c[k] - a[k]) * f;
        return OL_OK;
    }
#else
    (void)d;
#endif
    return sample_level(t, u, v, out);
}
/* rec records each TEX2D coordinate by instruction (probe pass); uvx/uvy hold the probe
 * coordinates at the x+1 / y+1 neighbours and turn into per-instruction deltas. */
static int run(const struct ol_shader_program *p, const float input[][4], unsigned inputs,
               const struct ol3d_bindings *b, struct ols_output *out, float (*rec)[2],
               float (*const uvx)[2], float (*const uvy)[2])
{
    float regs[OLS_REGISTERS][4];
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < p->count; i++) {
        const struct ols_instruction *c = &p->code[i];
        float v[4] = {0}, *a = regs[c->a < OLS_REGISTERS ? c->a : 0],
              *bb = regs[c->b < OLS_REGISTERS ? c->b : 0],
              *cc = regs[c->c < OLS_REGISTERS ? c->c : 0];
        switch (c->op) {
        case OLS_CONST:
            memcpy(v, c->literal, sizeof v);
            break;
        case OLS_INPUT:
            if (c->a >= inputs)
                return OL_ARGUMENT;
            memcpy(v, input[c->a], sizeof v);
            break;
        case OLS_UNIFORM:
            if (!b->uniforms || c->a >= b->uniform_count)
                return OL_ARGUMENT;
            memcpy(v, b->uniforms[c->a], sizeof v);
            break;
        case OLS_MAT4:
            if (!b->uniforms || c->b + 4 > b->uniform_count)
                return OL_ARGUMENT;
            for (int k = 0; k < 4; k++)
                for (int j = 0; j < 4; j++)
                    v[k] += b->uniforms[c->b + j][k] * a[j];
            break;
        case OLS_TEX2D:
            if (!b->textures || c->b >= b->texture_count)
                return OL_ARGUMENT;
        {
            struct uv_delta d, *dp = 0;
            if (rec) {
                rec[i][0] = a[0];
                rec[i][1] = a[1];
            }
            if (uvx) {
                d = (struct uv_delta){{uvx[i][0] - a[0], uvx[i][1] - a[1]},
                                      {uvy[i][0] - a[0], uvy[i][1] - a[1]}};
                dp = &d;
            }
            if (texture(&b->textures[c->b], a[0], a[1], dp, v))
                return OL_ARGUMENT;
        }
            break;
        case OLS_POSITION:
            memcpy(out->position, a, sizeof v);
            continue;
        case OLS_VARYING:
            memcpy(out->varying[c->dst], a, sizeof v);
            continue;
        case OLS_COLOR:
            memcpy(out->color, a, sizeof v);
            continue;
        case OLS_DOT3:
        case OLS_DOT4: {
            float dot = 0;
            for (unsigned k = 0; k < (c->op == OLS_DOT3 ? 3u : 4u); k++)
                dot += a[k] * bb[k];
            for (int k = 0; k < 4; k++)
                v[k] = dot;
            break;
        }
        default:
            for (int k = 0; k < 4; k++)
                switch (c->op) {
                case OLS_MOV:
                    v[k] = a[k];
                    break;
                case OLS_ADD:
                    v[k] = a[k] + bb[k];
                    break;
                case OLS_SUB:
                    v[k] = a[k] - bb[k];
                    break;
                case OLS_MUL:
                    v[k] = a[k] * bb[k];
                    break;
                case OLS_MAD:
                    v[k] = a[k] * bb[k] + cc[k];
                    break;
                case OLS_MIN:
                    v[k] = fmin(a[k], bb[k]);
                    break;
                case OLS_MAX:
                    v[k] = fmax(a[k], bb[k]);
                    break;
                case OLS_RCP:
                    if (a[k] == 0)
                        return OL_RENDER_FAILED;
                    v[k] = 1 / a[k];
                    break;
                case OLS_RSQRT:
                    if (a[k] <= 0)
                        return OL_RENDER_FAILED;
                    v[k] = 1 / sqrtf(a[k]);
                    break;
                case OLS_SIN:
                    v[k] = sin(a[k]);
                    break;
                case OLS_COS:
                    v[k] = cos(a[k]);
                    break;
                case OLS_SELECT:
                    v[k] = a[k] >= 0 ? bb[k] : cc[k];
                    break;
                case OLS_SWIZZLE:
                    v[k] = a[(c->b >> (2 * k)) & 3];
                    break;
                default:
                    return OL_ARGUMENT;
                }
            break;
        }
        if (!ols_finite4(v))
            return OL_RENDER_FAILED;
        memcpy(regs[c->dst], v, sizeof v);
    }
#ifdef OPENLOGIT_SHADER_DISABLED
    for (int k = 0; k < 4; k++)
        out->color[k] = 0;
#endif
    return OL_OK;
}
int ols_run(const struct ol_shader_program *p, const float input[][4], unsigned inputs,
            const struct ol3d_bindings *b, struct ols_output *out)
{
    return run(p, input, inputs, b, out, 0, 0, 0);
}
/* Quad LOD: the pixel shader runs at the pixel and at its x+1 and y+1 neighbours (inputs
 * advanced by ddx / ddy), the neighbours only to capture each TEX2D coordinate. Skipped
 * unless a bound texture asks for mips, so legacy draws run exactly once. */
int ols_run_lod(const struct ol_shader_program *p, const float input[][4], unsigned inputs,
                const struct ol3d_bindings *b, struct ols_output *out, const float (*ddx)[4],
                const float (*ddy)[4])
{
    int mips = 0;
    if (ddx && ddy && b && b->textures && inputs <= OL3D_MAX_VARYINGS)
        for (unsigned i = 0; i < b->texture_count; i++)
            mips |= b->textures[i].mip_filter != OL3D_MIP_NONE;
    if (!mips)
        return run(p, input, inputs, b, out, 0, 0, 0);
    float nx[OL3D_MAX_VARYINGS][4], ny[OL3D_MAX_VARYINGS][4];
    float rx[OLS_INSTRUCTIONS][2] = {{0}}, ry[OLS_INSTRUCTIONS][2] = {{0}};
    struct ols_output scratch;
    for (unsigned j = 0; j < inputs; j++)
        for (int k = 0; k < 4; k++) {
            nx[j][k] = input[j][k] + ddx[j][k];
            ny[j][k] = input[j][k] + ddy[j][k];
        }
    int r = run(p, nx, inputs, b, &scratch, rx, 0, 0);
    if (!r)
        r = run(p, ny, inputs, b, &scratch, ry, 0, 0);
    if (r)
        return r;
    return run(p, input, inputs, b, out, 0, rx, ry);
}
