/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include "runtime/capability_bits.h"

void at_ir_require_raw(Gen *g, AtNode *site)
{
    Val allowed = at_ir_temp(g, AT_I32), denied = at_ir_temp(g, AT_BOOL);
    /* Unsafe is a lexical memory contract, not a grant. Recheck held rights
     * on each operation; a previously obtained integer address conveys none. */
    at_ir_emit(g, "  %s = call i32 @at_caps_have(i32 %u)\n", allowed.text, AS_CAP_RAW);
    at_ir_emit(g, "  %s = icmp eq i32 %s, 0\n", denied.text, allowed.text);
    at_ir_guard(g, denied, site, AT_E_PERMISSION);
}

Val at_ir_memory(Gen *g, AtNode *node, Val result)
{
    Val address = at_ir_expression(g, node->args[0]);
    if (node->symbol == AT_CALL_ALLOC || node->symbol == AT_CALL_DEALLOC) {
        Val status = at_ir_temp(g, AT_I32);
        if (node->symbol == AT_CALL_ALLOC) {
            at_ir_emit(g, "  %s = call i32 @at_manual_alloc(ptr %%storage%d, i64 %s)\n",
                       status.text, node->id, address.text);
        } else {
            at_ir_emit(g, "  %s = call i32 @at_manual_dealloc(i64 %s)\n", status.text,
                       address.text);
        }
        /* The runtime checks current authority before changing ownership.
         * Propagate failures through the same cleanup paths as other calls. */
        at_ir_runtime_status(g, status, node);
        if (node->symbol == AT_CALL_ALLOC) {
            at_ir_emit(g, "  %s = load i64, ptr %%storage%d\n", result.text, node->id);
            return result;
        }
        return at_ir_value(AT_VOID, "");
    }
    Val value = {0};
    if (node->symbol == AT_CALL_POKE) {
        value = at_ir_expression(g, node->args[1]);
    }
    at_ir_require_raw(g, node);
    if (node->symbol == AT_CALL_POINTER ||
        (node->symbol == AT_CALL_ADDR && g->p->types[address.type].kind == AT_POINTER)) {
        at_ir_emit(g, "  %s = add i64 0, %s\n", result.text, address.text);
        return result;
    }
    Val pointer = at_ir_temp(g, 0);
    if (node->symbol == AT_CALL_ADDR) {
        if (at_byte_storage_kind(g->p->types[address.type].kind)) {
            at_ir_emit(g, "  %s = call ptr @at_buffer_data(ptr %s)\n", pointer.text, address.text);
        } else {
            at_ir_emit(g, "  %s = extractvalue { ptr, i64 } %s, 0\n", pointer.text, address.text);
        }
        at_ir_emit(g, "  %s = ptrtoint ptr %s to i64\n", result.text, pointer.text);
        return result;
    }

    Val null = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp eq i64 %s, 0\n", null.text, address.text);
    at_ir_guard(g, null, node, AT_E_VALUE);
    at_ir_emit(g, "  %s = inttoptr i64 %s to ptr\n", pointer.text, address.text);
    if (node->symbol == AT_CALL_PEEK) {
        if (node->op == 64) {
            at_ir_emit(g, "  %s = load i64, ptr %s, align 1\n", result.text, pointer.text);
        } else {
            Val bits = at_ir_temp(g, 0);
            at_ir_emit(g, "  %s = load i%d, ptr %s, align 1\n", bits.text, node->op, pointer.text);
            at_ir_emit(g, "  %s = zext i%d %s to i64\n", result.text, node->op, bits.text);
        }
        return result;
    }

    /* The raw API writes low bits in native byte order, matching its existing
     * contract. It does not inherit checked Buffer indexing semantics. align 1
     * permits deliberately unaligned field reads/writes without LLVM UB. */
    if (node->op != 64) {
        Val truncated = at_ir_temp(g, 0);
        at_ir_emit(g, "  %s = trunc i64 %s to i%d\n", truncated.text, value.text, node->op);
        value = truncated;
    }
    at_ir_emit(g, "  store i%d %s, ptr %s, align 1\n", node->op, value.text, pointer.text);
    return at_ir_value(AT_VOID, "");
}

Val at_ir_memory_text(Gen *g, AtNode *node, Val result)
{
    Val memory = at_ir_expression(g, node->args[0]);
    Val length =
        node->count == 2 ? at_ir_expression(g, node->args[1]) : at_ir_value(AT_I64, "4096");
    Val pointer = at_ir_temp(g, 0);
    Val capacity = at_ir_value(AT_I64, "-1");
    if (memory.type == AT_U64 || g->p->types[memory.type].kind == AT_POINTER) {
        at_ir_emit(g, "  %s = inttoptr i64 %s to ptr\n", pointer.text, memory.text);
    } else {
        capacity = at_ir_temp(g, AT_I64);
        at_ir_emit(g, "  %s = call ptr @at_buffer_data(ptr %s)\n", pointer.text, memory.text);
        at_ir_emit(g, "  %s = call i64 @at_buffer_len(ptr %s)\n", capacity.text, memory.text);
    }
    /* The runtime copies bytes into managed text, so neither a returned
     * substring nor later collection can retain mutable external storage. */
    Val status = at_ir_temp(g, AT_I32);
    at_ir_emit(g,
               "  %s = call i32 @at_memory_text(ptr %%nativeresult%d, ptr %s, i64 %s, "
               "i64 %s, i32 %d)\n",
               status.text, node->id, pointer.text, capacity.text, length.text, node->op);
    at_ir_runtime_status(g, status, node);
    at_ir_emit(g, "  %s = load { ptr, i64 }, ptr %%nativeresult%d\n", result.text, node->id);
    return result;
}
