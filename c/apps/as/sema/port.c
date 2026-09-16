/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include "runtime/port.h"

int at_check_port_builtin(Checker *checker, AtNode *node, Token name, int *result)
{
    if (at_scope_name_equal(name, "port_stats")) {
        node->symbol = AT_CALL_PORT_STATS;
        if (node->count) {
            at_check_error(checker, node, "AS3204", "port_stats expects no arguments");
            for (int i = 0; i < node->count; i++) {
                at_check_expression(checker, node->args[i], 0);
            }
        }
        *result = at_dict_type(checker->p, AT_STR, AT_I64, node->module, node->token);
        return 1;
    }
    if (at_scope_name_equal(name, "pipe")) {
        node->symbol = AT_CALL_PORT_PIPE;
        if (checker->resource_acquisition != node) {
            at_check_error(checker, node, "AS3401",
                           "Acquire both pipe owners with 'with reader, writer = pipe()'");
        }
        if (node->count) {
            at_check_error(checker, node, "AS3204", "pipe expects no arguments");
            for (int i = 0; i < node->count; i++) {
                at_check_expression(checker, node->args[i], 0);
            }
        }
        /* Acquisition writes two scoped owners. It is not a copyable tuple
         * or List[Port], which would lose the single-owner cleanup contract. */
        *result = AT_VOID;
        return 1;
    }
    int borrowed = at_scope_name_equal(name, "port");
    if (!borrowed && !at_scope_name_equal(name, "open")) {
        return 0;
    }
    node->symbol = borrowed ? AT_CALL_PORT_BORROW : AT_CALL_PORT_OPEN;
    if (checker->resource_acquisition != node) {
        at_check_error(checker, node, "AS3401",
                       "Acquire a Port with 'with owner = open(...)' or 'with view = port(fd)'");
    }
    if (node->count < 1 || node->count > (borrowed ? 1 : 2)) {
        at_check_error(checker, node, "AS3204",
                       borrowed ? "port expects one descriptor"
                                : "open expects a path and an optional mode string");
    }
    for (int i = 0; i < node->count; i++) {
        int expected = borrowed ? AT_I64 : AT_STR;
        at_check_mismatch(checker, node->args[i], expected,
                          at_check_expression(checker, node->args[i], expected));
    }
    *result = AT_PORT;
    return 1;
}

int at_check_port_call(Checker *checker, AtNode *node, int *result)
{
    AtNode *callee = node->a;
    if (callee->kind != AN_FIELD ||
        (callee->a->kind == AN_NAME && at_scope_resolve_local(checker, callee->a->token) < 0 &&
         at_global_lookup(checker->p, checker->f->module, callee->a->token, 1) < 0)) {
        return 0; /* Qualified module functions keep ordinary resolution. */
    }

    /* Avoid rechecking arbitrary expressions for every call. Only known port
     * method spellings enter this receiver probe; user-defined methods with
     * the same spelling still continue through ordinary class resolution. */
    static const struct {
        const char *name;
        int method, argument, result;
    } methods[] = {
        {"read", AT_PORT_READ, AT_I64, AT_BYTES},
        {"read_exact", AT_PORT_READ_EXACT, AT_I64, AT_BYTES},
        {"readall", AT_PORT_READ_ALL, 0, AT_BYTES},
        {"write", AT_PORT_WRITE, -1, AT_I64},
        {"line", AT_PORT_LINE, 0, AT_STR},
        {"close", AT_PORT_CLOSE, 0, AT_VOID},
        {"closed", AT_PORT_CLOSED, 0, AT_BOOL},
        {"fd", AT_PORT_FD, 0, AT_I64},
        {"kind", AT_PORT_KIND, 0, AT_STR},
        {"lines", AT_PORT_LINES, 0, AT_PORT},
    };

    for (unsigned i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        if (!at_scope_name_equal(callee->token, methods[i].name)) {
            continue;
        }
        AtNode *previous = checker->resource_use;
        checker->resource_use = callee->a;
        int receiver = at_check_expression(checker, callee->a, 0);
        checker->resource_use = previous;
        if (receiver != AT_PORT) {
            return 0;
        }
        node->symbol = AT_CALL_PORT_METHOD;
        node->op = methods[i].method;
        int count = methods[i].argument != 0;
        if (node->count != count) {
            at_check_error(checker, node, "AS3204", "Port method argument count is incorrect");
        }
        for (int j = 0; j < node->count; j++) {
            int expected = methods[i].argument > 0 ? methods[i].argument : 0;
            int actual = at_check_expression(checker, node->args[j], expected);
            if (expected) {
                at_check_mismatch(checker, node->args[j], expected, actual);
            } else if (actual && actual != AT_STR && actual != AT_BYTES) {
                at_check_error(checker, node->args[j], "AS3202",
                               "Port.write requires str or Bytes");
            }
        }
        *result = methods[i].result;
        if (node->op == AT_PORT_READ || node->op == AT_PORT_LINE) {
            *result =
                at_compound_type(checker->p, AT_OPTIONAL, *result, 0, node->module, node->token);
        }
        return 1;
    }
    return 0;
}
