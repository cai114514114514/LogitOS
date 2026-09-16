/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

static int manual_allocation(Checker *checker, AtNode *node, Token name, int *result)
{
    int allocate = at_scope_name_equal(name, "alloc");
    if (!allocate && !at_scope_name_equal(name, "dealloc")) {
        return 0;
    }

    node->symbol = allocate ? AT_CALL_ALLOC : AT_CALL_DEALLOC;
    if (!checker->unsafe_depth) {
        at_check_error(checker, node, "AS3810",
                       "Manual allocation and release require an unsafe block");
    }
    if (node->count != 1) {
        at_check_error(checker, node, "AS3204",
                       allocate ? "alloc expects one i64 byte count"
                                : "dealloc expects one pointer from alloc");
    }
    for (int index = 0; index < node->count; index++) {
        int actual = at_check_expression(checker, node->args[index], allocate ? AT_I64 : 0);
        if (allocate) {
            at_check_mismatch(checker, node->args[index], AT_I64, actual);
        } else if (actual && checker->p->types[actual].kind != AT_POINTER) {
            at_check_error(checker, node->args[index], "AS3202",
                           "dealloc requires a Ptr to the base of a live manual allocation");
        }
    }

    /* The old alloc supplied unsigned bytes. Preserve that layout without
     * boxing ownership into a VM object: the caller must release this address. */
    *result = allocate
                  ? at_compound_type(checker->p, AT_POINTER, AT_U8, 0, node->module, node->token)
                  : AT_VOID;
    return 1;
}

/* A pointer constructor changes the static pointee width, not ownership.
 * Raw pointers can be copied; dereferencing still requires lexical unsafe. */
int at_check_pointer_builtin(Checker *checker, AtNode *node, Token name, int *result)
{
    if (manual_allocation(checker, node, name, result)) {
        return 1;
    }

    static const char *pointer_names[] = {"i8ptr", "i16ptr", "i32ptr", "i64ptr"};
    for (int index = 0; index < 4; index++) {
        if (!at_scope_name_equal(name, pointer_names[index])) {
            continue;
        }
        node->symbol = AT_CALL_POINTER;
        if (!checker->unsafe_depth) {
            at_check_error(checker, node, "AS3810",
                           "Pointer construction requires an unsafe block");
        }
        if (node->count != 1) {
            at_check_error(checker, node, "AS3204", "Pointer constructor expects one u64 address");
        }
        for (int i = 0; i < node->count; i++) {
            at_check_mismatch(checker, node->args[i], AT_U64,
                              at_check_expression(checker, node->args[i], AT_U64));
        }
        *result =
            at_compound_type(checker->p, AT_POINTER, AT_I8 + index, 0, node->module, node->token);
        return 1;
    }

    return 0;
}
