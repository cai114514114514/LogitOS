#ifndef OLS_INTERNAL_H
#define OLS_INTERNAL_H
#include "openlogit_3d.h"
struct ols_output {
    float position[4], varying[OL3D_MAX_VARYINGS][4], color[4];
};
int ols_run(const struct ol_shader_program *, const float input[][4], unsigned inputs,
            const struct ol3d_bindings *, struct ols_output *);
#endif
