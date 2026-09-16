/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include "runtime/port.h"

static Val text_part(Gen *g, Val text, int field)
{
    Val part = at_ir_temp(g, field ? AT_I64 : 0);
    at_ir_emit(g, "  %s = extractvalue { ptr, i64 } %s, %d\n", part.text, text.text, field);
    return part;
}

static Val optional_result(Gen *g, AtNode *node, Val value, Val present)
{
    Val payload = at_ir_temp(g, node->type), result = at_ir_temp(g, node->type);
    at_ir_emit(g, "  %s = insertvalue %s zeroinitializer, %s %s, 1\n", payload.text,
               at_ir_type(g, node->type), at_ir_type(g, value.type), value.text);
    at_ir_emit(g, "  %s = insertvalue %s %s, i1 %s, 0\n", result.text, at_ir_type(g, node->type),
               payload.text, present.text);
    return result;
}

Val at_ir_port(Gen *g, AtNode *node, Val result)
{
    if (node->symbol == AT_CALL_PORT_STATS) {
        /* Use the same key callbacks as an ordinary Dict[str, i64]. A private
         * runtime hash convention would break lookup after the caller inserts
         * additional keys or triggers a resize. */
        at_ir_emit(g, "  %s = call ptr @at_port_stats(ptr @hash%d, ptr @equal%d)\n", result.text,
                   AT_STR, AT_STR);
        at_ir_allocation(g, result, node, 1);
        return result;
    }
    Val status = at_ir_temp(g, AT_I32);
    if (node->symbol == AT_CALL_PORT_BORROW) {
        Val descriptor = at_ir_expression(g, node->args[0]);
        at_ir_emit(g, "  %s = call i32 @at_port_borrow(ptr %%portresult%d, i64 %s)\n", status.text,
                   node->id, descriptor.text);
        at_ir_runtime_status(g, status, node);
        at_ir_emit(g, "  %s = load ptr, ptr %%portresult%d\n", result.text, node->id);
        return result;
    }
    if (node->symbol == AT_CALL_PORT_OPEN) {
        Val path = at_ir_expression(g, node->args[0]);
        Val path_data = text_part(g, path, 0), path_length = text_part(g, path, 1);
        Val mode_data = at_ir_value(0, "@port_mode_r"), mode_length = at_ir_value(AT_I64, "1");
        if (node->count == 2) {
            Val mode = at_ir_expression(g, node->args[1]);
            mode_data = text_part(g, mode, 0);
            mode_length = text_part(g, mode, 1);
        }
        at_ir_emit(
            g,
            "  %s = call i32 @at_port_open(ptr %%portresult%d, ptr %s, i64 %s, ptr %s, i64 %s)\n",
            status.text, node->id, path_data.text, path_length.text, mode_data.text,
            mode_length.text);
        at_ir_runtime_status(g, status, node);
        at_ir_emit(g, "  %s = load ptr, ptr %%portresult%d\n", result.text, node->id);
        return result;
    }
    Val receiver = at_ir_expression(g, node->a->a);
    if (node->op == AT_PORT_LINES) {
        return receiver;
    }
    if (node->op == AT_PORT_KIND) {
        at_ir_emit(g, "  call void @at_port_kind(ptr %%portresult%d, ptr %s)\n", node->id,
                   receiver.text);
        at_ir_emit(g, "  %s = load { ptr, i64 }, ptr %%portresult%d\n", result.text, node->id);
        return result;
    }
    if (node->op == AT_PORT_FD) {
        at_ir_emit(g, "  %s = call i64 @at_port_fd(ptr %s)\n", result.text, receiver.text);
        return result;
    }
    if (node->op == AT_PORT_CLOSED) {
        at_ir_emit(g, "  %s = call i32 @at_port_closed(ptr %s)\n", status.text, receiver.text);
        at_ir_emit(g, "  %s = icmp ne i32 %s, 0\n", result.text, status.text);
        return result;
    }
    if (node->op == AT_PORT_CLOSE) {
        at_ir_emit(g, "  %s = call i32 @at_port_close(ptr %s)\n", status.text, receiver.text);
        at_ir_runtime_status(g, status, node);
        return result;
    }
    if (node->op == AT_PORT_WRITE) {
        Val data = at_ir_expression(g, node->args[0]);
        Val pointer, length;
        if (data.type == AT_STR) {
            pointer = text_part(g, data, 0);
            length = text_part(g, data, 1);
        } else {
            pointer = at_ir_temp(g, 0);
            length = at_ir_temp(g, AT_I64);
            at_ir_emit(g, "  %s = call ptr @at_buffer_data(ptr %s)\n", pointer.text, data.text);
            at_ir_emit(g, "  %s = call i64 @at_buffer_len(ptr %s)\n", length.text, data.text);
        }
        at_ir_emit(g,
                   "  %s = call i32 @at_port_write(ptr %%portresult%d, ptr %s, ptr %s, i64 %s)\n",
                   status.text, node->id, receiver.text, pointer.text, length.text);
    } else if (node->op == AT_PORT_LINE) {
        at_ir_emit(
            g, "  %s = call i32 @at_port_line(ptr %%portresult%d, ptr %%portpresent%d, ptr %s)\n",
            status.text, node->id, node->id, receiver.text);
    } else if (node->op == AT_PORT_READ_ALL) {
        at_ir_emit(g, "  %s = call i32 @at_port_readall(ptr %%portresult%d, ptr %s)\n", status.text,
                   node->id, receiver.text);
    } else {
        Val count = at_ir_expression(g, node->args[0]);
        at_ir_emit(g, "  %s = call i32 @at_port_read(ptr %%portresult%d, ptr %s, i64 %s, i32 %d)\n",
                   status.text, node->id, receiver.text, count.text,
                   node->op == AT_PORT_READ_EXACT);
    }
    at_ir_runtime_status(g, status, node);
    if (node->op == AT_PORT_LINE || node->op == AT_PORT_READ) {
        int type = node->op == AT_PORT_LINE ? AT_STR : AT_BYTES;
        Val value = at_ir_temp(g, type), present = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = load %s, ptr %%portresult%d\n", value.text, at_ir_type(g, type),
                   node->id);
        if (node->op == AT_PORT_LINE) {
            Val flag = at_ir_temp(g, AT_I32);
            at_ir_emit(g, "  %s = load i32, ptr %%portpresent%d\n", flag.text, node->id);
            at_ir_emit(g, "  %s = icmp ne i32 %s, 0\n", present.text, flag.text);
        } else {
            at_ir_emit(g, "  %s = icmp ne ptr %s, null\n", present.text, value.text);
        }
        return optional_result(g, node, value, present);
    }
    at_ir_emit(g, "  %s = load %s, ptr %%portresult%d\n", result.text, at_ir_type(g, result.type),
               node->id);
    return result;
}

void at_ir_port_loop(Gen *g, AtNode *node)
{
    Val port = at_ir_expression(g, node->b);
    int before_break = g->break_label, before_continue = g->continue_label;
    AtCleanup *before_cleanup = g->loop_cleanup;
    int condition = at_ir_label(g), body = at_ir_label(g), done = at_ir_label(g);
    g->break_label = done;
    g->continue_label = condition;
    g->loop_cleanup = g->cleanup;
    at_ir_jump(g, condition);
    at_ir_mark(g, condition);
    Val status = at_ir_temp(g, AT_I32);
    at_ir_emit(g,
               "  %s = call i32 @at_port_line(ptr %%portresult%d, ptr %%portpresent%d, ptr %s)\n",
               status.text, node->id, node->id, port.text);
    at_ir_runtime_status(g, status, node);
    Val present = at_ir_temp(g, AT_I32), available = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = load i32, ptr %%portpresent%d\n", present.text, node->id);
    at_ir_emit(g, "  %s = icmp ne i32 %s, 0\n", available.text, present.text);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", available.text, body, done);
    at_ir_mark(g, body);
    Val line = at_ir_temp(g, AT_STR);
    at_ir_emit(g, "  %s = load { ptr, i64 }, ptr %%portresult%d\n", line.text, node->id);
    at_ir_store_binding(g, node->a, line);
    at_ir_statements(g, node->c);
    at_ir_jump(g, condition);
    at_ir_mark(g, done);
    g->break_label = before_break;
    g->continue_label = before_continue;
    g->loop_cleanup = before_cleanup;
}
