/* SPDX-License-Identifier: MIT */
#include "ir/model.h"
#include <string.h>

void at_quote(FILE *f, const char *s)
{
    at_quote_bytes(f, s, strlen(s));
}

void at_quote_bytes(FILE *f, const char *s, size_t bytes)
{
    fputc('"', f);
    const unsigned char *end = (const unsigned char *)s + bytes;
    for (const unsigned char *p = (const unsigned char *)s; p < end; p++) {
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

static void related_locations(AsTypedProject *project, FILE *out, int module, int diagnostic)
{
    fputc('[', out);
    AsDiagnostic *item = &project->modules[module].diagnostics.items[diagnostic];
    int count = 0;
    if (item->related_path) {
        fputs("{\"path\":", out);
        at_quote(out, item->related_path);
        fprintf(out, ",\"start\":%u,\"end\":%u,\"line\":%u,\"column\":%u,\"source_checksum\":%u}",
                item->related_start, item->related_end, item->related_line, item->related_column,
                item->related_checksum);
        count++;
    }
    for (int i = 0; i < project->ncycles; i++) {
        AtImportCycle *cycle = &project->cycles[i];
        if (cycle->diagnostic_module != module || cycle->diagnostic_index != diagnostic) {
            continue;
        }
        for (int j = 0; j < cycle->count; j++) {
            AtModule *source = &project->modules[cycle->modules[j]];
            if (count++) {
                fputc(',', out);
            }
            fputs("{\"path\":", out);
            at_quote(out, source->path);
            fprintf(out, ",\"line\":1,\"column\":1,\"source_checksum\":%u}",
                    source->diagnostics.checksum);
        }
        break;
    }
    fputc(']', out);
}

void as_typed_report(AsTypedProject *p, FILE *out, int json)
{
    if (!p) {
        fputs(json ? "{\"ok\":false,\"error\":\"compiler allocation failed\"}\n"
                   : "Compiler allocation failed\n",
              out);
        return;
    }
    if (!json) {
        AsDiagnostics saved = as_diagnostics;
        for (int m = 0; m < p->nmodules; m++) {
            as_diagnostics = p->modules[m].diagnostics;
            as_diagnostic_write(out, 0);
        }
        as_diagnostics = saved;
        for (int i = 0; i < p->ncycles; i++) {
            fputs("  import cycle: ", out);
            for (int j = 0; j < p->cycles[i].count; j++) {
                fprintf(out, "%s%s", j ? " -> " : "", p->modules[p->cycles[i].modules[j]].path);
            }
            fputc('\n', out);
        }
        if (p->oom) {
            fputs("Compiler allocation failed\n", out);
        }
        return;
    }
    AtModule *entry = &p->modules[0];
    fprintf(out, "{\"version\":1,\"language\":3,\"ok\":%s,\"file\":",
            as_typed_errors(p) ? "false" : "true");
    at_quote(out, entry->path);
    fprintf(out, ",\"source_bytes\":%zu,\"source_checksum\":%u,\"diagnostics\":[",
            entry->diagnostics.bytes, entry->diagnostics.checksum);
    int comma = 0;
    for (int m = 0; m < p->nmodules; m++) {
        for (int i = 0; i < p->modules[m].diagnostics.count; i++) {
            AtModule *module = &p->modules[m];
            AsDiagnostic *d = &module->diagnostics.items[i];
            if (comma++) {
                fputc(',', out);
            }
            fputs("{\"severity\":\"error\",\"path\":", out);
            at_quote(out, module->path);
            fputs(",\"code\":", out);
            at_quote(out, d->code);
            fputs(",\"message\":", out);
            at_quote(out, d->message);
            fputs(",\"help\":", out);
            at_quote(out, d->help);
            fprintf(out,
                    ",\"start\":%u,\"end\":%u,\"line\":%u,\"column\":%u,\"end_line\":%u,\"end_"
                    "column\":%u,\"source_checksum\":%u,\"related\":",
                    d->start, d->end, d->line, d->column, d->end_line, d->end_column,
                    module->diagnostics.checksum);
            related_locations(p, out, m, i);
            fputc('}', out);
        }
    }
    int truncated = p->oom;
    for (int module = 0; module < p->nmodules; module++) {
        truncated |= p->modules[module].diagnostics.truncated;
    }
    fprintf(out, "],\"truncated\":%s,\"sources\":[", truncated ? "true" : "false");
    for (int m = 0; m < p->nmodules; m++) {
        if (m) {
            fputc(',', out);
        }
        fputs("{\"path\":", out);
        at_quote(out, p->modules[m].path);
        fprintf(out, ",\"bytes\":%zu,\"checksum\":%u}", p->modules[m].diagnostics.bytes,
                p->modules[m].diagnostics.checksum);
    }
    fputs("],\"symbols\":[", out);
    comma = 0;
    /* Report the same nominal declarations the checker resolves. Layouts do
     * not create runtime module variables; omitting them here would make their
     * constructors invisible to tools consuming the frontend's symbol table. */
    for (int i = AT_BUILTIN_LAST + 1; i < p->ntypes; i++) {
        AtType *type = &p->types[i];
        if ((type->kind != AT_STRUCT && type->kind != AT_CLASS && type->kind != AT_LAYOUT) ||
            !type->declaration.start || i == p->exception_type) {
            continue;
        }
        AtModule *module = &p->modules[type->module];
        if (comma++) {
            fputc(',', out);
        }
        fputs("{\"kind\":\"type\",\"name\":", out);
        at_quote(out, type->name);
        fputs(",\"path\":", out);
        at_quote(out, module->path);
        size_t start = (size_t)(type->declaration.start - module->source);
        fprintf(out, ",\"start\":%zu,\"end\":%zu,\"line\":%d,\"type\":", start,
                start + type->declaration.len, type->declaration.line);
        at_quote(out, type->name);
        fputc('}', out);
    }
    for (int i = 0; i < p->nfunctions; i++) {
        AtFunction *f = &p->functions[i];
        if (f->template_id >= 0 || f->module_initializer) {
            continue; /* Specializations do not create new source declarations. */
        }
        if (comma++) {
            fputc(',', out);
        }
        fputs(f->method_owner ? "{\"kind\":\"method\",\"name\":"
                              : "{\"kind\":\"function\",\"name\":",
              out);
        at_quote(out, f->name);
        if (f->lexical_parent) {
            fputs(",\"lexical_owner\":", out);
            at_quote(out, p->functions[f->lexical_parent - 1].name);
        }
        if (f->method_owner) {
            fputs(",\"owner\":", out);
            at_quote(out, p->types[f->method_owner].name);
        }
        fputs(",\"path\":", out);
        at_quote(out, p->modules[f->module].path);
        fprintf(out, ",\"start\":%zu,\"end\":%zu,\"line\":%d,\"type\":",
                (size_t)(f->token.start - p->modules[f->module].source),
                (size_t)(f->token.start - p->modules[f->module].source) + f->token.len,
                f->token.line);
        at_quote(out, p->types[f->result].name);
        fputs(",\"type_parameters\":[", out);
        for (int parameter = 0; parameter < f->generic_count; parameter++) {
            AtType *type = &p->types[f->type_parameters[parameter]];
            if (parameter) {
                fputc(',', out);
            }
            fputs("{\"name\":", out);
            at_quote(out, type->name);
            fputs(",\"constraint\":", out);
            const char *constraint =
                type->constraint == AT_CONSTRAINT_NUMBER                 ? "Number"
                : type->constraint == AT_CONSTRAINT_INTEGER              ? "Integer"
                : type->constraint == AT_CONSTRAINT_HASHABLE             ? "Hashable"
                : type->constraint == AT_CONSTRAINT_EQUATABLE            ? "Equatable"
                : type->constraint == AT_CONSTRAINT_ORDERED              ? "Ordered"
                : type->constraint == AT_CONSTRAINT_BYTE_STORAGE         ? "ByteStorage"
                : type->constraint == AT_CONSTRAINT_MUTABLE_BYTE_STORAGE ? "MutableByteStorage"
                                                                         : "";
            at_quote(out, constraint);
            fputc('}', out);
        }
        fputc(']', out);
        fputc('}', out);
    }
    for (int i = 0; i < p->nglobals; i++) {
        AtGlobal *global = &p->globals[i];
        AtModule *module = &p->modules[global->module];
        if (comma++) {
            fputc(',', out);
        }
        fputs("{\"kind\":\"variable\",\"name\":", out);
        at_quote(out, global->name);
        fputs(",\"path\":", out);
        at_quote(out, module->path);
        size_t start = (size_t)(global->token.start - module->source);
        fprintf(out, ",\"start\":%zu,\"end\":%zu,\"line\":%d,\"type\":", start,
                start + global->token.len, global->token.line);
        at_quote(out, p->types[global->type].name);
        fputc('}', out);
    }
    fputs("]}\n", out);
}
