/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"
#include <stdlib.h>
#include <string.h>

/* Generics have two stages. The checker first validates a template against
 * the operations its constraints promise. Concrete calls then get independent
 * trees and signatures. Mutating the template for the first caller would make
 * a second caller silently reuse its types, slots and arithmetic widths. */

int at_compound_type(AsTypedProject *project, int kind, int element, int count, int module,
                     Token site)
{
    if (!element) {
        return AT_ERROR;
    }
    /* The first raw-pointer interface covers the existing integer widths.
     * Reject managed pointees here: a store through a raw pointer cannot
     * establish the GC ownership promised by a class/List/str field. */
    if (kind == AT_POINTER && !at_satisfies_constraint(project, element, AT_CONSTRAINT_INTEGER)) {
        at_error(project, module, site, "AS3812", "Ptr requires an Integer element type");
        return AT_ERROR;
    }
    for (int i = 0; i < project->ntypes; i++) {
        AtType *type = &project->types[i];
        if (type->kind == kind && type->element == element && type->count == count) {
            return i;
        }
    }
    int id = at_allocate_type(project, module, site);
    if (!id) {
        return AT_ERROR;
    }
    AtType *type = &project->types[id];
    type->kind = kind;
    type->element = element;
    type->count = count;
    type->module = module;
    if (kind == AT_ARRAY) {
        snprintf(type->name, sizeof type->name, "Array[%.64s, %d]", project->types[element].name,
                 count);
    } else {
        snprintf(type->name, sizeof type->name, "%s[%.64s]",
                 kind == AT_LIST        ? "List"
                 : kind == AT_OPTIONAL  ? "Optional"
                 : kind == AT_POINTER   ? "Ptr"
                 : kind == AT_MUT_SLICE ? "MutSlice"
                                        : "Slice",
                 project->types[element].name);
    }
    return id;
}

int at_dict_type(AsTypedProject *project, int key, int value, int module, Token site)
{
    if (!key || !value) {
        return AT_ERROR;
    }
    for (int i = 0; i < project->ntypes; i++) {
        AtType *type = &project->types[i];
        if (type->kind == AT_DICT && type->key == key && type->element == value) {
            return i;
        }
    }
    int id = at_allocate_type(project, module, site);
    if (!id) {
        return AT_ERROR;
    }
    AtType *type = &project->types[id];
    type->kind = AT_DICT;
    type->module = module;
    type->key = key;
    type->element = value;
    snprintf(type->name, sizeof type->name, "Dict[%.40s, %.40s]", project->types[key].name,
             project->types[value].name);
    return id;
}

int at_function_type(AsTypedProject *project, AtFunction *function, Token name)
{
    if (!function) {
        return AT_ERROR;
    }
    AtFunction *declaration = function;
    if (function->template_id >= 0) {
        declaration = &project->functions[function->template_id];
    }
    for (int i = 0; i < declaration->generic_count; i++) {
        int type = declaration->type_parameters[i];
        const char *spelling = project->types[type].name;
        if ((int)strlen(spelling) == name.len && !memcmp(spelling, name.start, (size_t)name.len)) {
            return function->template_id >= 0 ? function->type_arguments[i] : type;
        }
    }
    if (function->lexical_parent) {
        return at_function_type(project, &project->functions[function->lexical_parent - 1], name);
    }
    return AT_ERROR;
}

int at_has_parameter(AsTypedProject *project, int type)
{
    AtType *value = &project->types[type];
    if (value->kind == AT_PARAMETER) {
        return 1;
    }
    if (value->kind == AT_CALLABLE) {
        for (int i = 0; i < value->count; i++) {
            if (at_has_parameter(project, value->fields[i])) {
                return 1;
            }
        }
        return at_has_parameter(project, value->element);
    }
    if (value->kind == AT_DICT) {
        return at_has_parameter(project, value->key) || at_has_parameter(project, value->element);
    }
    if (value->kind == AT_ARRAY || at_slice_kind(value->kind) || value->kind == AT_LIST ||
        value->kind == AT_OPTIONAL || value->kind == AT_POINTER) {
        return at_has_parameter(project, value->element);
    }
    /* Source structs cannot declare generic fields yet; their nominal layout
     * is already closed when a function signature refers to them. */
    return 0;
}

int at_satisfies_constraint(AsTypedProject *project, int type, int constraint)
{
    if (type == AT_ERROR || type == AT_VOID) {
        return 0;
    }
    if (constraint == AT_CONSTRAINT_NONE) {
        return 1;
    }
    if (constraint == AT_CONSTRAINT_BYTE_STORAGE ||
        constraint == AT_CONSTRAINT_MUTABLE_BYTE_STORAGE) {
        return at_check_byte_storage(project, type,
                                     constraint == AT_CONSTRAINT_MUTABLE_BYTE_STORAGE);
    }
    AtType *value = &project->types[type];
    if (constraint == AT_CONSTRAINT_EQUATABLE || constraint == AT_CONSTRAINT_ORDERED) {
        if (value->kind == AT_OPTIONAL) {
            return constraint == AT_CONSTRAINT_EQUATABLE &&
                   at_satisfies_constraint(project, value->element, constraint);
        }
        if (value->kind == AT_PARAMETER) {
            int promised = value->constraint;
            return promised == AT_CONSTRAINT_NUMBER || promised == AT_CONSTRAINT_INTEGER ||
                   promised == AT_CONSTRAINT_ORDERED ||
                   (constraint == AT_CONSTRAINT_EQUATABLE &&
                    (promised == AT_CONSTRAINT_EQUATABLE || promised == AT_CONSTRAINT_HASHABLE ||
                     promised == AT_CONSTRAINT_BYTE_STORAGE ||
                     promised == AT_CONSTRAINT_MUTABLE_BYTE_STORAGE));
        }
        if (at_integer(project, type) || type == AT_F32 || type == AT_F64 || type == AT_BOOL ||
            type == AT_STR) {
            return 1;
        }
        return constraint == AT_CONSTRAINT_EQUATABLE &&
               (value->kind == AT_LIST || value->kind == AT_DICT || value->kind == AT_CALLABLE ||
                value->kind == AT_CLASS || type == AT_ANY || type == AT_NONE || type == AT_RANGE ||
                at_byte_storage_kind(value->kind) || type == AT_CAP || type == AT_COMMAND ||
                value->kind == AT_POINTER);
    }
    if (constraint == AT_CONSTRAINT_HASHABLE) {
        if (value->kind == AT_PARAMETER) {
            return value->constraint == AT_CONSTRAINT_HASHABLE ||
                   value->constraint == AT_CONSTRAINT_INTEGER;
        }
        return at_integer(project, type) || type == AT_BOOL || type == AT_STR;
    }
    if (value->kind == AT_PARAMETER) {
        return value->constraint == AT_CONSTRAINT_INTEGER ||
               (constraint == AT_CONSTRAINT_NUMBER && value->constraint == AT_CONSTRAINT_NUMBER);
    }
    if (at_integer(project, type)) {
        return 1;
    }
    return constraint == AT_CONSTRAINT_NUMBER && (type == AT_F32 || type == AT_F64);
}

static int parameter_index(AtFunction *function, int type)
{
    for (int i = 0; i < function->generic_count; i++) {
        if (function->type_parameters[i] == type) {
            return i;
        }
    }
    return -1;
}

int at_bind_result_context(AsTypedProject *project, AtFunction *function, int pattern, int actual,
                           int bindings[AT_TYPE_PARAMETERS])
{
    /* Arguments are authoritative. A result annotation may fill unresolved
     * parameters (such as sets.empty()), but cannot retarget inferred ones.
     * Work on a caller-owned copy so a partial mismatch is discarded whole. */
    if (!pattern || !actual) {
        return 0;
    }
    int parameter = parameter_index(function, pattern);
    if (parameter >= 0) {
        if (bindings[parameter]) {
            return bindings[parameter] == actual;
        }
        int unit_result =
            actual == AT_VOID && project->types[pattern].constraint == AT_CONSTRAINT_NONE;
        if (!unit_result &&
            !at_satisfies_constraint(project, actual, project->types[pattern].constraint)) {
            return 0;
        }
        bindings[parameter] = actual;
        return 1;
    }
    AtType *expected = &project->types[pattern];
    AtType *found = &project->types[actual];
    if (expected->kind != found->kind) {
        return 0;
    }
    if (expected->kind == AT_CALLABLE) {
        if (expected->count != found->count) {
            return 0;
        }
        for (int i = 0; i < expected->count; i++) {
            if (!at_bind_result_context(project, function, expected->fields[i], found->fields[i],
                                        bindings)) {
                return 0;
            }
        }
        return at_bind_result_context(project, function, expected->element, found->element,
                                      bindings);
    }
    if (expected->kind == AT_DICT) {
        return at_bind_result_context(project, function, expected->key, found->key, bindings) &&
               at_bind_result_context(project, function, expected->element, found->element,
                                      bindings);
    }
    if (expected->kind == AT_ARRAY || expected->kind == AT_LIST || at_slice_kind(expected->kind) ||
        expected->kind == AT_OPTIONAL || expected->kind == AT_POINTER) {
        return expected->count == found->count &&
               at_bind_result_context(project, function, expected->element, found->element,
                                      bindings);
    }
    return pattern == actual;
}

int at_bind_type(AsTypedProject *project, AtFunction *function, int pattern, int actual,
                 int bindings[AT_TYPE_PARAMETERS], AtNode *site)
{
    if (!pattern || !actual) {
        return 0; /* Preserve the original error rather than add a cascade. */
    }
    int parameter = parameter_index(function, pattern);
    if (parameter >= 0) {
        int unit_result =
            actual == AT_VOID && project->types[pattern].constraint == AT_CONSTRAINT_NONE;
        if (!unit_result &&
            !at_satisfies_constraint(project, actual, project->types[pattern].constraint)) {
            at_error(project, site->module, site->token, "AS3601",
                     "Type argument does not satisfy the declared protocol constraint");
            return 0;
        }
        if (bindings[parameter] && bindings[parameter] != actual) {
            at_error(project, site->module, site->token, "AS3602",
                     "Arguments infer conflicting types for the same generic parameter");
            return 0;
        }
        bindings[parameter] = actual;
        return 1;
    }
    AtType *expected = &project->types[pattern];
    AtType *found = &project->types[actual];
    if (expected->kind == AT_OPTIONAL && found->kind != AT_OPTIONAL) {
        return at_bind_type(project, function, expected->element, actual, bindings, site);
    }
    if (expected->kind == AT_CALLABLE && found->kind == AT_CALLABLE &&
        expected->count == found->count) {
        int valid = 1;
        for (int i = 0; i < expected->count; i++) {
            valid &= at_bind_type(project, function, expected->fields[i], found->fields[i],
                                  bindings, site);
        }
        return at_bind_type(project, function, expected->element, found->element, bindings, site) &&
               valid;
    }
    if (expected->kind == AT_DICT && found->kind == AT_DICT) {
        int key = at_bind_type(project, function, expected->key, found->key, bindings, site);
        int value =
            at_bind_type(project, function, expected->element, found->element, bindings, site);
        return key && value;
    }
    if ((expected->kind == AT_OPTIONAL && found->kind == AT_OPTIONAL) ||
        (expected->kind == AT_MUT_SLICE && found->kind == AT_MUT_SLICE) ||
        (expected->kind == AT_POINTER && found->kind == AT_POINTER) ||
        (expected->kind == AT_LIST && found->kind == AT_LIST) ||
        (expected->kind == AT_ARRAY && found->kind == AT_ARRAY &&
         expected->count == found->count) ||
        (expected->kind == AT_SLICE && (found->kind == AT_ARRAY || found->kind == AT_SLICE))) {
        return at_bind_type(project, function, expected->element, found->element, bindings, site);
    }
    if (pattern != actual) {
        at_error(project, site->module, site->token, "AS3602",
                 "Argument type differs from the generic function signature");
        return 0;
    }
    return 1;
}

int at_substitute_type(AsTypedProject *project, AtFunction *function, int type,
                       const int bindings[AT_TYPE_PARAMETERS])
{
    int parameter = parameter_index(function, type);
    if (parameter >= 0) {
        return bindings[parameter];
    }
    AtType *original = &project->types[type];
    if (original->kind == AT_CALLABLE) {
        int parameters[AT_ARGS];
        for (int i = 0; i < original->count; i++) {
            parameters[i] = at_substitute_type(project, function, original->fields[i], bindings);
        }
        int result = at_substitute_type(project, function, original->element, bindings);
        return at_callable_type(project, parameters, original->count, result, function->module,
                                function->token);
    }
    if (original->kind == AT_DICT) {
        int key = at_substitute_type(project, function, original->key, bindings);
        int value = at_substitute_type(project, function, original->element, bindings);
        return at_dict_type(project, key, value, function->module, function->token);
    }
    if (original->kind != AT_ARRAY && !at_slice_kind(original->kind) && original->kind != AT_LIST &&
        original->kind != AT_OPTIONAL && original->kind != AT_POINTER) {
        return type;
    }
    int element = at_substitute_type(project, function, original->element, bindings);
    return at_compound_type(project, original->kind, element, original->count, function->module,
                            function->token);
}

typedef struct {
    AsTypedProject *project;
    AtFunction *declaration;
    const int *bindings;
    int function_id;
} CloneContext;

static AtNode *clone_node(CloneContext *context, AtNode *source, int depth);

static int clone_closure(CloneContext *context, int original_id, AtNode *site)
{
    AsTypedProject *project = context->project;
    if (project->nfunctions == AT_FUNCTIONS) {
        at_error(project, site->module, site->token, "AS3600", "Closure instance limit reached");
        return original_id;
    }
    AtFunction *original = &project->functions[original_id];
    int id = project->nfunctions++;
    AtFunction *instance = &project->functions[id];
    *instance = *original;
    instance->template_id = original_id;
    instance->lexical_parent = context->function_id + 1;
    instance->checked = instance->checking = instance->capture_count = 0;
    instance->active_checker = NULL;
    instance->result =
        at_substitute_type(project, context->declaration, original->result, context->bindings);
    instance->nlocals = instance->nparams;
    memset(instance->locals + instance->nparams, 0,
           (size_t)(AT_LOCALS - instance->nparams) * sizeof *instance->locals);
    for (int index = 0; index < instance->nparams; index++) {
        AtLocal *parameter = &instance->locals[index];
        parameter->type =
            at_substitute_type(project, context->declaration, parameter->type, context->bindings);
        parameter->initialized = AT_LOCAL_INITIALIZED;
        parameter->captured = parameter->capture_parent = 0;
    }
    /* Nested bodies belong to the same outer type substitution, but have a
     * different lexical parent and node-storage owner in every instantiation. */
    CloneContext nested = *context;
    nested.function_id = id;
    instance->body = clone_node(&nested, original->body, 0);
    return id;
}

static AtNode *clone_node(CloneContext *context, AtNode *source, int depth)
{
    AsTypedProject *project = context->project;
    AtNode *head = NULL;
    AtNode **tail = &head;
    /* Statement chains are iterative. Their length does not consume C stack;
     * only expression/block nesting contributes to the explicit depth bound. */
    for (; source && !project->oom; source = source->next) {
        if (depth > 128 || project->nnodes == 65536) {
            at_error(project, source->module, source->token, "AS3600",
                     "Generic syntax expansion limit reached");
            return head;
        }
        if (project->nnodes == project->nodecap) {
            int capacity = project->nodecap ? project->nodecap * 2 : 128;
            AtNode **nodes = realloc(project->nodes, (size_t)capacity * sizeof *nodes);
            if (!nodes) {
                project->oom = 1;
                return head;
            }
            project->nodes = nodes;
            project->nodecap = capacity;
        }
        AtNode *node = calloc(1, sizeof *node);
        if (!node) {
            project->oom = 1;
            return head;
        }
        *node = *source;
        node->id = project->nnodes;
        node->function = context->function_id;
        node->args = NULL;
        node->next = NULL;
        project->nodes[project->nnodes++] = node;
        *tail = node;
        tail = &node->next;
        node->type =
            at_substitute_type(project, context->declaration, source->type, context->bindings);
        if (source->kind == AN_CLOSURE) {
            node->symbol = clone_closure(context, source->symbol, source);
        }
        node->a = clone_node(context, source->a, depth + 1);
        node->b = clone_node(context, source->b, depth + 1);
        node->c = clone_node(context, source->c, depth + 1);
        if (source->count) {
            node->args = calloc((size_t)source->count, sizeof *node->args);
            if (!node->args) {
                project->oom = 1;
                return head;
            }
            for (int i = 0; i < source->count; i++) {
                node->args[i] = clone_node(context, source->args[i], depth + 1);
            }
        }
    }
    return head;
}

int at_specialize(AsTypedProject *project, int template_id, const int bindings[AT_TYPE_PARAMETERS],
                  AtNode *site, int *result_type)
{
    AtFunction *declaration = &project->functions[template_id];
    int abstract = 0;
    for (int i = 0; i < declaration->generic_count; i++) {
        if (!bindings[i]) {
            at_error(project, site->module, site->token, "AS3602",
                     "A generic parameter cannot be inferred from arguments or result context");
            return -1;
        }
        abstract |= at_has_parameter(project, bindings[i]);
    }
    *result_type = at_substitute_type(project, declaration, declaration->result, bindings);
    if (abstract) {
        /* A call inside another template only checks/substitutes its signature.
         * It must not manufacture a machine function with an unknown layout. */
        return template_id;
    }
    for (int i = 0; i < project->nfunctions; i++) {
        AtFunction *function = &project->functions[i];
        if (function->template_id == template_id &&
            !memcmp(function->type_arguments, bindings,
                    (size_t)declaration->generic_count * sizeof(int))) {
            return i;
        }
    }
    if (project->nfunctions == AT_FUNCTIONS) {
        at_error(project, site->module, site->token, "AS3600",
                 "Generic function instance limit reached");
        return -1;
    }
    /* Publish the signature in the cache before checking its body. A recursive
     * call with the same arguments can then reuse this instance. Each tree is
     * checked again, preserving width/borrow/definite-assignment guarantees. */
    int id = project->nfunctions++;
    AtFunction *instance = &project->functions[id];
    *instance = *declaration;
    instance->template_id = template_id;
    instance->generic_count = 0;
    instance->checked = instance->checking = 0;
    instance->active_checker = NULL;
    instance->result = *result_type;
    memcpy(instance->type_arguments, bindings, sizeof instance->type_arguments);
    instance->nlocals = instance->nparams;
    memset(instance->locals + instance->nparams, 0,
           (size_t)(AT_LOCALS - instance->nparams) * sizeof *instance->locals);
    for (int i = 0; i < instance->nparams; i++) {
        instance->locals[i].type =
            at_substitute_type(project, declaration, declaration->locals[i].type, bindings);
        instance->locals[i].initialized = 1;
    }
    CloneContext context = {project, declaration, bindings, id};
    instance->body = clone_node(&context, declaration->body, 0);
    return id;
}
