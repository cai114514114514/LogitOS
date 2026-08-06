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
                 * compiler-built const pool; a native holds a raw C fn ptr.
                 * Naming the type matters: reaching here means the compiler put
                 * something new in a const pool, and "unserializable constant"
                 * alone doesn't say what. */
                snprintf(as_err, sizeof as_err, "as_dump: unserializable %s constant",
                         as_obj_type_name(AS_OBJ(v)));
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
 * read / bad tag / nesting too deep; the partially built tree is on g_objs and
 * reclaimed by the next as_free_objects/GC (the load is abandoned). depth caps the
 * K_FN recursion: an unbounded .la could otherwise blow the C stack. */
#define AS_LA_MAX_DEPTH 256
static ObjFn *load_fn(Cursor *c, int *err, int depth)
{
    if (depth > AS_LA_MAX_DEPTH) { strcpy(as_err, "as_load: functions nested too deep"); *err = 1; return NULL; }
    ObjFn *fn = as_fn_new();          /* code/consts NULL, count/kcount 0, module NULL */
    if (!fn) { *err = 1; return NULL; }         /* g_oom set */
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
            ObjStr *os = as_str_copy(sb ? sb : "", (int)slen);
            free(sb);
            if (!os) { *err = 1; return fn; }        /* g_oom set */
            fn->consts[i] = OBJ_VAL(os);
            break;
        }
        case K_FN: {
            ObjFn *sub = load_fn(c, err, depth + 1);
            if (*err) return fn;
            fn->consts[i] = OBJ_VAL(sub);
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

/* ---- verifier: a loaded .la is UNTRUSTED input (import prefers NAME.la over
 * NAME.as, and `as -run` executes one directly). load_fn checks only the file's
 * structural integrity; this pass rejects bytecode the compiler could never have
 * emitted: unknown opcodes, truncated operands, const-pool indexes out of range
 * or of the wrong type (a fake ObjStr/ObjFn constant would be a type-confusion
 * primitive), jump targets outside [0, count), upvalue indexes beyond
 * upvalue_count, and upvalue captures in the top-level fn (it runs via call_fn
 * with a NULL closure). 1-byte local-slot operands are frame-relative and cannot
 * be bounded statically -- .la files must still come from the trusted in-tree
 * compiler for stack-slot integrity. Returns 0 ok, 1 bad. */
static int verify_fn(ObjFn *fn, int depth, int is_top)
{
    if (depth > AS_LA_MAX_DEPTH) return 1;
    if (fn->arity < 0 || fn->upvalue_count < 0 || fn->upvalue_count > 256) return 1;
    for (int i = 0; i < fn->kcount; i++)
        if (IS_FN(fn->consts[i]) && verify_fn(AS_FN(fn->consts[i]), depth + 1, 0)) return 1;
    uint8_t *code = fn->code; int n = fn->count;
    int ip = 0;
    while (ip < n) {
        uint8_t op = code[ip++];
        switch (op) {
        /* no operand */
        case OP_NIL: case OP_TRUE: case OP_FALSE: case OP_POP:
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_NEG:
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_NOT:
        case OP_RET:
        case OP_INDEX_GET: case OP_INDEX_SET: case OP_LEN:
        case OP_IN: case OP_ITER:
        case OP_CLOSE_UPVALUE:
        case OP_INHERIT: case OP_RAISE:
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_BNOT: case OP_SHL: case OP_SHR: case OP_POW:
            break;
        /* 1-byte operand */
        case OP_GET_LOCAL: case OP_SET_LOCAL:
        case OP_CALL: case OP_MAKE_LIST: case OP_MAKE_DICT:
            if (ip + 1 > n) return 1;
            ip += 1;
            break;
        /* 1-byte upvalue index: bounded by this fn's upvalue_count, and needs a
         * closure (the top-level fn runs with frame->closure == NULL) */
        case OP_GET_UPVALUE: case OP_SET_UPVALUE:
            if (is_top) return 1;
            if (ip + 1 > n) return 1;
            if (code[ip] >= fn->upvalue_count) return 1;
            ip += 1;
            break;
        /* 2-byte const-pool index, any constant type */
        case OP_CONST: {
            if (ip + 2 > n) return 1;
            int k = (code[ip] << 8) | code[ip + 1];
            if (k >= fn->kcount) return 1;
            ip += 2;
            break;
        }
        /* 2-byte const-pool index that must name a string (the VM casts it with
         * AS_STR without a type check) */
        case OP_GET_GLOBAL: case OP_SET_GLOBAL: case OP_DEF_GLOBAL:
        case OP_IMPORT:
        case OP_CLASS: case OP_METHOD:
        case OP_GET_PROPERTY: case OP_SET_PROPERTY: case OP_GET_SUPER: {
            if (ip + 2 > n) return 1;
            int k = (code[ip] << 8) | code[ip + 1];
            if (k >= fn->kcount || !IS_STR(fn->consts[k])) return 1;
            ip += 2;
            break;
        }
        /* 2-byte string const + 1-byte argc */
        case OP_INVOKE: {
            if (ip + 3 > n) return 1;
            int k = (code[ip] << 8) | code[ip + 1];
            if (k >= fn->kcount || !IS_STR(fn->consts[k])) return 1;
            ip += 3;
            break;
        }
        /* 2-byte FN const + inline {is_local,index} capture pairs (one per
         * upvalue of the child fn -- the VM reads them from the code stream) */
        case OP_CLOSURE: {
            if (ip + 2 > n) return 1;
            int k = (code[ip] << 8) | code[ip + 1];
            if (k >= fn->kcount || !IS_FN(fn->consts[k])) return 1;
            ObjFn *sub = AS_FN(fn->consts[k]);
            if (is_top && sub->upvalue_count > 0) return 1;   /* no enclosing closure to capture from */
            if (ip + 2 + 2 * sub->upvalue_count > n) return 1;
            ip += 2;
            for (int i = 0; i < sub->upvalue_count; i++) {
                if (code[ip] > 1) return 1;                    /* is_local is a boolean */
                ip += 2;
            }
            break;
        }
        /* 2-byte forward jump offset: target must land inside the code */
        case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_SETUP_TRY: case OP_POP_TRY: {
            if (ip + 2 > n) return 1;
            int off = (code[ip] << 8) | code[ip + 1];
            if (ip + 2 + off >= n) return 1;
            ip += 2;
            break;
        }
        /* 2-byte backward jump offset: target must not pass the start */
        case OP_LOOP: {
            if (ip + 2 > n) return 1;
            int off = (code[ip] << 8) | code[ip + 1];
            if (off > ip + 2) return 1;
            ip += 2;
            break;
        }
        default:
            return 1;   /* unknown / handler-less opcode (incl. OP_GET_ATTR) */
        }
    }
    return 0;   /* ip == n here: ended exactly on an instruction boundary */
}

/* ---- disassembler (as -dis foo.la) ----------------------------------------
 * When a self-hosting stage stops reproducing itself, the only evidence is two
 * .la files that differ somewhere in ~37 KB of bytes. This turns both into a
 * line-per-instruction text form so `diff` names the instruction that moved.
 * Output goes through as_emit (fd 1), not fprintf: it must work under mini-libc
 * on Logit too. Operand widths mirror verify_fn above -- `make check-asops`
 * asserts OPNAMES stays in step with the OpCode enum. */

static const char *const OPNAMES[] = {
    "CONST", "NIL", "TRUE", "FALSE", "POP",
    "GET_LOCAL", "SET_LOCAL", "GET_GLOBAL", "SET_GLOBAL", "DEF_GLOBAL",
    "ADD", "SUB", "MUL", "DIV", "MOD", "NEG",
    "EQ", "NE", "LT", "LE", "GT", "GE", "NOT",
    "JUMP", "JUMP_IF_FALSE", "LOOP",
    "CALL", "RET",
    "MAKE_LIST", "INDEX_GET", "INDEX_SET", "LEN", "INVOKE",
    "GET_ATTR", "IMPORT",
    "MAKE_DICT", "IN", "ITER",
    "CLOSURE", "GET_UPVALUE", "SET_UPVALUE", "CLOSE_UPVALUE",
    "CLASS", "INHERIT", "METHOD",
    "GET_PROPERTY", "SET_PROPERTY", "GET_SUPER",
    "SETUP_TRY", "POP_TRY", "RAISE",
    "BAND", "BOR", "BXOR", "BNOT", "SHL", "SHR", "POW",
};
#define N_OPNAMES ((int)(sizeof OPNAMES / sizeof OPNAMES[0]))

static void dis_emit(const char *s) { as_emit_cstr(s); }

static void dis_pad(int from, int to)
{
    for (int i = from; i < to; i++) as_emit(" ", 1);
}

/* One constant, abbreviated: `str "foo"` / `int 42` / `fn <name>`. */
static void dis_const(Value v, char *buf, int cap)
{
    if (IS_FN(v)) {
        ObjFn *f = AS_FN(v);
        int n = snprintf(buf, (size_t)cap, "fn <%.*s>",
                         f->name ? f->name->len : 6, f->name ? f->name->chars : "script");
        if (n < 0) buf[0] = 0;
        return;
    }
    const char *kind = IS_STR(v) ? "str " : IS_INT(v) ? "int " : IS_FLOAT(v) ? "float "
                     : IS_BOOL(v) ? "bool " : IS_NIL(v) ? "" : "obj ";
    int p = snprintf(buf, (size_t)cap, "%s", kind);
    if (p < 0 || p >= cap) { if (cap) buf[cap - 1] = 0; return; }
    if (IS_NIL(v)) { snprintf(buf + p, (size_t)(cap - p), "nil"); return; }
    if (IS_STR(v)) {
        /* quote it so a trailing-space or empty-string difference is visible */
        int q = p;
        if (q < cap - 1) buf[q++] = '"';
        ObjStr *s = AS_STR(v);
        for (int i = 0; i < s->len && q < cap - 2; i++) {
            unsigned char c = (unsigned char)s->chars[i];
            if (c == '\n') { if (q < cap - 3) { buf[q++] = '\\'; buf[q++] = 'n'; } }
            else if (c == '"') { if (q < cap - 3) { buf[q++] = '\\'; buf[q++] = '"'; } }
            else if (c < 32)   { if (q < cap - 3) { buf[q++] = '\\'; buf[q++] = '?'; } }
            else buf[q++] = (char)c;
        }
        if (q < cap - 1) buf[q++] = '"';
        buf[q] = 0;
        return;
    }
    value_to_cstr(v, buf + p, cap - p);
}

static void dis_fn(ObjFn *fn, int depth)
{
    char buf[512];
    const char *nm = fn->name ? fn->name->chars : "script";
    int nl = fn->name ? fn->name->len : 6;
    snprintf(buf, sizeof buf, "\nfn <%.*s> arity=%d upvals=%d consts=%d code=%d depth=%d\n",
             nl, nm, fn->arity, fn->upvalue_count, fn->kcount, fn->count, depth);
    dis_emit(buf);

    for (int i = 0; i < fn->kcount; i++) {
        char cv[400];
        dis_const(fn->consts[i], cv, sizeof cv);
        snprintf(buf, sizeof buf, "  k[%d] %s\n", i, cv);
        dis_emit(buf);
    }

    uint8_t *code = fn->code;
    int n = fn->count, ip = 0;
    while (ip < n) {
        int at = ip;
        uint8_t op = code[ip++];
        const char *name = (op < N_OPNAMES) ? OPNAMES[op] : "???";
        /* snprintf returns the width it wrote -- use that rather than summing
         * field widths by hand, so the comment column always lines up. The
         * mnemonic is NOT padded here: operand-less opcodes must not emit
         * trailing whitespace, or every diff of a disassembly is noise. */
        int col = snprintf(buf, sizeof buf, "  %04d  %s", at, name);
        if (col < 0) col = 0;
        dis_emit(buf);
        /* OPERAND(): pad to the operand column. Only the arms that actually
         * print an operand call it, so operand-less opcodes end the line right
         * after the mnemonic -- no trailing whitespace for diff to trip on. */
#define OPERAND() do { dis_pad(col, 22); if (col < 22) col = 22; } while (0)

        switch (op) {
        /* 2-byte const index: show the constant it names */
        case OP_CONST: case OP_GET_GLOBAL: case OP_SET_GLOBAL: case OP_DEF_GLOBAL:
        case OP_IMPORT: case OP_CLASS: case OP_METHOD:
        case OP_GET_PROPERTY: case OP_SET_PROPERTY: case OP_GET_SUPER: {
            OPERAND();
            if (ip + 2 > n) { dis_emit("  <truncated>\n"); return; }
            int k = (code[ip] << 8) | code[ip + 1];
            ip += 2;
            int w = snprintf(buf, sizeof buf, "%d", k);
            dis_emit(buf); col += (w > 0 ? w : 0);
            dis_pad(col, 40);
            char cv[400];
            if (k < fn->kcount) dis_const(fn->consts[k], cv, sizeof cv);
            else snprintf(cv, sizeof cv, "<oob>");
            snprintf(buf, sizeof buf, "; %s\n", cv);
            dis_emit(buf);
            break;
        }
        case OP_INVOKE: {
            OPERAND();
            if (ip + 3 > n) { dis_emit("  <truncated>\n"); return; }
            int k = (code[ip] << 8) | code[ip + 1];
            int argc = code[ip + 2];
            ip += 3;
            char cv[400];
            if (k < fn->kcount) dis_const(fn->consts[k], cv, sizeof cv);
            else snprintf(cv, sizeof cv, "<oob>");
            int w = snprintf(buf, sizeof buf, "%d argc=%d", k, argc);
            dis_emit(buf); col += (w > 0 ? w : 0);
            dis_pad(col, 40);
            snprintf(buf, sizeof buf, "; %s\n", cv);
            dis_emit(buf);
            break;
        }
        case OP_CLOSURE: {
            OPERAND();
            if (ip + 2 > n) { dis_emit("  <truncated>\n"); return; }
            int k = (code[ip] << 8) | code[ip + 1];
            ip += 2;
            char cv[400];
            if (k < fn->kcount) dis_const(fn->consts[k], cv, sizeof cv);
            else snprintf(cv, sizeof cv, "<oob>");
            int w = snprintf(buf, sizeof buf, "%d", k);
            dis_emit(buf); col += (w > 0 ? w : 0);
            dis_pad(col, 40);
            snprintf(buf, sizeof buf, "; %s\n", cv);
            dis_emit(buf);
            /* inline {is_local,index} capture pairs live in this code stream */
            int ups = (k < fn->kcount && IS_FN(fn->consts[k])) ? AS_FN(fn->consts[k])->upvalue_count : 0;
            for (int i = 0; i < ups && ip + 2 <= n; i++) {
                snprintf(buf, sizeof buf, "        upval %s %d\n",
                         code[ip] ? "local" : "outer", code[ip + 1]);
                dis_emit(buf);
                ip += 2;
            }
            break;
        }
        /* 2-byte relative jump: show the absolute target */
        case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_SETUP_TRY: case OP_POP_TRY: {
            OPERAND();
            if (ip + 2 > n) { dis_emit("  <truncated>\n"); return; }
            int off = (code[ip] << 8) | code[ip + 1];
            ip += 2;
            int w = snprintf(buf, sizeof buf, "%d", off);
            dis_emit(buf); col += (w > 0 ? w : 0);
            dis_pad(col, 40);
            snprintf(buf, sizeof buf, "; -> %d\n", ip + off);
            dis_emit(buf);
            break;
        }
        case OP_LOOP: {
            OPERAND();
            if (ip + 2 > n) { dis_emit("  <truncated>\n"); return; }
            int off = (code[ip] << 8) | code[ip + 1];
            ip += 2;
            int w = snprintf(buf, sizeof buf, "%d", off);
            dis_emit(buf); col += (w > 0 ? w : 0);
            dis_pad(col, 40);
            snprintf(buf, sizeof buf, "; -> %d\n", ip - off);
            dis_emit(buf);
            break;
        }
        /* 1-byte operand */
        case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_CALL:
        case OP_MAKE_LIST: case OP_MAKE_DICT:
        case OP_GET_UPVALUE: case OP_SET_UPVALUE: {
            OPERAND();
            if (ip + 1 > n) { dis_emit("  <truncated>\n"); return; }
            snprintf(buf, sizeof buf, "%d\n", code[ip]);
            dis_emit(buf);
            ip += 1;
            break;
        }
        default:
            dis_emit("\n");
            break;
        }
    }

    #undef OPERAND

    /* nested functions after the parent, depth-first, so the order is stable */
    for (int i = 0; i < fn->kcount; i++)
        if (IS_FN(fn->consts[i])) dis_fn(AS_FN(fn->consts[i]), depth + 1);
}

void as_disasm(ObjFn *fn)
{
    char buf[128];
    snprintf(buf, sizeof buf, "; AetherScript .la disassembly (AS_BC_VERSION %u)\n",
             (unsigned)AS_BC_VERSION);
    as_emit_cstr(buf);
    dis_fn(fn, 0);
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
    ObjFn *fn = load_fn(&c, &err, 0);
    if (!err && verify_fn(fn, 0, 1)) {
        strcpy(as_err, "as_load: bytecode failed verification");
        err = 1;
    }
    as_gc_pop_disable();
    return err ? NULL : fn;
}
