/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <stdlib.h>
#include <string.h>

/* Ownership is a separate forward dataflow analysis. Ordinary initialization
 * restores the pre-loop state and cannot represent a consumed owner. Each edge
 * below records which owners are live on EVERY path reaching that edge. */
#define OWNER_WORDS ((AT_LOCALS + 63) / 64)

enum OwnerExit {
    OWNER_NEXT,
    OWNER_RETURN,
    OWNER_BREAK,
    OWNER_CONTINUE,
    OWNER_THROW,
    OWNER_EXITS
};

typedef struct {
    int reachable;
    uint64_t live[OWNER_WORDS];
} OwnerPath;

typedef struct {
    OwnerPath paths[OWNER_EXITS];
} OwnerFlow;

typedef struct {
    AsTypedProject *project;
    unsigned char *reported;
} OwnerCheck;

static void join_path(OwnerPath *destination, const OwnerPath *source)
{
    if (!source->reachable) {
        return;
    }
    if (!destination->reachable) {
        *destination = *source;
        return;
    }
    for (int word = 0; word < OWNER_WORDS; word++) {
        destination->live[word] &= source->live[word];
    }
}

static void join_flow(OwnerFlow *destination, const OwnerFlow *source)
{
    for (int exit = 0; exit < OWNER_EXITS; exit++) {
        join_path(&destination->paths[exit], &source->paths[exit]);
    }
}

static void set_live(OwnerPath *path, int slot, int live)
{
    uint64_t mask = UINT64_C(1) << (slot % 64);
    if (live) {
        path->live[slot / 64] |= mask;
    } else {
        path->live[slot / 64] &= ~mask;
    }
}

static int visit_expression(OwnerCheck *check, AtNode *node, const OwnerPath *path)
{
    if (!node || !path->reachable) {
        return 0;
    }
    if (node->kind == AN_NAME && node->type == AT_REGION && node->symbol >= 0) {
        int slot = node->symbol;
        uint64_t mask = UINT64_C(1) << (slot % 64);
        if (!(path->live[slot / 64] & mask) && !check->reported[node->id]) {
            AsDiagnostics *diagnostics = &check->project->modules[node->module].diagnostics;
            int previous_count = diagnostics->count;
            at_error(check->project, node->module, node->token, "AS3402",
                     "Region may have been moved or released on a path reaching this use");
            if (diagnostics->count > previous_count) {
                snprintf(diagnostics->items[previous_count].help,
                         sizeof diagnostics->items[previous_count].help,
                         "Use the destination inside its with scope; after a branch, use an "
                         "owner only if it remains live on every reaching path.");
            }
            check->reported[node->id] = 1;
        }
    }
    if (node->kind == AN_CLOSURE || node->kind == AN_FUNCTION) {
        /* Nested bodies have their own flow. Capturing an owner is already
         * rejected by the shared resource checker, before this pass runs. */
        return node->kind == AN_CLOSURE;
    }

    int throwing = visit_expression(check, node->a, path);
    throwing |= visit_expression(check, node->b, path);
    throwing |= visit_expression(check, node->c, path);
    for (int i = 0; i < node->count; i++) {
        throwing |= visit_expression(check, node->args[i], path);
    }
    switch (node->kind) {
    case AN_CALL:
    case AN_INDEX:
    case AN_FIELD:
    case AN_GLOBAL:
    case AN_BINARY:
    case AN_UNARY:
    case AN_FORMAT:
    case AN_ARRAY:
    case AN_DICT:
    case AN_COMPREHENSION:
        return 1;
    default:
        return throwing;
    }
}

static OwnerFlow statements(OwnerCheck *check, AtNode *node, OwnerPath input);

static OwnerFlow with_owner(OwnerCheck *check, AtNode *node, OwnerPath input)
{
    OwnerFlow result = {0};
    visit_expression(check, node->b, &input);
    /* Acquisition can fail before publishing the new owner. In particular a
     * move's failure edge retains its source, while its success edge consumes
     * that source. A later body exception must carry the consumed state. */
    join_path(&result.paths[OWNER_THROW], &input);
    int region = node->a->type == AT_REGION;
    if (region) {
        if (node->b->symbol == AT_CALL_REGION_MOVE) {
            set_live(&input, node->b->a->a->symbol, 0);
        }
        set_live(&input, node->a->symbol, 1);
    }
    OwnerFlow body = statements(check, node->c, input);
    for (int exit = 0; exit < OWNER_EXITS; exit++) {
        if (region) {
            set_live(&body.paths[exit], node->a->symbol, 0);
        } else if (exit != OWNER_THROW) {
            /* Port close / Process wait can fail while crossing return or a
             * loop edge. The enclosing handler sees ownership at that exit,
             * not the state at entry to its try block. */
            join_path(&result.paths[OWNER_THROW], &body.paths[exit]);
        }
    }
    join_flow(&result, &body);
    return result;
}

static OwnerFlow loop(OwnerCheck *check, AtNode *node, OwnerPath input)
{
    OwnerFlow result = {0};
    AtNode *condition = node->kind == AN_WHILE ? node->a : node->b;
    AtNode *body_node = node->kind == AN_WHILE ? node->b : node->c;
    int throwing = visit_expression(check, condition, &input);
    if (throwing) {
        join_path(&result.paths[OWNER_THROW], &input);
    }

    OwnerPath header = input;
    OwnerFlow body;
    for (;;) {
        if (node->kind == AN_WHILE) {
            visit_expression(check, condition, &header);
        }
        body = statements(check, body_node, header);
        OwnerPath back = body.paths[OWNER_NEXT];
        join_path(&back, &body.paths[OWNER_CONTINUE]);
        OwnerPath next_header = input;
        join_path(&next_header, &back);
        if (!memcmp(header.live, next_header.live, sizeof header.live)) {
            break;
        }
        /* This descending finite bitset reaches a fixed point in at most one
         * change per owner. break/return never feed the back edge: moving an
         * outer owner then breaking is valid; moving it on every iteration is
         * not. Owners acquired inside the body start live on each iteration. */
        header = next_header;
    }
    result.paths[OWNER_NEXT] = header; /* The loop can execute zero times. */
    join_path(&result.paths[OWNER_NEXT], &body.paths[OWNER_BREAK]);
    join_path(&result.paths[OWNER_RETURN], &body.paths[OWNER_RETURN]);
    join_path(&result.paths[OWNER_THROW], &body.paths[OWNER_THROW]);
    if (throwing && node->kind == AN_WHILE) {
        join_path(&result.paths[OWNER_THROW], &header);
    }
    return result;
}

static OwnerFlow try_handlers(OwnerCheck *check, AtNode *node, OwnerPath input)
{
    OwnerFlow body = statements(check, node->a, input);
    OwnerFlow result = body;
    memset(&result.paths[OWNER_NEXT], 0, sizeof(OwnerPath));
    memset(&result.paths[OWNER_THROW], 0, sizeof(OwnerPath));
    OwnerFlow otherwise = statements(check, node->c, body.paths[OWNER_NEXT]);
    join_flow(&result, &otherwise);
    int catches_all = 0;
    for (AtNode *handler = node->b; handler; handler = handler->next) {
        OwnerFlow handled = statements(check, handler->b, body.paths[OWNER_THROW]);
        join_flow(&result, &handled);
        if (handler->op == AT_E_ANY) {
            catches_all = 1;
            break;
        }
    }
    if (!catches_all) {
        join_path(&result.paths[OWNER_THROW], &body.paths[OWNER_THROW]);
    }
    return result;
}

static OwnerFlow statement(OwnerCheck *check, AtNode *node, OwnerPath input)
{
    OwnerFlow result = {0};
    if (node->kind == AN_WITH) {
        return with_owner(check, node, input);
    }
    if (node->kind == AN_UNSAFE) {
        return statements(check, node->a, input);
    }
    if (node->kind == AN_TRY) {
        return try_handlers(check, node, input);
    }
    if (node->kind == AN_WHILE || node->kind == AN_FOR) {
        return loop(check, node, input);
    }
    if (node->kind == AN_IF) {
        if (visit_expression(check, node->a, &input)) {
            join_path(&result.paths[OWNER_THROW], &input);
        }
        OwnerFlow yes = statements(check, node->b, input);
        OwnerFlow no = statements(check, node->c, input);
        join_flow(&result, &yes);
        join_flow(&result, &no);
        return result;
    }

    if (visit_expression(check, node, &input) || node->kind == AN_ASSERT) {
        join_path(&result.paths[OWNER_THROW], &input);
    }
    int exit = node->kind == AN_RETURN     ? OWNER_RETURN
               : node->kind == AN_BREAK    ? OWNER_BREAK
               : node->kind == AN_CONTINUE ? OWNER_CONTINUE
               : node->kind == AN_RAISE    ? OWNER_THROW
                                           : OWNER_NEXT;
    join_path(&result.paths[exit], &input);
    return result;
}

static OwnerFlow statements(OwnerCheck *check, AtNode *node, OwnerPath input)
{
    OwnerFlow result = {0};
    result.paths[OWNER_NEXT] = input;
    for (; node && result.paths[OWNER_NEXT].reachable; node = node->next) {
        OwnerFlow step = statement(check, node, result.paths[OWNER_NEXT]);
        memset(&result.paths[OWNER_NEXT], 0, sizeof(OwnerPath));
        join_flow(&result, &step);
    }
    return result;
}

void at_check_region_flow(AsTypedProject *project)
{
    unsigned char *reported = calloc((size_t)project->nnodes + 1, 1);
    if (!reported) {
        project->oom = 1;
        return;
    }
    OwnerCheck check = {project, reported};
    for (int i = 0; i < project->nfunctions; i++) {
        AtFunction *function = &project->functions[i];
        int has_region = 0;
        for (int local = 0; local < function->nlocals; local++) {
            has_region |= function->locals[local].type == AT_REGION;
        }
        if (function->checked && has_region) {
            statements(&check, function->body, (OwnerPath){.reachable = 1});
        }
    }
    free(reported);
}
