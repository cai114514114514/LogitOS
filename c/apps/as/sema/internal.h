/* SPDX-License-Identifier: MIT */
#ifndef AS_TYPED_CHECK_INTERNAL_H
#define AS_TYPED_CHECK_INTERNAL_H
#include "ir/model.h"

typedef struct AtScopedBinding {
    Token name;
    int local;
    struct AtScopedBinding *previous;
} AtScopedBinding;

/* One function's checking state. Flow analysis owns initialization facts;
 * scope resolution owns binding identity. Neither owns project allocations. */
typedef struct AtChecker {
    AsTypedProject *p;
    AtFunction *f;
    int loops;
    int handlers;
    int unsafe_depth;      /* Lexical to this function; nested functions start at zero. */
    int preserve_optional; /* None tests inspect the tag, even after a previous narrowing. */
    int field_receiver;    /* Reading self as the base of one field is not an escape. */
    AtNode *assignment_target;
    AtNode *resource_acquisition; /* Only the exact with RHS may create an owner. */
    AtNode *resource_use;         /* A method/iterator borrows its exact receiver. */
    AtNode *creating_closure;  /* Named recursive bindings are initialized by this construction. */
    AtScopedBinding *bindings; /* Expression scopes; entries live on the checker call stack. */
} Checker;

int at_scope_name_equal(Token token, const char *name);
int at_check_byte_storage(AsTypedProject *project, int type, int writable);
void at_check_error(Checker *checker, AtNode *node, const char *code, const char *message);
void at_check_mismatch(Checker *checker, AtNode *node, int expected, int actual);
int at_scope_local(AtFunction *function, Token token);
int at_scope_resolve_local(Checker *checker, Token token);
int at_closure_capture(Checker *checker, Token name);
int at_check_closure(Checker *checker, AtNode *node, int expected);
int at_scope_explicit_global(Checker *checker, Token name);
int at_scope_read_global(Checker *checker, AtNode *node, int id);
int at_scope_qualified_global(Checker *checker, AtNode *node);
int at_scope_import_module(Checker *checker, Token name);
int at_scope_add_local(Checker *checker, AtNode *node, int type);
void at_scope_collect_bindings(Checker *checker);
int at_scope_function(Checker *checker, AtNode *node);
int at_scope_constructor(Checker *checker, AtNode *node);
void at_scope_check_imports(AsTypedProject *project);
int at_check_number(AsTypedProject *project, int type);
int at_check_expression(Checker *checker, AtNode *node, int expected);
int at_check_runtime_builtin(Checker *checker, AtNode *node, Token name, int *result);
int at_check_system_constant(AtNode *node);
int at_check_capability_call(Checker *checker, AtNode *node, int *result);
int at_check_bytes_call(Checker *checker, AtNode *node, int *result);
int at_check_port_call(Checker *checker, AtNode *node, int *result);
int at_check_port_builtin(Checker *checker, AtNode *node, Token name, int *result);
/* Returns the number of scoped owners (one or two), or zero after an error. */
int at_check_resource_binding(Checker *checker, AtNode *node);
int at_check_command_builtin(Checker *checker, AtNode *node, Token name, int *result);
int at_check_command_call(Checker *checker, AtNode *node, int *result);
int at_resource_type(int type);
int at_check_resource_receiver(Checker *checker, AtNode *node);
int at_check_region_builtin(Checker *checker, AtNode *node, Token name, int *result);
int at_check_region_call(Checker *checker, AtNode *node, int *result);
void at_check_region_flow(AsTypedProject *project);
void at_check_region_borrows(AsTypedProject *project);
int at_check_borrow_argument(Checker *checker, AtNode *node, int expected);
int at_check_view_read(Checker *checker, AtNode *node);
void at_check_resource_types(AsTypedProject *project);
int at_check_memory_builtin(Checker *checker, AtNode *node, Token name, int *result);
int at_check_pointer_builtin(Checker *checker, AtNode *node, Token name, int *result);
int at_check_system_builtin(Checker *checker, AtNode *node, Token name, int *result);
int at_check_function_value(Checker *checker, AtNode *node, int declaration, int expected);
int at_check_indirect_call(Checker *checker, AtNode *node);
void at_check_function(AsTypedProject *project, AtFunction *function);
int at_check_optional_conversion(Checker *checker, AtNode *node, int actual, int expected);
void at_optional_assume(Checker *checker, AtNode *condition, int truth);
void at_optional_forget(AtFunction *function);
int at_check_conditional_value(Checker *checker, AtNode *node, int expected);
int at_check_comprehension(Checker *checker, AtNode *node, int expected);
void at_check_unpack(Checker *checker, AtNode *node);
int at_class_method(AsTypedProject *project, int type, Token name);
int at_class_subtype(AsTypedProject *project, int actual, int expected);
void at_class_check_overrides(AsTypedProject *project);
int at_class_super(Checker *checker, AtNode *node);
int at_class_lexical_owner(AsTypedProject *project, AtFunction *function);
int at_class_receiver_owner(AsTypedProject *project, AtFunction *function, int slot);
int at_class_constructor(Checker *checker, AtNode *node, int type);
int at_class_method_value(Checker *checker, AtNode *node, int method);
int at_class_initializing(Checker *checker);
int at_class_initialized(Checker *checker);
void at_class_require_initialized(Checker *checker, AtNode *site);
void at_class_field_use(Checker *checker, AtNode *node);
void at_class_field_store(Checker *checker, AtNode *node);
#endif
