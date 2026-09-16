/* SPDX-License-Identifier: MIT */
#ifndef AS_DIAGNOSTIC_H
#define AS_DIAGNOSTIC_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define AS_DIAGNOSTIC_MAX 32

typedef struct {
    unsigned start, end, line, column, end_line, end_column;
    char code[16], message[256], help[192];
    /* Optional originating location in the same owned project snapshot. The
     * path belongs to that project; diagnostics must not outlive the snapshot. */
    const char *related_path;
    unsigned related_start, related_end, related_line, related_column;
    uint32_t related_checksum;
} AsDiagnostic;

typedef struct {
    const char *source, *path;
    size_t bytes;
    uint32_t checksum;
    int active, count, truncated;
    AsDiagnostic items[AS_DIAGNOSTIC_MAX];
} AsDiagnostics;

extern AsDiagnostics as_diagnostics;
void as_diagnostic_begin(const char *path, const char *source);
void as_diagnostic_begin_n(const char *path, const char *source, size_t bytes);
void as_diagnostic_end(void);
void as_diagnostic_add(const char *code, const char *message, size_t start, size_t end);
void as_diagnostic_token(const char *code, const char *message, const char *start, int bytes);
void as_diagnostic_write(FILE *out, int json);
#endif
