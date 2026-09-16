/* SPDX-License-Identifier: MIT */
#include "backend/llvm/internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Checked machine-width arithmetic and scalar conversions. Every trap uses
 * the shared source/handler helpers, keeping O0 and O2 behavior identical. */

Val at_ir_shift(Gen *g, int operation, Val left, Val count, AtNode *site, int wrap)
{
    const char *type = at_ir_type(g, left.type);
    int bits = at_bits(g->p, left.type);
    int signed_value = at_signed(g->p, left.type);
    Val invalid = at_ir_temp(g, AT_BOOL);

    /* LLVM shifts by a negative count or a count >= width are poison. An
     * unsigned comparison rejects both before LLVM gets to evaluate the shift. */
    at_ir_emit(g, "  %s = icmp uge %s %s, %d\n", invalid.text, type, count.text, bits);
    at_ir_guard(g, invalid, site, 5);

    Val result = at_ir_temp(g, left.type);
    const char *instruction = operation == T_SHL ? "shl" : signed_value ? "ashr" : "lshr";
    at_ir_emit(g, "  %s = %s %s %s, %s\n", result.text, instruction, type, left.text, count.text);
    if (operation == T_SHL && !wrap) {
        /* Reversing a representable multiplication by 2**count gives the
         * original value. Arithmetic reversal also catches a changed sign. */
        Val restored = at_ir_temp(g, left.type);
        Val overflow = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = %s %s %s, %s\n", restored.text, signed_value ? "ashr" : "lshr", type,
                   result.text, count.text);
        at_ir_emit(g, "  %s = icmp ne %s %s, %s\n", overflow.text, type, restored.text, left.text);
        at_ir_guard(g, overflow, site, 1);
    }
    return result;
}

static Val checked_power(Gen *g, Val base, Val exponent, AtNode *site)
{
    const char *type = at_ir_type(g, base.type);
    if (at_signed(g->p, exponent.type)) {
        Val negative = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = icmp slt %s %s, 0\n", negative.text, type, exponent.text);
        at_ir_guard(g, negative, site, 5);
    }

    /* Exponentiation by squaring needs O(log exponent) checked multiplies.
     * Crucially, never square the base after the last exponent bit: 100**1
     * fits i8 even though the unused 100*100 intermediate would overflow. */
    int entry = at_ir_label(g), loop = at_ir_label(g), body = at_ir_label(g);
    int multiply = at_ir_label(g), multiplied = at_ir_label(g), skip = at_ir_label(g);
    int merge = at_ir_label(g), square = at_ir_label(g), latch = at_ir_label(g),
        done = at_ir_label(g);
    Val factor = at_ir_temp(g, base.type), remaining = at_ir_temp(g, exponent.type);
    Val accumulated = at_ir_temp(g, base.type), next = at_ir_temp(g, base.type);
    Val shifted = at_ir_temp(g, exponent.type), squared = at_ir_temp(g, base.type);
    at_ir_jump(g, entry);
    at_ir_mark(g, entry);
    at_ir_jump(g, loop);
    at_ir_mark(g, loop);
    at_ir_emit(g, "  %s = phi %s [ %s, %%b%d ], [ %s, %%b%d ]\n", factor.text, type, base.text,
               entry, squared.text, latch);
    at_ir_emit(g, "  %s = phi %s [ %s, %%b%d ], [ %s, %%b%d ]\n", remaining.text, type,
               exponent.text, entry, shifted.text, latch);
    at_ir_emit(g, "  %s = phi %s [ 1, %%b%d ], [ %s, %%b%d ]\n", accumulated.text, type, entry,
               next.text, latch);
    Val empty = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp eq %s %s, 0\n", empty.text, type, remaining.text);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", empty.text, done, body);

    at_ir_mark(g, body);
    Val bit = at_ir_temp(g, base.type), odd = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = and %s %s, 1\n", bit.text, type, remaining.text);
    at_ir_emit(g, "  %s = icmp ne %s %s, 0\n", odd.text, type, bit.text);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", odd.text, multiply, skip);
    at_ir_mark(g, multiply);
    Val product = at_ir_numeric(g, T_STAR, accumulated, factor, site, 0);
    at_ir_jump(g, multiplied);
    at_ir_mark(g, multiplied);
    at_ir_jump(g, merge);
    at_ir_mark(g, skip);
    at_ir_jump(g, merge);
    at_ir_mark(g, merge);
    at_ir_emit(g, "  %s = phi %s [ %s, %%b%d ], [ %s, %%b%d ]\n", next.text, type, product.text,
               multiplied, accumulated.text, skip);
    at_ir_emit(g, "  %s = lshr %s %s, 1\n", shifted.text, type, remaining.text);
    Val finished = at_ir_temp(g, AT_BOOL);
    at_ir_emit(g, "  %s = icmp eq %s %s, 0\n", finished.text, type, shifted.text);
    at_ir_emit(g, "  br i1 %s, label %%b%d, label %%b%d\n", finished.text, done, square);
    at_ir_mark(g, square);
    Val square_value = at_ir_numeric(g, T_STAR, factor, factor, site, 0);
    /* A named latch makes phi predecessors independent of guard()'s blocks. */
    at_ir_emit(g, "  %s = add %s %s, 0\n", squared.text, type, square_value.text);
    at_ir_jump(g, latch);
    at_ir_mark(g, latch);
    at_ir_jump(g, loop);
    at_ir_mark(g, done);
    Val result = at_ir_temp(g, base.type);
    at_ir_emit(g, "  %s = phi %s [ %s, %%b%d ], [ %s, %%b%d ]\n", result.text, type,
               accumulated.text, loop, next.text, merge);
    return result;
}

Val at_ir_text_operation(Gen *g, Val left, Val right, int find)
{
    Val left_ptr = at_ir_temp(g, 0), left_len = at_ir_temp(g, AT_I64);
    Val right_ptr = at_ir_temp(g, 0), right_len = at_ir_temp(g, AT_I64);
    at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", left_ptr.text, at_ir_type(g, AT_STR),
               left.text);
    at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", left_len.text, at_ir_type(g, AT_STR),
               left.text);
    at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", right_ptr.text, at_ir_type(g, AT_STR),
               right.text);
    at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", right_len.text, at_ir_type(g, AT_STR),
               right.text);
    Val result = at_ir_temp(g, find ? AT_I64 : AT_I32);
    at_ir_emit(g, "  %s = call %s @at_text_%s(ptr %s, i64 %s, ptr %s, i64 %s)\n", result.text,
               at_ir_type(g, result.type), find ? "find" : "compare", left_ptr.text, left_len.text,
               right_ptr.text, right_len.text);
    return result;
}

Val at_ir_numeric(Gen *g, int op, Val a, Val b, AtNode *n, int wrap)
{
    if (a.type == AT_BYTES) {
        Val equal = at_ir_temp(g, AT_I32);
        Val result = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = call i32 @at_bytes_equal(ptr %s, ptr %s)\n", equal.text, a.text,
                   b.text);
        at_ir_emit(g, "  %s = icmp %s i32 %s, 0\n", result.text, op == T_EQ ? "ne" : "eq",
                   equal.text);
        return result;
    }
    if (a.type == AT_STR) {
        if (op == T_PLUS || op == T_STAR) {
            Val pointer = at_ir_temp(g, 0), length = at_ir_temp(g, AT_I64),
                status = at_ir_temp(g, AT_I32);
            at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", pointer.text, at_ir_type(g, AT_STR),
                       a.text);
            at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", length.text, at_ir_type(g, AT_STR),
                       a.text);
            if (op == T_PLUS) {
                Val right = at_ir_temp(g, 0), size = at_ir_temp(g, AT_I64);
                at_ir_emit(g, "  %s = extractvalue %s %s, 0\n", right.text, at_ir_type(g, AT_STR),
                           b.text);
                at_ir_emit(g, "  %s = extractvalue %s %s, 1\n", size.text, at_ir_type(g, AT_STR),
                           b.text);
                at_ir_emit(
                    g,
                    "  %s = call i32 @at_text_concat(ptr %%storage%d, ptr %s, i64 %s, ptr %s, i64 "
                    "%s)\n",
                    status.text, n->id, pointer.text, length.text, right.text, size.text);
            } else {
                at_ir_emit(
                    g, "  %s = call i32 @at_text_repeat(ptr %%storage%d, ptr %s, i64 %s, i64 %s)\n",
                    status.text, n->id, pointer.text, length.text, b.text);
            }
            at_ir_allocation(g, status, n, 0);
            Val result = at_ir_temp(g, AT_STR);
            at_ir_emit(g, "  %s = load %s, ptr %%storage%d\n", result.text, at_ir_type(g, AT_STR),
                       n->id);
            return result;
        }
        Val result = at_ir_temp(g, AT_BOOL);
        if (op == T_IN) {
            Val found = at_ir_text_operation(g, b, a, 1);
            at_ir_emit(g, "  %s = icmp sge i64 %s, 0\n", result.text, found.text);
        } else {
            Val compared = at_ir_text_operation(g, a, b, 0);
            const char *predicate = op == T_EQ   ? "eq"
                                    : op == T_NE ? "ne"
                                    : op == T_LT ? "slt"
                                    : op == T_LE ? "sle"
                                    : op == T_GT ? "sgt"
                                                 : "sge";
            at_ir_emit(g, "  %s = icmp %s i32 %s, 0\n", result.text, predicate, compared.text);
        }
        return result;
    }
    if (op == T_SHL || op == T_SHR) {
        return at_ir_shift(g, op, a, b, n, wrap);
    }
    if (op == T_POW) {
        return checked_power(g, a, b, n);
    }
    Val v = at_ir_temp(g, a.type);
    const char *t = at_ir_type(g, a.type);
    int fp = a.type == AT_F32 || a.type == AT_F64;
    if (op >= T_EQ && op <= T_GE) {
        const char *pred = op == T_EQ   ? "eq"
                           : op == T_NE ? "ne"
                           : op == T_LT ? "lt"
                           : op == T_LE ? "le"
                           : op == T_GT ? "gt"
                                        : "ge";
        char p[16];
        if (fp) {
            snprintf(p, sizeof p, "%s%s", op == T_NE ? "u" : "o", pred);
        } else {
            snprintf(p, sizeof p, "%s%s",
                     op == T_EQ || op == T_NE  ? ""
                     : at_signed(g->p, a.type) ? "s"
                                               : "u",
                     pred);
        }
        v.type = AT_BOOL;
        at_ir_emit(g, "  %s = %scmp %s %s %s, %s\n", v.text, fp ? "f" : "i", p, t, a.text, b.text);
        return v;
    }
    if (fp) {
        const char *name = op == T_PLUS    ? "fadd"
                           : op == T_MINUS ? "fsub"
                           : op == T_STAR  ? "fmul"
                           : op == T_SLASH ? "fdiv"
                                           : "frem";
        if (op == T_SLASH || op == T_PERCENT) {
            Val bad = at_ir_temp(g, AT_BOOL);
            at_ir_emit(g, "  %s = fcmp oeq %s %s, 0.0\n", bad.text, t, b.text);
            at_ir_guard(g, bad, n, 2);
        }
        at_ir_emit(g, "  %s = %s %s %s, %s\n", v.text, name, t, a.text, b.text);
        return v;
    }
    if ((op == T_PLUS || op == T_MINUS || op == T_STAR) && !wrap) {
        Val pair = at_ir_temp(g, 0), bad = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = call { %s, i1 } @llvm.%s%s.with.overflow.%s(%s %s, %s %s)\n",
                   pair.text, t, at_signed(g->p, a.type) ? "s" : "u",
                   op == T_PLUS    ? "add"
                   : op == T_MINUS ? "sub"
                                   : "mul",
                   t, t, a.text, t, b.text);
        at_ir_emit(g,
                   "  %s = extractvalue { %s, i1 } %s, 0\n  %s = extractvalue { %s, i1 } %s, 1\n",
                   v.text, t, pair.text, bad.text, t, pair.text);
        at_ir_guard(g, bad, n, 1);
        return v;
    }
    if (op == T_SLASH || op == T_PERCENT) {
        Val bad = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = icmp eq %s %s, 0\n", bad.text, t, b.text);
        at_ir_guard(g, bad, n, 2);
        if (at_signed(g->p, a.type)) {
            unsigned bits = (unsigned)at_bits(g->p, a.type);
            uint64_t min = 1ull << (bits - 1);
            Val x = at_ir_temp(g, AT_BOOL), y = at_ir_temp(g, AT_BOOL), z = at_ir_temp(g, AT_BOOL);
            at_ir_emit(
                g, "  %s = icmp eq %s %s, %llu\n  %s = icmp eq %s %s, -1\n  %s = and i1 %s, %s\n",
                x.text, t, a.text, (unsigned long long)min, y.text, t, b.text, z.text, x.text,
                y.text);
            at_ir_guard(g, z, n, 1);
        }
    }
    const char *name = op == T_PLUS    ? "add"
                       : op == T_MINUS ? "sub"
                       : op == T_STAR  ? "mul"
                       : op == T_AMP   ? "and"
                       : op == T_PIPE  ? "or"
                       : op == T_CARET ? "xor"
                       : op == T_SLASH ? (at_signed(g->p, a.type) ? "sdiv" : "udiv")
                                       : (at_signed(g->p, a.type) ? "srem" : "urem");
    at_ir_emit(g, "  %s = %s %s %s, %s\n", v.text, name, t, a.text, b.text);
    return v;
}

Val at_ir_convert(Gen *g, Val a, int target, AtNode *n)
{
    if (a.type == target) {
        return a;
    }
    Val v = at_ir_temp(g, target);
    int frombits = at_bits(g->p, a.type), tobits = at_bits(g->p, target);
    int fromint = at_integer(g->p, a.type), toint = at_integer(g->p, target);
    if (fromint && toint) {
        /* Check representability before narrowing/changing signedness. Widen both
         * operands to i128 so comparisons themselves cannot overflow or become UB. */
        Val wide = at_ir_temp(g, 0), lo = at_ir_temp(g, AT_BOOL), hi = at_ir_temp(g, AT_BOOL),
            bad = at_ir_temp(g, AT_BOOL);
        at_ir_emit(g, "  %s = %s %s %s to i128\n", wide.text,
                   at_signed(g->p, a.type) ? "sext" : "zext", at_ir_type(g, a.type), a.text);
        uint64_t high = at_signed(g->p, target) ? (UINT64_MAX >> (65 - tobits))
                        : tobits == 64          ? UINT64_MAX
                                                : (1ull << tobits) - 1;
        char low[64];
        if (at_signed(g->p, target)) {
            snprintf(low, sizeof low, "-%llu", (unsigned long long)(1ull << (tobits - 1)));
        } else {
            strcpy(low, "0");
        }
        at_ir_emit(
            g, "  %s = icmp slt i128 %s, %s\n  %s = icmp sgt i128 %s, %llu\n  %s = or i1 %s, %s\n",
            lo.text, wide.text, low, hi.text, wide.text, (unsigned long long)high, bad.text,
            lo.text, hi.text);
        at_ir_guard(g, bad, n, 4);
        if (frombits == tobits) {
            a.type = target;
            return a;
        }
        at_ir_emit(g, "  %s = %s %s %s to %s\n", v.text,
                   frombits > tobits         ? "trunc"
                   : at_signed(g->p, a.type) ? "sext"
                                             : "zext",
                   at_ir_type(g, a.type), a.text, at_ir_type(g, target));
    } else if (fromint) {
        at_ir_emit(g, "  %s = %s %s %s to %s\n", v.text,
                   at_signed(g->p, a.type) ? "sitofp" : "uitofp", at_ir_type(g, a.type), a.text,
                   at_ir_type(g, target));
    } else if (toint) {
        /* fptosi/fptoui on NaN or outside range produce poison. Compare against the
         * exclusive power-of-two upper bound before conversion, including u64. */
        double upper = 1;
        for (int i = 0; i < tobits - (at_signed(g->p, target) ? 1 : 0); i++) {
            upper *= 2;
        }
        double lower = at_signed(g->p, target) ? -upper : 0;

        union {
            double d;
            uint64_t u;
        } up = {.d = upper}, low = {.d = lower};

        Val a64 = a;
        if (a.type == AT_F32) {
            a64 = at_ir_temp(g, AT_F64);
            at_ir_emit(g, "  %s = fpext float %s to double\n", a64.text, a.text);
        }
        Val x = at_ir_temp(g, AT_BOOL), y = at_ir_temp(g, AT_BOOL), bad = at_ir_temp(g, AT_BOOL);
        at_ir_emit(
            g,
            "  %s = fcmp ult double %s, 0x%016llX\n  %s = fcmp uge double %s, 0x%016llX\n  %s = "
            "or i1 %s, %s\n",
            x.text, a64.text, (unsigned long long)low.u, y.text, a64.text, (unsigned long long)up.u,
            bad.text, x.text, y.text);
        at_ir_guard(g, bad, n, 4);
        at_ir_emit(g, "  %s = %s %s %s to %s\n", v.text,
                   at_signed(g->p, target) ? "fptosi" : "fptoui", at_ir_type(g, a.type), a.text,
                   at_ir_type(g, target));
    } else {
        at_ir_emit(g, "  %s = %s %s %s to %s\n", v.text, frombits < tobits ? "fpext" : "fptrunc",
                   at_ir_type(g, a.type), a.text, at_ir_type(g, target));
    }
    return v;
}
