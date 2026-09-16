/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

Val at_ir_optional_wrap(Gen *g, Val value, int type)
{
    if (value.type == AT_NONE) {
        return at_ir_value(type, "zeroinitializer");
    }
    Val payload = at_ir_temp(g, type), result = at_ir_temp(g, type);
    at_ir_emit(g, "  %s = insertvalue %s zeroinitializer, %s %s, 1\n", payload.text,
               at_ir_type(g, type), at_ir_type(g, value.type), value.text);
    at_ir_emit(g, "  %s = insertvalue %s %s, i1 true, 0\n", result.text, at_ir_type(g, type),
               payload.text);
    return result;
}

Val at_ir_optional_equal(Gen *g, Val left, Val right, AtNode *site)
{
    Val a = at_ir_temp(g, AT_BOOL), b = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", a.text, at_ir_type(g, left.type), left.text);
    if (right.type == AT_NONE) {
        at_ir_emit(g, "  %s = xor i1 %s, true\n", b.text, a.text);
        return b;
    }
    at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", b.text, at_ir_type(g, right.type), right.text);
    Val tags = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp eq i1 %s, %s\n", tags.text, a.text, b.text);
    int same = at_ir_label(g), different = at_ir_label(g), payload = at_ir_label(g);
    int empty = at_ir_label(g), compared = at_ir_label(g), done = at_ir_label(g);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", tags.text, same, different);
    at_ir_mark(g, same);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", a.text, payload, empty);
    at_ir_mark(g, payload);
    int element = g->p->types[left.type].element;
    Val x = at_ir_temp(g, element), y = at_ir_temp(g, element);
    at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", x.text, at_ir_type(g, left.type), left.text);
    at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", y.text, at_ir_type(g, right.type), right.text);
    Val equal = at_ir_equal(g, x, y, site);
    /* Nested Optional or Any equality may create blocks of its own. Give the
     * phi a dedicated predecessor instead of assuming payload is still current. */
    at_ir_jump(g, compared);
    at_ir_mark(g, compared);
    at_ir_jump(g, done);
    at_ir_mark(g, empty);
    at_ir_jump(g, done);
    at_ir_mark(g, different);
    at_ir_jump(g, done);
    at_ir_mark(g, done);
    Val result = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = phi i1 [ %s, %%b%d ], [ true, %%b%d ], [ false, %%b%d ]\n", result.text,
               equal.text, compared, empty, different);
    return result;
}
