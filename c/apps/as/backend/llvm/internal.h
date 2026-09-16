/* SPDX-License-Identifier: MIT */
#ifndef AS_TYPED_LLVM_INTERNAL_H
#define AS_TYPED_LLVM_INTERNAL_H
#include "ir/model.h"

/* All lowering units share one register/label allocator and handler state.
 * Splitting code generation must never reset these counters inside a function. */
typedef struct {
    int type;
    char text[96];
} Val;

typedef struct AtCleanup {
    AtNode *owner;
    struct AtCleanup *previous;
    int handler_label; /* Handler outside the scope being released. */
    struct AtCleanup *handler_cleanup;
} AtCleanup;

typedef struct {
    AsTypedProject *p;
    FILE *out;
    AtFunction *f;
    int id, label, terminated, break_label, continue_label;
    int handler_label; /* Zero routes to the function's propagation block. */
    int failed;        /* Lowering failure must prevent artifact publication. */
    AtNode *caught;    /* Lexically active handler's independent stack record. */
    AtCleanup *cleanup;
    AtCleanup *handler_cleanup;
    AtCleanup *loop_cleanup;
    char types[AT_TYPES][128];
    char class_layouts[AT_TYPES][32];
} Gen;

void at_ir_emit(Gen *generator, const char *format, ...);
const char *at_ir_type(Gen *generator, int type);
const char *at_ir_class_layout(Gen *generator, int type);
Val at_ir_class_construct(Gen *generator, AtNode *node, Val result);
Val at_ir_bound_method(Gen *generator, AtNode *node, Val result);
void at_ir_class_scanner(Gen *generator, int type);
void at_ir_class_tables(Gen *generator);
void at_ir_class_ready(Gen *generator, Val receiver, AtNode *site);
void at_ir_class_field_initialized(Gen *generator, AtNode *field);
void at_ir_bound_adapter(Gen *generator, int method);
Val at_ir_value(int type, const char *text);
Val at_ir_temp(Gen *generator, int type);
int at_ir_label(Gen *generator);
void at_ir_mark(Gen *generator, int label);
void at_ir_jump(Gen *generator, int label);
void at_ir_throw(Gen *generator);
int at_ir_column(Gen *generator, AtNode *node);
void at_ir_guard(Gen *generator, Val bad, AtNode *node, int kind);
Val at_ir_capability(Gen *generator, AtNode *node, Val result);
Val at_ir_memory(Gen *generator, AtNode *node, Val result);
Val at_ir_region(Gen *generator, AtNode *node, Val result);
Val at_ir_region_borrow(Gen *generator, AtNode *node, Val result);
Val at_ir_region_at(Gen *generator, Val owner, Val index, AtNode *site, int writable);
void at_ir_require_raw(Gen *generator, AtNode *site);
Val at_ir_pointer_at(Gen *generator, Val pointer, Val index, AtNode *site);
Val at_ir_memory_text(Gen *generator, AtNode *node, Val result);
Val at_ir_system(Gen *generator, AtNode *node, Val result);
void at_ir_propagate(Gen *generator);
int at_ir_references(Gen *generator, int type);
void at_ir_root(Gen *generator, const char *slot, int type);
void at_ir_release_temporaries(Gen *generator, AtNode *statement);
void at_ir_allocation(Gen *generator, Val result, AtNode *site, int pointer);
Val at_ir_shift(Gen *generator, int operation, Val left, Val count, AtNode *site, int wrap);
Val at_ir_numeric(Gen *generator, int operation, Val left, Val right, AtNode *site, int wrap);
Val at_ir_convert(Gen *generator, Val value, int target, AtNode *site);
Val at_ir_text_operation(Gen *generator, Val left, Val right, int find);
Val at_ir_expression(Gen *generator, AtNode *node);
Val at_ir_address(Gen *generator, AtNode *node);
Val at_ir_list_at(Gen *generator, Val list, Val index, AtNode *site);
void at_ir_statements(Gen *generator, AtNode *node);
void at_ir_store_binding(Gen *generator, AtNode *binding, Val value);
void at_ir_unpack(Gen *generator, AtNode *node);
void at_ir_compound_assignment(Gen *generator, AtNode *node);
void at_ir_store_target(Gen *generator, AtNode *node, Val value);
Val at_ir_buffer_new(Gen *generator, AtNode *node, Val result);
Val at_ir_layout_address(Gen *generator, AtNode *field, Val owner);
Val at_ir_layout_read(Gen *generator, AtNode *field, Val result);
void at_ir_layout_store(Gen *generator, AtNode *field, Val value);
Val at_ir_bytes_new(Gen *generator, AtNode *node, Val result);
Val at_ir_bytes_decode(Gen *generator, AtNode *node, Val result);
void at_ir_runtime_status(Gen *generator, Val status, AtNode *site);
Val at_ir_file(Gen *generator, AtNode *node, Val result);
Val at_ir_port(Gen *generator, AtNode *node, Val result);
void at_ir_with(Gen *generator, AtNode *node);
void at_ir_cleanup_to(Gen *generator, AtCleanup *stop, int unwinding);
Val at_ir_command(Gen *generator, AtNode *node, Val result);
Val at_ir_command_operator(Gen *generator, AtNode *node);
void at_ir_port_loop(Gen *generator, AtNode *node);
Val at_ir_buffer_at(Gen *generator, Val buffer, Val index, AtNode *site);
Val at_ir_buffer_read(Gen *generator, Val address);
void at_ir_buffer_store(Gen *generator, AtNode *assignment, Val value);
void at_ir_buffer_write(Gen *generator, AtNode *site, Val address, Val value);
typedef void (*AtLoopBody)(Gen *generator, AtNode *context);
void at_ir_for_loop(Gen *generator, AtNode *loop, AtLoopBody body, AtNode *context);
Val at_ir_comprehension(Gen *generator, AtNode *node, Val result);
Val at_ir_closure(Gen *generator, AtNode *node, Val result);
void at_ir_function_locals(Gen *generator);
int at_ir_template_function(AsTypedProject *project, AtFunction *function);
Val at_ir_call(Gen *generator, AtNode *node, Val result);
Val at_ir_argument(Gen *generator, AtNode *node, int expected);
Val at_ir_indirect_call(Gen *generator, AtNode *node, Val result);
void at_ir_callable_adapter(Gen *generator, int function);
Val at_ir_equal(Gen *generator, Val left, Val right, AtNode *site);
Val at_ir_optional_wrap(Gen *generator, Val value, int type);
Val at_ir_optional_equal(Gen *generator, Val left, Val right, AtNode *site);
Val at_ir_sequence_contains(Gen *generator, AtNode *node, Val key, Val sequence);
void at_ir_range_bounds(Gen *generator, AtNode *node, Val *start, Val *stop, Val *step);
Val at_ir_range_new(Gen *generator, AtNode *node, Val result);
Val at_ir_range_length(Gen *generator, AtNode *node, Val range, Val result);
Val at_ir_range_index(Gen *generator, AtNode *node, Val result);
void at_ir_type_descriptions(Gen *generator);
void at_ir_array_definitions(Gen *generator);
void at_ir_format_value(Gen *generator, AtNode *site, AtNode *source, Val value, const char *out);
Val at_ir_dict_lookup(Gen *generator, AtNode *site, Val dictionary, Val key);
void at_ir_dict_store(Gen *generator, AtNode *site, Val dictionary, Val key, Val value);
Val at_ir_dict_literal(Gen *generator, AtNode *node, Val result);
Val at_ir_dict_call(Gen *generator, AtNode *node, Val result);
void at_ir_dict_callbacks(Gen *generator, int type);
void at_ir_dict_slots(Gen *generator, AtNode *node);
#endif
