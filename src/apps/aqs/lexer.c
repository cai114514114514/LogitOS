#include "aqs.h"
#include "lexer.h"
#include <stdio.h>      /* snprintf for error messages */

/* Self-contained char classes (no <ctype.h>/locale dependency). */
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }
static int is_hex(char c)   { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

typedef struct { Token *t; int n, cap; } TokBuf;

static void push(TokBuf *b, TokType ty, const char *start, int len, int line)
{
    if (b->n + 1 > b->cap) {
        b->cap = b->cap < 64 ? 64 : b->cap * 2;
        b->t = (Token *)realloc(b->t, b->cap * sizeof(Token));
    }
    b->t[b->n].type = ty; b->t[b->n].start = start; b->t[b->n].len = len; b->t[b->n].line = line;
    b->n++;
}

static TokType keyword(const char *s, int n)
{
    switch (n) {
    case 2: if (!memcmp(s, "if", 2)) return T_IF;
            if (!memcmp(s, "or", 2)) return T_OR;
            if (!memcmp(s, "in", 2)) return T_IN; break;
    case 3: if (!memcmp(s, "def", 3)) return T_DEF;
            if (!memcmp(s, "and", 3)) return T_AND;
            if (!memcmp(s, "not", 3)) return T_NOT;
            if (!memcmp(s, "for", 3)) return T_FOR;
            if (!memcmp(s, "nil", 3)) return T_NIL; break;
    case 4: if (!memcmp(s, "elif", 4)) return T_ELIF;
            if (!memcmp(s, "else", 4)) return T_ELSE;
            if (!memcmp(s, "from", 4)) return T_FROM;
            if (!memcmp(s, "true", 4)) return T_TRUE; break;
    case 5: if (!memcmp(s, "while", 5)) return T_WHILE;
            if (!memcmp(s, "false", 5)) return T_FALSE;
            if (!memcmp(s, "class", 5)) return T_CLASS;
            if (!memcmp(s, "super", 5)) return T_SUPER; break;
    case 6: if (!memcmp(s, "return", 6)) return T_RETURN;
            if (!memcmp(s, "import", 6)) return T_IMPORT;
            if (!memcmp(s, "lambda", 6)) return T_LAMBDA; break;
    }
    return T_IDENT;
}

Token *aqs_lex(const char *src, int *count)
{
    TokBuf b = { NULL, 0, 0 };
    int line = 1;
    int indent[64]; int ni = 0; indent[ni++] = 0;   /* indentation stack */
    int bracket = 0;                                  /* () [] depth -> line continuation */
    const char *p = src;
    int err = 0;

    while (*p) {
        /* ---- logical-line start: indentation (only outside brackets) ---- */
        if (bracket == 0) {
            const char *ls = p;
            int col = 0;
            while (*p == ' ' || *p == '\t') { col += (*p == '\t') ? (8 - (col % 8)) : 1; p++; }
            if (*p == '\n') { p++; line++; continue; }            /* blank line */
            if (*p == '#')  { while (*p && *p != '\n') p++; continue; }  /* comment-only line */
            if (*p == 0) break;
            if (col > indent[ni - 1]) {
                if (ni >= 64) { snprintf(aqs_err, sizeof aqs_err, "too many indentation levels (line %d)", line); err = 1; break; }
                indent[ni++] = col; push(&b, T_INDENT, ls, 0, line);
            } else {
                while (col < indent[ni - 1]) { ni--; push(&b, T_DEDENT, p, 0, line); }
                if (col != indent[ni - 1]) { snprintf(aqs_err, sizeof aqs_err, "inconsistent indentation (line %d)", line); err = 1; break; }
            }
        }

        /* ---- scan tokens to the end of this logical line ---- */
        for (;;) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#') { while (*p && *p != '\n') p++; }
            if (*p == 0) break;
            if (*p == '\n') { if (bracket > 0) { p++; line++; continue; } break; }

            const char *s = p;
            char c = *p;
            if (is_digit(c) || (c == '.' && is_digit(p[1]))) {
                if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
                    p += 2; while (is_hex(*p)) p++;
                    push(&b, T_INT, s, (int)(p - s), line);
                } else {
                    int isf = 0;
                    while (is_digit(*p)) p++;
                    if (*p == '.') { isf = 1; p++; while (is_digit(*p)) p++; }
                    if (*p == 'e' || *p == 'E') { isf = 1; p++; if (*p == '+' || *p == '-') p++; while (is_digit(*p)) p++; }
                    push(&b, isf ? T_FLOAT : T_INT, s, (int)(p - s), line);
                }
            } else if (is_alpha(c)) {
                while (is_alnum(*p)) p++;
                int len = (int)(p - s);
                push(&b, keyword(s, len), s, len, line);
            } else if (c == '"' || c == '\'') {
                char q = c; p++; const char *cs = p;
                while (*p && *p != q) { if (*p == '\\' && p[1]) p += 2; else p++; }
                if (*p != q) { snprintf(aqs_err, sizeof aqs_err, "unterminated string (line %d)", line); err = 1; break; }
                push(&b, T_STR, cs, (int)(p - cs), line);
                p++;   /* closing quote */
            } else {
                p++;
                switch (c) {
                case '(': push(&b, T_LPAREN, s, 1, line); bracket++; break;
                case ')': push(&b, T_RPAREN, s, 1, line); if (bracket > 0) bracket--; break;
                case '[': push(&b, T_LBRACKET, s, 1, line); bracket++; break;
                case ']': push(&b, T_RBRACKET, s, 1, line); if (bracket > 0) bracket--; break;
                case '{': push(&b, T_LBRACE, s, 1, line); bracket++; break;
                case '}': push(&b, T_RBRACE, s, 1, line); if (bracket > 0) bracket--; break;
                case ',': push(&b, T_COMMA, s, 1, line); break;
                case ':': push(&b, T_COLON, s, 1, line); break;
                case '.': push(&b, T_DOT, s, 1, line); break;
                case '+': push(&b, T_PLUS, s, 1, line); break;
                case '-': push(&b, T_MINUS, s, 1, line); break;
                case '*': push(&b, T_STAR, s, 1, line); break;
                case '/': push(&b, T_SLASH, s, 1, line); break;
                case '%': push(&b, T_PERCENT, s, 1, line); break;
                case '=': if (*p == '=') { p++; push(&b, T_EQ, s, 2, line); } else push(&b, T_ASSIGN, s, 1, line); break;
                case '!': if (*p == '=') { p++; push(&b, T_NE, s, 2, line); }
                          else { snprintf(aqs_err, sizeof aqs_err, "unexpected '!' (line %d)", line); err = 1; } break;
                case '<': if (*p == '=') { p++; push(&b, T_LE, s, 2, line); } else push(&b, T_LT, s, 1, line); break;
                case '>': if (*p == '=') { p++; push(&b, T_GE, s, 2, line); } else push(&b, T_GT, s, 1, line); break;
                default:  snprintf(aqs_err, sizeof aqs_err, "unexpected character '%c' (line %d)", c, line); err = 1; break;
                }
            }
            if (err) break;
        }
        if (err) break;

        if (bracket == 0 && *p == '\n') { push(&b, T_NEWLINE, p, 0, line); p++; line++; }
        else if (*p == 0) break;
    }

    if (err) { free(b.t); return NULL; }

    if (b.n > 0 && b.t[b.n - 1].type != T_NEWLINE) push(&b, T_NEWLINE, p, 0, line);
    while (ni > 1) { ni--; push(&b, T_DEDENT, p, 0, line); }
    push(&b, T_EOF, p, 0, line);
    *count = b.n;
    return b.t;
}
