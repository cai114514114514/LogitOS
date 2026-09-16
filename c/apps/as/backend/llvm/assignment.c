/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include <stdio.h>

static void check_length(Gen *g, AtNode *node, Val source)
{
    AtType *type = &g->p->types[source.type];
    Val length = at_ir_temp(g, AT_I64);
    if (type->kind == AT_ARRAY) {
        snprintf(length.text, sizeof length.text, "%d", type->count);
    } else if (type->kind == AT_LIST || type->kind == AT_RANGE ||
               at_byte_storage_kind(type->kind)) {
        const char *read_length = "at_buffer_len";
        if (type->kind == AT_LIST) {
            read_length = "at_list_len";
        } else if (type->kind == AT_RANGE) {
            read_length = "at_range_len";
        }
        at_ir_emit(g, "  %s = call i64 @%s(ptr %s)\n", length.text, read_length, source.text);
    } else {
        at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", length.text, at_ir_type(g, source.type),
                   source.text);
    }
    Val mismatch = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp ne i64 %s, %d\n", mismatch.text, length.text, node->count);
    at_ir_guard(g, mismatch, node->a, AT_E_VALUE);
}

static Val sequence_item(Gen *g, AtNode *node, Val source, int index)
{
    AtType *type = &g->p->types[source.type];
    int element = type->kind == AT_RANGE ? AT_I64 : type->kind == AT_STR ? AT_STR : type->element;
    Val result = at_ir_temp(g, element), pointer = at_ir_temp(g, 0);
    if (at_byte_storage_kind(type->kind)) {
        Val position = at_ir_value(AT_I64, "");
        snprintf(position.text, sizeof position.text, "%d", index);
        return at_ir_buffer_read(g, at_ir_buffer_at(g, source, position, node->a));
    }
    if (type->kind == AT_RANGE) {
        Val status = at_ir_temp(g, AT_I32), invalid = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = call i32 @at_range_at(ptr %%unpackitem%d, ptr %s, i64 %d)\n",
                   status.text, node->id, source.text, index);
        at_ir_emit(g, "  %s = icmp eq i32 %s, 0\n", invalid.text, status.text);
        at_ir_guard(g, invalid, node->a, AT_E_INDEX);
        at_ir_emit(g, "  %s = load i64, ptr %%unpackitem%d\n", result.text, node->id);
        return result;
    }
    if (type->kind == AT_ARRAY) {
        at_ir_emit(g, "  %s = getelementptr %s, ptr %%storage%d, i32 0, i32 %d\n", pointer.text,
                   at_ir_type(g, source.type), node->a->id, index);
    } else if (type->kind == AT_LIST) {
        Val position = at_ir_value(AT_I64, "");
        snprintf(position.text, sizeof position.text, "%d", index);
        pointer = at_ir_list_at(g, source, position, node->a);
    } else {
        Val data = at_ir_temp(g, 0);
        at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", data.text, at_ir_type(g, source.type),
                   source.text);
        at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i64 %d\n", pointer.text,
                   type->kind == AT_STR ? "i8" : at_ir_type(g, element), data.text, index);
        if (type->kind == AT_STR) {
            Val partial = at_ir_temp(g, AT_STR);
            at_ir_emit(g, "  %s = insertvalue { ptr, i64 } undef, ptr %s, 0\n", partial.text,
                       pointer.text);
            at_ir_emit(g, "  %s = insertvalue { ptr, i64 } %s, i64 1, 1\n", result.text,
                       partial.text);
            return result;
        }
    }
    at_ir_emit(g, "  %s = load %s, ptr %s\n", result.text, at_ir_type(g, element), pointer.text);
    return result;
}

void at_ir_unpack(Gen *g, AtNode *node)
{
    Val values[AT_ASSIGN_TARGETS];
    if (node->a) {
        Val source = at_ir_expression(g, node->a);
        check_length(g, node, source);
        if (g->p->types[source.type].kind == AT_ARRAY) {
            at_ir_emit(g, "  store %s %s, ptr %%storage%d\n", at_ir_type(g, source.type),
                       source.text, node->a->id);
        }
        for (int index = 0; index < node->count; index++) {
            AtNode *assignment = node->args[index];
            values[index] = sequence_item(g, node, source, index);
            if (assignment->conversion == AT_OPTION_WRAP) {
                values[index] = at_ir_optional_wrap(g, values[index], assignment->type);
            }
        }
    } else {
        for (int index = 0; index < node->count; index++) {
            values[index] = at_ir_expression(g, node->args[index]->b);
        }
    }
    /* No store occurs until extraction/evaluation succeeds. Expression roots
     * retain earlier managed values if a later right side allocates or throws. */
    for (int index = 0; index < node->count; index++) {
        at_ir_store_binding(g, node->args[index]->a, values[index]);
    }
}
