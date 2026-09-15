#include "lsl_internal.h"

#include <string.h>

struct lsl_symbol *lsl_find_symbol(struct lsl_parser *parser, const char *name)
{
    for (unsigned index = 0; index < parser->symbol_count; index++) {
        if (strcmp(name, parser->symbols[index].binding.name) == 0)
            return &parser->symbols[index];
    }
    return NULL;
}

struct lsl_symbol *lsl_add_symbol(struct lsl_parser *parser, const char *name,
                                  unsigned storage, int type, unsigned location)
{
    if (lsl_find_symbol(parser, name)) {
        lsl_fail(parser, "duplicate symbol name");
        return NULL;
    }
    if (parser->symbol_count == LSL_MAX_SYMBOLS) {
        lsl_fail(parser, "symbol budget exhausted");
        return NULL;
    }
    struct lsl_symbol *symbol = &parser->symbols[parser->symbol_count++];
    *symbol = (struct lsl_symbol){
        .binding = {.storage = storage, .type = type, .location = location},
        .register_index = -1
    };
    memcpy(symbol->binding.name, name, strlen(name) + 1);
    return symbol;
}

struct lsl_value lsl_read_symbol(struct lsl_parser *parser, const char *name)
{
    struct lsl_symbol *symbol = lsl_find_symbol(parser, name);
    if (!symbol) {
        lsl_fail(parser, "unknown symbol");
        return (struct lsl_value){0};
    }
    unsigned type = symbol->binding.type;
    unsigned storage = symbol->binding.storage;
    unsigned location = symbol->binding.location;
    if (type == LSL_MAT4 || type == LSL_SAMPLER2D)
        return (struct lsl_value){.type = type, .binding_slot = location};

    if (storage == LSL_INPUT || storage == LSL_UNIFORM) {
        unsigned opcode = storage == LSL_INPUT ? OLS_INPUT : OLS_UNIFORM;
        struct lsl_value value = lsl_emit_instruction(parser, type, opcode, location, 0, 0);
        if (type == LSL_FLOAT) {
            struct lsl_value broadcast = lsl_emit_instruction(
                parser, type, OLS_SWIZZLE, value.register_index, 0, 0);
            lsl_release_value(parser, value);
            return broadcast;
        }
        return value;
    }
    if (!symbol->initialized) {
        lsl_fail(parser, "symbol read before assignment");
        return (struct lsl_value){0};
    }

    /* Expressions own their temporaries. Copy the local so evaluating x + x
     * cannot release x's persistent register before a later statement reads it. */
    return lsl_emit_instruction(parser, type, OLS_MOV, symbol->register_index, 0, 0);
}

int lsl_assign_symbol(struct lsl_parser *parser, struct lsl_symbol *symbol,
                       struct lsl_value value)
{
    if (!symbol) {
        lsl_fail(parser, "assignment to unknown symbol");
    } else if (symbol->binding.storage == LSL_INPUT ||
               symbol->binding.storage == LSL_UNIFORM) {
        lsl_fail(parser, "input and uniform bindings are read-only");
    } else if (symbol->binding.type != (unsigned)value.type) {
        lsl_fail(parser, "assignment types do not match");
    }
    if (parser->failed) {
        lsl_release_value(parser, value);
        return 0;
    }

    if (symbol->initialized) {
        struct lsl_value previous = {
            .type = symbol->binding.type,
            .register_index = symbol->register_index
        };
        lsl_release_value(parser, previous);
    }
    symbol->register_index = value.register_index;
    symbol->initialized = 1;
    return 1;
}
