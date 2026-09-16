/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <string.h>

int at_class_subtype(AsTypedProject *project, int actual, int expected)
{
    if (project->types[actual].kind != AT_CLASS || project->types[expected].kind != AT_CLASS) {
        return 0;
    }
    for (int type = actual; type; type = project->types[type].base) {
        if (type == expected) {
            return 1;
        }
    }
    return 0;
}

void at_class_check_overrides(AsTypedProject *project)
{
    for (int id = 0; id < project->nfunctions; id++) {
        AtFunction *method = &project->functions[id];
        int base = project->types[method->method_owner].base;
        if (!base || !strcmp(method->name, "init")) {
            continue;
        }
        int inherited = at_class_method(project, base, method->token);
        if (inherited < 0) {
            continue;
        }
        AtFunction *parent = &project->functions[inherited];
        int same = method->nparams == parent->nparams && method->result == parent->result;
        for (int parameter = 1; parameter < method->nparams && parameter < parent->nparams;
             parameter++) {
            same &= method->locals[parameter].type == parent->locals[parameter].type;
        }
        /* Exact signatures keep every vtable entry ABI-compatible. Constructors
         * are selected statically and may deliberately take different inputs. */
        if (!same || method->infer_result || parent->infer_result) {
            at_error(
                project, method->module, method->token, "AS3211",
                "An override must declare the same parameter and return types as its base method");
        }
    }
}

int at_class_super(Checker *checker, AtNode *node)
{
    int owner = at_class_lexical_owner(checker->p, checker->f);
    int base = checker->p->types[owner].base;
    if (!base || !checker->field_receiver) {
        at_check_error(checker, node, "AS3211",
                       "super is only a method receiver inside a derived class");
        return AT_ERROR;
    }
    Token self = {.start = "self", .len = 4};
    node->symbol = at_scope_resolve_local(checker, self);
    if (at_class_receiver_owner(checker->p, checker->f, node->symbol) != owner) {
        at_check_error(checker, node, "AS3211", "super requires its enclosing method's self");
        return AT_ERROR;
    }
    return base;
}

int at_class_lexical_owner(AsTypedProject *project, AtFunction *function)
{
    while (!function->method_owner && function->lexical_parent) {
        function = &project->functions[function->lexical_parent - 1];
    }
    return function->method_owner;
}

int at_class_receiver_owner(AsTypedProject *project, AtFunction *function, int slot)
{
    /* The spelling self is insufficient: a nested parameter or comprehension
     * can shadow it. Follow cell identity back to the declaring method so
     * neither super nor a write can mistake another local for its receiver. */
    while (slot >= 0 && slot < function->nlocals) {
        AtLocal *local = &function->locals[slot];
        if (!local->capture_parent || !function->lexical_parent) {
            return slot == 0 ? function->method_owner : 0;
        }
        slot = local->capture_parent - 1;
        function = &project->functions[function->lexical_parent - 1];
    }
    return 0;
}
