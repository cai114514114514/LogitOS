/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"

Val at_ir_bytes_new(Gen *g, AtNode *node, Val result)
{
    Val source = at_ir_expression(g, node->args[0]);
    if (source.type == AT_BYTES) {
        /* An immutable value can be shared, including through Any/containers.
         * Buffer takes the copy path below; sharing that owner would silently
         * let a later byte store change an already published Bytes value. */
        return source;
    }
    Val data = at_ir_temp(g, 0);
    Val length = at_ir_temp(g, AT_I64);
    if (source.type == AT_STR || at_slice_kind(g->p->types[source.type].kind)) {
        /* Borrowed bytes become an owned immutable snapshot. Keep this on the
         * copy path; retaining the raw data pointer would outlive the loan. */
        at_ir_emit(g, "  %s = extractvalue { ptr, i64 } %s, 0\n", data.text, source.text);
        at_ir_emit(g, "  %s = extractvalue { ptr, i64 } %s, 1\n", length.text, source.text);
    } else {
        at_ir_emit(g, "  %s = call ptr @at_buffer_data(ptr %s)\n", data.text, source.text);
        at_ir_emit(g, "  %s = call i64 @at_buffer_len(ptr %s)\n", length.text, source.text);
    }
    Val oversized = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp ugt i64 %s, %lld\n", oversized.text, length.text,
               (long long)AT_BUFFER_LIMIT);
    at_ir_guard(g, oversized, node, AT_E_VALUE);
    at_ir_emit(g, "  %s = call ptr @at_bytes_copy(ptr %s, i64 %s)\n", result.text, data.text,
               length.text);
    at_ir_allocation(g, result, node, 1);
    return result;
}

Val at_ir_buffer_new(Gen *g, AtNode *node, Val result)
{
    Val length = at_ir_expression(g, node->args[0]);
    Val invalid = at_ir_temp(g, AT_BOOL);
    /* Unsigned comparison rejects negative counts as well as oversized ones,
     * before an allocation failure could obscure the actual input error. */
    at_ir_emit(g, "  %s = icmp ugt i64 %s, %lld\n", invalid.text, length.text,
               (long long)AT_BUFFER_LIMIT);
    at_ir_guard(g, invalid, node, AT_E_VALUE);
    at_ir_emit(g, "  %s = call ptr @at_buffer_new(i64 %s)\n", result.text, length.text);
    at_ir_allocation(g, result, node, 1);
    return result;
}

Val at_ir_buffer_at(Gen *g, Val buffer, Val index, AtNode *site)
{
    Val address = at_ir_temp(g, 0), invalid = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = call ptr @at_buffer_at(ptr %s, i64 %s)\n", address.text, buffer.text,
               index.text);
    at_ir_emit(g, "  %s = icmp eq ptr %s, null\n", invalid.text, address.text);
    at_ir_guard(g, invalid, site, AT_E_INDEX);
    return address;
}

Val at_ir_buffer_read(Gen *g, Val address)
{
    Val byte = at_ir_temp(g, AT_U8), result = at_ir_temp(g, AT_I64);
    at_ir_emit(g, "  %s = load i8, ptr %s\n", byte.text, address.text);
    at_ir_emit(g, "  %s = zext i8 %s to i64\n", result.text, byte.text);
    return result;
}

void at_ir_buffer_store(Gen *g, AtNode *assignment, Val value)
{
    AtNode *target = assignment->a;
    Val buffer = at_ir_expression(g, target->a);
    Val index = at_ir_expression(g, target->b);
    Val address = at_ir_buffer_at(g, buffer, index, target);
    at_ir_buffer_write(g, assignment, address, value);
}

void at_ir_buffer_write(Gen *g, AtNode *assignment, Val address, Val value)
{
    /* No write occurs on overflow, invalid index or an out-of-byte value.
     * The ordinary assignment path stores i64 and would corrupt adjacent bytes. */
    Val invalid = at_ir_temp(g, AT_BOOL), byte = at_ir_temp(g, AT_U8);
    at_ir_emit(g, "  %s = icmp ugt i64 %s, 255\n", invalid.text, value.text);
    at_ir_guard(g, invalid, assignment, AT_E_VALUE);
    at_ir_emit(g, "  %s = trunc i64 %s to i8\n", byte.text, value.text);
    at_ir_emit(g, "  store i8 %s, ptr %s\n", byte.text, address.text);
}
