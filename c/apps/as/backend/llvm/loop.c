/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include <stdio.h>

/* A shared native iteration path serves statements and list comprehensions.
 * Source evaluation, snapshots, widened range cursors and break/continue labels
 * remain identical; callers supply only the code to run for each element. */
void at_ir_for_loop(Gen *g, AtNode *n, AtLoopBody emit_body, AtNode *context)
{
    AtCleanup *old_cleanup = g->loop_cleanup;
    g->loop_cleanup = g->cleanup;
    int range = n->b->type == AT_RANGE;
    int dictionary = g->p->types[n->b->type].kind == AT_DICT;
    Val start = at_ir_value(AT_I64, "0"), end, ptr = at_ir_value(0, "null");
    Val increment = at_ir_value(AT_I64, "1");
    Val forward = at_ir_value(AT_BOOL, "true");
    const char *counter_type = range ? "i128" : "i64";
    if (range) {
        at_ir_range_bounds(g, n->b, &start, &end, &increment);
        Val zero_step = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = icmp eq i64 %s, 0\n", zero_step.text, increment.text);
        at_ir_guard(g, zero_step, n->b, 5);
        forward = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = icmp sgt i64 %s, 0\n", forward.text, increment.text);

        /* The user-visible range elements are i64. Widen only the
         * hidden cursor: an overshooting final increment must end a
         * range, not wrap into another iteration or raise overflow. */
        Val wide_start = at_ir_temp(g, 0), wide_end = at_ir_temp(g, 0),
            wide_step = at_ir_temp(g, 0);
        at_ir_emit(g, "  %s = sext i64 %s to i128\n", wide_start.text, start.text);
        at_ir_emit(g, "  %s = sext i64 %s to i128\n", wide_end.text, end.text);
        at_ir_emit(g, "  %s = sext i64 %s to i128\n", wide_step.text, increment.text);
        start = wide_start;
        end = wide_end;
        increment = wide_step;
    } else {
        AtType *array = &g->p->types[n->b->type];
        if (array->kind == AT_ARRAY) {
            ptr = at_ir_address(g, n->b);
            char s[32];
            snprintf(s, sizeof s, "%d", array->count);
            end = at_ir_value(AT_I64, s);
        } else if (array->kind == AT_DICT) {
            Val source = at_ir_expression(g, n->b);
            ptr = at_ir_temp(g, 0);
            at_ir_emit(g, "  %s = call ptr @at_dict_keys(ptr %s)\n", ptr.text, source.text);
            at_ir_allocation(g, ptr, n, 1);
            at_ir_emit(g, "  store ptr %s, ptr %%snapshot%d\n", ptr.text, n->id);
            end = at_ir_value(AT_I64, "0");
        } else if (at_byte_storage_kind(array->kind)) {
            ptr = at_ir_expression(g, n->b);
            end = at_ir_temp(g, AT_I64);
            at_ir_emit(g, "  %s = call i64 @at_buffer_len(ptr %s)\n", end.text, ptr.text);
        } else if (array->kind == AT_LIST) {
            ptr = at_ir_expression(g, n->b);
            end = at_ir_value(AT_I64, "0"); /* Reload length at the loop header. */
        } else {
            Val slice = at_ir_expression(g, n->b);
            ptr = at_ir_temp(g, 0);
            end = at_ir_temp(g, AT_I64);
            at_ir_emit(g, "  %s = extractvalue %s %s, 0\n  %s = extractvalue %s %s, 1\n", ptr.text,
                       at_ir_type(g, slice.type), slice.text, end.text, at_ir_type(g, slice.type),
                       slice.text);
        }
    }
    Val index = {.type = AT_I64};
    snprintf(index.text, sizeof index.text, "%%index%d", n->id);
    at_ir_emit(g, "  store %s %s, ptr %s\n", counter_type, start.text, index.text);
    int cond = at_ir_label(g), body_label = at_ir_label(g), step = at_ir_label(g),
        done = at_ir_label(g), ob = g->break_label, oc = g->continue_label;
    g->break_label = done;
    g->continue_label = step;
    at_ir_jump(g, cond);
    at_ir_mark(g, cond);
    Val i = at_ir_temp(g, AT_I64), test = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = load %s, ptr %s\n", i.text, counter_type, index.text);
    if (range) {
        Val below = at_ir_temp(g, AT_BOOL), above = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = icmp slt i128 %s, %s\n", below.text, i.text, end.text);
        at_ir_emit(g, "  %s = icmp sgt i128 %s, %s\n", above.text, i.text, end.text);
        at_ir_emit(g, "  %s = select i1 %s, i1 %s, i1 %s\n", test.text, forward.text, below.text,
                   above.text);
    } else {
        if (g->p->types[n->b->type].kind == AT_LIST || dictionary) {
            end = at_ir_temp(g, AT_I64);
            at_ir_emit(g, "  %s = call i64 @at_list_len(ptr %s)\n", end.text, ptr.text);
        }
        at_ir_emit(g, "  %s = icmp slt i64 %s, %s\n", test.text, i.text, end.text);
    }
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", test.text, body_label, done);
    at_ir_mark(g, body_label);
    Val item = i;
    if (range) {
        item = at_ir_temp(g, AT_I64);
        at_ir_emit(g, "  %s = trunc i128 %s to i64\n", item.text, i.text);
    } else if (at_byte_storage_kind(g->p->types[n->b->type].kind)) {
        item = at_ir_buffer_read(g, at_ir_buffer_at(g, ptr, i, n));
    } else if (n->b->type == AT_STR) {
        /* Text iteration follows the existing byte-index semantics.
         * A one-byte view retains the original immutable text owner. */
        Val byte = at_ir_temp(g, 0), slice = at_ir_temp(g, AT_STR);
        item = at_ir_temp(g, AT_STR);
        at_ir_emit(g, "  %s = getelementptr i8, ptr %s, i64 %s\n", byte.text, ptr.text, i.text);
        at_ir_emit(g, "  %s = insertvalue { ptr, i64 } undef, ptr %s, 0\n", slice.text, byte.text);
        at_ir_emit(g, "  %s = insertvalue { ptr, i64 } %s, i64 1, 1\n", item.text, slice.text);
    } else {
        Val at = at_ir_temp(g, n->a->type);
        item = at_ir_temp(g, n->a->type);
        if (g->p->types[n->b->type].kind == AT_LIST || dictionary) {
            at = at_ir_list_at(g, ptr, i, n);
        } else {
            at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i64 %s\n", at.text,
                       at_ir_type(g, n->a->type), ptr.text, i.text);
        }
        at_ir_emit(g, "  %s = load %s, ptr %s\n", item.text, at_ir_type(g, n->a->type), at.text);
    }
    at_ir_store_binding(g, n->a, item);
    if (emit_body) {
        emit_body(g, context);
    } else {
        at_ir_statements(g, n->c);
    }
    at_ir_jump(g, step);
    at_ir_mark(g, step);
    Val next = at_ir_temp(g, AT_I64);
    at_ir_emit(g, "  %s = add %s %s, %s\n  store %s %s, ptr %s\n", next.text, counter_type, i.text,
               increment.text, counter_type, next.text, index.text);
    at_ir_jump(g, cond);
    at_ir_mark(g, done);
    if (dictionary) {
        at_ir_emit(g, "  store ptr null, ptr %%snapshot%d\n", n->id);
    }
    g->break_label = ob;
    g->continue_label = oc;
    g->loop_cleanup = old_cleanup;
}
