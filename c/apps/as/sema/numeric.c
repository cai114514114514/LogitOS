/* SPDX-License-Identifier: MIT */
#include "sema/internal.h"

int at_integer(AsTypedProject *p, int t)
{
    return t >= 0 && t < p->ntypes && p->types[t].kind >= AT_I8 && p->types[t].kind <= AT_U64;
}

int at_signed(AsTypedProject *p, int t)
{
    return at_integer(p, t) && p->types[t].kind <= AT_I64;
}

int at_bits(AsTypedProject *p, int t)
{
    int k = p->types[t].kind;
    return k == AT_BOOL       ? 1
           : k == AT_F32      ? 32
           : k == AT_F64      ? 64
           : at_integer(p, t) ? 8 << ((k - AT_I8) % 4)
                              : 0;
}

int at_check_number(AsTypedProject *p, int t)
{
    return at_satisfies_constraint(p, t, AT_CONSTRAINT_NUMBER);
}

int at_numeric_type(AsTypedProject *p, int left, int right)
{
    int left_float = left == AT_F32 || left == AT_F64;
    int right_float = right == AT_F32 || right == AT_F64;
    if (!(left_float || right_float) ||
        !(left_float || at_integer(p, left)) || !(right_float || at_integer(p, right))) {
        return AT_ERROR;
    }
    /* This is an operation's value type, never a binding's storage type.
     * Relabeling an integer operand as float would load its bits as IEEE data.
     * Both execution paths convert values using this same concrete-type rule;
     * bool, Any, and unresolved generic parameters cannot enter it. */
    return left == AT_F64 || right == AT_F64 ? AT_F64 : AT_F32;
}

void at_check_assignment_type(Checker *c, AtNode *assignment, int expected, int actual)
{
    int op = assignment->op;
    if (op == T_PLUSEQ || op == T_MINUSEQ || op == T_STAREQ ||
        op == T_SLASHEQ || op == T_PERCENTEQ) {
        int result = at_numeric_type(c->p, expected, actual);
        if (result) {
            /* f += i stores a float. i += f and f32 += f64 would narrow the
             * operation's result into the existing binding, so still reject. */
            actual = result;
        }
    }
    at_check_mismatch(c, assignment->b, expected, actual);
}
