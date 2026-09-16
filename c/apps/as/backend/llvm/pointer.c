/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

Val at_ir_pointer_at(Gen *g, Val pointer, Val index, AtNode *site)
{
    at_ir_require_raw(g, site);
    Val null = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp eq i64 %s, 0\n", null.text, pointer.text);
    at_ir_guard(g, null, site, AT_E_VALUE);

    int element = g->p->types[pointer.type].element;
    char width[16];
    snprintf(width, sizeof width, "%d", at_bits(g->p, element) / 8);
    Val offset = at_ir_numeric(g, T_STAR, index, at_ir_value(AT_I64, width), site, 0);
    Val negative = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp slt i64 %s, 0\n", negative.text, offset.text);
    int subtract = at_ir_label(g), add = at_ir_label(g), done = at_ir_label(g);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", negative.text, subtract, add);

    /* Negative indices are useful for a pointer into the middle of an owned
     * allocation. Check scaling and address under/overflow separately; LLVM
     * inbounds GEP would promise an allocation boundary we cannot establish. */
    at_ir_mark(g, subtract);
    Val magnitude = at_ir_temp(g, AT_U64);
    at_ir_emit(g, "  %s = sub i64 0, %s\n", magnitude.text, offset.text);
    Val lower = at_ir_numeric(g, T_MINUS, at_ir_value(AT_U64, pointer.text), magnitude, site, 0);
    /* Checked arithmetic can introduce its own success block. Give the phi
     * explicit predecessors instead of guessing the last generated label. */
    int lower_block = at_ir_label(g);
    at_ir_jump(g, lower_block);
    at_ir_mark(g, lower_block);
    at_ir_emit(g, "  br label %%b%d\n", done);
    at_ir_mark(g, add);
    Val upper = at_ir_numeric(g, T_PLUS, at_ir_value(AT_U64, pointer.text),
                              at_ir_value(AT_U64, offset.text), site, 0);
    int upper_block = at_ir_label(g);
    at_ir_jump(g, upper_block);
    at_ir_mark(g, upper_block);
    at_ir_emit(g, "  br label %%b%d\n", done);
    at_ir_mark(g, done);
    Val address = at_ir_temp(g, AT_U64), result = at_ir_temp(g, 0);
    at_ir_emit(g, "  %s = phi i64 [ %s, %%b%d ], [ %s, %%b%d ]\n", address.text, lower.text,
               lower_block, upper.text, upper_block);
    null = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp eq i64 %s, 0\n", null.text, address.text);
    at_ir_guard(g, null, site, AT_E_VALUE);
    at_ir_emit(g, "  %s = inttoptr i64 %s to ptr\n", result.text, address.text);
    return result;
}
