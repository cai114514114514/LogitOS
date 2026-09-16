#ifndef AETHERSCRIPT_LEXER_H
#define AETHERSCRIPT_LEXER_H
#include <stddef.h>

/* AetherScript tokens. The lexer is batch (whole source -> token array) because
 * Python-style INDENT/DEDENT is far easier to get right line-by-line than in a
 * pull scanner. The compiler walks the array by index. */

typedef enum {
    T_NEWLINE,
    T_INDENT,
    T_DEDENT, /* layout */
    T_INT,
    T_FLOAT,
    T_STR,
    T_IDENT, /* literals */
    T_DEF,
    T_RETURN,
    T_IF,
    T_ELIF,
    T_ELSE,
    T_CLASS,
    T_SUPER, /* keywords */
    T_TRY,
    T_EXCEPT,
    T_RAISE, /* M22.4 exceptions */
    T_WHILE,
    T_FOR,
    T_IN,
    T_AND,
    T_OR,
    T_NOT,
    T_LAMBDA,
    T_IMPORT,
    T_FROM,
    T_TRUE,
    T_FALSE,
    T_NIL,
    T_LPAREN,
    T_RPAREN,
    T_LBRACKET,
    T_RBRACKET,
    T_LBRACE,
    T_RBRACE,
    T_COMMA,
    T_COLON,
    T_DOT,
    T_PLUS,
    T_MINUS,
    T_STAR,
    T_SLASH,
    T_PERCENT,
    T_ASSIGN,
    T_EQ,
    T_NE,
    T_LT,
    T_LE,
    T_GT,
    T_GE,
    T_BREAK,
    T_CONTINUE, /* loop control */
    T_AMP,
    T_PIPE,
    T_CARET,
    T_TILDE,
    T_SHL,
    T_SHR,
    T_POW, /* bitwise / shift / power */
    T_PLUSEQ,
    T_MINUSEQ,
    T_STAREQ,
    T_SLASHEQ,
    T_PERCENTEQ, /* compound assignment */
    T_SEMI,      /* ';' statement separator */
    T_FSTR,      /* M23 f-string: raw interior, holes intact */
    /* M27 ports. `<-` wins over `<` `-`: `x <-1` now lexes as x <- 1, so a
     * comparison against a negative literal must be written `x < -1`. Checked
     * against the whole in-tree .as corpus before taking the token. */
    T_PIPEOP,
    T_ARROW,
    T_LARROW, /* |>   ->   <- */
    T_WITH,   /* with NAME = port: */
    T_EOF,
    T_ERROR
} TokType;

typedef struct {
    TokType type;
    const char *start; /* into the source buffer */
    int len;
    int line;
} Token;

/* Per-request lexical errors keep the native frontend independent of runtime
 * globals. resize may be NULL (libc realloc), or a fault-injection allocator;
 * returned tokens are always freed by the caller with free(). */
typedef struct {
    const char *start;
    char message[256];
    int out_of_memory;
} AsLexError;

Token *as_lex_source(const char *source, int *count, AsLexError *error,
                     void *(*resize)(void *, size_t));
/* Decode a T_STR/T_FSTR token into raw bytes. out may be NULL to count only. */
int as_token_decode(Token token, char *out);

/* Native editor/checking recovery preserves valid lines around a lexical
 * error. Each discarded line fragment becomes T_ERROR and is reported once;
 * allocation failure remains fatal. Strict callers keep as_lex_source. */
typedef void (*AsLexReport)(void *context, Token site, const char *message);
Token *as_lex_recover(const char *source, int *count, AsLexError *error,
                      AsLexReport report, void *context, void *(*resize)(void *, size_t));

/* as_lex() was declared here and DEFINED in the A2 engine: a two-line wrapper
 * over as_lex_source() whose only addition was reporting through the VM's
 * as_err global. It went with the VM. Callers use as_lex_source() directly and
 * read AsLexError, which says whether the failure was lexical or an allocation
 * -- a distinction as_err could not carry. */

#endif /* AETHERSCRIPT_LEXER_H */
