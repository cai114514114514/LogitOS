/* SPDX-License-Identifier: MIT */
/* User-visible output belongs here; allocation and error state live elsewhere. */
#include "native.h"
#include <stdio.h>

void at_print_i64(int64_t n)
{
    printf("%lld", (long long)n);
}

void at_print_u64(uint64_t n)
{
    printf("%llu", (unsigned long long)n);
}

void at_print_f64(double n)
{
    printf("%.17g", n);
}

void at_print_str(const char *p, int64_t n)
{
    if (n > 0) {
        fwrite(p, 1, (size_t)n, stdout);
    }
}

void at_print_error(const AtNativeException *error)
{
    fputs(at_exception_name(error->code), stdout);
    if (error->message.length) {
        fputs(": ", stdout);
        at_print_str(error->message.data, error->message.length);
    }
}

void at_print_bool(int n)
{
    fputs(n ? "true" : "false", stdout);
}

void at_print_sep(int n)
{
    fputc(n ? '\n' : ' ', stdout);
    if (n) {
        /* A completed language print must precede a subsequent port write or
         * command's output even when stdout is a pipe. Host stdio otherwise
         * buffers the shell banner until after its children's output. Flush
         * once per print, not once per argument or character. */
        fflush(stdout);
    }
}
