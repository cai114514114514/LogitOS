#include "lsl_internal.h"

#include <math.h>
#include <string.h>

static int expect_keyword(struct lsl_parser *parser, const char *name)
{
    if (!lsl_is_identifier(parser, name))
        return lsl_fail(parser, "expected declaration or main function");
    lsl_next_token(parser);
    return !parser->failed;
}

static int parse_header(struct lsl_parser *parser)
{
    lsl_next_token(parser);
    if (!expect_keyword(parser, "lsl"))
        return 0;
    if (parser->token != LSL_TOKEN_NUMBER || parser->number != 1)
        return lsl_fail(parser, "expected LSL version 1");
    lsl_next_token(parser);

    if (lsl_is_identifier(parser, "vertex"))
        parser->shader->ir.stage = LSL_VERTEX;
    else if (lsl_is_identifier(parser, "fragment"))
        parser->shader->ir.stage = LSL_FRAGMENT;
    else
        return lsl_fail(parser, "stage must be vertex or fragment");
    lsl_next_token(parser);

    if (parser->shader->ir.stage == LSL_VERTEX)
        lsl_add_symbol(parser, "gl_Position", LSL_POSITION, LSL_VEC4, 0);
    return !parser->failed;
}

static int binding_is_valid(struct lsl_parser *parser,
                            const struct ol_lsl_binding *binding, int sampler_layout)
{
    int sampler = binding->type == LSL_SAMPLER2D;
    unsigned width = binding->type == LSL_MAT4 ? 4 : 1;
    unsigned limit;

    if (sampler != sampler_layout)
        return lsl_fail(parser, "sampler uses binding; other values use location");
    if (binding->storage == LSL_UNIFORM) {
        if (!lsl_is_numeric(binding->type) && binding->type != LSL_MAT4 && !sampler)
            return lsl_fail(parser, "unsupported uniform type");
        limit = sampler ? OL3D_MAX_TEXTURES : OL3D_MAX_UNIFORMS;
    } else {
        if (!lsl_is_numeric(binding->type))
            return lsl_fail(parser, "stage interfaces require float or vector types");
        limit = binding->storage == LSL_INPUT && parser->shader->ir.stage == LSL_VERTEX
              ? OL3D_MAX_ATTRIBUTES : OL3D_MAX_VARYINGS;
        if (binding->storage == LSL_OUTPUT && parser->shader->ir.stage == LSL_FRAGMENT) {
            if (binding->type != LSL_VEC4 || binding->location != 0)
                return lsl_fail(parser, "fragment output must be vec4 at location 0");
        }
    }
    if (binding->location >= limit || width > limit - binding->location)
        return lsl_fail(parser, "binding range exceeds stage limit");

    for (unsigned index = 0; index < parser->shader->binding_count; index++) {
        const struct ol_lsl_binding *existing = &parser->shader->bindings[index];
        if (existing->storage != binding->storage ||
            (existing->type == LSL_SAMPLER2D) != sampler)
            continue;
        unsigned existing_width = existing->type == LSL_MAT4 ? 4 : 1;
        if (binding->location < existing->location + existing_width &&
            existing->location < binding->location + width)
            return lsl_fail(parser, "binding ranges overlap");
    }
    return 1;
}

static int parse_binding(struct lsl_parser *parser)
{
    if (!expect_keyword(parser, "layout") || !lsl_expect_token(parser, '('))
        return 0;
    int sampler_layout = lsl_is_identifier(parser, "binding");
    if (!sampler_layout && !lsl_is_identifier(parser, "location"))
        return lsl_fail(parser, "layout requires location or binding");
    lsl_next_token(parser);
    if (!lsl_expect_token(parser, '='))
        return 0;
    if (parser->token != LSL_TOKEN_NUMBER || parser->number < 0 ||
        parser->number >= OL3D_MAX_UNIFORMS || floorf(parser->number) != parser->number)
        return lsl_fail(parser, "layout index must be an in-range integer");
    unsigned location = (unsigned)parser->number;
    lsl_next_token(parser);
    if (!lsl_expect_token(parser, ')'))
        return 0;

    unsigned storage;
    if (lsl_is_identifier(parser, "in"))
        storage = LSL_INPUT;
    else if (lsl_is_identifier(parser, "out"))
        storage = LSL_OUTPUT;
    else if (lsl_is_identifier(parser, "uniform"))
        storage = LSL_UNIFORM;
    else
        return lsl_fail(parser, "expected in, out or uniform");
    lsl_next_token(parser);
    int type = lsl_type_from_name(parser->identifier);
    if (parser->token != LSL_TOKEN_IDENTIFIER || !type)
        return lsl_fail(parser, "unknown binding type");
    lsl_next_token(parser);
    if (parser->token != LSL_TOKEN_IDENTIFIER)
        return lsl_fail(parser, "binding requires a name");
    if (strncmp(parser->identifier, "gl_", 3) == 0)
        return lsl_fail(parser, "gl_ names are reserved for stage built-ins");

    struct ol_lsl_binding binding = {.storage = storage, .type = type, .location = location};
    memcpy(binding.name, parser->identifier, sizeof binding.name);
    if (!binding_is_valid(parser, &binding, sampler_layout))
        return 0;
    if (parser->shader->binding_count == LSL_MAX_BINDINGS)
        return lsl_fail(parser, "binding budget exhausted");
    if (!lsl_add_symbol(parser, binding.name, storage, type, location))
        return 0;
    parser->shader->bindings[parser->shader->binding_count++] = binding;
    lsl_next_token(parser);
    return lsl_expect_token(parser, ';');
}

static int parse_statement(struct lsl_parser *parser)
{
    int type = lsl_type_from_name(parser->identifier);
    if (parser->token != LSL_TOKEN_IDENTIFIER)
        return lsl_fail(parser, "expected initialized local or assignment");
    if (type) {
        if (!lsl_is_numeric(type) && type != LSL_BOOL)
            return lsl_fail(parser, "matrices and samplers must be uniform bindings");
        lsl_next_token(parser);
    }
    if (parser->token != LSL_TOKEN_IDENTIFIER)
        return lsl_fail(parser, "expected variable name");

    struct lsl_symbol *symbol;
    if (type) {
        if (strncmp(parser->identifier, "gl_", 3) == 0)
            return lsl_fail(parser, "gl_ names are reserved for stage built-ins");
        symbol = lsl_add_symbol(parser, parser->identifier, LSL_LOCAL, type, 0);
    } else {
        symbol = lsl_find_symbol(parser, parser->identifier);
        if (!symbol)
            return lsl_fail(parser, "unknown assignment target or unsupported statement");
    }
    if (!symbol)
        return 0;
    lsl_next_token(parser);
    if (!lsl_expect_token(parser, '='))
        return 0;
    struct lsl_value value = lsl_parse_expression(parser, 1);
    if (!lsl_assign_symbol(parser, symbol, value))
        return 0;
    return lsl_expect_token(parser, ';');
}

static int emit_stage_outputs(struct lsl_parser *parser)
{
    unsigned output_count = 0;
    for (unsigned index = 0; index < parser->symbol_count; index++) {
        struct lsl_symbol *symbol = &parser->symbols[index];
        unsigned storage = symbol->binding.storage;
        if (storage != LSL_OUTPUT && storage != LSL_POSITION)
            continue;
        if (!symbol->initialized)
            return lsl_fail(parser, "stage output is never assigned");

        unsigned opcode;
        if (storage == LSL_POSITION)
            opcode = OLS_POSITION;
        else if (parser->shader->ir.stage == LSL_FRAGMENT)
            opcode = OLS_COLOR;
        else
            opcode = OLS_VARYING;
        lsl_emit_output(parser, opcode, symbol->binding.location, symbol->register_index);
        output_count++;
    }
    if (!output_count)
        return lsl_fail(parser, "stage requires an output");
    return !parser->failed;
}

int lsl_parse_program(struct lsl_parser *parser)
{
    if (!parse_header(parser))
        return 0;
    while (!parser->failed && lsl_is_identifier(parser, "layout")) {
        if (!parse_binding(parser))
            return 0;
    }
    if (!expect_keyword(parser, "void") || !expect_keyword(parser, "main") ||
        !lsl_expect_token(parser, '(') || !lsl_expect_token(parser, ')') ||
        !lsl_expect_token(parser, '{'))
        return 0;
    while (!parser->failed && parser->token != '}') {
        if (!parse_statement(parser))
            return 0;
    }
    if (!lsl_expect_token(parser, '}') || !lsl_expect_token(parser, LSL_TOKEN_END))
        return 0;
    return emit_stage_outputs(parser);
}
