/* SPDX-License-Identifier: MIT */
#include "frontend/internal.h"
#include "runtime/integer_parse.h"
#include <stdlib.h>
#include <string.h>

static void invalid(AtParser *parser, AtNode *node, Token fallback, const char *message)
{
    at_parse_error(parser, node ? node->token : fallback, "AS3811", message);
}

static int literal_name(AtParser *parser, AtNode *node, char *out, int capacity, Token site)
{
    /* Layout metadata names are constants, not runtime string expressions.
     * Requiring an unescaped spelling also keeps ABI tags reproducible. */
    if (!node || node->kind != AN_STR || node->token.len <= 0 || node->token.len >= capacity ||
        memchr(node->token.start, '\\', (size_t)node->token.len)) {
        invalid(parser, node, site, "Layout names require a nonempty unescaped string literal");
        return 0;
    }
    at_parse_name(node->token, out, capacity);
    return 1;
}

static int literal_size(AtParser *parser, AtNode *node, int *out, Token site)
{
    int64_t value;
    if (!node || node->kind != AN_INT ||
        !at_parse_i64_exact(node->token.start, (size_t)node->token.len, &value) || value < 0 ||
        value > 65535) {
        invalid(parser, node, site,
                "Layout sizes and offsets require integer literals in 0..65535");
        return 0;
    }
    *out = (int)value;
    return 1;
}

static void field(AtParser *parser, AtType *type, AtNode *node, Token site)
{
    if (!node || node->kind != AN_ARRAY || node->count != 4) {
        invalid(parser, node, site, "A layout field is [name, offset, width, kind]");
        return;
    }
    int index = type->count;
    char name[64], kind[8];
    int offset, width;
    if (!literal_name(parser, node->args[0], name, sizeof name, site) ||
        !literal_size(parser, node->args[1], &offset, site) ||
        !literal_size(parser, node->args[2], &width, site) ||
        !literal_name(parser, node->args[3], kind, sizeof kind, site)) {
        return;
    }
    /* Use the language lexer so a keyword or punctuation cannot create an
     * apparently valid field that no source expression can ever address. */
    AsLexError error = {0};
    int count = 0;
    Token *tokens = as_lex_source(name, &count, &error, NULL);
    if (error.out_of_memory) {
        parser->p->oom = 1;
    }
    int identifier =
        tokens && count > 0 && tokens[0].type == T_IDENT && tokens[0].len == (int)strlen(name);
    free(tokens);
    if (!identifier) {
        invalid(parser, node->args[0], site, "Layout field name must be an accessible identifier");
        return;
    }
    if (index == AT_ARGS || !width || offset > type->layout_size ||
        width > type->layout_size - offset) {
        invalid(parser, node, site, "Layout field exceeds record bounds or the 32-field limit");
        return;
    }
    int scalar = width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : width == 8 ? 3 : -1;
    int value_type;
    if (!strcmp(kind, "s")) {
        value_type = AT_BYTES;
    } else if (!strcmp(kind, "p") && width == 8) {
        value_type = AT_U64;
    } else if (scalar >= 0 && (!strcmp(kind, "i") || !strcmp(kind, "u"))) {
        value_type = (!strcmp(kind, "i") ? AT_I8 : AT_U8) + scalar;
    } else {
        invalid(parser, node->args[3], site,
                "Layout kind is i/u (1,2,4,8 bytes), p (8 bytes), or s (fixed bytes)");
        return;
    }
    for (int i = 0; i < index; i++) {
        if (!strcmp(type->names[i], name)) {
            invalid(parser, node->args[0], site, "Duplicate layout field name");
            return;
        }
    }
    strcpy(type->names[index], name);
    type->fields[index] = value_type;
    type->offsets[index] = offset;
    type->widths[index] = width;
    type->count++;
}

int at_parse_layout(AtParser *parser)
{
    AtModule *module = &parser->p->modules[parser->m];
    if (parser->pos + 3 >= module->count) {
        return 0;
    }
    Token *tokens = module->tokens + parser->pos;
    if (tokens[0].type != T_IDENT || tokens[1].type != T_ASSIGN ||
        !at_parse_word(tokens[2], "layout") || tokens[3].type != T_LPAREN) {
        return 0;
    }
    Token name = at_parse_advance(parser);
    if (name.len >= (int)sizeof parser->p->types[0].name) {
        invalid(parser, NULL, name, "Layout type name exceeds the identifier limit");
    }
    at_parse_advance(parser);
    AtNode *call = at_parse_expression(parser, 1);
    /* Consume the complete expression even on invalid metadata. Normal module
     * recovery then starts after this declaration, not in its nested lists. */
    if (!call || call->kind != AN_CALL || call->count != 3 || !call->a ||
        call->a->kind != AN_NAME || !at_parse_word(call->a->token, "layout")) {
        invalid(parser, call, name, "A layout declaration requires (C name, size, fields)");
        return 1;
    }
    int id = at_allocate_type(parser->p, parser->m, name);
    if (!id) {
        return 1;
    }
    AtType *type = &parser->p->types[id];
    type->kind = AT_LAYOUT;
    type->module = parser->m;
    at_parse_name(name, type->name, sizeof type->name);
    for (int i = 1; i < id; i++) {
        AtType *previous = &parser->p->types[i];
        if (!strcmp(previous->name, type->name) &&
            (i <= AT_BUILTIN_LAST || previous->module == parser->m)) {
            invalid(parser, NULL, name, "Type is already declared in this module");
        }
    }
    if (at_exception_code(name) >= 0) {
        invalid(parser, NULL, name, "Exception type names are reserved");
    }
    if (!literal_name(parser, call->args[0], type->layout_name, sizeof type->layout_name, name) ||
        !literal_size(parser, call->args[1], &type->layout_size, name)) {
        return 1;
    }
    if (!type->layout_size) {
        invalid(parser, call->args[1], name, "A layout must occupy at least one byte");
    }
    AtNode *fields = call->args[2];
    if (!fields || fields->kind != AN_ARRAY) {
        invalid(parser, fields, name, "Layout fields must be a literal list");
        return 1;
    }
    for (int i = 0; i < fields->count; i++) {
        field(parser, type, fields->args[i], name);
    }
    return 1;
}
