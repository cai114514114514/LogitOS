/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <stdio.h>
#include <string.h>

int at_callable_type(AsTypedProject *project, const int *parameters, int count, int result,
                     int module, Token site)
{
    if (!result || count < 0 || count > AT_ARGS) {
        return AT_ERROR;
    }
    for (int i = 0; i < count; i++) {
        if (!parameters[i]) {
            return AT_ERROR;
        }
        if (parameters[i] == AT_VOID) {
            at_error(project, module, site, "AS3202", "Callable parameters require value types");
            return AT_ERROR;
        }
    }
    for (int i = 0; i < project->ntypes; i++) {
        AtType *type = &project->types[i];
        if (type->kind == AT_CALLABLE && type->count == count && type->element == result &&
            !memcmp(type->fields, parameters, (size_t)count * sizeof(int))) {
            return i;
        }
    }
    int id = at_allocate_type(project, module, site);
    if (!id) {
        return AT_ERROR;
    }
    AtType *type = &project->types[id];
    type->kind = AT_CALLABLE;
    type->module = module;
    type->count = count;
    type->element = result;
    memcpy(type->fields, parameters, (size_t)count * sizeof(int));
    strcpy(type->name, "Callable[[");
    for (int i = 0; i < count; i++) {
        size_t used = strlen(type->name);
        snprintf(type->name + used, sizeof type->name - used, "%s%s", i ? ", " : "",
                 project->types[parameters[i]].name);
    }
    size_t used = strlen(type->name);
    snprintf(type->name + used, sizeof type->name - used, "], %s]", project->types[result].name);
    return id;
}

int at_check_function_value(Checker *checker, AtNode *node, int declaration, int expected)
{
    AsTypedProject *project = checker->p;
    AtFunction *function = &project->functions[declaration];
    AtType *context = &project->types[expected];
    int id = declaration;
    if (function->generic_count) {
        if (context->kind != AT_CALLABLE || context->count != function->nparams) {
            at_check_error(checker, node, "AS3602",
                           "Generic function values require a matching Callable context");
            return AT_ERROR;
        }
        int bindings[AT_TYPE_PARAMETERS] = {0};
        int valid = 1;
        for (int i = 0; i < function->nparams; i++) {
            valid &= at_bind_type(project, function, function->locals[i].type, context->fields[i],
                                  bindings, node);
        }
        int contextual[AT_TYPE_PARAMETERS];
        memcpy(contextual, bindings, sizeof bindings);
        if (at_bind_result_context(project, function, function->result, context->element,
                                   contextual)) {
            memcpy(bindings, contextual, sizeof bindings);
        }
        if (!valid) {
            return AT_ERROR;
        }
        at_check_function(project, function);
        int result;
        id = at_specialize(project, declaration, bindings, node, &result);
        if (id < 0) {
            return AT_ERROR;
        }
        if (id == declaration) {
            /* Inside another template no code address exists yet. Retain its
             * declaration and substitute the signature; cloning resolves it
             * again when the enclosing function gets concrete arguments. */
            int parameters[AT_ARGS];
            for (int i = 0; i < function->nparams; i++) {
                parameters[i] =
                    at_substitute_type(project, function, function->locals[i].type, bindings);
            }
            node->kind = AN_FUNCTION;
            node->op = declaration + 1;
            node->symbol = declaration;
            return at_callable_type(project, parameters, function->nparams, result, node->module,
                                    node->token);
        }
        function = &project->functions[id];
    } else if (context->kind == AT_CALLABLE && context->count == function->nparams) {
        for (int i = 0; i < function->nparams; i++) {
            if (!function->locals[i].type) {
                function->locals[i].type = context->fields[i];
            }
        }
    }
    at_check_function(project, function);
    int parameters[AT_ARGS];
    for (int i = 0; i < function->nparams; i++) {
        parameters[i] = function->locals[i].type;
        if (!parameters[i]) {
            at_check_error(checker, node, "AS3201",
                           "Function value requires an inferred or annotated signature");
            return AT_ERROR;
        }
    }
    node->kind = AN_FUNCTION;
    node->op = declaration + 1;
    node->symbol = id;
    return at_callable_type(project, parameters, function->nparams, function->result, node->module,
                            node->token);
}

int at_check_indirect_call(Checker *checker, AtNode *node)
{
    if (node->a->kind == AN_CLOSURE) {
        /* An immediately invoked lambda has no assignment annotation, but its
         * concrete arguments can supply parameter types. This is only checking
         * order; the emitter still constructs the callee before its arguments. */
        AtFunction *function = &checker->p->functions[node->a->symbol];
        for (int index = 0; index < node->count && index < function->nparams; index++) {
            if (!function->locals[index].type) {
                function->locals[index].type = at_check_expression(checker, node->args[index], 0);
            }
        }
    }
    int callee = at_check_expression(checker, node->a, AT_ERROR);
    AtType *signature = &checker->p->types[callee];
    if (signature->kind != AT_CALLABLE) {
        at_check_error(checker, node->a, "AS3202", "This value is not callable");
        for (int i = 0; i < node->count; i++) {
            at_check_expression(checker, node->args[i], 0);
        }
        return AT_ERROR;
    }
    node->symbol = AT_CALL_INDIRECT;
    if (node->count != signature->count) {
        at_check_error(checker, node, "AS3204",
                       "Argument count differs from the Callable signature");
    }
    for (int i = 0; i < node->count; i++) {
        int expected = i < signature->count ? signature->fields[i] : AT_ERROR;
        int actual = at_check_borrow_argument(checker, node->args[i], expected);
        AtType *want = &checker->p->types[expected], *found = &checker->p->types[actual];
        if (want->kind == AT_SLICE && found->kind == AT_ARRAY && want->element == found->element) {
            continue;
        }
        at_check_mismatch(checker, node->args[i], expected, actual);
    }
    if (node->a->kind == AN_METHOD && node->a->a->kind == AN_SUPER &&
        !strcmp(checker->p->functions[node->a->symbol].name, "init")) {
        /* A successful parent init establishes inherited fields. Exception
         * flow restores the pre-try mask if the call throws instead. */
        int owner = checker->p->functions[node->a->symbol].method_owner;
        int count = checker->p->types[owner].count;
        checker->f->initialized_fields |= count == 32 ? UINT32_MAX : (1u << count) - 1;
    }
    return signature->element;
}
