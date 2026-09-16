/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

static int memory_text(Checker *checker, AtNode *node, Token name, int *result)
{
    int terminated = at_scope_name_equal(name, "mem2cstr");
    if (!terminated && !at_scope_name_equal(name, "mem2str")) {
        return 0;
    }
    node->symbol = AT_CALL_MEMORY_TEXT;
    node->op = terminated;
    if (!checker->unsafe_depth) {
        at_check_error(checker, node, "AS3810", "Memory text conversion requires an unsafe block");
    }
    if (node->count < (terminated ? 1 : 2) || node->count > 2) {
        at_check_error(checker, node, "AS3204",
                       "mem2str expects memory and length; mem2cstr accepts an optional limit");
    }
    for (int i = 0; i < node->count; i++) {
        int actual = at_check_expression(checker, node->args[i], i ? AT_I64 : 0);
        if (i) {
            at_check_mismatch(checker, node->args[i], AT_I64, actual);
        } else if (actual && actual != AT_U64 && checker->p->types[actual].kind != AT_POINTER &&
                   !at_check_byte_storage(checker->p, actual, 0)) {
            at_check_error(checker, node->args[i], "AS3202",
                           "Memory text conversion requires byte storage, Ptr or a u64 address");
        }
    }
    *result = AT_STR;
    return 1;
}

int at_check_memory_builtin(Checker *checker, AtNode *node, Token name, int *result)
{
    if (memory_text(checker, node, name, result) ||
        at_check_pointer_builtin(checker, node, name, result)) {
        return 1;
    }

    static const struct {
        const char *name;
        int symbol;
        int bits;
        int count;
    } signatures[] = {
        {"addr", AT_CALL_ADDR, 0, 1},    {"peek8", AT_CALL_PEEK, 8, 1},
        {"peek16", AT_CALL_PEEK, 16, 1}, {"peek32", AT_CALL_PEEK, 32, 1},
        {"peek64", AT_CALL_PEEK, 64, 1}, {"poke8", AT_CALL_POKE, 8, 2},
        {"poke16", AT_CALL_POKE, 16, 2}, {"poke32", AT_CALL_POKE, 32, 2},
        {"poke64", AT_CALL_POKE, 64, 2},
    };

    for (unsigned index = 0; index < sizeof signatures / sizeof signatures[0]; index++) {
        if (!at_scope_name_equal(name, signatures[index].name)) {
            continue;
        }
        node->symbol = signatures[index].symbol;
        node->op = signatures[index].bits;
        if (!checker->unsafe_depth) {
            at_check_error(checker, node, "AS3810",
                           "Raw memory operations require an unsafe block");
        }
        if (node->count != signatures[index].count) {
            at_check_error(checker, node, "AS3204", "Raw memory builtin argument count differs");
        }
        for (int argument = 0; argument < node->count; argument++) {
            int expected = argument ? AT_I64 : AT_U64;
            if (node->symbol == AT_CALL_ADDR) {
                int actual = at_check_expression(checker, node->args[argument], 0);
                if (actual && actual != AT_STR && checker->p->types[actual].kind != AT_POINTER &&
                    !at_check_byte_storage(checker->p, actual, 0)) {
                    at_check_error(checker, node->args[argument], "AS3202",
                                   "addr requires byte/text storage or Ptr");
                }
            } else {
                int actual = at_check_expression(checker, node->args[argument], expected);
                at_check_mismatch(checker, node->args[argument], expected, actual);
            }
        }
        *result = node->symbol == AT_CALL_ADDR   ? AT_U64
                  : node->symbol == AT_CALL_PEEK ? AT_I64
                                                 : AT_VOID;
        return 1;
    }
    return 0;
}
