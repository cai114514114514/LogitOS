/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <stdio.h>
#include <string.h>

/* Shared resource binding and escape checks belong here, not in the Port
 * method checker. Region ownership can extend this pass without coupling
 * memory lifetimes to descriptor methods. */
int at_resource_type(int type)
{
    return type == AT_PORT || type == AT_PROCESS || type == AT_REGION;
}

int at_check_resource_receiver(Checker *checker, AtNode *node)
{
    AtNode *previous = checker->resource_use;
    checker->resource_use = node;
    int type = at_check_expression(checker, node, 0);
    checker->resource_use = previous;
    return type;
}

static int bind_owner(Checker *checker, AtNode *owner, int type)
{
    char name[64];
    snprintf(name, sizeof name, "$resource%d", owner->id);
    int slot = -1;
    for (int i = 0; i < checker->f->nlocals; i++) {
        if (!strcmp(checker->f->locals[i].name, name)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = at_scope_add_local(checker, owner, type);
    }
    if (slot >= 0) {
        AtLocal *local = &checker->f->locals[slot];
        strcpy(local->name, name);
        local->type = type;
        local->initialized = AT_LOCAL_INITIALIZED;
        local->scoped_borrow = at_slice_kind(checker->p->types[type].kind);
        owner->type = type;
        owner->symbol = slot;
    }
    return slot;
}

int at_check_resource_binding(Checker *checker, AtNode *node)
{
    AtNode *previous = checker->resource_acquisition;
    checker->resource_acquisition = node->b;
    int type = at_check_expression(checker, node->b, 0);
    checker->resource_acquisition = previous;
    int pair = node->a->kind == AN_UNPACK;
    int pipe_call = node->b->kind == AN_CALL && node->b->symbol == AT_CALL_PORT_PIPE;
    if (!pair && at_slice_kind(checker->p->types[type].kind) && node->b->kind == AN_CALL &&
        node->b->symbol == AT_CALL_REGION_BORROW) {
        return bind_owner(checker, node->a, type) >= 0;
    }
    if (!pair && type == AT_REGION && node->b->kind == AN_CALL &&
        (node->b->symbol == AT_CALL_REGION || node->b->symbol == AT_CALL_REGION_MOVE)) {
        return bind_owner(checker, node->a, AT_REGION) >= 0;
    }
    if (!pair && (type == AT_COMMAND || type == AT_PROCESS)) {
        return bind_owner(checker, node->a, AT_PROCESS) >= 0;
    }
    if (pair != pipe_call) {
        at_check_error(checker, node, "AS3401",
                       "pipe requires exactly two owner names; open and port require one");
        return 0;
    }
    if (!pipe_call &&
        (type != AT_PORT || node->b->kind != AN_CALL ||
         (node->b->symbol != AT_CALL_PORT_OPEN && node->b->symbol != AT_CALL_PORT_BORROW))) {
        at_check_error(checker, node, "AS3401", "with requires a newly acquired resource owner");
        return 0;
    }
    if (pair) {
        Token first = node->a->args[0]->token;
        Token second = node->a->args[1]->token;
        if (first.len == second.len && !memcmp(first.start, second.start, (size_t)first.len)) {
            at_check_error(checker, node->a->args[1], "AS3401",
                           "Pipe endpoints need distinct owner names");
            return 0;
        }
    }
    int count = pair ? 2 : 1;
    for (int i = 0; i < count; i++) {
        AtNode *owner = pair ? node->a->args[i] : node->a;
        if (bind_owner(checker, owner, AT_PORT) < 0) {
            return 0;
        }
    }
    return count;
}

void at_check_resource_types(AsTypedProject *project)
{
    /* Transfer/aggregate ownership requires additional move and drop analysis.
     * Until those rules exist, reject storing owners rather than silently
     * copying an fd into a GC object or returning a freed scope allocation. */
    for (int i = 0; i < project->ntypes; i++) {
        AtType *type = &project->types[i];
        int contains = at_resource_type(type->element) || at_resource_type(type->key);
        if (type->kind == AT_STRUCT || type->kind == AT_CLASS || type->kind == AT_CALLABLE) {
            for (int j = 0; j < type->count; j++) {
                contains |= at_resource_type(type->fields[j]);
            }
        }
        if (contains) {
            Token site = {.start = project->modules[type->module].source, .line = 1};
            at_error(project, type->module, site, "AS3401",
                     "Resource owners cannot be stored in aggregates or call signatures yet");
        }
    }
    for (int i = 0; i < project->nfunctions; i++) {
        AtFunction *function = &project->functions[i];
        int contains = at_resource_type(function->result);
        for (int j = 0; j < function->nparams; j++) {
            contains |= at_resource_type(function->locals[j].type);
        }
        if (contains) {
            at_error(project, function->module, function->token, "AS3401",
                     "Resource ownership transfer across calls is not implemented yet");
        }
    }
}
