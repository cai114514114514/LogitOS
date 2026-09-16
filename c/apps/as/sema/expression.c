/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int at_integer(AsTypedProject *p, int t)
{
    return t >= 0 && t < p->ntypes && p->types[t].kind >= AT_I8 && p->types[t].kind <= AT_U64;
}

int at_signed(AsTypedProject *p, int t)
{
    return at_integer(p, t) && p->types[t].kind <= AT_I64;
}

int at_bits(AsTypedProject *p, int t)
{
    int k = p->types[t].kind;
    return k == AT_BOOL       ? 1
           : k == AT_F32      ? 32
           : k == AT_F64      ? 64
           : at_integer(p, t) ? 8 << ((k - AT_I8) % 4)
                              : 0;
}

int at_check_number(AsTypedProject *p, int t)
{
    return at_satisfies_constraint(p, t, AT_CONSTRAINT_NUMBER);
}

static int integer_operand(AsTypedProject *project, int type)
{
    return at_satisfies_constraint(project, type, AT_CONSTRAINT_INTEGER);
}

/* Literals are parsed against the expected machine width, including u64 and
 * the negative i64 minimum. strtod/strtoll would silently round or saturate. */
static int literal(Checker *c, AtNode *n, int want, int negative)
{
    int ty = at_integer(c->p, want) ? want : AT_I64;
    Token t = n->token;
    unsigned bits = (unsigned)at_bits(c->p, ty);
    uint64_t limit = at_signed(c->p, ty) ? (UINT64_MAX >> (65 - bits)) + (unsigned)negative
                     : bits == 64        ? UINT64_MAX
                                         : ((1ull << bits) - 1);
    if (negative && !at_signed(c->p, ty)) {
        at_check_error(c, n, "AS3203", "Negative literal does not fit an unsigned type");
        return ty;
    }
    int radix = 10, i = 0;
    if (t.len > 2 && t.start[0] == '0' && (t.start[1] == 'x' || t.start[1] == 'X')) {
        radix = 16;
        i = 2;
    }
    uint64_t value = 0;
    int bad = i == t.len;
    for (; i < t.len; i++) {
        unsigned ch = (unsigned char)t.start[i], d = ch >= '0' && ch <= '9'   ? ch - '0'
                                                     : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                                     : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                                                              : 255;
        if (d >= (unsigned)radix || value > limit / (unsigned)radix ||
            (value == limit / (unsigned)radix && d > limit % (unsigned)radix)) {
            bad = 1;
            break;
        }
        value = value * (unsigned)radix + d;
    }
    if (bad) {
        at_check_error(c, n, "AS3203", "Integer literal is outside the declared machine type");
    }
    n->integer = value;
    return ty;
}

static int generic_call(Checker *checker, AtNode *call, int template_id, int want)
{
    AsTypedProject *project = checker->p;
    int errors_before = as_typed_errors(project);
    AtFunction *function = &project->functions[template_id];
    at_check_function(project, function);
    int bindings[AT_TYPE_PARAMETERS] = {0};
    int valid = call->count == function->nparams;
    if (!valid) {
        at_check_error(checker, call, "AS3204",
                       "Argument count differs from the generic function declaration");
    }
    int deferred[AT_ARGS] = {0};
    for (int i = 0; i < call->count; i++) {
        AtNode *argument = call->args[i];
        int declaration =
            argument->kind == AN_FUNCTION ? argument->op - 1 : at_scope_function(checker, argument);
        int local =
            argument->kind == AN_NAME && at_scope_resolve_local(checker, argument->token) >= 0;
        deferred[i] = argument->kind == AN_CLOSURE ||
                      (!local && declaration >= 0 && project->functions[declaration].generic_count);
        if (i < function->nparams && project->types[function->locals[i].type].kind == AT_OPTIONAL) {
            deferred[i] = 2;
        }
    }
    /* Concrete arguments constrain callbacks even when the callback is first
     * in source order, as in map(identity, numbers). Checking order changes;
     * runtime argument evaluation remains left to right. */
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < call->count; i++) {
            if (deferred[i] != pass) {
                continue;
            }
            int pattern = i < function->nparams ? function->locals[i].type : AT_ERROR;
            /* Concrete parts of a signature still supply literal context. A T or
             * Array[T,N] cannot invent an element type for an empty/untyped value. */
            int contextual[AT_TYPE_PARAMETERS];
            for (int j = 0; j < function->generic_count; j++) {
                contextual[j] = bindings[j] ? bindings[j] : function->type_parameters[j];
            }
            int context = at_substitute_type(project, function, pattern, contextual);
            if (project->types[context].kind != AT_CALLABLE &&
                !at_slice_kind(project->types[context].kind) &&
                project->types[context].kind != AT_OPTIONAL && at_has_parameter(project, context)) {
                context = AT_ERROR;
            }
            int actual = at_check_borrow_argument(checker, call->args[i], context);
            if (actual == AT_VOID) {
                at_check_error(checker, call->args[i], "AS3202",
                               "A call argument must produce a value");
                valid = 0;
            }
            if (i < function->nparams) {
                /* None has no payload from which to infer T. Other arguments
                 * or the declared result must determine Optional[T] first. */
                int empty_optional = project->types[pattern].kind == AT_OPTIONAL &&
                                     call->args[i]->value_type == AT_NONE;
                if (!empty_optional) {
                    valid &=
                        at_bind_type(project, function, pattern, actual, bindings, call->args[i]);
                }
            }
        }
    }
    /* An unrelated diagnostic (including the unfinished member after this
     * call) must not erase an otherwise resolved result type. Fail on this
     * call's errors; the build driver still rejects every erroneous project. */
    if (!valid || project->oom || as_typed_errors(project) != errors_before) {
        return AT_ERROR;
    }
    if (want) {
        int contextual[AT_TYPE_PARAMETERS];
        memcpy(contextual, bindings, sizeof contextual);
        if (at_bind_result_context(project, function, function->result, want, contextual)) {
            memcpy(bindings, contextual, sizeof bindings);
        }
    }
    int result = AT_ERROR;
    int instance = at_specialize(project, template_id, bindings, call, &result);
    if (instance < 0) {
        return AT_ERROR;
    }
    call->symbol = instance;
    for (int i = 0; i < call->count; i++) {
        if (project->types[function->locals[i].type].kind == AT_OPTIONAL) {
            int expected =
                at_substitute_type(project, function, function->locals[i].type, bindings);
            at_check_mismatch(checker, call->args[i], expected,
                              at_check_borrow_argument(checker, call->args[i], expected));
        }
    }
    if (!project->functions[instance].generic_count) {
        at_check_function(project, &project->functions[instance]);
    }
    return result;
}

static int conversion_builtin(Checker *checker, AtNode *node, Token name, int *result)
{
    static const struct {
        const char *name;
        int symbol;
        int input;
        int output;
    } signatures[] = {
        {"str", AT_CALL_STR, AT_ERROR, AT_STR},
        {"parse_int", AT_CALL_PARSE_INT, AT_STR, AT_I64},
        {"parse_float", AT_CALL_PARSE_FLOAT, AT_STR, AT_F64},
        {"chr", AT_CALL_CHR, AT_I64, AT_STR},
        {"ord", AT_CALL_ORD, AT_STR, AT_I64},
        {"f64bits", AT_CALL_F64_BITS, AT_ERROR, AT_I64},
    };

    for (unsigned i = 0; i < sizeof signatures / sizeof *signatures; i++) {
        if (!at_scope_name_equal(name, signatures[i].name)) {
            continue;
        }
        node->symbol = signatures[i].symbol;
        *result = signatures[i].output;
        if (node->count != 1) {
            at_check_error(checker, node, "AS3204", "Conversion expects one argument");
        }
        for (int j = 0; j < node->count; j++) {
            int actual = node->symbol == AT_CALL_STR
                             ? at_check_view_read(checker, node->args[j])
                             : at_check_expression(checker, node->args[j], signatures[i].input);
            if (signatures[i].input) {
                at_check_mismatch(checker, node->args[j], signatures[i].input, actual);
            } else if (actual && !at_check_number(checker->p, actual)) {
                if (node->symbol != AT_CALL_STR || actual == AT_VOID) {
                    at_check_error(checker, node->args[j], "AS3202",
                                   "This conversion requires a number; str requires a value");
                }
            }
        }
        return 1;
    }
    return 0;
}

static int call(Checker *c, AtNode *n, int want)
{
    AsTypedProject *p = c->p;
    AtNode *callee = n->a;
    if (!callee) {
        return 0;
    }
    int capability_result;
    if (at_check_region_call(c, n, &capability_result)) {
        return capability_result;
    }
    if (at_check_command_call(c, n, &capability_result)) {
        return capability_result;
    }
    if (at_check_port_call(c, n, &capability_result)) {
        return capability_result;
    }
    if (at_check_capability_call(c, n, &capability_result)) {
        return capability_result;
    }
    if (at_check_bytes_call(c, n, &capability_result)) {
        return capability_result;
    }
    if ((callee->kind == AN_FIELD || callee->kind == AN_METHOD) && callee->a->kind == AN_SUPER) {
        callee->op = 1; /* A direct call, rather than an escaping init callback. */
        return at_check_indirect_call(c, n);
    }
    if (callee->kind == AN_FIELD &&
        (at_scope_name_equal(callee->token, "get") || at_scope_name_equal(callee->token, "has") ||
         at_scope_name_equal(callee->token, "keys") ||
         at_scope_name_equal(callee->token, "values") ||
         at_scope_name_equal(callee->token, "remove")) &&
        (callee->a->kind != AN_NAME || at_scope_resolve_local(c, callee->a->token) >= 0 ||
         at_global_lookup(p, c->f->module, callee->a->token, 1) >= 0)) {
        int receiver = at_check_expression(c, callee->a, 0);
        AtType *dictionary = &p->types[receiver];
        if (dictionary->kind != AT_DICT) {
            if (dictionary->kind == AT_STRUCT || dictionary->kind == AT_CLASS) {
                return at_check_indirect_call(c, n);
            }
            at_check_error(c, callee, "AS3202", "This method requires a Dict receiver");
            return AT_ERROR;
        }
        int count = 1, result = AT_BOOL;
        n->symbol = AT_CALL_DICT_HAS;
        if (at_scope_name_equal(callee->token, "get")) {
            n->symbol = AT_CALL_DICT_GET;
            count = n->count == 1 ? 1 : 2;
            result = count == 1 ? at_compound_type(p, AT_OPTIONAL, dictionary->element, 0,
                                                   n->module, n->token)
                                : dictionary->element;
        } else if (at_scope_name_equal(callee->token, "remove")) {
            n->symbol = AT_CALL_DICT_REMOVE;
        } else if (at_scope_name_equal(callee->token, "keys") ||
                   at_scope_name_equal(callee->token, "values")) {
            int keys = at_scope_name_equal(callee->token, "keys");
            n->symbol = keys ? AT_CALL_DICT_KEYS : AT_CALL_DICT_VALUES;
            count = 0;
            result = at_compound_type(p, AT_LIST, keys ? dictionary->key : dictionary->element, 0,
                                      n->module, n->token);
        }
        if (n->count != count) {
            at_check_error(c, n, "AS3204",
                           "Dictionary method argument count differs from its declaration");
        }
        for (int i = 0; i < n->count; i++) {
            int expected = i == 0 ? dictionary->key : dictionary->element;
            at_check_mismatch(c, n->args[i], expected,
                              at_check_expression(c, n->args[i], expected));
        }
        return result;
    }
    if (callee->kind == AN_TYPE_APPLICATION) {
        n->symbol = callee->op ? AT_CALL_ANY_TEST : AT_CALL_ANY_CAST;
        if (n->count != 1) {
            at_check_error(c, n, "AS3204", "cast[T] and is_type[T] require one Any value");
        }
        if (callee->type == AT_VOID) {
            at_check_error(c, callee, "AS3202", "A checked cast requires a value type");
        }
        for (int i = 0; i < n->count; i++) {
            at_check_mismatch(c, n->args[i], AT_ANY, at_check_expression(c, n->args[i], AT_ANY));
        }
        return callee->op ? AT_BOOL : callee->type;
    }
    if (callee->kind == AN_FIELD &&
        (at_scope_name_equal(callee->token, "find") ||
         at_scope_name_equal(callee->token, "slice") || at_scope_name_equal(callee->token, "sub") ||
         at_scope_name_equal(callee->token, "join") ||
         at_scope_name_equal(callee->token, "split") ||
         at_scope_name_equal(callee->token, "strip") ||
         at_scope_name_equal(callee->token, "upper") ||
         at_scope_name_equal(callee->token, "lower") ||
         at_scope_name_equal(callee->token, "replace") ||
         at_scope_name_equal(callee->token, "append")) &&
        (callee->a->kind != AN_NAME || at_scope_resolve_local(c, callee->a->token) >= 0 ||
         at_global_lookup(p, c->f->module, callee->a->token, 1) >= 0)) {
        int receiver = at_check_expression(c, callee->a, 0);
        if (p->types[receiver].kind == AT_STRUCT || p->types[receiver].kind == AT_CLASS) {
            return at_check_indirect_call(c, n);
        }
        int expected = AT_STR;
        int arguments = 1;
        int minimum = 1;
        int result = AT_I64;
        n->symbol = AT_CALL_STR_FIND;
        if (at_scope_name_equal(callee->token, "append")) {
            n->symbol = AT_CALL_LIST_APPEND;
            result = AT_VOID;
            if (p->types[receiver].kind != AT_LIST) {
                at_check_error(c, callee, "AS3202", "append requires a List receiver");
                expected = AT_ERROR;
            } else {
                expected = p->types[receiver].element;
            }
        } else {
            at_check_mismatch(c, callee->a, AT_STR, receiver);
            if (at_scope_name_equal(callee->token, "slice") ||
                at_scope_name_equal(callee->token, "sub")) {
                n->symbol = at_scope_name_equal(callee->token, "slice") ? AT_CALL_STR_SLICE
                                                                        : AT_CALL_STR_SUB;
                expected = AT_I64;
                arguments = 2;
                minimum = 2;
                result = AT_STR;
            } else if (at_scope_name_equal(callee->token, "join")) {
                n->symbol = AT_CALL_STR_JOIN;
                expected = at_compound_type(p, AT_LIST, AT_STR, 0, n->module, n->token);
                result = AT_STR;
            } else if (at_scope_name_equal(callee->token, "split")) {
                n->symbol = AT_CALL_STR_SPLIT;
                minimum = 0;
                result = at_compound_type(p, AT_LIST, AT_STR, 0, n->module, n->token);
            } else if (at_scope_name_equal(callee->token, "replace")) {
                n->symbol = AT_CALL_STR_REPLACE;
                minimum = arguments = 2;
                result = AT_STR;
            } else if (at_scope_name_equal(callee->token, "strip") ||
                       at_scope_name_equal(callee->token, "upper") ||
                       at_scope_name_equal(callee->token, "lower")) {
                n->symbol = at_scope_name_equal(callee->token, "strip") ? AT_CALL_STR_STRIP
                                                                        : AT_CALL_STR_CASE;
                n->op = at_scope_name_equal(callee->token, "upper");
                minimum = arguments = 0;
                result = AT_STR;
            }
        }
        if (n->count < minimum || n->count > arguments) {
            at_check_error(c, n, "AS3204", "Method argument count differs from its declaration");
        }
        for (int i = 0; i < n->count; i++) {
            at_check_mismatch(c, n->args[i], expected,
                              at_check_expression(c, n->args[i], expected));
        }
        return result;
    }
    int declaration = at_scope_function(c, callee);
    if (callee->kind == AN_NAME && (at_scope_resolve_local(c, callee->token) >= 0 ||
                                    at_global_lookup(p, c->f->module, callee->token, 1) >= 0)) {
        return at_check_indirect_call(c, n);
    }
    int constructor = declaration < 0 ? at_scope_constructor(c, callee) : AT_ERROR;
    if (p->types[constructor].kind == AT_LAYOUT) {
        n->symbol = AT_CALL_LAYOUT_NEW;
        if (n->count) {
            at_check_error(c, n, "AS3204", "A layout constructor takes no arguments");
        }
        for (int i = 0; i < n->count; i++) {
            at_check_expression(c, n->args[i], 0);
        }
        return constructor;
    }
    if (p->types[constructor].kind == AT_CLASS) {
        return at_class_constructor(c, n, constructor);
    }
    if (p->types[constructor].kind == AT_STRUCT && constructor != p->exception_type) {
        n->symbol = AT_CALL_STRUCT_BASE - constructor;
        AtType *structure = &p->types[constructor];
        if (n->count != structure->count) {
            at_check_error(c, n, "AS3204",
                           "Structure constructor must initialize every declared field");
        }
        for (int i = 0; i < n->count; i++) {
            int expected = i < structure->count ? structure->fields[i] : AT_ERROR;
            at_check_mismatch(c, n->args[i], expected,
                              at_check_expression(c, n->args[i], expected));
        }
        return constructor;
    }
    /* User declarations shadow predeclared builtins. Resolving builtins first
     * made a local `def str` visible in reports but impossible to call. */
    if (callee->kind == AN_NAME && declaration < 0) {
        Token t = callee->token;
        if (at_scope_name_equal(t, "layout")) {
            at_check_error(c, n, "AS3811",
                           "layout is a module type declaration with literal metadata; "
                           "runtime layout factories are not supported");
            return AT_ERROR;
        }
        int converted;
        if (conversion_builtin(c, n, t, &converted)) {
            return converted;
        }
        if (at_scope_name_equal(t, "Any")) {
            n->symbol = AT_CALL_ANY_BOX;
            if (n->count != 1) {
                at_check_error(c, n, "AS3204", "Any construction requires one value");
            }
            for (int i = 0; i < n->count; i++) {
                if (at_check_expression(c, n->args[i], 0) == AT_VOID) {
                    at_check_error(c, n->args[i], "AS3202",
                                   "Any cannot box a function with no result");
                }
            }
            return AT_ANY;
        }
        if (at_check_runtime_builtin(c, n, t, &converted)) {
            return converted;
        }
        int exception = at_exception_code(t);
        if (exception >= 0) {
            n->symbol = AT_CALL_EXCEPTION;
            n->op = exception ? exception : AT_E_RUNTIME;
            if (n->count != 1) {
                at_check_error(c, n, "AS3204",
                               "Exception construction requires one string message");
            }
            for (int i = 0; i < n->count; i++) {
                at_check_mismatch(c, n->args[i], AT_STR,
                                  at_check_expression(c, n->args[i], AT_STR));
            }
            return p->exception_type;
        }
        if (at_scope_name_equal(t, "print")) {
            n->symbol = AT_CALL_PRINT;
            for (int i = 0; i < n->count; i++) {
                int ty = at_check_view_read(c, n->args[i]);
                if (ty == AT_VOID) {
                    at_check_error(c, n->args[i], "AS3202",
                                   "print requires a value for each argument");
                }
            }
            return AT_VOID;
        }
        if (at_scope_name_equal(t, "len")) {
            n->symbol = AT_CALL_LEN;
            if (n->count != 1) {
                at_check_error(c, n, "AS3204", "len expects one argument");
                return 0;
            }
            int ty = at_check_resource_receiver(c, n->args[0]);
            if (ty && p->types[ty].kind != AT_ARRAY && !at_slice_kind(p->types[ty].kind) &&
                p->types[ty].kind != AT_LIST && p->types[ty].kind != AT_DICT && ty != AT_STR &&
                ty != AT_RANGE && ty != AT_REGION && !at_check_byte_storage(p, ty, 0)) {
                at_check_error(c, n, "AS3202",
                               "len requires an array, slice, list, dictionary, range, buffer, "
                               "Bytes or string");
            }
            return AT_I64;
        }
        if (at_scope_name_equal(t, "range")) {
            n->symbol = AT_CALL_RANGE;
            if (n->count < 1 || n->count > 3) {
                at_check_error(c, n, "AS3204",
                               "range expects stop, start/stop, or start/stop/step");
            }
            for (int i = 0; i < n->count; i++) {
                at_check_mismatch(c, n->args[i], AT_I64,
                                  at_check_expression(c, n->args[i], AT_I64));
            }
            return AT_RANGE;
        }
        int cast_type = at_function_type(p, c->f, t);
        if (!cast_type) {
            for (int i = AT_I8; i <= AT_F64; i++) {
                if (at_scope_name_equal(t, p->types[i].name)) {
                    cast_type = i;
                    break;
                }
            }
        }
        if (cast_type) {
            n->symbol = AT_CALL_CAST_BASE - cast_type;
            if (!at_check_number(p, cast_type)) {
                at_check_error(
                    c, n, "AS3601",
                    "Generic numeric construction requires a Number or Integer constraint");
            }
            if (n->count != 1) {
                at_check_error(c, n, "AS3204", "Numeric conversion expects one argument");
                return cast_type;
            }
            int source = at_check_expression(c, n->args[0], 0);
            if (source && !at_check_number(p, source)) {
                at_check_error(c, n, "AS3202", "Numeric conversion requires a number");
            }
            return cast_type;
        }
        if (at_scope_name_equal(t, "wrapping_add") || at_scope_name_equal(t, "wrapping_sub") ||
            at_scope_name_equal(t, "wrapping_mul") || at_scope_name_equal(t, "wrapping_shl")) {
            if (at_scope_name_equal(t, "wrapping_add")) {
                n->symbol = AT_CALL_WRAPPING_ADD;
            } else if (at_scope_name_equal(t, "wrapping_sub")) {
                n->symbol = AT_CALL_WRAPPING_SUB;
            } else if (at_scope_name_equal(t, "wrapping_shl")) {
                n->symbol = AT_CALL_WRAPPING_SHL;
            } else {
                n->symbol = AT_CALL_WRAPPING_MUL;
            }
            if (n->count != 2) {
                at_check_error(c, n, "AS3204", "Wrapping operation expects two arguments");
                return 0;
            }
            int a = at_check_expression(c, n->args[0], want),
                b = at_check_expression(c, n->args[1], a);
            at_check_mismatch(c, n, a, b);
            if (a && !integer_operand(p, a)) {
                at_check_error(c, n, "AS3202", "Wrapping operation requires integers");
            }
            return a;
        }
    }
    int id = declaration;
    if (id < 0) {
        if (callee->kind != AN_NAME) {
            return at_check_indirect_call(c, n);
        }
        at_check_error(c, callee, "AS3205", "Unknown function or unsupported callable value");
        for (int i = 0; i < n->count; i++) {
            at_check_expression(c, n->args[i], 0);
        }
        return 0;
    }
    n->symbol = id;
    AtFunction *f = &p->functions[id];
    if (f->generic_count) {
        return generic_call(c, n, id, want);
    }
    if (n->count != f->nparams) {
        at_check_error(c, n, "AS3204", "Argument count differs from the function declaration");
    }
    for (int i = 0; i < n->count; i++) {
        int expected = i < f->nparams ? f->locals[i].type : 0,
            got = at_check_borrow_argument(c, n->args[i], expected);
        if (!expected && i < f->nparams && got != AT_ERROR) {
            if (at_has_parameter(p, got)) {
                at_check_error(
                    c, n, "AS3601",
                    "A generic body requires an explicit signature for a nongeneric callee");
                continue;
            }
            /* A private function has one static signature, not an implicit
             * overload per call. Later callers must agree with this binding.
             * Generic specialization uses a separate, explicit language form. */
            f->locals[i].type = got;
            expected = got;
        }
        /* Only a call-scoped borrow is currently formed from a fixed array. The
         * callee cannot return/store slices, so a stack array cannot escape. */
        if (expected && got && p->types[expected].kind == AT_SLICE &&
            p->types[got].kind == AT_ARRAY && p->types[expected].element == p->types[got].element) {
            continue;
        }
        at_check_mismatch(c, n->args[i], expected, got);
    }
    at_check_function(p, f);
    return f->result;
}

static int expression_type(Checker *c, AtNode *n, int want)
{
    if (!n) {
        return AT_VOID;
    }
    AsTypedProject *p = c->p;
    int ty = 0;
    switch (n->kind) {
    case AN_ERROR:
        return AT_ERROR;
    case AN_NONE:
        ty = AT_NONE;
        break;
    case AN_INT:
        ty = literal(c, n, want, 0);
        break;
    case AN_CONSTANT:
        ty = AT_I64;
        break;
    case AN_FLOAT:
        ty = want == AT_F32 ? AT_F32 : AT_F64;
        break;
    case AN_BOOL:
        ty = AT_BOOL;
        break;
    case AN_STR:
        ty = AT_STR;
        break;
    case AN_FUNCTION:
        ty = at_check_function_value(c, n, n->op - 1, want ? want : n->type);
        break;
    case AN_CLOSURE:
        ty = at_check_closure(c, n, want);
        break;
    case AN_METHOD:
        c->field_receiver++;
        at_check_expression(c, n->a, 0);
        c->field_receiver--;
        ty = at_class_method_value(c, n, n->symbol);
        break;
    case AN_SUPER:
        ty = at_class_super(c, n);
        break;
    case AN_NAME: {
        int i = at_scope_resolve_local(c, n->token);
        n->symbol = i;
        if (i < 0) {
            int global = at_global_lookup(p, c->f->module, n->token, 1);
            if (global >= 0) {
                ty = at_scope_read_global(c, n, global);
            } else {
                int function = at_scope_function(c, n);
                if (function >= 0) {
                    ty = at_check_function_value(c, n, function, want);
                } else if (at_check_system_constant(n)) {
                    ty = AT_I64;
                } else {
                    at_check_error(c, n, "AS3205", "Unknown variable");
                }
            }
        } else {
            ty = c->f->locals[i].type;
            if (!i && !c->field_receiver) {
                at_class_require_initialized(c, n);
            }
            if (!c->f->locals[i].initialized) {
                at_check_error(c, n, "AS3206", "Variable may be read before initialization");
            }
        }
        break;
    }
    case AN_GLOBAL:
        ty = at_scope_read_global(c, n, n->symbol);
        break;
    case AN_DICT: {
        if (!want && n->count) {
            AtNode *pair = n->args[0];
            int key = at_check_expression(c, pair->a, 0);
            int value = at_check_expression(c, pair->b, 0);
            want = at_dict_type(p, key, value, n->module, n->token);
        }
        if (!want || p->types[want].kind != AT_DICT) {
            at_check_error(c, n, "AS3900", "Empty dictionary requires Dict[K, V] context");
            for (int i = 0; i < n->count; i++) {
                at_check_expression(c, n->args[i]->a, 0);
                at_check_expression(c, n->args[i]->b, 0);
            }
            break;
        }
        AtType *dictionary = &p->types[want];
        for (int i = 0; i < n->count; i++) {
            AtNode *pair = n->args[i];
            at_check_mismatch(c, pair->a, dictionary->key,
                              at_check_expression(c, pair->a, dictionary->key));
            at_check_mismatch(c, pair->b, dictionary->element,
                              at_check_expression(c, pair->b, dictionary->element));
        }
        ty = want;
        break;
    }
    case AN_COMPREHENSION:
        ty = at_check_comprehension(c, n, want);
        break;
    case AN_ARRAY: {
        if (!want && n->count) {
            int element = at_check_expression(c, n->args[0], 0);
            want = at_compound_type(p, AT_LIST, element, 0, n->module, n->token);
        }
        if (!want || (p->types[want].kind != AT_ARRAY && p->types[want].kind != AT_LIST)) {
            at_check_error(c, n, "AS3900",
                           "Empty container requires List[T] or Array[T, N] context");
            for (int i = 0; i < n->count; i++) {
                at_check_expression(c, n->args[i], 0);
            }
            break;
        }
        AtType *t = &p->types[want];
        ty = want;
        if (t->kind == AT_ARRAY && n->count != t->count) {
            at_check_error(c, n, "AS3202", "Array literal length differs from its declared size");
        }
        for (int i = 0; i < n->count; i++) {
            at_check_mismatch(c, n->args[i], t->element,
                              at_check_expression(c, n->args[i], t->element));
        }
        break;
    }
    case AN_FORMAT:
        ty = at_check_view_read(c, n->a);
        if (ty == AT_VOID) {
            at_check_error(c, n->a, "AS3202", "Formatted string expression requires a value");
        }
        ty = ty ? AT_STR : AT_ERROR;
        break;
    case AN_CALL:
        ty = call(c, n, want);
        break;
    case AN_INDEX: {
        int a = at_check_resource_receiver(c, n->a);
        int key = p->types[a].kind == AT_DICT ? p->types[a].key : AT_I64;
        at_check_mismatch(c, n->b, key, at_check_expression(c, n->b, key));
        if (a) {
            int k = p->types[a].kind;
            if (k == AT_STR) {
                ty = AT_STR;
            } else if (k == AT_POINTER) {
                if (!c->unsafe_depth) {
                    at_check_error(c, n, "AS3810", "Pointer dereference requires an unsafe block");
                }
                ty = p->types[a].element;
            } else if (k == AT_RANGE || k == AT_REGION || at_check_byte_storage(p, a, 0)) {
                ty = AT_I64;
            } else if (k != AT_ARRAY && !at_slice_kind(k) && k != AT_LIST && k != AT_DICT) {
                at_check_error(c, n, "AS3202",
                               "Indexing requires an array, slice, list, dictionary or string");
            } else {
                ty = p->types[a].element;
            }
        }
        break;
    }
    case AN_FIELD: {
        int global = at_scope_qualified_global(c, n);
        if (global >= 0) {
            ty = at_scope_read_global(c, n, global);
            break;
        }
        int function = at_scope_function(c, n);
        if (function >= 0) {
            ty = at_check_function_value(c, n, function, want);
            break;
        }
        c->field_receiver++;
        int a = at_check_expression(c, n->a, 0);
        c->field_receiver--;
        if (a && (p->types[a].kind == AT_STRUCT || p->types[a].kind == AT_CLASS ||
                  p->types[a].kind == AT_LAYOUT)) {
            AtType *s = &p->types[a];
            int i;
            for (i = 0; i < s->count; i++) {
                if (at_scope_name_equal(n->token, s->names[i])) {
                    break;
                }
            }
            if (i == s->count) {
                int method = s->kind == AT_CLASS ? at_class_method(p, a, n->token) : -1;
                if (method >= 0) {
                    ty = at_class_method_value(c, n, method);
                } else {
                    at_check_error(c, n, "AS3205", "Object has no field or method with this name");
                }
            } else {
                n->field = i;
                if (n->a->kind == AN_SUPER) {
                    at_check_error(c, n, "AS3211",
                                   "super selects methods; use self for inherited fields");
                }
                ty = s->fields[i];
                at_class_field_use(c, n);
            }
        } else if (a) {
            at_check_error(c, n, "AS3202", "Field access requires a structure");
        }
        break;
    }
    case AN_CONDITIONAL:
        ty = at_check_conditional_value(c, n, want);
        break;
    case AN_UNARY:
        if (n->op == T_MINUS && n->a && n->a->kind == AN_INT) {
            ty = literal(c, n->a, want, 1);
            n->a->type = ty;
            break;
        }
        ty = at_check_expression(c, n->a, want);
        if (n->op == T_NOT) {
            at_check_mismatch(c, n, AT_BOOL, ty);
            ty = AT_BOOL;
        } else if (ty &&
                   (!at_check_number(p, ty) || (n->op == T_TILDE && !integer_operand(p, ty)))) {
            at_check_error(c, n, "AS3202", "Unary operator does not accept this type");
        }
        break;
    case AN_BINARY: {
        if ((n->op == T_EQ || n->op == T_NE) && (n->a->kind == AN_NONE || n->b->kind == AN_NONE)) {
            c->preserve_optional++;
            int a = at_check_expression(c, n->a, 0);
            int b = at_check_expression(c, n->b, 0);
            c->preserve_optional--;
            int other = a == AT_NONE ? b : a;
            if (other && other != AT_NONE && other != AT_ANY &&
                p->types[other].kind != AT_OPTIONAL) {
                at_check_error(c, n, "AS3202", "None comparison requires Optional, Any or None");
            }
            ty = AT_BOOL;
            break;
        }
        if (n->op == T_AND || n->op == T_OR) {
            at_check_mismatch(c, n->a, AT_BOOL, at_check_expression(c, n->a, AT_BOOL));
            int before[AT_LOCALS];
            for (int i = 0; i < c->f->nlocals; i++) {
                before[i] = c->f->locals[i].initialized;
            }
            at_optional_assume(c, n->a, n->op == T_AND);
            at_check_mismatch(c, n->b, AT_BOOL, at_check_expression(c, n->b, AT_BOOL));
            for (int i = 0; i < c->f->nlocals; i++) {
                c->f->locals[i].initialized = before[i];
            }
            ty = AT_BOOL;
            break;
        }
        if (n->op == T_IN) {
            /* Only type analysis visits the container first. Generated code
             * still evaluates the key before the container, in source order. */
            int container = at_check_view_read(c, n->b);
            AtType *type = &p->types[container];
            int sequence = type->kind == AT_LIST || type->kind == AT_ARRAY ||
                           at_slice_kind(type->kind) || type->kind == AT_RANGE ||
                           at_check_byte_storage(p, container, 0);
            int key = AT_STR;
            if (type->kind == AT_DICT) {
                key = type->key;
            } else if (type->kind == AT_RANGE || at_check_byte_storage(p, container, 0)) {
                key = AT_I64;
            } else if (sequence) {
                key = type->element;
            }
            if (container && container != AT_STR && type->kind != AT_DICT && !sequence) {
                at_check_error(c, n, "AS3202", "Membership requires a sequence or dictionary");
            }
            if (sequence && !at_satisfies_constraint(p, key, AT_CONSTRAINT_EQUATABLE)) {
                at_check_error(c, n, "AS3601", "Sequence membership requires Equatable elements");
            }
            at_check_mismatch(c, n->a, key, at_check_expression(c, n->a, key));
            ty = AT_BOOL;
            break;
        }
        if (n->op == T_PIPEOP || n->op == T_ARROW || n->op == T_LARROW) {
            at_check_mismatch(c, n->a, AT_COMMAND, at_check_expression(c, n->a, AT_COMMAND));
            int right = n->op == T_PIPEOP ? AT_COMMAND : AT_STR;
            at_check_mismatch(c, n->b, right, at_check_expression(c, n->b, right));
            ty = AT_COMMAND;
            break;
        }
        /* A comparison produces bool, but its operands may be containers,
         * numbers or callables. Passing the result context into the left
         * operand used to misdiagnose [1] < [2] as an empty-container error. */
        int comparison = n->op >= T_EQ && n->op <= T_GE;
        int a = at_check_expression(c, n->a, comparison ? AT_ERROR : want);
        int repeat = a == AT_STR && n->op == T_STAR;
        int b = at_check_expression(c, n->b, repeat ? AT_I64 : a);
        if (repeat) {
            at_check_mismatch(c, n, AT_I64, b);
            ty = AT_STR;
            break;
        }
        at_check_mismatch(c, n, a, b);
        ty = a;
        if (n->op == T_AND || n->op == T_OR) {
            at_check_mismatch(c, n, AT_BOOL, a);
            ty = AT_BOOL;
        } else if (n->op == T_IN) {
            at_check_mismatch(c, n, AT_STR, a);
            ty = AT_BOOL;
        } else if (n->op >= T_EQ && n->op <= T_GE) {
            int constraint =
                (n->op == T_EQ || n->op == T_NE) ? AT_CONSTRAINT_EQUATABLE : AT_CONSTRAINT_ORDERED;
            if (a && !at_satisfies_constraint(p, a, constraint)) {
                at_check_error(c, n, "AS3202",
                               "Comparison requires the Equatable or Ordered protocol");
            }
            ty = AT_BOOL;
        } else if (a && !at_check_number(p, a) && !(a == AT_STR && n->op == T_PLUS)) {
            at_check_error(c, n, "AS3202", "Arithmetic requires numeric operands");
        }
        if ((n->op == T_SHL || n->op == T_SHR || n->op == T_POW) && a && !integer_operand(p, a)) {
            at_check_error(c, n, "AS3202", "Shift and power operators require integer operands");
        }
        if ((n->op == T_AMP || n->op == T_PIPE || n->op == T_CARET) && a &&
            !integer_operand(p, a)) {
            at_check_error(c, n, "AS3202", "Bitwise operators require integers");
        }
        break;
    }
    default:
        at_check_error(c, n, "AS3900", "Unsupported expression node");
        break;
    }
    n->type = ty;
    return ty;
}

int at_check_expression(Checker *checker, AtNode *node, int expected)
{
    if (!node) {
        return AT_VOID;
    }
    int context = expected;
    if (checker->p->types[expected].kind == AT_OPTIONAL && node->kind != AN_CALL &&
        node->kind != AN_CONDITIONAL) {
        context = checker->p->types[expected].element;
    }
    /* A None comparison preserves only its operand's tag. Nested receivers
     * and arguments still use their own proven local facts: d.get(k) == None
     * must not make an already narrowed Optional[Dict] receiver nullable again. */
    int preserve = checker->preserve_optional;
    int receiver = checker->field_receiver;
    checker->preserve_optional = 0;
    if (node->kind != AN_NAME && node->kind != AN_SUPER) {
        checker->field_receiver = 0;
    }
    int actual = expression_type(checker, node, context);
    int scoped_view = node->kind == AN_NAME && node->symbol >= 0 &&
                      node->symbol < checker->f->nlocals &&
                      checker->f->locals[node->symbol].scoped_borrow;
    if ((at_resource_type(actual) || scoped_view ||
         checker->p->types[actual].kind == AT_MUT_SLICE) &&
        node != checker->resource_use && node != checker->resource_acquisition) {
        at_check_error(checker, node, "AS3401",
                       "A scoped resource may only be borrowed by its methods or iteration");
    }
    checker->preserve_optional = preserve;
    checker->field_receiver = receiver;
    node->type = at_check_optional_conversion(checker, node, actual, expected);
    return node->type;
}
