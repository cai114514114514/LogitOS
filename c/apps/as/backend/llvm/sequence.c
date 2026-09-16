/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include <stdio.h>

Val at_ir_equal(Gen *g, Val left, Val right, AtNode *site)
{
    if (left.type == AT_NONE) {
        if (right.type == AT_NONE) {
            return at_ir_value(AT_BOOL, "true");
        }
        return at_ir_equal(g, right, left, site);
    }
    if (g->p->types[left.type].kind == AT_OPTIONAL) {
        return at_ir_optional_equal(g, left, right, site);
    }
    if (left.type == AT_ANY && right.type == AT_NONE) {
        Val data = at_ir_temp(g, 0), equal = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = call ptr @at_any_data(ptr %s, i32 %d)\n", data.text, left.text,
                   AT_NONE);
        at_ir_emit(g, "  %s = icmp ne ptr %s, null\n", equal.text, data.text);
        return equal;
    }
    if (left.type == AT_ANY) {
        Val status = at_ir_temp(g, AT_I32), bad = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = call i32 @at_any_equal(ptr %s, ptr %s)\n", status.text, left.text,
                   right.text);
        at_ir_emit(g, "  %s = icmp slt i32 %s, 0\n", bad.text, status.text);
        at_ir_guard(g, bad, site, AT_E_MEMORY);
        Val equal = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = icmp ne i32 %s, 0\n", equal.text, status.text);
        return equal;
    }
    if (g->p->types[left.type].kind != AT_CALLABLE) {
        return at_ir_numeric(g, T_EQ, left, right, site, 0);
    }
    /* Function values compare both the entry point and captured environment.
     * Comparing code alone would merge closures with different state. */
    Val same[2];
    for (int i = 0; i < 2; i++) {
        Val a = at_ir_temp(g, 0), b = at_ir_temp(g, 0);
        same[i] = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = extractvalue %s %s, %d\n", a.text, at_ir_type(g, left.type),
                   left.text, i);
        at_ir_emit(g, "  %s = extractvalue %s %s, %d\n", b.text, at_ir_type(g, right.type),
                   right.text, i);
        at_ir_emit(g, "  %s = icmp eq ptr %s, %s\n", same[i].text, a.text, b.text);
    }
    Val result = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = and i1 %s, %s\n", result.text, same[0].text, same[1].text);
    return result;
}

Val at_ir_sequence_contains(Gen *g, AtNode *node, Val key, Val sequence)
{
    if (sequence.type == AT_RANGE || at_byte_storage_kind(g->p->types[sequence.type].kind)) {
        Val status = at_ir_temp(g, AT_I32), result = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = call i32 @%s(ptr %s, i64 %s)\n", status.text,
                   sequence.type == AT_RANGE ? "at_range_contains" : "at_buffer_contains",
                   sequence.text, key.text);
        at_ir_emit(g, "  %s = icmp ne i32 %s, 0\n", result.text, status.text);
        return result;
    }
    AtType *type = &g->p->types[sequence.type];
    Val pointer = at_ir_temp(g, 0), length = at_ir_temp(g, AT_I64);
    if (type->kind == AT_LIST) {
        pointer = sequence;
        at_ir_emit(g, "  %s = call i64 @at_list_len(ptr %s)\n", length.text, pointer.text);
    } else if (type->kind == AT_ARRAY) {
        /* The right expression has already executed. Store its value once;
         * taking its address by re-evaluating it would repeat side effects. */
        at_ir_emit(g, "  store %s %s, ptr %%storage%d\n", at_ir_type(g, sequence.type),
                   sequence.text, node->b->id);
        at_ir_emit(g, "  %s = getelementptr %s, ptr %%storage%d, i32 0, i32 0\n", pointer.text,
                   at_ir_type(g, sequence.type), node->b->id);
        snprintf(length.text, sizeof length.text, "%d", type->count);
    } else {
        at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", pointer.text, at_ir_type(g, sequence.type),
                   sequence.text);
        at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", length.text, at_ir_type(g, sequence.type),
                   sequence.text);
    }
    int entry = at_ir_label(g), loop = at_ir_label(g), body = at_ir_label(g);
    int step = at_ir_label(g), found = at_ir_label(g), absent = at_ir_label(g),
        done = at_ir_label(g);
    Val index = at_ir_temp(g, AT_I64), next = at_ir_temp(g, AT_I64);
    at_ir_jump(g, entry);
    at_ir_mark(g, entry);
    at_ir_jump(g, loop);
    at_ir_mark(g, loop);
    at_ir_emit(g, "  %s = phi i64 [ 0, %%b%d ], [ %s, %%b%d ]\n", index.text, entry, next.text,
               step);
    Val more = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp slt i64 %s, %s\n", more.text, index.text, length.text);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", more.text, body, absent);
    at_ir_mark(g, body);
    Val address = at_ir_temp(g, 0), value = at_ir_temp(g, type->element);
    if (type->kind == AT_LIST) {
        at_ir_emit(g, "  %s = call ptr @at_list_at(ptr %s, i64 %s)\n", address.text, pointer.text,
                   index.text);
    } else {
        at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i64 %s\n", address.text,
                   at_ir_type(g, type->element), pointer.text, index.text);
    }
    at_ir_emit(g, "  %s = load %s, ptr %s\n", value.text, at_ir_type(g, type->element),
               address.text);
    Val equal = at_ir_equal(g, key, value, node);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", equal.text, found, step);
    at_ir_mark(g, step);
    at_ir_emit(g, "  %s = add i64 %s, 1\n", next.text, index.text);
    at_ir_jump(g, loop);
    at_ir_mark(g, found);
    at_ir_jump(g, done);
    at_ir_mark(g, absent);
    at_ir_jump(g, done);
    at_ir_mark(g, done);
    Val result = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = phi i1 [ true, %%b%d ], [ false, %%b%d ]\n", result.text, found, absent);
    return result;
}
