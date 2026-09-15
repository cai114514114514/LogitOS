#include "openlogit_scene.h"

#include <math.h>
#include <string.h>

static float dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross(float out[3], const float a[3], const float b[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int camera_basis(const struct ol3d_orbit *camera, float eye[3],
                        float forward[3], float right[3], float up[3])
{
    if (!camera || !isfinite(camera->yaw) || !isfinite(camera->pitch) ||
        fabsf(camera->pitch) >= 1.55f || !isfinite(camera->distance) || camera->distance <= 0 ||
        !isfinite(camera->fovy) || camera->fovy <= 0 || camera->fovy >= 3.1f ||
        !isfinite(camera->aspect) || camera->aspect <= 0)
        return OL_ARGUMENT;
    for (unsigned axis = 0; axis < 3; axis++) {
        if (!isfinite(camera->target[axis]))
            return OL_ARGUMENT;
    }
    /* The guest SDK links the double-precision libm subset. Evaluate angles
     * with its real exports and round to the scene's float storage here;
     * host-only sinf/cosf calls compile on macOS but break the shipped SDK. */
    float pitch_cosine = (float)cos(camera->pitch);
    float pitch_sine = (float)sin(camera->pitch);
    float yaw_cosine = (float)cos(camera->yaw);
    float yaw_sine = (float)sin(camera->yaw);
    forward[0] = -pitch_cosine * yaw_sine;
    forward[1] = -pitch_sine;
    forward[2] = -pitch_cosine * yaw_cosine;
    right[0] = yaw_cosine;
    right[1] = 0;
    right[2] = -yaw_sine;
    cross(up, right, forward);
    for (unsigned axis = 0; axis < 3; axis++)
        eye[axis] = camera->target[axis] - camera->distance * forward[axis];
    return OL_OK;
}

int ol3d_orbit_matrices(const struct ol3d_orbit *camera, float eye[3], float view_projection[16])
{
    float location[3], forward[3], right[3], up[3], view[16], projection[16];
    if (!eye || !view_projection || camera_basis(camera, location, forward, right, up) != OL_OK)
        return OL_ARGUMENT;
    if (ol_mat4_look_at(view, location, camera->target, up) != OL_OK ||
        ol_mat4_perspective(projection, camera->fovy, camera->aspect,
                            camera->near_z, camera->far_z) != OL_OK)
        return OL_ARGUMENT;
    ol_mat4_mul(view_projection, projection, view);
    memcpy(eye, location, sizeof location);
    return OL_OK;
}

int ol3d_orbit_ray(const struct ol3d_orbit *camera, float x, float y, struct ol3d_ray *out)
{
    float eye[3], forward[3], right[3], up[3];
    if (!out || !isfinite(x) || !isfinite(y) ||
        camera_basis(camera, eye, forward, right, up) != OL_OK)
        return OL_ARGUMENT;
    struct ol3d_ray ray;
    float tangent = (float)tan(camera->fovy * .5f);
    for (unsigned axis = 0; axis < 3; axis++) {
        ray.origin[axis] = eye[axis];
        ray.direction[axis] = forward[axis] + tangent *
            (x * camera->aspect * right[axis] + y * up[axis]);
    }
    float length = sqrtf(dot(ray.direction, ray.direction));
    if (!isfinite(length) || length < 1e-8f)
        return OL_ARGUMENT;
    for (unsigned axis = 0; axis < 3; axis++)
        ray.direction[axis] /= length;
    *out = ray;
    return OL_OK;
}

static int transform(float out[3], const float model[16], const float point[4])
{
    float w = model[3] * point[0] + model[7] * point[1] + model[11] * point[2] + model[15];
    if (!isfinite(w) || fabsf(w) < 1e-8f)
        return 0;
    for (unsigned axis = 0; axis < 3; axis++) {
        out[axis] = (model[axis] * point[0] + model[4 + axis] * point[1] +
                     model[8 + axis] * point[2] + model[12 + axis]) / w;
        if (!isfinite(out[axis]))
            return 0;
    }
    return 1;
}

float ol3d_mesh_pick(const struct ol3d_mesh *mesh, const float model[16],
                    const struct ol3d_ray *ray, unsigned *triangle)
{
    if (!mesh || !model || !ray || !mesh->vertices || !mesh->indices || mesh->index_count % 3)
        return -1;
    float length = dot(ray->direction, ray->direction);
    if (!isfinite(length) || fabsf(length - 1) > .001f)
        return -1;
    float nearest = -1;
    unsigned selected = 0;
    for (size_t index = 0; index < mesh->index_count; index += 3) {
        float points[3][3], edge1[3], edge2[3], offset[3], perpendicular[3], q[3];
        for (unsigned corner = 0; corner < 3; corner++) {
            uint32_t vertex = mesh->indices[index + corner];
            if (vertex >= mesh->vertex_count ||
                !transform(points[corner], model, mesh->vertices[vertex].attribute[0]))
                return -1;
        }
        for (unsigned axis = 0; axis < 3; axis++) {
            edge1[axis] = points[1][axis] - points[0][axis];
            edge2[axis] = points[2][axis] - points[0][axis];
            offset[axis] = ray->origin[axis] - points[0][axis];
        }
        cross(perpendicular, ray->direction, edge2);
        float determinant = dot(edge1, perpendicular);
        if (fabsf(determinant) < 1e-8f)
            continue;
        float u = dot(offset, perpendicular) / determinant;
        cross(q, offset, edge1);
        float v = dot(ray->direction, q) / determinant;
        float distance = dot(edge2, q) / determinant;
        if (u >= 0 && v >= 0 && u + v <= 1 && distance >= 0 &&
            isfinite(distance) && (nearest < 0 || distance < nearest)) {
            nearest = distance;
            selected = (unsigned)(index / 3);
        }
    }
    if (nearest >= 0 && triangle)
        *triangle = selected;
    return nearest;
}

int ol3d_planar_shadow(float out[16], const float plane[4], const float light[4])
{
    if (!out || !plane || !light)
        return OL_ARGUMENT;
    float product = 0;
    for (unsigned axis = 0; axis < 4; axis++) {
        if (!isfinite(plane[axis]) || !isfinite(light[axis]))
            return OL_ARGUMENT;
        product += plane[axis] * light[axis];
    }
    if (!isfinite(product) || fabsf(product) < 1e-8f || dot(plane, plane) < 1e-12f)
        return OL_ARGUMENT;
    float matrix[16];
    for (unsigned column = 0; column < 4; column++) {
        for (unsigned row = 0; row < 4; row++) {
            float value = (column == row ? product : 0) - light[row] * plane[column];
            if (!isfinite(value))
                return OL_ARGUMENT;
            matrix[column * 4 + row] = value;
        }
    }
    memcpy(out, matrix, sizeof matrix);
    return OL_OK;
}
