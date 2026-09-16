/* SPDX-License-Identifier: MIT */
#include "common/diagnostic.h"
#include <string.h>

AsDiagnostics as_diagnostics;

void as_diagnostic_begin(const char *path, const char *source)
{
    as_diagnostic_begin_n(path, source, strlen(source));
}

void as_diagnostic_begin_n(const char *path, const char *source, size_t bytes)
{
    memset(&as_diagnostics, 0, sizeof as_diagnostics);
    as_diagnostics.source = source;
    as_diagnostics.path = path;
    as_diagnostics.bytes = bytes;
    as_diagnostics.active = 1;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < as_diagnostics.bytes; i++) {
        h = (h ^ (unsigned char)source[i]) * 16777619u;
    }
    as_diagnostics.checksum = h;
}

void as_diagnostic_end(void)
{
    as_diagnostics.active = 0;
    as_diagnostics.source = NULL;
    as_diagnostics.path = NULL;
}

static void position(size_t off, unsigned *line, unsigned *column)
{
    *line = *column = 1;
    for (size_t i = 0; i < off; i++) {
        unsigned char c = (unsigned char)as_diagnostics.source[i];
        if (c == '\n') {
            (*line)++;
            *column = 1;
        } else if ((c & 0xc0) != 0x80) {
            (*column)++;
        }
    }
}

void as_diagnostic_add(const char *code, const char *message, size_t start, size_t end)
{
    AsDiagnostics *d = &as_diagnostics;
    if (!d->active) {
        return;
    }
    if (d->count == AS_DIAGNOSTIC_MAX) {
        d->truncated = 1;
        return;
    }
    if (start > d->bytes) {
        start = d->bytes;
    }
    if (end > d->bytes) {
        end = d->bytes;
    }
    if (end < start) {
        end = start;
    }
    /* Byte offsets are authoritative for edits; columns are Unicode scalar
     * counts for display. Always expand a span to whole UTF-8 code points. */
    while (start && start < d->bytes && ((unsigned char)d->source[start] & 0xc0) == 0x80) {
        start--;
    }
    while (end < d->bytes && ((unsigned char)d->source[end] & 0xc0) == 0x80) {
        end++;
    }
    AsDiagnostic *e = &d->items[d->count++];
    e->start = (unsigned)start;
    e->end = (unsigned)end;
    position(start, &e->line, &e->column);
    position(end, &e->end_line, &e->end_column);
    snprintf(e->code, sizeof e->code, "%s", code);
    snprintf(e->message, sizeof e->message, "%s", message);
    if (strstr(message, "indent")) {
        snprintf(e->help, sizeof e->help, "Align this block with its enclosing indentation level.");
    } else if (strstr(message, "unterminated")) {
        snprintf(e->help, sizeof e->help, "Close the string or expression that starts here.");
    } else if (strstr(message, "expected")) {
        snprintf(e->help, sizeof e->help,
                 "Add or correct the token named in the diagnostic before this location.");
    } else if (strstr(message, "outside")) {
        snprintf(e->help, sizeof e->help, "Move this statement into the required enclosing scope.");
    }
}

void as_diagnostic_token(const char *code, const char *message, const char *start, int bytes)
{
    if (!as_diagnostics.active) {
        return;
    }
    uintptr_t p = (uintptr_t)start, b = (uintptr_t)as_diagnostics.source;
    /* f-string holes are separately allocated source buffers. Their caller
     * reports the enclosing source span instead of subtracting unrelated C
     * pointers (undefined behaviour, sometimes a huge bogus editor offset). */
    if (p < b || p - b > as_diagnostics.bytes) {
        return;
    }
    as_diagnostic_add(code, message, (size_t)(p - b),
                      (size_t)(p - b) + (bytes > 0 ? (size_t)bytes : 0));
}

static void quoted(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
            fputc(*p, f);
        } else if (*p < 32) {
            fprintf(f, "\\u%04x", *p);
        } else {
            fputc(*p, f);
        }
    }
    fputc('"', f);
}

static void write_related(FILE *out, const AsDiagnostic *diagnostic)
{
    fputs(",\"related\":[", out);
    if (diagnostic->related_path) {
        fputs("{\"path\":", out);
        quoted(out, diagnostic->related_path);
        fprintf(out, ",\"start\":%u,\"end\":%u,\"line\":%u,\"column\":%u,\"source_checksum\":%u}",
                diagnostic->related_start, diagnostic->related_end, diagnostic->related_line,
                diagnostic->related_column, diagnostic->related_checksum);
    }
    fputs("]}", out);
}

void as_diagnostic_write(FILE *f, int json)
{
    AsDiagnostics *d = &as_diagnostics;
    if (json) {
        fprintf(f, "{\"version\":1,\"ok\":%s,\"file\":", d->count ? "false" : "true");
        quoted(f, d->path);
        fprintf(f,
                ",\"source_bytes\":%zu,\"source_checksum\":%u,\"truncated\":%s,\"diagnostics\":[",
                d->bytes, d->checksum, d->truncated ? "true" : "false");
    }
    for (int i = 0; i < d->count; i++) {
        AsDiagnostic *e = &d->items[i];
        if (json) {
            if (i) {
                fputc(',', f);
            }
            fprintf(f, "{\"severity\":\"error\",\"code\":");
            quoted(f, e->code);
            fprintf(f, ",\"message\":");
            quoted(f, e->message);
            fprintf(f, ",\"help\":");
            quoted(f, e->help);
            fprintf(f,
                    ",\"start\":%u,\"end\":%u,\"line\":%u,\"column\":%u,\"end_line\":%u,\"end_"
                    "column\":%u",
                    e->start, e->end, e->line, e->column, e->end_line, e->end_column);
            write_related(f, e);
        } else {
            fprintf(f, "%s:%u:%u: %s: %s\n", d->path, e->line, e->column, e->code, e->message);
            size_t start = e->start;
            while (start && d->source[start - 1] != '\n') {
                start--;
            }
            size_t end = start;
            while (end < d->bytes && d->source[end] != '\n') {
                end++;
            }
            fprintf(f, "  %.*s\n  ", (int)(end - start), d->source + start);
            for (unsigned c = 1; c < e->column; c++) {
                fputc(' ', f);
            }
            fputs("^\n", f);
            if (e->help[0]) {
                fprintf(f, "  help: %s\n", e->help);
            }
            if (e->related_path) {
                fprintf(f, "  borrow: %s:%u:%u\n", e->related_path, e->related_line,
                        e->related_column);
            }
        }
    }
    if (json) {
        fputs("]}\n", f);
    } else if (!d->count) {
        fprintf(f, "%s: check passed\n", d->path);
    }
}
