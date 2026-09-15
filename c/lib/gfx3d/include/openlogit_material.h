#ifndef OPENLOGIT_MATERIAL_H
#define OPENLOGIT_MATERIAL_H
#include "openlogit_3d.h"

/* Programmable screen-space pass, using the SAME OLS-IR pixel stage as 3D.
 * This module is user-space only. It is a drawing primitive, independent of
 * widget identity, layout, input dispatch, theme and animation scheduling.
 *
 * input 0 = (u, v, 0, 1), top-left origin, sampled at pixel centers.
 * input 1 = (x + .5, y + .5, width, height), in target pixels.
 * color is component-wise straight RGBA8 (clamped/rounded, no sRGB transfer).
 * Compose the resulting surface with ol_cmd_layer for transform/clip/opacity.
 * Uniforms and RGBA texture samplers use the existing ol3d_bindings ABI. */
struct ol_material_caps {
    unsigned size, api_version, backend, features;
    unsigned max_width, max_height, inputs;
};
int ol_material_query_caps(struct ol_material_caps *);

struct ol_material;
struct ol_material_context;
/* Creation copies and validates the program. Later edits to the supplied
 * bytecode cannot change a material. Only the two documented inputs exist. */
int ol_material_create(const struct ol_shader_program *, struct ol_material **,
                       struct ol_shader_error *);
void ol_material_destroy(struct ol_material *);
/* One reusable RGBA staging buffer; allocation occurs at creation, not draw.
 * Output dimensions must match. The context, target and borrowed binding
 * storage must have one owner and stay unchanged throughout this synchronous
 * call. Sampling the old target as a texture is allowed: every pixel executes
 * before upload, including when an error occurs near the end of the pass.
 * On ANY error, target pixels and generation stay at the previous frame.
 * No depth buffer, triangle rasterizer or GPU is required/claimed. */
int ol_material_context_create(unsigned width, unsigned height, struct ol_material_context **);
void ol_material_context_destroy(struct ol_material_context *);
int ol_material_render(struct ol_material_context *, const struct ol_material *,
                       const struct ol3d_bindings *, struct ol_surface *output,
                       struct ol_shader_error *);
#endif
