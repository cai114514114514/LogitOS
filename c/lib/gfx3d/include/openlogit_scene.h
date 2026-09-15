#ifndef OPENLOGIT_SCENE_H
#define OPENLOGIT_SCENE_H

#include "openlogit_3d.h"

/* Optional user-space scene tools. Generated meshes use attribute 0=position,
 * 2=normal, 3=UV, matching the SDK examples. Rendering remains in ol3d. */
enum ol3d_primitive { OL3D_SPHERE = 1, OL3D_TORUS, OL3D_TUBE, OL3D_PLANE };
struct ol3d_mesh_desc {
    unsigned primitive, columns, rows;
    float radius, secondary; /* torus minor radius, tube height, plane half-depth */
};
struct ol3d_mesh {
    struct ol3d_vertex *vertices;
    uint32_t *indices;
    size_t vertex_count, index_count;
};
int ol3d_mesh_create(const struct ol3d_mesh_desc *description, struct ol3d_mesh *out);
void ol3d_mesh_destroy(struct ol3d_mesh *mesh);

struct ol3d_orbit {
    float target[3], yaw, pitch, distance, fovy, aspect, near_z, far_z;
};
struct ol3d_ray { float origin[3], direction[3]; };
int ol3d_orbit_matrices(const struct ol3d_orbit *camera, float eye[3], float view_projection[16]);
/* Coordinates are normalized viewport coordinates, x/y in [-1,1], y upward. */
int ol3d_orbit_ray(const struct ol3d_orbit *camera, float x, float y, struct ol3d_ray *out);
/* Positive nearest world-ray distance, or -1 for no hit/invalid arguments.
 * Model may include nonuniform scale. Indices refer to triangles. */
float ol3d_mesh_pick(const struct ol3d_mesh *mesh, const float model[16],
                    const struct ol3d_ray *ray, unsigned *triangle);
/* Homogeneous point/directional light projected onto ax+by+cz+d=0. This is a
 * planar projection, not a shadow map; render receivers and depth normally. */
int ol3d_planar_shadow(float out[16], const float plane[4], const float light[4]);

struct ol3d_skin_weights { unsigned joints[4]; float weights[4]; };
/* Both operations validate all inputs before writing output. Skinning supports
 * exact in-place operation and transforms normals by each bone's inverse transpose.
 * Matrices must be finite, affine and nonsingular; weights are normalized. */
int ol3d_pose_mix(struct ol3d_node *out, const struct ol3d_node *a,
                 const struct ol3d_node *b, unsigned count, float blend);
int ol3d_skin_mesh(struct ol3d_vertex *out, const struct ol3d_vertex *source,
                  const struct ol3d_skin_weights *weights, size_t count,
                  const float (*bones)[16], unsigned bone_count);

#endif
