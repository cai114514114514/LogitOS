/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

Val at_ir_bytes_decode(Gen *g, AtNode *node, Val result)
{
    Val bytes = at_ir_expression(g, node->a->a);
    Val status = at_ir_temp(g, AT_I32);
    at_ir_emit(g, "  %s = call i32 @at_bytes_decode(ptr %%decoded%d, ptr %s)\n", status.text,
               node->id, bytes.text);
    at_ir_runtime_status(g, status, node);
    /* A dedicated text slot also works when the outer expression converts
     * this result to Optional[str], whose storage has an additional tag. */
    at_ir_emit(g, "  %s = load { ptr, i64 }, ptr %%decoded%d\n", result.text, node->id);
    return result;
}
