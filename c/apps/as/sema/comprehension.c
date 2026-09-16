/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <stdio.h>
#include <string.h>

static int iteration_type(Checker *checker, AtNode *source)
{
    int type = at_check_resource_receiver(checker, source);
    AtType *iterable = &checker->p->types[type];
    if (at_check_byte_storage(checker->p, type, 0)) {
        return AT_I64;
    }
    switch (iterable->kind) {
    case AT_RANGE:
    case AT_LAYOUT:
    case AT_BUFFER:
    case AT_BYTES:
        return AT_I64;
    case AT_STR:
        return AT_STR;
    case AT_ARRAY:
    case AT_LIST:
    case AT_SLICE:
    case AT_MUT_SLICE:
        return iterable->element;
    case AT_DICT:
        return iterable->key;
    case AT_ERROR:
        return AT_ERROR;
    default:
        at_check_error(checker, source, "AS3202", "Comprehension requires an iterable value");
        return AT_ERROR;
    }
}

static int binding_slot(Checker *checker, AtNode *node, int type)
{
    /* A source variable can shadow a parameter, import or another expression
     * scope. Its hidden slot uses an unspellable name and never joins the
     * function-wide namespace. Rechecking the same node reuses the slot. */
    char name[64];
    snprintf(name, sizeof name, "$comprehension%d", node->id);
    AtFunction *function = checker->f;
    for (int index = 0; index < function->nlocals; index++) {
        if (!strcmp(function->locals[index].name, name)) {
            function->locals[index].type = type;
            return index;
        }
    }
    int index = at_scope_add_local(checker, node->args[0], type);
    if (index >= 0) {
        strcpy(function->locals[index].name, name);
    }
    return index;
}

int at_check_comprehension(Checker *checker, AtNode *node, int expected)
{
    /* The iterable is evaluated outside the new binding, just like Python's
     * [x for x in x]. Filter and element see the binding; nothing escapes it. */
    int item_type = iteration_type(checker, node->b);
    if (!item_type || !node->args[0]) {
        return AT_ERROR;
    }
    int slot = binding_slot(checker, node, item_type);
    if (slot < 0) {
        return AT_ERROR;
    }
    node->args[0]->type = item_type;
    node->args[0]->value_type = item_type;
    node->args[0]->symbol = slot;
    int before[AT_LOCALS] = {0};
    for (int index = 0; index < checker->f->nlocals; index++) {
        before[index] = checker->f->locals[index].initialized;
    }
    checker->f->locals[slot].initialized = AT_LOCAL_INITIALIZED;
    AtScopedBinding binding = {node->args[0]->token, slot, checker->bindings};
    checker->bindings = &binding;
    if (node->c) {
        at_check_mismatch(checker, node->c, AT_BOOL,
                          at_check_expression(checker, node->c, AT_BOOL));
        at_optional_assume(checker, node->c, 1);
    }
    int element = 0;
    if (expected) {
        if (checker->p->types[expected].kind != AT_LIST) {
            at_check_error(checker, node, "AS3202", "A comprehension produces a List[T]");
        } else {
            element = checker->p->types[expected].element;
        }
    }
    int actual = at_check_expression(checker, node->a, element);
    if (element) {
        at_check_mismatch(checker, node->a, element, actual);
    }
    checker->bindings = binding.previous;
    for (int index = 0; index < checker->f->nlocals; index++) {
        checker->f->locals[index].initialized =
            before[index] | (checker->f->locals[index].capture_parent ? AT_LOCAL_INITIALIZED : 0);
    }
    return at_compound_type(checker->p, AT_LIST, element ? element : actual, 0, node->module,
                            node->token);
}
