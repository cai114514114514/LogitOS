#ifndef OLS_UTIL_H
#define OLS_UTIL_H
#include "ols_internal.h"
#include <math.h>
#include <stdio.h>
/* Small shared validation primitives; no compiler or rasterizer dependency. */
static inline int ols_error(struct ol_shader_error *e, unsigned line, const char *msg)
{
    if (e) {
        e->line = line;
        snprintf(e->message, sizeof e->message, "%s", msg);
    }
    return OL_ARGUMENT;
}
static inline int ols_finite4(const float v[4])
{
    for (int j = 0; j < 4; j++)
        if (!isfinite(v[j]))
            return 0;
    return 1;
}
#endif
