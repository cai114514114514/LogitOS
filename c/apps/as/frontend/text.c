/* SPDX-License-Identifier: MIT */
#include "ir/model.h"

int as_token_decode(Token t, char *out)
{
    int n = 0;
    for (int i = 0; i < t.len; i++) {
        unsigned char c = (unsigned char)t.start[i];
        if (c == '\\' && i + 1 < t.len) {
            c = (unsigned char)t.start[++i];
            c = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c == '0' ? '\0' : c;
        } else if (t.type == T_FSTR && (c == '{' || c == '}') && i + 1 < t.len &&
                   (unsigned char)t.start[i + 1] == c) {
            i++;
        }
        if (out) {
            out[n] = (char)c;
        }
        n++;
    }
    return n;
}

/* Formatted strings become ordinary checked concatenations plus AN_FORMAT
 * value conversions. No generated source, implicit Any boxes or runtime
 * expression interpreter is involved. Every segment borrows the snapshot. */
typedef struct {
    AsTypedProject *project;
    int module;
    int function;
    Token text;
} TextParser;

static Token span(TextParser *parser, int begin, int end)
{
    Token token = parser->text;
    token.start += begin;
    token.len = end - begin;
    return token;
}

static AtNode *make(TextParser *parser, int kind, Token token)
{
    return at_parse_new_node(parser->project, parser->module, parser->function, kind, token);
}

static void fail(TextParser *parser, int begin, int end, const char *message)
{
    at_error(parser->project, parser->module, span(parser, begin, end), "AS3100", message);
}

static int quoted_end(TextParser *parser, int start)
{
    const char *text = parser->text.start;
    char quote = text[start++];
    while (start < parser->text.len) {
        if (text[start] == '\\' && start + 1 < parser->text.len) {
            start += 2;
        } else if (text[start++] == quote) {
            break;
        }
    }
    return start;
}

static int hole_end(TextParser *parser, int start)
{
    const char *text = parser->text.start;
    int depth = 0;
    for (int index = start; index < parser->text.len; index++) {
        char byte = text[index];
        if (byte == '\'' || byte == '"') {
            index = quoted_end(parser, index) - 1;
        } else if (byte == '{' || byte == '(' || byte == '[') {
            depth++;
        } else if (byte == ')' || byte == ']') {
            depth--;
        } else if (byte == '}') {
            if (depth == 0) {
                return index;
            }
            depth--;
        } else if (byte == ':' && depth == 0) {
            fail(parser, index, index + 1, "format spec not supported in f-string");
            return -1;
        }
    }
    fail(parser, start - 1, start, "unterminated '{' in f-string");
    return -1;
}

static AtNode *append(TextParser *parser, AtNode *result, AtNode *piece)
{
    if (!result || !piece) {
        return piece;
    }
    AtNode *joined = make(parser, AN_BINARY, piece->token);
    if (!joined) {
        return NULL;
    }
    joined->op = T_PLUS;
    joined->a = result;
    joined->b = piece;
    return joined;
}

AtNode *at_parse_fstring(AsTypedProject *project, int module, int function, Token text, int depth)
{
    TextParser parser = {project, module, function, text};
    AtNode *result = NULL;
    int index = 0;
    int pieces = 0;
    while (index < text.len && !project->oom) {
        /* Bound the desugared concatenation tree as well as nested hole
         * parsing: otherwise a flat literal could evade expression limits. */
        if (++pieces + depth > 64) {
            fail(&parser, index, index + 1, "Formatted string nesting/piece limit reached");
            return result;
        }
        int begin = index;
        if (text.start[index] != '{' || (index + 1 < text.len && text.start[index + 1] == '{')) {
            while (index < text.len) {
                char byte = text.start[index];
                if (index + 1 < text.len && (byte == '\\' || ((byte == '{' || byte == '}') &&
                                                              text.start[index + 1] == byte))) {
                    index += 2;
                } else if (byte == '{') {
                    break;
                } else {
                    index++;
                }
            }
            result = append(&parser, result, make(&parser, AN_STR, span(&parser, begin, index)));
            continue;
        }
        int end = hole_end(&parser, ++index);
        if (end < 0) {
            return result;
        }
        while (index < end && (text.start[index] == ' ' || text.start[index] == '\t' ||
                               text.start[index] == '\n' || text.start[index] == '\r')) {
            index++;
        }
        if (index == end) {
            fail(&parser, begin, end + 1, "empty expression in f-string");
            return result;
        }
        Token hole = span(&parser, index, end);
        AtNode *value = at_parse_fragment(project, module, function, hole, depth + 1);
        AtNode *formatted = make(&parser, AN_FORMAT, hole);
        if (!value || !formatted) {
            return result;
        }
        formatted->a = value;
        result = append(&parser, result, formatted);
        index = end + 1;
    }
    return result ? result : make(&parser, AN_STR, span(&parser, 0, 0));
}
