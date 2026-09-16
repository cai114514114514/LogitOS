/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include "sema/internal.h"
#include <stdio.h>
#include <string.h>

void at_ir_class_ready(Gen *g, Val receiver, AtNode *site)
{
    Val ready = at_ir_temp(g, AT_I32), bad = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = call i32 @at_class_ready(ptr %s)\n", ready.text, receiver.text);
    at_ir_emit(g, "  %s = icmp eq i32 %s, 0\n", bad.text, ready.text);
    at_ir_guard(g, bad, site, AT_E_RUNTIME);
}

void at_ir_class_field_initialized(Gen *g, AtNode *field)
{
    if (field->field_store) {
        Val receiver = at_ir_temp(g, field->a->type);
        at_ir_emit(g, "  %s = load ptr, ptr %%local0\n", receiver.text);
        at_ir_emit(g, "  call void @at_class_field_initialized(ptr %s, i32 %d)\n", receiver.text,
                   field->field);
    }
}

void at_ir_class_tables(Gen *g)
{
    /* A declaration ID is a stable slot throughout this compilation. Entries
     * for ancestor declarations point at the most-derived override. Unused
     * slots remain null; checked code can only select a declared method. */
    for (int type = 0; type < g->p->ntypes; type++) {
        if (g->p->types[type].kind != AT_CLASS) {
            continue;
        }
        at_ir_emit(g, "@methods%d = private constant [%d x ptr] [", type, g->p->nfunctions);
        for (int slot = 0; slot < g->p->nfunctions; slot++) {
            AtFunction *declaration = &g->p->functions[slot];
            int method = -1;
            if (at_class_subtype(g->p, type, declaration->method_owner)) {
                method = at_class_method(g->p, type, declaration->token);
            }
            at_ir_emit(g, "%sptr ", slot ? ", " : "");
            if (method >= 0) {
                at_ir_emit(g, "@bound%d", method);
            } else {
                at_ir_emit(g, "null");
            }
        }
        at_ir_emit(g, "]\n");
    }
}

const char *at_ir_class_layout(Gen *g, int type)
{
    if (!g->class_layouts[type][0]) {
        snprintf(g->class_layouts[type], sizeof g->class_layouts[type], "%%Object%d", type);
    }
    return g->class_layouts[type];
}

Val at_ir_class_construct(Gen *g, AtNode *node, Val result)
{
    AtFunction *initializer = node->field ? &g->p->functions[node->field - 1] : NULL;
    AtType *type = &g->p->types[node->type];
    Val arguments[AT_ARGS];
    for (int i = 0; i < node->count; i++) {
        int expected = initializer ? initializer->locals[i + 1].type : type->fields[i];
        arguments[i] = at_ir_argument(g, node->args[i], expected);
    }
    /* Arguments run before allocation. Hold the new object independently of
     * its final expression type (which may be Optional) while init can collect. */
    at_ir_emit(g,
               "  %s = call ptr @at_object_new(i64 ptrtoint (ptr getelementptr (%s, "
               "ptr null, i32 1) to i64), ptr @objectscan%d)\n",
               result.text, at_ir_class_layout(g, node->type), node->type);
    at_ir_allocation(g, result, node, 1);
    at_ir_emit(g, "  store ptr %s, ptr %%construction%d\n", result.text, node->id);
    uint64_t required = type->count == 32 ? UINT32_MAX : (UINT64_C(1) << type->count) - 1;
    at_ir_emit(g, "  call void @at_class_prepare(ptr %s, ptr @methods%d, i64 %llu)\n", result.text,
               node->type, (unsigned long long)required);
    if (initializer) {
        at_ir_emit(g, "  call void @fn%d(ptr %s", node->field - 1, result.text);
        for (int i = 0; i < node->count; i++) {
            at_ir_emit(g, ", %s %s", at_ir_type(g, arguments[i].type), arguments[i].text);
        }
        at_ir_emit(g, ")\n");
        at_ir_propagate(g);
    } else {
        for (int i = 0; i < node->count; i++) {
            Val address = at_ir_temp(g, 0);
            at_ir_emit(g, "  %s = getelementptr %s, ptr %s, i32 0, i32 %d\n", address.text,
                       at_ir_class_layout(g, node->type), result.text, i + AT_CLASS_HEADER_COUNT);
            at_ir_emit(g, "  store %s %s, ptr %s\n", at_ir_type(g, arguments[i].type),
                       arguments[i].text, address.text);
            at_ir_emit(g, "  call void @at_class_field_initialized(ptr %s, i32 %d)\n", result.text,
                       i);
        }
    }
    at_ir_class_ready(g, result, node);
    return result;
}

Val at_ir_bound_method(Gen *g, AtNode *node, Val result)
{
    Val receiver = at_ir_expression(g, node->a), code = at_ir_temp(g, node->type);
    Val target = at_ir_value(0, "");
    if (node->a->kind == AN_SUPER || !strcmp(g->p->functions[node->symbol].name, "init")) {
        snprintf(target.text, sizeof target.text, "@bound%d", node->symbol);
    } else {
        Val table = at_ir_temp(g, 0), address = at_ir_temp(g, 0);
        target = at_ir_temp(g, 0);
        at_ir_emit(g, "  %s = load ptr, ptr %s\n", table.text, receiver.text);
        at_ir_emit(g, "  %s = getelementptr ptr, ptr %s, i32 %d\n", address.text, table.text,
                   node->symbol);
        at_ir_emit(g, "  %s = load ptr, ptr %s\n", target.text, address.text);
    }
    at_ir_emit(g, "  %s = insertvalue %s zeroinitializer, ptr %s, 0\n", code.text,
               at_ir_type(g, node->type), target.text);
    at_ir_emit(g, "  %s = insertvalue %s %s, ptr %s, 1\n", result.text, at_ir_type(g, node->type),
               code.text, receiver.text);
    return result;
}

void at_ir_class_scanner(Gen *g, int type)
{
    AtType *object = &g->p->types[type];
    at_ir_emit(g, "define internal void @objectscan%d(ptr %%object) {\nentry:\n", type);
    for (int field = 0; field < object->count; field++) {
        if (at_ir_references(g, object->fields[field])) {
            at_ir_emit(g, "  %%field%d = getelementptr %s, ptr %%object, i32 0, i32 %d\n", field,
                       at_ir_class_layout(g, type), field + AT_CLASS_HEADER_COUNT);
            at_ir_emit(g, "  call void @scan%d(ptr %%field%d)\n", object->fields[field], field);
        }
    }
    at_ir_emit(g, "  ret void\n}\n");
}

void at_ir_bound_adapter(Gen *g, int id)
{
    AtFunction *method = &g->p->functions[id];
    at_ir_emit(g, "define internal %s @bound%d(ptr %%self", at_ir_type(g, method->result), id);
    for (int i = 1; i < method->nparams; i++) {
        at_ir_emit(g, ", %s %%arg%d", at_ir_type(g, method->locals[i].type), i);
    }
    at_ir_emit(g, ") {\nentry:\n  ");
    if (method->result != AT_VOID) {
        at_ir_emit(g, "%%result = ");
    }
    at_ir_emit(g, "call %s @fn%d(ptr %%self", at_ir_type(g, method->result), id);
    for (int i = 1; i < method->nparams; i++) {
        at_ir_emit(g, ", %s %%arg%d", at_ir_type(g, method->locals[i].type), i);
    }
    at_ir_emit(g, ")\n");
    /* The caller of the bound adapter uses the ordinary checked indirect-call
     * path; it inspects pending exceptions before consuming this return value. */
    if (method->result == AT_VOID) {
        at_ir_emit(g, "  ret void\n}\n");
    } else {
        at_ir_emit(g, "  ret %s %%result\n}\n", at_ir_type(g, method->result));
    }
}
