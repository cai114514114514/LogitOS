/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <string.h>

int at_closure_capture(Checker *checker, Token name)
{
    AtFunction *function = checker->f;
    if (!function->lexical_parent) {
        return -1;
    }
    AtFunction *parent = &checker->p->functions[function->lexical_parent - 1];
    Checker *outer = parent->active_checker;
    if (!outer) {
        return -1;
    }
    /* Resolving through the parent's live checker also preserves expression
     * scopes: a lambda in a comprehension captures that comprehension binding,
     * and a grandchild makes its otherwise-unused middle parent forward a cell. */
    int parent_slot = at_scope_resolve_local(outer, name);
    if (parent_slot < 0) {
        return -1;
    }
    AtLocal *source = &parent->locals[parent_slot];
    if (at_resource_type(source->type) || source->scoped_borrow) {
        at_error(checker->p, function->module, name, "AS3401",
                 "A closure cannot capture a scoped resource owner");
    }
    AtNode *construction = outer->creating_closure;
    int recursive =
        construction && construction->op && at_scope_name_equal(construction->token, source->name);
    if (!(source->initialized & AT_LOCAL_INITIALIZED) && !recursive) {
        at_error(checker->p, function->module, name, "AS3206",
                 "Closure captures a variable before it is initialized");
    }
    if (!parent_slot && parent->method_owner) {
        at_class_require_initialized(outer, construction);
    }
    AtNode binding = {.token = name, .module = function->module};
    int slot = at_scope_add_local(checker, &binding, source->type);
    if (slot < 0) {
        return -1;
    }
    AtLocal *capture = &function->locals[slot];
    capture->capture_parent = parent_slot + 1;
    capture->capture_index = function->capture_count++;
    capture->captured = 1;
    capture->initialized = AT_LOCAL_INITIALIZED;
    source->captured = 1;
    return slot;
}

int at_check_closure(Checker *checker, AtNode *node, int expected)
{
    AtFunction *function = &checker->p->functions[node->symbol];
    AtType *context = &checker->p->types[expected];
    if (context->kind == AT_CALLABLE && context->count == function->nparams) {
        for (int index = 0; index < function->nparams; index++) {
            if (!function->locals[index].type) {
                function->locals[index].type = context->fields[index];
            }
        }
        if (function->infer_result && !function->result &&
            !at_has_parameter(checker->p, context->element)) {
            function->result = context->element;
        }
    }
    int parameters[AT_ARGS];
    for (int index = 0; index < function->nparams; index++) {
        parameters[index] = function->locals[index].type;
        if (!parameters[index]) {
            at_check_error(checker, node, "AS3201",
                           "Closure parameters require annotations or a Callable context");
            return AT_ERROR;
        }
    }
    if (node->op && function->result) {
        /* Publish an annotated named function signature before checking its
         * recursive body. Its cell receives the actual closure only at runtime;
         * no ordinary right-side read acquires an initialization fact here. */
        int slot = at_scope_local(checker->f, node->token);
        if (slot >= 0 && !checker->f->locals[slot].type) {
            checker->f->locals[slot].type =
                at_callable_type(checker->p, parameters, function->nparams, function->result,
                                 node->module, node->token);
        }
    }
    AtNode *saved = checker->creating_closure;
    checker->creating_closure = node;
    at_check_function(checker->p, function);
    checker->creating_closure = saved;
    return at_callable_type(checker->p, parameters, function->nparams, function->result,
                            node->module, node->token);
}
