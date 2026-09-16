/* SPDX-License-Identifier: MIT */
#include "frontend/internal.h"
#include "common/numeric.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Bounded recursive descent builds a recoverable tree instead of emitting VM
 * instructions while parsing. All tokens continue to point into owned source. */
static int compound_type(AtParser *r, int kind, int element, int count)
{
    return at_compound_type(r->p, kind, element, count, r->m, at_parse_current(r));
}

int at_parse_type(AtParser *r)
{
    Token t = at_parse_advance(r);
    if (t.type == T_ERROR) {
        return AT_ERROR;
    }
    char s[64];
    at_parse_name(t, s, sizeof s);
    int parameter = at_function_type(r->p, r->f, t);
    if (parameter) {
        return parameter;
    }
    if (at_parse_word(t, "None") || at_parse_word(t, "void")) {
        return AT_VOID;
    }
    if (at_parse_word(t, "int") || at_parse_word(t, "isize")) {
        return AT_I64;
    }
    if (at_parse_word(t, "usize")) {
        return AT_U64;
    }
    if (at_parse_word(t, "float")) {
        return AT_F64;
    }
    if (at_parse_word(t, "Callable")) {
        at_parse_expect(r, T_LBRACKET, "expected '[' in Callable type");
        at_parse_expect(r, T_LBRACKET, "expected parameter type list in Callable");
        if (++r->depth > 64) {
            at_parse_error(r, t, "AS3100", "Type nesting limit reached");
            r->depth--;
            return AT_ERROR;
        }
        int parameters[AT_ARGS], count = 0;
        while (!at_parse_has(r, T_RBRACKET) && !at_parse_has(r, T_EOF)) {
            int parameter = at_parse_type(r);
            if (count == AT_ARGS) {
                at_parse_error(r, t, "AS3100", "Callable supports at most 32 parameters");
                break;
            }
            parameters[count++] = parameter;
            if (!at_parse_accept(r, T_COMMA)) {
                break;
            }
        }
        at_parse_expect(r, T_RBRACKET, "expected ']' after callable parameters");
        at_parse_expect(r, T_COMMA, "expected comma before callable result");
        int result = at_parse_type(r);
        r->depth--;
        at_parse_expect(r, T_RBRACKET, "expected ']' after Callable type");
        return at_callable_type(r->p, parameters, count, result, r->m, t);
    }
    if (at_parse_word(t, "Dict")) {
        at_parse_expect(r, T_LBRACKET, "expected '[' in Dict type");
        if (++r->depth > 64) {
            at_parse_error(r, t, "AS3100", "Type nesting limit reached");
            r->depth--;
            return AT_ERROR;
        }
        int key = at_parse_type(r);
        at_parse_expect(r, T_COMMA, "expected comma between Dict key and value types");
        int value = at_parse_type(r);
        r->depth--;
        at_parse_expect(r, T_RBRACKET, "expected ']' after Dict type");
        return at_dict_type(r->p, key, value, r->m, t);
    }
    if (at_parse_word(t, "Array") || at_parse_word(t, "Slice") || at_parse_word(t, "MutSlice") ||
        at_parse_word(t, "List") || at_parse_word(t, "Optional") || at_parse_word(t, "Ptr")) {
        int kind = at_parse_word(t, "Array")      ? AT_ARRAY
                   : at_parse_word(t, "List")     ? AT_LIST
                   : at_parse_word(t, "Optional") ? AT_OPTIONAL
                   : at_parse_word(t, "Ptr")      ? AT_POINTER
                   : at_parse_word(t, "MutSlice") ? AT_MUT_SLICE
                                                  : AT_SLICE;
        at_parse_expect(r, T_LBRACKET, "expected '[' in container type");
        if (++r->depth > 64) {
            at_parse_error(r, t, "AS3100", "Type nesting limit reached");
            r->depth--;
            return 0;
        }
        int elem = at_parse_type(r);
        r->depth--;
        int count = 0;
        if (kind == AT_ARRAY) {
            at_parse_expect(r, T_COMMA, "expected array length");
            Token size = at_parse_expect(r, T_INT, "expected constant array length");
            char n[32];
            at_parse_name(size, n, sizeof n);
            count = atoi(n);
            if (count < 1 || count > 65536) {
                at_parse_error(r, size, "AS3200", "Array length must be 1..65536");
                count = 1;
            }
        }
        at_parse_expect(r, T_RBRACKET, "expected ']' after container type");
        return compound_type(r, kind, elem, count);
    }
    int resolved = at_named_type(r->p, r->m, s);
    if (resolved) {
        return resolved;
    }
    at_parse_error(r, t, "AS3200", "Unknown or not yet supported static type");
    return 0;
}

static void recover(AtParser *r)
{
    /* Leave DEDENT for the enclosing block parser. Consuming it here would
     * pull the following function into the malformed statement's scope. */
    while (!at_parse_has(r, T_NEWLINE) && !at_parse_has(r, T_DEDENT) && !at_parse_has(r, T_EOF)) {
        at_parse_advance(r);
    }
    at_parse_accept(r, T_NEWLINE);
}

static void newline(AtParser *r)
{
    if (!at_parse_accept(r, T_NEWLINE) && !at_parse_has(r, T_EOF) && !at_parse_has(r, T_DEDENT)) {
        at_parse_error(r, at_parse_current(r), "AS3100", "expected end of statement");
        recover(r);
    }
}

static void recover_declaration(AtParser *parser)
{
    /* A failed signature owns its entire indented suite. Recovering only to
     * NEWLINE reinterprets that suite as module-level code and produces many
     * unrelated errors. Stop after its matching DEDENT, preserving siblings. */
    recover(parser);
    while (at_parse_accept(parser, T_NEWLINE)) {
    }
    if (!at_parse_accept(parser, T_INDENT)) {
        return;
    }
    int nesting = 1;
    while (nesting && !at_parse_has(parser, T_EOF)) {
        Token token = at_parse_advance(parser);
        if (token.type == T_INDENT) {
            nesting++;
        } else if (token.type == T_DEDENT) {
            nesting--;
        }
    }
}

static AtNode *block(AtParser *r);

static int assertion_function_call(AtParser *parser)
{
    Token *tokens = parser->p->modules[parser->m].tokens;
    if (tokens[parser->pos + 1].type != T_LPAREN) {
        return 0;
    }
    /* Preserve the library's assert(condition, message) call while keeping
     * language statements such as assert (x + 1) == y unambiguous. Commas
     * inside nested calls/containers belong to the asserted expression. */
    int depth = 0;
    for (int index = parser->pos + 1; tokens[index].type != T_EOF; index++) {
        int kind = tokens[index].type;
        if (kind == T_LPAREN || kind == T_LBRACKET || kind == T_LBRACE) {
            depth++;
        } else if (kind == T_RPAREN || kind == T_RBRACKET || kind == T_RBRACE) {
            if (--depth <= 0) {
                return 0;
            }
        } else if (kind == T_COMMA && depth == 1) {
            return 1;
        }
    }
    return 0;
}

static void function(AtParser *r, int owner);

static AtNode *statement(AtParser *r)
{
    Token t = at_parse_current(r);
    AtNode *n = NULL;
    if (at_parse_accept(r, T_DEF)) {
        Token name = at_parse_current(r);
        n = at_parse_node(r, AN_ASSIGN, name);
        if (!n) {
            return NULL;
        }
        n->op = T_ASSIGN;
        n->a = at_parse_node(r, AN_NAME, name);
        n->b = at_parse_node(r, AN_CLOSURE, name);
        if (n->b) {
            n->b->op = 1; /* A named definition may capture its own binding. */
            n->b->symbol = r->p->nfunctions;
        }
        function(r, 0);
    } else if (at_parse_accept(r, T_RETURN)) {
        n = at_parse_node(r, AN_RETURN, t);
        if (n && !at_parse_has(r, T_NEWLINE) && !at_parse_has(r, T_DEDENT) &&
            !at_parse_has(r, T_EOF)) {
            n->a = at_parse_expression(r, 1);
        }
        newline(r);
    } else if (at_parse_accept(r, T_RAISE)) {
        n = at_parse_node(r, AN_RAISE, t);
        if (n && !at_parse_has(r, T_NEWLINE) && !at_parse_has(r, T_DEDENT) &&
            !at_parse_has(r, T_EOF)) {
            n->a = at_parse_expression(r, 1);
        }
        newline(r);
    } else if (at_parse_accept(r, T_WITH)) {
        n = at_parse_node(r, AN_WITH, t);
        if (!n) {
            return NULL;
        }
        n->a = at_parse_node(r, AN_NAME, at_parse_expect(r, T_IDENT, "expected with owner name"));
        if (at_parse_accept(r, T_COMMA)) {
            AtNode *owners = at_parse_node(r, AN_UNPACK, t);
            if (!owners) {
                return NULL;
            }
            at_parse_arg(r, owners, n->a);
            at_parse_arg(
                r, owners,
                at_parse_node(r, AN_NAME,
                              at_parse_expect(r, T_IDENT, "expected second with owner name")));
            n->a = owners;
        }
        at_parse_expect(r, T_ASSIGN, "expected '=' after with owner name");
        n->b = at_parse_expression(r, 1);
        n->c = block(r);
    } else if (at_parse_accept(r, T_TRY)) {
        n = at_parse_node(r, AN_TRY, t);
        if (!n) {
            return NULL;
        }
        n->a = block(r);
        AtNode **tail = &n->b;
        while (at_parse_accept(r, T_EXCEPT)) {
            Token exception = at_parse_current(r);
            AtNode *handler = at_parse_node(r, AN_EXCEPT, exception);
            if (!handler) {
                break;
            }
            handler->op = AT_E_ANY;
            if (!at_parse_has(r, T_COLON)) {
                at_parse_advance(r);
                handler->op = at_exception_code(exception);
                if (handler->op < 0) {
                    at_parse_error(r, exception, "AS3700",
                                   "Expected an exception type; use except Error as error:");
                }
                if (at_parse_word(at_parse_current(r), "as")) {
                    at_parse_advance(r);
                    handler->a = at_parse_node(
                        r, AN_NAME, at_parse_expect(r, T_IDENT, "expected exception binding"));
                }
            }
            handler->b = block(r);
            *tail = handler;
            tail = &handler->next;
        }
        if (!n->b) {
            at_parse_error(r, t, "AS3700", "try requires at least one except handler");
        }
        if (at_parse_accept(r, T_ELSE)) {
            n->c = block(r);
        }
    } else if (at_parse_accept(r, T_IF) || at_parse_accept(r, T_ELIF)) {
        n = at_parse_node(r, AN_IF, t);
        if (!n) {
            return NULL;
        }
        n->a = at_parse_expression(r, 1);
        n->b = block(r);
        if (at_parse_has(r, T_ELIF)) {
            n->c = statement(r);
        } else if (at_parse_accept(r, T_ELSE)) {
            n->c = block(r);
        }
    } else if (at_parse_accept(r, T_WHILE)) {
        n = at_parse_node(r, AN_WHILE, t);
        if (n) {
            n->a = at_parse_expression(r, 1);
            n->b = block(r);
        }
    } else if (at_parse_accept(r, T_FOR)) {
        n = at_parse_node(r, AN_FOR, t);
        Token var = at_parse_expect(r, T_IDENT, "expected loop variable");
        if (n) {
            n->a = at_parse_node(r, AN_NAME, var);
            at_parse_expect(r, T_IN, "expected 'in' after loop variable");
            n->b = at_parse_expression(r, 1);
            n->c = block(r);
        }
    } else if (at_parse_accept(r, T_BREAK) || at_parse_accept(r, T_CONTINUE)) {
        n = at_parse_node(r, t.type == T_BREAK ? AN_BREAK : AN_CONTINUE, t);
        newline(r);
    } else if (at_parse_word(t, "assert") && !assertion_function_call(r)) {
        at_parse_advance(r);
        n = at_parse_node(r, AN_ASSERT, t);
        if (n) {
            n->a = at_parse_expression(r, 1);
        }
        newline(r);
    } else if (at_parse_word(t, "pass")) {
        at_parse_advance(r);
        n = at_parse_node(r, AN_PASS, t);
        newline(r);
    } else if (at_parse_word(t, "global")) {
        at_parse_advance(r);
        n = at_parse_node(r, AN_GLOBAL_DECL, t);
        do {
            Token binding = at_parse_expect(r, T_IDENT, "expected module variable after global");
            if (n && n->count < AT_ARGS) {
                at_parse_arg(r, n, at_parse_node(r, AN_NAME, binding));
            } else {
                at_parse_error(r, binding, "AS3200", "Too many global declarations");
            }
        } while (at_parse_accept(r, T_COMMA));
        newline(r);
    } else if (at_parse_word(t, "unsafe")) {
        at_parse_advance(r);
        n = at_parse_node(r, AN_UNSAFE, t);
        if (n) {
            n->a = block(r);
        }
    } else if (t.type == T_CLASS || t.type == T_WITH) {
        at_parse_error(r, t, "AS3900",
                       "This construct is not implemented by the native frontend yet");
        recover(r);
        if (at_parse_accept(r, T_INDENT)) {
            int level = 1;
            while (level && !at_parse_has(r, T_EOF)) {
                int k = at_parse_advance(r).type;
                level += (k == T_INDENT) - (k == T_DEDENT);
            }
        }
    } else {
        AtNode *lhs = at_parse_expression(r, 1);
        if (lhs && at_parse_has(r, T_COMMA)) {
            n = at_parse_unpack(r, lhs);
            newline(r);
            return n;
        }
        int annotation = 0;
        if (at_parse_accept(r, T_COLON)) {
            annotation = at_parse_type(r);
        }
        int op = at_parse_current(r).type;
        if (op == T_ASSIGN || (op >= T_PLUSEQ && op <= T_PERCENTEQ)) {
            at_parse_advance(r);
            n = at_parse_node(r, AN_ASSIGN, t);
            if (n) {
                n->a = lhs;
                n->type = annotation;
                n->op = op;
                n->b = at_parse_expression(r, 1);
            }
        } else if (annotation) {
            n = at_parse_node(r, AN_ASSIGN, t);
            if (n) {
                n->a = lhs;
                n->type = annotation;
                n->op = T_ASSIGN;
            }
        } else {
            n = at_parse_node(r, AN_EXPR, t);
            if (n) {
                n->a = lhs;
            }
        }
        newline(r);
    }
    return n;
}

static AtNode *block(AtParser *r)
{
    at_parse_expect(r, T_COLON, "expected ':' before block");
    at_parse_expect(r, T_NEWLINE, "expected newline before block");
    if (!at_parse_accept(r, T_INDENT)) {
        at_parse_error(r, at_parse_current(r), "AS3100", "expected indented block");
        return NULL;
    }
    if (++r->depth > 64) {
        at_parse_error(r, at_parse_current(r), "AS3100", "Block nesting limit reached");
        r->depth--;
        return NULL;
    }
    AtNode *head = NULL, **tail = &head;
    while (!at_parse_has(r, T_DEDENT) && !at_parse_has(r, T_EOF) && !r->p->oom) {
        int before = r->pos;
        if (at_parse_accept(r, T_NEWLINE)) {
            continue;
        }
        AtNode *n = statement(r);
        if (n) {
            *tail = n;
            tail = &n->next;
        }
        if (before == r->pos) {
            at_parse_advance(r);
        }
    }
    at_parse_expect(r, T_DEDENT, "expected end of block");
    r->depth--;
    return head;
}

static Token at_parse_lookahead(AtParser *r, int ahead)
{
    Token *tokens = r->fragment ? r->fragment : r->p->modules[r->m].tokens;
    int pos = r->pos;
    while (ahead > 0 && tokens[pos].type != T_EOF) {
        pos++;
        ahead--;
    }
    return tokens[pos];
}

static int module_ident_is_variable(AtParser *r)
{
    Token name = at_parse_current(r);
    if (at_parse_word(name, "unsafe") || at_parse_word(name, "assert") ||
        at_parse_word(name, "pass") || at_parse_word(name, "global")) {
        return 0;
    }
    int next = (int)at_parse_lookahead(r, 1).type;
    return next == T_ASSIGN || next == T_COLON || next == T_COMMA;
}

static AtFunction *script_main(AtParser *r)
{
    AsTypedProject *p = r->p;
    if (r->implicit_main >= 0) {
        return &p->functions[r->implicit_main];
    }
    for (int i = 0; i < p->nfunctions; i++) {
        AtFunction *existing = &p->functions[i];
        if (existing->module == r->m && !existing->method_owner && !existing->lexical_parent &&
            !strcmp(existing->name, "main")) {
            at_parse_error(r, at_parse_current(r), "AS3900",
                           "This module already has main(); move this statement into that function");
            return NULL;
        }
    }
    if (p->nfunctions == AT_FUNCTIONS) {
        at_parse_error(r, at_parse_current(r), "AS3100", "Too many functions");
        return NULL;
    }
    r->implicit_main = p->nfunctions;
    AtFunction *f = &p->functions[p->nfunctions++];
    memset(f, 0, sizeof *f);
    strcpy(f->name, "main");
    f->module = r->m;
    f->token = at_parse_current(r);
    f->template_id = -1;
    f->result = AT_VOID;
    return f;
}

static void append_script_statement(AtFunction *f, AtNode *n)
{
    if (!n) {
        return;
    }
    if (!f->body) {
        f->body = n;
        return;
    }
    AtNode *end = f->body;
    while (end->next) {
        end = end->next;
    }
    end->next = n;
}

static void parse_script_statement(AtParser *r)
{
    AtFunction *main = script_main(r);
    if (!main) {
        recover(r);
        return;
    }
    AtFunction *saved = r->f;
    r->f = main;
    append_script_statement(main, statement(r));
    r->f = saved;
}

static void parse_script_suite(AtParser *r)
{
    /* A version cookie is a comment. People still indent the first statement
     * under `#aether: 3` as if it opened a block. That indent is the program. */
    AtFunction *main = script_main(r);
    if (!main) {
        recover(r);
        return;
    }
    AtFunction *saved = r->f;
    r->f = main;
    if (++r->depth > 64) {
        at_parse_error(r, at_parse_current(r), "AS3100", "Block nesting limit reached");
        r->depth--;
        r->f = saved;
        return;
    }
    while (!at_parse_has(r, T_DEDENT) && !at_parse_has(r, T_EOF) && !r->p->oom) {
        int before = r->pos;
        if (at_parse_accept(r, T_NEWLINE)) {
            continue;
        }
        append_script_statement(main, statement(r));
        if (before == r->pos) {
            at_parse_advance(r);
        }
    }
    at_parse_expect(r, T_DEDENT, "expected end of block");
    r->depth--;
    r->f = saved;
}

static int load_module(AsTypedProject *p, const char *path, int parent, Token site);

static void module_import(AtParser *r, int from)
{
    Token t;
    char module[64];
    char path[512];
    if (!at_parse_import_path(r, module, path, &t)) {
        recover(r);
        return;
    }
    AtModule *m = &r->p->modules[r->m];
    if (m->nimports == AT_ARGS) {
        at_parse_error(r, t, "AS3300", "Too many imports in this module");
        recover(r);
        return;
    }
    int target = load_module(r->p, path, r->m, t);
    if (from) {
        at_parse_expect(r, T_IMPORT, "expected 'import' after module name");
    }
    do {
        if (m->nimports == AT_ARGS) {
            at_parse_error(r, t, "AS3300", "Too many imported names");
            break;
        }
        AtImport *i = &m->imports[m->nimports++];
        i->target = target;
        i->token = t;
        if (from) {
            Token member = at_parse_expect(r, T_IDENT, "expected imported name");
            i->token = member;
            at_parse_name(member, i->name, sizeof i->name);
            strcpy(i->member, i->name);
            if (i->name[0] == '_') {
                at_parse_error(r, member, "AS3300", "Private names cannot be imported");
            }
        } else {
            strcpy(i->name, module);
        }
        if (at_parse_word(at_parse_current(r), "as")) {
            at_parse_advance(r);
            Token alias = at_parse_expect(r, T_IDENT, "expected import alias after 'as'");
            if (alias.len >= (int)sizeof i->name) {
                at_parse_error(r, alias, "AS3300", "Import alias is too long");
            }
            at_parse_name(alias, i->name, sizeof i->name);
        }
    } while (from && at_parse_accept(r, T_COMMA));
    newline(r);
}

static int type_parameters(AtParser *parser, AtFunction *function)
{
    if (!at_parse_accept(parser, T_LBRACKET)) {
        return 1;
    }
    AsTypedProject *project = parser->p;
    while (!at_parse_has(parser, T_RBRACKET) && !at_parse_has(parser, T_EOF)) {
        Token parameter = at_parse_expect(parser, T_IDENT, "expected generic type parameter");
        if (parameter.type != T_IDENT || function->generic_count == AT_TYPE_PARAMETERS) {
            at_parse_error(parser, parameter, "AS3600",
                           "Invalid or excessive generic type parameters");
            return 0;
        }
        char spelling[64];
        at_parse_name(parameter, spelling, sizeof spelling);
        if (at_function_type(project, function, parameter) ||
            at_named_type(project, parser->m, spelling) || at_parse_word(parameter, "int") ||
            at_parse_word(parameter, "float") || at_parse_word(parameter, "isize") ||
            at_parse_word(parameter, "usize") || at_parse_word(parameter, "Array") ||
            at_parse_word(parameter, "Slice") || at_parse_word(parameter, "List") ||
            at_parse_word(parameter, "Dict") || at_parse_word(parameter, "Callable") ||
            at_parse_word(parameter, "None") || at_parse_word(parameter, "void")) {
            at_parse_error(parser, parameter, "AS3600",
                           "Generic parameter duplicates or shadows an existing type");
        }
        int id = at_allocate_type(project, parser->m, parameter);
        if (!id) {
            return 0;
        }
        AtType *type = &project->types[id];
        type->kind = AT_PARAMETER;
        type->module = parser->m;
        at_parse_name(parameter, type->name, sizeof type->name);
        function->type_parameters[function->generic_count++] = id;
        if (at_parse_accept(parser, T_COLON)) {
            Token constraint = at_parse_expect(parser, T_IDENT, "expected protocol constraint");
            if (at_parse_word(constraint, "Number")) {
                type->constraint = AT_CONSTRAINT_NUMBER;
            } else if (at_parse_word(constraint, "Integer")) {
                type->constraint = AT_CONSTRAINT_INTEGER;
            } else if (at_parse_word(constraint, "Hashable")) {
                type->constraint = AT_CONSTRAINT_HASHABLE;
            } else if (at_parse_word(constraint, "Equatable")) {
                type->constraint = AT_CONSTRAINT_EQUATABLE;
            } else if (at_parse_word(constraint, "Ordered")) {
                type->constraint = AT_CONSTRAINT_ORDERED;
            } else if (at_parse_word(constraint, "ByteStorage")) {
                type->constraint = AT_CONSTRAINT_BYTE_STORAGE;
            } else if (at_parse_word(constraint, "MutableByteStorage")) {
                type->constraint = AT_CONSTRAINT_MUTABLE_BYTE_STORAGE;
            } else {
                at_parse_error(
                    parser, constraint, "AS3601",
                    "Unknown protocol; supported constraints are Number, Integer, Hashable, "
                    "Equatable, Ordered, ByteStorage and MutableByteStorage");
            }
        }
        if (!at_parse_accept(parser, T_COMMA)) {
            break;
        }
    }
    at_parse_expect(parser, T_RBRACKET, "expected ']' after generic parameters");
    if (!function->generic_count) {
        at_parse_error(parser, at_parse_current(parser), "AS3600",
                       "A generic declaration needs at least one type parameter");
    }
    return 1;
}

static void function(AtParser *r, int owner)
{
    AtFunction *parent = r->f;
    Token t = at_parse_expect(r, T_IDENT, "expected function name");
    AsTypedProject *p = r->p;
    if (p->nfunctions == AT_FUNCTIONS) {
        at_parse_error(r, t, "AS3100", "Too many functions");
        recover(r);
        return;
    }
    AtFunction *f = &p->functions[p->nfunctions++];
    at_parse_name(t, f->name, sizeof f->name);
    f->module = r->m;
    f->token = t;
    f->template_id = -1;
    f->method_owner = owner;
    f->lexical_parent = parent ? (int)(parent - p->functions) + 1 : 0;
    if (!owner && !parent && !strcmp(f->name, "main") && r->implicit_main >= 0) {
        at_parse_error(r, t, "AS3200",
                       "main() is already implied by top-level statements; keep one or the other");
    }
    /* Type parameters are scoped to this declaration, including annotations
     * in its body. The module type lookup must never expose a neighbor's T. */
    r->f = f;
    if (!type_parameters(r, f)) {
        r->f = parent;
        recover_declaration(r);
        return;
    }
    at_parse_expect(r, T_LPAREN, "expected '(' after function name");
    while (!at_parse_has(r, T_RPAREN) && !at_parse_has(r, T_EOF)) {
        Token param = at_parse_expect(r, T_IDENT, "expected parameter name");
        int ty = 0;
        int annotated = at_parse_accept(r, T_COLON);
        if (annotated) {
            ty = at_parse_type(r);
        }
        if (owner && !f->nparams) {
            if (!at_parse_word(param, "self") || (ty && ty != owner)) {
                at_parse_error(r, param, "AS3201",
                               "A method's first parameter must be self of its class type");
            }
            ty = owner;
        }
        /* A present but invalid annotation already has its own diagnostic;
         * saying it was absent turns one misspelling into two unrelated errors. */
        if (!annotated && !ty && ((!parent && f->name[0] != '_') || f->generic_count)) {
            at_parse_error(r, param, "AS3201",
                           "Public and generic functions require explicit parameter types");
        }
        if (f->nparams == AT_ARGS) {
            at_parse_error(r, param, "AS3100", "At most 32 parameters are supported");
            recover(r);
            break;
        }
        AtLocal *l = &f->locals[f->nparams++];
        at_parse_name(param, l->name, sizeof l->name);
        l->type = ty;
        l->initialized = 1;
        l->token = param;
        if (!at_parse_accept(r, T_COMMA)) {
            break;
        }
    }
    at_parse_expect(r, T_RPAREN, "expected ')' after parameters");
    f->nlocals = f->nparams;
    if (owner && !f->nparams) {
        at_parse_error(r, t, "AS3201", "A method requires self as its first parameter");
    }
    if (owner && f->generic_count) {
        at_parse_error(r, t, "AS3900",
                       "Generic methods require specialization of a bound receiver");
    }
    if (parent && f->generic_count) {
        at_parse_error(r, t, "AS3900",
                       "A nested function cannot declare its own type parameters yet");
    }
    if (at_parse_accept(r, T_ARROW)) {
        f->result = at_parse_type(r);
    } else {
        f->result = AT_ERROR;
        f->infer_result = 1;
        if ((!parent && f->name[0] != '_') || f->generic_count) {
            at_parse_error(r, t, "AS3201",
                           "Public and generic functions require an explicit return type");
        }
    }
    r->f = f;
    f->body = block(r);
    r->f = parent;
}

static void structure(AtParser *r, int kind)
{
    Token t = at_parse_expect(r, T_IDENT, "expected structure name");
    AsTypedProject *p = r->p;
    int id = at_allocate_type(p, r->m, t);
    if (!id) {
        recover_declaration(r);
        return;
    }
    AtType *s = &p->types[id];
    s->kind = kind;
    s->module = r->m;
    at_parse_name(t, s->name, sizeof s->name);
    if (at_exception_code(t) >= 0) {
        at_parse_error(r, t, "AS3200", "Exception type names are reserved");
    }
    for (int i = 1; i < p->ntypes - 1; i++) {
        AtType *previous = &p->types[i];
        if (!strcmp(previous->name, s->name) &&
            (i <= AT_BUILTIN_LAST || ((previous->kind == AT_STRUCT || previous->kind == AT_CLASS ||
                                       previous->kind == AT_LAYOUT) &&
                                      previous->module == r->m))) {
            at_parse_error(r, t, "AS3200", "Type is already declared in this module");
        }
    }
    if (at_parse_accept(r, T_LPAREN)) {
        Token base_site = at_parse_current(r);
        int base = at_parse_type(r);
        if (kind != AT_CLASS || p->types[base].kind != AT_CLASS || base == (int)(s - p->types)) {
            at_parse_error(r, base_site, "AS3211",
                           "A class base must be a previously declared class");
        } else {
            /* Inherited fields keep their offsets. Redeclaration is diagnosed
             * by the same duplicate-field check as an ordinary field. */
            s->base = base;
            s->count = p->types[base].count;
            memcpy(s->fields, p->types[base].fields, sizeof s->fields);
            memcpy(s->names, p->types[base].names, sizeof s->names);
            memcpy(s->field_tokens, p->types[base].field_tokens, sizeof s->field_tokens);
        }
        at_parse_expect(r, T_RPAREN, "expected ')' after the single class base");
    }
    at_parse_expect(r, T_COLON, "expected ':' after structure");
    at_parse_expect(r, T_NEWLINE, "expected newline after structure");
    at_parse_expect(r, T_INDENT, "expected structure fields");
    while (!at_parse_has(r, T_DEDENT) && !at_parse_has(r, T_EOF)) {
        int before = r->pos;
        if (at_parse_accept(r, T_ERROR)) {
            newline(r);
            continue;
        }
        if (at_parse_accept(r, T_NEWLINE)) {
            continue;
        }
        if (kind == AT_CLASS && at_parse_accept(r, T_DEF)) {
            function(r, (int)(s - p->types));
            continue;
        }
        if (kind == AT_CLASS && at_parse_word(at_parse_current(r), "pass")) {
            at_parse_advance(r);
            newline(r);
            continue;
        }
        Token field = at_parse_expect(r, T_IDENT, "expected declared field");
        at_parse_expect(r, T_COLON, "expected field type");
        int ty = at_parse_type(r);
        if (s->count == AT_ARGS) {
            at_parse_error(r, field, "AS3200", "At most 32 fields are supported");
        } else {
            at_parse_name(field, s->names[s->count], 64);
            for (int i = 0; i < s->count; i++) {
                if (!strcmp(s->names[i], s->names[s->count])) {
                    at_parse_error(r, field, "AS3200", "Duplicate structure field name");
                }
            }
            s->field_tokens[s->count] = field;
            s->fields[s->count++] = ty;
        }
        newline(r);
        if (before == r->pos) {
            at_parse_advance(r);
        }
    }
    at_parse_expect(r, T_DEDENT, "expected end of structure");
}

/* Each module owns one ordinary native function for its initialization. This
 * preserves source sites and the normal exception/GC paths without running
 * user code while checking imports. Dependency order is recorded by the loader. */
static void module_variable(AtParser *r)
{
    AsTypedProject *p = r->p;
    AtModule *module = &p->modules[r->m];
    if (module->initializer < 0) {
        if (p->nfunctions == AT_FUNCTIONS) {
            at_parse_error(r, at_parse_current(r), "AS3200",
                           "Too many functions for module initializer");
            recover(r);
            return;
        }
        module->initializer = p->nfunctions++;
        AtFunction *initializer = &p->functions[module->initializer];
        strcpy(initializer->name, "__as_initialize_module");
        initializer->module = r->m;
        initializer->token = at_parse_current(r);
        initializer->template_id = -1;
        initializer->module_initializer = 1;
        initializer->result = AT_VOID;
    }
    AtFunction *initializer = &p->functions[module->initializer];
    r->f = initializer;
    AtNode *assignment = statement(r);
    r->f = NULL;
    if (!assignment || (assignment->kind != AN_ASSIGN && assignment->kind != AN_UNPACK)) {
        at_parse_error(r, assignment ? assignment->token : at_parse_current(r), "AS3200",
                       "Module variables require a name and initializer; put other code in main()");
        return;
    }
    int unpack = assignment->kind == AN_UNPACK;
    int count = unpack ? assignment->count : 1;
    for (int index = 0; index < count; index++) {
        AtNode *item = unpack ? assignment->args[index] : assignment;
        if (!item->a || item->a->kind != AN_NAME || item->op != T_ASSIGN ||
            (!item->b && !(unpack && assignment->a))) {
            at_parse_error(r, item->token, "AS3200", "Module variable requires an initializer");
            return;
        }
        AtNode *binding = item->a;
        if (at_global_lookup(p, r->m, binding->token, 0) >= 0) {
            at_parse_error(r, binding->token, "AS3200", "Module variable is already declared");
            return;
        }
        if (p->nglobals == AT_GLOBALS) {
            at_parse_error(r, binding->token, "AS3200", "Too many module variables");
            return;
        }
        int id = p->nglobals++;
        AtGlobal *global = &p->globals[id];
        at_parse_name(binding->token, global->name, sizeof global->name);
        global->module = r->m;
        global->token = binding->token;
        global->type = item->type;
        binding->kind = AN_GLOBAL;
        binding->symbol = id;
    }
    /* Keep a multi-binding initializer as one statement. Flattening its child
     * assignments here would change evaluation order and partially initialize
     * globals when a later right side throws. */
    AtNode **tail = &initializer->body;
    while (*tail) {
        tail = &(*tail)->next;
    }
    *tail = assignment;
}

static char *source_file(AsTypedProject *p, const char *path)
{
    for (int i = 0; i < p->noverlays; i++) {
        if (!strcmp(p->overlays[i].path, path)) {
            size_t n = p->overlays[i].bytes;
            if (n > AT_SOURCE_MAX || memchr(p->overlays[i].source, 0, n)) {
                return NULL;
            }
            char *s = malloc(n + 1);
            if (s) {
                memcpy(s, p->overlays[i].source, n);
                s[n] = 0;
            }
            return s;
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *s = malloc(AT_SOURCE_MAX + 1);
    if (!s) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(s, 1, AT_SOURCE_MAX + 1, f);
    int bad = ferror(f) || n > AT_SOURCE_MAX || memchr(s, 0, n);
    fclose(f);
    if (bad) {
        free(s);
        return NULL;
    }
    s[n] = 0;
    return s;
}

static void report_import_cycle(AsTypedProject *project, int target, int parent, Token site)
{
    /* Diagnostics have a bounded human-readable message, but a cycle may be
     * 64 long paths. Keep the complete chain separately for related locations
     * instead of silently truncating it to the last import edge. */
    int reversed[AT_MODULES];
    int count = 0;
    for (int module = parent; module >= 0 && count < AT_MODULES;
         module = project->modules[module].parent) {
        reversed[count++] = module;
        if (module == target) {
            break;
        }
    }
    if (project->ncycles < AT_MODULES) {
        AtImportCycle *cycle = &project->cycles[project->ncycles++];
        cycle->diagnostic_module = parent;
        cycle->diagnostic_index = project->modules[parent].diagnostics.count;
        for (int i = count - 1; i >= 0; i--) {
            cycle->modules[cycle->count++] = reversed[i];
        }
        cycle->modules[cycle->count++] = target;
    }
    at_error(project, parent, site, "AS3301",
             "Cyclic import; the complete ordered cycle is listed in related locations");
}

static void report_lexical_error(void *context, Token site, const char *message)
{
    AtParser *parser = context;
    at_error(parser->p, parser->m, site, "AS1001", message);
}

static int load_module(AsTypedProject *p, const char *path, int parent, Token site)
{
    /* A module is marked visiting before descending into imports. Re-entering
     * a visiting module is a cycle; reusing a finished module is ordinary DAG
     * sharing. Loading gathers declarations and never executes module code. */
    for (int i = 0; i < p->nmodules; i++) {
        if (!strcmp(p->modules[i].path, path)) {
            if (p->modules[i].state == 1 && parent >= 0) {
                report_import_cycle(p, i, parent, site);
            }
            return i;
        }
    }
    if (p->nmodules == AT_MODULES) {
        if (parent >= 0) {
            at_error(p, parent, site, "AS3300", "Module limit reached");
        }
        return -1;
    }
    int id = p->nmodules++;
    AtModule *m = &p->modules[id];
    m->initializer = -1;
    m->parent = parent;
    snprintf(m->path, sizeof m->path, "%s", path);
    m->source = source_file(p, path);
    int unavailable = m->source == NULL;
    m->state = 1;
    if (!m->source) {
        m->source = calloc(1, 1);
        if (!m->source) {
            p->oom = 1;
            return -1;
        }
    }
    AsDiagnostics saved = as_diagnostics;
    /* Imports arrive through a temporary path buffer in module_import.
     * Diagnostics outlive that frame, so retain the module-owned pathname.
     * JSON happened to use m->path already; plain reports used to print the
     * last imported name for several unrelated source modules. */
    as_diagnostic_begin(m->path, m->source);
    m->diagnostics = as_diagnostics;
    as_diagnostics = saved;
    Token first = {.start = m->source, .len = (int)strcspn(m->source, "\n"), .line = 1};
    if (unavailable) {
        at_error(p, id, first, "AS3303",
                 "Cannot read source: missing/unreadable file, embedded NUL, or size above 1 MiB");
        if (parent >= 0) {
            at_error(p, parent, site, "AS3300", "Imported source could not be loaded");
        }
        m->state = 2;
        return id;
    }
    /* Version 3 never enters the bytecode loader or executes an import as a side
     * effect of checking it. Empty/unreadable modules are diagnosed explicitly. */
    int version = as_source_version(m->source);
    if (version != AS_LANGUAGE_NATIVE) {
        at_error(
            p, id, first, "AS3302",
            version == AS_LANGUAGE_RETIRED
                ? as_version_error(version)
                : "Native modules require '# aether: 3.0' (or '3'); A2 imports need migration");
        if (parent >= 0) {
            at_error(p, parent, site, "AS3300",
                     "Imported module is unavailable or is not language version 3");
        }
        m->state = 2;
        return id;
    }
    AsLexError lexical;
    AtParser r = {.p = p, .m = id, .implicit_main = -1};
    m->tokens = as_lex_recover(m->source, &m->count, &lexical, report_lexical_error, &r, NULL);
    if (!m->tokens) {
        Token site = {.start = lexical.start ? lexical.start : m->source, .len = 1, .line = 1};
        at_error(p, id, site, "AS1001", lexical.message);
        p->oom |= lexical.out_of_memory;
        m->state = 2;
        return id;
    }
    while (!at_parse_has(&r, T_EOF) && !p->oom) {
        int before = r.pos;
        if (at_parse_accept(&r, T_NEWLINE)) {
            continue;
        }
        if (at_parse_accept(&r, T_ERROR)) {
            newline(&r);
        } else if (at_parse_accept(&r, T_IMPORT)) {
            module_import(&r, 0);
        } else if (at_parse_accept(&r, T_FROM)) {
            module_import(&r, 1);
        } else if (at_parse_accept(&r, T_DEF)) {
            function(&r, 0);
        } else if (at_parse_accept(&r, T_CLASS)) {
            structure(&r, AT_CLASS);
        } else if (at_parse_word(at_parse_current(&r), "struct")) {
            at_parse_advance(&r);
            structure(&r, AT_STRUCT);
        } else if (at_parse_accept(&r, T_INDENT)) {
            parse_script_suite(&r);
        } else if (at_parse_has(&r, T_IF) || at_parse_has(&r, T_FOR) || at_parse_has(&r, T_WHILE) ||
                   at_parse_has(&r, T_TRY) || at_parse_has(&r, T_RAISE) ||
                   at_parse_has(&r, T_WITH) || at_parse_has(&r, T_RETURN)) {
            parse_script_statement(&r);
        } else if (at_parse_has(&r, T_IDENT)) {
            if (!at_parse_layout(&r)) {
                if (module_ident_is_variable(&r)) {
                    module_variable(&r);
                } else {
                    parse_script_statement(&r);
                }
            }
        } else {
            at_parse_error(
                &r, at_parse_current(&r), "AS3900",
                "Native module scope currently accepts imports, structs, functions and "
                "top-level statements that become main()");
            recover(&r);
        }
        if (before == r.pos) {
            at_parse_advance(&r);
        }
    }
    m->state = 2;
    if (m->initializer >= 0) {
        p->initialization_order[p->ninitializers++] = m->initializer;
    }
    return id;
}

AsTypedProject *as_typed_check(const char *entry, const AsSourceOverlay *overlays, int count)
{
    return as_typed_check_with_library(entry, overlays, count, "/usr/as/lib");
}

AsTypedProject *as_typed_check_with_library(const char *entry, const AsSourceOverlay *overlays,
                                            int count, const char *library)
{
    AsTypedProject *p = calloc(1, sizeof *p);
    if (!p) {
        return NULL;
    }
    p->overlays = overlays;
    p->noverlays = count;
    if (library) {
        if (strlen(library) >= sizeof p->library) {
            free(p);
            return NULL;
        }
        snprintf(p->library, sizeof p->library, "%s", library);
    }
    const char *names[] = {"<error>", "None", "bool", "i8",       "i16",     "i32",
                           "i64",     "u8",   "u16",  "u32",      "u64",     "f32",
                           "f64",     "str",  "Any",  "NoneType", "Range",   "Buffer",
                           "Bytes",   "Cap",  "Port", "Command",  "Process", "Region"};
    p->ntypes = AT_BUILTIN_LAST + 1;
    for (int i = 0; i < p->ntypes; i++) {
        p->types[i].kind = i;
        strcpy(p->types[i].name, names[i]);
    }
    at_initialize_exceptions(p);
    load_module(p, entry, -1, (Token){0});
    p->overlays = NULL;
    p->noverlays = 0;
    if (!as_typed_errors(p)) {
        at_check_functions(p);
    }
    return p;
}

int as_typed_errors(const AsTypedProject *p)
{
    if (!p) {
        return 1;
    }
    int n = p->oom;
    for (int i = 0; i < p->nmodules; i++) {
        n += p->modules[i].diagnostics.count;
    }
    return n;
}

void as_typed_free(AsTypedProject *p)
{
    if (!p) {
        return;
    }
    for (int i = 0; i < p->nmodules; i++) {
        free(p->modules[i].source);
        free(p->modules[i].tokens);
    }
    for (int i = 0; i < p->nnodes; i++) {
        free(p->nodes[i]->args);
        free(p->nodes[i]);
    }
    free(p->nodes);
    free(p);
}
