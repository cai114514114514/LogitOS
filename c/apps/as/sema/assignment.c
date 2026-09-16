/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

static int sequence_element(Checker *checker, AtNode *node)
{
    int type = at_check_resource_receiver(checker, node->a);
    AtType *sequence = &checker->p->types[type];
    if (at_check_byte_storage(checker->p, type, 0)) {
        return AT_I64;
    }
    switch (sequence->kind) {
    case AT_RANGE:
    case AT_LAYOUT:
    case AT_BUFFER:
    case AT_BYTES:
        return AT_I64;
    case AT_STR:
        return AT_STR;
    case AT_ARRAY:
        if (sequence->count != node->count) {
            at_check_error(checker, node, "AS3204", "Array length differs from assignment count");
        }
        /* fall through */
    case AT_SLICE:
    case AT_MUT_SLICE:
    case AT_LIST:
        return sequence->element;
    case AT_ERROR:
        return AT_ERROR;
    default:
        at_check_error(checker, node->a, "AS3202", "Unpacking requires a sequence value");
        return AT_ERROR;
    }
}

void at_check_unpack(Checker *checker, AtNode *node)
{
    int element = node->a ? sequence_element(checker, node) : AT_ERROR;
    for (int index = 0; index < node->count; index++) {
        AtNode *assignment = node->args[index], *target = assignment->a;
        int *type;
        if (target->kind == AN_GLOBAL) {
            type = &checker->p->globals[target->symbol].type;
        } else {
            int slot = at_scope_resolve_local(checker, target->token);
            if (slot < 0) {
                slot = at_scope_add_local(checker, target, 0);
            }
            if (slot < 0) {
                continue;
            }
            if (at_class_receiver_owner(checker->p, checker->f, slot)) {
                at_check_error(checker, target, "AS3210",
                               "A method cannot rebind its self parameter");
            }
            target->symbol = slot;
            if (checker->f->locals[slot].scoped_borrow) {
                at_check_error(checker, target, "AS3401",
                               "A scoped view binding cannot be reassigned");
            }
            type = &checker->f->locals[slot].type;
        }
        int actual;
        if (node->a) {
            actual = at_check_optional_conversion(checker, assignment, element, *type);
        } else {
            actual = at_check_expression(checker, assignment->b, *type);
        }
        if (actual == AT_VOID) {
            at_check_error(checker, assignment, "AS3202", "Assignment requires a value");
        }
        if (*type) {
            at_check_mismatch(checker, assignment, *type, actual);
        } else {
            *type = actual;
        }
        target->type = assignment->type = *type;
    }
    /* Definite assignment changes only after ALL right sides have been read.
     * This rejects a,b = 1,a when a was not initialized before the statement. */
    for (int index = 0; index < node->count; index++) {
        AtNode *target = node->args[index]->a;
        if (target->kind == AN_GLOBAL) {
            if (checker->f->module_initializer) {
                checker->p->globals[target->symbol].initialized = 1;
            }
        } else if (target->symbol >= 0) {
            checker->f->locals[target->symbol].initialized = AT_LOCAL_INITIALIZED;
        }
    }
}
