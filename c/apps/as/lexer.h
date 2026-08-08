#ifndef AETHERSCRIPT_LEXER_H
#define AETHERSCRIPT_LEXER_H

/* AetherScript tokens. The lexer is batch (whole source -> token array) because
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
    T_BREAK, T_CONTINUE,                                   /* loop control */
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_SHL, T_SHR, T_POW,  /* bitwise / shift / power */
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ, /* compound assignment */
    T_SEMI,                                                /* ';' statement separator */
    T_FSTR,                                                /* M23 f-string: raw interior, holes intact */
    /* M27 ports. `<-` wins over `<` `-`: `x <-1` now lexes as x <- 1, so a
     * comparison against a negative literal must be written `x < -1`. Checked
     * against the whole in-tree .as corpus before taking the token. */
    T_PIPEOP, T_ARROW, T_LARROW,                           /* |>   ->   <- */
    T_WITH,                                                /* with NAME = port: */
    T_EOF, T_ERROR
} TokType;

typedef struct {
    TokType type;
    const char *start;   /* into the source buffer */
    int len;
    int line;
} Token;

/* Lex `src` into a malloc'd, T_EOF-terminated array; *count gets the length
 * (incl. the EOF token). Returns NULL and sets as_err on a lexical error. */
Token *as_lex(const char *src, int *count);

#endif /* AETHERSCRIPT_LEXER_H */
