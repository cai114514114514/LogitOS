/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

/* Signature tables keep the checker independent of runtime implementation
 * details. Called only after lexical/module declarations have had priority. */
int at_check_runtime_builtin(Checker *checker, AtNode *node, Token name, int *result)
{
    if (at_check_region_builtin(checker, node, name, result) ||
        at_check_port_builtin(checker, node, name, result) ||
        at_check_command_builtin(checker, node, name, result)) {
        return 1;
    }
    int reading = at_scope_name_equal(name, "file_read");
    if (reading || at_scope_name_equal(name, "file_write")) {
        node->symbol = reading ? AT_CALL_FILE_READ : AT_CALL_FILE_WRITE;
        int expected = reading ? 1 : 2;
        if (node->count != expected) {
            at_check_error(checker, node, "AS3204",
                           reading ? "file_read expects a path string"
                                   : "file_write expects a path string and Bytes");
        }
        for (int index = 0; index < node->count; index++) {
            int type = index == 0 ? AT_STR : AT_BYTES;
            at_check_mismatch(checker, node->args[index], type,
                              at_check_expression(checker, node->args[index], type));
        }
        *result = reading ? AT_BYTES : AT_I64;
        return 1;
    }
    if (at_check_system_builtin(checker, node, name, result) ||
        at_check_memory_builtin(checker, node, name, result)) {
        return 1;
    }
    if (at_scope_name_equal(name, "Bytes")) {
        node->symbol = AT_CALL_BYTES;
        if (node->count != 1) {
            at_check_error(checker, node, "AS3204", "Bytes expects one Buffer, str or Bytes value");
        }
        for (int index = 0; index < node->count; index++) {
            int type = at_check_view_read(checker, node->args[index]);
            AtType *source = &checker->p->types[type];
            int byte_view = at_slice_kind(source->kind) && source->element == AT_U8;
            if (type && type != AT_STR && !byte_view &&
                !at_check_byte_storage(checker->p, type, 0)) {
                at_check_error(checker, node->args[index], "AS3202",
                               "Bytes requires a Buffer, UTF-8 str or Bytes value");
            }
        }
        *result = AT_BYTES;
        return 1;
    }
    if (at_scope_name_equal(name, "buffer") || at_scope_name_equal(name, "Buffer")) {
        node->symbol = AT_CALL_BUFFER;
        if (node->count != 1) {
            at_check_error(checker, node, "AS3204", "buffer expects one byte count");
        }
        for (int index = 0; index < node->count; index++) {
            at_check_mismatch(checker, node->args[index], AT_I64,
                              at_check_expression(checker, node->args[index], AT_I64));
        }
        *result = AT_BUFFER;
        return 1;
    }

    static const struct {
        const char *name;
        int symbol;
        int result;
    } signatures[] = {
        {"gc_collect", AT_CALL_GC_COLLECT, AT_VOID},
        {"gc_live_bytes", AT_CALL_GC_BYTES, AT_I64},
        {"gc_stats", AT_CALL_GC_OBJECTS, AT_I64},
        {"gc", AT_CALL_GC_RECLAIM, AT_I64},
        {"args", AT_CALL_ARGS, AT_LIST},
        {"caps", AT_CALL_CAPS, AT_CAP},
    };

    for (unsigned index = 0; index < sizeof signatures / sizeof signatures[0]; index++) {
        if (!at_scope_name_equal(name, signatures[index].name)) {
            continue;
        }
        node->symbol = signatures[index].symbol;
        if (node->count) {
            at_check_error(checker, node, "AS3204", "This builtin expects no arguments");
        }
        *result = signatures[index].result;
        if (*result == AT_LIST) {
            *result = at_compound_type(checker->p, AT_LIST, AT_STR, 0, node->module, node->token);
        }
        return 1;
    }
    return 0;
}
