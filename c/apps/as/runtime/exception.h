/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_EXCEPTION_H
#define AS_NATIVE_EXCEPTION_H
#include <stdint.h>

/* The first six numbers are the original native trap ABI. Already-lowered
 * arithmetic and typed handlers must continue to agree on their meaning. */
enum AtExceptionCode {
    AT_E_ANY = 0,
    AT_E_OVERFLOW = 1,
    AT_E_ZERO_DIVISION = 2,
    AT_E_INDEX = 3,
    AT_E_CONVERSION = 4,
    AT_E_VALUE = 5,
    AT_E_ASSERTION = 6,
    AT_E_RUNTIME = 7,
    AT_E_IO = 8,
    AT_E_MEMORY = 9,
    AT_E_TYPE = 10,
    AT_E_KEY = 11,
    AT_E_PERMISSION = 12
};

typedef struct {
    const char *data;
    int64_t length;
} AtNativeText;

/* Catch resolution and runtime formatting consume the same name table. */
#define AT_EXCEPTION_NAMES(ENTRY)                                                                  \
    ENTRY(AT_E_ANY, Error)                                                                         \
    ENTRY(AT_E_OVERFLOW, OverflowError)                                                            \
    ENTRY(AT_E_ZERO_DIVISION, ZeroDivisionError)                                                   \
    ENTRY(AT_E_INDEX, IndexError)                                                                  \
    ENTRY(AT_E_CONVERSION, ConversionError)                                                        \
    ENTRY(AT_E_VALUE, ValueError)                                                                  \
    ENTRY(AT_E_ASSERTION, AssertionError)                                                          \
    ENTRY(AT_E_RUNTIME, RuntimeError)                                                              \
    ENTRY(AT_E_IO, IOError)                                                                        \
    ENTRY(AT_E_MEMORY, MemoryError)                                                                \
    ENTRY(AT_E_TYPE, TypeError)                                                                    \
    ENTRY(AT_E_KEY, KeyError)                                                                      \
    ENTRY(AT_E_PERMISSION, PermissionError)

/* One field list feeds both C and the compiler. Calls use output pointers so
 * target-specific aggregate return conventions never enter this boundary. */
#define AT_EXCEPTION_FIELDS(FIELD)                                                                 \
    FIELD(code, int64_t, AT_I64)                                                                   \
    FIELD(message, AtNativeText, AT_STR)                                                           \
    FIELD(file, AtNativeText, AT_STR)                                                              \
    FIELD(line, int64_t, AT_I64)                                                                   \
    FIELD(column, int64_t, AT_I64)

typedef struct {
#define AT_C_FIELD(name, c_type, language_type) c_type name;
    AT_EXCEPTION_FIELDS(AT_C_FIELD)
#undef AT_C_FIELD
} AtNativeException;

void at_exception_make(AtNativeException *out, int code, const char *message, int64_t length);
void at_exception_throw(const AtNativeException *error, const char *path, int line, int column);
void at_exception_take(AtNativeException *out);
int at_exception_kind(void);
void at_print_error(const AtNativeException *error);
#endif
