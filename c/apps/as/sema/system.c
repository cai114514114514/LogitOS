/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

int at_check_system_builtin(Checker *checker, AtNode *node, Token name, int *result)
{
    if (!at_scope_name_equal(name, "syscall")) {
        return 0;
    }
    node->symbol = AT_CALL_SYSCALL;
    if (!checker->unsafe_depth) {
        at_check_error(checker, node, "AS3810", "syscall requires an unsafe block");
    }
    if (node->count < 1 || node->count > 4) {
        at_check_error(checker, node, "AS3204", "syscall expects a number and up to three words");
    }
    for (int i = 0; i < node->count; i++) {
        int actual = at_check_expression(checker, node->args[i], 0);
        /* Addresses are u64, while syscall numbers, flags and negative fd
         * sentinels are i64. Both occupy one ABI word without a conversion.
         * Smaller integers require an explicit extension by the caller. */
        if (actual && actual != AT_I64 && (i == 0 || actual != AT_U64)) {
            at_check_error(checker, node->args[i], "AS3202",
                           i == 0 ? "The syscall number must be i64"
                                  : "A syscall argument must be i64 or u64");
        }
    }
    *result = AT_I64;
    return 1;
}
