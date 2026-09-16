/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

int at_check_region_builtin(Checker *checker, AtNode *node, Token name, int *result)
{
    if (!at_scope_name_equal(name, "region")) {
        return 0;
    }
    node->symbol = AT_CALL_REGION;
    if (checker->resource_acquisition != node) {
        at_check_error(checker, node, "AS3401",
                       "Acquire a Region with 'with owner = region(size)'");
    }
    if (node->count != 1) {
        at_check_error(checker, node, "AS3204", "region expects one i64 byte count");
    }
    for (int i = 0; i < node->count; i++) {
        at_check_mismatch(checker, node->args[i], AT_I64,
                          at_check_expression(checker, node->args[i], AT_I64));
    }
    *result = AT_REGION;
    return 1;
}

int at_check_region_call(Checker *checker, AtNode *node, int *result)
{
    AtNode *callee = node->a;
    if (callee->kind != AN_FIELD) {
        return 0;
    }
    int move = at_scope_name_equal(callee->token, "move");
    int shared = at_scope_name_equal(callee->token, "borrow");
    int mutable = at_scope_name_equal(callee->token, "borrow_mut");
    if (!move && !shared && !mutable) {
        return 0;
    }
    if (callee->a->kind == AN_NAME && at_scope_resolve_local(checker, callee->a->token) < 0 &&
        at_global_lookup(checker->p, checker->f->module, callee->a->token, 1) < 0) {
        return 0; /* A module function named move retains ordinary resolution. */
    }
    int type = at_check_resource_receiver(checker, callee->a);
    int kind = checker->p->types[type].kind;
    if (type != AT_REGION && !at_slice_kind(kind)) {
        return 0;
    }
    if (!move) {
        if (checker->resource_acquisition != node || callee->a->kind != AN_NAME) {
            at_check_error(checker, node, "AS3401",
                           "Acquire a view with 'with view = owner.borrow(start, stop)'");
        }
        if (at_slice_kind(kind) && (callee->a->kind != AN_NAME || callee->a->symbol < 0 ||
                                    !checker->f->locals[callee->a->symbol].scoped_borrow)) {
            at_check_error(checker, node, "AS3403",
                           "Reborrowing requires a scoped view with a native loan record");
        }
        if (mutable && kind == AT_SLICE) {
            at_check_error(checker, node, "AS3403",
                           "A read-only Slice cannot create a mutable borrow");
        }
        if (node->count != 2) {
            at_check_error(checker, node, "AS3204", "A borrow expects start and stop byte offsets");
        }
        for (int i = 0; i < node->count; i++) {
            at_check_mismatch(checker, node->args[i], AT_I64,
                              at_check_expression(checker, node->args[i], AT_I64));
        }
        node->symbol = AT_CALL_REGION_BORROW;
        node->op = mutable;
        *result = at_compound_type(checker->p, mutable ? AT_MUT_SLICE : AT_SLICE, AT_U8, 0,
                                   node->module, node->token);
        return 1;
    }
    if (type != AT_REGION) {
        at_check_error(checker, node, "AS3401",
                       "A borrowed view cannot transfer its owner's lifetime");
        *result = AT_ERROR;
        return 1;
    }
    node->symbol = AT_CALL_REGION_MOVE;
    if (checker->resource_acquisition != node || callee->a->kind != AN_NAME) {
        at_check_error(checker, node, "AS3401",
                       "Move a named Region with 'with destination = owner.move()'");
    }
    if (node->count) {
        at_check_error(checker, node, "AS3204", "Region.move expects no arguments");
        for (int i = 0; i < node->count; i++) {
            at_check_expression(checker, node->args[i], 0);
        }
    }
    /* The separate flow pass consumes the source along each control-flow edge.
     * Mutating definite-assignment here would lose moves at loop/except joins. */
    *result = AT_REGION;
    return 1;
}

int at_check_borrow_argument(Checker *checker, AtNode *node, int expected)
{
    AtNode *previous = checker->resource_use;
    if (at_slice_kind(checker->p->types[expected].kind)) {
        /* Callees cannot retain views. Mutable calls also receive temporary
         * exclusive loans checked across all arguments by region_borrow.c. */
        checker->resource_use = node;
    }
    int type = at_check_expression(checker, node, expected);
    checker->resource_use = previous;
    return type;
}

int at_check_view_read(Checker *checker, AtNode *node)
{
    AtNode *previous = checker->resource_use;
    int slot = node->kind == AN_NAME ? at_scope_resolve_local(checker, node->token) : -1;
    if (slot >= 0 && (checker->f->locals[slot].scoped_borrow ||
                      checker->p->types[checker->f->locals[slot].type].kind == AT_MUT_SLICE)) {
        /* Formatting, membership and snapshots consume bytes immediately.
         * They produce no view alias and therefore do not extend its lifetime. */
        checker->resource_use = node;
    }
    int type = at_check_expression(checker, node, 0);
    checker->resource_use = previous;
    return type;
}
