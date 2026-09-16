/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

Val at_ir_region(Gen *g, AtNode *node, Val result)
{
    Val status = at_ir_temp(g, AT_I32);
    if (node->symbol == AT_CALL_REGION) {
        Val length = at_ir_expression(g, node->args[0]);
        at_ir_emit(g, "  %s = call i32 @at_region_new(ptr %%regionresult%d, i64 %s)\n", status.text,
                   node->id, length.text);
    } else {
        int source = node->a->a->symbol;
        AtCleanup *cleanup = g->cleanup;
        while (cleanup && cleanup->owner->symbol != source) {
            cleanup = cleanup->previous;
        }
        if (!cleanup) {
            /* The checker permits only a lexically scoped owner. Never emit a
             * guessed address when that invariant is broken by a later pass. */
            g->failed = 1;
            at_error(g->p, node->module, node->token, "AS3501", "Region move has no scoped owner");
            return at_ir_value(AT_REGION, "null");
        }
        at_ir_emit(g, "  %s = call i32 @at_region_move(ptr %%regionresult%d, ptr %%resource%d)\n",
                   status.text, node->id, cleanup->owner->id);
    }
    at_ir_runtime_status(g, status, node);
    if (node->symbol == AT_CALL_REGION_MOVE) {
        /* The runtime consumes the cleanup slot; clear the visible local too.
         * Clearing only the local would double-free on the outer scope exit. */
        at_ir_emit(g, "  store ptr null, ptr %%local%d\n", node->a->a->symbol);
    }
    at_ir_emit(g, "  %s = load ptr, ptr %%regionresult%d\n", result.text, node->id);
    /* Entry-block workspaces are reused on every loop iteration. Ownership
     * transfers immediately into the with slot without a throwing operation. */
    at_ir_emit(g, "  store ptr null, ptr %%regionresult%d\n", node->id);
    return result;
}

Val at_ir_region_at(Gen *g, Val owner, Val index, AtNode *site, int writable)
{
    Val status = at_ir_temp(g, AT_I32), address = at_ir_temp(g, 0);
    at_ir_emit(
        g, "  %s = call i32 @at_region_address(ptr %%regionaddress%d, ptr %s, i64 %s, i32 %d)\n",
        status.text, site->id, owner.text, index.text, writable);
    at_ir_runtime_status(g, status, site);
    at_ir_emit(g, "  %s = load ptr, ptr %%regionaddress%d\n", address.text, site->id);
    return address;
}

Val at_ir_region_borrow(Gen *g, AtNode *node, Val result)
{
    AtNode *source = node->a->a;
    Val owner = at_ir_expression(g, source);
    if (source->type != AT_REGION) {
        AtCleanup *cleanup = g->cleanup;
        while (cleanup && cleanup->owner->symbol != source->symbol) {
            cleanup = cleanup->previous;
        }
        if (!cleanup) {
            g->failed = 1;
            at_error(g->p, node->module, node->token, "AS3501",
                     "Reborrow has no scoped loan record");
            return at_ir_value(node->type, "zeroinitializer");
        }
        owner = at_ir_temp(g, 0);
        at_ir_emit(g, "  %s = load ptr, ptr %%resource%d\n", owner.text, cleanup->owner->id);
    }
    Val start = at_ir_expression(g, node->args[0]);
    Val stop = at_ir_expression(g, node->args[1]);
    Val status = at_ir_temp(g, AT_I32);
    const char *acquire = source->type == AT_REGION ? "at_region_borrow" : "at_region_reborrow";
    at_ir_emit(g, "  %s = call i32 @%s(ptr %%regionresult%d, ptr %s, i64 %s, i64 %s, i32 %d)\n",
               status.text, acquire, node->id, owner.text, start.text, stop.text, node->op);
    at_ir_runtime_status(g, status, node);
    Val loan = at_ir_temp(g, 0), data = at_ir_temp(g, 0), length = at_ir_temp(g, AT_I64);
    Val partial = at_ir_temp(g, node->type);
    at_ir_emit(g, "  %s = load ptr, ptr %%regionresult%d\n", loan.text, node->id);
    at_ir_emit(g, "  %s = call ptr @at_region_borrow_data(ptr %s)\n", data.text, loan.text);
    /* Acquisition checked 0 <= start <= stop <= extent before subtraction.
     * The descriptor remains in the result slot until with registers cleanup;
     * no allocation or throwing operation may be inserted into this transfer. */
    at_ir_emit(g, "  %s = sub i64 %s, %s\n", length.text, stop.text, start.text);
    at_ir_emit(g, "  %s = insertvalue { ptr, i64 } zeroinitializer, ptr %s, 0\n", partial.text,
               data.text);
    at_ir_emit(g, "  %s = insertvalue { ptr, i64 } %s, i64 %s, 1\n", result.text, partial.text,
               length.text);
    return result;
}
