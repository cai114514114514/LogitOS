/* SPDX-License-Identifier: MIT */
/* Explicit propagation keeps the first source location without a system unwinder. */
#include "native.h"
#include <stdio.h>
#include <string.h>

int at_failed;
static AtNativeException pending;

void at_exception_mark(void)
{
    at_gc_mark((void *)pending.message.data);
    at_gc_mark((void *)pending.file.data);
}

const char *at_exception_name(int64_t code)
{
    switch (code) {
#define AT_RUNTIME_NAME(code, name)                                                                \
    case code:                                                                                     \
        return #name;
        AT_EXCEPTION_NAMES(AT_RUNTIME_NAME)
#undef AT_RUNTIME_NAME
    default:
        return "RuntimeError";
    }
}

void at_exception_make(AtNativeException *out, int code, const char *message, int64_t length)
{
    *out = (AtNativeException){0};
    out->code = code;
    out->message = (AtNativeText){message, length};
}

void at_exception_throw(const AtNativeException *error, const char *path, int line, int column)
{
    if (at_failed) {
        return;
    }
    pending = *error;
    /* A new error receives its raise site. Rethrowing a captured value keeps
     * its original location, including when another exception was handled in
     * between. Each handler owns a native stack copy, not a shared VM slot. */
    if (!pending.line) {
        pending.file = (AtNativeText){path, (int64_t)strlen(path)};
        pending.line = line;
        pending.column = column;
    }
    at_failed = 1;
}

void at_exception_take(AtNativeException *out)
{
    *out = pending;
    pending = (AtNativeException){0};
    at_failed = 0;
}

int at_exception_kind(void)
{
    return (int)pending.code;
}

void at_raise(int kind, const char *path, int line, int column)
{
    AtNativeException error;
    const char *message = at_exception_name(kind);
    at_exception_make(&error, kind, message, (int64_t)strlen(message));
    at_exception_throw(&error, path, line, column);
}

int at_finish(void)
{
    if (!at_failed) {
        return 0;
    }
    const char *kind = at_exception_name(pending.code);
    if (pending.file.length) {
        fwrite(pending.file.data, 1, (size_t)pending.file.length, stderr);
    }
    fprintf(stderr, ":%lld:%lld: %s", (long long)pending.line, (long long)pending.column, kind);
    if (pending.message.length &&
        (pending.message.length != (int64_t)strlen(kind) ||
         memcmp(pending.message.data, kind, (size_t)pending.message.length))) {
        fputs(": ", stderr);
        fwrite(pending.message.data, 1, (size_t)pending.message.length, stderr);
    }
    fputc('\n', stderr);
    return 1;
}
