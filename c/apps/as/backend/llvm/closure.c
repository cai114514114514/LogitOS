/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include <stdio.h>

int at_ir_template_function(AsTypedProject *project, AtFunction *function)
{
    for (;;) {
        if (function->generic_count) {
            return 1;
        }
        if (!function->lexical_parent) {
            return 0;
        }
        function = &project->functions[function->lexical_parent - 1];
    }
}

static void cell_slot(Gen *g, int slot)
{
    char name[64];
    at_ir_emit(g, "  %%cellroot%d = alloca ptr\n", slot);
    at_ir_emit(g, "  store ptr null, ptr %%cellroot%d\n", slot);
    snprintf(name, sizeof name, "%%cellroot%d", slot);
    /* scanAny marks one allocation pointer without inspecting an Any header.
     * Here that same pointer scanner retains the cell itself, whose allocation
     * metadata supplies the independent scanner for its concrete payload. */
    at_ir_root(g, name, AT_ANY);
}

void at_ir_function_locals(Gen *g)
{
    AtFunction *function = g->f;
    for (int slot = 0; slot < function->nlocals; slot++) {
        AtLocal *local = &function->locals[slot];
        if (!local->type) {
            continue; /* Unreachable bindings have no storage layout. */
        }
        const char *type = at_ir_type(g, local->type);
        if (local->captured) {
            cell_slot(g, slot);
            if (local->capture_parent) {
                at_ir_emit(g,
                           "  %%local%d = call ptr @at_closure_cell(ptr %%environment, i64 %d)\n",
                           slot, local->capture_index);
                at_ir_emit(g, "  store ptr %%local%d, ptr %%cellroot%d\n", slot, slot);
            } else if (slot < function->nparams && at_ir_references(g, local->type)) {
                /* Root incoming managed arguments BEFORE allocating any cells.
                 * Parameters still live in registers at the first safepoint. */
                char name[64];
                at_ir_emit(g, "  %%parameter%d = alloca %s\n", slot, type);
                at_ir_emit(g, "  store %s %%arg%d, ptr %%parameter%d\n", type, slot, slot);
                snprintf(name, sizeof name, "%%parameter%d", slot);
                at_ir_root(g, name, local->type);
            }
            continue;
        }
        at_ir_emit(g, "  %%local%d = alloca %s\n", slot, type);
        at_ir_emit(g, "  store %s zeroinitializer, ptr %%local%d\n", type, slot);
        if (slot < function->nparams) {
            at_ir_emit(g, "  store %s %%arg%d, ptr %%local%d\n", type, slot, slot);
        }
        if (at_ir_references(g, local->type)) {
            char name[64];
            snprintf(name, sizeof name, "%%local%d", slot);
            at_ir_root(g, name, local->type);
        }
    }
    for (int slot = 0; slot < function->nlocals; slot++) {
        AtLocal *local = &function->locals[slot];
        if (!local->type || !local->captured || local->capture_parent) {
            continue;
        }
        const char *type = at_ir_type(g, local->type);
        at_ir_emit(g,
                   "  %%local%d = call ptr @at_object_new(i64 ptrtoint (ptr getelementptr "
                   "(%s, ptr null, i32 1) to i64), ptr @scan%d)\n",
                   slot, type, local->type);
        Val cell = at_ir_value(0, "");
        snprintf(cell.text, sizeof cell.text, "%%local%d", slot);
        AtNode site = {.module = function->module, .token = local->token};
        at_ir_allocation(g, cell, &site, 1);
        at_ir_emit(g, "  store ptr %%local%d, ptr %%cellroot%d\n", slot, slot);
        if (slot < function->nparams) {
            at_ir_emit(g, "  store %s %%arg%d, ptr %%local%d\n", type, slot, slot);
        }
    }
}

Val at_ir_closure(Gen *g, AtNode *node, Val result)
{
    AtFunction *function = &g->p->functions[node->symbol];
    for (int slot = 0; slot < function->nlocals; slot++) {
        AtLocal *local = &function->locals[slot];
        if (!local->capture_parent) {
            continue;
        }
        Val field = at_ir_temp(g, 0);
        at_ir_emit(g, "  %s = getelementptr [%d x ptr], ptr %%captures%d, i32 0, i32 %d\n",
                   field.text, function->capture_count, node->id, local->capture_index);
        at_ir_emit(g, "  store ptr %%local%d, ptr %s\n", local->capture_parent - 1, field.text);
    }
    Val environment = at_ir_temp(g, 0), code = at_ir_temp(g, node->type);
    at_ir_emit(g, "  %s = call ptr @at_closure_new(i64 %d, ptr %%captures%d)\n", environment.text,
               function->capture_count, node->id);
    at_ir_allocation(g, environment, node, 1);
    at_ir_emit(g, "  %s = insertvalue %s zeroinitializer, ptr @fn%d, 0\n", code.text,
               at_ir_type(g, node->type), node->symbol);
    at_ir_emit(g, "  %s = insertvalue %s %s, ptr %s, 1\n", result.text, at_ir_type(g, node->type),
               code.text, environment.text);
    return result;
}
