/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <string.h>

int at_class_method(AsTypedProject *project, int type, Token name)
{
    for (int owner = type; owner; owner = project->types[owner].base) {
        for (int i = 0; i < project->nfunctions; i++) {
            AtFunction *method = &project->functions[i];
            if (method->method_owner == owner && at_scope_name_equal(name, method->name)) {
                return i;
            }
        }
    }
    return -1;
}

int at_class_initializing(Checker *checker)
{
    return checker->f->method_owner && !strcmp(checker->f->name, "init");
}

int at_class_initialized(Checker *checker)
{
    if (!at_class_initializing(checker)) {
        return 1;
    }
    int count = checker->p->types[checker->f->method_owner].count;
    uint32_t all = count == 32 ? UINT32_MAX : (1u << count) - 1;
    return checker->f->initialized_fields == all;
}

void at_class_require_initialized(Checker *checker, AtNode *site)
{
    if (!at_class_initialized(checker)) {
        at_check_error(checker, site, "AS3210",
                       "Constructor must initialize every field before self escapes or returns");
    }
    if (site && at_class_initializing(checker) &&
        (site->kind == AN_NAME || site->kind == AN_FIELD || site->kind == AN_METHOD ||
         site->kind == AN_CLOSURE)) {
        /* A base initializer knows only its own fields. Check derived fields
         * too before exposing self through a value, callback or closure. */
        site->class_ready = 1;
    }
}

static int self_field(Checker *checker, AtNode *node)
{
    return at_class_initializing(checker) && node->kind == AN_FIELD && node->a &&
           node->a->kind == AN_NAME && node->a->symbol == 0 &&
           node->a->type == checker->f->method_owner;
}

void at_class_field_use(Checker *checker, AtNode *node)
{
    if (self_field(checker, node) && checker->assignment_target != node &&
        !(checker->f->initialized_fields & (1u << node->field))) {
        at_check_error(checker, node, "AS3210", "Constructor reads a field before initialization");
    }
}

void at_class_field_store(Checker *checker, AtNode *node)
{
    if (self_field(checker, node)) {
        checker->f->initialized_fields |= 1u << node->field;
        node->field_store = 1;
    }
}

int at_class_method_value(Checker *checker, AtNode *node, int method_id)
{
    AtFunction *method = &checker->p->functions[method_id];
    int parent_init = node->a->kind == AN_SUPER && !strcmp(method->name, "init");
    if (parent_init && !node->op) {
        at_check_error(checker, node, "AS3211", "super.init must be called directly");
    }
    if (parent_init && !at_class_initializing(checker)) {
        at_check_error(checker, node, "AS3211", "super.init is only available in a constructor");
    }
    if (!parent_init &&
        ((node->a->kind == AN_NAME && node->a->symbol == 0) || node->a->kind == AN_SUPER)) {
        at_class_require_initialized(checker, node);
    }
    if (method->name[0] == '_' &&
        at_class_lexical_owner(checker->p, checker->f) != method->method_owner) {
        at_check_error(checker, node, "AS3300",
                       "Private method is only accessible inside its class");
    }
    at_check_function(checker->p, method);
    int parameters[AT_ARGS];
    for (int i = 1; i < method->nparams; i++) {
        parameters[i - 1] = method->locals[i].type;
    }
    node->kind = AN_METHOD;
    node->symbol = method_id;
    return at_callable_type(checker->p, parameters, method->nparams - 1, method->result,
                            node->module, node->token);
}

int at_class_constructor(Checker *checker, AtNode *node, int type)
{
    Token init = {.start = "init", .len = 4};
    int method_id = at_class_method(checker->p, type, init);
    AtType *object = &checker->p->types[type];
    AtFunction *method = method_id >= 0 ? &checker->p->functions[method_id] : NULL;
    if (method && method->method_owner != type &&
        object->count != checker->p->types[method->method_owner].count) {
        at_check_error(checker, node, "AS3210",
                       "A derived class with new fields needs its own initializer");
    }
    int count = method ? method->nparams - 1 : object->count;
    node->symbol = AT_CALL_CLASS;
    node->field = method_id + 1;
    if (node->count != count) {
        at_check_error(checker, node, "AS3204",
                       "Class constructor argument count differs from its declaration");
    }
    for (int i = 0; i < node->count; i++) {
        int expected =
            i < count ? (method ? method->locals[i + 1].type : object->fields[i]) : AT_ERROR;
        at_check_mismatch(checker, node->args[i], expected,
                          at_check_expression(checker, node->args[i], expected));
    }
    if (method) {
        at_check_function(checker->p, method);
    }
    return type;
}
