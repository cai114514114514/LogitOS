#include "ols_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct ol3d_context {
    unsigned width, height;
    unsigned char *front, *work;
    float *depth;
    int active, error;
    struct ol3d_stats stats;
};
struct vertex {
    float p[4], v[OL3D_MAX_VARYINGS][4];
};
void ol3d_caps(struct ol3d_caps *c)
{
    if (c)
        *c = (struct ol3d_caps){OL_VERSION(1, 1), OL_BACKEND_SOFTWARE, OL_CAP_3D, 4096,
                                OLS_REGISTERS,    OLS_INSTRUCTIONS};
}
int ol3d_create(unsigned w, unsigned h, struct ol3d_context **out)
{
    if (!out)
        return OL_ARGUMENT;
    *out = 0;
    if (!w || !h || w > 4096 || h > 4096)
        return OL_ARGUMENT;
    struct ol3d_context *c = calloc(1, sizeof *c);
    if (!c)
        return OL_LIMIT;
    size_t bytes = (size_t)w * h * 4;
    c->front = calloc(1, bytes);
    c->work = malloc(bytes);
    c->depth = malloc(bytes);
    if (!c->front || !c->work || !c->depth) {
        ol3d_destroy(c);
        return OL_LIMIT;
    }
    c->width = w;
    c->height = h;
    *out = c;
    return OL_OK;
}
void ol3d_destroy(struct ol3d_context *c)
{
    if (c) {
        free(c->depth);
        free(c->work);
        free(c->front);
        free(c);
    }
}
int ol3d_begin(struct ol3d_context *c, unsigned rgb)
{
    if (!c)
        return OL_ARGUMENT;
    if (c->active)
        return OL_STATE;
    c->active = 1;
    c->error = 0;
    for (size_t i = 0; i < (size_t)c->width * c->height; i++) {
        c->work[4 * i] = GFX_R(rgb);
        c->work[4 * i + 1] = GFX_G(rgb);
        c->work[4 * i + 2] = GFX_B(rgb);
        c->work[4 * i + 3] = 255;
        c->depth[i] = 1;
    }
    return OL_OK;
}
static float distance(const struct vertex *v, int plane)
{
    switch (plane) {
    case 0:
        return v->p[3] + v->p[0];
    case 1:
        return v->p[3] - v->p[0];
    case 2:
        return v->p[3] + v->p[1];
    case 3:
        return v->p[3] - v->p[1];
    case 4:
        return v->p[2];
    default:
        return v->p[3] - v->p[2];
    }
}
static struct vertex mix(const struct vertex *a, const struct vertex *b, float t, unsigned vary)
{
    struct vertex r = {0};
    for (int k = 0; k < 4; k++)
        r.p[k] = a->p[k] + (b->p[k] - a->p[k]) * t;
    for (unsigned j = 0; j < vary; j++)
        for (int k = 0; k < 4; k++)
            r.v[j][k] = a->v[j][k] + (b->v[j][k] - a->v[j][k]) * t;
    return r;
}
static unsigned clip(struct vertex *out, const struct vertex in[3], unsigned vary)
{
    struct vertex a[12], b[12];
    memcpy(a, in, 3 * sizeof *a);
    unsigned n = 3;
    for (int plane = 0; plane < 6 && n; plane++) {
        unsigned count = 0;
        for (unsigned i = 0; i < n; i++) {
            const struct vertex *x = &a[i], *y = &a[(i + 1) % n];
            float dx = distance(x, plane), dy = distance(y, plane);
            if (dx >= 0)
                b[count++] = *x;
            if ((dx < 0) != (dy < 0))
                b[count++] = mix(x, y, dx / (dx - dy), vary);
        }
        n = count;
        memcpy(a, b, n * sizeof *a);
    }
    memcpy(out, a, n * sizeof *out);
    return n;
}
struct projected {
    int64_t x, y;
    float z, invw;
    const struct vertex *source;
};
static int64_t edge(const struct projected *a, const struct projected *b, int64_t x, int64_t y)
{
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}
static int top_left(const struct projected *a, const struct projected *b)
{
    return b->y < a->y || (b->y == a->y && b->x > a->x);
}
static int raster(struct ol3d_context *c, const struct vertex tri[3], const struct ol3d_draw *draw)
{
    struct projected v[3];
    for (int i = 0; i < 3; i++) {
        if (tri[i].p[3] <= 1e-12f)
            return OL_OK;
        float inv = 1 / tri[i].p[3];
        v[i] = (struct projected){
            (int64_t)floorf((tri[i].p[0] * inv * .5f + .5f) * c->width * 256 + .5f),
            (int64_t)floorf((.5f - tri[i].p[1] * inv * .5f) * c->height * 256 + .5f),
            tri[i].p[2] * inv, inv, &tri[i]};
    }
    int64_t area = edge(&v[0], &v[1], v[2].x, v[2].y);
    if (!area || (draw->pipeline.cull_back && area >= 0))
        return OL_OK;
    if (area < 0) {
        struct projected tmp = v[1];
        v[1] = v[2];
        v[2] = tmp;
        area = -area;
    }
    int64_t x0 = v[0].x, x1 = x0, y0 = v[0].y, y1 = y0;
    for (int i = 1; i < 3; i++) {
        if (v[i].x < x0)
            x0 = v[i].x;
        if (v[i].x > x1)
            x1 = v[i].x;
        if (v[i].y < y0)
            y0 = v[i].y;
        if (v[i].y > y1)
            y1 = v[i].y;
    }
    int left = (int)(x0 / 256), right = (int)((x1 + 255) / 256), top = (int)(y0 / 256),
        bottom = (int)((y1 + 255) / 256);
    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > (int)c->width)
        right = c->width;
    if (bottom > (int)c->height)
        bottom = c->height;
    int tl0 = top_left(&v[1], &v[2]), tl1 = top_left(&v[2], &v[0]), tl2 = top_left(&v[0], &v[1]);
    double inv_area = 1.0 / (double)area;
    for (int y = top; y < bottom; y++)
        for (int x = left; x < right; x++) {
            int64_t px = x * 256 + 128, py = y * 256 + 128;
            int64_t e0 = edge(&v[1], &v[2], px, py), e1 = edge(&v[2], &v[0], px, py),
                    e2 = edge(&v[0], &v[1], px, py);
            if (e0 < 0 || e1 < 0 || e2 < 0 || (!e0 && !tl0) || (!e1 && !tl1) || (!e2 && !tl2))
                continue;
            float a = (float)(e0 * inv_area), b = (float)(e1 * inv_area),
                  d = (float)(e2 * inv_area);
            float z = a * v[0].z + b * v[1].z + d * v[2].z;
            size_t offset = (size_t)y * c->width + x;
            c->stats.fragments++;
#ifndef OPENLOGIT_DEPTH_DISABLED
            if (draw->pipeline.depth_test && z >= c->depth[offset])
                continue;
#endif
            float wa = a * v[0].invw, wb = b * v[1].invw, wd = d * v[2].invw, den = wa + wb + wd;
            if (den <= 0 || !isfinite(den))
                return OL_RENDER_FAILED;
            float input[OL3D_MAX_VARYINGS][4] = {{0}};
            for (unsigned j = 0; j < draw->pipeline.varying_count; j++)
                for (int k = 0; k < 4; k++)
                    input[j][k] = (wa * v[0].source->v[j][k] + wb * v[1].source->v[j][k] +
                                   wd * v[2].source->v[j][k]) /
                                  den;
            struct ols_output output;
            int status = ols_run(draw->pipeline.pixel, input, draw->pipeline.varying_count,
                                 &draw->bindings, &output);
            if (status)
                return status;
            c->stats.shaded_pixels++;
            int rgba[4];
            for (int k = 0; k < 4; k++)
                rgba[k] = (int)(fmin(1, fmax(0, output.color[k])) * 255 + .5f);
            unsigned char *dst = c->work + offset * 4;
            if (draw->pipeline.blend)
                gfx_over(dst, rgba[0], rgba[1], rgba[2], rgba[3], 255);
            else
                for (int k = 0; k < 4; k++)
                    dst[k] = (unsigned char)rgba[k];
            if (draw->pipeline.depth_write)
                c->depth[offset] = z;
        }
    return OL_OK;
}
int ol3d_abort(struct ol3d_context *c,int error)
{
    if(!c||!c->active)return OL_STATE;
    if(error>=0)error=OL_RENDER_FAILED;
    if(!c->error)c->error=error;
    return c->error;
}
int ol3d_draw_indexed(struct ol3d_context *c, const struct ol3d_draw *d)
{
    if (!c || !d)
        return OL_ARGUMENT;
    if (!c->active)
        return OL_STATE;
    if (c->error)
        return c->error;
    if (!d->vertices || !d->indices || !d->vertex_count || d->vertex_count > 65536 ||
        d->index_count > 196608 || d->index_count % 3 ||
        d->pipeline.varying_count > OL3D_MAX_VARYINGS ||
        ol3d_pipeline_validate(&d->pipeline, &d->bindings, 0) ||
        d->pipeline.vertex->stage != OLS_VERTEX || d->pipeline.pixel->stage != OLS_PIXEL ||
        d->bindings.uniform_count > OL3D_MAX_UNIFORMS ||
        d->bindings.texture_count > OL3D_MAX_TEXTURES)
        return c->error = OL_ARGUMENT;
    /* Validate all indices BEFORE any vertex shader or target write. Buffers
     * are borrowed only during this synchronous call, never retained naked. */
    for (size_t i = 0; i < d->index_count; i++)
        if (d->indices[i] >= d->vertex_count)
            return c->error = OL_ARGUMENT;
    struct vertex *cache = calloc(d->vertex_count, sizeof *cache);
    if (!cache)
        return c->error = OL_LIMIT;
    for (size_t i = 0; i < d->vertex_count; i++) {
        struct ols_output o;
        int r = ols_run(d->pipeline.vertex, d->vertices[i].attribute, OL3D_MAX_ATTRIBUTES,
                        &d->bindings, &o);
        if (r) {
            c->error = r;
            break;
        }
        for (int k = 0; k < 4; k++)
            if (!isfinite(o.position[k]) || fabsf(o.position[k]) > 1e12f)
                c->error = OL_RENDER_FAILED;
        if (c->error)
            break;
        memcpy(cache[i].p, o.position, sizeof cache[i].p);
        memcpy(cache[i].v, o.varying, sizeof cache[i].v);
    }
    for (size_t i = 0; i < d->index_count && !c->error; i += 3) {
        struct vertex tri[3] = {cache[d->indices[i]], cache[d->indices[i + 1]],
                                cache[d->indices[i + 2]]},
                      polygon[12];
        c->stats.triangles++;
        unsigned n = clip(polygon, tri, d->pipeline.varying_count);
        for (unsigned j = 1; j + 1 < n; j++) {
            struct vertex t[3] = {polygon[0], polygon[j], polygon[j + 1]};
            c->stats.clipped_triangles++;
            int r = raster(c, t, d);
            if (r) {
                c->error = r;
                break;
            }
        }
    }
    free(cache);
    return c->error;
}
int ol3d_end(struct ol3d_context *c, struct ol_surface *output)
{
    if (!c || !c->active)
        return OL_STATE;
    c->active = 0;
    if (c->error)
        return c->error;
    if (output) {
        struct ol_surface_view view;
        int r = ol_surface_view(output, &view);
        if (r)
            return r;
        if (view.width != c->width || view.height != c->height)
            return OL_ARGUMENT;
        r = ol_surface_upload(output, c->work, (size_t)c->width * c->height * 4, c->width * 4);
        if (r)
            return r;
    }
    memcpy(c->front, c->work, (size_t)c->width * c->height * 4);
    c->stats.frames++;
    return OL_OK;
}
int ol3d_readback(const struct ol3d_context *c, unsigned char *rgba, size_t bytes, unsigned stride)
{
    if (!c || !rgba || stride < c->width * 4 ||
        bytes < (size_t)(c->height - 1) * stride + c->width * 4)
        return OL_ARGUMENT;
    for (unsigned y = 0; y < c->height; y++)
        memcpy(rgba + (size_t)y * stride, c->front + (size_t)y * c->width * 4, c->width * 4);
    return OL_OK;
}
void ol3d_stats(const struct ol3d_context *c, struct ol3d_stats *s)
{
    if (c && s)
        *s = c->stats;
}
void ol_mat4_identity(float m[16])
{
    for (int i = 0; i < 16; i++)
        m[i] = i % 5 == 0 ? 1 : 0;
}
void ol_mat4_mul(float out[16], const float a[16], const float b[16])
{
    float m[16] = {0};
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++)
            for (int k = 0; k < 4; k++)
                m[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
    memcpy(out, m, sizeof m);
}
int ol_mat4_perspective(float out[16], float fov, float aspect, float near, float far)
{
    if (!out || !isfinite(fov) || !isfinite(aspect) || !isfinite(near) || !isfinite(far) ||
        fov <= 0 || fov >= 3.14159f || aspect <= 0 || near <= 0 || far <= near)
        return OL_ARGUMENT;
    memset(out, 0, 16 * sizeof *out);
    float f = 1 / tan(fov * .5f);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = far / (near - far);
    out[11] = -1;
    out[14] = near * far / (near - far);
    return OL_OK;
}
static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static int normalize(float v[3])
{
    float n = sqrtf(dot3(v, v));
    if (!isfinite(n) || n < 1e-12f)
        return 0;
    for (int i = 0; i < 3; i++)
        v[i] /= n;
    return 1;
}
static void cross(float o[3], const float a[3], const float b[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
int ol_mat4_look_at(float out[16], const float eye[3], const float center[3], const float up[3])
{
    if (!out || !eye || !center || !up)
        return OL_ARGUMENT;
    float f[3], s[3], u[3];
    for (int i = 0; i < 3; i++)
        f[i] = center[i] - eye[i];
    if (!normalize(f))
        return OL_ARGUMENT;
    cross(s, f, up);
    if (!normalize(s))
        return OL_ARGUMENT;
    cross(u, s, f);
    ol_mat4_identity(out);
    for (int i = 0; i < 3; i++) {
        out[i * 4] = s[i];
        out[i * 4 + 1] = u[i];
        out[i * 4 + 2] = -f[i];
    }
    out[12] = -dot3(s, eye);
    out[13] = -dot3(u, eye);
    out[14] = dot3(f, eye);
    return OL_OK;
}
void ol_mat4_trs(float out[16], const float p[3], const float q[4], const float scale[3])
{
    float id[4] = {0, 0, 0, 1}, r[4];
    ol_quat_mix(r, q, id, 0);
    float x = r[0], y = r[1], z = r[2], w = r[3];
    out[0] = (1 - 2 * y * y - 2 * z * z) * scale[0];
    out[1] = (2 * x * y + 2 * w * z) * scale[0];
    out[2] = (2 * x * z - 2 * w * y) * scale[0];
    out[3] = 0;
    out[4] = (2 * x * y - 2 * w * z) * scale[1];
    out[5] = (1 - 2 * x * x - 2 * z * z) * scale[1];
    out[6] = (2 * y * z + 2 * w * x) * scale[1];
    out[7] = 0;
    out[8] = (2 * x * z + 2 * w * y) * scale[2];
    out[9] = (2 * y * z - 2 * w * x) * scale[2];
    out[10] = (1 - 2 * x * x - 2 * y * y) * scale[2];
    out[11] = 0;
    out[12] = p[0];
    out[13] = p[1];
    out[14] = p[2];
    out[15] = 1;
}
int ol3d_scene_matrices(const struct ol3d_node *nodes, unsigned count, float (*world)[16])
{
    if (!nodes || !world || count > 4096)
        return OL_ARGUMENT;
    for (unsigned i = 0; i < count; i++)
        if (nodes[i].parent < -1 || nodes[i].parent >= (int)i)
            return OL_ARGUMENT;
    for (unsigned i = 0; i < count; i++) {
        float m[16];
        ol_mat4_trs(m, nodes[i].position, nodes[i].rotation, nodes[i].scale);
        if (nodes[i].parent >= 0)
            ol_mat4_mul(world[i], world[nodes[i].parent], m);
        else
            memcpy(world[i], m, sizeof m);
    }
    return OL_OK;
}
int ol3d_skin_vertex(float out[3], const float p[3], const unsigned joints[4],
                     const float weights[4], const float (*skin)[16], unsigned count)
{
    if (!out || !p || !joints || !weights || !skin || !count || count > 64)
        return OL_ARGUMENT;
    float sum = 0;
    for (int i = 0; i < 4; i++) {
        if (!isfinite(weights[i]) || weights[i] < 0 || (weights[i] > 0 && joints[i] >= count))
            return OL_ARGUMENT;
        sum += weights[i];
    }
    if (sum < 1e-8f || !isfinite(sum))
        return OL_ARGUMENT;
    float r[3] = {0};
    for (int i = 0; i < 4; i++)
        if (weights[i] > 0) {
            const float *m = skin[joints[i]];
            for (int j = 0; j < 3; j++)
                r[j] += (m[j] * p[0] + m[4 + j] * p[1] + m[8 + j] * p[2] + m[12 + j]) * weights[i] /
                        sum;
        }
    memcpy(out, r, sizeof r);
    return OL_OK;
}

int ol_mat4_normal(float out[16],const float m[16])
{
    if(!out||!m)return OL_ARGUMENT;
    double a=m[0],b=m[4],c=m[8],d=m[1],e=m[5],f=m[9],g=m[2],h=m[6],i=m[10];
    double det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    if(!isfinite(det)||fabs(det)<1e-20)return OL_ARGUMENT;
    float n[16]={0};
    n[0]=(e*i-f*h)/det;n[1]=(c*h-b*i)/det;n[2]=(b*f-c*e)/det;
    n[4]=(f*g-d*i)/det;n[5]=(a*i-c*g)/det;n[6]=(c*d-a*f)/det;
    n[8]=(d*h-e*g)/det;n[9]=(b*g-a*h)/det;n[10]=(a*e-b*d)/det;
    for(int k=0;k<16;k++)if(!isfinite(n[k]))return OL_ARGUMENT;
    memcpy(out,n,sizeof n);return OL_OK;
}
