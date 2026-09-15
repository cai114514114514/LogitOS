#ifndef LSL_INTERNAL_H
#define LSL_INTERNAL_H

#include "openlogit_lsl.h"

#define LSL_MAX_BINDINGS 64
#define LSL_MAX_SYMBOLS 128
#define LSL_MAX_EXPRESSION_DEPTH 64
#define LSL_MAX_SOURCE_BYTES (128 * 1024)

enum lsl_token {
    LSL_TOKEN_END = 0,
    LSL_TOKEN_IDENTIFIER = 256,
    LSL_TOKEN_NUMBER,
    LSL_TOKEN_GREATER_EQUAL,
    LSL_TOKEN_LESS_EQUAL,
    LSL_TOKEN_EQUAL,
    LSL_TOKEN_NOT_EQUAL
};

enum lsl_storage {
    LSL_LOCAL = 0,
    LSL_POSITION = 4
};

struct ol_lsl_shader {
    struct ol_shader_program ir;
    unsigned binding_count;
    struct ol_lsl_binding bindings[LSL_MAX_BINDINGS];
};

struct lsl_symbol {
    struct ol_lsl_binding binding;
    int register_index;
    int initialized;
};

/* A matrix or sampler names a binding, not a live VM register. Keeping the
 * two addresses separate prevents releasing a uniform slot as a temporary. */
struct lsl_value {
    int type;
    int register_index;
    unsigned binding_slot;
};

struct lsl_parser {
    const char *source;
    size_t source_bytes;
    size_t cursor;
    unsigned line;
    unsigned token_line;
    unsigned expression_depth;
    unsigned symbol_count;
    int token;
    int failed;
    char identifier[48];
    float number;
    struct ol_shader_error *error;
    struct ol_lsl_shader *shader;
    struct lsl_symbol symbols[LSL_MAX_SYMBOLS];
    uint64_t live_registers;
};

/* Lexer and diagnostics. The first error is retained during unwinding. */
int lsl_fail(struct lsl_parser *parser, const char *message);
void lsl_next_token(struct lsl_parser *parser);
int lsl_is_identifier(const struct lsl_parser *parser, const char *name);
int lsl_expect_token(struct lsl_parser *parser, int token);

int lsl_type_from_name(const char *name);
int lsl_is_numeric(int type);
int lsl_common_numeric_type(struct lsl_parser *parser, int left, int right);

/* Emission helpers borrow operands. Binary expressions and calls consume their
 * arguments; callers must copy a local before passing it to either entry. */
int lsl_allocate_register(struct lsl_parser *parser);
void lsl_release_value(struct lsl_parser *parser, struct lsl_value value);
struct lsl_value lsl_emit_instruction(struct lsl_parser *parser, int type,
                                    unsigned opcode, unsigned a,
                                    unsigned b, unsigned c);
struct lsl_value lsl_emit_vector(struct lsl_parser *parser,
                               float x, float y, float z, float w);
struct lsl_value lsl_emit_scalar(struct lsl_parser *parser, float number);
void lsl_emit_output(struct lsl_parser *parser, unsigned opcode,
                     unsigned slot, unsigned register_index);
struct lsl_value lsl_emit_binary(struct lsl_parser *parser, int operator,
                               struct lsl_value left, struct lsl_value right);
struct lsl_value lsl_emit_constructor(struct lsl_parser *parser, int type,
                                    const struct lsl_value *arguments,
                                    unsigned argument_count);
struct lsl_value lsl_emit_call(struct lsl_parser *parser, const char *name,
                             struct lsl_value *arguments,
                             unsigned argument_count);

int lsl_parse_program(struct lsl_parser *parser);
struct lsl_value lsl_parse_expression(struct lsl_parser *parser, int precedence);
struct lsl_symbol *lsl_find_symbol(struct lsl_parser *parser, const char *name);
struct lsl_symbol *lsl_add_symbol(struct lsl_parser *parser, const char *name,
                                  unsigned storage, int type, unsigned location);
struct lsl_value lsl_read_symbol(struct lsl_parser *parser, const char *name);
int lsl_assign_symbol(struct lsl_parser *parser, struct lsl_symbol *symbol,
                       struct lsl_value value);

#endif
