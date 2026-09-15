#include "lsl_internal.h"

#include <string.h>

static int binary_precedence(int token)
{
    switch (token) {
    case LSL_TOKEN_EQUAL:
    case LSL_TOKEN_NOT_EQUAL:
        return 1;
    case '<':
    case '>':
    case LSL_TOKEN_GREATER_EQUAL:
    case LSL_TOKEN_LESS_EQUAL:
        return 2;
    case '+':
    case '-':
        return 3;
    case '*':
    case '/':
        return 4;
    default:
        return 0;
    }
}

static struct lsl_value parse_call(struct lsl_parser *parser, const char *name)
{
    struct lsl_value arguments[4] = {{0}};
    unsigned count = 0;
    lsl_expect_token(parser, '(');

    while (!parser->failed && parser->token != ')') {
        if (count == sizeof arguments / sizeof arguments[0]) {
            lsl_fail(parser, "function accepts at most four arguments");
            break;
        }
        arguments[count++] = lsl_parse_expression(parser, 1);
        if (parser->token != ',')
            break;
        lsl_next_token(parser);
        if (parser->token == ')')
            lsl_fail(parser, "missing argument after comma");
    }
    lsl_expect_token(parser, ')');
    return lsl_emit_call(parser, name, arguments, count);
}

static struct lsl_value parse_primary(struct lsl_parser *parser)
{
    if (parser->token == LSL_TOKEN_NUMBER) {
        struct lsl_value value = lsl_emit_scalar(parser, parser->number);
        lsl_next_token(parser);
        return value;
    }
    if (parser->token == '(') {
        lsl_next_token(parser);
        struct lsl_value value = lsl_parse_expression(parser, 1);
        lsl_expect_token(parser, ')');
        return value;
    }
    if (parser->token == '+' || parser->token == '-') {
        int negate = parser->token == '-';
        lsl_next_token(parser);
        struct lsl_value value = lsl_parse_expression(parser, 5);
        if (!lsl_is_numeric(value.type))
            lsl_fail(parser, "unary sign requires a numeric value");
        if (!negate)
            return value;
        struct lsl_value zero = lsl_emit_scalar(parser, 0);
        return lsl_emit_binary(parser, '-', zero, value);
    }
    if (parser->token == LSL_TOKEN_IDENTIFIER) {
        char name[sizeof parser->identifier];
        memcpy(name, parser->identifier, sizeof name);
        lsl_next_token(parser);
        if (parser->token == '(')
            return parse_call(parser, name);
        if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0) {
            struct lsl_value value = lsl_emit_scalar(parser, strcmp(name, "true") == 0);
            if (!parser->failed)
                value.type = LSL_BOOL;
            return value;
        }
        return lsl_read_symbol(parser, name);
    }
    lsl_fail(parser, "expected expression");
    return (struct lsl_value){0};
}

static struct lsl_value parse_swizzle(struct lsl_parser *parser,
                                      struct lsl_value value)
{
    lsl_next_token(parser);
    unsigned count = (unsigned)strlen(parser->identifier);
    if (parser->token != LSL_TOKEN_IDENTIFIER || !lsl_is_numeric(value.type) ||
        count < 1 || count > 4) {
        lsl_fail(parser, "swizzle requires one to four vector components");
        lsl_release_value(parser, value);
        return (struct lsl_value){0};
    }

    const char *alphabet = strchr("xyzw", parser->identifier[0]) ? "xyzw" : "rgba";
    unsigned packed = 0;
    unsigned last_lane = 0;
    for (unsigned index = 0; index < 4; index++) {
        if (index < count) {
            const char *component = strchr(alphabet, parser->identifier[index]);
            if (!component || component - alphabet >= value.type) {
                lsl_fail(parser, "swizzle component is outside vector width");
                break;
            }
            last_lane = (unsigned)(component - alphabet);
        }
        /* A one-component swizzle is a scalar, so repeat its lane four times. */
        packed |= last_lane << (index * 2);
    }
    struct lsl_value result = lsl_emit_instruction(
        parser, count, OLS_SWIZZLE, value.register_index, packed, 0);
    lsl_release_value(parser, value);
    lsl_next_token(parser);
    return result;
}

struct lsl_value lsl_parse_expression(struct lsl_parser *parser, int precedence)
{
    if (parser->failed)
        return (struct lsl_value){0};
    if (parser->expression_depth == LSL_MAX_EXPRESSION_DEPTH) {
        lsl_fail(parser, "expression nesting limit exceeded");
        return (struct lsl_value){0};
    }

    parser->expression_depth++;
    struct lsl_value left = parse_primary(parser);
    while (!parser->failed && parser->token == '.')
        left = parse_swizzle(parser, left);

    while (!parser->failed) {
        int following_precedence = binary_precedence(parser->token);
        if (following_precedence < precedence)
            break;
        int operator = parser->token;
        lsl_next_token(parser);
        struct lsl_value right = lsl_parse_expression(parser, following_precedence + 1);
        left = lsl_emit_binary(parser, operator, left, right);
    }
    parser->expression_depth--;
    return left;
}
