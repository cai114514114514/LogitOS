/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_TYPE_H
#define AS_NATIVE_TYPE_H
#include "exception.h"
#include <stdint.h>

/* Metadata is used only when an operation explicitly needs a dynamic view,
 * such as formatting Any or a container. Arithmetic still uses native values.
 * Ordinary field offsets come from LLVM's target layout. Explicit ABI records
 * use checked source offsets instead; neither uses the retiring VM's Value. */
enum AtNativeKind {
    AT_NATIVE_BOOL = 1,
    AT_NATIVE_INTEGER,
    AT_NATIVE_FLOAT,
    AT_NATIVE_TEXT,
    AT_NATIVE_LIST,
    AT_NATIVE_DICT,
    AT_NATIVE_ARRAY,
    AT_NATIVE_SLICE,
    AT_NATIVE_STRUCT,
    AT_NATIVE_ANY,
    AT_NATIVE_CALLABLE,
    AT_NATIVE_EXCEPTION,
    AT_NATIVE_NONE,
    AT_NATIVE_OPTIONAL,
    AT_NATIVE_CLASS,
    AT_NATIVE_RANGE,
    AT_NATIVE_BUFFER,
    AT_NATIVE_CAP,
    AT_NATIVE_BYTES,
    AT_NATIVE_LAYOUT,
    AT_NATIVE_BYTE_SPAN, /* Inline fixed bytes, only in ABI field metadata. */
    AT_NATIVE_COMMAND,
    AT_NATIVE_POINTER
};

typedef struct AtNativeType AtNativeType;

typedef struct {
    int64_t offset;
    const AtNativeType *type;
    const char *name;
} AtNativeField;

struct AtNativeType {
    int32_t kind;
    int32_t bits;
    int32_t is_signed;
    int32_t count;
    int64_t bytes;
    const AtNativeType *element;
    const AtNativeType *key;
    const AtNativeField *fields;
    const char *name;
};

int at_format_value(AtNativeText *out, const AtNativeType *type, const void *value);
#endif
