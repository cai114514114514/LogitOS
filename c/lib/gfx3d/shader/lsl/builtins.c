#include "lsl_internal.h"

#include <string.h>

static struct lsl_value emit_texture(struct lsl_parser *parser,
                                    const struct lsl_value *arguments,
                                    unsigned argument_count)
{
    if (argument_count != 2 || arguments[0].type != LSL_SAMPLER2D ||
        arguments[1].type != LSL_VEC2 || parser->shader->ir.stage != LSL_FRAGMENT) {
        lsl_fail(parser, "texture requires fragment sampler2D and vec2");
        return (struct lsl_value){0};
    }
    return lsl_emit_instruction(parser, LSL_VEC4, OLS_TEX2D,
                                arguments[1].register_index,
                                arguments[0].binding_slot, 0);
}

static struct lsl_value emit_dot(struct lsl_parser *parser,
                                struct lsl_value left, struct lsl_value right)
{
    struct lsl_value masked = {0};
    if (left.type == LSL_VEC2) {
        /* There is no DOT2 opcode. Remove invisible lanes before DOT3 rather
         * than requiring every vector producer to zero all padding lanes. */
        struct lsl_value mask = lsl_emit_vector(parser, 1, 1, 0, 0);
        masked = lsl_emit_instruction(parser, LSL_VEC2, OLS_MUL,
                                      left.register_index, mask.register_index, 0);
        lsl_release_value(parser, mask);
        left = masked;
    }
    unsigned opcode = left.type == LSL_VEC4 ? OLS_DOT4 : OLS_DOT3;
    struct lsl_value result = lsl_emit_instruction(
        parser, LSL_FLOAT, opcode, left.register_index, right.register_index, 0);
    lsl_release_value(parser, masked);
    return result;
}

static struct lsl_value emit_vector_function(struct lsl_parser *parser,
                                            const char *name,
                                            const struct lsl_value *arguments,
                                            unsigned argument_count)
{
    int normalize = strcmp(name, "normalize") == 0;
    unsigned required = normalize ? 1 : 2;
    if (argument_count != required || arguments[0].type < LSL_VEC2 ||
        arguments[0].type > LSL_VEC4 ||
        (!normalize && arguments[0].type != arguments[1].type)) {
        lsl_fail(parser, "dot and normalize require matching vectors");
        return (struct lsl_value){0};
    }

    struct lsl_value right = normalize ? arguments[0] : arguments[1];
    struct lsl_value product = emit_dot(parser, arguments[0], right);
    if (!normalize)
        return product;

    struct lsl_value inverse_length = lsl_emit_instruction(
        parser, LSL_FLOAT, OLS_RSQRT, product.register_index, 0, 0);
    struct lsl_value result = lsl_emit_instruction(
        parser, arguments[0].type, OLS_MUL, arguments[0].register_index,
        inverse_length.register_index, 0);
    lsl_release_value(parser, inverse_length);
    lsl_release_value(parser, product);
    return result;
}

static struct lsl_value emit_unary_function(struct lsl_parser *parser,
                                           const char *name,
                                           const struct lsl_value *arguments,
                                           unsigned argument_count)
{
    if (argument_count != 1 || !lsl_is_numeric(arguments[0].type)) {
        lsl_fail(parser, "unary function requires one numeric value");
        return (struct lsl_value){0};
    }
    struct lsl_value value = arguments[0];
    if (strcmp(name, "abs") != 0) {
        unsigned opcode = strcmp(name, "sin") == 0 ? OLS_SIN : OLS_COS;
        return lsl_emit_instruction(parser, value.type, opcode,
                                    value.register_index, 0, 0);
    }

    struct lsl_value zero = lsl_emit_scalar(parser, 0);
    struct lsl_value negative = lsl_emit_instruction(
        parser, value.type, OLS_SUB, zero.register_index, value.register_index, 0);
    struct lsl_value result = lsl_emit_instruction(
        parser, value.type, OLS_MAX, value.register_index, negative.register_index, 0);
    lsl_release_value(parser, zero);
    lsl_release_value(parser, negative);
    return result;
}

static struct lsl_value emit_binary_function(struct lsl_parser *parser,
                                            const char *name,
                                            const struct lsl_value *arguments,
                                            unsigned argument_count)
{
    if (argument_count != 2) {
        lsl_fail(parser, "binary function requires two arguments");
        return (struct lsl_value){0};
    }
    int type = lsl_common_numeric_type(parser, arguments[0].type, arguments[1].type);
    if (!type)
        return (struct lsl_value){0};

    if (strcmp(name, "step") != 0) {
        unsigned opcode = strcmp(name, "min") == 0 ? OLS_MIN : OLS_MAX;
        return lsl_emit_instruction(parser, type, opcode,
                                    arguments[0].register_index,
                                    arguments[1].register_index, 0);
    }
    struct lsl_value difference = lsl_emit_instruction(
        parser, type, OLS_SUB, arguments[1].register_index,
        arguments[0].register_index, 0);
    struct lsl_value one = lsl_emit_scalar(parser, 1);
    struct lsl_value zero = lsl_emit_scalar(parser, 0);
    struct lsl_value result = lsl_emit_instruction(
        parser, type, OLS_SELECT, difference.register_index,
        one.register_index, zero.register_index);
    lsl_release_value(parser, difference);
    lsl_release_value(parser, one);
    lsl_release_value(parser, zero);
    return result;
}

static struct lsl_value emit_ternary_function(struct lsl_parser *parser,
                                             const char *name,
                                             const struct lsl_value *arguments,
                                             unsigned argument_count)
{
    if (argument_count != 3) {
        lsl_fail(parser, "clamp and mix require three arguments");
        return (struct lsl_value){0};
    }
    int type = lsl_common_numeric_type(parser, arguments[0].type, arguments[1].type);
    if (!type)
        return (struct lsl_value){0};
    type = lsl_common_numeric_type(parser, type, arguments[2].type);
    if (!type)
        return (struct lsl_value){0};

    struct lsl_value intermediate;
    struct lsl_value result;
    if (strcmp(name, "clamp") == 0) {
        intermediate = lsl_emit_instruction(
            parser, type, OLS_MAX, arguments[0].register_index,
            arguments[1].register_index, 0);
        result = lsl_emit_instruction(parser, type, OLS_MIN,
                                      intermediate.register_index,
                                      arguments[2].register_index, 0);
    } else {
        intermediate = lsl_emit_instruction(
            parser, type, OLS_SUB, arguments[1].register_index,
            arguments[0].register_index, 0);
        result = lsl_emit_instruction(parser, type, OLS_MAD,
                                      intermediate.register_index,
                                      arguments[2].register_index,
                                      arguments[0].register_index);
    }
    lsl_release_value(parser, intermediate);
    return result;
}

static struct lsl_value emit_selection(struct lsl_parser *parser,
                                       const struct lsl_value *arguments,
                                       unsigned argument_count)
{
    if (argument_count != 3 || arguments[0].type != LSL_BOOL) {
        lsl_fail(parser, "select requires a bool condition and two numeric values");
        return (struct lsl_value){0};
    }
    int type = lsl_common_numeric_type(parser, arguments[1].type, arguments[2].type);
    if (!type)
        return (struct lsl_value){0};

    /* LSL bool is 0/1; native SELECT chooses its first value at >= 0. Move
     * false below zero while retaining the sign of true. Arguments are eager. */
    struct lsl_value threshold = lsl_emit_scalar(parser, .5f);
    struct lsl_value condition = lsl_emit_instruction(
        parser, LSL_FLOAT, OLS_SUB, arguments[0].register_index,
        threshold.register_index, 0);
    struct lsl_value result = lsl_emit_instruction(
        parser, type, OLS_SELECT, condition.register_index,
        arguments[1].register_index, arguments[2].register_index);
    lsl_release_value(parser, threshold);
    lsl_release_value(parser, condition);
    return result;
}

struct lsl_value lsl_emit_call(struct lsl_parser *parser, const char *name,
                             struct lsl_value *arguments, unsigned argument_count)
{
    struct lsl_value result = {0};
    int constructor_type = lsl_type_from_name(name);

    if (parser->failed) {
        /* Still release the successfully parsed arguments on error. */
    } else if (constructor_type) {
        result = lsl_emit_constructor(parser, constructor_type, arguments, argument_count);
    } else if (strcmp(name, "texture") == 0) {
        result = emit_texture(parser, arguments, argument_count);
    } else if (strcmp(name, "select") == 0) {
        result = emit_selection(parser, arguments, argument_count);
    } else if (strcmp(name, "dot") == 0 || strcmp(name, "normalize") == 0) {
        result = emit_vector_function(parser, name, arguments, argument_count);
    } else if (strcmp(name, "sin") == 0 || strcmp(name, "cos") == 0 ||
               strcmp(name, "abs") == 0) {
        result = emit_unary_function(parser, name, arguments, argument_count);
    } else if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0 ||
               strcmp(name, "step") == 0) {
        result = emit_binary_function(parser, name, arguments, argument_count);
    } else if (strcmp(name, "clamp") == 0 || strcmp(name, "mix") == 0) {
        result = emit_ternary_function(parser, name, arguments, argument_count);
    } else {
        lsl_fail(parser, "unknown or unsupported function");
    }

    for (unsigned index = 0; index < argument_count; index++)
        lsl_release_value(parser, arguments[index]);
    return result;
}
