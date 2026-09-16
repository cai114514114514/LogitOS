/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Call resolution belongs to the checker. Lowering uses its resolved symbol
 * and concrete argument types; it must not guess again from the source name.
 * The result register is allocated by expression() to preserve SSA ordering. */
static Val text_component(Gen *g, Val text, int component)
{
    Val result = at_ir_temp(g, component ? AT_I64 : 0);
    at_ir_emit(g, "  %s = extractvalue %s %s, %d\n", result.text, at_ir_type(g, AT_STR), text.text,
               component);
    return result;
}

static Val emit_text_method(Gen *g, AtNode *node, Val result)
{
    Val receiver = at_ir_expression(g, node->a->a);
    Val text = text_component(g, receiver, 0), length = text_component(g, receiver, 1);
    Val args[2];
    for (int i = 0; i < node->count; i++) {
        args[i] = at_ir_expression(g, node->args[i]);
    }
    Val status = at_ir_temp(g, AT_I32);
    int allocates = 1;
    switch (node->symbol) {
    case AT_CALL_STR_JOIN:
        at_ir_emit(g, "  %s = call i32 @at_text_join(ptr %%storage%d, ptr %s, i64 %s, ptr %s)\n",
                   status.text, node->id, text.text, length.text, args[0].text);
        break;
    case AT_CALL_STR_CASE:
        at_ir_emit(g, "  %s = call i32 @at_text_case(ptr %%storage%d, ptr %s, i64 %s, i32 %d)\n",
                   status.text, node->id, text.text, length.text, node->op);
        break;
    case AT_CALL_STR_STRIP:
        allocates = 0;
        at_ir_emit(g, "  call void @at_text_strip(ptr %%storage%d, ptr %s, i64 %s)\n", node->id,
                   text.text, length.text);
        break;
    case AT_CALL_STR_SPLIT:
    case AT_CALL_STR_REPLACE: {
        Val separator = at_ir_value(0, "null"), size = at_ir_value(AT_I64, "0");
        if (node->count) {
            separator = text_component(g, args[0], 0);
            size = text_component(g, args[0], 1);
            Val empty = at_ir_temp(g, AT_BOOL);
            at_ir_emit(g, "  %s = icmp eq i64 %s, 0\n", empty.text, size.text);
            at_ir_guard(g, empty, node, AT_E_VALUE);
        }
        if (node->symbol == AT_CALL_STR_SPLIT) {
            at_ir_emit(
                g,
                "  %s = call i32 @at_text_split(ptr %%storage%d, ptr %s, i64 %s, ptr %s, i64 %s)\n",
                status.text, node->id, text.text, length.text, separator.text, size.text);
        } else {
            Val replacement = text_component(g, args[1], 0);
            Val replacement_size = text_component(g, args[1], 1);
            at_ir_emit(
                g,
                "  %s = call i32 @at_text_replace(ptr %%storage%d, ptr %s, i64 %s, ptr %s, i64 "
                "%s, ptr %s, i64 %s)\n",
                status.text, node->id, text.text, length.text, separator.text, size.text,
                replacement.text, replacement_size.text);
        }
        break;
    }
    case AT_CALL_STR_SUB:
        /* A2 sub() clamps negative bounds to zero; the newer slice() uses
         * negative offsets from the end. Keep the two explicit APIs distinct. */
        allocates = 0;
        for (int i = 0; i < 2; i++) {
            Val negative = at_ir_temp(g, AT_BOOL), bounded = at_ir_temp(g, AT_I64);
            at_ir_emit(g, "  %s = icmp slt i64 %s, 0\n", negative.text, args[i].text);
            at_ir_emit(g, "  %s = select i1 %s, i64 0, i64 %s\n", bounded.text, negative.text,
                       args[i].text);
            args[i] = bounded;
        }
        at_ir_emit(g,
                   "  call void @at_text_slice(ptr %%storage%d, ptr %s, i64 %s, i64 %s, i64 %s)\n",
                   node->id, text.text, length.text, args[0].text, args[1].text);
        break;
    }
    if (allocates) {
        at_ir_allocation(g, status, node, 0);
    }
    at_ir_emit(g, "  %s = load %s, ptr %%storage%d\n", result.text, at_ir_type(g, node->type),
               node->id);
    return result;
}

static Val emit_scalar_conversion(Gen *g, AtNode *node, Val result)
{
    Val source = at_ir_expression(g, node->args[0]);
    if (node->symbol == AT_CALL_STR && source.type != AT_STR && source.type != AT_BOOL &&
        !at_integer(g->p, source.type) && source.type != AT_F32 && source.type != AT_F64) {
        char out[64];
        snprintf(out, sizeof out, "%%storage%d", node->id);
        at_ir_format_value(g, node, node->args[0], source, out);
        at_ir_emit(g, "  %s = load %s, ptr %s\n", result.text, at_ir_type(g, AT_STR), out);
        return result;
    }
    if (node->symbol == AT_CALL_F64_BITS) {
        Val real = at_ir_convert(g, source, AT_F64, node);
        at_ir_emit(g, "  %s = bitcast double %s to i64\n", result.text, real.text);
        return result;
    }
    if (node->symbol == AT_CALL_ORD || node->symbol == AT_CALL_PARSE_INT ||
        node->symbol == AT_CALL_PARSE_FLOAT) {
        Val pointer = text_component(g, source, 0), length = text_component(g, source, 1);
        Val status = at_ir_temp(g, AT_I32), bad = at_ir_temp(g, AT_BOOL);
        if (node->symbol == AT_CALL_ORD) {
            at_ir_emit(g, "  %s = icmp eq i64 %s, 0\n", bad.text, length.text);
            at_ir_guard(g, bad, node, AT_E_VALUE);
            Val byte = at_ir_temp(g, AT_U8);
            at_ir_emit(g, "  %s = load i8, ptr %s\n", byte.text, pointer.text);
            at_ir_emit(g, "  %s = zext i8 %s to i64\n", result.text, byte.text);
            return result;
        }
        at_ir_emit(g, "  %s = call i32 @%s(ptr %%parsed%d, ptr %s, i64 %s)\n", status.text,
                   node->symbol == AT_CALL_PARSE_INT ? "at_parse_int" : "at_parse_float", node->id,
                   pointer.text, length.text);
        if (node->symbol == AT_CALL_PARSE_FLOAT) {
            Val out_of_memory = at_ir_temp(g, AT_BOOL);
            at_ir_emit(g, "  %s = icmp slt i32 %s, 0\n", out_of_memory.text, status.text);
            at_ir_guard(g, out_of_memory, node, AT_E_MEMORY);
        }
        at_ir_emit(g, "  %s = icmp eq i32 %s, 0\n", bad.text, status.text);
        at_ir_guard(g, bad, node, AT_E_CONVERSION);
        at_ir_emit(g, "  %s = load %s, ptr %%parsed%d\n", result.text, at_ir_type(g, node->type),
                   node->id);
        return result;
    }
    if (node->symbol == AT_CALL_STR && source.type == AT_STR) {
        return source;
    }
    Val status = at_ir_temp(g, AT_I32);
    if (node->symbol == AT_CALL_CHR) {
        Val invalid = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = icmp ugt i64 %s, 255\n", invalid.text, source.text);
        at_ir_guard(g, invalid, node, AT_E_VALUE);
        at_ir_emit(g, "  %s = call i32 @at_text_chr(ptr %%storage%d, i64 %s)\n", status.text,
                   node->id, source.text);
    } else if (source.type == AT_BOOL) {
        Val boolean = at_ir_temp(g, AT_I32);
        at_ir_emit(g, "  %s = zext i1 %s to i32\n", boolean.text, source.text);
        at_ir_emit(g, "  call void @at_format_bool(ptr %%storage%d, i32 %s)\n", node->id,
                   boolean.text);
    } else {
        int format = at_integer(g->p, source.type)
                         ? (at_signed(g->p, source.type) ? AT_I64 : AT_U64)
                         : AT_F64;
        Val scalar = at_ir_convert(g, source, format, node);
        const char *name = format == AT_I64   ? "at_format_i64"
                           : format == AT_U64 ? "at_format_u64"
                                              : "at_format_f64";
        at_ir_emit(g, "  %s = call i32 @%s(ptr %%storage%d, %s %s)\n", status.text, name, node->id,
                   at_ir_type(g, format), scalar.text);
    }
    if (source.type != AT_BOOL) {
        at_ir_allocation(g, status, node, 0);
    }
    at_ir_emit(g, "  %s = load %s, ptr %%storage%d\n", result.text, at_ir_type(g, AT_STR),
               node->id);
    return result;
}

Val at_ir_call(Gen *g, AtNode *n, Val v)
{
    if (n->symbol == AT_CALL_LAYOUT_NEW) {
        at_ir_emit(g, "  %s = call ptr @at_buffer_new(i64 %d)\n", v.text,
                   g->p->types[n->type].layout_size);
        at_ir_allocation(g, v, n, 1);
        return v;
    }
    if (n->symbol == AT_CALL_SYSCALL) {
        return at_ir_system(g, n, v);
    }
    if (n->symbol == AT_CALL_MEMORY_TEXT) {
        return at_ir_memory_text(g, n, v);
    }
    if (n->symbol == AT_CALL_PORT_OPEN || n->symbol == AT_CALL_PORT_BORROW ||
        n->symbol == AT_CALL_PORT_METHOD || n->symbol == AT_CALL_PORT_STATS) {
        return at_ir_port(g, n, v);
    }
    if (n->symbol == AT_CALL_COMMAND_NEW || n->symbol == AT_CALL_COMMAND_METHOD) {
        return at_ir_command(g, n, v);
    }
    if (n->symbol == AT_CALL_REGION || n->symbol == AT_CALL_REGION_MOVE) {
        return at_ir_region(g, n, v);
    }
    if (n->symbol == AT_CALL_REGION_BORROW) {
        return at_ir_region_borrow(g, n, v);
    }
    if (n->symbol == AT_CALL_BYTES_DECODE) {
        return at_ir_bytes_decode(g, n, v);
    }
    if (n->symbol == AT_CALL_FILE_READ || n->symbol == AT_CALL_FILE_WRITE) {
        return at_ir_file(g, n, v);
    }
    if (n->symbol == AT_CALL_BYTES) {
        return at_ir_bytes_new(g, n, v);
    }
    if (n->symbol == AT_CALL_ADDR || n->symbol == AT_CALL_PEEK || n->symbol == AT_CALL_POKE ||
        n->symbol == AT_CALL_POINTER || n->symbol == AT_CALL_ALLOC ||
        n->symbol == AT_CALL_DEALLOC) {
        return at_ir_memory(g, n, v);
    }
    if (n->symbol <= AT_CALL_CAPS && n->symbol >= AT_CALL_CAP_SCOPE) {
        return at_ir_capability(g, n, v);
    }
    if (n->symbol == AT_CALL_BUFFER) {
        return at_ir_buffer_new(g, n, v);
    }
    if (n->symbol == AT_CALL_RANGE) {
        return at_ir_range_new(g, n, v);
    }
    if (n->symbol == AT_CALL_CLASS) {
        return at_ir_class_construct(g, n, v);
    }
    if (n->symbol == AT_CALL_INDIRECT) {
        return at_ir_indirect_call(g, n, v);
    }
    if (n->symbol <= AT_CALL_DICT_GET && n->symbol >= AT_CALL_DICT_REMOVE) {
        return at_ir_dict_call(g, n, v);
    }
    if (n->symbol <= AT_CALL_STR && n->symbol >= AT_CALL_F64_BITS) {
        return emit_scalar_conversion(g, n, v);
    }
    if (n->symbol == AT_CALL_ANY_BOX) {
        Val source = at_ir_expression(g, n->args[0]);
        if (source.type == AT_ANY) {
            return source;
        }
        at_ir_emit(g, "  store %s %s, ptr %%argument%d\n", at_ir_type(g, source.type), source.text,
                   n->id);
        at_ir_emit(g,
                   "  %s = call ptr @at_any_new(i32 %d, i64 ptrtoint (ptr getelementptr "
                   "(%s, ptr null, i32 1) to i64), ptr @scan%d, ptr %%argument%d, ptr @type%d)\n",
                   v.text, source.type, at_ir_type(g, source.type), source.type, n->id,
                   source.type);
        at_ir_allocation(g, v, n, 1);
        return v;
    }
    if (n->symbol == AT_CALL_ANY_CAST || n->symbol == AT_CALL_ANY_TEST) {
        Val source = at_ir_expression(g, n->args[0]);
        int target = n->a->type;
        if (target == AT_ANY) {
            return n->symbol == AT_CALL_ANY_TEST ? at_ir_value(AT_BOOL, "true") : source;
        }
        Val data = at_ir_temp(g, 0);
        at_ir_emit(g, "  %s = call ptr @at_any_data(ptr %s, i32 %d)\n", data.text, source.text,
                   target);
        if (n->symbol == AT_CALL_ANY_TEST) {
            at_ir_emit(g, "  %s = icmp ne ptr %s, null\n", v.text, data.text);
        } else {
            Val bad = at_ir_temp(g, AT_BOOL);
            at_ir_emit(g, "  %s = icmp eq ptr %s, null\n", bad.text, data.text);
            at_ir_guard(g, bad, n, AT_E_TYPE);
            at_ir_emit(g, "  %s = load %s, ptr %s\n", v.text, at_ir_type(g, target), data.text);
        }
        return v;
    }
    if (n->symbol == AT_CALL_WRAPPING_SHL) {
        Val left = at_ir_expression(g, n->args[0]), count = at_ir_expression(g, n->args[1]);
        return at_ir_shift(g, T_SHL, left, count, n, 1);
    }
    if (n->symbol <= AT_CALL_STR_JOIN && n->symbol >= AT_CALL_STR_SUB) {
        return emit_text_method(g, n, v);
    }
    if (n->symbol == AT_CALL_GC_COLLECT) {
        at_ir_emit(g, "  call void @at_gc_collect()\n");
        return at_ir_value(AT_VOID, "");
    }
    if (n->symbol == AT_CALL_GC_BYTES || n->symbol == AT_CALL_GC_OBJECTS ||
        n->symbol == AT_CALL_GC_RECLAIM) {
        const char *function = n->symbol == AT_CALL_GC_BYTES     ? "at_gc_live_bytes"
                               : n->symbol == AT_CALL_GC_OBJECTS ? "at_gc_live_objects"
                                                                 : "at_gc_reclaim";
        at_ir_emit(g, "  %s = call i64 @%s()\n", v.text, function);
        return v;
    }
    if (n->symbol == AT_CALL_ARGS) {
        at_ir_emit(g, "  %s = call ptr @at_process_args()\n", v.text);
        at_ir_allocation(g, v, n, 1);
        return v;
    }
    if (n->symbol == AT_CALL_LIST_APPEND) {
        Val list = at_ir_expression(g, n->a->a);
        Val item = at_ir_expression(g, n->args[0]);
        at_ir_emit(g, "  store %s %s, ptr %%argument%d\n", at_ir_type(g, item.type), item.text,
                   n->id);
        Val status = at_ir_temp(g, AT_I32);
        at_ir_emit(g, "  %s = call i32 @at_list_append(ptr %s, ptr %%argument%d)\n", status.text,
                   list.text, n->id);
        at_ir_allocation(g, status, n, 0);
        return at_ir_value(AT_VOID, "");
    }
    if (n->symbol == AT_CALL_STR_SLICE) {
        Val text = at_ir_expression(g, n->a->a);
        Val start = at_ir_expression(g, n->args[0]), stop = at_ir_expression(g, n->args[1]);
        Val pointer = at_ir_temp(g, 0), length = at_ir_temp(g, AT_I64);
        at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", pointer.text, at_ir_type(g, AT_STR),
                   text.text);
        at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", length.text, at_ir_type(g, AT_STR),
                   text.text);
        at_ir_emit(g,
                   "  call void @at_text_slice(ptr %%storage%d, ptr %s, i64 %s, i64 %s, i64 %s)\n",
                   n->id, pointer.text, length.text, start.text, stop.text);
        at_ir_emit(g, "  %s = load %s, ptr %%storage%d\n", v.text, at_ir_type(g, AT_STR), n->id);
        return v;
    }
    if (n->symbol == AT_CALL_STR_FIND) {
        Val text = at_ir_expression(g, n->a->a);
        Val needle = at_ir_expression(g, n->args[0]);
        return at_ir_text_operation(g, text, needle, 1);
    }
    if (n->symbol == AT_CALL_EXCEPTION) {
        Val message = at_ir_expression(g, n->args[0]);
        Val pointer = at_ir_temp(g, 0), length = at_ir_temp(g, AT_I64);
        at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", pointer.text, at_ir_type(g, AT_STR),
                   message.text);
        at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", length.text, at_ir_type(g, AT_STR),
                   message.text);
        at_ir_emit(g, "  call void @at_exception_make(ptr %%storage%d, i32 %d, ptr %s, i64 %s)\n",
                   n->id, n->op, pointer.text, length.text);
        at_ir_emit(g, "  %s = load %s, ptr %%storage%d\n", v.text, at_ir_type(g, n->type), n->id);
        return v;
    }
    if (n->symbol == AT_CALL_PRINT) {
        Val arguments[AT_ARGS];
        for (int i = 0; i < n->count; i++) {
            arguments[i] = at_ir_expression(g, n->args[i]);
        }
        for (int i = 0; i < n->count; i++) {
            if (i) {
                at_ir_emit(g, "  call void @at_print_sep(i32 0)\n");
            }
            Val a = arguments[i];
            if (at_integer(g->p, a.type)) {
                int sign = at_signed(g->p, a.type);
                if (at_bits(g->p, a.type) < 64) {
                    Val wide = at_ir_temp(g, sign ? AT_I64 : AT_U64);
                    at_ir_emit(g, "  %s = %s %s %s to i64\n", wide.text, sign ? "sext" : "zext",
                               at_ir_type(g, a.type), a.text);
                    a = wide;
                }
                at_ir_emit(g, "  call void @at_print_%s(i64 %s)\n", sign ? "i64" : "u64", a.text);
            } else if (a.type == AT_F32 || a.type == AT_F64) {
                if (a.type == AT_F32) {
                    Val wide = at_ir_temp(g, AT_F64);
                    at_ir_emit(g, "  %s = fpext float %s to double\n", wide.text, a.text);
                    a = wide;
                }
                at_ir_emit(g, "  call void @at_print_f64(double %s)\n", a.text);
            } else if (a.type == AT_BOOL) {
                Val b = at_ir_temp(g, AT_I32);
                at_ir_emit(g, "  %s = zext i1 %s to i32\n  call void @at_print_bool(i32 %s)\n",
                           b.text, a.text, b.text);
            } else if (a.type == g->p->exception_type) {
                at_ir_emit(g, "  store %s %s, ptr %%storage%d\n", at_ir_type(g, a.type), a.text,
                           n->args[i]->id);
                at_ir_emit(g, "  call void @at_print_error(ptr %%storage%d)\n", n->args[i]->id);
            } else if (a.type == AT_STR) {
                Val ptr = at_ir_temp(g, 0), len = at_ir_temp(g, AT_I64);
                at_ir_emit(
                    g,
                    "  %s = extractvalue %s %s, 0\n  %s = extractvalue %s %s, 1\n  call void "
                    "@at_print_str(ptr %s, i64 %s)\n",
                    ptr.text, at_ir_type(g, a.type), a.text, len.text, at_ir_type(g, a.type),
                    a.text, ptr.text, len.text);
            } else {
                char out[64];
                snprintf(out, sizeof out, "%%printtext%d", n->id);
                at_ir_format_value(g, n, n->args[i], a, out);
                Val text = at_ir_temp(g, AT_STR);
                at_ir_emit(g, "  %s = load %s, ptr %s\n", text.text, at_ir_type(g, AT_STR), out);
                Val pointer = text_component(g, text, 0), length = text_component(g, text, 1);
                at_ir_emit(g, "  call void @at_print_str(ptr %s, i64 %s)\n", pointer.text,
                           length.text);
            }
        }
        at_ir_emit(g, "  call void @at_print_sep(i32 1)\n");
        return at_ir_value(AT_VOID, "");
    }
    if (n->symbol == AT_CALL_LEN) {
        Val a = at_ir_expression(g, n->args[0]);
        if (a.type == AT_REGION) {
            Val status = at_ir_temp(g, AT_I32);
            at_ir_emit(g, "  %s = call i32 @at_region_length(ptr %%regionlength%d, ptr %s)\n",
                       status.text, n->id, a.text);
            at_ir_runtime_status(g, status, n);
            at_ir_emit(g, "  %s = load i64, ptr %%regionlength%d\n", v.text, n->id);
            return v;
        }
        if (at_byte_storage_kind(g->p->types[a.type].kind)) {
            at_ir_emit(g, "  %s = call i64 @at_buffer_len(ptr %s)\n", v.text, a.text);
            return v;
        }
        if (a.type == AT_RANGE) {
            return at_ir_range_length(g, n, a, v);
        }
        if (g->p->types[a.type].kind == AT_DICT) {
            at_ir_emit(g, "  %s = call i64 @at_dict_len(ptr %s)\n", v.text, a.text);
            return v;
        }
        if (g->p->types[a.type].kind == AT_LIST) {
            at_ir_emit(g, "  %s = call i64 @at_list_len(ptr %s)\n", v.text, a.text);
            return v;
        }
        if (g->p->types[a.type].kind == AT_ARRAY) {
            snprintf(v.text, sizeof v.text, "%d", g->p->types[a.type].count);
            return v;
        }
        at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", v.text, at_ir_type(g, a.type), a.text);
        return v;
    }
    if (n->symbol <= AT_CALL_STRUCT_BASE) {
        Val old = at_ir_value(n->type, "zeroinitializer");
        for (int i = 0; i < n->count; i++) {
            Val a = at_ir_expression(g, n->args[i]), next = at_ir_temp(g, n->type);
            at_ir_emit(g, "  %s = insertvalue %s %s, %s %s, %d\n", next.text,
                       at_ir_type(g, n->type), old.text, at_ir_type(g, a.type), a.text, i);
            old = next;
        }
        return old;
    }
    if (n->symbol <= AT_CALL_CAST_BASE) {
        return at_ir_convert(g, at_ir_expression(g, n->args[0]), n->type, n);
    }
    if (n->symbol >= AT_CALL_WRAPPING_MUL && n->symbol <= AT_CALL_WRAPPING_ADD) {
        Val a = at_ir_expression(g, n->args[0]), b = at_ir_expression(g, n->args[1]);
        int operation = T_STAR;
        if (n->symbol == AT_CALL_WRAPPING_ADD) {
            operation = T_PLUS;
        } else if (n->symbol == AT_CALL_WRAPPING_SUB) {
            operation = T_MINUS;
        }
        return at_ir_numeric(g, operation, a, b, n, 1);
    }
    if (n->symbol < 0) {
        return v;
    }
    AtFunction *f = &g->p->functions[n->symbol];
    Val args[AT_ARGS];
    for (int i = 0; i < n->count; i++) {
        args[i] = at_ir_argument(g, n->args[i], f->locals[i].type);
    }
    if (f->result != AT_VOID) {
        at_ir_emit(g, "  %s = ", v.text);
    } else {
        at_ir_emit(g, "  ");
    }
    at_ir_emit(g, "call %s @fn%d(", at_ir_type(g, f->result), n->symbol);
    for (int i = 0; i < n->count; i++) {
        at_ir_emit(g, "%s%s %s", i ? ", " : "", at_ir_type(g, args[i].type), args[i].text);
    }
    at_ir_emit(g, ")\n");
    at_ir_propagate(g);
    return v;
}
