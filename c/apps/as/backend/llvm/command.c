/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include "runtime/command.h"

Val at_ir_command_operator(Gen *generator, AtNode *node)
{
    Val left = at_ir_expression(generator, node->a);
    Val right = at_ir_expression(generator, node->b);
    Val status = at_ir_temp(generator, AT_I32);
    if (node->op == T_PIPEOP) {
        at_ir_emit(generator, "  %s = call i32 @at_command_pipe(ptr %s, ptr %s)\n", status.text,
                   left.text, right.text);
    } else {
        Val path = at_ir_temp(generator, 0), length = at_ir_temp(generator, AT_I64);
        at_ir_emit(generator, "  %s = extractvalue { ptr, i64 } %s, 0\n", path.text, right.text);
        at_ir_emit(generator, "  %s = extractvalue { ptr, i64 } %s, 1\n", length.text, right.text);
        at_ir_emit(generator,
                   "  %s = call i32 @at_command_redirect(ptr %s, ptr %s, i64 %s, i32 %d)\n",
                   status.text, left.text, path.text, length.text, node->op == T_ARROW);
    }
    at_ir_runtime_status(generator, status, node);
    return left;
}

Val at_ir_command(Gen *generator, AtNode *node, Val result)
{
    Val status = at_ir_temp(generator, AT_I32);
    if (node->symbol == AT_CALL_COMMAND_NEW) {
        if (node->op) {
            Val arguments = at_ir_expression(generator, node->args[0]);
            at_ir_emit(generator,
                       "  %s = call i32 @at_command_new_list(ptr %%commandresult%d, ptr %s)\n",
                       status.text, node->id, arguments.text);
        } else {
            for (int i = 0; i < node->count; i++) {
                Val argument = at_ir_expression(generator, node->args[i]);
                Val slot = at_ir_temp(generator, 0);
                at_ir_emit(generator,
                           "  %s = getelementptr [%d x { ptr, i64 }], ptr %%commandargs%d, i32 0, "
                           "i32 %d\n",
                           slot.text, node->count, node->id, i);
                at_ir_emit(generator, "  store { ptr, i64 } %s, ptr %s\n", argument.text,
                           slot.text);
            }
            at_ir_emit(generator,
                       "  %s = call i32 @at_command_new(ptr %%commandresult%d, ptr "
                       "%%commandargs%d, i64 %d)\n",
                       status.text, node->id, node->id, node->count);
        }
        at_ir_runtime_status(generator, status, node);
        at_ir_emit(generator, "  %s = load ptr, ptr %%commandresult%d\n", result.text, node->id);
        return result;
    }
    Val receiver = at_ir_expression(generator, node->a->a);
    if (node->op == AT_COMMAND_START) {
        at_ir_emit(generator, "  %s = call i32 @at_command_acquire(ptr %s)\n", status.text,
                   receiver.text);
        at_ir_runtime_status(generator, status, node);
        receiver.type = AT_PROCESS;
        return receiver;
    }
    if (node->op == AT_COMMAND_WAIT || node->op == AT_COMMAND_OUT) {
        const char *operation = node->op == AT_COMMAND_WAIT ? "wait" : "out";
        at_ir_emit(generator, "  %s = call i32 @at_command_%s(ptr %%commandresult%d, ptr %s)\n",
                   status.text, operation, node->id, receiver.text);
        at_ir_runtime_status(generator, status, node);
        at_ir_emit(generator, "  %s = load %s, ptr %%commandresult%d\n", result.text,
                   at_ir_type(generator, node->type), node->id);
    } else {
        const char *operation = node->op == AT_COMMAND_PID      ? "pid"
                                : node->op == AT_COMMAND_STATUS ? "status"
                                                                : "argv";
        at_ir_emit(generator, "  %s = call %s @at_command_%s(ptr %s)\n", result.text,
                   at_ir_type(generator, node->type), operation, receiver.text);
    }
    return result;
}
