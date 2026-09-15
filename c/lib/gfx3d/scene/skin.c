#include "openlogit_scene.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int valid_pose(const struct ol3d_node *node, unsigned index)
{
    if (node->parent < -1 || node->parent >= (int)index)
        return 0;
    float rotation_length = 0;
    for (unsigned axis = 0; axis < 3; axis++) {
        if (!isfinite(node->position[axis]) || !isfinite(node->scale[axis]))
            return 0;
    }
    for (unsigned axis = 0; axis < 4; axis++) {
        if (!isfinite(node->rotation[axis]))
            return 0;
        rotation_length += node->rotation[axis] * node->rotation[axis];
    }
    return isfinite(rotation_length) && rotation_length > 1e-12f;
}

int ol3d_pose_mix(struct ol3d_node *out, const struct ol3d_node *a,
                 const struct ol3d_node *b, unsigned count, float blend)
{
    if (!out || !a || !b || !count || count > 4096 ||
        !isfinite(blend) || blend < 0 || blend > 1)
        return OL_ARGUMENT;
    for (unsigned index = 0; index < count; index++) {
        if (!valid_pose(a + index, index) || !valid_pose(b + index, index) ||
            a[index].parent != b[index].parent)
            return OL_ARGUMENT;
    }
    struct ol3d_node *mixed = malloc(count * sizeof *mixed);
    if (!mixed)
        return OL_LIMIT;
    for (unsigned index = 0; index < count; index++) {
        mixed[index].parent = a[index].parent;
        for (unsigned axis = 0; axis < 3; axis++) {
            mixed[index].position[axis] = a[index].position[axis] * (1 - blend) +
                                          b[index].position[axis] * blend;
            mixed[index].scale[axis] = a[index].scale[axis] * (1 - blend) +
                                       b[index].scale[axis] * blend;
        }
        ol_quat_mix(mixed[index].rotation, a[index].rotation, b[index].rotation, blend);
        if (!valid_pose(mixed + index, index)) {
            free(mixed);
            return OL_ARGUMENT;
        }
    }
    memcpy(out, mixed, count * sizeof *mixed);
    free(mixed);
    return OL_OK;
}

int ol3d_skin_mesh(struct ol3d_vertex *out, const struct ol3d_vertex *source,
                  const struct ol3d_skin_weights *weights, size_t count,
                  const float (*bones)[16], unsigned bone_count)
{
    if (!out || !source || !weights || !bones || !count || count > 262144 ||
        !bone_count || bone_count > 64)
        return OL_ARGUMENT;
    float normals[64][16];
    for (unsigned bone = 0; bone < bone_count; bone++) {
        for (unsigned element = 0; element < 16; element++) {
            if (!isfinite(bones[bone][element]))
                return OL_ARGUMENT;
        }
        if (bones[bone][3] != 0 || bones[bone][7] != 0 || bones[bone][11] != 0 ||
            bones[bone][15] != 1 || ol_mat4_normal(normals[bone], bones[bone]) != OL_OK)
            return OL_ARGUMENT;
    }

    /* A late bad weight or cancelling normal must not leave a half-updated
     * mesh in a caller's live vertex buffer. Stage, validate, then publish. */
    struct ol3d_vertex *result = malloc(count * sizeof *result);
    if (!result)
        return OL_LIMIT;
    int status = OL_OK;
    for (size_t index = 0; index < count; index++) {
        result[index] = source[index];
        float position[3];
        if (ol3d_skin_vertex(position, source[index].attribute[0], weights[index].joints,
                             weights[index].weights, bones, bone_count) != OL_OK) {
            status = OL_ARGUMENT;
            break;
        }
        float normal[3] = {0};
        for (unsigned influence = 0; influence < 4; influence++) {
            float weight = weights[index].weights[influence];
            if (weight == 0)
                continue;
            const float *matrix = normals[weights[index].joints[influence]];
            const float *original = source[index].attribute[2];
            for (unsigned axis = 0; axis < 3; axis++)
                normal[axis] += weight * (matrix[axis] * original[0] +
                    matrix[4 + axis] * original[1] + matrix[8 + axis] * original[2]);
        }
        float length = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (!isfinite(length) || length < 1e-12f) {
            status = OL_ARGUMENT;
            break;
        }
        for (unsigned axis = 0; axis < 3; axis++) {
            if (!isfinite(position[axis]))
                status = OL_ARGUMENT;
            result[index].attribute[0][axis] = position[axis];
            result[index].attribute[2][axis] = normal[axis] / length;
        }
        result[index].attribute[0][3] = 1;
        result[index].attribute[2][3] = 0;
        if (status != OL_OK)
            break;
    }
    if (status == OL_OK)
        memcpy(out, result, count * sizeof *result);
    free(result);
    return status;
}
