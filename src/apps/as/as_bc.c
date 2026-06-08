/* as_bc.c -- the .la compiled-bytecode (de)serializer.
 *
 * A module compiles to one top-level ObjFn whose body defines the module's
 * globals + functions; nested `def`s ride in its const pool as FN constants.
 * as_dump serializes that tree to a FILE; as_load rebuilds it from a byte
 * buffer into a runnable ObjFn.
 *
 * Endianness: both the host (arm64) and the target (x86_64) are little-endian,
 * so every integer and the IEEE-754 double are written as raw LE bytes (a memcpy
 * of Value.as.i / Value.as.f). No cross-endian support.
 *
 * NOT serialized: ObjFn->module (re-stamped by the import/CLI caller), the
 * cap/kcap (rebuilt = count/kcount), ObjStr->hash (recomputed by as_str_copy),
 * and the Obj GC header. Upvalue {is_local,index} capture pairs are NOT a
 * separate section -- the compiler emits them inline into the *parent* function's
 * code stream after each OP_CLOSURE (vm.c reads them from there at runtime), so
 * serializing fn->code verbatim round-trips them for free; upvalue_count is
 * written only as metadata (so as_closure_new can size the upvalue array). */
#include "as.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* file format: header "LAQ1" + u32 AS_BC_VERSION, then a recursive ObjFn:
 *   u32 arity, u32 upvalue_count, u32 name_len + name bytes,
 *   u32 code_len + code bytes, u32 const_count + that many tagged constants. */

enum { K_NIL = 0, K_BOOL = 1, K_INT = 2, K_FLOAT = 3, K_STR = 4, K_FN = 5 };

/* ---- dump side: little-endian writers over FILE* (return 0 ok, 1 on error) ---- */

static int wr_bytes(FILE *out, const void *p, int n)
{
    return (n > 0 && fwrite(p, 1, (size_t)n, out) != (size_t)n) ? 1 : 0;
}
static int wr_u32(FILE *out, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return wr_bytes(out, b, 4);
}
static int wr_i64(FILE *out, int64_t v)
{
    uint64_t u = (uint64_t)v;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(u >> (8 * i));
    return wr_bytes(out, b, 8);
}
static int wr_f64(FILE *out, double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);               /* host LE == target LE */
    return wr_i64(out, (int64_t)u);
}

static int dump_fn(FILE *out, ObjFn *fn)
{
    if (wr_u32(out, (uint32_t)fn->arity)) return 1;
    if (wr_u32(out, (uint32_t)fn->upvalue_count)) return 1;
    int nl = fn->name ? fn->name->len : 0;
    if (wr_u32(out, (uint32_t)nl)) return 1;
    if (nl && wr_bytes(out, fn->name->chars, nl)) return 1;
    if (wr_u32(out, (uint32_t)fn->count)) return 1;
    if (fn->count && wr_bytes(out, fn->code, fn->count)) return 1;
    if (wr_u32(out, (uint32_t)fn->kcount)) return 1;
    for (int i = 0; i < fn->kcount; i++) {
        Value v = fn->consts[i];
        switch (v.type) {
        case V_NIL:   if (fputc(K_NIL,  out) == EOF) return 1; break;
        case V_BOOL:  if (fputc(K_BOOL, out) == EOF) return 1;
                      if (fputc(v.as.i ? 1 : 0, out) == EOF) return 1; break;
        case V_INT:   if (fputc(K_INT,  out) == EOF) return 1;
                      if (wr_i64(out, v.as.i)) return 1; break;
        case V_FLOAT: if (fputc(K_FLOAT, out) == EOF) return 1;
                      if (wr_f64(out, v.as.f)) return 1; break;
        case V_OBJ:
            if (IS_STR(v)) {
                ObjStr *s = AS_STR(v);
                if (fputc(K_STR, out) == EOF) return 1;
                if (wr_u32(out, (uint32_t)s->len)) return 1;
                if (s->len && wr_bytes(out, s->chars, s->len)) return 1;
            } else if (IS_FN(v)) {
                if (fputc(K_FN, out) == EOF) return 1;
                if (dump_fn(out, AS_FN(v))) return 1;   /* recurse */
            } else {
                /* O_NATIVE/O_LIST/O_DICT/O_CLOSURE/etc. never appear in a
                 * compiler-built const pool; a native holds a raw C fn ptr. */
                strcpy(as_err, "as_dump: unserializable constant");
                return 1;
            }
            break;
        default:
            strcpy(as_err, "as_dump: unknown value type");
            return 1;
        }
    }
    return 0;
}

int as_dump(ObjFn *fn, FILE *out)
{
    if (fwrite("LAQ1", 1, 4, out) != 4) return 1;
    if (wr_u32(out, AS_BC_VERSION)) return 1;
    return dump_fn(out, fn);
}

/* ---- load side: a bounds-checked cursor over buf[0..len). ---- */

typedef struct { const uint8_t *p; int pos, len; } Cursor;

static int rd_bytes(Cursor *c, void *dst, int n)   /* 0 ok, 1 short read */
{
    if (n < 0 || c->pos + n > c->len) return 1;
    if (n) memcpy(dst, c->p + c->pos, (size_t)n);
    c->pos += n;
    return 0;
}
static int rd_u8(Cursor *c, uint8_t *out)
{
    if (c->pos + 1 > c->len) return 1;
    *out = c->p[c->pos++];
    return 0;
}
static int rd_u32(Cursor *c, uint32_t *out)
{
    uint8_t b[4];
    if (rd_bytes(c, b, 4)) return 1;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}
static int rd_i64(Cursor *c, int64_t *out)
{
    uint8_t b[8];
    if (rd_bytes(c, b, 8)) return 1;
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= (uint64_t)b[i] << (8 * i);
    *out = (int64_t)u;
    return 0;
}
static int rd_f64(Cursor *c, double *out)
{
    int64_t i;
    if (rd_i64(c, &i)) return 1;
    uint64_t u = (uint64_t)i;
    memcpy(out, &u, 8);
    return 0;
}

/* Build one ObjFn (and, recursively, its FN constants). Sets *err=1 on any short
 * read / bad tag; the partially built tree is on g_objs and reclaimed by the next
 * as_free_objects/GC (the load is abandoned). */
static ObjFn *load_fn(Cursor *c, int *err)
{
    ObjFn *fn = as_fn_new();          /* code/consts NULL, count/kcount 0, module NULL */
    uint32_t u;

    if (rd_u32(c, &u)) { *err = 1; return fn; }
    fn->arity = (int)u;
    if (rd_u32(c, &u)) { *err = 1; return fn; }
    fn->upvalue_count = (int)u;

    uint32_t name_len;
    if (rd_u32(c, &name_len)) { *err = 1; return fn; }
    if (name_len) {
        char *nm = (char *)malloc(name_len);
        if (!nm) { *err = 1; return fn; }
        if (rd_bytes(c, nm, (int)name_len)) { free(nm); *err = 1; return fn; }
        fn->name = as_str_copy(nm, (int)name_len);   /* recomputes hash */
        free(nm);
    }

    uint32_t code_len;
    if (rd_u32(c, &code_len)) { *err = 1; return fn; }
    if (code_len) {
        fn->code = (uint8_t *)malloc(code_len);       /* freed by free_object */
        if (!fn->code) { *err = 1; return fn; }
        if (rd_bytes(c, fn->code, (int)code_len)) { *err = 1; return fn; }
    }
    fn->count = fn->cap = (int)code_len;

    uint32_t kcount;
    if (rd_u32(c, &kcount)) { *err = 1; return fn; }
    if (kcount) {
        fn->consts = (Value *)malloc(sizeof(Value) * (size_t)kcount);
        if (!fn->consts) { *err = 1; return fn; }
    }
    /* set kcount/kcap incrementally so a mid-load abort frees only the slots we
     * actually filled (free_object frees the whole consts array, not the slots). */
    fn->kcount = fn->kcap = (int)kcount;
    for (uint32_t i = 0; i < kcount; i++) {
        uint8_t tag;
        if (rd_u8(c, &tag)) { *err = 1; return fn; }
        switch (tag) {
        case K_NIL:  fn->consts[i] = NIL_VAL; break;
        case K_BOOL: {
            uint8_t b;
            if (rd_u8(c, &b)) { *err = 1; return fn; }
            fn->consts[i] = BOOL_VAL(b != 0);
            break;
        }
        case K_INT: {
            int64_t v;
            if (rd_i64(c, &v)) { *err = 1; return fn; }
            fn->consts[i] = INT_VAL(v);
            break;
        }
        case K_FLOAT: {
            double d;
            if (rd_f64(c, &d)) { *err = 1; return fn; }
            fn->consts[i] = FLOAT_VAL(d);
            break;
        }
        case K_STR: {
            uint32_t slen;
            if (rd_u32(c, &slen)) { *err = 1; return fn; }
            char *sb = NULL;
            if (slen) {
                sb = (char *)malloc(slen);
                if (!sb) { *err = 1; return fn; }
                if (rd_bytes(c, sb, (int)slen)) { free(sb); *err = 1; return fn; }
            }
            fn->consts[i] = OBJ_VAL(as_str_copy(sb ? sb : "", (int)slen));
            free(sb);
            break;
        }
        case K_FN: {
            ObjFn *sub = load_fn(c, err);
            fn->consts[i] = OBJ_VAL(sub);
            if (*err) return fn;
            break;
        }
        default:
            strcpy(as_err, "as_load: bad constant tag");
            *err = 1;
            return fn;
        }
    }
    return fn;
}

ObjFn *as_load(const uint8_t *buf, int len)
{
    if (len < 8 || memcmp(buf, "LAQ1", 4) != 0) return NULL;
    uint32_t ver = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                   ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    if (ver != AS_BC_VERSION) return NULL;

    Cursor c = { buf, 8, len };
    as_gc_push_disable();             /* the tree is unrooted until returned (mirrors as_compile) */
    int err = 0;
    ObjFn *fn = load_fn(&c, &err);
    as_gc_pop_disable();
    return err ? NULL : fn;
}
