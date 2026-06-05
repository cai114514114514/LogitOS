#include "aqs.h"
#include <stdio.h>      /* snprintf (host libc or mini-libc; %g/%lld verified) */

char aqs_err[256];

int aqs_truthy(Value v)
{
    switch (v.type) {
    case V_NIL:   return 0;
    case V_BOOL:  return AS_BOOL(v);
    case V_INT:   return AS_INT(v) != 0;
    case V_FLOAT: return AS_FLOAT(v) != 0;
    default:      return 1;            /* objects (str/list/fn) are truthy */
    }
}

/* Strict structural equality: types must match. Used for constant-pool dedup and
 * as the default for `==` on non-numeric operands. The VM cross-compares numbers
 * (1 == 1.0) itself before falling back here. */
int aqs_value_eq(Value a, Value b)
{
    if (a.type != b.type) return 0;
    switch (a.type) {
    case V_NIL:   return 1;
    case V_BOOL:  return AS_BOOL(a) == AS_BOOL(b);
    case V_INT:   return AS_INT(a) == AS_INT(b);
    case V_FLOAT: return AS_FLOAT(a) == AS_FLOAT(b);
    case V_OBJ:
        if (IS_STR(a) && IS_STR(b)) {
            ObjStr *x = AS_STR(a), *y = AS_STR(b);
            return x->len == y->len && memcmp(x->chars, y->chars, x->len) == 0;
        }
        return AS_OBJ(a) == AS_OBJ(b);
    }
    return 0;
}

void aqs_print_value(Value v)
{
    char buf[40]; int n;
    switch (v.type) {
    case V_NIL:   aqs_emit_cstr("nil"); break;
    case V_BOOL:  aqs_emit_cstr(AS_BOOL(v) ? "true" : "false"); break;
    case V_INT:   n = snprintf(buf, sizeof buf, "%lld", (long long)AS_INT(v));
                  aqs_emit(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1); break;
    case V_FLOAT: n = snprintf(buf, sizeof buf, "%g", AS_FLOAT(v));
                  aqs_emit(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1); break;
    case V_OBJ:
        if (IS_STR(v))         { ObjStr *s = AS_STR(v); aqs_emit(s->chars, s->len); }
        else if (IS_FN(v))      aqs_emit_cstr("<fn>");
        else if (IS_NATIVE(v))  aqs_emit_cstr("<native fn>");
        else                    aqs_emit_cstr("<obj>");
        break;
    }
}
