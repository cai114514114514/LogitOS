/* SPDX-License-Identifier: MIT */
/* User-visible output belongs here; allocation and error state live elsewhere. */
#include "native.h"
#include <stdio.h>
#include <string.h>

/* Previously each argument/separator wrote stdout immediately. LogitOS libc
 * src/stdio.c initializes stdout as _IONBF, so kernel messages could split a
 * language print between those writes even with fd/file.c's console lock.
 *
 * One KiB coalesces short diagnostic lines while bounding process-local storage
 * without allocating or changing libc's prompt-before-read behavior. This is
 * an application chunk size, not a serial-lock budget: fd/file.c independently
 * bounds its lock hold time and may split a large write further.
 * backend/llvm/calls.c's AT_CALL_PRINT evaluates all arguments before emitting
 * these calls and ends each print with at_print_sep(1). The runtime currently
 * has one print producer per process; this state does not add thread safety. */
#define PRINT_BUFFER_SIZE 1024
static char print_buffer[PRINT_BUFFER_SIZE];
static size_t print_length;

/* Submit only populated chunks, avoiding an extra zero-byte write when the
 * final newline exactly fills the buffer. Keep stdio's existing error policy. */
static void print_flush(void)
{
    if (print_length) {
        fwrite(print_buffer, 1, print_length, stdout);
        print_length = 0;
    }
}

/* Append explicit byte lengths so embedded NULs survive and argument joins
 * consume spare space instead of creating short chunks between full ones. */
static void print_append(const char *text, size_t length)
{
#ifdef ASPRINT_NEGCTL_PER_ARG
    /* Restore the old fragmented-output defect for the host gate. */
    if (length) {
        fwrite(text, 1, length, stdout);
    }
#else
    while (length) {
        size_t available = sizeof print_buffer - print_length;
        size_t chunk = length < available ? length : available;
        memcpy(print_buffer + print_length, text, chunk);
        print_length += chunk;
        text += chunk;
        length -= chunk;
        if (print_length == sizeof print_buffer) {
            print_flush();
        }
    }
#endif
}

/* Retain printf's signed conversion, including INT64_MIN, without stdout I/O.
 * 32 bytes fit the sign, at most 20 decimal digits of a 64-bit value, and NUL. */
void at_print_i64(int64_t value)
{
    char text[32];
    int length = snprintf(text, sizeof text, "%lld", (long long)value);
    print_append(text, (size_t)length);
}

/* Unsigned output uses the original format and the same bounded decimal size. */
void at_print_u64(uint64_t value)
{
    char text[32];
    int length = snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    print_append(text, (size_t)length);
}

/* Keep %.17g's original rounding and spelling. A binary64 scientific result
 * needs at most sign + 17 digits + dot + exponent sign/marker + 3 exponent
 * digits (24 bytes); 32 also covers fixed notation and the terminating NUL. */
void at_print_f64(double value)
{
    char text[32];
    int length = snprintf(text, sizeof text, "%.17g", value);
    print_append(text, (size_t)length);
}

/* Language strings carry lengths; strlen would silently truncate binary data.
 * Nonpositive lengths retain the old no-output behavior, even for NULL. */
void at_print_str(const char *text, int64_t length)
{
    if (length > 0) {
        print_append(text, (size_t)length);
    }
}

/* Exception names and optional messages join the same pending line; preserve
 * the original ': ' punctuation and length-based message representation. */
void at_print_error(const AtNativeException *error)
{
    const char *name = at_exception_name(error->code);
    print_append(name, strlen(name));
    if (error->message.length) {
        print_append(": ", 2);
        at_print_str(error->message.data, error->message.length);
    }
}

/* Boolean spelling stays lowercase, matching the existing language output. */
void at_print_bool(int value)
{
    const char *text = value ? "true" : "false";
    print_append(text, strlen(text));
}

/* The compiler's final separator is the commit point, including print() with
 * no arguments. Spaces and the newline must be buffered like argument bytes. */
void at_print_sep(int newline)
{
    print_append(newline ? "\n" : " ", 1);
    if (newline) {
        print_flush();
        /* A completed language print must precede a subsequent port write or
         * command's output even when stdout is a pipe. Host stdio otherwise
         * buffers the shell banner until after its children's output. Flush
         * once per print, not once per argument or character. */
        fflush(stdout);
    }
}
