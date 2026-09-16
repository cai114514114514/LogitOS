/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

int at_check_optional_conversion(Checker *checker, AtNode *node, int actual, int expected)
{
    AsTypedProject *project = checker->p;
    node->value_type = actual;
    node->conversion = AT_OPTION_IDENTITY;
    if (actual != expected && at_class_subtype(project, actual, expected)) {
        node->conversion = AT_CLASS_UPCAST;
        return expected;
    }
    if (project->types[expected].kind == AT_OPTIONAL && actual != expected &&
        (actual == AT_NONE || actual == project->types[expected].element ||
         at_class_subtype(project, actual, project->types[expected].element))) {
        node->conversion = AT_OPTION_WRAP;
        return expected;
    }
    /* A local Optional is an inline value, not an aliasable reference slot.
     * Calls cannot replace it. Fields/globals are deliberately excluded:
     * another alias or a callee may change them after a successful tag test. */
    if (!checker->preserve_optional && actual != expected &&
        (project->types[expected].kind != AT_OPTIONAL ||
         project->types[actual].element == expected) &&
        node->kind == AN_NAME && node->symbol >= 0 && project->types[actual].kind == AT_OPTIONAL &&
        (checker->f->locals[node->symbol].initialized & AT_LOCAL_PRESENT)) {
        node->conversion = AT_OPTION_UNWRAP;
        return project->types[actual].element;
    }
    return actual;
}

void at_optional_assume(Checker *checker, AtNode *condition, int truth)
{
    if (!condition) {
        return;
    }
    if (condition->kind == AN_UNARY && condition->op == T_NOT) {
        at_optional_assume(checker, condition->a, !truth);
        return;
    }
    if (condition->kind != AN_BINARY) {
        return;
    }
    if ((condition->op == T_AND && truth) || (condition->op == T_OR && !truth)) {
        at_optional_assume(checker, condition->a, truth);
        at_optional_assume(checker, condition->b, truth);
        return;
    }
    if (condition->op != T_EQ && condition->op != T_NE) {
        return;
    }
    AtNode *value = condition->a, *none = condition->b;
    if (value->kind == AN_NONE) {
        value = condition->b;
        none = condition->a;
    }
    if (none->kind != AN_NONE || value->kind != AN_NAME || value->symbol < 0 ||
        checker->p->types[value->value_type].kind != AT_OPTIONAL) {
        return;
    }
    AtLocal *local = &checker->f->locals[value->symbol];
    if (truth == (condition->op == T_NE)) {
        local->initialized |= AT_LOCAL_PRESENT;
    } else {
        local->initialized &= ~AT_LOCAL_PRESENT;
    }
}

void at_optional_forget(AtFunction *function)
{
    for (int i = 0; i < function->nlocals; i++) {
        function->locals[i].initialized &= ~AT_LOCAL_PRESENT;
    }
}

int at_check_conditional_value(Checker *checker, AtNode *node, int expected)
{
    at_check_mismatch(checker, node->a, AT_BOOL, at_check_expression(checker, node->a, AT_BOOL));
    /* Either branch can discover a new capture. Start its old flow slot empty. */
    int before[AT_LOCALS] = {0};
    for (int i = 0; i < checker->f->nlocals; i++) {
        before[i] = checker->f->locals[i].initialized;
    }
    at_optional_assume(checker, node->a, 1);
    int yes = at_check_expression(checker, node->b, expected);
    for (int i = 0; i < checker->f->nlocals; i++) {
        checker->f->locals[i].initialized =
            before[i] | (checker->f->locals[i].capture_parent ? AT_LOCAL_INITIALIZED : 0);
    }
    at_optional_assume(checker, node->a, 0);
    int no = at_check_expression(checker, node->c, expected ? expected : yes);
    for (int i = 0; i < checker->f->nlocals; i++) {
        checker->f->locals[i].initialized =
            before[i] | (checker->f->locals[i].capture_parent ? AT_LOCAL_INITIALIZED : 0);
    }
    if (!expected && yes != no && (yes == AT_NONE || no == AT_NONE)) {
        /* Only the explicit None alternative introduces a nullable type.
         * Other unequal alternatives remain errors, not implicit Any values. */
        int element = yes == AT_NONE ? no : yes;
        if (element != AT_VOID) {
            expected = checker->p->types[element].kind == AT_OPTIONAL
                           ? element
                           : at_compound_type(checker->p, AT_OPTIONAL, element, 0, node->module,
                                              node->token);
            at_optional_assume(checker, node->a, 1);
            yes = at_check_expression(checker, node->b, expected);
            for (int i = 0; i < checker->f->nlocals; i++) {
                checker->f->locals[i].initialized =
                    before[i] | (checker->f->locals[i].capture_parent ? AT_LOCAL_INITIALIZED : 0);
            }
            at_optional_assume(checker, node->a, 0);
            no = at_check_expression(checker, node->c, expected);
            for (int i = 0; i < checker->f->nlocals; i++) {
                checker->f->locals[i].initialized =
                    before[i] | (checker->f->locals[i].capture_parent ? AT_LOCAL_INITIALIZED : 0);
            }
        }
    }
    at_check_mismatch(checker, node->c, yes, no);
    if (yes == AT_VOID) {
        at_check_error(checker, node, "AS3202", "Conditional expressions must produce a value");
    }
    return yes;
}
