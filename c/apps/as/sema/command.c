/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include "runtime/command.h"

int at_check_command_builtin(Checker *checker, AtNode *node, Token name, int *result)
{
    if (!at_scope_name_equal(name, "run")) {
        return 0;
    }
    node->symbol = AT_CALL_COMMAND_NEW;
    node->op = 0;
    if (!node->count) {
        at_check_error(checker, node, "AS3204", "run expects a command or List[str] arguments");
    }
    for (int i = 0; i < node->count; i++) {
        int expected = 0;
        if (node->count == 1 && node->args[i]->kind == AN_ARRAY) {
            expected = at_compound_type(checker->p, AT_LIST, AT_STR, 0, node->module, node->token);
        }
        int type = at_check_expression(checker, node->args[i], expected);
        AtType *argument = &checker->p->types[type];
        if (node->count == 1 && argument->kind == AT_LIST && argument->element == AT_STR) {
            node->op = 1;
        } else {
            at_check_mismatch(checker, node->args[i], AT_STR, type);
        }
    }
    *result = AT_COMMAND;
    return 1;
}

int at_check_command_call(Checker *checker, AtNode *node, int *result)
{
    AtNode *callee = node->a;
    if (callee->kind != AN_FIELD ||
        (callee->a->kind == AN_NAME && at_scope_resolve_local(checker, callee->a->token) < 0 &&
         at_global_lookup(checker->p, checker->f->module, callee->a->token, 1) < 0)) {
        return 0;
    }

    static const struct {
        const char *name;
        int method, result;
    } methods[] = {
        {"start", AT_COMMAND_START, AT_PROCESS}, {"wait", AT_COMMAND_WAIT, AT_I64},
        {"out", AT_COMMAND_OUT, AT_STR},         {"pid", AT_COMMAND_PID, AT_I64},
        {"status", AT_COMMAND_STATUS, AT_I64},   {"argv", AT_COMMAND_ARGV, AT_LIST},
    };

    for (unsigned i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        if (!at_scope_name_equal(callee->token, methods[i].name)) {
            continue;
        }
        AtNode *previous = checker->resource_use;
        checker->resource_use = callee->a;
        int type = at_check_expression(checker, callee->a, 0);
        checker->resource_use = previous;
        if (type != AT_COMMAND && type != AT_PROCESS) {
            return 0;
        }
        node->symbol = AT_CALL_COMMAND_METHOD;
        node->op = methods[i].method;
        if (node->count) {
            at_check_error(checker, node, "AS3204", "Command methods expect no arguments");
        }
        if (node->op == AT_COMMAND_START &&
            (type == AT_PROCESS || checker->resource_acquisition != node)) {
            at_check_error(checker, node, "AS3401",
                           "Start a command with 'with process = command.start()'");
        }
        *result = methods[i].result;
        if (*result == AT_LIST) {
            *result = at_compound_type(checker->p, AT_LIST, AT_STR, 0, node->module, node->token);
        }
        return 1;
    }
    return 0;
}
