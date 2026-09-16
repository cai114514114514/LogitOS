/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

int at_check_bytes_call(Checker *checker, AtNode *node, int *result)
{
    AtNode *callee = node->a;
    if (callee->kind != AN_FIELD || !at_scope_name_equal(callee->token, "decode")) {
        return 0;
    }
    AtNode *receiver = callee->a;
    /* An imported decoder module or a class's decode method keeps its own
     * function resolution. Only a Bytes receiver selects this primitive API. */
    if (receiver->kind == AN_NAME && at_scope_resolve_local(checker, receiver->token) < 0 &&
        at_global_lookup(checker->p, checker->f->module, receiver->token, 1) < 0) {
        return 0;
    }
    if (at_check_expression(checker, receiver, 0) != AT_BYTES) {
        return 0;
    }
    node->symbol = AT_CALL_BYTES_DECODE;
    if (node->count) {
        at_check_error(checker, node, "AS3204",
                       "Bytes.decode takes no arguments and requires UTF-8");
    }
    for (int index = 0; index < node->count; index++) {
        at_check_expression(checker, node->args[index], 0);
    }
    *result = AT_STR;
    return 1;
}
