/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

Val at_ir_argument(Gen *g, AtNode *node, int expected)
{
    AtType *target = &g->p->types[expected], *source = &g->p->types[node->type];
    if (target->kind != AT_SLICE || source->kind != AT_ARRAY) {
        return at_ir_expression(g, node);
    }
    Val pointer = at_ir_address(g, node);
    Val data = at_ir_temp(g, expected), slice = at_ir_temp(g, expected);
    at_ir_emit(g, "  %s = insertvalue %s zeroinitializer, ptr %s, 0\n", data.text,
               at_ir_type(g, expected), pointer.text);
    at_ir_emit(g, "  %s = insertvalue %s %s, i64 %d, 1\n", slice.text, at_ir_type(g, expected),
               data.text, source->count);
    return slice;
}

Val at_ir_indirect_call(Gen *g, AtNode *node, Val result)
{
    Val callable = at_ir_expression(g, node->a);
    AtType *signature = &g->p->types[callable.type];
    Val code = at_ir_temp(g, 0), environment = at_ir_temp(g, 0);
    at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", code.text, at_ir_type(g, callable.type),
               callable.text);
    at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", environment.text, at_ir_type(g, callable.type),
               callable.text);
    Val arguments[AT_ARGS];
    for (int i = 0; i < node->count; i++) {
        arguments[i] = at_ir_argument(g, node->args[i], signature->fields[i]);
    }
    if (node->type == AT_VOID) {
        at_ir_emit(g, "  ");
    } else {
        at_ir_emit(g, "  %s = ", result.text);
    }
    at_ir_emit(g, "call %s %s(ptr %s", at_ir_type(g, node->type), code.text, environment.text);
    for (int i = 0; i < node->count; i++) {
        at_ir_emit(g, ", %s %s", at_ir_type(g, arguments[i].type), arguments[i].text);
    }
    at_ir_emit(g, ")\n");
    at_ir_propagate(g);
    return result;
}

void at_ir_callable_adapter(Gen *g, int id)
{
    AtFunction *function = &g->p->functions[id];
    /* A function value carries code and an environment pointer. Plain module
     * functions use null environments; this adapter preserves their direct
     * call ABI. No VM stack or universal argument box enters either path. */
    at_ir_emit(g, "define internal %s @callable%d(ptr %%environment",
               at_ir_type(g, function->result), id);
    for (int i = 0; i < function->nparams; i++) {
        at_ir_emit(g, ", %s %%arg%d", at_ir_type(g, function->locals[i].type), i);
    }
    at_ir_emit(g, ") {\nentry:\n  ");
    if (function->result != AT_VOID) {
        at_ir_emit(g, "%%result = ");
    }
    at_ir_emit(g, "call %s @fn%d(", at_ir_type(g, function->result), id);
    for (int i = 0; i < function->nparams; i++) {
        at_ir_emit(g, "%s%s %%arg%d", i ? ", " : "", at_ir_type(g, function->locals[i].type), i);
    }
    at_ir_emit(g, ")\n");
    if (function->result == AT_VOID) {
        at_ir_emit(g, "  ret void\n}\n");
    } else {
        at_ir_emit(g, "  ret %s %%result\n}\n", at_ir_type(g, function->result));
    }
}
