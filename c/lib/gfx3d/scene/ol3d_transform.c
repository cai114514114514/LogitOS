#include "openlogit_3d.h"
#include <math.h>
#include <string.h>

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
