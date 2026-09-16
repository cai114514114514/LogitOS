/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

static void append_element(Gen *g, AtNode *node)
{
    int skip = 0;
    if (node->c) {
        Val condition = at_ir_expression(g, node->c);
        int include = at_ir_label(g);
        skip = at_ir_label(g);
        at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", condition.text, include, skip);
        at_ir_mark(g, include);
    }
    Val item = at_ir_expression(g, node->a), list = at_ir_temp(g, 0);
    Val status = at_ir_temp(g, AT_I32);
    at_ir_emit(g, "  %s = load ptr, ptr %%construction%d\n", list.text, node->id);
    at_ir_emit(g, "  store %s %s, ptr %%argument%d\n", at_ir_type(g, item.type), item.text,
               node->id);
    at_ir_emit(g, "  %s = call i32 @at_list_append(ptr %s, ptr %%argument%d)\n", status.text,
               list.text, node->id);
    at_ir_allocation(g, status, node, 0);
    if (skip) {
        at_ir_jump(g, skip);
        at_ir_mark(g, skip);
    }
}

Val at_ir_comprehension(Gen *g, AtNode *node, Val result)
{
    int element = g->p->types[node->type].element;
    at_ir_emit(g,
               "  %s = call ptr @at_list_new(i64 ptrtoint (ptr getelementptr (%s, ptr null, "
               "i32 1) to i64), ptr @scan%d)\n",
               result.text, at_ir_type(g, element), element);
    at_ir_allocation(g, result, node, 1);
    at_ir_emit(g, "  store ptr %s, ptr %%construction%d\n", result.text, node->id);
    /* A loop view borrows the same node identity and entry-block slots. It
     * changes only the child roles; no post-check syntax allocation occurs. */
    AtNode loop = *node;
    loop.a = node->args[0];
    at_ir_for_loop(g, &loop, append_element, node);
    return result;
}
