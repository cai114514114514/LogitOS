/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

Val at_ir_layout_address(Gen *g, AtNode *field, Val owner)
{
    AtType *layout = &g->p->types[field->a->type];
    Val data = at_ir_temp(g, 0);
    Val address = at_ir_temp(g, 0);
    at_ir_emit(g, "  %s = call ptr @at_buffer_data(ptr %s)\n", data.text, owner.text);
    at_ir_emit(g, "  %s = getelementptr i8, ptr %s, i64 %d\n", address.text, data.text,
               layout->offsets[field->field]);
    return address;
}

Val at_ir_layout_read(Gen *g, AtNode *field, Val result)
{
    Val owner = at_ir_expression(g, field->a);
    Val address = at_ir_layout_address(g, field, owner);
    AtType *layout = &g->p->types[owner.type];
    if (field->type == AT_BYTES) {
        /* A span read is an immutable snapshot, including trailing NUL bytes.
         * Returning the interior address as Bytes would interpret user bytes
         * as an object header and would also permit later writes to mutate it. */
        at_ir_emit(g, "  %s = call ptr @at_bytes_copy(ptr %s, i64 %d)\n", result.text, address.text,
                   layout->widths[field->field]);
        at_ir_allocation(g, result, field, 1);
    } else {
        /* ABI metadata may describe packed fields or overlapping union members.
         * Native integer alignment must never override the declared offset. */
        at_ir_emit(g, "  %s = load %s, ptr %s, align 1\n", result.text, at_ir_type(g, field->type),
                   address.text);
    }
    return result;
}

void at_ir_layout_store(Gen *g, AtNode *field, Val value)
{
    Val owner = at_ir_expression(g, field->a);
    AtType *layout = &g->p->types[owner.type];
    if (field->type == AT_BYTES) {
        Val status = at_ir_temp(g, AT_I32);
        at_ir_emit(g, "  %s = call i32 @at_layout_store_bytes(ptr %s, i64 %d, i64 %d, ptr %s)\n",
                   status.text, owner.text, layout->offsets[field->field],
                   layout->widths[field->field], value.text);
        at_ir_runtime_status(g, status, field);
    } else {
        Val address = at_ir_layout_address(g, field, owner);
        at_ir_emit(g, "  store %s %s, ptr %s, align 1\n", at_ir_type(g, field->type), value.text,
                   address.text);
    }
}
