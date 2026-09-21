/* SPDX-License-Identifier: MIT */
#include "ir/model.h"
#include <stdio.h>
#include <string.h>

int at_allocate_type(AsTypedProject *project, int module, Token site)
{
    if (project->ntypes >= AT_TYPES) {
        /* The arena remains intact for editor diagnostics. Repeating this for
         * every signature would obscure the first declaration that exceeded
         * the snapshot's resource budget. Type zero is the poison type. */
        if (!project->type_limit_reported) {
            at_error(project, module, site, "AS3600", "Project type capacity exceeded");
            project->type_limit_reported = 1;
        }
        return AT_ERROR;
    }
    int id = project->ntypes++;
    project->types[id].declaration = site;
    return id;
}

/* Generic struct instantiation. The template's own fields may name its
 * parameters (including through containers and other generic structs), so
 * materializing Pair[i64] substitutes every field type, and a nested
 * Pair[Pair[T]] re-enters this function through at_substitute_type's struct
 * case. Two consequences are load-bearing:
 *
 *  - The instance id is allocated BEFORE its fields are substituted, and a
 *    re-entrant call with the same (template, arguments) answers that id:
 *    self-referencing fields (next: Optional[Node[T]]) need the forward
 *    answer. count stays -1 until the body is complete. An actually
 *    infinite layout (by-value self containment) is still refused later by
 *    the ordinary struct recursion check -- this ordering only makes the
 *    finite cases well-defined.
 *  - Constraint checks run here, once per materialization, not per use site:
 *    Pair[str] against Pair[T: Number] fails at the annotation/constructor
 *    that first asks for it, with that source location. */
int at_instantiate_struct(AsTypedProject *project, int template, const int *arguments, int count,
                          int module, Token site)
{
    AtType *pattern = &project->types[template];
    if (pattern->kind != AT_STRUCT || !pattern->generic_count || pattern->template_id >= 0) {
        at_error(project, module, site, "AS3601", "Only a generic struct template can instantiate");
        return AT_ERROR;
    }
    if (count != pattern->generic_count) {
        at_error(project, module, site, "AS3601",
                 "Generic struct argument count differs from its declaration");
        return AT_ERROR;
    }
    for (int i = 0; i < project->ntypes; i++) {
        AtType *existing = &project->types[i];
        if (existing->kind == AT_STRUCT && existing->template_id == template &&
            existing->count >= 0 && existing->generic_count == 0 &&
            !memcmp(existing->type_arguments, arguments, sizeof(int) * count)) {
            return i;
        }
        /* Under construction: answer the forward reference (see header). */
        if (existing->kind == AT_STRUCT && existing->template_id == template &&
            existing->count == -1 && existing->generic_count == 0 &&
            !memcmp(existing->type_arguments, arguments, sizeof(int) * count)) {
            return i;
        }
    }
    for (int i = 0; i < count; i++) {
        AtType *parameter = &project->types[pattern->type_parameters[i]];
        if (!at_satisfies_constraint(project, arguments[i], parameter->constraint)) {
            at_error(project, module, site, "AS3601",
                     "Generic struct type argument does not satisfy its declared constraint");
            return AT_ERROR;
        }
    }
    int id = at_allocate_type(project, module, site);
    if (!id) {
        return AT_ERROR;
    }
    AtType *instance = &project->types[id];
    instance->kind = AT_STRUCT;
    instance->module = pattern->module; /* Nominal identity stays the declarant. */
    instance->template_id = template;
    instance->generic_count = 0;
    memcpy(instance->type_arguments, arguments, sizeof(int) * count);
    int length = snprintf(instance->name, sizeof instance->name, "%s[", pattern->name);
    for (int i = 0; i < count && length > 0; i++) {
        length += snprintf(instance->name + length, sizeof instance->name - (size_t)length, "%s%s",
                           project->types[arguments[i]].name, i + 1 == count ? "" : ", ");
    }
    if (length > 0) {
        snprintf(instance->name + length, sizeof instance->name - (size_t)length, "]");
    }
    instance->count = -1; /* Forward-reference sentinel until fields land. */
    memcpy(instance->names, pattern->names, sizeof instance->names);
    memcpy(instance->field_tokens, pattern->field_tokens, sizeof instance->field_tokens);

    /* Substitute fields through a throwaway function context so every
     * composite kind (Optional/List/Dict/Callable/struct) reuses the existing
     * substitution machinery, including its nested-instantiation path. */
    AtFunction bindings = {0};
    bindings.generic_count = pattern->generic_count;
    memcpy(bindings.type_parameters, pattern->type_parameters, sizeof bindings.type_parameters);
    for (int i = 0; i < pattern->count; i++) {
        instance->fields[i] = at_substitute_type(project, &bindings, pattern->fields[i], arguments);
    }
    instance->count = pattern->count;
    return id;
}
