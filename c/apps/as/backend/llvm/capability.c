/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

Val at_ir_capability(Gen *g, AtNode *node, Val result)
{
    if (node->symbol == AT_CALL_CAPS) {
        at_ir_emit(g, "  %s = call ptr @at_caps_value()\n", result.text);
        at_ir_allocation(g, result, node, 1);
        return result;
    }

    Val receiver = at_ir_expression(g, node->a->a);
    if (node->symbol == AT_CALL_CAP_BITS) {
        at_ir_emit(g, "  %s = call i64 @at_cap_bits(ptr %s)\n", result.text, receiver.text);
    } else if (node->symbol == AT_CALL_CAP_WITHOUT) {
        Val mask = at_ir_expression(g, node->args[0]);
        at_ir_emit(g, "  %s = call ptr @at_cap_without(ptr %s, i64 %s)\n", result.text,
                   receiver.text, mask.text);
        at_ir_allocation(g, result, node, 1);
    } else if (node->symbol == AT_CALL_CAP_SCOPE) {
        Val path = at_ir_expression(g, node->args[0]);
        Val data = at_ir_temp(g, 0), length = at_ir_temp(g, AT_I64), status = at_ir_temp(g, AT_I32);
        at_ir_emit(g, "  %s = extractvalue { ptr, i64 } %s, 0\n", data.text, path.text);
        at_ir_emit(g, "  %s = extractvalue { ptr, i64 } %s, 1\n", length.text, path.text);
        at_ir_emit(g, "  %s = call i32 @at_cap_scope(ptr %%storage%d, ptr %s, ptr %s, i64 %s)\n",
                   status.text, node->id, receiver.text, data.text, length.text);
        at_ir_runtime_status(g, status, node);
        at_ir_emit(g, "  %s = load ptr, ptr %%storage%d\n", result.text, node->id);
    } else {
        Val data = at_ir_temp(g, 0), length = at_ir_temp(g, AT_I64);
        Val present = at_ir_temp(g, AT_BOOL), text_head = at_ir_temp(g, AT_STR);
        Val text = at_ir_temp(g, AT_STR), payload = at_ir_temp(g, node->type);
        at_ir_emit(g, "  %s = call ptr @at_cap_path(ptr %s)\n", data.text, receiver.text);
        at_ir_emit(g, "  %s = call i64 @at_cap_path_length(ptr %s)\n", length.text, receiver.text);
        at_ir_emit(g, "  %s = icmp ne ptr %s, null\n", present.text, data.text);
        at_ir_emit(g, "  %s = insertvalue { ptr, i64 } zeroinitializer, ptr %s, 0\n",
                   text_head.text, data.text);
        at_ir_emit(g, "  %s = insertvalue { ptr, i64 } %s, i64 %s, 1\n", text.text, text_head.text,
                   length.text);
        at_ir_emit(g, "  %s = insertvalue %s zeroinitializer, { ptr, i64 } %s, 1\n", payload.text,
                   at_ir_type(g, node->type), text.text);
        at_ir_emit(g, "  %s = insertvalue %s %s, i1 %s, 0\n", result.text,
                   at_ir_type(g, node->type), payload.text, present.text);
    }
    return result;
}
