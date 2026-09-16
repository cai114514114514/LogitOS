/* SPDX-License-Identifier: MIT */
#include "frontend/internal.h"
#include <stdio.h>

AtNode *at_parse_lambda(AtParser *parser, Token token)
{
    AtNode *node = at_parse_node(parser, AN_CLOSURE, token);
    AsTypedProject *project = parser->p;
    if (!node) {
        return NULL;
    }
    if (!parser->f || project->nfunctions == AT_FUNCTIONS) {
        at_parse_error(parser, token, "AS3600", "Lambda function limit reached");
        return node;
    }
    AtFunction *parent = parser->f;
    node->symbol = project->nfunctions++;
    AtFunction *function = &project->functions[node->symbol];
    snprintf(function->name, sizeof function->name, "$lambda%d", node->id);
    function->module = parser->m;
    function->token = token;
    function->template_id = -1;
    function->lexical_parent = (int)(parent - project->functions) + 1;
    function->infer_result = 1;
    parser->f = function;
    while (!at_parse_has(parser, T_COLON) && !at_parse_has(parser, T_EOF)) {
        Token name = at_parse_expect(parser, T_IDENT, "expected lambda parameter");
        if (function->nparams == AT_ARGS) {
            at_parse_error(parser, name, "AS3600", "Lambda supports at most 32 parameters");
            break;
        }
        AtLocal *parameter = &function->locals[function->nparams++];
        at_parse_name(name, parameter->name, sizeof parameter->name);
        parameter->token = name;
        parameter->initialized = AT_LOCAL_INITIALIZED;
        if (!at_parse_accept(parser, T_COMMA)) {
            break;
        }
    }
    function->nlocals = function->nparams;
    at_parse_expect(parser, T_COLON, "expected ':' after lambda parameters");
    AtNode *result = at_parse_node(parser, AN_RETURN, token);
    function->body = result;
    if (result) {
        result->a = at_parse_expression(parser, 1);
    }
    parser->f = parent;
    return node;
}
