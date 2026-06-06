#ifndef AQUASCRIPT_LEXER_H
#define AQUASCRIPT_LEXER_H

/* AquaScript tokens. The lexer is batch (whole source -> token array) because
 * Python-style INDENT/DEDENT is far easier to get right line-by-line than in a
 * pull scanner. The compiler walks the array by index. */

typedef enum {
    T_NEWLINE, T_INDENT, T_DEDENT,          /* layout */
    T_INT, T_FLOAT, T_STR, T_IDENT,          /* literals */
    T_DEF, T_RETURN, T_IF, T_ELIF, T_ELSE, T_CLASS, T_SUPER,   /* keywords */
    T_TRY, T_EXCEPT, T_RAISE,                                   /* M22.4 exceptions */
    T_WHILE, T_FOR, T_IN, T_AND, T_OR, T_NOT, T_LAMBDA,
    T_IMPORT, T_FROM,
    T_TRUE, T_FALSE, T_NIL,
    T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_LBRACE, T_RBRACE,
    T_COMMA, T_COLON, T_DOT,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_ASSIGN, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_EOF, T_ERROR
} TokType;

typedef struct {
    TokType type;
    const char *start;   /* into the source buffer */
    int len;
    int line;
} Token;

/* Lex `src` into a malloc'd, T_EOF-terminated array; *count gets the length
 * (incl. the EOF token). Returns NULL and sets aqs_err on a lexical error. */
Token *aqs_lex(const char *src, int *count);

#endif /* AQUASCRIPT_LEXER_H */
