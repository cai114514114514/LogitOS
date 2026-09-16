#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "frontend/lexer.h"
#include <stdio.h> /* snprintf for error messages */

/* Self-contained char classes (no <ctype.h>/locale dependency). */
static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_alnum(char c)
{
    return is_alpha(c) || is_digit(c);
}

static int is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

typedef struct {
    Token *t;
    int n, cap;
    AsLexError *error;
    void *(*resize)(void *, size_t);
} TokBuf;

static void push(TokBuf *buffer, TokType type, const char *start, int length, int line)
{
    if (buffer->error->out_of_memory) {
        return;
    }
    if (buffer->n == buffer->cap) {
        if (buffer->cap > INT_MAX / 2) {
            buffer->error->out_of_memory = 1;
            snprintf(buffer->error->message, sizeof buffer->error->message, "too many tokens");
            return;
        }
        int capacity = buffer->cap < 64 ? 64 : buffer->cap * 2;
        Token *tokens = buffer->resize(buffer->t, (size_t)capacity * sizeof(Token));
        if (!tokens) {
            /* Keep the old allocation reachable until the error exit frees it.
             * Assigning realloc directly to t leaks that buffer on failure. */
            buffer->error->out_of_memory = 1;
            snprintf(buffer->error->message, sizeof buffer->error->message, "out of memory");
            return;
        }
        buffer->t = tokens;
        buffer->cap = capacity;
    }
    buffer->t[buffer->n++] = (Token){type, start, length, line};
}

static TokType keyword(const char *s, int n)
{
    switch (n) {
    case 2:
        if (!memcmp(s, "if", 2)) {
            return T_IF;
        }
        if (!memcmp(s, "or", 2)) {
            return T_OR;
        }
        if (!memcmp(s, "in", 2)) {
            return T_IN;
        }
        break;
    case 3:
        if (!memcmp(s, "def", 3)) {
            return T_DEF;
        }
        if (!memcmp(s, "and", 3)) {
            return T_AND;
        }
        if (!memcmp(s, "not", 3)) {
            return T_NOT;
        }
        if (!memcmp(s, "for", 3)) {
            return T_FOR;
        }
        if (!memcmp(s, "nil", 3)) {
            return T_NIL;
        }
        if (!memcmp(s, "try", 3)) {
            return T_TRY;
        }
        break;
    case 4:
        if (!memcmp(s, "with", 4)) {
            return T_WITH;
        }
        if (!memcmp(s, "elif", 4)) {
            return T_ELIF;
        }
        if (!memcmp(s, "else", 4)) {
            return T_ELSE;
        }
        if (!memcmp(s, "from", 4)) {
            return T_FROM;
        }
        if (!memcmp(s, "true", 4)) {
            return T_TRUE;
        }
        break;
    case 5:
        if (!memcmp(s, "while", 5)) {
            return T_WHILE;
        }
        if (!memcmp(s, "false", 5)) {
            return T_FALSE;
        }
        if (!memcmp(s, "class", 5)) {
            return T_CLASS;
        }
        if (!memcmp(s, "super", 5)) {
            return T_SUPER;
        }
        if (!memcmp(s, "break", 5)) {
            return T_BREAK;
        }
        if (!memcmp(s, "raise", 5)) {
            return T_RAISE;
        }
        break;
    case 6:
        if (!memcmp(s, "return", 6)) {
            return T_RETURN;
        }
        if (!memcmp(s, "import", 6)) {
            return T_IMPORT;
        }
        if (!memcmp(s, "lambda", 6)) {
            return T_LAMBDA;
        }
        if (!memcmp(s, "except", 6)) {
            return T_EXCEPT;
        }
        break;
    case 8:
        if (!memcmp(s, "continue", 8)) {
            return T_CONTINUE;
        }
        break;
    }
    return T_IDENT;
}

static Token *lex_source(const char *src, int *count, AsLexError *error,
                         void *(*resize)(void *, size_t), AsLexReport report, void *context)
{
    TokBuf b = {.error = error, .resize = resize ? resize : realloc};
    memset(error, 0, sizeof *error);
    *count = 0;
    int line = 1;
    int indent[64];
    int ni = 0;
    indent[ni++] = 0; /* indentation stack */
    int bracket = 0;  /* () [] depth -> line continuation */
    const char *p = src;
    const char *error_start = src;
    int err = 0;
    int error_line = 1;

    while (*p) {
        /* ---- logical-line start: indentation (only outside brackets) ---- */
        if (bracket == 0) {
            error_start = p;
            error_line = line;
            const char *ls = p;
            int col = 0;
            while (*p == ' ' || *p == '\t') {
                col += (*p == '\t') ? (8 - (col % 8)) : 1;
                p++;
            }
            if (*p == '\n') {
                p++;
                line++;
                continue;
            } /* blank line */
            if (*p == '#') {
                while (*p && *p != '\n') {
                    p++;
                }
                continue;
            } /* comment-only line */
            if (*p == 0) {
                break;
            }
            if (col > indent[ni - 1]) {
                if (ni >= 64) {
                    snprintf(error->message, sizeof error->message,
                             "too many indentation levels (line %d)", line);
                    err = 1;
                    goto recover_line;
                }
                indent[ni++] = col;
                push(&b, T_INDENT, ls, 0, line);
            } else {
                while (col < indent[ni - 1]) {
                    ni--;
                    push(&b, T_DEDENT, p, 0, line);
                }
                if (col != indent[ni - 1]) {
                    snprintf(error->message, sizeof error->message,
                             "inconsistent indentation (line %d)", line);
                    err = 1;
                    goto recover_line;
                }
            }
        }

        /* ---- scan tokens to the end of this logical line ---- */
        for (;;) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '#') {
                while (*p && *p != '\n') {
                    p++;
                }
            }
            if (*p == 0) {
                break;
            }
            if (*p == '\n') {
                if (bracket > 0) {
                    p++;
                    line++;
                    continue;
                }
                break;
            }

            const char *s = p;
            error_start = s;
            error_line = line;
            char c = *p;
            if (is_digit(c) || (c == '.' && is_digit(p[1]))) {
                if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
                    p += 2;
                    const char *hx = p;
                    while (is_hex(*p)) {
                        p++;
                    }
                    if (p == hx) {
                        snprintf(error->message, sizeof error->message,
                                 "'0x' needs hex digits (line %d)", line);
                        err = 1;
                    } else {
                        push(&b, T_INT, s, (int)(p - s), line);
                    }
                } else {
                    int isf = 0;
                    while (is_digit(*p)) {
                        p++;
                    }
                    if (*p == '.') {
                        isf = 1;
                        p++;
                        while (is_digit(*p)) {
                            p++;
                        }
                    }
                    if (*p == 'e' || *p == 'E') {
                        isf = 1;
                        p++;
                        if (*p == '+' || *p == '-') {
                            p++;
                        }
                        while (is_digit(*p)) {
                            p++;
                        }
                    }
                    push(&b, isf ? T_FLOAT : T_INT, s, (int)(p - s), line);
                }
            } else if (is_alpha(c)) {
                while (is_alnum(*p)) {
                    p++;
                }
                int len = (int)(p - s);
                if (len == 1 && (s[0] == 'f' || s[0] == 'F') && (*p == '"' || *p == '\'')) {
                    /* M23 f-string: emit the raw interior (holes intact) as ONE
                     * T_FSTR token; the compiler re-lexes each {hole}. The scan
                     * must treat {{ }} as literal braces, and inside a hole track
                     * ()[]{} nesting + skip string literals, so f"{ {1: 2} }" and
                     * f"{ ',' }" terminate at the right closing quote. */
                    char q = *p++;
                    const char *cs = p;
                    int depth = 0;
                    while (*p && (*p != q || depth > 0)) {
                        char ch = *p;
                        if (ch == '\n') {
                            line++;
                            p++;
                            continue;
                        }
                        if (depth == 0) {
                            if (ch == '\\' && p[1]) {
                                p += 2;
                                continue;
                            }
                            if (ch == '{' && p[1] == '{') {
                                p += 2;
                                continue;
                            }
                            if (ch == '}' && p[1] == '}') {
                                p += 2;
                                continue;
                            }
                            if (ch == '{') {
                                depth = 1;
                                p++;
                                continue;
                            }
                            p++;
                            continue;
                        }
                        if (ch == '\'' || ch == '"') { /* string literal inside the hole */
                            char hq = ch;
                            p++;
                            while (*p && *p != hq) {
                                if (*p == '\\' && p[1]) {
                                    p += 2;
                                } else {
                                    if (*p == '\n') {
                                        line++;
                                    }
                                    p++;
                                }
                            }
                            if (*p) {
                                p++;
                            }
                            continue;
                        }
                        if (ch == '{' || ch == '(' || ch == '[') {
                            depth++;
                        } else if (ch == '}' || ch == ')' || ch == ']') {
                            depth--; /* hole's } -> 0 */
                        }
                        p++;
                    }
                    if (*p != q) {
                        snprintf(error->message, sizeof error->message,
                                 "unterminated f-string (line %d)", report ? error_line : line);
                        err = 1;
                        break;
                    }
                    push(&b, T_FSTR, cs, (int)(p - cs), line);
                    p++; /* closing quote */
                } else {
                    push(&b, keyword(s, len), s, len, line);
                }
            } else if (c == '"' || c == '\'') {
                char q = c;
                p++;
                const char *cs = p;
                /* track embedded newlines so line numbers stay correct past a
                 * multi-line string literal (the old code didn't, skewing every
                 * later error/line by the number of newlines inside the string). */
                while (*p && *p != q) {
                    if (*p == '\\' && p[1]) {
                        if (p[1] == '\n') {
                            line++;
                        }
                        p += 2;
                    } else {
                        if (*p == '\n') {
                            line++;
                        }
                        p++;
                    }
                }
                if (*p != q) {
                    snprintf(error->message, sizeof error->message, "unterminated string (line %d)",
                             report ? error_line : line);
                    err = 1;
                    break;
                }
                push(&b, T_STR, cs, (int)(p - cs), line);
                p++; /* closing quote */
            } else {
                p++;
                switch (c) {
                case '(':
                    push(&b, T_LPAREN, s, 1, line);
                    bracket++;
                    break;
                case ')':
                    push(&b, T_RPAREN, s, 1, line);
                    if (bracket > 0) {
                        bracket--;
                    }
                    break;
                case '[':
                    push(&b, T_LBRACKET, s, 1, line);
                    bracket++;
                    break;
                case ']':
                    push(&b, T_RBRACKET, s, 1, line);
                    if (bracket > 0) {
                        bracket--;
                    }
                    break;
                case '{':
                    push(&b, T_LBRACE, s, 1, line);
                    bracket++;
                    break;
                case '}':
                    push(&b, T_RBRACE, s, 1, line);
                    if (bracket > 0) {
                        bracket--;
                    }
                    break;
                case ',':
                    push(&b, T_COMMA, s, 1, line);
                    break;
                case ':':
                    push(&b, T_COLON, s, 1, line);
                    break;
                case ';':
                    push(&b, T_SEMI, s, 1, line);
                    break;
                case '.':
                    push(&b, T_DOT, s, 1, line);
                    break;
                case '+':
                    if (*p == '=') {
                        p++;
                        push(&b, T_PLUSEQ, s, 2, line);
                    } else {
                        push(&b, T_PLUS, s, 1, line);
                    }
                    break;
                case '-':
                    if (*p == '=') {
                        p++;
                        push(&b, T_MINUSEQ, s, 2, line);
                    } else if (*p == '>') {
                        p++;
                        push(&b, T_ARROW, s, 2, line);
                    } /* M27 redirect */
                    else {
                        push(&b, T_MINUS, s, 1, line);
                    }
                    break;
                case '*':
                    if (*p == '*') {
                        p++;
                        push(&b, T_POW, s, 2, line);
                    } else if (*p == '=') {
                        p++;
                        push(&b, T_STAREQ, s, 2, line);
                    } else {
                        push(&b, T_STAR, s, 1, line);
                    }
                    break;
                case '/':
                    if (*p == '=') {
                        p++;
                        push(&b, T_SLASHEQ, s, 2, line);
                    } else {
                        push(&b, T_SLASH, s, 1, line);
                    }
                    break;
                case '%':
                    if (*p == '=') {
                        p++;
                        push(&b, T_PERCENTEQ, s, 2, line);
                    } else {
                        push(&b, T_PERCENT, s, 1, line);
                    }
                    break;
                case '&':
                    push(&b, T_AMP, s, 1, line);
                    break;
                case '|':
                    if (*p == '>') {
                        p++;
                        push(&b, T_PIPEOP, s, 2, line);
                    } /* M27 pipeline */
                    else {
                        push(&b, T_PIPE, s, 1, line);
                    }
                    break;
                case '^':
                    push(&b, T_CARET, s, 1, line);
                    break;
                case '~':
                    push(&b, T_TILDE, s, 1, line);
                    break;
                case '=':
                    if (*p == '=') {
                        p++;
                        push(&b, T_EQ, s, 2, line);
                    } else {
                        push(&b, T_ASSIGN, s, 1, line);
                    }
                    break;
                case '!':
                    if (*p == '=') {
                        p++;
                        push(&b, T_NE, s, 2, line);
                    } else {
                        snprintf(error->message, sizeof error->message, "unexpected '!' (line %d)",
                                 line);
                        err = 1;
                    }
                    break;
                case '<':
                    if (*p == '=') {
                        p++;
                        push(&b, T_LE, s, 2, line);
                    } else if (*p == '<') {
                        p++;
                        push(&b, T_SHL, s, 2, line);
                    } else if (*p == '-') {
                        p++;
                        push(&b, T_LARROW, s, 2, line);
                    } /* M27 redirect in */
                    else {
                        push(&b, T_LT, s, 1, line);
                    }
                    break;
                case '>':
                    if (*p == '=') {
                        p++;
                        push(&b, T_GE, s, 2, line);
                    } else if (*p == '>') {
                        p++;
                        push(&b, T_SHR, s, 2, line);
                    } else {
                        push(&b, T_GT, s, 1, line);
                    }
                    break;
                default:
                    if ((unsigned char)c >= 128) {
                        /* A single byte of a UTF-8 character is not a valid
                         * UTF-8 diagnostic. Keep JSON readable when identifiers
                         * contain characters this lexer does not yet accept. */
                        snprintf(error->message, sizeof error->message,
                                 "unexpected non-ASCII character (line %d)", line);
                    } else {
                        snprintf(error->message, sizeof error->message,
                                 "unexpected character '%c' (line %d)", c, line);
                    }
                    err = 1;
                    break;
                }
            }
            if (err || error->out_of_memory) {
                break;
            }
        }
    recover_line:
        if (err && report && !error->out_of_memory) {
            /* A failed multiline string may have scanned to EOF. Rewind only
             * after failure is known; valid multiline strings keep their old
             * meaning. Resume on the next physical line with a fresh bracket
             * depth so one broken expression cannot swallow later definitions. */
            p = error_start + strcspn(error_start, "\n");
            line = error_line;
            Token site = {T_ERROR, error_start, (int)(p - error_start), error_line};
            report(context, site, error->message);
            push(&b, T_ERROR, site.start, site.len, site.line);
            bracket = 0;
            err = 0;
        }
        if (err || error->out_of_memory) {
            break;
        }

        if (bracket == 0 && *p == '\n') {
            push(&b, T_NEWLINE, p, 0, line);
            p++;
            line++;
        } else if (*p == 0) {
            break;
        }
    }

    if (err || error->out_of_memory) {
        error->start = error_start;
        free(b.t);
        return NULL;
    } /* error->out_of_memory: error->message already "out of memory" */

    if (b.n > 0 && b.t[b.n - 1].type != T_NEWLINE) {
        push(&b, T_NEWLINE, p, 0, line);
    }
    while (ni > 1) {
        ni--;
        push(&b, T_DEDENT, p, 0, line);
    }
    push(&b, T_EOF, p, 0, line);
    if (error->out_of_memory) {
        error->start = p;
        free(b.t);
        return NULL;
    } /* OOM in the trailing pushes */
    *count = b.n;
    return b.t;
}

Token *as_lex_source(const char *source, int *count, AsLexError *error,
                     void *(*resize)(void *, size_t))
{
    return lex_source(source, count, error, resize, NULL, NULL);
}

Token *as_lex_recover(const char *source, int *count, AsLexError *error,
                      AsLexReport report, void *context, void *(*resize)(void *, size_t))
{
    return lex_source(source, count, error, resize, report, context);
}
