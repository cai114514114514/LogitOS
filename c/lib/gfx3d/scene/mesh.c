#include "openlogit_scene.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.2831853071795864769f

void ol3d_mesh_destroy(struct ol3d_mesh *mesh)
{
    if (!mesh)
        return;
    free(mesh->vertices);
    free(mesh->indices);
    *mesh = (struct ol3d_mesh){0};
}

static void generate_vertex(struct ol3d_vertex *vertex,
                            const struct ol3d_mesh_desc *description, float u, float v)
{
    float longitude = u * TAU;
    /* Match the camera's use of the guest's double-only libm. Conversion at
     * each sample keeps vertex/normal storage and subsequent arithmetic float. */
    float cosine = (float)cos(longitude);
    float sine = (float)sin(longitude);
    float *position = vertex->attribute[0];
    float *normal = vertex->attribute[2];

    if (description->primitive == OL3D_SPHERE) {
        float latitude = v * TAU * .5f;
        float latitude_sine = (float)sin(latitude);
        normal[0] = latitude_sine * cosine;
        normal[1] = (float)cos(latitude);
        normal[2] = latitude_sine * sine;
        for (unsigned axis = 0; axis < 3; axis++)
            position[axis] = description->radius * normal[axis];
    } else if (description->primitive == OL3D_TORUS) {
        float tube_angle = v * TAU;
        float tube_cosine = (float)cos(tube_angle);
        float tube_sine = (float)sin(tube_angle);
        float ring = description->radius + description->secondary * tube_cosine;
        position[0] = ring * cosine;
        position[1] = -description->secondary * tube_sine;
        position[2] = ring * sine;
        normal[0] = tube_cosine * cosine;
        normal[1] = -tube_sine;
        normal[2] = tube_cosine * sine;
    } else if (description->primitive == OL3D_TUBE) {
        position[0] = description->radius * cosine;
        position[1] = (.5f - v) * description->secondary;
        position[2] = description->radius * sine;
        normal[0] = cosine;
        normal[2] = sine;
    } else {
        position[0] = (u * 2 - 1) * description->radius;
        position[2] = (1 - v * 2) * description->secondary;
        normal[1] = 1;
    }
    position[3] = 1;
    vertex->attribute[3][0] = u;
    vertex->attribute[3][1] = v;
}

int ol3d_mesh_create(const struct ol3d_mesh_desc *description, struct ol3d_mesh *out)
{
    if (!out)
        return OL_ARGUMENT;
    *out = (struct ol3d_mesh){0};
    if (!description || description->primitive < OL3D_SPHERE ||
        description->primitive > OL3D_PLANE || description->columns < 3 ||
        description->rows < 2 || description->columns > 256 || description->rows > 256 ||
        !isfinite(description->radius) || !isfinite(description->secondary) ||
        description->radius <= 0 || description->secondary <= 0 ||
        (description->primitive == OL3D_TORUS && description->secondary >= description->radius))
        return OL_ARGUMENT;

    struct ol3d_mesh mesh = {0};
    unsigned columns = description->columns;
    unsigned rows = description->rows;
    mesh.vertex_count = (size_t)(columns + 1) * (rows + 1);
    mesh.vertices = calloc(mesh.vertex_count, sizeof *mesh.vertices);
    mesh.indices = malloc((size_t)columns * rows * 6 * sizeof *mesh.indices);
    if (!mesh.vertices || !mesh.indices) {
        ol3d_mesh_destroy(&mesh);
        return OL_LIMIT;
    }
    for (unsigned row = 0; row <= rows; row++) {
        for (unsigned column = 0; column <= columns; column++) {
            generate_vertex(&mesh.vertices[row * (columns + 1) + column], description,
                             (float)column / columns, (float)row / rows);
        }
    }
    for (unsigned row = 0; row < rows; row++) {
        for (unsigned column = 0; column < columns; column++) {
            uint32_t a = row * (columns + 1) + column;
            uint32_t b = a + 1;
            uint32_t d = a + columns + 1;
            uint32_t c = d + 1;
            /* Sphere poles share positions but keep distinct UVs. Omit the
             * zero-area half of each polar quad instead of submitting it. */
            if (description->primitive != OL3D_SPHERE || row != 0) {
                mesh.indices[mesh.index_count++] = a;
                mesh.indices[mesh.index_count++] = b;
                mesh.indices[mesh.index_count++] = c;
            }
            if (description->primitive != OL3D_SPHERE || row + 1 != rows) {
                mesh.indices[mesh.index_count++] = a;
                mesh.indices[mesh.index_count++] = c;
                mesh.indices[mesh.index_count++] = d;
            }
        }
    }
    *out = mesh;
    return OL_OK;
}
