#include "ols_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* OLS-IR is straight-line vec4 code. No program can manufacture a pointer,
 * branch backwards, or invoke a C callback. Bytecode is validated again at
 * binding, so editing a compiled blob does not bypass operand validation. */
static int error(struct ol_shader_error *e, unsigned line, const char *msg)
{
    if (e) {
        e->line = line;
        snprintf(e->message, sizeof e->message, "%s", msg);
    }
    return OL_ARGUMENT;
}
static int finite4(const float v[4])
{
    for (int j = 0; j < 4; j++)
        if (!isfinite(v[j]))
            return 0;
    return 1;
}
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
        return error(e, 0, "invalid OLS-IR v1 header");
    uint64_t initialized = 0;
    unsigned outputs = 0;
    for (unsigned i = 0; i < p->count; i++) {
        const struct ols_instruction *c = &p->code[i];
        if (c->op > OLS_COLOR || c->dst >= OLS_REGISTERS || c->a >= OL3D_MAX_UNIFORMS ||
            c->b >= OL3D_MAX_UNIFORMS || c->c >= OL3D_MAX_UNIFORMS)
            return error(e, i + 1, "operand outside declared limits");
        for (unsigned r = 0; r < OL3D_MAX_UNIFORMS; r++)
            if (reads(c, r) && (r >= OLS_REGISTERS || !(initialized & (1ull << r))))
                return error(e, i + 1, "read of undefined register");
        if (c->op == OLS_CONST && !finite4(c->literal))
            return error(e, i + 1, "constant must be finite");
        if (c->op == OLS_INPUT && c->a >= OL3D_MAX_ATTRIBUTES)
            return error(e, i + 1, "input index exceeds limit");
        if (c->op == OLS_MAT4 && c->b > OL3D_MAX_UNIFORMS - 4)
            return error(e, i + 1, "matrix uniforms exceed limit");
        if (c->op == OLS_TEX2D && (c->b >= OL3D_MAX_TEXTURES || p->stage != OLS_PIXEL))
            return error(e, i + 1, "texture sampling requires pixel stage and valid slot");
        if (c->op == OLS_SWIZZLE && c->b > 255)
            return error(e, i + 1, "swizzle must contain four 2-bit components");
        if (c->op == OLS_POSITION) {
            if (p->stage != OLS_VERTEX)
                return error(e, i + 1, "position output requires vertex stage");
            outputs++;
        } else if (c->op == OLS_COLOR) {
            if (p->stage != OLS_PIXEL)
                return error(e, i + 1, "color output requires pixel stage");
            outputs++;
        } else if (c->op == OLS_VARYING) {
            if (p->stage != OLS_VERTEX || c->dst >= OL3D_MAX_VARYINGS)
                return error(e, i + 1, "invalid varying output");
        } else
            initialized |= 1ull << c->dst;
    }
    if (outputs != 1)
        return error(e, 0, "exactly one stage output is required");
    if (e)
        *e = (struct ol_shader_error){0};
    return OL_OK;
}
static int number(const char *s, unsigned *out, int reg)
{
    char *end;
    if (reg) {
        if (*s != 'r')
            return 0;
        s++;
    }
    if (*s < '0' || *s > '9')
        return 0;
    unsigned long n = strtoul(s, &end, 10);
    if (*end || n > 255)
        return 0;
    *out = (unsigned)n;
    return 1;
}
int ol_shader_compile(const char *text, size_t bytes, struct ol_shader_program *p,
                      struct ol_shader_error *e)
{
    if (!text || !p || bytes > 131072)
        return error(e, 0, "source exceeds 128 KiB");
    memset(p, 0, sizeof *p);
    unsigned line = 0;
    size_t at = 0;
    int header = 0;
    static const char *names[] = {"const",   "input", "uniform",  "mov",     "add",    "sub",
                                  "mul",     "mad",   "dot3",     "dot4",    "min",    "max",
                                  "rcp",     "rsqrt", "sin",      "cos",     "select", "tex2d",
                                  "swizzle", "mat4",  "position", "varying", "color"};
    while (at < bytes) {
        line++;
        char buf[512];
        size_t n = 0;
        while (at < bytes && text[at] != '\n') {
            if (n >= sizeof buf - 1)
                return error(e, line, "line exceeds 511 bytes");
            buf[n++] = text[at++];
        }
        if (at < bytes)
            at++;
        buf[n] = 0;
        char *comment = strchr(buf, '#');
        if (comment)
            *comment = 0;
        char *tok[9], *q = buf;
        unsigned nt = 0;
        while (*q) {
            while (*q == ' ' || *q == '\t' || *q == '\r')
                q++;
            if (!*q)
                break;
            if (nt == 9)
                return error(e, line, "too many operands");
            tok[nt++] = q;
            while (*q && *q != ' ' && *q != '\t' && *q != '\r')
                q++;
            if (*q)
                *q++ = 0;
        }
        if (!nt)
            continue;
        if (!header) {
            if (nt != 3 || strcmp(tok[0], "ols") || strcmp(tok[1], "1") ||
                (strcmp(tok[2], "vertex") && strcmp(tok[2], "pixel")))
                return error(e, line, "expected: ols 1 vertex|pixel");
            p->magic = OLS_MAGIC;
            p->version = 1;
            p->stage = !strcmp(tok[2], "vertex") ? OLS_VERTEX : OLS_PIXEL;
            header = 1;
            continue;
        }
        if (p->count == OLS_INSTRUCTIONS)
            return error(e, line, "instruction budget exhausted");
        struct ols_instruction *c = &p->code[p->count++];
        unsigned op = 0;
        while (op <= OLS_COLOR && strcmp(tok[0], names[op]))
            op++;
        if (op > OLS_COLOR)
            return error(e, line, "unknown instruction");
        c->op = op;
        int ok = 1;
        if (op == OLS_CONST) {
            ok = nt == 6 && number(tok[1], &c->dst, 1);
            if (ok)
                for (int k = 0; k < 4; k++) {
                    char *end;
                    c->literal[k] = strtof(tok[k + 2], &end);
                    if (*end || !isfinite(c->literal[k]))
                        ok = 0;
                }
        } else if (op == OLS_POSITION || op == OLS_COLOR)
            ok = nt == 2 && number(tok[1], &c->a, 1);
        else if (op == OLS_VARYING)
            ok = nt == 3 && number(tok[1], &c->dst, 0) && number(tok[2], &c->a, 1);
        else if (op == OLS_INPUT || op == OLS_UNIFORM)
            ok = nt == 3 && number(tok[1], &c->dst, 1) && number(tok[2], &c->a, 0);
        else if (op == OLS_MOV || op == OLS_RCP || op == OLS_RSQRT || op == OLS_SIN ||
                 op == OLS_COS)
            ok = nt == 3 && number(tok[1], &c->dst, 1) && number(tok[2], &c->a, 1);
        else if (op == OLS_MAD || op == OLS_SELECT)
            ok = nt == 5 && number(tok[1], &c->dst, 1) && number(tok[2], &c->a, 1) &&
                 number(tok[3], &c->b, 1) && number(tok[4], &c->c, 1);
        else
            ok = nt == 4 && number(tok[1], &c->dst, 1) && number(tok[2], &c->a, 1) &&
                 number(tok[3], &c->b, !(op == OLS_TEX2D || op == OLS_SWIZZLE || op == OLS_MAT4));
        if (!ok)
            return error(e, line, "incorrect operands");
    }
    return ol_shader_validate(p, e);
}
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
static int texture(const struct ol3d_texture *t, float u, float v, float out[4])
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
int ols_run(const struct ol_shader_program *p, const float input[][4], unsigned inputs,
            const struct ol3d_bindings *b, struct ols_output *out)
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
            if (texture(&b->textures[c->b], a[0], a[1], v))
                return OL_ARGUMENT;
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
        if (!finite4(v))
            return OL_RENDER_FAILED;
        memcpy(regs[c->dst], v, sizeof v);
    }
#ifdef OPENLOGIT_SHADER_DISABLED
    for (int k = 0; k < 4; k++)
        out->color[k] = 0;
#endif
    return OL_OK;
}
