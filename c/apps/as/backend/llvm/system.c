/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

Val at_ir_system(Gen *g, AtNode *node, Val result)
{
    Val words[4];
    for (int i = 0; i < 4; i++) {
        words[i] = i < node->count ? at_ir_expression(g, node->args[i]) : at_ir_value(AT_I64, "0");
    }
    /* Runtime status reports authority/platform errors. The syscall's raw
     * signed result is separate: negative errno values belong to its ABI. */
    Val status = at_ir_temp(g, AT_I32);
    at_ir_emit(g,
               "  %s = call i32 @at_system_call(ptr %%nativeresult%d, i64 %s, i64 %s, "
               "i64 %s, i64 %s)\n",
               status.text, node->id, words[0].text, words[1].text, words[2].text, words[3].text);
    at_ir_runtime_status(g, status, node);
    at_ir_emit(g, "  %s = load i64, ptr %%nativeresult%d\n", result.text, node->id);
    return result;
}
