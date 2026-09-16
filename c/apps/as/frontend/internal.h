/* SPDX-License-Identifier: MIT */
#ifndef AS_TYPED_PARSE_INTERNAL_H
#define AS_TYPED_PARSE_INTERNAL_H
#include "ir/model.h"

/* One cursor/depth state is shared by the statement and expression parsers.
 * Fragment cursors borrow a private token array whose text has already been
 * rebased to the same immutable module source as ordinary tokens. */
typedef struct {
    AsTypedProject *p;
    int m, pos, depth;
    int implicit_main; /* -1, or the synthesized main() for top-level statements. */
    AtFunction *f;
    Token *fragment; /* Optional private token stream, already rebased to module source. */
} AtParser;

Token at_parse_current(AtParser *parser);
int at_parse_has(AtParser *parser, int kind);
Token at_parse_advance(AtParser *parser);
int at_parse_accept(AtParser *parser, int kind);
int at_parse_word(Token token, const char *word);
void at_parse_name(Token token, char *out, int capacity);
void at_parse_error(AtParser *parser, Token token, const char *code, const char *message);
Token at_parse_expect(AtParser *parser, int kind, const char *message);
AtNode *at_parse_node(AtParser *parser, int kind, Token token);
void at_parse_arg(AtParser *parser, AtNode *node, AtNode *argument);
int at_parse_type(AtParser *parser);
AtNode *at_parse_expression(AtParser *parser, int precedence);
AtNode *at_parse_unpack(AtParser *parser, AtNode *first_target);
AtNode *at_parse_lambda(AtParser *parser, Token token);
int at_parse_layout(AtParser *parser);
/* Resolve an explicit std.* path without local shadowing. Bare imports keep
 * their original local-first lookup, including unsaved source overlays. */
int at_parse_import_path(AtParser *parser, char module[64], char path[512], Token *site);
#endif
