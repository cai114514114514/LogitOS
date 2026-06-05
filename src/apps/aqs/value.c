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

static void print_repr(Value v);   /* like print, but strings are quoted (for list elements) */

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
        else if (IS_LIST(v)) {
            ObjList *l = AS_LIST(v);
            aqs_emit("[", 1);
            for (int i = 0; i < l->count; i++) { if (i) aqs_emit(", ", 2); print_repr(l->items[i]); }
            aqs_emit("]", 1);
        }
        else if (IS_DICT(v)) {
            ObjDict *d = AS_DICT(v);
            aqs_emit("{", 1);
            int first = 1;
            for (int i = 0; i < d->cap; i++) {
                DictEntry *e = &d->entries[i];
                if (e->kind != AQS_DK_STR && e->kind != AQS_DK_INT) continue;
                if (!first) aqs_emit(", ", 2);
                first = 0;
                if (e->kind == AQS_DK_STR) print_repr(OBJ_VAL(e->kstr));
                else { n = snprintf(buf, sizeof buf, "%lld", (long long)e->kint); aqs_emit(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1); }
                aqs_emit(": ", 2);
                print_repr(e->val);
            }
            aqs_emit("}", 1);
        }
        else if (IS_PTR(v)) {
            ObjPtr *p = AS_PTR(v);
            n = snprintf(buf, sizeof buf, "<i%d ptr @0x%llx>", p->width * 8, (unsigned long long)p->addr);
            aqs_emit(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
        }
        else if (IS_MODULE(v)) { aqs_emit_cstr("<module "); ObjStr *nm = AS_MODULE(v)->name; aqs_emit(nm->chars, nm->len); aqs_emit_cstr(">"); }
        else if (IS_FN(v))      aqs_emit_cstr("<fn>");
        else if (IS_NATIVE(v))  aqs_emit_cstr("<native fn>");
        else                    aqs_emit_cstr("<obj>");
        break;
    }
}

static void print_repr(Value v)
{
    if (IS_STR(v)) { ObjStr *s = AS_STR(v); aqs_emit("'", 1); aqs_emit(s->chars, s->len); aqs_emit("'", 1); }
    else aqs_print_value(v);
}
