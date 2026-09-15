#include "lsl_internal.h"

static int is_comparison(int operator)
{
    return operator == '<' || operator == '>' ||
           operator == LSL_TOKEN_GREATER_EQUAL ||
           operator == LSL_TOKEN_LESS_EQUAL ||
           operator == LSL_TOKEN_EQUAL || operator == LSL_TOKEN_NOT_EQUAL;
}

static struct lsl_value emit_comparison(struct lsl_parser *parser, int operator,
                                       struct lsl_value left,
                                       struct lsl_value right)
{
    if (left.type != LSL_FLOAT || right.type != LSL_FLOAT) {
        lsl_fail(parser, "comparisons require scalar floats");
        return (struct lsl_value){0};
    }

    struct lsl_value difference = lsl_emit_instruction(
        parser, LSL_FLOAT, OLS_SUB, left.register_index, right.register_index, 0);
    struct lsl_value reverse = lsl_emit_instruction(
        parser, LSL_FLOAT, OLS_SUB, right.register_index, left.register_index, 0);
    struct lsl_value one = lsl_emit_scalar(parser, 1);
    struct lsl_value zero = lsl_emit_scalar(parser, 0);
    struct lsl_value greater_equal = lsl_emit_instruction(
        parser, LSL_BOOL, OLS_SELECT, difference.register_index,
        one.register_index, zero.register_index);
    struct lsl_value less_equal = lsl_emit_instruction(
        parser, LSL_BOOL, OLS_SELECT, reverse.register_index,
        one.register_index, zero.register_index);
    struct lsl_value result;

    /* SELECT includes zero. Strict comparisons are the complement of the
     * opposite inclusive comparison, so equality is never treated as true. */
    if (operator == LSL_TOKEN_GREATER_EQUAL || operator == LSL_TOKEN_LESS_EQUAL) {
        int selected = operator == LSL_TOKEN_GREATER_EQUAL
                     ? greater_equal.register_index : less_equal.register_index;
        result = lsl_emit_instruction(parser, LSL_BOOL, OLS_MOV, selected, 0, 0);
    } else if (operator == '<' || operator == '>') {
        int opposite = operator == '<'
                     ? greater_equal.register_index : less_equal.register_index;
        result = lsl_emit_instruction(parser, LSL_BOOL, OLS_SUB,
                                      one.register_index, opposite, 0);
    } else {
        result = lsl_emit_instruction(parser, LSL_BOOL, OLS_MUL,
                                      greater_equal.register_index,
                                      less_equal.register_index, 0);
        if (operator == LSL_TOKEN_NOT_EQUAL) {
            struct lsl_value equal = result;
            result = lsl_emit_instruction(parser, LSL_BOOL, OLS_SUB,
                                          one.register_index,
                                          equal.register_index, 0);
            lsl_release_value(parser, equal);
        }
    }

    lsl_release_value(parser, difference);
    lsl_release_value(parser, reverse);
    lsl_release_value(parser, one);
    lsl_release_value(parser, zero);
    lsl_release_value(parser, greater_equal);
    lsl_release_value(parser, less_equal);
    return result;
}

static struct lsl_value emit_reciprocal(struct lsl_parser *parser,
                                       struct lsl_value divisor)
{
    if (divisor.type == LSL_FLOAT || divisor.type == LSL_VEC4)
        return lsl_emit_instruction(parser, divisor.type, OLS_RCP,
                                    divisor.register_index, 0, 0);

    /* The VM evaluates all four lanes. Pad unused vector lanes with one before
     * RCP; a vec2 division must not fail because its invisible z/w lanes are 0. */
    struct lsl_value mask = lsl_emit_vector(parser, 1, 1,
                                           divisor.type == LSL_VEC3, 0);
    struct lsl_value padding = lsl_emit_vector(parser, 0, 0,
                                              divisor.type == LSL_VEC2, 1);
    struct lsl_value padded = lsl_emit_instruction(
        parser, LSL_VEC4, OLS_MAD, divisor.register_index,
        mask.register_index, padding.register_index);
    struct lsl_value reciprocal = lsl_emit_instruction(
        parser, divisor.type, OLS_RCP, padded.register_index, 0, 0);

    lsl_release_value(parser, mask);
    lsl_release_value(parser, padding);
    lsl_release_value(parser, padded);
    return reciprocal;
}

static struct lsl_value emit_arithmetic(struct lsl_parser *parser, int operator,
                                       struct lsl_value left,
                                       struct lsl_value right)
{
    int type = lsl_common_numeric_type(parser, left.type, right.type);
    if (!type)
        return (struct lsl_value){0};

    if (operator == '/') {
        struct lsl_value reciprocal = emit_reciprocal(parser, right);
        struct lsl_value result = lsl_emit_instruction(
            parser, type, OLS_MUL, left.register_index,
            reciprocal.register_index, 0);
        lsl_release_value(parser, reciprocal);
        return result;
    }

    unsigned opcode;
    switch (operator) {
    case '+': opcode = OLS_ADD; break;
    case '-': opcode = OLS_SUB; break;
    case '*': opcode = OLS_MUL; break;
    default:
        lsl_fail(parser, "unsupported binary operator");
        return (struct lsl_value){0};
    }
    return lsl_emit_instruction(parser, type, opcode,
                                left.register_index, right.register_index, 0);
}

struct lsl_value lsl_emit_binary(struct lsl_parser *parser, int operator,
                               struct lsl_value left, struct lsl_value right)
{
    struct lsl_value result = {0};
    if (!parser->failed) {
        if (operator == '*' && left.type == LSL_MAT4 && right.type == LSL_VEC4) {
            result = lsl_emit_instruction(parser, LSL_VEC4, OLS_MAT4,
                                          right.register_index,
                                          left.binding_slot, 0);
        } else if (is_comparison(operator)) {
            result = emit_comparison(parser, operator, left, right);
        } else {
            result = emit_arithmetic(parser, operator, left, right);
        }
    }
    lsl_release_value(parser, left);
    lsl_release_value(parser, right);
    return result;
}
