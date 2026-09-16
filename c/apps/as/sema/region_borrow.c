/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

/* Explicit borrow acquisitions are lexical with scopes; their view bindings
 * cannot be copied, stored, captured or returned. Parameters receive temporary
 * call loans instead. Read-only parameter aliases stay within the callee and
 * cannot escape it. Conflicts therefore depend on active lexical/call loans,
 * while moved-owner liveness remains the separate fixed-point analysis. */
typedef struct Loan {
    int source;
    int writable;
    AtNode *site;
    struct Loan *previous;
} Loan;

typedef struct {
    AsTypedProject *project;
    Loan *loans;
} BorrowCheck;

enum Access {
    ACCESS_READ,
    ACCESS_WRITE,
    ACCESS_MOVE,
    ACCESS_SHARED,
    ACCESS_EXCLUSIVE
};

static void conflict(BorrowCheck *check, AtNode *source, Loan *loan)
{
    AsDiagnostics *diagnostics = &check->project->modules[source->module].diagnostics;
    int previous_count = diagnostics->count;
    at_error(check->project, source->module, source->token, "AS3403",
             "This access conflicts with an active borrow");
    if (diagnostics->count == previous_count) {
        return;
    }
    AsDiagnostic *diagnostic = &diagnostics->items[previous_count];
    snprintf(diagnostic->help, sizeof diagnostic->help,
             "End the view's with scope before this access, or compute later arguments before "
             "passing an exclusive view to the call.");
    AtModule *module = &check->project->modules[loan->site->module];
    Token token = loan->site->token;
    uintptr_t begin = (uintptr_t)module->source, position = (uintptr_t)token.start;
    if (position < begin || position - begin > module->diagnostics.bytes) {
        return;
    }
    diagnostic->related_path = module->path;
    diagnostic->related_start = (unsigned)(position - begin);
    diagnostic->related_end = diagnostic->related_start + (unsigned)token.len;
    diagnostic->related_checksum = module->diagnostics.checksum;
    diagnostic->related_line = diagnostic->related_column = 1;
    for (unsigned offset = 0; offset < diagnostic->related_start; offset++) {
        unsigned char byte = (unsigned char)module->source[offset];
        if (byte == '\n') {
            diagnostic->related_line++;
            diagnostic->related_column = 1;
        } else if ((byte & 0xc0) != 0x80) {
            diagnostic->related_column++;
        }
    }
}

static void check_access(BorrowCheck *check, AtNode *source, int access)
{
    if (!source || source->kind != AN_NAME) {
        return;
    }
    int kind = check->project->types[source->type].kind;
    if (kind != AT_REGION && !at_slice_kind(kind)) {
        return;
    }
    for (Loan *loan = check->loans; loan; loan = loan->previous) {
        if (loan->source != source->symbol) {
            continue;
        }
        int overlaps = access == ACCESS_WRITE || access == ACCESS_MOVE ||
                       access == ACCESS_EXCLUSIVE || loan->writable ||
                       (access == ACCESS_READ && kind == AT_MUT_SLICE);
        if (overlaps) {
            conflict(check, source, loan);
            return;
        }
    }
}

static void expression(BorrowCheck *check, AtNode *node, int access);

static void expression_children(BorrowCheck *check, AtNode *node)
{
    expression(check, node->a, ACCESS_READ);
    expression(check, node->b, ACCESS_READ);
    expression(check, node->c, ACCESS_READ);
    for (int i = 0; i < node->count; i++) {
        expression(check, node->args[i], ACCESS_READ);
    }
}

static void call_arguments(BorrowCheck *check, AtNode *node)
{
    expression(check, node->a, ACCESS_READ);
    Loan *previous = check->loans;
    Loan arguments[AT_ARGS];
    int count = 0;
    for (int i = 0; i < node->count; i++) {
        AtNode *argument = node->args[i];
        int kind = check->project->types[argument->type].kind;
        int writable = kind == AT_MUT_SLICE;
        expression(check, argument, writable ? ACCESS_EXCLUSIVE : ACCESS_READ);
        if (argument->kind == AN_NAME && at_slice_kind(kind) && count < AT_ARGS) {
            /* Keep earlier argument loans until the call returns. Otherwise
             * f(view, view), or f(view, read(view)), could grant conflicting
             * access through distinct parameter names in a checked callee.
             * Passing a parameter onward follows the same rule recursively. */
            Loan *loan = &arguments[count++];
            *loan = (Loan){argument->symbol, writable, argument, check->loans};
            check->loans = loan;
        }
    }
    check->loans = previous;
}

static void expression(BorrowCheck *check, AtNode *node, int access)
{
    if (!node || node->kind == AN_FUNCTION || node->kind == AN_CLOSURE) {
        return;
    }
    if (node->kind == AN_NAME) {
        check_access(check, node, access);
        return;
    }
    if (node->kind == AN_INDEX) {
        expression(check, node->a, access);
        expression(check, node->b, ACCESS_READ);
        return;
    }
    if (node->kind == AN_CALL) {
        if (node->symbol == AT_CALL_REGION_MOVE) {
            expression(check, node->a->a, ACCESS_MOVE);
            return;
        }
        if (node->symbol == AT_CALL_REGION_BORROW) {
            expression(check, node->a->a, node->op ? ACCESS_EXCLUSIVE : ACCESS_SHARED);
            for (int i = 0; i < node->count; i++) {
                expression(check, node->args[i], ACCESS_READ);
            }
            return;
        }
        if (node->symbol == AT_CALL_LEN && node->count == 1 &&
            (node->args[0]->type == AT_REGION ||
             at_slice_kind(check->project->types[node->args[0]->type].kind))) {
            /* Length is immutable metadata. A suspended parent's extent is
             * still valid; inspecting it neither reads nor mutates payload. */
            return;
        }
        if (node->symbol >= 0 || node->symbol == AT_CALL_INDIRECT) {
            call_arguments(check, node);
            return;
        }
    }
    expression_children(check, node);
}

static void statements(BorrowCheck *check, AtNode *node)
{
    for (; node; node = node->next) {
        if (node->kind == AN_WITH) {
            expression(check, node->b, ACCESS_READ);
            if (node->b->symbol == AT_CALL_REGION_BORROW) {
                Loan loan = {node->b->a->a->symbol, node->b->op, node->b, check->loans};
                check->loans = &loan;
                statements(check, node->c);
                check->loans = loan.previous;
            } else {
                statements(check, node->c);
            }
        } else if (node->kind == AN_ASSIGN) {
            expression(check, node->b, ACCESS_READ);
            expression(check, node->a, ACCESS_WRITE);
        } else if (node->kind == AN_IF || node->kind == AN_WHILE) {
            expression(check, node->a, ACCESS_READ);
            statements(check, node->b);
            statements(check, node->c);
        } else if (node->kind == AN_FOR) {
            expression(check, node->b, ACCESS_READ);
            statements(check, node->c);
        } else if (node->kind == AN_TRY) {
            statements(check, node->a);
            for (AtNode *handler = node->b; handler; handler = handler->next) {
                statements(check, handler->b);
            }
            statements(check, node->c);
        } else if (node->kind == AN_UNSAFE) {
            statements(check, node->a);
        } else {
            expression(check, node, ACCESS_READ);
        }
    }
}

void at_check_region_borrows(AsTypedProject *project)
{
    for (int i = 0; i < project->nfunctions; i++) {
        if (project->functions[i].checked) {
            BorrowCheck check = {.project = project};
            statements(&check, project->functions[i].body);
        }
    }
}
