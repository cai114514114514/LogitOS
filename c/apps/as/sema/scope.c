/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <stdio.h>
#include <string.h>

/* All source-name resolution lives here. The checker and emitted IR consume
 * these binding IDs; neither code generation nor Studio needs to guess names. */

static int declared_type(AsTypedProject *project, int module, const char *name)
{
    for (int i = 1; i < project->ntypes; i++) {
        AtType *type = &project->types[i];
        if (!strcmp(type->name, name) &&
            (i <= AT_BUILTIN_LAST ||
             ((type->kind == AT_STRUCT || type->kind == AT_CLASS || type->kind == AT_LAYOUT) &&
              type->module == module))) {
            return i;
        }
    }
    return AT_ERROR;
}

int at_named_type(AsTypedProject *project, int module, const char *name)
{
    Token token = {.start = name, .len = (int)strlen(name)};
    if (at_exception_code(token) >= 0) {
        return project->exception_type;
    }
    int type = declared_type(project, module, name);
    if (type) {
        return type;
    }
    AtModule *scope = &project->modules[module];
    for (int i = 0; i < scope->nimports; i++) {
        AtImport *import = &scope->imports[i];
        if (import->target >= 0 && import->member[0] && !strcmp(import->name, name)) {
            return declared_type(project, import->target, import->member);
        }
    }
    return AT_ERROR;
}

int at_scope_name_equal(Token t, const char *s)
{
    return t.len == (int)strlen(s) && !memcmp(t.start, s, (size_t)t.len);
}

void at_check_error(Checker *c, AtNode *n, const char *code, const char *s)
{
    if (n) {
        at_error(c->p, n->module, n->token, code, s);
    }
}

void at_check_mismatch(Checker *c, AtNode *n, int want, int got)
{
    /* AT_ERROR is already diagnosed at its source. Propagating it avoids one
     * unknown variable producing a second error at every enclosing operator. */
    if (want && got && want != got) {
        char s[256];
        snprintf(s, sizeof s,
                 "Expected %s, found %s; implicit numeric/dynamic conversion is not allowed",
                 c->p->types[want].name, c->p->types[got].name);
        at_check_error(c, n, "AS3202", s);
    }
}

int at_scope_local(AtFunction *f, Token t)
{
    for (int i = 0; i < f->nlocals; i++) {
        if (at_scope_name_equal(t, f->locals[i].name)) {
            return i;
        }
    }
    return -1;
}

int at_global_lookup(AsTypedProject *project, int module, Token name, int imports)
{
    for (int i = 0; i < project->nglobals; i++) {
        if (project->globals[i].module == module &&
            at_scope_name_equal(name, project->globals[i].name)) {
            return i;
        }
    }
    if (imports) {
        AtModule *scope = &project->modules[module];
        for (int i = 0; i < scope->nimports; i++) {
            AtImport *import = &scope->imports[i];
            if (import->target >= 0 && import->member[0] &&
                at_scope_name_equal(name, import->name)) {
                Token member = {.start = import->member, .len = (int)strlen(import->member)};
                return at_global_lookup(project, import->target, member, 0);
            }
        }
    }
    return -1;
}

int at_scope_explicit_global(Checker *checker, Token name)
{
    for (int i = 0; i < checker->f->nglobals; i++) {
        int id = checker->f->globals[i];
        if (at_scope_name_equal(name, checker->p->globals[id].name)) {
            return id;
        }
    }
    return -1;
}

int at_scope_read_global(Checker *checker, AtNode *node, int id)
{
    AtGlobal *global = &checker->p->globals[id];
    node->kind = AN_GLOBAL;
    node->symbol = id;
    if (checker->f->module_initializer && !global->initialized) {
        at_check_error(checker, node, "AS3206", "Module variable is read before its initializer");
    } else if (!global->type) {
        at_check_error(
            checker, node, "AS3201",
            "Module variable type is not known yet; annotate it when initializer calls refer back "
            "to it");
    }
    return global->type;
}

int at_scope_import_module(Checker *checker, Token name)
{
    if (at_scope_resolve_local(checker, name) >= 0 ||
        at_global_lookup(checker->p, checker->f->module, name, 1) >= 0) {
        return -1;
    }
    AtModule *module = &checker->p->modules[checker->f->module];
    for (int index = 0; index < module->nimports; index++) {
        AtImport *import = &module->imports[index];
        if (!import->member[0] && at_scope_name_equal(name, import->name)) {
            return import->target;
        }
    }
    return -1;
}

int at_scope_qualified_global(Checker *checker, AtNode *node)
{
    if (!node->a || node->a->kind != AN_NAME ||
        at_scope_resolve_local(checker, node->a->token) >= 0 ||
        at_global_lookup(checker->p, checker->f->module, node->a->token, 1) >= 0) {
        return -1;
    }
    AtModule *scope = &checker->p->modules[checker->f->module];
    for (int i = 0; i < scope->nimports; i++) {
        AtImport *import = &scope->imports[i];
        if (import->target >= 0 && !import->member[0] &&
            at_scope_name_equal(node->a->token, import->name)) {
            int id = at_global_lookup(checker->p, import->target, node->token, 0);
            if (id >= 0 && checker->p->globals[id].name[0] == '_') {
                at_check_error(checker, node, "AS3300",
                               "Private module variables cannot be imported");
            }
            return id;
        }
    }
    return -1;
}

int at_scope_resolve_local(Checker *checker, Token token)
{
    for (AtScopedBinding *binding = checker->bindings; binding; binding = binding->previous) {
        if (binding->name.len == token.len &&
            !memcmp(binding->name.start, token.start, (size_t)token.len)) {
            return binding->local;
        }
    }
    int slot = at_scope_local(checker->f, token);
    return slot >= 0 ? slot : at_closure_capture(checker, token);
}

int at_scope_add_local(Checker *c, AtNode *n, int type)
{
    AtFunction *f = c->f;
    if (f->nlocals == AT_LOCALS) {
        at_check_error(c, n, "AS3200", "Too many local variables");
        return -1;
    }
    int i = f->nlocals++;
    AtLocal *l = &f->locals[i];
    int len = n->token.len;
    if (len >= 64) {
        at_check_error(c, n, "AS3200", "Identifier exceeds 63 bytes");
        len = 63;
    }
    memcpy(l->name, n->token.start, (size_t)len);
    l->name[len] = 0;
    l->token = n->token;
    l->type = type;
    return i;
}

/* Binding is a function-wide decision, independent of control-flow visitation.
 * Without this pass, `print(x); x = 1` would silently read module x, and a name
 * assigned only in the second branch would look global in the first branch. */
void at_scope_collect_bindings(Checker *checker)
{
    AsTypedProject *project = checker->p;
    AtFunction *function = checker->f;
    int owner = (int)(function - project->functions);
    function->nglobals = 0;
    for (int i = 0; i < project->nnodes; i++) {
        AtNode *node = project->nodes[i];
        if (node->function != owner || node->kind != AN_GLOBAL_DECL) {
            continue;
        }
        for (int j = 0; j < node->count; j++) {
            AtNode *name = node->args[j];
            int id = at_global_lookup(project, function->module, name->token, 0);
            if (id < 0) {
                at_check_error(checker, name, "AS3205",
                               "global requires a variable declared in this module");
            } else if (at_scope_local(function, name->token) >= 0 &&
                       at_scope_local(function, name->token) < function->nparams) {
                at_check_error(checker, name, "AS3200", "A parameter cannot also be global");
            } else if (at_scope_explicit_global(checker, name->token) < 0) {
                if (function->nglobals == AT_ARGS) {
                    at_check_error(checker, name, "AS3200",
                                   "Too many global bindings in one function");
                } else {
                    function->globals[function->nglobals++] = id;
                }
            }
        }
    }
    for (int i = 0; i < project->nnodes; i++) {
        AtNode *node = project->nodes[i];
        if (node->function != owner ||
            (node->kind != AN_ASSIGN && node->kind != AN_FOR && node->kind != AN_EXCEPT)) {
            continue;
        }
        AtNode *name = node->a;
        if (!name || name->kind != AN_NAME) {
            continue;
        }
        int global = at_scope_explicit_global(checker, name->token);
        if (global >= 0) {
            name->kind = AN_GLOBAL;
            name->symbol = global;
        } else if (at_scope_local(function, name->token) < 0) {
            int definition =
                node->kind == AN_ASSIGN && node->b && node->b->kind == AN_CLOSURE && node->b->op;
            if (definition || at_closure_capture(checker, name->token) < 0) {
                at_scope_add_local(checker, name, 0);
            }
        }
    }
}

int at_scope_function(Checker *c, AtNode *n)
{
    /* Resolve qualified imports before looking up the member. Do not search
     * every loaded module: loading a dependency does not make its names public
     * in the caller's scope. */
    int module = c->f->module;
    Token t = n->token;
    if (n->kind == AN_FIELD && n->a && n->a->kind == AN_NAME) {
        int target = at_scope_import_module(c, n->a->token);
        if (target < 0) {
            return -1;
        }
        module = target;
        if (t.len && t.start[0] == '_') {
            at_check_error(c, n, "AS3300",
                           "Private functions cannot be accessed through another module");
            return -1;
        }
    } else if (n->kind != AN_NAME) {
        return -1;
    }
    for (int i = 0; i < c->p->nfunctions; i++) {
        if (!c->p->functions[i].module_initializer && !c->p->functions[i].method_owner &&
            !c->p->functions[i].lexical_parent && c->p->functions[i].module == module &&
            at_scope_name_equal(t, c->p->functions[i].name)) {
            return i;
        }
    }
    if (n->kind == AN_NAME) {
        AtModule *m = &c->p->modules[module];
        for (int i = 0; i < m->nimports; i++) {
            if (at_scope_name_equal(t, m->imports[i].name) && m->imports[i].member[0]) {
                for (int j = 0; j < c->p->nfunctions; j++) {
                    if (!c->p->functions[j].module_initializer &&
                        !c->p->functions[j].method_owner && !c->p->functions[j].lexical_parent &&
                        c->p->functions[j].module == m->imports[i].target &&
                        !strcmp(c->p->functions[j].name, m->imports[i].member)) {
                        return j;
                    }
                }
            }
        }
    }
    return -1;
}

int at_scope_constructor(Checker *checker, AtNode *node)
{
    char spelling[96];
    if (node->token.len >= (int)sizeof spelling) {
        return AT_ERROR;
    }
    memcpy(spelling, node->token.start, (size_t)node->token.len);
    spelling[node->token.len] = 0;
    if (node->kind == AN_NAME) {
        return at_named_type(checker->p, checker->f->module, spelling);
    }
    if (node->kind != AN_FIELD || !node->a || node->a->kind != AN_NAME ||
        at_scope_resolve_local(checker, node->a->token) >= 0 ||
        at_global_lookup(checker->p, checker->f->module, node->a->token, 1) >= 0) {
        return AT_ERROR;
    }
    AtModule *module = &checker->p->modules[checker->f->module];
    for (int i = 0; i < module->nimports; i++) {
        AtImport *import = &module->imports[i];
        if (import->target >= 0 && !import->member[0] &&
            at_scope_name_equal(node->a->token, import->name)) {
            if (spelling[0] == '_') {
                at_check_error(checker, node, "AS3300",
                               "Private types cannot be constructed from another module");
                return AT_ERROR;
            }
            return declared_type(checker->p, import->target, spelling);
        }
    }
    return AT_ERROR;
}

void at_scope_check_imports(AsTypedProject *project)
{
    /* A module has one value namespace. Accepting an import, variable and
     * function with the same spelling made reads and calls select different
     * declarations, which an editor could not represent consistently. */
    for (int i = 0; i < project->nglobals; i++) {
        AtGlobal *global = &project->globals[i];
        if (declared_type(project, global->module, global->name) > AT_BUILTIN_LAST) {
            at_error(project, global->module, global->token, "AS3200",
                     "Module variable conflicts with a type declaration");
        }
        for (int j = 0; j < project->nfunctions; j++) {
            AtFunction *function = &project->functions[j];
            if (!function->module_initializer && !function->method_owner &&
                !function->lexical_parent && function->module == global->module &&
                !strcmp(function->name, global->name)) {
                at_error(project, global->module, global->token, "AS3200",
                         "Module variable conflicts with a function declaration");
            }
        }
    }
    for (int i = 0; i < project->nfunctions; i++) {
        AtFunction *function = &project->functions[i];
        if (!function->module_initializer && !function->method_owner && !function->lexical_parent &&
            declared_type(project, function->module, function->name) > AT_BUILTIN_LAST) {
            at_error(project, function->module, function->token, "AS3200",
                     "Function conflicts with a type declaration");
        }
    }
    for (int module = 0; module < project->nmodules; module++) {
        AtModule *scope = &project->modules[module];
        for (int i = 0; i < scope->nimports; i++) {
            AtImport *import = &scope->imports[i];
            Token binding = {.start = import->name, .len = (int)strlen(import->name)};
            int conflict = at_global_lookup(project, module, binding, 0) >= 0;
            conflict |= declared_type(project, module, import->name) > AT_BUILTIN_LAST;
            for (int j = 0; j < project->nfunctions; j++) {
                AtFunction *function = &project->functions[j];
                conflict |= !function->module_initializer && !function->method_owner &&
                            !function->lexical_parent && function->module == module &&
                            !strcmp(function->name, import->name);
            }
            if (conflict) {
                at_error(project, module, import->token, "AS3300",
                         "Import conflicts with a module declaration");
            }
            for (int previous = 0; previous < i; previous++) {
                if (!strcmp(scope->imports[previous].name, import->name)) {
                    at_error(project, module, import->token, "AS3300",
                             "Import name is already bound");
                }
            }
            if (import->target < 0 || !import->member[0]) {
                continue;
            }
            int found = declared_type(project, import->target, import->member) > AT_ANY;
            Token member = {.start = import->member, .len = (int)strlen(import->member)};
            found |= at_global_lookup(project, import->target, member, 0) >= 0;
            for (int function = 0; function < project->nfunctions; function++) {
                AtFunction *candidate = &project->functions[function];
                found |= !candidate->method_owner && !candidate->lexical_parent &&
                         candidate->module == import->target &&
                         !strcmp(candidate->name, import->member);
            }
            /* Imports are declarations, so a misspelled member is an error
             * even when no expression happens to use it yet. */
            if (!found) {
                at_error(project, module, import->token, "AS3300",
                         "Imported module has no public declaration with this name");
            }
        }
    }
}
