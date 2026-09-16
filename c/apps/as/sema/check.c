/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Statement flow, definite assignment and project-wide layout/lifetime checks.
 * Expression typing and source binding are separate translation units. */

enum FlowExit {
    FLOW_FALLTHROUGH = 1,
    FLOW_RETURN = 2,
    FLOW_BREAK = 4,
    FLOW_CONTINUE = 8,
    FLOW_THROW = 16
};

static int statements(Checker *checker, AtNode *node);

/* The final slot carries constructor field facts through the same branch,
 * loop and exception joins as local initialization. Fields are not fake locals
 * and never allocate runtime flag slots merely to support this analysis. */
#define AT_FLOW_SLOTS (AT_LOCALS + 1)

static void save_initialization(const AtFunction *function, int state[AT_FLOW_SLOTS])
{
    memset(state, 0, sizeof(int) * AT_FLOW_SLOTS);
    for (int i = 0; i < function->nlocals; i++) {
        state[i] = function->locals[i].initialized;
    }
    state[AT_LOCALS] = (int)function->initialized_fields;
}

static void restore_initialization(AtFunction *function, const int state[AT_FLOW_SLOTS])
{
    for (int i = 0; i < function->nlocals; i++) {
        function->locals[i].initialized =
            state[i] | (function->locals[i].capture_parent ? AT_LOCAL_INITIALIZED : 0);
    }
    function->initialized_fields = (uint32_t)state[AT_LOCALS];
}

/* Only branches that reach the following statement contribute to definite
 * assignment. For example, `if bad: return 1; else: x = 2` leaves x initialized.
 * Snapshot slots beyond the current local count are zero: a name first seen
 * in one branch must not look initialized in the other branch. */
static int check_conditional(Checker *checker, AtNode *node)
{
    at_check_mismatch(checker, node->a, AT_BOOL, at_check_expression(checker, node->a, AT_BOOL));
    AtFunction *function = checker->f;
    int before[AT_FLOW_SLOTS];
    int after_then[AT_FLOW_SLOTS];
    save_initialization(function, before);

    at_optional_assume(checker, node->a, 1);
    int then_flow = statements(checker, node->b);
    save_initialization(function, after_then);
    restore_initialization(function, before);
    at_optional_assume(checker, node->a, 0);
    int else_flow = statements(checker, node->c);

    for (int i = 0; i < function->nlocals; i++) {
        if (!(then_flow & FLOW_FALLTHROUGH)) {
            /* Only the else path can reach subsequent code. */
            continue;
        }
        if (!(else_flow & FLOW_FALLTHROUGH)) {
            function->locals[i].initialized = after_then[i];
        } else {
            function->locals[i].initialized = after_then[i] & function->locals[i].initialized;
        }
    }
    if (then_flow & FLOW_FALLTHROUGH) {
        if (!(else_flow & FLOW_FALLTHROUGH)) {
            function->initialized_fields = (uint32_t)after_then[AT_LOCALS];
        } else {
            function->initialized_fields &= (uint32_t)after_then[AT_LOCALS];
        }
    }
    return then_flow | else_flow;
}

static int check_loop_iterable(Checker *checker, AtNode *loop)
{
    AtNode *previous_use = checker->resource_use;
    checker->resource_use = loop->b;
    int iterable_type = at_check_expression(checker, loop->b, 0);
    checker->resource_use = previous_use;
    return iterable_type;
}

static void bind_loop_variable(Checker *checker, AtNode *loop, int iterable_type)
{
    int element_type = AT_I64;
    int is_range = iterable_type == AT_RANGE;

    if (!is_range) {
        AtType *iterable = &checker->p->types[iterable_type];
        if (iterable_type && (iterable->kind == AT_ARRAY || at_slice_kind(iterable->kind) ||
                              iterable->kind == AT_LIST)) {
            element_type = iterable->element;
        } else if (iterable->kind == AT_DICT) {
            element_type = iterable->key;
        } else if (iterable->kind == AT_STR || iterable->kind == AT_PORT) {
            element_type = AT_STR;
        } else if (at_check_byte_storage(checker->p, iterable_type, 0)) {
            element_type = AT_I64;
        } else {
            at_check_error(checker, loop->b, "AS3202",
                           "for requires Range, Array, Slice, List, Dict or str");
        }
    }

    AtNode *variable = loop->a;
    if (variable->kind == AN_GLOBAL) {
        AtGlobal *global = &checker->p->globals[variable->symbol];
        at_check_mismatch(checker, variable, global->type, element_type);
        variable->type = global->type;
        return;
    }
    int index = at_scope_resolve_local(checker, variable->token);
    if (index < 0) {
        index = at_scope_add_local(checker, variable, element_type);
    }
    if (index < 0) {
        return;
    }

    AtLocal *binding = &checker->f->locals[index];
    if (binding->scoped_borrow || at_resource_type(binding->type)) {
        at_check_error(checker, variable, "AS3401",
                       "A loop cannot rebind a scoped resource or view");
    }
    if (at_class_receiver_owner(checker->p, checker->f, index)) {
        at_check_error(checker, variable, "AS3210",
                       "A loop cannot rebind a method's self parameter");
    }
    at_check_mismatch(checker, variable, binding->type, element_type);
    if (!binding->type) {
        binding->type = element_type;
    }
    binding->initialized = 1;
    variable->type = element_type;
    variable->symbol = index;
}

static int check_loop(Checker *checker, AtNode *loop)
{
    /* A for iterable is evaluated once before entering the loop. Preserve its
     * incoming Optional proof for that evaluation, even if the body later
     * replaces the original local with None. The native iterator retains the
     * selected value. While conditions and loop bodies still need backedge
     * checks below; moving the forget past the body would be unsound. */
    int iterable_type = loop->kind == AN_FOR ? check_loop_iterable(checker, loop) : AT_ERROR;
    /* A later iteration may replace a previously nonempty Optional. Forget
     * incoming presence facts before checking the body, then learn only from
     * the condition that executes again on every iteration. */
    at_optional_forget(checker->f);
    int before[AT_FLOW_SLOTS];
    save_initialization(checker->f, before);
    if (loop->kind == AN_WHILE) {
        at_check_mismatch(checker, loop->a, AT_BOOL,
                          at_check_expression(checker, loop->a, AT_BOOL));
        at_optional_assume(checker, loop->a, 1);
    } else {
        bind_loop_variable(checker, loop, iterable_type);
    }

    checker->loops++;
    int body_flow = statements(checker, loop->kind == AN_FOR ? loop->c : loop->b);
    checker->loops--;

    /* A loop can execute zero times. Names/types learned from its body remain
     * in the function table, but assignments inside it cannot establish that
     * a variable is initialized after the loop. */
    restore_initialization(checker->f, before);
    return FLOW_FALLTHROUGH | (body_flow & FLOW_RETURN);
}

static int check_try(Checker *checker, AtNode *node)
{
    AtFunction *function = checker->f;
    /* A throw may follow a nullable assignment anywhere in the protected
     * region. Handler entry must not inherit a pre-try presence assumption. */
    at_optional_forget(function);
    int before[AT_FLOW_SLOTS], merged[AT_FLOW_SLOTS];
    save_initialization(function, before);
    int flow = statements(checker, node->a);
    if (flow & FLOW_FALLTHROUGH) {
        flow = (flow & ~FLOW_FALLTHROUGH) | statements(checker, node->c);
    }
    save_initialization(function, merged);
    unsigned seen = 0;
    for (AtNode *handler = node->b; handler; handler = handler->next) {
        unsigned mask = 1u << handler->op;
        if ((seen & 1u) || (seen & mask)) {
            at_check_error(checker, handler, "AS3700",
                           "Exception handler is hidden by an earlier handler");
        }
        seen |= mask;
        /* An operation may fail before any assignment in the protected body.
         * Catch entry therefore starts with the pre-try facts, not the state
         * at the end of the successful path. Types/names remain function-wide. */
        restore_initialization(function, before);
        if (handler->a) {
            AtNode *name = handler->a;
            int scoped = at_scope_resolve_local(checker, name->token);
            if (scoped >= 0 && (at_resource_type(function->locals[scoped].type) ||
                                function->locals[scoped].scoped_borrow)) {
                /* Catch bindings normally reuse function locals. A with owner
                 * is a scoped binding instead; do not overwrite it or create
                 * an invisible exception local behind that binding. */
                at_check_error(checker, name, "AS3401",
                               "An exception binding cannot replace a resource owner");
                continue;
            }
            int index = name->kind == AN_GLOBAL ? -1 : at_scope_local(function, name->token);
            if (name->kind == AN_GLOBAL) {
                name->type = checker->p->globals[name->symbol].type;
                at_check_mismatch(checker, name, name->type, checker->p->exception_type);
            } else {
                if (index < 0) {
                    index = at_scope_add_local(checker, name, checker->p->exception_type);
                }
                if (index >= 0) {
                    at_check_mismatch(checker, name, function->locals[index].type,
                                      checker->p->exception_type);
                    name->type = checker->p->exception_type;
                    name->symbol = index;
                    function->locals[index].initialized = 1;
                    function->locals[index].type = checker->p->exception_type;
                }
            }
        }
        checker->handlers++;
        int handler_flow = statements(checker, handler->b);
        checker->handlers--;
        if (handler_flow & FLOW_FALLTHROUGH) {
            for (int i = 0; i < function->nlocals; i++) {
                merged[i] =
                    function->locals[i].initialized & ((flow & FLOW_FALLTHROUGH) ? merged[i] : ~0);
            }
            merged[AT_LOCALS] = (int)function->initialized_fields &
                                ((flow & FLOW_FALLTHROUGH) ? merged[AT_LOCALS] : ~0);
        }
        flow |= handler_flow;
    }
    restore_initialization(function, merged);
    return flow;
}

static int statements(Checker *c, AtNode *n)
{
    int flow = FLOW_FALLTHROUGH;
    for (; n && (flow & FLOW_FALLTHROUGH); n = n->next) {
        int next_flow = FLOW_FALLTHROUGH;
        if (n->kind == AN_UNPACK) {
            at_check_unpack(c, n);
        } else if (n->kind == AN_ASSIGN) {
            AtNode *lhs = n->a;
            int ty = n->type;
            if (!lhs) {
                continue;
            }
            if (lhs->kind == AN_GLOBAL) {
                AtGlobal *global = &c->p->globals[lhs->symbol];
                if (n->op != T_ASSIGN) {
                    at_scope_read_global(c, lhs, lhs->symbol);
                }
                if (ty) {
                    at_check_mismatch(c, lhs, global->type, ty);
                } else {
                    ty = global->type;
                }
                if (n->b) {
                    int got = at_check_expression(c, n->b, ty);
                    if (got == AT_VOID) {
                        at_check_error(c, n->b, "AS3202", "Module variables require a value");
                    }
                    at_check_mismatch(c, n->b, ty, got);
                    if (!ty) {
                        ty = got;
                    }
                    if (c->f->module_initializer) {
                        global->initialized = 1;
                    }
                }
                if (!global->type) {
                    global->type = ty;
                }
                lhs->type = n->type = global->type;
            } else if (lhs->kind == AN_NAME) {
                int i = at_scope_resolve_local(c, lhs->token);
                if (i < 0) {
                    i = at_scope_add_local(c, lhs, ty);
                }
                if (i < 0) {
                    continue;
                }
                AtLocal *l = &c->f->locals[i];
                if (at_resource_type(l->type) || l->scoped_borrow ||
                    c->p->types[l->type].kind == AT_MUT_SLICE) {
                    at_check_error(c, lhs, "AS3401", "A with owner binding cannot be reassigned");
                }
                if (at_class_receiver_owner(c->p, c->f, i)) {
                    at_check_error(c, lhs, "AS3210", "A method cannot rebind its self parameter");
                }
                lhs->symbol = i;
                if (ty && l->type) {
                    at_check_mismatch(c, lhs, l->type, ty);
                }
                if (!ty) {
                    ty = l->type;
                }
                if (n->op != T_ASSIGN && !l->initialized) {
                    at_check_error(c, lhs, "AS3206",
                                   "Compound assignment reads an uninitialized variable");
                }
                if (n->b) {
                    int got = at_check_expression(c, n->b, ty);
                    if (got == AT_VOID) {
                        at_check_error(c, n->b, "AS3202",
                                       "A function with no result cannot initialize a variable");
                    }
                    if (ty) {
                        at_check_mismatch(c, n->b, ty, got);
                    } else {
                        ty = got;
                    }
                    l->initialized = 1;
                }
                if (!ty) {
                    at_check_error(c, lhs, "AS3200", "Cannot infer local type");
                }
                if (!l->type) {
                    l->type = ty;
                }
                lhs->type = l->type;
                n->type = l->type;
            } else if (lhs->kind == AN_FIELD || lhs->kind == AN_INDEX) {
                c->assignment_target = n->op == T_ASSIGN ? lhs : NULL;
                ty = at_check_expression(c, lhs, 0);
                c->assignment_target = NULL;
                if (lhs->kind == AN_GLOBAL) {
                    at_check_error(c, lhs, "AS3400",
                                   "Imported module bindings cannot be reassigned");
                }
                at_check_mismatch(c, n->b, ty, at_check_expression(c, n->b, ty));
                if (lhs->kind == AN_METHOD) {
                    at_check_error(c, lhs, "AS3202", "Methods cannot be replaced by assignment");
                }
                at_class_field_store(c, lhs);
                n->type = ty;
                AtNode *base = lhs;
                int reference_storage = 0;
                while (base && (base->kind == AN_FIELD || base->kind == AN_INDEX)) {
                    int owner_kind = c->p->types[base->a->type].kind;
                    if ((base->kind == AN_FIELD &&
                         (owner_kind == AT_CLASS || owner_kind == AT_LAYOUT)) ||
                        (base->kind == AN_INDEX &&
                         (owner_kind == AT_LIST || owner_kind == AT_DICT ||
                          owner_kind == AT_POINTER || owner_kind == AT_REGION ||
                          owner_kind == AT_MUT_SLICE ||
                          at_check_byte_storage(c->p, base->a->type, 0)))) {
                        reference_storage = 1;
                    }
                    if (base->kind == AN_FIELD && base->a->type == c->p->exception_type) {
                        at_check_error(c, lhs, "AS3702", "Exception records are immutable");
                    }
                    /* Read-only access is transitive through inline fields and
                     * nested arrays: slice[0].field is still borrowed storage. */
                    if (base->kind == AN_INDEX && c->p->types[base->a->type].kind == AT_SLICE) {
                        at_check_error(c, lhs, "AS3400",
                                       "Cannot mutate storage through a read-only Slice");
                    }
                    if (base->kind == AN_INDEX && base->a->type == AT_STR) {
                        at_check_error(c, lhs, "AS3400", "Strings are immutable");
                    }
                    if (base->kind == AN_INDEX && at_check_byte_storage(c->p, base->a->type, 0) &&
                        !at_check_byte_storage(c->p, base->a->type, 1)) {
                        at_check_error(c, lhs, "AS3400", "Byte storage is read-only");
                    }
                    if (base->kind == AN_INDEX && base->a->type == AT_RANGE) {
                        at_check_error(c, lhs, "AS3400", "Ranges are immutable");
                    }
                    base = base->a;
                }
                /* A returned reference still owns writable storage. Inline
                 * structs/arrays require a binding: changing their temporary
                 * copy would otherwise look successful and discard the write. */
                if (!reference_storage &&
                    (!base || (base->kind != AN_NAME && base->kind != AN_GLOBAL))) {
                    at_check_error(c, lhs, "AS3400",
                                   "Assignment target must be rooted in a variable");
                }
            } else {
                at_check_error(c, lhs, "AS3202", "Invalid assignment target");
            }
            if (n->op != T_ASSIGN && !at_check_number(c->p, n->type) &&
                !(n->op == T_PLUSEQ && n->type == AT_STR)) {
                at_check_error(c, n, "AS3202", "Compound assignment requires numeric values");
            }
        } else if (n->kind == AN_WITH) {
            int count = at_check_resource_binding(c, n);
            if (count) {
                AtScopedBinding bindings[2];
                AtScopedBinding *previous = c->bindings;
                for (int i = 0; i < count; i++) {
                    AtNode *owner = count == 2 ? n->a->args[i] : n->a;
                    bindings[i] = (AtScopedBinding){owner->token, owner->symbol, c->bindings};
                    c->bindings = &bindings[i];
                }
                next_flow = statements(c, n->c);
                c->bindings = previous;
                for (int i = 0; i < count; i++) {
                    c->f->locals[bindings[i].local].initialized = 0;
                }
            }
        } else if (n->kind == AN_TRY) {
            next_flow = check_try(c, n);
        } else if (n->kind == AN_UNSAFE) {
            c->unsafe_depth++;
            next_flow = statements(c, n->a);
            c->unsafe_depth--;
        } else if (n->kind == AN_RAISE) {
            if (n->a) {
                at_check_mismatch(c, n->a, c->p->exception_type,
                                  at_check_expression(c, n->a, c->p->exception_type));
            } else if (!c->handlers) {
                at_check_error(c, n, "AS3701", "Bare raise requires an enclosing except handler");
            }
            next_flow = FLOW_THROW;
        } else if (n->kind == AN_EXPR) {
            at_check_expression(c, n->a, 0);
            /* Preserve command-statement behavior: an outer composition or
             * redirect executes synchronously. A bare run(...) remains a
             * description; constructing it alone does not spawn a process. */
            n->op = n->a->kind == AN_BINARY && n->a->type == AT_COMMAND &&
                    (n->a->op == T_PIPEOP || n->a->op == T_ARROW || n->a->op == T_LARROW);
        } else if (n->kind == AN_ASSERT) {
            at_check_mismatch(c, n->a, AT_BOOL, at_check_expression(c, n->a, AT_BOOL));
            at_optional_assume(c, n->a, 1);
        } else if (n->kind == AN_RETURN) {
            at_class_require_initialized(c, n);
            int result = at_check_expression(c, n->a, c->f->result);
            if (c->f->result == AT_VOID && result == AT_NONE) {
                result = AT_VOID;
            }
            if (c->f->infer_result && !c->f->result) {
                c->f->result = result;
            }
            at_check_mismatch(c, n, c->f->result, result);
            next_flow = FLOW_RETURN;
        } else if (n->kind == AN_IF) {
            next_flow = check_conditional(c, n);
        } else if (n->kind == AN_WHILE || n->kind == AN_FOR) {
            next_flow = check_loop(c, n);
        } else if (n->kind == AN_BREAK || n->kind == AN_CONTINUE) {
            if (!c->loops) {
                at_check_error(c, n, "AS3207", "Loop control requires an enclosing loop");
            }
            next_flow = n->kind == AN_BREAK ? FLOW_BREAK : FLOW_CONTINUE;
        }
        flow = (flow & ~FLOW_FALLTHROUGH) | next_flow;
    }
    return flow;
}

static int contains_borrow(AsTypedProject *project, int type, int visited[AT_TYPES])
{
    if (visited[type]) {
        return 0;
    }
    visited[type] = 1;
    AtType *value = &project->types[type];
    if (at_slice_kind(value->kind)) {
        return 1;
    }
    if (value->kind == AT_ARRAY || value->kind == AT_LIST || value->kind == AT_OPTIONAL) {
        return contains_borrow(project, value->element, visited);
    }
    if (value->kind == AT_DICT) {
        return contains_borrow(project, value->key, visited) ||
               contains_borrow(project, value->element, visited);
    }
    if (value->kind == AT_STRUCT) {
        for (int i = 0; i < value->count; i++) {
            if (contains_borrow(project, value->fields[i], visited)) {
                return 1;
            }
        }
    }
    return 0;
}

static int layout(AsTypedProject *p, int id, int *visiting)
{
    /* Struct fields and fixed arrays are inline values. A back edge through
     * either would require infinite storage; slice references do not recurse
     * into their element's storage layout. */
    if (visiting[id] == 1) {
        return 0;
    }
    if (visiting[id] == 2) {
        return 1;
    }
    visiting[id] = 1;
    AtType *t = &p->types[id];
    if ((t->kind == AT_ARRAY || t->kind == AT_OPTIONAL) && !layout(p, t->element, visiting)) {
        return 0;
    }
    if (t->kind == AT_STRUCT) {
        for (int i = 0; i < t->count; i++) {
            if (!layout(p, t->fields[i], visiting)) {
                return 0;
            }
        }
    }
    visiting[id] = 2;
    return 1;
}

void at_check_function(AsTypedProject *project, AtFunction *function)
{
    if (function->checked) {
        return;
    }
    if (function->checking) {
        /* Mutually recursive bodies cannot use a guessed return type: that
         * choice would depend on which source file was visited first. */
        if (!function->result) {
            at_error(project, function->module, function->token, "AS3201",
                     "Recursive return inference is ambiguous; declare an explicit return type");
        }
        return;
    }
    for (int i = 0; i < function->nparams; i++) {
        if (!function->locals[i].type) {
            return;
        }
    }

    if (function->template_id >= 0 && project->specialization_depth >= 64) {
        at_error(project, function->module, function->token, "AS3600",
                 "Generic instantiation depth exceeds 64");
        return;
    }
    if (function->template_id >= 0) {
        project->specialization_depth++;
    }
    function->checking = 1;
    function->initialized_fields = 0;
    Checker checker = {.p = project, .f = function};
    function->active_checker = &checker;
    at_scope_collect_bindings(&checker);
    int flow = statements(&checker, function->body);
    if (at_class_initializing(&checker)) {
        if (function->result != AT_VOID) {
            at_error(project, function->module, function->token, "AS3210", "init must return None");
        }
        if ((flow & FLOW_FALLTHROUGH) && !at_class_initialized(&checker)) {
            at_error(project, function->module, function->token, "AS3210",
                     "Constructor exits without initializing every declared field");
        }
    }
    if (!function->result && function->infer_result) {
        function->result = AT_VOID;
    }
    int visited[AT_TYPES] = {0};
    if (contains_borrow(project, function->result, visited)) {
        at_error(project, function->module, function->token, "AS3400",
                 "Returning a value containing a borrowed Slice requires an owner lifetime; return "
                 "an owned value");
    }
    if ((flow & FLOW_FALLTHROUGH) && function->result != AT_VOID) {
        at_error(project, function->module, function->token, "AS3208",
                 "Not every control-flow path returns a value");
    }
    function->checking = 0;
    function->active_checker = NULL;
    function->checked = 1;
    if (function->template_id >= 0) {
        project->specialization_depth--;
    }
}

void at_check_functions(AsTypedProject *p)
{
    at_scope_check_imports(p);
    at_class_check_overrides(p);
    /* Module initialization follows import postorder. Types can be learned
     * from initializers, but checking them never performs their side effects. */
    for (int i = 0; i < p->ninitializers; i++) {
        at_check_function(p, &p->functions[p->initialization_order[i]]);
    }
    for (int i = 0; i < p->ntypes; i++) {
        AtType *type = &p->types[i];
        int invalid = (type->kind == AT_ARRAY || at_slice_kind(type->kind) ||
                       type->kind == AT_LIST || type->kind == AT_OPTIONAL) &&
                      type->element == AT_VOID;
        if (type->kind == AT_STRUCT || type->kind == AT_CLASS) {
            for (int field = 0; field < type->count; field++) {
                invalid |= type->fields[field] == AT_VOID;
                if (type->kind == AT_CLASS) {
                    int visited[AT_TYPES] = {0};
                    if (contains_borrow(p, type->fields[field], visited)) {
                        Token site = {.start = p->modules[type->module].source, .line = 1};
                        at_error(p, type->module, site, "AS3400",
                                 "A GC class field cannot retain a call-scoped Slice");
                    }
                    Token member = {.start = type->names[field],
                                    .len = (int)strlen(type->names[field])};
                    int method = at_class_method(p, i, member);
                    if (method >= 0) {
                        at_error(p, type->module, p->functions[method].token, "AS3200",
                                 "A class field and method cannot have the same name");
                    }
                }
            }
        }
        if (invalid) {
            Token token = {.start = p->modules[0].source, .line = 1};
            at_error(p, 0, token, "AS3202",
                     "None is a return type, not a field or container element type");
        }
        int visiting[AT_TYPES] = {0};
        if (!layout(p, i, visiting)) {
            Token t = {.start = p->modules[0].source, .line = 1};
            at_error(p, 0, t, "AS3200", "Recursive value layout has infinite size");
            return;
        }
    }
    for (int i = 0; i < p->nfunctions; i++) {
        AtFunction *f = &p->functions[i];
        if (f->template_id >= 0) {
            continue;
        }
        for (int j = 0; j < i; j++) {
            if (p->functions[j].module == f->module &&
                p->functions[j].method_owner == f->method_owner &&
                p->functions[j].lexical_parent == f->lexical_parent &&
                !strcmp(p->functions[j].name, f->name)) {
                at_error(p, f->module, f->token, "AS3200", "Function is already declared");
            }
        }
        for (int j = 0; j < f->nparams; j++) {
            if (f->locals[j].type == AT_VOID) {
                at_error(p, f->module, f->locals[j].token, "AS3202",
                         "A parameter requires a value type");
            }
            for (int k = 0; k < j; k++) {
                if (!strcmp(f->locals[j].name, f->locals[k].name)) {
                    at_error(p, f->module, f->locals[j].token, "AS3200",
                             "Duplicate parameter name");
                }
            }
        }
        if (!f->lexical_parent) {
            at_check_function(p, f);
        }
    }
    for (int i = 0; i < p->nfunctions; i++) {
        AtFunction *function = &p->functions[i];
        if (!function->checked) {
            at_error(p, function->module, function->token, "AS3201",
                     "Private parameter types could not be inferred from calls; add parameter "
                     "annotations");
        }
    }
    /* Inference/specialization may create containers after the initial layout
     * pass. Heap lists cannot carry a call-scoped stack view back to a caller
     * via append(), even if the mutating function itself returns None. */
    for (int i = 0; i < p->nnodes; i++) {
        AtNode *node = p->nodes[i];
        if (node->kind == AN_NAME && node->conversion == AT_OPTION_UNWRAP && node->function >= 0 &&
            node->symbol >= 0 && p->functions[node->function].locals[node->symbol].captured) {
            at_error(p, node->module, node->token, "AS3400",
                     "Copy a captured Optional into a local snapshot before narrowing it");
        }
        if (node->kind == AN_CALL && node->symbol == AT_CALL_ANY_BOX && node->count == 1) {
            int visited[AT_TYPES] = {0};
            if (contains_borrow(p, node->args[0]->type, visited)) {
                at_error(p, node->module, node->token, "AS3400",
                         "Any cannot retain a call-scoped Slice, including inside an aggregate");
            }
        }
    }
    for (int i = 0; i < p->nglobals; i++) {
        AtGlobal *global = &p->globals[i];
        int visited[AT_TYPES] = {0};
        if (contains_borrow(p, global->type, visited)) {
            at_error(p, global->module, global->token, "AS3400",
                     "Module storage cannot retain a call-scoped Slice");
        }
    }
    for (int i = 0; i < p->nfunctions; i++) {
        AtFunction *function = &p->functions[i];
        for (int slot = 0; slot < function->nlocals; slot++) {
            AtLocal *local = &function->locals[slot];
            int visited[AT_TYPES] = {0};
            if (local->captured && contains_borrow(p, local->type, visited)) {
                at_error(p, function->module, local->token, "AS3400",
                         "A closure cannot retain a call-scoped Slice");
            }
        }
    }
    for (int i = 0; i < p->ntypes; i++) {
        AtType *type = &p->types[i];
        if (type->kind != AT_LIST && type->kind != AT_DICT) {
            continue;
        }
        Token site = {.start = p->modules[0].source, .line = 1};
        if (type->kind == AT_DICT &&
            !at_satisfies_constraint(p, type->key, AT_CONSTRAINT_HASHABLE)) {
            at_error(p, 0, site, "AS3601",
                     "Dictionary keys require Hashable: integers, bool or str");
        }
        int element = p->types[i].element;
        if (element == AT_VOID) {
            at_error(p, 0, site, "AS3202", "Container elements must produce values");
        }
        int visited[AT_TYPES] = {0};
        if (contains_borrow(p, element, visited)) {
            at_error(p, 0, site, "AS3400", "A heap container cannot retain a call-scoped Slice");
        }
    }
    at_check_resource_types(p);
    if (!as_typed_errors(p)) {
        at_check_region_flow(p);
        at_check_region_borrows(p);
    }
}
