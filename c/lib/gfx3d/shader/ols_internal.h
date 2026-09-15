#ifndef OLS_INTERNAL_H
#define OLS_INTERNAL_H
#include "openlogit_3d.h"
struct ols_output {
    float position[4], varying[OL3D_MAX_VARYINGS][4], color[4];
};
int ols_run(const struct ol_shader_program *, const float input[][4], unsigned inputs,
            const struct ol3d_bindings *, struct ols_output *);
/* Shared by mesh pipelines and screen-space materials; callers validate the
 * program first. This lives with the VM so 2D materials need no 3D context. */
int ols_bindings_validate(const struct ol_shader_program *, const struct ol3d_bindings *,
                         struct ol_shader_error *);
#endif
