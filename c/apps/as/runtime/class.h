/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_CLASS_H
#define AS_NATIVE_CLASS_H
#include <stdint.h>

/* One field list defines both the C header and the LLVM payload prefix.
 * Field offsets and GC scanners must advance past this same prefix. */
#define AT_CLASS_HEADER(FIELD)                                                                     \
    FIELD(methods, const void *, "ptr")                                                            \
    FIELD(required, int64_t, "i64")                                                                \
    FIELD(initialized, int64_t, "i64")

typedef struct {
#define AT_HEADER_MEMBER(name, type, llvm_type) type name;
    AT_CLASS_HEADER(AT_HEADER_MEMBER)
#undef AT_HEADER_MEMBER
} AtClassHeader;

enum {
#define AT_HEADER_COUNT(name, type, llvm_type) +1
    AT_CLASS_HEADER_COUNT = 0 AT_CLASS_HEADER(AT_HEADER_COUNT)
#undef AT_HEADER_COUNT
};
#endif
