/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

int at_check_capability_call(Checker *checker, AtNode *node, int *result)
{
    AtNode *callee = node->a;
    if (callee->kind != AN_FIELD) {
        return 0;
    }
    /* An imported module may export an ordinary function with these names.
     * Only receiver expressions are candidates for primitive Cap methods. */
    AtNode *receiver = callee->a;
    if (receiver->kind == AN_NAME && at_scope_resolve_local(checker, receiver->token) < 0 &&
        at_global_lookup(checker->p, checker->f->module, receiver->token, 1) < 0) {
        return 0;
    }

    static const struct {
        const char *name;
        int symbol;
        int argument;
        int result;
    } methods[] = {
        {"bits", AT_CALL_CAP_BITS, AT_VOID, AT_I64},
        {"path", AT_CALL_CAP_PATH, AT_VOID, AT_OPTIONAL},
        {"without", AT_CALL_CAP_WITHOUT, AT_I64, AT_CAP},
        {"scope", AT_CALL_CAP_SCOPE, AT_STR, AT_CAP},
    };

    for (unsigned index = 0; index < sizeof methods / sizeof methods[0]; index++) {
        if (!at_scope_name_equal(callee->token, methods[index].name)) {
            continue;
        }
        if (at_check_expression(checker, receiver, 0) != AT_CAP) {
            return 0;
        }
        node->symbol = methods[index].symbol;
        int argument = methods[index].argument;
        if (node->count != (argument == AT_VOID ? 0 : 1)) {
            at_check_error(checker, node, "AS3204", "Capability method argument count differs");
        }
        for (int item = 0; item < node->count; item++) {
            int actual = at_check_expression(checker, node->args[item], argument);
            at_check_mismatch(checker, node->args[item], argument, actual);
        }
        *result = methods[index].result;
        if (*result == AT_OPTIONAL) {
            *result =
                at_compound_type(checker->p, AT_OPTIONAL, AT_STR, 0, node->module, node->token);
        }
        return 1;
    }
    return 0;
}
