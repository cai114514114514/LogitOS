/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

Val at_ir_file(Gen *g, AtNode *node, Val result)
{
    Val path = at_ir_expression(g, node->args[0]);
    Val pointer = at_ir_temp(g, 0);
    Val length = at_ir_temp(g, AT_I64);
    Val status = at_ir_temp(g, AT_I32);
    at_ir_emit(g, "  %s = extractvalue { ptr, i64 } %s, 0\n", pointer.text, path.text);
    at_ir_emit(g, "  %s = extractvalue { ptr, i64 } %s, 1\n", length.text, path.text);
    if (node->symbol == AT_CALL_FILE_READ) {
        at_ir_emit(g, "  %s = call i32 @at_file_read(ptr %%file%d, ptr %s, i64 %s)\n", status.text,
                   node->id, pointer.text, length.text);
    } else {
        /* Evaluate data after path, keeping the generated path root live if
         * computing this later argument allocates or explicitly collects. */
        Val data = at_ir_expression(g, node->args[1]);
        at_ir_emit(g, "  %s = call i32 @at_file_write(ptr %%file%d, ptr %s, i64 %s, ptr %s)\n",
                   status.text, node->id, pointer.text, length.text, data.text);
    }
    at_ir_runtime_status(g, status, node);
    at_ir_emit(g, "  %s = load %s, ptr %%file%d\n", result.text, at_ir_type(g, result.type),
               node->id);
    return result;
}
