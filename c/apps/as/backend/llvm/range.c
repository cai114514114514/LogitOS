/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

void at_ir_range_bounds(Gen *g, AtNode *node, Val *start, Val *stop, Val *step)
{
    *start = at_ir_value(AT_I64, "0");
    *step = at_ir_value(AT_I64, "1");
    if (node->kind == AN_CALL && node->symbol == AT_CALL_RANGE) {
        /* Direct for/range loops consume evaluated scalars and allocate no
         * range object. A stored/passed range keeps its original identity. */
        if (node->count >= 2) {
            *start = at_ir_expression(g, node->args[0]);
        }
        *stop = at_ir_expression(g, node->args[node->count == 1 ? 0 : 1]);
        if (node->count == 3) {
            *step = at_ir_expression(g, node->args[2]);
        }
        return;
    }
    Val range = at_ir_expression(g, node);
    Val *bounds[] = {start, stop, step};
    for (int index = 0; index < 3; index++) {
        *bounds[index] = at_ir_temp(g, AT_I64);
        at_ir_emit(g, "  %s = call i64 @at_range_bound(ptr %s, i32 %d)\n", bounds[index]->text,
                   range.text, index);
    }
}

Val at_ir_range_new(Gen *g, AtNode *node, Val result)
{
    Val start, stop, step;
    at_ir_range_bounds(g, node, &start, &stop, &step);
    Val zero = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp eq i64 %s, 0\n", zero.text, step.text);
    at_ir_guard(g, zero, node, AT_E_VALUE);
    at_ir_emit(g, "  %s = call ptr @at_range_new(i64 %s, i64 %s, i64 %s)\n", result.text,
               start.text, stop.text, step.text);
    at_ir_allocation(g, result, node, 1);
    return result;
}

Val at_ir_range_length(Gen *g, AtNode *node, Val range, Val result)
{
    at_ir_emit(g, "  %s = call i64 @at_range_len(ptr %s)\n", result.text, range.text);
    Val overflow = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp slt i64 %s, 0\n", overflow.text, result.text);
    at_ir_guard(g, overflow, node, AT_E_OVERFLOW);
    return result;
}

Val at_ir_range_index(Gen *g, AtNode *node, Val result)
{
    Val range = at_ir_expression(g, node->a), index = at_ir_expression(g, node->b);
    Val status = at_ir_temp(g, AT_I32), invalid = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = call i32 @at_range_at(ptr %%rangeitem%d, ptr %s, i64 %s)\n", status.text,
               node->id, range.text, index.text);
    at_ir_emit(g, "  %s = icmp eq i32 %s, 0\n", invalid.text, status.text);
    at_ir_guard(g, invalid, node, AT_E_INDEX);
    at_ir_emit(g, "  %s = load i64, ptr %%rangeitem%d\n", result.text, node->id);
    return result;
}
