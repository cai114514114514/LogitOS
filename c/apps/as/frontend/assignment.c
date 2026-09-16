/* SPDX-License-Identifier: MIT */
#include "frontend/internal.h"

AtNode *at_parse_unpack(AtParser *parser, AtNode *target)
{
    AtNode *unpack = at_parse_node(parser, AN_UNPACK, target->token);
    if (!unpack) {
        return NULL;
    }
    for (;;) {
        if (target->kind != AN_NAME) {
            at_parse_error(parser, target->token, "AS3100",
                           "Multiple assignment targets must be names");
        }
        /* Ordinary assignment children participate in the existing lexical
         * binding pass. Their right sides are checked together before any
         * target gains an initialization fact or a runtime store. */
        AtNode *assignment = at_parse_node(parser, AN_ASSIGN, target->token);
        if (!assignment) {
            return unpack;
        }
        assignment->a = target;
        assignment->op = T_ASSIGN;
        at_parse_arg(parser, unpack, assignment);
        if (parser->p->oom) {
            return unpack;
        }
        if (!at_parse_accept(parser, T_COMMA)) {
            break;
        }
        Token name = at_parse_expect(parser, T_IDENT, "expected a name in multiple assignment");
        target = at_parse_node(parser, AN_NAME, name);
        if (!target) {
            return unpack;
        }
    }
    at_parse_expect(parser, T_ASSIGN, "expected '=' in multiple assignment");
    AtNode *first = at_parse_expression(parser, 1);
    if (!at_parse_has(parser, T_COMMA)) {
        unpack->a = first;
        return unpack;
    }
    int count = 1;
    unpack->args[0]->b = first;
    while (at_parse_accept(parser, T_COMMA)) {
        AtNode *value = at_parse_expression(parser, 1);
        if (count < unpack->count) {
            unpack->args[count]->b = value;
        }
        count++;
    }
    if (count != unpack->count) {
        at_parse_error(parser, unpack->token, "AS3204", "Assignment count mismatch");
    }
    return unpack;
}
