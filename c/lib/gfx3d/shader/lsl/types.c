#include "lsl_internal.h"

#include <string.h>

int lsl_type_from_name(const char *name)
{
    static const struct {
        const char *name;
        int type;
    } types[] = {
        {"float", LSL_FLOAT},
        {"vec2", LSL_VEC2},
        {"vec3", LSL_VEC3},
        {"vec4", LSL_VEC4},
        {"mat4", LSL_MAT4},
        {"sampler2D", LSL_SAMPLER2D},
        {"bool", LSL_BOOL}
    };

    for (unsigned index = 0; index < sizeof types / sizeof types[0]; index++) {
        if (strcmp(name, types[index].name) == 0)
            return types[index].type;
    }
    return 0;
}

int lsl_is_numeric(int type)
{
    return type >= LSL_FLOAT && type <= LSL_VEC4;
}

int lsl_common_numeric_type(struct lsl_parser *parser, int left, int right)
{
    if (!lsl_is_numeric(left) || !lsl_is_numeric(right))
        return lsl_fail(parser, "arithmetic requires numeric values");
    if (left == right)
        return left;
    if (left == LSL_FLOAT)
        return right;
    if (right == LSL_FLOAT)
        return left;
    return lsl_fail(parser, "vector widths do not match");
}
