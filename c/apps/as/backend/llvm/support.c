/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

void at_ir_emit(Gen *g, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(g->out, format, ap);
    va_end(ap);
}

const char *at_ir_type(Gen *g, int t)
{
    if (!g->types[t][0]) {
        AtType *x = &g->p->types[t];
        if (at_integer(g->p, t)) {
            snprintf(g->types[t], 128, "i%d", at_bits(g->p, t));
        } else if (x->kind == AT_POINTER) {
            /* A raw address carries no managed owner or runtime object. */
            strcpy(g->types[t], "i64");
        } else if (x->kind == AT_BOOL) {
            strcpy(g->types[t], "i1");
        } else if (x->kind == AT_VOID) {
            strcpy(g->types[t], "void");
        } else if (x->kind == AT_NONE) {
            strcpy(g->types[t], "i8");
        } else if (x->kind == AT_F32) {
            strcpy(g->types[t], "float");
        } else if (x->kind == AT_F64) {
            strcpy(g->types[t], "double");
        } else if (x->kind == AT_STR || at_slice_kind(x->kind)) {
            strcpy(g->types[t], "{ ptr, i64 }");
        } else if (x->kind == AT_CALLABLE) {
            strcpy(g->types[t], "{ ptr, ptr }");
        } else if (x->kind == AT_LIST || x->kind == AT_ANY || x->kind == AT_DICT ||
                   x->kind == AT_CLASS || x->kind == AT_RANGE || at_byte_storage_kind(x->kind) ||
                   x->kind == AT_CAP || x->kind == AT_PORT || x->kind == AT_COMMAND ||
                   x->kind == AT_PROCESS || x->kind == AT_REGION) {
            strcpy(g->types[t], "ptr");
        } else if (x->kind == AT_ARRAY) {
            /* Name arrays as well as structs. Spelling every nested array
             * inline used to truncate a valid type at the 128-byte buffer. */
            snprintf(g->types[t], 128, "%%A%d", t);
        } else {
            snprintf(g->types[t], 128, "%%S%d", t);
        }
    }
    return g->types[t];
}

Val at_ir_value(int type, const char *text)
{
    Val v = {.type = type};
    snprintf(v.text, sizeof v.text, "%s", text);
    return v;
}

Val at_ir_temp(Gen *g, int type)
{
    Val v = {.type = type};
    snprintf(v.text, sizeof v.text, "%%v%d", ++g->id);
    return v;
}

int at_ir_label(Gen *g)
{
    return ++g->label;
}

void at_ir_mark(Gen *g, int l)
{
    at_ir_emit(g, "b%d:\n", l);
    g->terminated = 0;
}

void at_ir_jump(Gen *g, int l)
{
    if (!g->terminated) {
        at_ir_emit(g, "  br label %%b%d\n", l);
        g->terminated = 1;
    }
}

void at_ir_throw(Gen *g)
{
    at_ir_cleanup_to(g, g->handler_cleanup, 1);
    if (g->handler_label) {
        at_ir_jump(g, g->handler_label);
    } else {
        at_ir_emit(g, "  br label %%failure\n");
        g->terminated = 1;
    }
}

int at_ir_column(Gen *g, AtNode *n)
{
    AtModule *m = &g->p->modules[n->module];
    int column = 1;
    const char *start = n->token.start;
    while (start > m->source && start[-1] != '\n') {
        start--;
    }
    for (const char *p = start; p < n->token.start; p++) {
        if (((unsigned char)*p & 0xc0) != 0x80) {
            column++;
        }
    }
    return column;
}

void at_ir_guard(Gen *g, Val bad, AtNode *n, int kind)
{
    /* A local handler gets first refusal. Unmatched errors reach the shared
     * failure block and are checked by the caller before its result is used. */
    int fail = at_ir_label(g), ok = at_ir_label(g);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", bad.text, fail, ok);
    at_ir_mark(g, fail);
    at_ir_emit(g, "  call void @at_raise(i32 %d, ptr @path%d, i32 %d, i32 %d)\n", kind, n->module,
               n->token.line, at_ir_column(g, n));
    at_ir_throw(g);
    at_ir_mark(g, ok);
}

void at_ir_propagate(Gen *g)
{
    /* Must follow every generated language call, including void functions.
     * Missing this check turns a callee's overflow into successful caller code. */
    Val flag = at_ir_temp(g, AT_I32), bad = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = load i32, ptr @at_failed\n  %s = icmp ne i32 %s, 0\n", flag.text,
               bad.text, flag.text);
    int failed = at_ir_label(g), ok = at_ir_label(g);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", bad.text, failed, ok);
    at_ir_mark(g, failed);
    at_ir_throw(g);
    at_ir_mark(g, ok);
}

int at_ir_references(Gen *g, int type)
{
    AtType *t = &g->p->types[type];
    if (t->kind == AT_STR || t->kind == AT_LIST || t->kind == AT_ANY || t->kind == AT_DICT ||
        t->kind == AT_CALLABLE || t->kind == AT_CLASS || t->kind == AT_RANGE ||
        at_byte_storage_kind(t->kind) || t->kind == AT_CAP || t->kind == AT_COMMAND ||
        t->kind == AT_PROCESS) {
        return 1;
    }
    if (t->kind == AT_ARRAY || at_slice_kind(t->kind) || t->kind == AT_OPTIONAL) {
        return at_ir_references(g, t->element);
    }
    if (t->kind == AT_STRUCT) {
        for (int i = 0; i < t->count; i++) {
            if (at_ir_references(g, t->fields[i])) {
                return 1;
            }
        }
    }
    return 0;
}

void at_ir_root(Gen *g, const char *slot, int type)
{
    int id = ++g->id;
    at_ir_emit(g, "  %%root%d = alloca { ptr, ptr, ptr }\n", id);
    at_ir_emit(g, "  call void @at_gc_root(ptr %%root%d, ptr %s, ptr @scan%d)\n", id, slot, type);
}

/* Runtime allocation reports success separately from its result. The source
 * site belongs to generated code, so OOM follows the same handler path as an
 * arithmetic error and never exposes a null list as a usable value. */
void at_ir_allocation(Gen *g, Val result, AtNode *site, int pointer)
{
    Val bad = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp eq %s %s, %s\n", bad.text, pointer ? "ptr" : "i32", result.text,
               pointer ? "null" : "0");
    at_ir_guard(g, bad, site, AT_E_MEMORY);
}

void at_ir_runtime_status(Gen *g, Val status, AtNode *site)
{
    Val failed = at_ir_temp(g, AT_BOOL);
    int error = at_ir_label(g), success = at_ir_label(g);
    at_ir_emit(g, "  %s = icmp ne i32 %s, 0\n", failed.text, status.text);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", failed.text, error, success);
    at_ir_mark(g, error);
    /* Runtime helpers return the reason; the compiler owns the source span.
     * Allocation, permission and I/O failures must retain distinct types. */
    at_ir_emit(g, "  call void @at_raise(i32 %s, ptr @path%d, i32 %d, i32 %d)\n", status.text,
               site->module, site->token.line, at_ir_column(g, site));
    at_ir_throw(g);
    at_ir_mark(g, success);
}
