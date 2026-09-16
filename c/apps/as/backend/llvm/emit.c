/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* A typed tree is lowered to LLVM SSA values and entry-block stack slots.
 * Clang's mem2reg/loop passes promote slots; no Value tag, opcode dispatch or
 * VM entry is linked into the emitted executable. Overflow intrinsics keep
 * O0 and O2 semantics identical: https://llvm.org/docs/LangRef.html */
Val at_ir_list_at(Gen *g, Val list, Val index, AtNode *site)
{
    Val result = at_ir_temp(g, 0), bad = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = call ptr @at_list_at(ptr %s, i64 %s)\n", result.text, list.text,
               index.text);
    at_ir_emit(g, "  %s = icmp eq ptr %s, null\n", bad.text, result.text);
    at_ir_guard(g, bad, site, AT_E_INDEX);
    return result;
}

Val at_ir_address(Gen *g, AtNode *n)
{
    if (n->conversion == AT_OPTION_UNWRAP) {
        AtNode raw = *n;
        raw.type = n->value_type;
        raw.conversion = AT_OPTION_IDENTITY;
        Val base = at_ir_address(g, &raw), result = at_ir_temp(g, n->type);
        at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i32 0, i32 1\n", result.text,
                   at_ir_type(g, raw.type), base.text);
        return result;
    }
    /* Assignments and call-scoped array borrows need storage, while ordinary
     * expressions need values. Keep the two paths separate so taking a field
     * address never accidentally updates a temporary copy of its struct. */
    if (n->kind == AN_GLOBAL) {
        /* Address-based array/field reads need this guard too. Otherwise a
         * function invoked during initialization could observe zero storage. */
        Val ready = at_ir_temp(g, AT_BOOL), bad = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = load i1, ptr @global_initialized%d\n", ready.text, n->symbol);
        at_ir_emit(g, "  %s = xor i1 %s, true\n", bad.text, ready.text);
        at_ir_guard(g, bad, n, AT_E_RUNTIME);
        Val v = {.type = n->type};
        snprintf(v.text, sizeof v.text, "@global%d", n->symbol);
        return v;
    }
    if (n->kind == AN_NAME) {
        Val v = {.type = n->type};
        snprintf(v.text, sizeof v.text, "%%local%d", n->symbol);
        return v;
    }
    if (n->kind == AN_FIELD) {
        if (g->p->types[n->a->type].kind == AT_LAYOUT) {
            return at_ir_layout_address(g, n, at_ir_expression(g, n->a));
        }
        if (g->p->types[n->a->type].kind == AT_CLASS) {
            Val object = at_ir_expression(g, n->a), address = at_ir_temp(g, n->type);
            at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i32 0, i32 %d\n", address.text,
                       at_ir_class_layout(g, n->a->type), object.text,
                       n->field + AT_CLASS_HEADER_COUNT);
            return address;
        }
        Val base = at_ir_address(g, n->a), v = at_ir_temp(g, n->type);
        at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i32 0, i32 %d\n", v.text,
                   at_ir_type(g, n->a->type), base.text, n->field);
        return v;
    }
    if (n->kind == AN_INDEX) {
        Val base;
        Val length;
        AtType *array = &g->p->types[n->a->type];
        if (array->kind == AT_REGION) {
            Val owner = at_ir_expression(g, n->a);
            Val index = at_ir_expression(g, n->b);
            return at_ir_region_at(g, owner, index, n, 0);
        }
        if (array->kind == AT_POINTER) {
            Val pointer = at_ir_expression(g, n->a);
            Val index = at_ir_expression(g, n->b);
            return at_ir_pointer_at(g, pointer, index, n);
        }
        if (at_byte_storage_kind(array->kind)) {
            Val buffer = at_ir_expression(g, n->a);
            Val index = at_ir_expression(g, n->b);
            return at_ir_buffer_at(g, buffer, index, n);
        }
        if (array->kind == AT_DICT) {
            Val dictionary = at_ir_expression(g, n->a);
            Val key = at_ir_expression(g, n->b);
            Val pointer = at_ir_dict_lookup(g, n, dictionary, key);
            Val missing = at_ir_temp(g, AT_BOOL);
            at_ir_emit(g, "  %s = icmp eq ptr %s, null\n", missing.text, pointer.text);
            at_ir_guard(g, missing, n, AT_E_KEY);
            return pointer;
        }
        if (array->kind == AT_LIST) {
            Val list = at_ir_expression(g, n->a);
            Val index = at_ir_expression(g, n->b);
            return at_ir_list_at(g, list, index, n);
        }
        if (array->kind == AT_ARRAY) {
            base = at_ir_address(g, n->a);
            char s[32];
            snprintf(s, sizeof s, "%d", array->count);
            length = at_ir_value(AT_I64, s);
        } else {
            Val slice = at_ir_expression(g, n->a);
            base = at_ir_temp(g, 0);
            length = at_ir_temp(g, AT_I64);
            at_ir_emit(g, "  %s = extractvalue %s %s, 0\n  %s = extractvalue %s %s, 1\n", base.text,
                       at_ir_type(g, slice.type), slice.text, length.text,
                       at_ir_type(g, slice.type), slice.text);
        }
        Val i = at_ir_expression(g, n->b), bad = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = icmp uge i64 %s, %s\n", bad.text, i.text, length.text);
        at_ir_guard(g, bad, n, 3);
        Val v = at_ir_temp(g, n->type);
        if (array->kind == AT_ARRAY) {
            at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i32 0, i64 %s\n", v.text,
                       at_ir_type(g, n->a->type), base.text, i.text);
        } else {
            at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i64 %s\n", v.text,
                       at_ir_type(g, n->type), base.text, i.text);
        }
        return v;
    }
    Val v = at_ir_expression(g, n), slot = {.type = n->type};
    snprintf(slot.text, sizeof slot.text, "%%storage%d", n->id);
    /* A source expression has one slot in this invocation's entry block.
     * alloca here would allocate again on every loop iteration at O0 and keep
     * all those dead temporaries until the containing function returned. */
    at_ir_emit(g, "  store %s %s, ptr %s\n", at_ir_type(g, n->type), v.text, slot.text);
    return slot;
}

static int string_bytes(Token t, char *out)
{
    return as_token_decode(t, out);
}

static Val expression_value(Gen *g, AtNode *n)
{
    if (!n) {
        return at_ir_value(AT_VOID, "");
    }
    Val v = at_ir_temp(g, n->type);
    if (n->kind == AN_NONE) {
        return at_ir_value(AT_NONE, "0");
    }
    if (n->kind == AN_INT || n->kind == AN_CONSTANT) {
        /* The checker already chose the radix and checked the target width.
         * Re-parsing with base 0 previously changed decimal 010 into octal 8. */
        snprintf(v.text, sizeof v.text, "%llu", (unsigned long long)n->integer);
        return v;
    }
    if (n->kind == AN_FLOAT) {
        char s[96];
        int len = n->token.len;
        if (len > 90) {
            len = 90;
        }
        memcpy(s, n->token.start, (size_t)len);
        s[len] = 0;

        union {
            double d;
            uint64_t u;
        } bits;

        bits.d = strtod(s, NULL);
        if (n->type == AT_F32) {
            bits.d = (double)(float)bits.d;
        }
        snprintf(v.text, sizeof v.text, "0x%016llX", (unsigned long long)bits.u);
        return v;
    }
    if (n->kind == AN_BOOL) {
        snprintf(v.text, sizeof v.text, "%d", n->token.type == T_TRUE || n->token.start[0] == 'T');
        return v;
    }
    if (n->kind == AN_SUPER) {
        at_ir_emit(g, "  %s = load ptr, ptr %%local%d\n", v.text, n->symbol);
        return v;
    }
    if (n->kind == AN_FUNCTION) {
        snprintf(v.text, sizeof v.text, "{ ptr @callable%d, ptr null }", n->symbol);
        return v;
    }
    if (n->kind == AN_CLOSURE) {
        return at_ir_closure(g, n, v);
    }
    if (n->kind == AN_METHOD) {
        return at_ir_bound_method(g, n, v);
    }
    if (n->kind == AN_STR) {
        /* A lowering view may carry the pre-injection type of an Optional.
         * Its stable node ID still names the original source string. */
        snprintf(v.text, sizeof v.text, "{ ptr @str%d, i64 %d }", n->id,
                 string_bytes(n->token, NULL));
        return v;
    }
    if (n->kind == AN_FORMAT) {
        /* Reuse precisely the str(value) conversion and its exception checks.
         * A synthetic source name could be shadowed by a local called str;
         * AN_FORMAT already denotes the built-in conversion after checking. */
        AtNode call = *n;
        call.kind = AN_CALL;
        call.symbol = AT_CALL_STR;
        call.count = 1;
        call.args = &n->a;
        return at_ir_call(g, &call, v);
    }
    if (n->kind == AN_INDEX && n->a->type == AT_RANGE) {
        return at_ir_range_index(g, n, v);
    }
    if (n->kind == AN_INDEX && n->a->type == AT_STR) {
        Val text = at_ir_expression(g, n->a), index = at_ir_expression(g, n->b);
        Val pointer = at_ir_temp(g, 0), length = at_ir_temp(g, AT_I64);
        at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", pointer.text, at_ir_type(g, AT_STR),
                   text.text);
        at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", length.text, at_ir_type(g, AT_STR),
                   text.text);
        Val negative = at_ir_temp(g, AT_BOOL), offset = at_ir_temp(g, AT_I64),
            normalized = at_ir_temp(g, AT_I64);
        at_ir_emit(g, "  %s = icmp slt i64 %s, 0\n", negative.text, index.text);
        at_ir_emit(g, "  %s = add i64 %s, %s\n", offset.text, index.text, length.text);
        at_ir_emit(g, "  %s = select i1 %s, i64 %s, i64 %s\n", normalized.text, negative.text,
                   offset.text, index.text);
        Val bad = at_ir_temp(g, AT_BOOL), stop = at_ir_temp(g, AT_I64);
        at_ir_emit(g, "  %s = icmp uge i64 %s, %s\n", bad.text, normalized.text, length.text);
        at_ir_guard(g, bad, n, AT_E_INDEX);
        at_ir_emit(g, "  %s = add i64 %s, 1\n", stop.text, normalized.text);
        at_ir_emit(g,
                   "  call void @at_text_slice(ptr %%storage%d, ptr %s, i64 %s, i64 %s, i64 %s)\n",
                   n->id, pointer.text, length.text, normalized.text, stop.text);
        at_ir_emit(g, "  %s = load %s, ptr %%storage%d\n", v.text, at_ir_type(g, AT_STR), n->id);
        return v;
    }
    if (n->kind == AN_NAME || n->kind == AN_GLOBAL || n->kind == AN_INDEX) {
        Val a = at_ir_address(g, n);
        if (n->kind == AN_INDEX &&
            (n->a->type == AT_REGION || at_byte_storage_kind(g->p->types[n->a->type].kind))) {
            return at_ir_buffer_read(g, a);
        }
        const char *alignment =
            n->kind == AN_INDEX && g->p->types[n->a->type].kind == AT_POINTER ? ", align 1" : "";
        at_ir_emit(g, "  %s = load %s, ptr %s%s\n", v.text, at_ir_type(g, n->type), a.text,
                   alignment);
        return v;
    }
    if (n->kind == AN_FIELD) {
        if (g->p->types[n->a->type].kind == AT_LAYOUT) {
            return at_ir_layout_read(g, n, v);
        }
        if (g->p->types[n->a->type].kind == AT_CLASS) {
            Val address = at_ir_address(g, n);
            at_ir_emit(g, "  %s = load %s, ptr %s\n", v.text, at_ir_type(g, n->type), address.text);
            return v;
        }
        Val a = at_ir_expression(g, n->a);
        at_ir_emit(g, "  %s = extractvalue %s %s, %d\n", v.text, at_ir_type(g, a.type), a.text,
                   n->field);
        return v;
    }
    if (n->kind == AN_DICT) {
        return at_ir_dict_literal(g, n, v);
    }
    if (n->kind == AN_COMPREHENSION) {
        return at_ir_comprehension(g, n, v);
    }
    if (n->kind == AN_ARRAY) {
        if (g->p->types[n->type].kind == AT_LIST) {
            int element = g->p->types[n->type].element;
            /* LLVM computes the actual native stride (including struct padding).
             * Register the list before evaluating elements: those calls allocate. */
            at_ir_emit(
                g,
                "  %s = call ptr @at_list_new(i64 ptrtoint (ptr getelementptr (%s, ptr null, i32 "
                "1) to i64), ptr @scan%d)\n",
                v.text, at_ir_type(g, element), element);
            at_ir_allocation(g, v, n, 1);
            at_ir_emit(g, "  store ptr %s, ptr %%construction%d\n", v.text, n->id);
            for (int i = 0; i < n->count; i++) {
                Val item = at_ir_expression(g, n->args[i]), status = at_ir_temp(g, AT_I32);
                at_ir_emit(g, "  store %s %s, ptr %%argument%d\n", at_ir_type(g, item.type),
                           item.text, n->id);
                at_ir_emit(g, "  %s = call i32 @at_list_append(ptr %s, ptr %%argument%d)\n",
                           status.text, v.text, n->id);
                at_ir_allocation(g, status, n, 0);
            }
            return v;
        }
        Val old = at_ir_value(n->type, "zeroinitializer");
        for (int i = 0; i < n->count; i++) {
            Val item = at_ir_expression(g, n->args[i]), next = at_ir_temp(g, n->type);
            at_ir_emit(g, "  %s = insertvalue %s %s, %s %s, %d\n", next.text,
                       at_ir_type(g, n->type), old.text, at_ir_type(g, item.type), item.text, i);
            old = next;
        }
        return old;
    }
    if (n->kind == AN_CONDITIONAL) {
        Val condition = at_ir_expression(g, n->a);
        int yes = at_ir_label(g), no = at_ir_label(g), yes_end = at_ir_label(g),
            no_end = at_ir_label(g), done = at_ir_label(g);
        at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", condition.text, yes, no);
        at_ir_mark(g, yes);
        Val when_true = at_ir_expression(g, n->b);
        at_ir_jump(g, yes_end);
        at_ir_mark(g, yes_end);
        at_ir_jump(g, done);
        at_ir_mark(g, no);
        Val when_false = at_ir_expression(g, n->c);
        at_ir_jump(g, no_end);
        at_ir_mark(g, no_end);
        at_ir_jump(g, done);
        at_ir_mark(g, done);
        /* Named predecessors remain valid when either arm adds error guards
         * or nested conditional blocks. Only the chosen arm is evaluated. */
        at_ir_emit(g, "  %s = phi %s [ %s, %%b%d ], [ %s, %%b%d ]\n", v.text,
                   at_ir_type(g, n->type), when_true.text, yes_end, when_false.text, no_end);
        return v;
    }
    if (n->kind == AN_UNARY) {
        if (n->op == T_MINUS && n->a->kind == AN_INT) {
            uint64_t x = n->a->integer;
            snprintf(v.text, sizeof v.text, "%llu", (unsigned long long)(0 - x));
            return v;
        }
        Val a = at_ir_expression(g, n->a);
        if (n->op == T_PLUS) {
            return a;
        }
        if (n->op == T_NOT || n->op == T_TILDE) {
            at_ir_emit(g, "  %s = xor %s %s, %s\n", v.text, at_ir_type(g, a.type), a.text,
                       n->op == T_NOT ? "1" : "-1");
        } else if (n->type == AT_F32 || n->type == AT_F64) {
            at_ir_emit(g, "  %s = fneg %s %s\n", v.text, at_ir_type(g, n->type), a.text);
        } else {
            return at_ir_numeric(g, T_MINUS, at_ir_value(a.type, "0"), a, n, 0);
        }
        return v;
    }
    if (n->kind == AN_BINARY) {
        if (n->op == T_PIPEOP || n->op == T_ARROW || n->op == T_LARROW) {
            return at_ir_command_operator(g, n);
        }
        Val a = at_ir_expression(g, n->a);
        if (n->op == T_AND || n->op == T_OR) {
            int rhs = at_ir_label(g), shortc = at_ir_label(g), done = at_ir_label(g);
            at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", a.text,
                       n->op == T_AND ? rhs : shortc, n->op == T_AND ? shortc : rhs);
            at_ir_mark(g, shortc);
            at_ir_jump(g, done);
            at_ir_mark(g, rhs);
            Val b = at_ir_expression(g, n->b);
            int from = at_ir_label(g);
            at_ir_jump(g, from);
            at_ir_mark(g, from);
            at_ir_jump(g, done);
            at_ir_mark(g, done);
            at_ir_emit(g, "  %s = phi i1 [ %d, %%b%d ], [ %s, %%b%d ]\n", v.text, n->op == T_OR,
                       shortc, b.text, from);
            return v;
        }
        Val b = at_ir_expression(g, n->b);
        if (n->op == T_IN && g->p->types[b.type].kind == AT_DICT) {
            Val pointer = at_ir_dict_lookup(g, n, b, a);
            at_ir_emit(g, "  %s = icmp ne ptr %s, null\n", v.text, pointer.text);
            return v;
        }
        if (n->op == T_IN && b.type != AT_STR) {
            return at_ir_sequence_contains(g, n, a, b);
        }
        if ((n->op == T_EQ || n->op == T_NE) &&
            (g->p->types[a.type].kind == AT_CALLABLE || a.type == AT_ANY || a.type == AT_NONE ||
             b.type == AT_NONE || g->p->types[a.type].kind == AT_OPTIONAL)) {
            Val equal = at_ir_equal(g, a, b, n);
            if (n->op == T_EQ) {
                return equal;
            }
            at_ir_emit(g, "  %s = xor i1 %s, true\n", v.text, equal.text);
            return v;
        }
        return at_ir_numeric(g, n->op, a, b, n, 0);
    }
    if (n->kind == AN_CALL) {
        return at_ir_call(g, n, v);
    }
    return v;
}

Val at_ir_expression(Gen *g, AtNode *n)
{
    if (n && n->class_ready) {
        Val receiver = at_ir_temp(g, g->f->method_owner);
        at_ir_emit(g, "  %s = load ptr, ptr %%local0\n", receiver.text);
        at_ir_class_ready(g, receiver, n);
    }
    AtNode raw;
    AtNode *source = n;
    if (n && n->conversion) {
        raw = *n;
        raw.type = n->value_type;
        raw.conversion = AT_OPTION_IDENTITY;
        source = &raw;
    }
    Val result = expression_value(g, source);
    if (n && n->conversion == AT_OPTION_WRAP) {
        result = at_ir_optional_wrap(g, result, n->type);
    } else if (n && n->conversion == AT_OPTION_UNWRAP) {
        Val payload = at_ir_temp(g, n->type);
        at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", payload.text, at_ir_type(g, result.type),
                   result.text);
        result = payload;
    } else if (n && n->conversion == AT_CLASS_UPCAST) {
        /* The allocation keeps its concrete layout and scanner. Only the
         * static view changes; both views use the same native pointer ABI. */
        result.type = n->type;
    }
    if (n && at_ir_references(g, n->type)) {
        /* Earlier arguments and intermediate aggregates must survive later
         * argument calls. Locals alone are insufficient roots for f(a(), b()).
         * Slots are reused on loop iterations and released on every return.
         * Correction: completed statements now clear their temporary slots too,
         * so overwritten owners are collectible before the function returns. */
        at_ir_emit(g, "  store %s %s, ptr %%rootvalue%d\n", at_ir_type(g, n->type), result.text,
                   n->id);
    }
    return result;
}

void at_ir_statements(Gen *g, AtNode *n);

static Val binding_address(Gen *g, AtNode *binding)
{
    if (binding->kind == AN_GLOBAL) {
        /* A direct store establishes initialization. Subfield/index stores
         * still use address(), requiring an already initialized aggregate. */
        Val target = {.type = binding->type};
        snprintf(target.text, sizeof target.text, "@global%d", binding->symbol);
        return target;
    }
    return at_ir_address(g, binding);
}

void at_ir_store_binding(Gen *g, AtNode *binding, Val value)
{
    if (binding->kind == AN_INDEX && binding->a->type == AT_REGION) {
        Val owner = at_ir_expression(g, binding->a);
        Val index = at_ir_expression(g, binding->b);
        at_ir_buffer_write(g, binding, at_ir_region_at(g, owner, index, binding, 1), value);
        return;
    }
    if (binding->kind == AN_FIELD && g->p->types[binding->a->type].kind == AT_LAYOUT) {
        at_ir_layout_store(g, binding, value);
        return;
    }
    if (binding->kind == AN_INDEX && at_byte_storage_kind(g->p->types[binding->a->type].kind)) {
        at_ir_buffer_write(g, binding, at_ir_address(g, binding), value);
        return;
    }
    Val target = binding_address(g, binding);
    const char *alignment =
        binding->kind == AN_INDEX && g->p->types[binding->a->type].kind == AT_POINTER ? ", align 1"
                                                                                      : "";
    at_ir_emit(g, "  store %s %s, ptr %s%s\n", at_ir_type(g, binding->type), value.text,
               target.text, alignment);
    if (binding->kind == AN_GLOBAL) {
        at_ir_emit(g, "  store i1 true, ptr @global_initialized%d\n", binding->symbol);
    }
}

static void emit_try(Gen *g, AtNode *node)
{
    int outer = g->handler_label;
    AtCleanup *outer_cleanup = g->handler_cleanup;
    int dispatch = at_ir_label(g), done = at_ir_label(g);
    g->handler_label = dispatch;
    g->handler_cleanup = g->cleanup;
    at_ir_statements(g, node->a);
    g->handler_label = outer;
    g->handler_cleanup = outer_cleanup;
    /* Errors in else or in a handler escape to the enclosing try, rather than
     * being caught a second time by siblings of this same except clause. */
    if (!g->terminated) {
        at_ir_statements(g, node->c);
    }
    int falls_through = !g->terminated;
    at_ir_jump(g, done);
    at_ir_mark(g, dispatch);
    for (AtNode *handler = node->b; handler; handler = handler->next) {
        int accepted = at_ir_label(g), next = at_ir_label(g);
        if (handler->op == AT_E_ANY) {
            at_ir_jump(g, accepted);
        } else {
            Val kind = at_ir_temp(g, AT_I32), matches = at_ir_temp(g, AT_BOOL);
            at_ir_emit(g, "  %s = call i32 @at_exception_kind()\n", kind.text);
            at_ir_emit(g, "  %s = icmp eq i32 %s, %d\n", matches.text, kind.text, handler->op);
            at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", matches.text, accepted, next);
        }
        at_ir_mark(g, accepted);
        at_ir_emit(g, "  call void @at_exception_take(ptr %%caught%d)\n", handler->id);
        if (handler->a) {
            Val error = at_ir_temp(g, g->p->exception_type);
            at_ir_emit(g, "  %s = load %s, ptr %%caught%d\n", error.text, at_ir_type(g, error.type),
                       handler->id);
            at_ir_store_binding(g, handler->a, error);
        }
        AtNode *previous = g->caught;
        g->caught = handler;
        at_ir_statements(g, handler->b);
        g->caught = previous;
        falls_through |= !g->terminated;
        at_ir_jump(g, done);
        at_ir_mark(g, next);
    }
    at_ir_throw(g);
    if (falls_through) {
        at_ir_mark(g, done);
    }
}

void at_ir_statements(Gen *g, AtNode *n)
{
    for (; n && !g->terminated; n = n->next) {
        if (n->kind == AN_UNPACK) {
            at_ir_unpack(g, n);
        } else if (n->kind == AN_ASSIGN && n->b) {
            if (n->op != T_ASSIGN) {
                at_ir_compound_assignment(g, n);
                at_ir_release_temporaries(g, n);
                continue;
            }
            Val v = at_ir_expression(g, n->b);
            if (n->a->kind == AN_INDEX && at_byte_storage_kind(g->p->types[n->a->a->type].kind)) {
                at_ir_buffer_store(g, n, v);
                at_ir_release_temporaries(g, n);
                continue;
            }
            if (n->op == T_ASSIGN && n->a->kind == AN_INDEX &&
                g->p->types[n->a->a->type].kind == AT_DICT) {
                /* Ordinary assignment can insert a new key; compound updates
                 * use the checked address path and require an existing key. */
                Val dictionary = at_ir_expression(g, n->a->a);
                Val key = at_ir_expression(g, n->a->b);
                at_ir_dict_store(g, n->a, dictionary, key, v);
                at_ir_release_temporaries(g, n);
                continue;
            }
            at_ir_store_target(g, n, v);
        } else if (n->kind == AN_WITH) {
            at_ir_with(g, n);
        } else if (n->kind == AN_UNSAFE) {
            at_ir_statements(g, n->a);
        } else if (n->kind == AN_TRY) {
            emit_try(g, n);
        } else if (n->kind == AN_RAISE) {
            if (n->a) {
                Val error = at_ir_expression(g, n->a);
                at_ir_emit(g, "  store %s %s, ptr %%storage%d\n", at_ir_type(g, error.type),
                           error.text, n->a->id);
                at_ir_emit(
                    g,
                    "  call void @at_exception_throw(ptr %%storage%d, ptr @path%d, i32 %d, i32 "
                    "%d)\n",
                    n->a->id, n->module, n->token.line, at_ir_column(g, n));
            } else {
                at_ir_emit(
                    g,
                    "  call void @at_exception_throw(ptr %%caught%d, ptr @path%d, i32 %d, i32 "
                    "%d)\n",
                    g->caught->id, n->module, n->token.line, at_ir_column(g, n));
            }
            at_ir_throw(g);
        } else if (n->kind == AN_EXPR) {
            Val value = at_ir_expression(g, n->a);
            if (n->op) {
                Val status = at_ir_temp(g, AT_I32);
                at_ir_emit(g, "  %s = call i32 @at_command_wait(ptr %%commandresult%d, ptr %s)\n",
                           status.text, n->id, value.text);
                at_ir_runtime_status(g, status, n);
            }
        } else if (n->kind == AN_ASSERT) {
            Val condition = at_ir_expression(g, n->a);
            Val failed = at_ir_temp(g, AT_BOOL);
            at_ir_emit(g, "  %s = xor i1 %s, true\n", failed.text, condition.text);
            at_ir_guard(g, failed, n, 6);
        } else if (n->kind == AN_RETURN) {
            Val v = at_ir_expression(g, n->a);
            at_ir_cleanup_to(g, NULL, 0);
            at_ir_emit(g, "  call void @at_gc_restore(ptr %%frame)\n");
            if (g->f->result == AT_VOID) {
                at_ir_emit(g, "  ret void\n");
            } else {
                at_ir_emit(g, "  ret %s %s\n", at_ir_type(g, v.type), v.text);
            }
            g->terminated = 1;
        } else if (n->kind == AN_IF) {
            Val v = at_ir_expression(g, n->a);
            int yes = at_ir_label(g), no = at_ir_label(g), end = at_ir_label(g);
            at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", v.text, yes, no);
            at_ir_mark(g, yes);
            at_ir_statements(g, n->b);
            int then_terminated = g->terminated;
            at_ir_jump(g, end);
            at_ir_mark(g, no);
            at_ir_statements(g, n->c);
            int else_terminated = g->terminated;
            at_ir_jump(g, end);
            if (then_terminated && else_terminated) {
                g->terminated = 1;
            } else {
                at_ir_mark(g, end);
            }
        } else if (n->kind == AN_WHILE) {
            AtCleanup *old_cleanup = g->loop_cleanup;
            g->loop_cleanup = g->cleanup;
            int cond = at_ir_label(g), body = at_ir_label(g), end = at_ir_label(g),
                oldbreak = g->break_label, oldcontinue = g->continue_label;
            g->break_label = end;
            g->continue_label = cond;
            at_ir_jump(g, cond);
            at_ir_mark(g, cond);
            Val v = at_ir_expression(g, n->a);
            at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", v.text, body, end);
            at_ir_mark(g, body);
            at_ir_statements(g, n->b);
            at_ir_jump(g, cond);
            at_ir_mark(g, end);
            g->break_label = oldbreak;
            g->continue_label = oldcontinue;
            g->loop_cleanup = old_cleanup;
        } else if (n->kind == AN_FOR) {
            if (n->b->type == AT_PORT) {
                at_ir_port_loop(g, n);
            } else {
                at_ir_for_loop(g, n, NULL, NULL);
            }
        } else if (n->kind == AN_BREAK) {
            at_ir_cleanup_to(g, g->loop_cleanup, 0);
            at_ir_jump(g, g->break_label);
        } else if (n->kind == AN_CONTINUE) {
            at_ir_cleanup_to(g, g->loop_cleanup, 0);
            at_ir_jump(g, g->continue_label);
        }
        if (!g->terminated) {
            at_ir_release_temporaries(g, n);
        }
    }
}

static void bytes(FILE *out, const char *s, int n)
{
    fputs("c\"", out);
    for (int i = 0; i < n; i++) {
        fprintf(out, "\\%02X", (unsigned char)s[i]);
    }
    fputs("\\00\"", out);
}

static void emit_scanner(Gen *g, int type)
{
    AtType *t = &g->p->types[type];
    at_ir_emit(g, "define internal void @scan%d(ptr %%slot) {\nentry:\n", type);
    if (t->kind == AT_STR || t->kind == AT_LIST || t->kind == AT_ANY || t->kind == AT_DICT ||
        t->kind == AT_CLASS || t->kind == AT_RANGE || at_byte_storage_kind(t->kind) ||
        t->kind == AT_CAP || t->kind == AT_COMMAND || t->kind == AT_PROCESS) {
        /* str starts with its data pointer; List and Any are single pointers. */
        at_ir_emit(g,
                   "  %%pointer = load ptr, ptr %%slot\n  call void @at_gc_mark(ptr %%pointer)\n");
    } else if (t->kind == AT_CALLABLE) {
        at_ir_emit(g, "  %%environment = getelementptr { ptr, ptr }, ptr %%slot, i32 0, i32 1\n"
                      "  %%pointer = load ptr, ptr %%environment\n"
                      "  call void @at_gc_mark(ptr %%pointer)\n");
    } else if (t->kind == AT_OPTIONAL && at_ir_references(g, t->element)) {
        /* Absent payloads need not contain initialized language values. The
         * tag is authoritative; never scan a None payload as a live pointer. */
        at_ir_emit(g, "  %%present = load i1, ptr %%slot\n"
                      "  br i1 %%present, label %%payload, label %%done\npayload:\n");
        at_ir_emit(g, "  %%value = getelementptr %s, ptr %%slot, i32 0, i32 1\n",
                   at_ir_type(g, type));
        at_ir_emit(g, "  call void @scan%d(ptr %%value)\n  br label %%done\ndone:\n", t->element);
    } else if (t->kind == AT_STRUCT) {
        for (int i = 0; i < t->count; i++) {
            if (at_ir_references(g, t->fields[i])) {
                at_ir_emit(g, "  %%field%d = getelementptr %s, ptr %%slot, i32 0, i32 %d\n", i,
                           at_ir_type(g, type), i);
                at_ir_emit(g, "  call void @scan%d(ptr %%field%d)\n", t->fields[i], i);
            }
        }
    } else if ((t->kind == AT_ARRAY || at_slice_kind(t->kind)) && at_ir_references(g, t->element)) {
        /* Emit a loop, not one call per element: Array[str, 65536] must not
         * expand a small type annotation into megabytes of scanner code. */
        if (t->kind == AT_ARRAY) {
            at_ir_emit(g, "  %%data = getelementptr %s, ptr %%slot, i32 0, i32 0\n",
                       at_ir_type(g, type));
            at_ir_emit(g, "  %%length = add i64 0, %d\n", t->count);
        } else {
            at_ir_emit(g, "  %%slice = load %s, ptr %%slot\n", at_ir_type(g, type));
            at_ir_emit(g, "  %%data = extractvalue %s %%slice, 0\n", at_ir_type(g, type));
            at_ir_emit(g, "  %%length = extractvalue %s %%slice, 1\n", at_ir_type(g, type));
        }
        at_ir_emit(g,
                   "  br label %%loop\nloop:\n  %%index = phi i64 [0, %%entry], [%%next, %%body]\n"
                   "  %%more = icmp slt i64 %%index, %%length\n"
                   "  br i1 %%more, label %%body, label %%done\nbody:\n");
        at_ir_emit(g, "  %%element = getelementptr %s, ptr %%data, i64 %%index\n",
                   at_ir_type(g, t->element));
        at_ir_emit(g, "  call void @scan%d(ptr %%element)\n", t->element);
        at_ir_emit(g, "  %%next = add i64 %%index, 1\n  br label %%loop\ndone:\n");
    }
    at_ir_emit(g, "  ret void\n}\n");
}

int as_typed_llvm(AsTypedProject *p, FILE *out, int tests)
{
    if (as_typed_errors(p)) {
        return -1;
    }
    int main = -1, ntests = 0;
    for (int i = 0; i < p->nfunctions; i++) {
        AtFunction *f = &p->functions[i];
        if (f->module_initializer || f->method_owner || f->lexical_parent || f->generic_count ||
            f->template_id >= 0) {
            continue;
        }
        if (f->module == 0 && !strcmp(f->name, "main")) {
            main = i;
        }
        if (!strncmp(f->name, "test_", 5) && !f->nparams && f->result == AT_VOID) {
            ntests++;
        }
    }
    if ((!tests &&
         (main < 0 || p->functions[main].nparams ||
          (p->functions[main].result != AT_VOID && p->functions[main].result != AT_I64))) ||
        (tests && !ntests)) {
        Token t = {.start = p->modules[0].source, .line = 1};
        at_error(p, 0, t, "AS3500",
                 tests ? "No zero-argument test_* functions returning None"
                       : "Native executable requires main() -> i64 or main() -> None");
        return -1;
    }
    Gen g = {.p = p, .out = out};
    at_ir_emit(&g, "; AetherScript 3 typed native module. No VM dependency.\n@at_failed = external "
                   "global i32\n");
    at_ir_array_definitions(&g);
    for (int i = 0; i < p->ntypes; i++) {
        if (at_has_parameter(p, i)) {
            continue;
        }
        if (p->types[i].kind == AT_OPTIONAL) {
            at_ir_emit(&g, "%%S%d = type { i1, %s }\n", i, at_ir_type(&g, p->types[i].element));
        } else if (p->types[i].kind == AT_STRUCT || p->types[i].kind == AT_CLASS) {
            at_ir_emit(&g, "%s = type { ",
                       p->types[i].kind == AT_CLASS ? at_ir_class_layout(&g, i)
                                                    : at_ir_type(&g, i));
            if (p->types[i].kind == AT_CLASS) {
                int header_field = 0;
#define AT_EMIT_HEADER(name, c_type, llvm_type)                                                    \
    at_ir_emit(&g, "%s%s", header_field++ ? ", " : "", llvm_type);
                AT_CLASS_HEADER(AT_EMIT_HEADER)
#undef AT_EMIT_HEADER
            }
            for (int j = 0; j < p->types[i].count; j++) {
                at_ir_emit(&g, "%s%s", j || p->types[i].kind == AT_CLASS ? ", " : "",
                           at_ir_type(&g, p->types[i].fields[j]));
            }
            at_ir_emit(&g, " }\n");
        }
    }
    at_ir_type_descriptions(&g);
    at_ir_class_tables(&g);
    for (int i = 0; i < p->nglobals; i++) {
        at_ir_emit(&g, "@global%d = internal global %s zeroinitializer\n", i,
                   at_ir_type(&g, p->globals[i].type));
        at_ir_emit(&g, "@global_initialized%d = internal global i1 false\n", i);
    }
    for (int m = 0; m < p->nmodules; m++) {
        at_ir_emit(&g, "@path%d = private constant [%zu x i8] ", m, strlen(p->modules[m].path) + 1);
        bytes(out, p->modules[m].path, (int)strlen(p->modules[m].path));
        at_ir_emit(&g, "\n");
    }
    for (int i = 0; i < p->nnodes; i++) {
        if (p->nodes[i]->kind == AN_STR) {
            Token t = p->nodes[i]->token;
            char *s = malloc((size_t)t.len + 1);
            if (!s) {
                return -1;
            }
            int n = string_bytes(t, s);
            at_ir_emit(&g, "@str%d = private constant [%d x i8] ", i, n + 1);
            bytes(out, s, n);
            at_ir_emit(&g, "\n");
            free(s);
        }
    }
    at_ir_emit(&g,
               "declare void @at_raise(i32, ptr, i32, i32)\ndeclare i32 @at_finish()\ndeclare void "
               "@at_print_i64(i64)\ndeclare void @at_print_u64(i64)\ndeclare void "
               "@at_print_f64(double)\ndeclare void @at_print_str(ptr, i64)\ndeclare void "
               "@at_print_bool(i32)\ndeclare void @at_print_sep(i32)\n");
    at_ir_emit(&g, "declare ptr @at_range_new(i64, i64, i64)\n"
                   "declare i64 @at_range_bound(ptr, i32)\n"
                   "declare i64 @at_range_len(ptr)\n"
                   "declare i32 @at_range_at(ptr, ptr, i64)\n"
                   "declare i32 @at_range_contains(ptr, i64)\n");
    at_ir_emit(&g, "declare i32 @at_command_new(ptr, ptr, i64)\n"
                   "declare i32 @at_command_new_list(ptr, ptr)\n"
                   "declare i32 @at_command_pipe(ptr, ptr)\n"
                   "declare i32 @at_command_redirect(ptr, ptr, i64, i32)\n"
                   "declare i32 @at_command_acquire(ptr)\n"
                   "declare i32 @at_command_release(ptr)\n"
                   "declare i32 @at_command_wait(ptr, ptr)\n"
                   "declare i32 @at_command_out(ptr, ptr)\n"
                   "declare i64 @at_command_pid(ptr)\n"
                   "declare i64 @at_command_status(ptr)\n"
                   "declare ptr @at_command_argv(ptr)\n");
    at_ir_emit(&g, "declare ptr @at_port_stats(ptr, ptr)\n"
                   "declare i32 @at_port_open(ptr, ptr, i64, ptr, i64)\n"
                   "declare i32 @at_port_borrow(ptr, i64)\n"
                   "declare i32 @at_port_pipe(ptr, ptr)\n"
                   "declare void @at_port_kind(ptr, ptr)\n"
                   "declare i32 @at_port_release(ptr)\ndeclare i32 @at_port_close(ptr)\n"
                   "declare i32 @at_port_read(ptr, ptr, i64, i32)\n"
                   "declare i32 @at_port_readall(ptr, ptr)\n"
                   "declare i32 @at_port_line(ptr, ptr, ptr)\n"
                   "declare i32 @at_port_write(ptr, ptr, ptr, i64)\n"
                   "declare i64 @at_port_fd(ptr)\ndeclare i32 @at_port_closed(ptr)\n"
                   "@port_mode_r = private constant [2 x i8] c\"r\\00\"\n");
    at_ir_emit(&g, "declare void @at_exception_make(ptr, i32, ptr, i64)\n"
                   "declare void @at_exception_throw(ptr, ptr, i32, i32)\n"
                   "declare void @at_exception_take(ptr)\ndeclare i32 @at_exception_kind()\n"
                   "declare void @at_print_error(ptr)\n");
    at_ir_emit(&g, "declare i32 @at_text_compare(ptr, i64, ptr, i64)\n"
                   "declare i64 @at_text_find(ptr, i64, ptr, i64)\n");
    at_ir_emit(&g,
               "declare ptr @at_dict_new(i64, i64, ptr, ptr, ptr, ptr)\n"
               "declare i64 @at_dict_len(ptr)\ndeclare ptr @at_dict_get(ptr, ptr)\n"
               "declare i32 @at_dict_set(ptr, ptr, ptr)\ndeclare i32 @at_dict_remove(ptr, ptr)\n"
               "declare ptr @at_dict_keys(ptr)\ndeclare ptr @at_dict_values(ptr)\n"
               "declare i64 @at_hash_u64(i64)\ndeclare i64 @at_hash_text(ptr, i64)\n");
    at_ir_emit(
        &g, "declare ptr @at_object_new(i64, ptr)\n"
            "declare void @at_class_prepare(ptr, ptr, i64)\n"
            "declare void @at_class_field_initialized(ptr, i32)\n"
            "declare i32 @at_class_ready(ptr)\n"
            "declare ptr @at_gc_frame()\ndeclare void @at_gc_root(ptr, ptr, ptr)\n"
            "declare i32 @at_format_i64(ptr, i64)\ndeclare i32 @at_format_u64(ptr, i64)\n"
            "declare i32 @at_format_f64(ptr, double)\ndeclare void @at_format_bool(ptr, i32)\n"
            "declare i32 @at_text_chr(ptr, i64)\ndeclare i32 @at_parse_int(ptr, ptr, i64)\n"
            "declare i32 @at_parse_float(ptr, ptr, i64)\n"
            "declare ptr @at_any_new(i32, i64, ptr, ptr, ptr)\ndeclare ptr @at_any_data(ptr, i32)\n"
            "declare i32 @at_format_value(ptr, ptr, ptr)\n"
            "declare i32 @at_any_equal(ptr, ptr)\n"
            "declare void @at_gc_restore(ptr)\ndeclare void @at_gc_mark(ptr)\n"
            "declare void @at_gc_collect()\ndeclare i64 @at_gc_live_bytes()\n"
            "declare i64 @at_gc_live_objects()\ndeclare i64 @at_gc_reclaim()\n"
            "declare ptr @at_closure_new(i64, ptr)\ndeclare ptr @at_closure_cell(ptr, i64)\n"
            "declare ptr @at_buffer_new(i64)\ndeclare i64 @at_buffer_len(ptr)\n"
            "declare ptr @at_bytes_copy(ptr, i64)\ndeclare i32 @at_bytes_equal(ptr, ptr)\n"
            "declare i32 @at_layout_store_bytes(ptr, i64, i64, ptr)\n"
            "declare i32 @at_bytes_decode(ptr, ptr)\n"
            "declare i32 @at_memory_text(ptr, ptr, i64, i64, i32)\n"
            "declare i32 @at_manual_alloc(ptr, i64)\n"
            "declare i32 @at_manual_dealloc(i64)\n"
            "declare i32 @at_region_new(ptr, i64)\n"
            "declare i32 @at_region_move(ptr, ptr)\n"
            "declare i32 @at_region_release(ptr)\n"
            "declare i32 @at_region_length(ptr, ptr)\n"
            "declare i32 @at_region_address(ptr, ptr, i64, i32)\n"
            "declare i32 @at_region_borrow(ptr, ptr, i64, i64, i32)\n"
            "declare i32 @at_region_reborrow(ptr, ptr, i64, i64, i32)\n"
            "declare i32 @at_region_borrow_release(ptr)\n"
            "declare ptr @at_region_borrow_data(ptr)\n"
            "declare i32 @at_system_call(ptr, i64, i64, i64, i64)\n"
            "declare i32 @at_file_read(ptr, ptr, i64)\n"
            "declare i32 @at_file_write(ptr, ptr, i64, ptr)\n"
            "declare ptr @at_buffer_at(ptr, i64)\ndeclare i32 @at_buffer_contains(ptr, i64)\n"
            "declare ptr @at_buffer_data(ptr)\ndeclare i32 @at_caps_have(i32)\n"
            "declare void @at_process_init(i32, ptr)\ndeclare ptr @at_process_args()\n"
            "declare void @at_caps_init()\ndeclare ptr @at_caps_value()\n"
            "declare i64 @at_cap_bits(ptr)\ndeclare ptr @at_cap_path(ptr)\n"
            "declare i64 @at_cap_path_length(ptr)\ndeclare ptr @at_cap_without(ptr, i64)\n"
            "declare i32 @at_cap_scope(ptr, ptr, ptr, i64)\n"
            "declare ptr @at_list_new(i64, ptr)\ndeclare i64 @at_list_len(ptr)\n"
            "declare ptr @at_list_at(ptr, i64)\ndeclare i32 @at_list_append(ptr, ptr)\n"
            "declare i32 @at_text_concat(ptr, ptr, i64, ptr, i64)\n"
            "declare i32 @at_text_repeat(ptr, ptr, i64, i64)\n"
            "declare void @at_text_slice(ptr, ptr, i64, i64, i64)\n"
            "declare i32 @at_text_join(ptr, ptr, i64, ptr)\n"
            "declare i32 @at_text_case(ptr, ptr, i64, i32)\n"
            "declare void @at_text_strip(ptr, ptr, i64)\n"
            "declare i32 @at_text_split(ptr, ptr, i64, ptr, i64)\n"
            "declare i32 @at_text_replace(ptr, ptr, i64, ptr, i64, ptr, i64)\n");
    for (int i = AT_BOOL; i < p->ntypes; i++) {
        if (!at_has_parameter(p, i)) {
            emit_scanner(&g, i);
            if (p->types[i].kind == AT_CLASS) {
                at_ir_class_scanner(&g, i);
            }
            if (at_satisfies_constraint(p, i, AT_CONSTRAINT_HASHABLE)) {
                at_ir_dict_callbacks(&g, i);
            }
        }
    }
    for (int bits = 8; bits <= 64; bits *= 2) {
        for (int sign = 0; sign < 2; sign++) {
            for (int op = 0; op < 3; op++) {
                at_ir_emit(&g, "declare { i%d, i1 } @llvm.%s%s.with.overflow.i%d(i%d, i%d)\n", bits,
                           sign ? "s" : "u",
                           op == 0   ? "add"
                           : op == 1 ? "sub"
                                     : "mul",
                           bits, bits, bits);
            }
        }
    }
    for (int i = 0; i < p->nfunctions; i++) {
        AtFunction *f = &p->functions[i];
        g.f = f;
        if (at_ir_template_function(p, f)) {
            continue; /* Templates have no storage layout and are never emitted. */
        }
        g.id = g.label = g.terminated = 0;
        g.handler_label = 0;
        g.caught = NULL;
        g.cleanup = g.handler_cleanup = g.loop_cleanup = NULL;
        at_ir_emit(&g, "define %s @fn%d(", at_ir_type(&g, f->result), i);
        if (f->lexical_parent) {
            at_ir_emit(&g, "ptr %%environment");
        }
        for (int j = 0; j < f->nparams; j++) {
            at_ir_emit(&g, "%s%s %%arg%d", j || f->lexical_parent ? ", " : "",
                       at_ir_type(&g, f->locals[j].type), j);
        }
        at_ir_emit(&g, ") {\nentry:\n");
        at_ir_emit(&g, "  %%frame = call ptr @at_gc_frame()\n");
        at_ir_function_locals(&g);
        for (int j = 0; j < p->nnodes; j++) {
            AtNode *node = p->nodes[j];
            if (node->function != i) {
                continue;
            }
            if (node->kind == AN_CLOSURE) {
                at_ir_emit(&g, "  %%captures%d = alloca [%d x ptr]\n", node->id,
                           p->functions[node->symbol].capture_count);
            }
            if (node->kind == AN_FOR || node->kind == AN_COMPREHENSION) {
                int range = node->b && node->b->type == AT_RANGE;
                at_ir_emit(&g, "  %%index%d = alloca %s\n", node->id, range ? "i128" : "i64");
                if (p->types[node->b->type].kind == AT_DICT) {
                    at_ir_emit(&g, "  %%snapshot%d = alloca ptr\n", node->id);
                    at_ir_emit(&g, "  store ptr null, ptr %%snapshot%d\n", node->id);
                    char slot[64];
                    snprintf(slot, sizeof slot, "%%snapshot%d", node->id);
                    /* Both snapshots and dictionaries use a pointer root;
                     * allocation metadata selects the actual object scanner. */
                    at_ir_root(&g, slot, node->b->type);
                }
            }
            if (node->kind == AN_EXCEPT) {
                at_ir_emit(&g, "  %%caught%d = alloca %s\n", node->id,
                           at_ir_type(&g, p->exception_type));
                at_ir_emit(&g, "  store %s zeroinitializer, ptr %%caught%d\n",
                           at_ir_type(&g, p->exception_type), node->id);
                char slot[64];
                snprintf(slot, sizeof slot, "%%caught%d", node->id);
                at_ir_root(&g, slot, p->exception_type);
            }
            if (node->kind == AN_WITH) {
                int count = node->a->kind == AN_UNPACK ? 2 : 1;
                for (int i = 0; i < count; i++) {
                    AtNode *owner = count == 2 ? node->a->args[i] : node->a;
                    at_ir_emit(&g, "  %%resource%d = alloca ptr\n", owner->id);
                    at_ir_emit(&g, "  store ptr null, ptr %%resource%d\n", owner->id);
                }
            }
            if (node->kind == AN_CALL &&
                (node->symbol == AT_CALL_REGION || node->symbol == AT_CALL_REGION_MOVE ||
                 node->symbol == AT_CALL_REGION_BORROW)) {
                at_ir_emit(&g, "  %%regionresult%d = alloca ptr\n", node->id);
                at_ir_emit(&g, "  store ptr null, ptr %%regionresult%d\n", node->id);
            }
            if (node->kind == AN_CALL && node->symbol == AT_CALL_LEN &&
                node->args[0]->type == AT_REGION) {
                at_ir_emit(&g, "  %%regionlength%d = alloca i64\n", node->id);
            }
            if (node->kind == AN_INDEX && node->a->type == AT_REGION) {
                at_ir_emit(&g, "  %%regionaddress%d = alloca ptr\n", node->id);
            }
            if ((node->kind == AN_CALL &&
                 (node->symbol == AT_CALL_COMMAND_NEW || node->symbol == AT_CALL_COMMAND_METHOD)) ||
                (node->kind == AN_EXPR && node->op)) {
                at_ir_emit(&g, "  %%commandresult%d = alloca { ptr, i64 }\n", node->id);
                if (node->kind == AN_CALL && node->symbol == AT_CALL_COMMAND_NEW && !node->op) {
                    at_ir_emit(&g, "  %%commandargs%d = alloca [%d x { ptr, i64 }]\n", node->id,
                               node->count);
                }
            }
            if (node->kind == AN_CALL &&
                (node->symbol == AT_CALL_SYSCALL || node->symbol == AT_CALL_MEMORY_TEXT)) {
                /* Output pointers avoid platform-specific C aggregate returns.
                 * Reserve once in the entry block, including calls in loops. */
                at_ir_emit(&g, "  %%nativeresult%d = alloca { ptr, i64 }\n", node->id);
            }
            if ((node->kind == AN_CALL &&
                 (node->symbol == AT_CALL_PORT_OPEN || node->symbol == AT_CALL_PORT_BORROW ||
                  node->symbol == AT_CALL_PORT_METHOD)) ||
                (node->kind == AN_FOR && node->b->type == AT_PORT)) {
                /* A fixed result workspace avoids C aggregate-return ABI
                 * assumptions and is reused, never allocated inside a loop. */
                at_ir_emit(&g, "  %%portresult%d = alloca { ptr, i64 }\n", node->id);
                at_ir_emit(&g, "  %%portpresent%d = alloca i32\n", node->id);
            }
            int kind = p->types[node->type].kind;
            int original_kind = p->types[node->value_type].kind;
            if (node->kind == AN_UNPACK && node->a && node->a->type == AT_RANGE) {
                at_ir_emit(&g, "  %%unpackitem%d = alloca i64\n", node->id);
            }
            if (node->kind == AN_INDEX && node->a->type == AT_RANGE) {
                at_ir_emit(&g, "  %%rangeitem%d = alloca i64\n", node->id);
            }
            if ((node->kind == AN_ARRAY && original_kind == AT_LIST) ||
                node->kind == AN_COMPREHENSION || node->kind == AN_DICT ||
                (node->kind == AN_CALL && node->symbol == AT_CALL_CLASS)) {
                /* A literal becomes an Optional only after its elements are
                 * initialized. Keep its raw collection pointer rooted while
                 * evaluating later elements that can allocate or collect. */
                at_ir_emit(&g, "  %%construction%d = alloca ptr\n", node->id);
                at_ir_emit(&g, "  store ptr null, ptr %%construction%d\n", node->id);
                char slot[64];
                snprintf(slot, sizeof slot, "%%construction%d", node->id);
                at_ir_root(&g, slot, node->value_type);
            }
            at_ir_dict_slots(&g, node);
            if (node->kind == AN_CALL && node->symbol == AT_CALL_PRINT) {
                at_ir_emit(&g, "  %%printtext%d = alloca { ptr, i64 }\n", node->id);
            }
            if (node->kind == AN_CALL && node->symbol == AT_CALL_BYTES_DECODE) {
                at_ir_emit(&g, "  %%decoded%d = alloca { ptr, i64 }\n", node->id);
            }
            if (node->kind == AN_CALL &&
                (node->symbol == AT_CALL_FILE_READ || node->symbol == AT_CALL_FILE_WRITE)) {
                at_ir_emit(&g, "  %%file%d = alloca %s\n", node->id, at_ir_type(&g, node->type));
            }
            if (node->kind == AN_CALL &&
                (node->symbol == AT_CALL_PARSE_INT || node->symbol == AT_CALL_PARSE_FLOAT)) {
                at_ir_emit(&g, "  %%parsed%d = alloca %s\n", node->id, at_ir_type(&g, node->type));
            }
            if (kind == AT_ARRAY || kind == AT_STRUCT || at_slice_kind(kind) || kind == AT_STR ||
                kind == AT_LIST || kind == AT_ANY || kind == AT_DICT || kind == AT_CALLABLE ||
                kind == AT_OPTIONAL || kind == AT_NONE || kind == AT_CLASS || kind == AT_RANGE ||
                at_byte_storage_kind(kind) || kind == AT_CAP || kind == AT_COMMAND ||
                kind == AT_PROCESS || kind == AT_POINTER) {
                at_ir_emit(&g, "  %%storage%d = alloca %s\n", node->id, at_ir_type(&g, node->type));
            }
            if (at_ir_references(&g, node->type)) {
                at_ir_emit(&g, "  %%rootvalue%d = alloca %s\n", node->id,
                           at_ir_type(&g, node->type));
                at_ir_emit(&g, "  store %s zeroinitializer, ptr %%rootvalue%d\n",
                           at_ir_type(&g, node->type), node->id);
                char slot[64];
                snprintf(slot, sizeof slot, "%%rootvalue%d", node->id);
                at_ir_root(&g, slot, node->type);
            }
            if ((node->kind == AN_CALL &&
                 (node->symbol == AT_CALL_LIST_APPEND || node->symbol == AT_CALL_ANY_BOX)) ||
                node->kind == AN_COMPREHENSION ||
                (node->kind == AN_ARRAY && p->types[node->value_type].kind == AT_LIST)) {
                int literal_list =
                    node->kind == AN_COMPREHENSION ||
                    (node->kind == AN_ARRAY && p->types[node->value_type].kind == AT_LIST);
                int element =
                    literal_list ? p->types[node->value_type].element : node->args[0]->type;
                at_ir_emit(&g, "  %%argument%d = alloca %s\n", node->id, at_ir_type(&g, element));
            }
        }
        at_ir_statements(&g, f->body);
        if (!g.terminated) {
            at_ir_emit(&g, "  call void @at_gc_restore(ptr %%frame)\n");
            at_ir_emit(&g, f->result == AT_VOID ? "  ret void\n" : "  unreachable\n");
        }
        at_ir_emit(&g, "failure:\n");
        at_ir_emit(&g, "  call void @at_gc_restore(ptr %%frame)\n");
        if (f->result == AT_VOID) {
            at_ir_emit(&g, "  ret void\n");
        } else {
            at_ir_emit(&g, "  ret %s zeroinitializer\n", at_ir_type(&g, f->result));
        }
        at_ir_emit(&g, "}\n");
        if (!f->lexical_parent) {
            at_ir_callable_adapter(&g, i);
        }
        if (f->method_owner) {
            at_ir_bound_adapter(&g, i);
        }
    }
    g.handler_label = 0;
    g.id = g.label = g.terminated = 0;
    at_ir_emit(&g, "define i32 @main(i32 %%argc, ptr %%argv) {\nentry:\n"
                   "  call void @at_process_init(i32 %%argc, ptr %%argv)\n"
                   "  call void @at_caps_init()\n");
    /* Roots live in the process entry frame, below every initializer/call
     * frame. Register all slots before the first allocation, including globals
     * initialized later; their zero values are safe for generated scanners. */
    for (int i = 0; i < p->nglobals; i++) {
        if (at_ir_references(&g, p->globals[i].type)) {
            char slot[64];
            snprintf(slot, sizeof slot, "@global%d", i);
            at_ir_root(&g, slot, p->globals[i].type);
        }
    }
    for (int i = 0; i < p->ninitializers; i++) {
        at_ir_emit(&g, "  call void @fn%d()\n", p->initialization_order[i]);
        at_ir_propagate(&g);
    }
    if (tests) {
        for (int i = 0; i < p->nfunctions; i++) {
            AtFunction *f = &p->functions[i];
            if (!f->generic_count && !f->lexical_parent && f->template_id < 0 &&
                !strncmp(f->name, "test_", 5) && !f->nparams && f->result == AT_VOID) {
                at_ir_emit(&g, "  call void @fn%d()\n", i);
                /* A failed assertion must stop later tests. Running another
                 * test with at_failed set can execute side effects before its
                 * first call-site guard notices the earlier failure. */
                at_ir_propagate(&g);
            }
        }
        at_ir_emit(&g,
                   "  br label %%failure\nfailure:\n  %%status = call i32 @at_finish()\n  ret i32 "
                   "%%status\n}\n");
    } else {
        AtFunction *f = &p->functions[main];
        if (f->result == AT_VOID) {
            at_ir_emit(&g,
                       "  call void @fn%d()\n  br label %%failure\n"
                       "failure:\n  %%status = call i32 @at_finish()\n  ret i32 %%status\n}\n",
                       main);
        } else {
            at_ir_emit(
                &g,
                "  %%result = call i64 @fn%d()\n  %%status = call i32 @at_finish()\n  %%bad = "
                "icmp ne i32 %%status, 0\n  %%code = trunc i64 %%result to i32\n  %%exit = select "
                "i1 %%bad, i32 %%status, i32 %%code\n  ret i32 %%exit\n"
                "failure:\n  %%failure_status = call i32 @at_finish()\n  ret i32 "
                "%%failure_status\n}\n",
                main);
        }
    }
    return ferror(out) || g.failed ? -1 : 0;
}
