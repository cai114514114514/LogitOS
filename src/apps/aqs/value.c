#include "aqs.h"
#include <stdio.h>      /* snprintf (host libc or mini-libc; %g/%lld verified) */
#include <stdlib.h>     /* strtod / atoi -- shortest round-trip float formatting */
#include <string.h>     /* strchr */

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

/* Format a double like Python/JS repr: the SHORTEST decimal that round-trips
 * (fewest significant digits whose strtod() gives `d` back), in fixed notation
 * for normal magnitudes and scientific only for very large/small. `%g`'s fixed
 * 6 digits silently lost precision (3.14159..., 0.333333); plain shortest-%g
 * printed 10.0 as "1e+01". A whole-valued float keeps ".0" (10.0 not "10"). */
int aqs_fmt_float(double d, char *buf, int cap)
{
    char tmp[40];
    /* Count the significant digits needed, notation-independent, via %e. */
    int P = 17;
    for (int p = 1; p <= 17; p++) {
        snprintf(tmp, sizeof tmp, "%.*e", p - 1, d);
        if (strtod(tmp, (char **)0) == d) { P = p; break; }
    }
    char *epos = strchr(tmp, 'e');                  /* decimal exponent of d */
    int E = epos ? atoi(epos + 1) : 0;
    int n;
    if (E >= -4 && E < 16) {                         /* fixed notation (Python's range) */
        int dec = P - 1 - E;
        if (dec < 0) dec = 0;
        n = snprintf(buf, (size_t)cap, "%.*f", dec, d);
        if (n >= cap) n = cap - 1;
        if (dec == 0 && n + 2 < cap) { buf[n++] = '.'; buf[n++] = '0'; buf[n] = 0; }  /* 10 -> 10.0 */
    } else {                                         /* scientific for very large / small */
        n = snprintf(buf, (size_t)cap, "%.*e", P - 1, d);
        if (n >= cap) n = cap - 1;
    }
    return n;
}

void aqs_print_value(Value v)
{
    char buf[40]; int n;
    switch (v.type) {
    case V_NIL:   aqs_emit_cstr("nil"); break;
    case V_BOOL:  aqs_emit_cstr(AS_BOOL(v) ? "true" : "false"); break;
    case V_INT:   n = snprintf(buf, sizeof buf, "%lld", (long long)AS_INT(v));
                  aqs_emit(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1); break;
    case V_FLOAT: n = aqs_fmt_float(AS_FLOAT(v), buf, sizeof buf);
                  aqs_emit(buf, n); break;
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
        else if (IS_FN(v) || IS_CLOSURE(v)) aqs_emit_cstr("<fn>");
        else if (IS_NATIVE(v))  aqs_emit_cstr("<native fn>");
        else if (IS_CLASS(v))   { aqs_emit_cstr("<class "); ObjStr *nm = AS_CLASS(v)->name; aqs_emit(nm->chars, nm->len); aqs_emit_cstr(">"); }
        else if (IS_INSTANCE(v)){ aqs_emit_cstr("<"); ObjStr *nm = AS_INSTANCE(v)->klass->name; aqs_emit(nm->chars, nm->len); aqs_emit_cstr(" instance>"); }
        else if (IS_BOUND_METHOD(v)) aqs_emit_cstr("<bound method>");
        else                    aqs_emit_cstr("<obj>");
        break;
    }
}

static void print_repr(Value v)
{
    if (IS_STR(v)) { ObjStr *s = AS_STR(v); aqs_emit("'", 1); aqs_emit(s->chars, s->len); aqs_emit("'", 1); }
    else aqs_print_value(v);
}
