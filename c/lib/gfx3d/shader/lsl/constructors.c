#include "lsl_internal.h"

static struct lsl_value append_component(struct lsl_parser *parser, int type,
                                         struct lsl_value vector,
                                         struct lsl_value argument,
                                         unsigned source_lane,
                                         unsigned destination_lane)
{
    /* Each pair of swizzle bits selects one source lane. Replication produces
     * a scalar, then a one-hot mask writes exactly one destination component. */
    unsigned swizzle = source_lane | (source_lane << 2) |
                       (source_lane << 4) | (source_lane << 6);
    struct lsl_value component = lsl_emit_instruction(
        parser, LSL_FLOAT, OLS_SWIZZLE, argument.register_index, swizzle, 0);
    struct lsl_value mask = lsl_emit_vector(
        parser, destination_lane == 0, destination_lane == 1,
        destination_lane == 2, destination_lane == 3);
    struct lsl_value result = lsl_emit_instruction(
        parser, type, OLS_MAD, component.register_index,
        mask.register_index, vector.register_index);
    lsl_release_value(parser, component);
    lsl_release_value(parser, mask);
    lsl_release_value(parser, vector);
    return result;
}

struct lsl_value lsl_emit_constructor(struct lsl_parser *parser, int type,
                                    const struct lsl_value *arguments,
                                    unsigned argument_count)
{
    if (!lsl_is_numeric(type)) {
        lsl_fail(parser, "only float and vector constructors are supported");
        return (struct lsl_value){0};
    }

    unsigned component_count = 0;
    for (unsigned index = 0; index < argument_count; index++) {
        if (!lsl_is_numeric(arguments[index].type)) {
            lsl_fail(parser, "constructor requires numeric values");
            return (struct lsl_value){0};
        }
        component_count += arguments[index].type;
    }

    if (argument_count == 1 &&
        (arguments[0].type == LSL_FLOAT || arguments[0].type >= type)) {
        /* float(vecN) must replicate x as well. MOV would leave y/z/w behind
         * and silently corrupt the next scalar/vector arithmetic expression. */
        unsigned opcode = type == LSL_FLOAT || arguments[0].type == LSL_FLOAT
                        ? OLS_SWIZZLE : OLS_MOV;
        return lsl_emit_instruction(parser, type, opcode,
                                    arguments[0].register_index, 0, 0);
    }
    if (component_count != (unsigned)type) {
        lsl_fail(parser, "constructor component count mismatch");
        return (struct lsl_value){0};
    }

    struct lsl_value result = lsl_emit_vector(parser, 0, 0, 0, 0);
    unsigned destination_lane = 0;
    for (unsigned index = 0; index < argument_count && !parser->failed; index++) {
        for (int lane = 0; lane < arguments[index].type; lane++) {
            result = append_component(parser, type, result, arguments[index],
                                      lane, destination_lane++);
        }
    }
    return result;
}
