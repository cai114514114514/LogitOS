/* SPDX-License-Identifier: MIT */
#include "completion_internal.h"
#include <string.h>

int at_completion_object_type(AsTypedProject *project, AtNode *field)
{
    if (field->function < 0 || field->function >= project->nfunctions) {
        return 0;
    }
    AtFunction *function = &project->functions[field->function];
    if (!function->checked) {
        /* A trailing dot is a recoverable syntax error. The recovered body
         * still carries its receiver; run the ordinary checker to learn its
         * type, rather than reproducing assignment inference in the editor.
         * Checking initializers learns globals but never executes user code. */
        for (int index = 0; index < project->ninitializers; index++) {
            at_check_function(project, &project->functions[project->initialization_order[index]]);
        }
        AtFunction *outer = function;
        while (outer->lexical_parent) {
            outer = &project->functions[outer->lexical_parent - 1];
        }
        at_check_function(project, outer);
        at_check_function(project, function);
    }
    int type = field->a->type;
    if (type <= 0 || type >= project->ntypes) {
        return 0;
    }
    int kind = project->types[type].kind;
    return kind == AT_STRUCT || kind == AT_CLASS ? type : 0;
}

void at_completion_object_items(AsTypedProject *project, AtNode *field, int type, FILE *out,
                                const char *prefix, size_t bytes, int *count)
{
    AtType *object = &project->types[type];
    /* super selects methods only. Ordinary fields, including leading-_ names,
     * follow the checker's current field visibility rather than module rules. */
    if (field->a->kind != AN_SUPER) {
        for (int index = 0; index < object->count; index++) {
            int owner = type;
            while (project->types[owner].base &&
                   index < project->types[project->types[owner].base].count) {
                owner = project->types[owner].base;
            }
            at_completion_item(project, out, object->names[index], "field", object->fields[index],
                               project->types[owner].module, object->field_tokens[index], prefix,
                               bytes, count);
        }
    }
    if (object->kind != AT_CLASS) {
        return;
    }
    int lexical_owner = at_class_lexical_owner(project, &project->functions[field->function]);
    for (int index = 0; index < project->nfunctions; index++) {
        AtFunction *method = &project->functions[index];
        if (!method->method_owner || method->template_id >= 0) {
            continue;
        }
        Token name = {.start = method->name, .len = (int)strlen(method->name)};
        /* This shared resolver also picks the most-derived override. Merely
         * walking base classes would duplicate overridden method candidates. */
        if (at_class_method(project, type, name) != index ||
            (method->name[0] == '_' && lexical_owner != method->method_owner)) {
            continue;
        }
        at_completion_item(project, out, method->name, "method", method->result, method->module,
                           method->token, prefix, bytes, count);
    }
}
