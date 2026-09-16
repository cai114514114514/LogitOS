/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Building the text uses malloc, which is not a GC safepoint. Only publishing
 * the final immutable string allocates in the managed heap. The generated
 * caller roots the original value across that final allocation. */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} TextBuilder;

static int append(TextBuilder *builder, const char *text, size_t length)
{
    if (length > (size_t)INT64_MAX - 1 - builder->length) {
        return 0;
    }
    size_t needed = builder->length + length + 1;
    if (needed > builder->capacity) {
        size_t capacity = builder->capacity ? builder->capacity : 64;
        while (capacity < needed) {
            if (capacity > (size_t)INT64_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        char *data = realloc(builder->data, capacity);
        if (!data) {
            return 0;
        }
        builder->data = data;
        builder->capacity = capacity;
    }
    if (length) {
        memcpy(builder->data + builder->length, text, length);
    }
    builder->length += length;
    builder->data[builder->length] = 0;
    return 1;
}

static int literal(TextBuilder *builder, const char *text)
{
    return append(builder, text, strlen(text));
}

static int format(TextBuilder *builder, const AtNativeType *type, const void *value, int depth,
                  int repr);

static int integer(TextBuilder *builder, const AtNativeType *type, const void *value)
{
    int64_t signed_value = 0;
    uint64_t unsigned_value = 0;
    /* Typed loads through memcpy avoid both alignment assumptions and
     * endian-dependent partial copies into an eight-byte accumulator. */
#define READ_INTEGER(width)                                                                        \
    case width: {                                                                                  \
        int##width##_t signed_part;                                                                \
        uint##width##_t unsigned_part;                                                             \
        memcpy(&signed_part, value, sizeof signed_part);                                           \
        memcpy(&unsigned_part, value, sizeof unsigned_part);                                       \
        signed_value = signed_part;                                                                \
        unsigned_value = unsigned_part;                                                            \
        break;                                                                                     \
    }
    switch (type->bits) {
        READ_INTEGER(8)
        READ_INTEGER(16)
        READ_INTEGER(32)
        READ_INTEGER(64)
    default:
        return 0;
    }
#undef READ_INTEGER
    char text[32];
    int length = type->is_signed
                     ? snprintf(text, sizeof text, "%lld", (long long)signed_value)
                     : snprintf(text, sizeof text, "%llu", (unsigned long long)unsigned_value);
    return length >= 0 && length < (int)sizeof text && append(builder, text, (size_t)length);
}

typedef struct {
    TextBuilder *builder;
    const AtNativeType *type;
    int depth;
    int first;
} DictFormat;

static int dictionary_entry(void *context, const void *key, const void *value)
{
    DictFormat *state = context;
    if (!state->first && !literal(state->builder, ", ")) {
        return 0;
    }
    state->first = 0;
    return format(state->builder, state->type->key, key, state->depth + 1, 1) &&
           literal(state->builder, ": ") &&
           format(state->builder, state->type->element, value, state->depth + 1, 1);
}

static int sequence(TextBuilder *builder, const AtNativeType *type, const void *value, int depth)
{
    if (depth >= 32) {
        return literal(builder, "[...]");
    }
    const unsigned char *data;
    int64_t count;
    if (type->kind == AT_NATIVE_LIST) {
        AtList *list;
        memcpy(&list, value, sizeof list);
        data = list->data;
        count = list->count;
    } else if (type->kind == AT_NATIVE_ARRAY) {
        data = value;
        count = type->count;
    } else {
        struct {
            const unsigned char *data;
            int64_t count;
        } slice;

        memcpy(&slice, value, sizeof slice);
        data = slice.data;
        count = slice.count;
    }
    if (!literal(builder, "[")) {
        return 0;
    }
    for (int64_t i = 0; i < count; i++) {
        if ((i && !literal(builder, ", ")) ||
            !format(builder, type->element, data + i * type->element->bytes, depth + 1, 1)) {
            return 0;
        }
    }
    return literal(builder, "]");
}

static int format_bytes(TextBuilder *builder, const unsigned char *data, int64_t length)
{
    if (!literal(builder, "Bytes(\"")) {
        return 0;
    }
    /* A printable hex representation cannot inject control characters or
     * accidentally interpret arbitrary file bytes as terminal UTF-8. */
    static const char hex[] = "0123456789abcdef";
    for (int64_t index = 0; index < length; index++) {
        unsigned char byte = data[index];
        char escaped[] = {'\\', 'x', hex[byte >> 4], hex[byte & 15]};
        if (!append(builder, escaped, sizeof escaped)) {
            return 0;
        }
    }
    return literal(builder, "\")");
}

static int format(TextBuilder *builder, const AtNativeType *type, const void *value, int depth,
                  int repr)
{
    switch (type->kind) {
    case AT_NATIVE_POINTER: {
        uint64_t address;
        memcpy(&address, value, sizeof address);
        char text[80];
        snprintf(text, sizeof text, "Ptr[%c%d](0x%llx)", type->element->is_signed ? 'i' : 'u',
                 type->element->bits, (unsigned long long)address);
        return literal(builder, text);
    }
    case AT_NATIVE_COMMAND:
        return literal(builder, "<Command>");
    case AT_NATIVE_BYTE_SPAN:
        return format_bytes(builder, value, type->count);
    case AT_NATIVE_BYTES: {
        const AtBytes *bytes;
        memcpy(&bytes, value, sizeof bytes);
        return format_bytes(builder, bytes->data, bytes->length);
    }
    case AT_NATIVE_CAP: {
        const AtCap *capability;
        memcpy(&capability, value, sizeof capability);

        static const struct {
            uint32_t bit;
            const char *name;
        } names[] = {
            {AS_CAP_FS_READ, "fs_read"}, {AS_CAP_FS_WRITE, "fs_write"}, {AS_CAP_NET, "net"},
            {AS_CAP_PROC, "proc"},       {AS_CAP_GUI, "gui"},           {AS_CAP_RAW, "raw"},
        };

        if (!literal(builder, "<cap ")) {
            return 0;
        }
        int first = 1;
        for (unsigned index = 0; index < sizeof names / sizeof names[0]; index++) {
            if (!(at_cap_bits(capability) & names[index].bit)) {
                continue;
            }
            if ((!first && !literal(builder, "|")) || !literal(builder, names[index].name)) {
                return 0;
            }
            first = 0;
        }
        if ((first && !literal(builder, "none")) || !literal(builder, " @")) {
            return 0;
        }
        const char *path = at_cap_path(capability);
        return literal(builder, path ? path : "/") && literal(builder, ">");
    }
    case AT_NATIVE_BUFFER: {
        const AtBuffer *buffer;
        memcpy(&buffer, value, sizeof buffer);
        char text[48];
        int length = snprintf(text, sizeof text, "<buffer %lld>", (long long)buffer->length);
        return length >= 0 && length < (int)sizeof text && append(builder, text, (size_t)length);
    }
    case AT_NATIVE_BOOL: {
        unsigned char bit;
        memcpy(&bit, value, 1);
        return literal(builder, bit & 1 ? "true" : "false");
    }
    case AT_NATIVE_INTEGER:
        return integer(builder, type, value);
    case AT_NATIVE_FLOAT: {
        double real;
        if (type->bits == 32) {
            float narrow;
            memcpy(&narrow, value, sizeof narrow);
            real = narrow;
        } else {
            memcpy(&real, value, sizeof real);
        }
        char text[128];
        int length = snprintf(text, sizeof text, "%.17g", real);
        return length >= 0 && length < (int)sizeof text && append(builder, text, (size_t)length);
    }
    case AT_NATIVE_TEXT: {
        AtNativeText text;
        memcpy(&text, value, sizeof text);
        return (!repr || literal(builder, "'")) &&
               append(builder, text.data, (size_t)text.length) && (!repr || literal(builder, "'"));
    }
    case AT_NATIVE_LIST:
    case AT_NATIVE_ARRAY:
    case AT_NATIVE_SLICE:
        return sequence(builder, type, value, depth);
    case AT_NATIVE_RANGE: {
        if (depth >= 32) {
            return literal(builder, "[...]");
        }
        AtRange *range;
        memcpy(&range, value, sizeof range);
        int64_t count = at_range_len(range);
        if (count < 0 || !literal(builder, "[")) {
            return 0;
        }
        for (int64_t index = 0; index < count; index++) {
            int64_t item;
            char text[32];
            if (!at_range_at(&item, range, index)) {
                return 0;
            }
            int length = snprintf(text, sizeof text, "%lld", (long long)item);
            if ((index && !literal(builder, ", ")) || length < 0 ||
                !append(builder, text, (size_t)length)) {
                return 0;
            }
        }
        return literal(builder, "]");
    }
    case AT_NATIVE_DICT: {
        if (depth >= 32) {
            return literal(builder, "{...}");
        }
        AtDict *dict;
        memcpy(&dict, value, sizeof dict);
        DictFormat state = {builder, type, depth, 1};
        return literal(builder, "{") && at_dict_visit(dict, dictionary_entry, &state) &&
               literal(builder, "}");
    }
    case AT_NATIVE_CLASS:
    case AT_NATIVE_LAYOUT:
        /* Classes are reference values. Their field descriptors describe the
         * pointed-to payload, whereas the descriptor's bytes is pointer size. */
        memcpy(&value, value, sizeof value);
        if (type->kind == AT_NATIVE_LAYOUT) {
            value = ((const AtBuffer *)value)->data;
        }
        /* fall through */
    case AT_NATIVE_STRUCT:
        if (depth >= 32) {
            return literal(builder, "(...)");
        }
        if (!literal(builder, type->name) || !literal(builder, "(")) {
            return 0;
        }
        for (int i = 0; i < type->count; i++) {
            const AtNativeField *field = &type->fields[i];
            if ((i && !literal(builder, ", ")) || !literal(builder, field->name) ||
                !literal(builder, "=") ||
                !format(builder, field->type, (const unsigned char *)value + field->offset,
                        depth + 1, 1)) {
                return 0;
            }
        }
        return literal(builder, ")");
    case AT_NATIVE_NONE:
        return literal(builder, "None");
    case AT_NATIVE_OPTIONAL:
        if (!(*(const unsigned char *)value & 1)) {
            return literal(builder, "None");
        }
        return format(builder, type->element, (const unsigned char *)value + type->fields[0].offset,
                      depth, repr);
    case AT_NATIVE_ANY: {
        AtAny *box;
        memcpy(&box, value, sizeof box);
        return format(builder, at_any_type(box), at_any_value(box), depth, repr);
    }
    case AT_NATIVE_CALLABLE:
        return literal(builder, "<fn>");
    case AT_NATIVE_EXCEPTION: {
        const AtNativeException *error = value;
        return literal(builder, at_exception_name(error->code)) &&
               (!error->message.length ||
                (literal(builder, ": ") &&
                 append(builder, error->message.data, (size_t)error->message.length)));
    }
    default:
        return 0;
    }
}

int at_format_value(AtNativeText *out, const AtNativeType *type, const void *value)
{
    TextBuilder builder = {0};
    int ok = format(&builder, type, value, 0, 0);
    char *data = ok ? at_gc_allocate(builder.length + 1, NULL) : NULL;
    if (data) {
        if (builder.length) {
            memcpy(data, builder.data, builder.length);
        }
        data[builder.length] = 0;
        *out = (AtNativeText){data, (int64_t)builder.length};
    }
    free(builder.data);
    return data != NULL;
}
