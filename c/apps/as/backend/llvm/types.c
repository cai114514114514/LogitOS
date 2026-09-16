/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include "runtime/type.h"
#include <string.h>

static void array_definition(Gen *g, int id, unsigned char *emitted)
{
    AtType *type = &g->p->types[id];
    if (type->kind != AT_ARRAY || emitted[id] || at_has_parameter(g->p, id)) {
        return;
    }
    emitted[id] = 1;
    array_definition(g, type->element, emitted);
    at_ir_emit(g, "%%A%d = type [%d x %s]\n", id, type->count, at_ir_type(g, type->element));
}

void at_ir_array_definitions(Gen *g)
{
    /* LLVM permits forward references to identified structs, but not array
     * aliases. A struct is registered before its fields are parsed, so table
     * order can place the struct before an Array field's type. Emit aliases
     * in element dependency order before any struct uses them. */
    unsigned char emitted[AT_TYPES] = {0};
    for (int id = AT_BOOL; id < g->p->ntypes; id++) {
        array_definition(g, id, emitted);
    }
}

static void constant_text(Gen *g, const char *text)
{
    at_ir_emit(g, "private constant [%zu x i8] c\"", strlen(text) + 1);
    for (const unsigned char *byte = (const unsigned char *)text; *byte; byte++) {
        at_ir_emit(g, "\\%02X", *byte);
    }
    at_ir_emit(g, "\\00\"\n");
}

static int native_kind(Gen *g, int id)
{
    if (at_integer(g->p, id)) {
        return AT_NATIVE_INTEGER;
    }
    if (id == g->p->exception_type) {
        return AT_NATIVE_EXCEPTION;
    }
    switch (g->p->types[id].kind) {
    case AT_NONE:
        return AT_NATIVE_NONE;
    case AT_POINTER:
        return AT_NATIVE_POINTER;
    case AT_OPTIONAL:
        return AT_NATIVE_OPTIONAL;
    case AT_BOOL:
        return AT_NATIVE_BOOL;
    case AT_F32:
    case AT_F64:
        return AT_NATIVE_FLOAT;
    case AT_STR:
        return AT_NATIVE_TEXT;
    case AT_LIST:
        return AT_NATIVE_LIST;
    case AT_RANGE:
        return AT_NATIVE_RANGE;
    case AT_BUFFER:
        return AT_NATIVE_BUFFER;
    case AT_LAYOUT:
        return AT_NATIVE_LAYOUT;
    case AT_BYTES:
        return AT_NATIVE_BYTES;
    case AT_CAP:
        return AT_NATIVE_CAP;
    case AT_COMMAND:
    case AT_PROCESS:
        return AT_NATIVE_COMMAND;
    case AT_DICT:
        return AT_NATIVE_DICT;
    case AT_ARRAY:
        return AT_NATIVE_ARRAY;
    case AT_SLICE:
    case AT_MUT_SLICE:
        return AT_NATIVE_SLICE;
    case AT_STRUCT:
        return AT_NATIVE_STRUCT;
    case AT_CLASS:
        return AT_NATIVE_CLASS;
    case AT_ANY:
        return AT_NATIVE_ANY;
    case AT_CALLABLE:
        return AT_NATIVE_CALLABLE;
    default:
        return 0;
    }
}

void at_ir_type_descriptions(Gen *g)
{
    /* This is the layout of runtime/type.h, containing target-sized offsets
     * and pointers. LLVM computes each language type's padding/stride below.
     * Metadata is immutable and contains no managed references. */
    at_ir_emit(g, "%%AtNativeType = type { i32, i32, i32, i32, i64, ptr, ptr, ptr, ptr }\n"
                  "%%AtNativeField = type { i64, ptr, ptr }\n");
    for (int id = AT_BOOL; id < g->p->ntypes; id++) {
        if (at_has_parameter(g->p, id)) {
            continue;
        }
        AtType *type = &g->p->types[id];
        int structure =
            type->kind == AT_STRUCT || type->kind == AT_CLASS || type->kind == AT_LAYOUT;
        int container = type->kind == AT_ARRAY || at_slice_kind(type->kind) ||
                        type->kind == AT_LIST || type->kind == AT_DICT ||
                        type->kind == AT_OPTIONAL || type->kind == AT_POINTER;
        if (type->kind == AT_OPTIONAL) {
            at_ir_emit(g,
                       "@fields%d = private constant [1 x %%AtNativeField] [%%AtNativeField { "
                       "i64 ptrtoint (ptr getelementptr (%s, ptr null, i32 0, i32 1) to i64), "
                       "ptr @type%d, ptr null }]\n",
                       id, at_ir_type(g, id), type->element);
        }
        if (structure) {
            at_ir_emit(g, "@typename%d = ", id);
            constant_text(g, type->kind == AT_LAYOUT ? type->layout_name : type->name);
            for (int field = 0; field < type->count; field++) {
                at_ir_emit(g, "@fieldname%d_%d = ", id, field);
                constant_text(g, type->names[field]);
                if (type->kind == AT_LAYOUT && type->fields[field] == AT_BYTES) {
                    /* Inline spans have no Bytes object header. Their private
                     * descriptor carries the fixed width for formatting only;
                     * language reads still construct ordinary Bytes snapshots. */
                    at_ir_emit(g,
                               "@layoutspan%d_%d = private constant %%AtNativeType { "
                               "i32 %d, i32 8, i32 0, i32 %d, i64 %d, "
                               "ptr null, ptr null, ptr null, ptr null }\n",
                               id, field, AT_NATIVE_BYTE_SPAN, type->widths[field],
                               type->widths[field]);
                }
            }
            at_ir_emit(g, "@fields%d = private constant [%d x %%AtNativeField] [", id, type->count);
            for (int field = 0; field < type->count; field++) {
                if (type->kind == AT_LAYOUT) {
                    at_ir_emit(g, "%s%%AtNativeField { i64 %d, ptr ", field ? ", " : "",
                               type->offsets[field]);
                    if (type->fields[field] == AT_BYTES) {
                        at_ir_emit(g, "@layoutspan%d_%d", id, field);
                    } else {
                        at_ir_emit(g, "@type%d", type->fields[field]);
                    }
                    at_ir_emit(g, ", ptr @fieldname%d_%d }", id, field);
                    continue;
                }
                at_ir_emit(g,
                           "%s%%AtNativeField { i64 ptrtoint (ptr getelementptr (%s, ptr null, "
                           "i32 0, i32 %d) to i64), ptr @type%d, ptr @fieldname%d_%d }",
                           field ? ", " : "",
                           type->kind == AT_CLASS ? at_ir_class_layout(g, id) : at_ir_type(g, id),
                           field + (type->kind == AT_CLASS ? AT_CLASS_HEADER_COUNT : 0),
                           type->fields[field], id, field);
            }
            at_ir_emit(g, "]\n");
        }
        at_ir_emit(g,
                   "@type%d = private constant %%AtNativeType { i32 %d, i32 %d, i32 %d, i32 %d, "
                   "i64 ptrtoint (ptr getelementptr (%s, ptr null, i32 1) to i64), ",
                   id, native_kind(g, id), at_bits(g->p, id), at_signed(g->p, id), type->count,
                   at_ir_type(g, id));
        if (container) {
            at_ir_emit(g, "ptr @type%d, ", type->element);
        } else {
            at_ir_emit(g, "ptr null, ");
        }
        if (type->kind == AT_DICT) {
            at_ir_emit(g, "ptr @type%d, ", type->key);
        } else {
            at_ir_emit(g, "ptr null, ");
        }
        if (structure) {
            at_ir_emit(g, "ptr @fields%d, ptr @typename%d }\n", id, id);
        } else if (type->kind == AT_OPTIONAL) {
            at_ir_emit(g, "ptr @fields%d, ptr null }\n", id);
        } else {
            at_ir_emit(g, "ptr null, ptr null }\n");
        }
    }
}

void at_ir_format_value(Gen *g, AtNode *site, AtNode *source, Val value, const char *out)
{
    at_ir_emit(g, "  store %s %s, ptr %%storage%d\n", at_ir_type(g, value.type), value.text,
               source->id);
    Val status = at_ir_temp(g, AT_I32);
    at_ir_emit(g, "  %s = call i32 @at_format_value(ptr %s, ptr @type%d, ptr %%storage%d)\n",
               status.text, out, value.type, source->id);
    at_ir_allocation(g, status, site, 0);
}
