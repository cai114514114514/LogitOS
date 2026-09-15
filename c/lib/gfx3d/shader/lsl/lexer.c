#include "lsl_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int lsl_fail(struct lsl_parser *parser, const char *message)
{
    if (!parser->failed && parser->error) {
        parser->error->line = parser->token_line;
        snprintf(parser->error->message, sizeof parser->error->message,
                 "%s", message);
    }
    parser->failed = 1;
    return 0;
}

static int is_identifier_start(int character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') || character == '_';
}

static int is_decimal_digit(int character)
{
    return character >= '0' && character <= '9';
}

static int peek_character(const struct lsl_parser *parser)
{
    if (parser->cursor == parser->source_bytes)
        return 0;
    return (unsigned char)parser->source[parser->cursor];
}

static int skip_block_comment(struct lsl_parser *parser)
{
    parser->token_line = parser->line;
    parser->cursor += 2;

    while (parser->cursor + 1 < parser->source_bytes) {
        if (parser->source[parser->cursor] == '*' &&
            parser->source[parser->cursor + 1] == '/') {
            parser->cursor += 2;
            return 1;
        }
        if (parser->source[parser->cursor++] == '\n')
            parser->line++;
    }
    return lsl_fail(parser, "unterminated block comment");
}

static int skip_trivia(struct lsl_parser *parser)
{
    for (;;) {
        int character = peek_character(parser);
        if (character == ' ' || character == '\t' ||
            character == '\r' || character == '\n') {
            parser->cursor++;
            if (character == '\n')
                parser->line++;
            continue;
        }
        if (character != '/' || parser->cursor + 1 >= parser->source_bytes)
            return 1;

        int following = parser->source[parser->cursor + 1];
        if (following == '/') {
            while (parser->cursor < parser->source_bytes &&
                   parser->source[parser->cursor] != '\n')
                parser->cursor++;
        } else if (following == '*') {
            if (!skip_block_comment(parser))
                return 0;
        } else {
            return 1;
        }
    }
}

static void read_identifier(struct lsl_parser *parser, size_t start)
{
    while (is_identifier_start(peek_character(parser)) ||
           is_decimal_digit(peek_character(parser)))
        parser->cursor++;

    size_t length = parser->cursor - start;
    if (length >= sizeof parser->identifier) {
        lsl_fail(parser, "identifier exceeds 47 bytes");
        return;
    }
    memcpy(parser->identifier, parser->source + start, length);
    parser->identifier[length] = '\0';
    parser->token = LSL_TOKEN_IDENTIFIER;
}

static void read_digits(struct lsl_parser *parser)
{
    while (is_decimal_digit(peek_character(parser)))
        parser->cursor++;
}

static void read_number(struct lsl_parser *parser, size_t start, int first)
{
    read_digits(parser);
    if (first != '.' && peek_character(parser) == '.') {
        parser->cursor++;
        read_digits(parser);
    }
    if (peek_character(parser) == 'e' || peek_character(parser) == 'E') {
        parser->cursor++;
        if (peek_character(parser) == '+' || peek_character(parser) == '-')
            parser->cursor++;
        if (!is_decimal_digit(peek_character(parser))) {
            lsl_fail(parser, "exponent requires decimal digits");
            return;
        }
        read_digits(parser);
    }

    char text[96];
    size_t length = parser->cursor - start;
    if (length >= sizeof text) {
        lsl_fail(parser, "numeric literal too long");
        return;
    }
    memcpy(text, parser->source + start, length);
    text[length] = '\0';

    char *end;
    parser->number = strtof(text, &end);
    if (*end || !isfinite(parser->number)) {
        lsl_fail(parser, "numeric literal must be finite");
        return;
    }
    if (peek_character(parser) == 'f')
        parser->cursor++;
    parser->token = LSL_TOKEN_NUMBER;
}

static int comparison_token(int character)
{
    switch (character) {
    case '>': return LSL_TOKEN_GREATER_EQUAL;
    case '<': return LSL_TOKEN_LESS_EQUAL;
    case '=': return LSL_TOKEN_EQUAL;
    case '!': return LSL_TOKEN_NOT_EQUAL;
    default: return 0;
    }
}

void lsl_next_token(struct lsl_parser *parser)
{
    parser->token = LSL_TOKEN_END;
    parser->identifier[0] = '\0';
    if (parser->failed || !skip_trivia(parser))
        return;

    parser->token_line = parser->line;
    if (parser->cursor == parser->source_bytes)
        return;

    size_t start = parser->cursor;
    int character = (unsigned char)parser->source[parser->cursor++];
    if (is_identifier_start(character)) {
        read_identifier(parser, start);
        return;
    }
    if (is_decimal_digit(character) ||
        (character == '.' && is_decimal_digit(peek_character(parser)))) {
        read_number(parser, start, character);
        return;
    }
    int comparison = comparison_token(character);
    if (comparison && peek_character(parser) == '=') {
        parser->cursor++;
        parser->token = comparison;
        return;
    }
    if (!character || !strchr("(){};,.=+-*/?:<>", character)) {
        lsl_fail(parser, "unsupported source character");
        return;
    }
    parser->token = character;
}

int lsl_is_identifier(const struct lsl_parser *parser, const char *name)
{
    return parser->token == LSL_TOKEN_IDENTIFIER &&
           strcmp(parser->identifier, name) == 0;
}

int lsl_expect_token(struct lsl_parser *parser, int token)
{
    if (parser->failed)
        return 0;
    if (parser->token != token)
        return lsl_fail(parser, "unexpected token");
    lsl_next_token(parser);
    return !parser->failed;
}
