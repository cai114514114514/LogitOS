#include "as.h"
#include <stdio.h>      /* snprintf (host libc or mini-libc; %g/%lld verified) */
#include <stdlib.h>     /* strtod / atoi -- shortest round-trip float formatting */
#include <string.h>     /* strchr */

char as_err[256];

int as_truthy(Value v)
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
int as_value_eq(Value a, Value b)
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

/* Cycle guard for container printing: a self-referential list/dict
 * (`l = []; l.append(l); print(l)`) would otherwise recurse until the C stack
 * blows. Past the cap, containers print as [...] / {...} (Python prints `[...]`). */
#define PRINT_MAX_DEPTH 32

/* Format a double like Python/JS repr: the SHORTEST decimal that round-trips
 * (fewest significant digits whose strtod() gives `d` back), in fixed notation
 * for normal magnitudes and scientific only for very large/small. `%g`'s fixed
 * 6 digits silently lost precision (3.14159..., 0.333333); plain shortest-%g
 * printed 10.0 as "1e+01". A whole-valued float keeps ".0" (10.0 not "10"). */
int as_fmt_float(double d, char *buf, int cap)
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

/* --- one formatter, two sinks ---------------------------------------------
 * `print` streams to as_emit; `str(x)` fills a caller buffer. These used to be
 * two full copies of the same 70-line walk (print_depth + vts), which is exactly
 * how they drifted. A Sink unifies them: buf==NULL streams, otherwise it fills
 * and truncates. The stream sink keeps `off` at 0 -- it has no capacity, so the
 * fill-side guards (`sink_full`) are simply never true for it, which reproduces
 * the old print path's unguarded loops. */
typedef struct { char *buf; int cap; int off; } Sink;

static void put(Sink *k, const char *s, int n)
{
    if (n <= 0) return;
    if (!k->buf) { as_emit(s, n); return; }        /* stream: no capacity, off stays 0 */
    if (n > k->cap - 1 - k->off) n = k->cap - 1 - k->off;   /* keep room for the NUL */
    if (n > 0) { memcpy(k->buf + k->off, s, (size_t)n); k->off += n; }
}
static void putz(Sink *k, const char *s) { put(k, s, (int)strlen(s)); }
static void putstr(Sink *k, ObjStr *s)   { put(k, s->chars, s->len); }
static int  sink_full(Sink *k)           { return k->buf && k->off >= k->cap - 1; }

/* snprintf into a fixed scratch buffer; returns the clamped byte count. */
static int fmt_i64(char *tmp, int cap, int64_t v)
{
    int n = snprintf(tmp, (size_t)cap, "%lld", (long long)v);
    return n < cap ? n : cap - 1;
}

/* `repr` quotes strings (containers print their elements repr-style, like
 * Python); `depth` is the cycle guard. */
static void fmt_value(Sink *k, Value v, int repr, int depth)
{
    char tmp[40];
    switch (v.type) {
    case V_NIL:   putz(k, "nil"); return;
    case V_BOOL:  putz(k, AS_BOOL(v) ? "true" : "false"); return;
    case V_INT:   put(k, tmp, fmt_i64(tmp, (int)sizeof tmp, AS_INT(v))); return;
    case V_FLOAT: put(k, tmp, as_fmt_float(AS_FLOAT(v), tmp, (int)sizeof tmp)); return;
    case V_OBJ:   break;
    default:      return;
    }

    switch (AS_OBJ(v)->type) {
    case O_STR:
        if (!repr) { putstr(k, AS_STR(v)); return; }
        put(k, "'", 1); putstr(k, AS_STR(v)); put(k, "'", 1);
        return;
    case O_LIST: {
        if (depth >= PRINT_MAX_DEPTH) { putz(k, "[...]"); return; }
        ObjList *l = AS_LIST(v);
        put(k, "[", 1);
        for (int i = 0; i < l->count && !sink_full(k); i++) {
            if (i) put(k, ", ", 2);
            fmt_value(k, l->items[i], 1, depth + 1);
        }
        put(k, "]", 1);
        return;
    }
    case O_DICT: {
        if (depth >= PRINT_MAX_DEPTH) { putz(k, "{...}"); return; }
        ObjDict *d = AS_DICT(v);
        int first = 1;
        put(k, "{", 1);
        for (int i = 0; i < d->cap && !sink_full(k); i++) {
            DictEntry *e = &d->entries[i];
            if (e->kind != AS_DK_STR && e->kind != AS_DK_INT) continue;
            if (!first) put(k, ", ", 2);
            first = 0;
            if (e->kind == AS_DK_STR) fmt_value(k, OBJ_VAL(e->kstr), 1, depth + 1);
            else                      put(k, tmp, fmt_i64(tmp, (int)sizeof tmp, e->kint));
            put(k, ": ", 2);
            fmt_value(k, e->val, 1, depth + 1);
        }
        put(k, "}", 1);
        return;
    }
    case O_PTR: {
        ObjPtr *p = AS_PTR(v);
        int n = snprintf(tmp, sizeof tmp, "<i%d ptr @0x%llx>", p->width * 8, (unsigned long long)p->addr);
        put(k, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
        return;
    }
    case O_MODULE:       putz(k, "<module "); putstr(k, AS_MODULE(v)->name); put(k, ">", 1); return;
    case O_FN: case O_CLOSURE: putz(k, "<fn>"); return;
    case O_NATIVE:       putz(k, "<native fn>"); return;
    case O_CLASS:        putz(k, "<class "); putstr(k, AS_CLASS(v)->name); put(k, ">", 1); return;
    case O_INSTANCE:     put(k, "<", 1); putstr(k, AS_INSTANCE(v)->klass->name); putz(k, " instance>"); return;
    case O_BOUND_METHOD: putz(k, "<bound method>"); return;
    case O_UPVALUE:      break;   /* never user-visible: lives only in a closure */
    }
    putz(k, "<obj>");
}

void as_print_value(Value v)
{
    Sink k = { NULL, 0, 0 };
    fmt_value(&k, v, 0, 0);
}

int value_to_cstr(Value v, char *buf, int cap)
{
    if (cap <= 0) return 0;
    Sink k = { buf, cap, 0 };
    fmt_value(&k, v, 0, 0);
    buf[k.off] = 0;
    return k.off;
}
