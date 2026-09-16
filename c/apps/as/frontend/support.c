/* SPDX-License-Identifier: MIT */
#include "frontend/internal.h"
#include <stdlib.h>
#include <string.h>

/* Allocation and diagnostics are shared by every parse path. Nodes enter
 * the project registry before recovery can abandon their partial tree. */
Token at_parse_current(AtParser *r)
{
    Token *tokens = r->fragment ? r->fragment : r->p->modules[r->m].tokens;
    return tokens[r->pos];
}

int at_parse_has(AtParser *r, int k)
{
    return (int)at_parse_current(r).type == k;
}

Token at_parse_advance(AtParser *r)
{
    Token t = at_parse_current(r);
    if (t.type != T_EOF) {
        r->pos++;
    }
    return t;
}

int at_parse_accept(AtParser *r, int k)
{
    if (!at_parse_has(r, k)) {
        return 0;
    }
    at_parse_advance(r);
    return 1;
}

int at_parse_word(Token t, const char *s)
{
    return t.len == (int)strlen(s) && !memcmp(t.start, s, (size_t)t.len);
}

void at_parse_name(Token t, char *out, int cap)
{
    int n = t.len;
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, t.start, (size_t)n);
    out[n] = 0;
}

void at_error(AsTypedProject *p, int m, Token t, const char *code, const char *message)
{
    /* The legacy formatter keeps one active source globally. Temporarily
     * select the token's module, otherwise imported-file offsets would point
     * into the entry source and Studio would underline the wrong text. */
    AtModule *s = &p->modules[m];
    AsDiagnostics saved = as_diagnostics;
    as_diagnostics = s->diagnostics;
    as_diagnostic_token(code, message, t.start, t.len);
    s->diagnostics = as_diagnostics;
    as_diagnostics = saved;
}

void at_parse_error(AtParser *r, Token t, const char *code, const char *s)
{
    at_error(r->p, r->m, t, code, s);
}

Token at_parse_expect(AtParser *r, int k, const char *message)
{
    Token t = at_parse_current(r);
    if (t.type == T_ERROR) {
        /* The lexer already described this damaged span. Consume the error
         * marker without multiplying expected-token errors at the same site. */
        return at_parse_advance(r);
    }
    if (!at_parse_accept(r, k)) {
        at_parse_error(r, t, "AS3100", message);
    }
    return t;
}

AtNode *at_parse_node(AtParser *r, int kind, Token t)
{
    /* Register allocations immediately: recovery may abandon a partial tree,
     * but project destruction must still reclaim every allocated node. */
    AsTypedProject *p = r->p;
    if (p->nnodes >= 65536) {
        at_parse_error(r, t, "AS3100", "Syntax tree exceeds 65536 nodes");
        return NULL;
    }
    if (p->nnodes == p->nodecap) {
        int cap = p->nodecap ? p->nodecap * 2 : 128;
        AtNode **n = realloc(p->nodes, (size_t)cap * sizeof *n);
        if (!n) {
            p->oom = 1;
            return NULL;
        }
        p->nodes = n;
        p->nodecap = cap;
    }
    AtNode *n = calloc(1, sizeof *n);
    if (!n) {
        p->oom = 1;
        return NULL;
    }
    p->nodes[p->nnodes++] = n;
    n->kind = kind;
    n->id = p->nnodes - 1;
    n->function = r->f ? (int)(r->f - p->functions) : -1;
    n->token = t;
    n->module = r->m;
    n->symbol = AT_CALL_UNRESOLVED;
    return n;
}

void at_parse_arg(AtParser *r, AtNode *n, AtNode *x)
{
    if (!n || !x) {
        return;
    }
    int limit = n->kind == AN_ARRAY || n->kind == AN_DICT ? 65536
                : n->kind == AN_UNPACK                    ? AT_ASSIGN_TARGETS
                                                          : AT_ARGS;
    if (n->count == limit) {
        at_parse_error(r, x->token, "AS3100", "Argument or container literal limit reached");
        return;
    }
    AtNode **a = realloc(n->args, (size_t)(n->count + 1) * sizeof *a);
    if (!a) {
        r->p->oom = 1;
        return;
    }
    n->args = a;
    n->args[n->count++] = x;
}

AtNode *at_parse_new_node(AsTypedProject *project, int module, int function, int kind, Token site)
{
    AtParser parser = {.p = project, .m = module};
    if (function >= 0) {
        parser.f = &project->functions[function];
    }
    return at_parse_node(&parser, kind, site);
}
