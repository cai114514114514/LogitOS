/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

void at_ir_cleanup_to(Gen *g, AtCleanup *stop, int unwinding)
{
    AtCleanup *saved = g->cleanup;
    AtCleanup *saved_handler_cleanup = g->handler_cleanup;
    int saved_handler = g->handler_label;
    for (AtCleanup *cleanup = saved; cleanup != stop; cleanup = cleanup->previous) {
        AtNode *owner = cleanup->owner;
        Val resource = at_ir_temp(g, owner->type), status = at_ir_temp(g, AT_I32);
        if (owner->type == AT_REGION || at_slice_kind(g->p->types[owner->type].kind)) {
            /* Region release consumes a slot, including the NULL slot left by
             * move. It is not the by-value Port/Process release ABI. */
            const char *release =
                owner->type == AT_REGION ? "at_region_release" : "at_region_borrow_release";
            at_ir_emit(g, "  %s = call i32 @%s(ptr %%resource%d)\n", status.text, release,
                       owner->id);
            at_ir_emit(g, "  store %s zeroinitializer, ptr %%local%d\n", at_ir_type(g, owner->type),
                       owner->symbol);
        } else {
            at_ir_emit(g, "  %s = load ptr, ptr %%resource%d\n", resource.text, owner->id);
            at_ir_emit(g, "  store ptr null, ptr %%resource%d\n", owner->id);
            const char *release =
                owner->type == AT_PROCESS ? "at_command_release" : "at_port_release";
            at_ir_emit(g, "  %s = call i32 @%s(ptr %s)\n", status.text, release, resource.text);
        }
        if (!unwinding) {
            /* A close failure belongs outside the scope being closed. An
             * inner try crossed by return must not catch an outer owner's
             * close error and resume with a resource we have already freed. */
            g->cleanup = cleanup->previous;
            g->handler_cleanup = cleanup->handler_cleanup;
            g->handler_label = cleanup->handler_label;
            at_ir_runtime_status(g, status, owner);
        }
        /* During propagation retain the original exception; release cannot
         * allocate, throw or overwrite that pending rooted exception. */
    }
    g->cleanup = saved;
    g->handler_cleanup = saved_handler_cleanup;
    g->handler_label = saved_handler;
}

void at_ir_with(Gen *g, AtNode *node)
{
    int count = node->a->kind == AN_UNPACK ? 2 : 1;
    AtNode *owners[2] = {node->a, NULL};
    Val view = {0};
    int borrowing = node->b->symbol == AT_CALL_REGION_BORROW;
    if (count == 2) {
        owners[0] = node->a->args[0];
        owners[1] = node->a->args[1];
        Val status = at_ir_temp(g, AT_I32);
        at_ir_emit(g, "  %s = call i32 @at_port_pipe(ptr %%resource%d, ptr %%resource%d)\n",
                   status.text, owners[0]->id, owners[1]->id);
        at_ir_runtime_status(g, status, node->b);
    } else {
        Val acquired = at_ir_expression(g, node->b);
        if (acquired.type == AT_COMMAND) {
            Val status = at_ir_temp(g, AT_I32);
            at_ir_emit(g, "  %s = call i32 @at_command_acquire(ptr %s)\n", status.text,
                       acquired.text);
            at_ir_runtime_status(g, status, node->b);
        }
        if (borrowing) {
            /* A Slice is a plain {data, length} value; its opaque loan record
             * lives in the independent cleanup slot and must not be copied. */
            view = acquired;
            Val record = at_ir_temp(g, 0);
            at_ir_emit(g, "  %s = load ptr, ptr %%regionresult%d\n", record.text, node->b->id);
            at_ir_emit(g, "  store ptr %s, ptr %%resource%d\n", record.text, owners[0]->id);
            at_ir_emit(g, "  store ptr null, ptr %%regionresult%d\n", node->b->id);
        } else {
            at_ir_emit(g, "  store ptr %s, ptr %%resource%d\n", acquired.text, owners[0]->id);
        }
    }

    /* Register both ends only after atomic acquisition. Pushing in source
     * order makes every exit release the writer first, then the reader. */
    AtCleanup *previous = g->cleanup;
    AtCleanup cleanups[2];
    for (int i = 0; i < count; i++) {
        Val acquired = at_ir_temp(g, owners[i]->type);
        if (borrowing) {
            acquired = view;
        } else {
            at_ir_emit(g, "  %s = load ptr, ptr %%resource%d\n", acquired.text, owners[i]->id);
        }
        at_ir_store_binding(g, owners[i], acquired);
        cleanups[i] = (AtCleanup){owners[i], g->cleanup, g->handler_label, g->handler_cleanup};
        g->cleanup = &cleanups[i];
    }
    at_ir_statements(g, node->c);
    if (!g->terminated) {
        at_ir_cleanup_to(g, previous, 0);
    }
    g->cleanup = previous;
}
