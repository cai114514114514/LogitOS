/* SPDX-License-Identifier: MIT */
#include "frontend/internal.h"
#include <stdlib.h>
#include <string.h>

/* Expression precedence, postfix operations and interpolation fragments.
 * Statements and declaration parsing remain in frontend/parser.c. */
static int precedence(int k)
{
    switch (k) {
    case T_OR:
    case T_PIPEOP:
    case T_ARROW:
    case T_LARROW:
        return 1;
    case T_AND:
        return 2;
    case T_EQ:
    case T_NE:
    case T_LT:
    case T_LE:
    case T_GT:
    case T_GE:
    case T_IN:
        return 3;
    case T_PIPE:
        return 4;
    case T_CARET:
        return 5;
    case T_AMP:
        return 6;
    case T_SHL:
    case T_SHR:
        return 7;
    case T_PLUS:
    case T_MINUS:
        return 8;
    case T_STAR:
    case T_SLASH:
    case T_PERCENT:
        return 9;
    case T_POW:
        return 11;
    default:
        return 0;
    }
}

AtNode *at_parse_expression(AtParser *r, int min);

static int binary_precedence(AtParser *parser)
{
    Token token = at_parse_current(parser);
    Token *tokens = parser->fragment ? parser->fragment : parser->p->modules[parser->m].tokens;
    if ((token.type == T_IDENT && at_parse_word(token, "is")) ||
        (token.type == T_NOT && tokens[parser->pos + 1].type == T_IN)) {
        return 3;
    }
    return precedence(token.type);
}

static AtNode *atom(AtParser *r)
{
    Token t = at_parse_advance(r);
    AtNode *n = NULL;
    if (t.type == T_ERROR) {
        n = at_parse_node(r, AN_ERROR, t);
    } else if (t.type == T_NIL || (t.type == T_IDENT && at_parse_word(t, "None"))) {
        n = at_parse_node(r, AN_NONE, t);
    } else if (t.type == T_INT) {
        n = at_parse_node(r, AN_INT, t);
    } else if (t.type == T_FLOAT) {
        n = at_parse_node(r, AN_FLOAT, t);
    } else if (t.type == T_TRUE || t.type == T_FALSE ||
               (t.type == T_IDENT && (at_parse_word(t, "True") || at_parse_word(t, "False")))) {
        n = at_parse_node(r, AN_BOOL, t);
    } else if (t.type == T_STR) {
        n = at_parse_node(r, AN_STR, t);
    } else if (t.type == T_FSTR) {
        int function = r->f ? (int)(r->f - r->p->functions) : -1;
        n = at_parse_fstring(r->p, r->m, function, t, r->depth);
    } else if (t.type == T_LAMBDA) {
        n = at_parse_lambda(r, t);
    } else if (t.type == T_IDENT) {
        n = at_parse_node(r, AN_NAME, t);
    } else if (t.type == T_SUPER) {
        n = at_parse_node(r, AN_SUPER, t);
    } else if (t.type == T_MINUS || t.type == T_PLUS || t.type == T_NOT || t.type == T_TILDE) {
        n = at_parse_node(r, AN_UNARY, t);
        if (n) {
            n->op = t.type;
            n->a = at_parse_expression(r, t.type == T_NOT ? 3 : 10);
        }
    } else if (t.type == T_LPAREN) {
        n = at_parse_expression(r, 1);
        at_parse_expect(r, T_RPAREN, "expected ')' after expression");
    } else if (t.type == T_LBRACE) {
        n = at_parse_node(r, AN_DICT, t);
        while (!at_parse_has(r, T_RBRACE) && !at_parse_has(r, T_EOF)) {
            AtNode *pair = at_parse_node(r, AN_PAIR, at_parse_current(r));
            if (!pair) {
                break;
            }
            pair->a = at_parse_expression(r, 1);
            at_parse_expect(r, T_COLON, "expected ':' between dictionary key and value");
            pair->b = at_parse_expression(r, 1);
            at_parse_arg(r, n, pair);
            if (!at_parse_accept(r, T_COMMA)) {
                break;
            }
        }
        at_parse_expect(r, T_RBRACE, "expected '}' after dictionary");
    } else if (t.type == T_LBRACKET) {
        n = at_parse_node(r, AN_ARRAY, t);
        while (!at_parse_has(r, T_RBRACKET) && !at_parse_has(r, T_EOF)) {
            at_parse_arg(r, n, at_parse_expression(r, 1));
            if (n && n->count == 1 && at_parse_accept(r, T_FOR)) {
                n->kind = AN_COMPREHENSION;
                n->a = n->args[0];
                Token binding = at_parse_expect(r, T_IDENT, "expected comprehension variable");
                n->args[0] = at_parse_node(r, AN_NAME, binding);
                at_parse_expect(r, T_IN, "expected 'in' after comprehension variable");
                /* A following if belongs to the filter. Parenthesized
                 * conditional expressions remain legal iterable values. */
                n->b = at_parse_expression(r, 0);
                if (at_parse_accept(r, T_IF)) {
                    n->c = at_parse_expression(r, 1);
                }
                break;
            }
            if (!at_parse_accept(r, T_COMMA)) {
                break;
            }
        }
        at_parse_expect(r, T_RBRACKET, "expected ']' after array");
    } else {
        at_parse_error(r, t, "AS3100",
                       "expected expression (this construct is not supported in native code yet)");
        n = at_parse_node(r, AN_INT, t);
    }
    while (n) {
        if (at_parse_accept(r, T_LPAREN)) {
            AtNode *call = at_parse_node(r, AN_CALL, t);
            if (!call) {
                return n;
            }
            call->a = n;
            while (!at_parse_has(r, T_RPAREN) && !at_parse_has(r, T_EOF)) {
                at_parse_arg(r, call, at_parse_expression(r, 1));
                if (!at_parse_accept(r, T_COMMA)) {
                    break;
                }
            }
            at_parse_expect(r, T_RPAREN, "expected ')' after call");
            n = call;
        } else if (at_parse_accept(r, T_LBRACKET)) {
            if (n->kind == AN_NAME &&
                (at_parse_word(n->token, "cast") || at_parse_word(n->token, "is_type"))) {
                /* These builtins take a type, not a runtime subscript. Keep
                 * the type on the callee node so specialization substitutes T
                 * exactly as it substitutes annotations elsewhere. */
                AtNode *application = at_parse_node(r, AN_TYPE_APPLICATION, n->token);
                int target = at_parse_type(r);
                at_parse_expect(r, T_RBRACKET, "expected ']' after type argument");
                if (application) {
                    application->type = target;
                    application->op = at_parse_word(n->token, "is_type");
                    n = application;
                }
                continue;
            }
            AtNode *index = at_parse_node(r, AN_INDEX, t);
            if (!index) {
                return n;
            }
            index->a = n;
            index->b = at_parse_expression(r, 1);
            at_parse_expect(r, T_RBRACKET, "expected ']' after index");
            n = index;
        } else if (at_parse_has(r, T_DOT)) {
            Token separator = at_parse_advance(r);
            Token field = at_parse_expect(r, T_IDENT, "expected field or module member");
            AtNode *f = at_parse_node(r, AN_FIELD, field);
            if (!f) {
                return n;
            }
            f->a = n;
            f->member_separator = separator;
            n = f;
        } else {
            break;
        }
    }
    return n;
}

AtNode *at_parse_expression(AtParser *r, int min)
{
    /* Increasing the right operand's minimum precedence makes operators
     * left-associative. Power keeps its precedence and associates right. */
    if (++r->depth > 64) {
        at_parse_error(r, at_parse_current(r), "AS3100", "Expression nesting limit reached");
        at_parse_advance(r);
        r->depth--;
        return NULL;
    }
    AtNode *left = atom(r);
    int prec;
    while (left && (prec = binary_precedence(r)) >= min && prec) {
        Token op = at_parse_advance(r);
        AtNode *n = at_parse_node(r, AN_BINARY, op);
        if (!n) {
            break;
        }
        n->op = op.type;
        int identity = op.type == T_IDENT && at_parse_word(op, "is");
        int negated_membership = op.type == T_NOT;
        if (identity) {
            n->op = at_parse_accept(r, T_NOT) ? T_NE : T_EQ;
        } else if (negated_membership) {
            at_parse_expect(r, T_IN, "expected 'in' after 'not'");
            n->op = T_IN;
        }
        n->a = left;
        n->b = at_parse_expression(r, prec + (op.type != T_POW));
        if (identity && n->a->kind != AN_NONE && n->b && n->b->kind != AN_NONE) {
            at_parse_error(r, op, "AS3202", "is/is not currently require a None operand");
        }
        left = n;
        if (negated_membership) {
            /* Reuse membership checking/lowering so negation cannot change
             * operand evaluation order or bypass a collection's equality rule. */
            AtNode *negated = at_parse_node(r, AN_UNARY, op);
            if (negated) {
                negated->op = T_NOT;
                negated->a = n;
                left = negated;
            }
        }
    }
    if (left && min == 1 && at_parse_accept(r, T_IF)) {
        AtNode *conditional = at_parse_node(r, AN_CONDITIONAL, left->token);
        if (conditional) {
            conditional->b = left;
            /* Zero accepts the full boolean expression while suppressing an
             * unparenthesized nested conditional before this clause's else. */
            conditional->a = at_parse_expression(r, 0);
            at_parse_expect(r, T_ELSE, "conditional expression requires else");
            conditional->c = at_parse_expression(r, 1);
            left = conditional;
        }
    }
    r->depth--;
    return left;
}

AtNode *at_parse_fragment(AsTypedProject *project, int module, int function, Token text, int depth)
{
    /* Parentheses put multiline holes into expression continuation mode. They
     * are scanner sentinels only: every retained token points at real source.
     * Reusing a AtParser keeps precedence, generic type arguments and depth
     * limits identical to expressions outside formatted strings. */
    char *wrapped = malloc((size_t)text.len + 3);
    if (!wrapped) {
        project->oom = 1;
        return NULL;
    }
    wrapped[0] = '(';
    memcpy(wrapped + 1, text.start, (size_t)text.len);
    wrapped[text.len + 1] = ')';
    wrapped[text.len + 2] = 0;
    AsLexError lexical = {0};
    int count = 0;
    Token *tokens = as_lex_source(wrapped, &count, &lexical, NULL);
    if (!tokens) {
        Token failure = text;
        if (lexical.start && lexical.start > wrapped && lexical.start <= wrapped + text.len) {
            failure.start += lexical.start - wrapped - 1;
            failure.len = 1;
        }
        at_error(project, module, failure, "AS3100", lexical.message);
        project->oom |= lexical.out_of_memory;
        free(wrapped);
        return NULL;
    }
    int line = 1;
    for (const char *byte = project->modules[module].source; byte < text.start; byte++) {
        if (*byte == '\n') {
            line++;
        }
    }
    for (int index = 0; index < count; index++) {
        ptrdiff_t offset = tokens[index].start - wrapped - 1;
        if (offset < 0) {
            offset = 0;
        } else if (offset > text.len) {
            offset = text.len;
        }
        tokens[index].start = text.start + offset;
        tokens[index].line += line - 1;
    }
    free(wrapped);
    AtParser parser = {.p = project, .m = module, .pos = 1, .depth = depth, .fragment = tokens};
    if (function >= 0) {
        parser.f = &project->functions[function];
    }
    AtNode *expression = at_parse_expression(&parser, 1);
    at_parse_expect(&parser, T_RPAREN, "unexpected text after the f-string expression");
    at_parse_accept(&parser, T_NEWLINE);
    if (!at_parse_has(&parser, T_EOF)) {
        at_parse_error(&parser, at_parse_current(&parser), "AS3100",
                       "unexpected text after the f-string expression");
    }
    free(tokens);
    return expression;
}
