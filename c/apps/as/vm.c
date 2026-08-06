#include "as.h"
#include <stdio.h>      /* vsnprintf */
#include <stdarg.h>

/* Stack bytecode VM. A1 uses a switch dispatch (correctness first); the design
 * earmarks computed-goto as a later speed pass. Locals live on the value stack
 * addressed off each call frame's base; globals are a small name->value table. */

#define STACK_MAX  (4096)
#define FRAMES_MAX (256)

typedef struct { ObjFn *fn; ObjClosure *closure; uint8_t *ip; Value *slots; int is_init; } Frame;

static Value  stack[STACK_MAX];
static Value *sp;
static Frame  frames[FRAMES_MAX];
static int    frame_count;
static ObjUpvalue *open_upvalues;   /* live captured locals, sorted by stack addr desc */

/* M22.4 exceptions: a handler stack + a pending-exception slot.
 * Each handler entry is captured when OP_SETUP_TRY runs; it records where to
 * resume (handler_ip), the value-stack level to restore (sp), and which frame
 * owned the try (frame_index = frame_count at setup time). The entries hold no
 * Obj pointers, so the GC never traces them. */
typedef struct { uint8_t *handler_ip; Value *sp; int frame_index; } Handler;
static Handler handler_stack[FRAMES_MAX];
static int     handler_count;
static Value   g_exc;       /* the pending thrown value (valid iff g_has_exc) */
static int     g_has_exc;   /* 1 while an exception is propagating */

/* Natives report failure by setting this flag (+ as_err) and returning nil;
 * call_value aborts the run when it's set. (Declared here so reset_stack can
 * clear it; assigned by as_native_fail below.) */
static int g_native_err;

/* Value-stack overflow guard. push() is unbounded; the call DEPTH is capped
 * (FRAMES_MAX) but the VALUE stack (STACK_MAX) is not, so deep recursion with
 * locals, or many net-increasing pushes, can overrun stack[]. checked_push sets
 * this flag (+ a catchable "stack overflow" via runtime_error) instead of
 * advancing sp past the end; the dispatch loop polls it and unwinds to err. */
static int g_stack_overflow;

/* Builtins (print/len/range/peek/.../SYS_*) are visible from every module; each
 * module has its own namespace (ObjModule.vars). GET_GLOBAL tries the running
 * function's module first, then falls back to builtins. */
static NameVal builtins[128];
static int     nbuiltins;

static ObjModule *modules[64];      /* loaded-module cache (by name) */
static int        nmodules;
static struct { const char *name; const char *src; } msrc[32];   /* in-memory module sources (tests) */
static int        nmsrc;

static int builtin_index(ObjStr *name)
{
    for (int i = 0; i < nbuiltins; i++)
        if (as_str_hash(builtins[i].name) == as_str_hash(name) && builtins[i].name->len == name->len
            && memcmp(builtins[i].name->chars, name->chars, name->len) == 0) return i;
    return -1;
}

static Value *builtin_slot(ObjStr *name, int create)
{
    int at = builtin_index(name);
    if (at >= 0) return &builtins[at].val;
    if (!create || nbuiltins >= 128) return NULL;
    builtins[nbuiltins].name = name; builtins[nbuiltins].val = NIL_VAL;
    as_globals_gen++;
    return &builtins[nbuiltins++].val;
}

static void push(Value v) { *sp++ = v; }
static Value pop(void)    { return *--sp; }
static Value peek(int d)  { return sp[-1 - d]; }

static int runtime_error(const char *fmt, ...);

/* Bounded push: on overflow raise a catchable "stack overflow" and set the flag
 * WITHOUT advancing sp (so the stack stays valid for unwinding). The dispatch
 * loop checks g_stack_overflow at the top of DISPATCH() and goes to err. */
static void checked_push(Value v)
{
    if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); g_stack_overflow = 1; return; }
    *sp++ = v;
}

static void reset_stack(void) { sp = stack; frame_count = 0; open_upvalues = NULL;
                                handler_count = 0; g_has_exc = 0; g_exc = NIL_VAL; g_native_err = 0;
                                g_stack_overflow = 0; g_oom = 0; }

/* GC roots: everything the running program can still reach. */
void as_vm_mark_roots(void)
{
    for (Value *v = stack; v < sp; v++) gc_mark_value(*v);
    for (int i = 0; i < frame_count; i++) {
        gc_mark_obj((Obj *)frames[i].fn);
        if (frames[i].closure) gc_mark_obj((Obj *)frames[i].closure);
    }
    for (ObjUpvalue *u = open_upvalues; u; u = u->next) gc_mark_obj((Obj *)u);
    for (int i = 0; i < nbuiltins; i++) { gc_mark_obj((Obj *)builtins[i].name); gc_mark_value(builtins[i].val); }
    for (int i = 0; i < nmodules; i++) gc_mark_obj((Obj *)modules[i]);
    if (g_has_exc) gc_mark_value(g_exc);
}

/* Set the pending exception. Returns 1 (the uniform error signal) so callers can
 * `return throw_value(v)` or fall into `goto err`. */
static int throw_value(Value v) { g_exc = v; g_has_exc = 1; return 1; }

/* At the error label: if a C-level error (built-in or native) left a message in
 * as_err but no exception value was set, wrap as_err into a catchable string
 * exception. Keyed on g_has_exc (NOT g_native_err) so a native failure reaching
 * `err:` via call_value's return value is converted here. */
static void ensure_exc(void)
{
    if (!g_has_exc) {
        ObjStr *s = as_str_copy(as_err, (int)strlen(as_err));  /* may OOM -> NULL */
        g_exc = s ? OBJ_VAL(s) : NIL_VAL;                          /* never OBJ_VAL(NULL) */
        g_has_exc = 1;
    }
}

static int runtime_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(as_err, sizeof as_err, fmt, ap);
    va_end(ap);
    ObjStr *s = as_str_copy(as_err, (int)strlen(as_err));      /* may OOM -> NULL */
    return throw_value(s ? OBJ_VAL(s) : NIL_VAL);                  /* a catcher must not deref NULL */
}

Value as_native_fail(const char *msg) { snprintf(as_err, sizeof as_err, "%s", msg); g_native_err = 1; return NIL_VAL; }

static int name_eq(ObjStr *s, const char *lit)
{ int n = (int)strlen(lit); return s->len == n && memcmp(s->chars, lit, n) == 0; }

static ObjStr *str_concat(ObjStr *a, ObjStr *b)
{
    if (a->len > INT32_MAX - 1 - b->len) { runtime_error("string too long"); return NULL; }
    int n = a->len + b->len;
    char *buf = (char *)as_malloc((size_t)n + 1);
    if (!buf) return NULL;                       /* g_oom set; op_ADD goes to err */
    memcpy(buf, a->chars, a->len); memcpy(buf + a->len, b->chars, b->len); buf[n] = 0;
    return as_str_take(buf, n);                 /* NULL on alloc_obj OOM (buf freed) */
}

/* Resolve a global read: the running function's module, then builtins. */
/* The inline-cache entry for constant `ki`, allocated on first use. A NULL
 * return just means "do not cache"; there is no error path. */
static struct PCache *fn_pcache(ObjFn *fn, int ki)
{
    if (!fn->pcache && fn->kcount > 0) {
        fn->pcache = (struct PCache *)calloc((size_t)fn->kcount, sizeof(struct PCache));
        fn->pcache_n = fn->pcache ? fn->kcount : 0;
    }
    return (fn->pcache && ki < fn->pcache_n) ? &fn->pcache[ki] : NULL;
}

/* Read an instance property through the cache. Returns 1 and fills *out on a
 * field hit, 0 if the instance has no such field (the caller then tries
 * methods). The guard is the shape id, not the shape pointer: ids are never
 * reused, so a shape that was collected and had its address recycled cannot be
 * mistaken for the one that was cached. */
static int instance_get_cached(ObjFn *fn, int ki, ObjInstance *in, Value *out)
{
    struct PCache *pc = fn_pcache(fn, ki);
    if (pc && pc->shape_id == in->shape->id) { *out = in->slots[pc->slot]; return 1; }
    int i = as_shape_find(in->shape, AS_STR(fn->consts[ki]));
    if (i < 0) return 0;
    if (pc) { pc->shape_id = in->shape->id; pc->slot = i; }
    *out = in->slots[i];
    return 1;
}

/* Resolve consts[ki] as a global, memoising where it landed in fn->gcache.
 * The cache stores an INDEX, not a slot pointer, so the module's var array is
 * still free to grow by realloc. Module first, then builtins -- keeping the
 * fallback dynamic is what lets a module define `range` and shadow the builtin
 * (fsroot/as/lib/stats.as does exactly that), which a compile-time binding
 * would get wrong. A failed cache allocation just means no caching; there is no
 * error path to propagate. */
static Value *global_slot(ObjFn *fn, int ki)
{
    if (fn->gcache_gen != as_globals_gen) {              /* a new global name appeared */
        if (fn->gcache) memset(fn->gcache, 0, sizeof(int32_t) * (size_t)fn->gcache_n);
        fn->gcache_gen = as_globals_gen;
    }
    if (!fn->gcache && fn->kcount > 0) {
        fn->gcache = (int32_t *)calloc((size_t)fn->kcount, sizeof(int32_t));
        fn->gcache_n = fn->gcache ? fn->kcount : 0;
    }
    int cached = (fn->gcache && ki < fn->gcache_n) ? fn->gcache[ki] : 0;
    if (cached > 0) return &fn->module->vars[cached - 1].val;
    if (cached < 0) return &builtins[-cached - 1].val;

    ObjStr *name = AS_STR(fn->consts[ki]);
    int at = fn->module ? as_module_index(fn->module, name) : -1;
    if (at < 0) {
        at = builtin_index(name);
        if (at < 0) return NULL;
        if (fn->gcache && ki < fn->gcache_n) fn->gcache[ki] = -(at + 1);
        return &builtins[at].val;
    }
    if (fn->gcache && ki < fn->gcache_n) fn->gcache[ki] = at + 1;
    return &fn->module->vars[at].val;
}

/* ---- module loader / cache ---- */
static ObjModule *module_find(ObjStr *name)
{
    for (int i = 0; i < nmodules; i++)
        if (modules[i]->name->len == name->len && memcmp(modules[i]->name->chars, name->chars, name->len) == 0)
            return modules[i];
    return NULL;
}

/* Return malloc'd source for module `name` (caller frees), or NULL. Checks the
 * in-memory registry first (tests), then NAME.as and /usr/as/NAME.as on disk. */
static char *module_source(ObjStr *name)
{
    for (int i = 0; i < nmsrc; i++)
        if ((int)strlen(msrc[i].name) == name->len && memcmp(msrc[i].name, name->chars, name->len) == 0) {
            int n = (int)strlen(msrc[i].src);
            char *buf = (char *)as_malloc((size_t)n + 1);
            if (buf) memcpy(buf, msrc[i].src, (size_t)n + 1);  /* NULL -> g_oom; as_import checks !src */
            return buf;
        }
    char path[160];
    const char *dirs[] = { "", "/usr/as/lib/", "/usr/as/" };
    for (int d = 0; d < 3; d++) {
        int p = 0;
        for (const char *pre = dirs[d]; *pre && p < 120; pre++) path[p++] = *pre;
        for (int i = 0; i < name->len && p < 150; i++) path[p++] = name->chars[i];
        const char *ext = ".as"; for (int i = 0; i < 4 && p < 156; i++) path[p++] = ext[i];
        path[p] = 0;
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t cap = 4096, len = 0; char *buf = (char *)as_malloc(cap);
        if (!buf) { fclose(f); return NULL; }
        for (;;) {
            if (len + 4096 + 1 > cap) {
                cap *= 2;
                char *nb = (char *)as_realloc(buf, cap);
                if (!nb) { free(buf); fclose(f); return NULL; }   /* g_oom set */
                buf = nb;
            }
            size_t r = fread(buf + len, 1, 4096, f);
            len += r; if (r < 4096) break;
        }
        buf[len] = 0; fclose(f);
        return buf;
    }
    return NULL;
}

/* Try NAME.la then /usr/as/NAME.la; on success as_load and return the ObjFn.
 * as_load keeps GC disabled across the whole load (mirrors as_compile_module).
 * Returns NULL if the file is absent or fails magic/version (-> .as fallback). */
static ObjFn *module_try_la(ObjStr *name)
{
    char path[160];
    const char *dirs[] = { "", "/usr/as/lib/", "/usr/as/" };
    for (int d = 0; d < 3; d++) {
        int p = 0;
        for (const char *pre = dirs[d]; *pre && p < 120; pre++) path[p++] = *pre;
        for (int i = 0; i < name->len && p < 150; i++) path[p++] = name->chars[i];
        const char *ext = ".la"; for (int i = 0; i < 3 && p < 156; i++) path[p++] = ext[i];
        path[p] = 0;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t cap = 4096, len = 0; uint8_t *buf = (uint8_t *)malloc(cap);
        if (!buf) { fclose(f); return NULL; }
        for (;;) {
            if (len + 4096 + 1 > cap) {
                cap *= 2;
                uint8_t *nb = (uint8_t *)realloc(buf, cap);
                if (!nb) { free(buf); fclose(f); return NULL; }
                buf = nb;
            }
            size_t r = fread(buf + len, 1, 4096, f);
            len += r; if (r < 4096) break;
        }
        fclose(f);
        ObjFn *fn = as_load(buf, (int)len);   /* as_load does its own push/pop_disable */
        free(buf);
        if (fn) return fn;                      /* bad magic/version -> NULL -> fall through to .as */
    }
    return NULL;
}
/* Recursively stamp ->module on fn and every O_FN constant (mirrors the compiler's
 * g_module). Pointer writes only -- allocates nothing, so GC-safe with no rooting. */
static void stamp_module(ObjFn *fn, ObjModule *m)
{
    fn->module = m;
    as_globals_gen++;      /* the fn's globals now resolve elsewhere: drop every gcache */
    for (int i = 0; i < fn->kcount; i++)
        if (IS_FN(fn->consts[i])) stamp_module(AS_FN(fn->consts[i]), m);
}

/* ---- natives ---- */
static Value native_print(int argc, Value *args)
{
    for (int i = 0; i < argc; i++) { if (i) as_emit(" ", 1); as_print_value(args[i]); }
    as_emit("\n", 1);
    return NIL_VAL;
}
static Value native_len(int argc, Value *args)
{
    if (argc != 1) return as_native_fail("len() takes exactly 1 argument");
    if (IS_LIST(args[0]))  return INT_VAL(AS_LIST(args[0])->count);
    if (IS_STR(args[0]))   return INT_VAL(AS_STR(args[0])->len);
    if (IS_DICT(args[0]))  return INT_VAL(AS_DICT(args[0])->live);
    if (IS_RANGE(args[0])) return INT_VAL(AS_RANGE(args[0])->count);
    return as_native_fail("len() needs a list, string, or dict");
}
/* ---- M21-P3 self-hosting natives: byte/char primitives + portable file I/O.
 * file_read/file_write use stdio (host libc AND Logit mini-libc), NOT Logit
 * syscalls -- the self-hosted compiler must run its fixpoint test on the host. */
static Value native_chr(int argc, Value *args)
{
    if (argc != 1 || !IS_INT(args[0]) || AS_INT(args[0]) < 0 || AS_INT(args[0]) > 255)
        return as_native_fail("chr() takes an integer 0..255");
    char c = (char)AS_INT(args[0]);
    ObjStr *s = as_str_copy(&c, 1);
    return s ? OBJ_VAL(s) : NIL_VAL;
}
static Value native_ord(int argc, Value *args)
{
    if (argc != 1 || !IS_STR(args[0]) || AS_STR(args[0])->len < 1)
        return as_native_fail("ord() takes a non-empty string");
    return INT_VAL((uint8_t)AS_STR(args[0])->chars[0]);
}
static Value native_f64bits(int argc, Value *args)   /* IEEE-754 bits of a float */
{
    if (argc != 1) return as_native_fail("f64bits() takes 1 argument");
    double d;
    if (IS_FLOAT(args[0]))    d = AS_FLOAT(args[0]);
    else if (IS_INT(args[0])) d = (double)AS_INT(args[0]);
    else return as_native_fail("f64bits() takes a number");
    int64_t u;
    memcpy(&u, &d, 8);
    return INT_VAL(u);
}
static Value native_parse_float(int argc, Value *args)  /* strtod parity with the C compiler */
{
    if (argc != 1 || !IS_STR(args[0])) return as_native_fail("parse_float() takes a string");
    return FLOAT_VAL(strtod(AS_STR(args[0])->chars, NULL));
}
static Value native_file_read(int argc, Value *args)
{
    if (argc != 1 || !IS_STR(args[0])) return as_native_fail("file_read() takes a path string");
    FILE *f = fopen(AS_STR(args[0])->chars, "rb");
    if (!f) return NIL_VAL;
    size_t cap = 4096, n = 0;
    char *buf = (char *)as_malloc(cap);
    if (!buf) { fclose(f); return NIL_VAL; }
    for (;;) {
        if (n + 4096 + 1 > cap) {
            cap *= 2;
            char *nb = (char *)as_realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NIL_VAL; }
            buf = nb;
        }
        size_t r = fread(buf + n, 1, 4096, f);
        n += r;
        if (r < 4096) break;
    }
    fclose(f);
    ObjStr *s = as_str_copy(buf, (int)n);    /* embedded NULs preserved (len-based) */
    free(buf);
    return s ? OBJ_VAL(s) : NIL_VAL;
}
static Value native_file_write(int argc, Value *args)
{
    if (argc != 2 || !IS_STR(args[0]) || !IS_STR(args[1]))
        return as_native_fail("file_write() takes (path, data) strings");
    FILE *f = fopen(AS_STR(args[0])->chars, "wb");
    if (!f) return INT_VAL(-1);
    ObjStr *s = AS_STR(args[1]);
    size_t w = s->len ? fwrite(s->chars, 1, (size_t)s->len, f) : 0;
    fclose(f);
    return INT_VAL((int64_t)w == s->len ? (int64_t)s->len : -1);
}
static int    g_argc = 0;
static char **g_argv = NULL;
void as_set_args(int argc, char **argv) { g_argc = argc; g_argv = argv; }
static Value native_args(int argc, Value *args)
{
    (void)argc; (void)args;
    ObjList *l = as_list_new();
    if (!l) return NIL_VAL;                            /* g_oom set; dispatch unwinds */
    as_gc_protect((Obj *)l);        /* rooting the list is enough: each string is pushed into
                                       it before the next allocation can collect */
    for (int i = 0; i < g_argc; i++) {
        ObjStr *s = as_str_copy(g_argv[i], (int)strlen(g_argv[i]));
        if (!s) { as_gc_release(1); return NIL_VAL; }   /* g_oom set; don't store OBJ_VAL(NULL) */
        as_list_push(l, OBJ_VAL(s));
    }
    as_gc_release(1);
    return OBJ_VAL(l);
}

static Value native_str(int argc, Value *args)   /* M23: print-form of any value */
{
    if (argc != 1) return as_native_fail("str() takes 1 argument");
    char buf[1024];
    int n = value_to_cstr(args[0], buf, sizeof buf);
    ObjStr *s = as_str_copy(buf, n);            /* args[] stay stack-rooted across this */
    return s ? OBJ_VAL(s) : NIL_VAL;            /* OOM: g_oom set, dispatch unwinds */
}
static Value native_gc_stats(int argc, Value *args)
{
    (void)argc; (void)args;
    return INT_VAL(as_gc_live());
}
static Value native_gc(int argc, Value *args)
{
    (void)argc; (void)args;
    long before = as_gc_live();
    gc_collect();
    return INT_VAL(before - as_gc_live());   /* objects freed this collection */
}
static Value native_range(int argc, Value *args)
{
    int64_t start = 0, stop, step = 1;
    for (int i = 0; i < argc; i++) if (!IS_INT(args[i])) return as_native_fail("range() needs integer arguments");
    if (argc == 1)      { stop = AS_INT(args[0]); }
    else if (argc == 2) { start = AS_INT(args[0]); stop = AS_INT(args[1]); }
    else if (argc == 3) { start = AS_INT(args[0]); stop = AS_INT(args[1]); step = AS_INT(args[2]); }
    else return as_native_fail("range() takes 1 to 3 arguments");
    if (step == 0) return as_native_fail("range() step must not be zero");
    /* Element count by division rather than by iterating: the sequence is never
     * materialised, and this is also what stops range(0, INT64_MAX) from being a
     * loop at all. Unsigned span arithmetic, because stop - start overflows for a
     * range wider than INT64_MAX. */
    int64_t count = 0;
    if (step > 0 && start < stop) {
        uint64_t span = (uint64_t)stop - (uint64_t)start;
        uint64_t n = (span + (uint64_t)step - 1) / (uint64_t)step;
        count = n > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)n;
    } else if (step < 0 && start > stop) {
        uint64_t span = (uint64_t)start - (uint64_t)stop;
        uint64_t mag  = (uint64_t)(-(step + 1)) + 1;      /* |step|, correct at INT64_MIN */
        uint64_t n = (span + mag - 1) / mag;
        count = n > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)n;
    }
    ObjRange *r = as_range_new(start, step, count);
    if (!r) return NIL_VAL;                  /* g_oom set; dispatch unwinds */
    return OBJ_VAL(r);
}
void as_define_native(const char *name, NativeFn fn)
{
    int len = (int)strlen(name);
    ObjStr *nm = as_str_copy(name, len);
    if (!nm) return;                         /* g_oom set; setup unwinds on the first DISPATCH */
    *builtin_slot(nm, 1) = OBJ_VAL(as_native_new(fn, name));
}
void as_define_int(const char *name, int64_t v)
{
    ObjStr *nm = as_str_copy(name, (int)strlen(name));
    if (!nm) return;                         /* g_oom set; setup unwinds on the first DISPATCH */
    *builtin_slot(nm, 1) = INT_VAL(v);
}
void as_add_module_source(const char *name, const char *src)
{
    if (nmsrc < 32) { msrc[nmsrc].name = name; msrc[nmsrc].src = src; nmsrc++; }
}

/* ---- calls ---- */
static int call_fn(ObjFn *fn, int argc)
{
    if (argc != fn->arity) return runtime_error("%s expected %d argument(s) but got %d",
                                                 fn->name ? fn->name->chars : "script", fn->arity, argc);
    if (frame_count == FRAMES_MAX) return runtime_error("call depth exceeded");
    Frame *f = &frames[frame_count++];
    f->fn = fn; f->closure = NULL; f->ip = fn->code; f->slots = sp - argc - 1; f->is_init = 0;  /* slot 0 = the callee */
    return 0;
}
static int call_closure(ObjClosure *cl, int argc)
{
    ObjFn *fn = cl->fn;
    if (argc != fn->arity) return runtime_error("%s expected %d argument(s) but got %d",
                                                 fn->name ? fn->name->chars : "fn", fn->arity, argc);
    if (frame_count == FRAMES_MAX) return runtime_error("call depth exceeded");
    Frame *f = &frames[frame_count++];
    f->fn = fn; f->closure = cl; f->ip = fn->code; f->slots = sp - argc - 1; f->is_init = 0;
    return 0;
}
static int call_value(Value callee, int argc)   /* 0 ok, 1 error */
{
    if (IS_OBJ(callee)) {
        switch (AS_OBJ(callee)->type) {
        case O_FN: return call_fn((ObjFn *)AS_OBJ(callee), argc);
        case O_CLOSURE: return call_closure((ObjClosure *)AS_OBJ(callee), argc);
        case O_CLASS: {
            ObjClass *k = (ObjClass *)AS_OBJ(callee);
            ObjInstance *in = as_instance_new(k);     /* self-roots its 2 allocs */
            sp[-argc - 1] = OBJ_VAL(in);               /* replace the class at the callee slot (=slot0=self) with the instance */
            Value init;
            if (as_dict_get(k->methods, OBJ_VAL(as_str_copy("init", 4)), &init)) {
                /* self is slot0 (the instance, just placed); user args at slots[1..]; init's
                 * arity excludes self, so call with argc. Mark is_init so OP_RET returns self. */
                if (call_closure(AS_CLOSURE(init), argc)) return 1;
                frames[frame_count - 1].is_init = 1;   /* OP_RET will return slots[0] (self) */
                return 0;
            }
            if (argc != 0) return runtime_error("%.*s() takes no arguments (no init)", k->name->len, k->name->chars);
            return 0;                                  /* instance already on the stack as the result */
        }
        case O_BOUND_METHOD: {
            ObjBoundMethod *bm = (ObjBoundMethod *)AS_OBJ(callee);
            sp[-argc - 1] = bm->receiver;              /* receiver at the callee slot -> slot0 = self */
            return call_closure(bm->method, argc);     /* arity excludes self */
        }
        case O_NATIVE: {
            g_native_err = 0;
            Value r = ((ObjNative *)AS_OBJ(callee))->fn(argc, sp - argc);
            sp -= argc + 1;            /* drop args + callee */
            if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); g_stack_overflow = 1; return 1; }
            push(r);
            return g_native_err;       /* native set as_err -> abort */
        }
        default: break;
        }
    }
    return runtime_error("can only call functions");
}

#define IS_NUM(v) (IS_INT(v) || IS_FLOAT(v))

/* Find or create the upvalue capturing stack slot `slot`; shared so two closures
 * over the same variable see each other's writes. List sorted by address descending. */
static ObjUpvalue *capture_upvalue(Value *slot)
{
    ObjUpvalue *prev = NULL, *cur = open_upvalues;
    while (cur && cur->location > slot) { prev = cur; cur = cur->next; }
    if (cur && cur->location == slot) return cur;
    ObjUpvalue *uv = as_upvalue_new(slot);
    if (!uv) return NULL;                    /* g_oom set; caller stores NULL, dispatch unwinds */
    uv->next = cur;
    if (prev) prev->next = uv; else open_upvalues = uv;
    return uv;
}
/* Close every open upvalue at or above `last` (move it off the stack into its own
 * storage). Called when captured locals die and on function return. */
static void close_upvalues(Value *last)
{
    while (open_upvalues && open_upvalues->location >= last) {
        ObjUpvalue *uv = open_upvalues;
        uv->closed = *uv->location;
        uv->location = &uv->closed;
        open_upvalues = uv->next;
    }
}

/* Look up `name` in `klass`'s (copy-down) method table; if found, push a bound
 * method (receiver bound) onto the stack as the result.
 * Returns 1 if bound (caller should break), 0 if no such method, -1 on stack
 * overflow (caller goes to err). Allocates a bound method: its inputs (recv, the
 * found closure) are already rooted before the single alloc, so no rooting gap. */
static int bind_method(ObjClass *klass, ObjStr *name, Value receiver)
{
    Value m;
    if (!as_dict_get(klass->methods, OBJ_VAL(name), &m)) return 0;
    ObjBoundMethod *bm = as_bound_method_new(receiver, AS_CLOSURE(m));
    if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); g_stack_overflow = 1; return -1; }
    push(OBJ_VAL(bm));
    return 1;
}

static int run_until(int floor);
static ObjModule *as_import(ObjStr *name);

/* Run `script` to completion on the shared stack (used for the main program and
 * for each imported module); returns when its frame returns. */
static int run_module(ObjFn *script)
{
    int floor = frame_count;
    if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); return 1; }
    push(OBJ_VAL(script));
    if (call_fn(script, 0)) return 1;
    return run_until(floor);
}

/* Load (compile + run) a module by name, with caching; returns its namespace. */
static ObjModule *as_import(ObjStr *name)
{
    ObjModule *m = module_find(name);
    if (m) return m;                              /* cached (loaded, or mid-load == partial) */
    if (nmodules >= 64) { runtime_error("too many modules"); return NULL; }
    m = as_module_new(name->chars, name->len);    /* protects itself across its second alloc */
    if (!m) { runtime_error("out of memory"); return NULL; }
    modules[nmodules++] = m;                       /* register before running -> circular-safe */
    ObjFn *script = module_try_la(name);           /* prefer a precompiled NAME.la */
    if (script) {
        stamp_module(script, m);                    /* pointer writes only -- GC-safe; must precede run_module (OP_DEF_GLOBAL needs ->module) */
    } else {
        char *src = module_source(name);            /* .as fallback (+ in-memory registry) */
        if (!src) {
            if (nmodules > 0 && modules[nmodules - 1] == m) nmodules--;  /* drop partial -> retry re-attempts */
            runtime_error("cannot import module '%.*s'", name->len, name->chars);
            return NULL;
        }
        script = as_compile_module(src, m);         /* stamps ->module via the compiler's g_module */
        free(src);
        if (!script) {                              /* compiler set as_err */
            if (nmodules > 0 && modules[nmodules - 1] == m) nmodules--;  /* drop partial -> retry re-attempts */
            return NULL;
        }
    }
    if (run_module(script)) {                        /* run_module push()es script -> rooted before any alloc */
        if (nmodules > 0 && modules[nmodules - 1] == m) nmodules--;      /* drop partial -> retry re-attempts */
        return NULL;
    }
    m->state = 1;
    return m;
}

static int run_until(int floor)
{
    Frame *frame = &frames[frame_count - 1];
#define READ_BYTE()  (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONST() (frame->fn->consts[READ_SHORT()])

    /* Computed-goto threaded dispatch: one label per OpCode (in enum order), so
     * the central indirect branch becomes ~51 monomorphic jumps the CPU branch
     * predictor can specialize -- the canonical bytecode-VM speedup (CPython,
     * LuaJIT). Clang (host + x86_64-elf) supports &&label / goto *ptr. Behavior
     * is identical to the old switch: each arm's body is unchanged, DISPATCH()
     * replaces break, and out-of-range / handler-less opcodes (e.g. OP_GET_ATTR)
     * land on op_BAD == the old `default: bad opcode`. */
    static void *const dispatch[] = {
        &&op_CONST, &&op_NIL, &&op_TRUE, &&op_FALSE, &&op_POP,
        &&op_GET_LOCAL, &&op_SET_LOCAL, &&op_GET_GLOBAL, &&op_SET_GLOBAL, &&op_DEF_GLOBAL,
        &&op_ADD, &&op_SUB, &&op_MUL, &&op_DIV, &&op_MOD, &&op_NEG,
        &&op_EQ, &&op_NE, &&op_LT, &&op_LE, &&op_GT, &&op_GE, &&op_NOT,
        &&op_JUMP, &&op_JUMP_IF_FALSE, &&op_LOOP,
        &&op_CALL, &&op_RET,
        &&op_MAKE_LIST, &&op_INDEX_GET, &&op_INDEX_SET, &&op_LEN, &&op_INVOKE,
        &&op_BAD /* OP_GET_ATTR: no handler in the switch -> default */, &&op_IMPORT,
        &&op_MAKE_DICT, &&op_IN, &&op_ITER,
        &&op_CLOSURE, &&op_GET_UPVALUE, &&op_SET_UPVALUE, &&op_CLOSE_UPVALUE,
        &&op_CLASS, &&op_INHERIT, &&op_METHOD,
        &&op_GET_PROPERTY, &&op_SET_PROPERTY, &&op_GET_SUPER,
        &&op_SETUP_TRY, &&op_POP_TRY, &&op_RAISE,
        &&op_BAND, &&op_BOR, &&op_BXOR, &&op_BNOT, &&op_SHL, &&op_SHR, &&op_POW,
    };
    uint8_t op;
#define DISPATCH() do { \
        if (g_stack_overflow) { g_stack_overflow = 0; goto err; } \
        if (g_oom) { g_oom = 0; runtime_error("out of memory"); goto err; } \
        op = READ_BYTE(); \
        if (op >= (uint8_t)(sizeof(dispatch) / sizeof(dispatch[0]))) goto op_BAD; \
        goto *dispatch[op]; } while (0)

    DISPATCH();
    {
        op_CONST: checked_push(READ_CONST()); DISPATCH();
        op_NIL:   checked_push(NIL_VAL); DISPATCH();
        op_TRUE:  checked_push(BOOL_VAL(1)); DISPATCH();
        op_FALSE: checked_push(BOOL_VAL(0)); DISPATCH();
        op_POP:   pop(); DISPATCH();

        op_GET_LOCAL: { uint8_t s = READ_BYTE(); checked_push(frame->slots[s]); DISPATCH(); }
        op_SET_LOCAL: { uint8_t s = READ_BYTE(); frame->slots[s] = peek(0); DISPATCH(); }
        op_GET_GLOBAL: {
            int ki = READ_SHORT();
            Value *s = global_slot(frame->fn, ki);
            if (!s) { ObjStr *n = AS_STR(frame->fn->consts[ki]);
                      runtime_error("undefined variable '%.*s'", n->len, n->chars); goto err; }
            checked_push(*s); DISPATCH();
        }
        op_DEF_GLOBAL: { ObjStr *n = AS_STR(READ_CONST()); *as_module_slot(frame->fn->module, n, 1) = peek(0); pop(); DISPATCH(); }
        op_SET_GLOBAL: {
            int ki = READ_SHORT();
            Value *s = global_slot(frame->fn, ki);
            if (!s) { ObjStr *n = AS_STR(frame->fn->consts[ki]);
                      runtime_error("undefined variable '%.*s'", n->len, n->chars); goto err; }
            *s = peek(0); DISPATCH();
        }

        op_ADD: {
            Value b = peek(0), a = peek(1);
            /* int+int promotes to float on i64 overflow (no silent two's-complement
             * wrap to a negative -- the "big number went negative" surprise). */
            if (IS_INT(a) && IS_INT(b))      { int64_t r; if (__builtin_add_overflow(AS_INT(a), AS_INT(b), &r)) { sp -= 2; push(FLOAT_VAL((double)AS_INT(a) + (double)AS_INT(b))); } else { sp -= 2; push(INT_VAL(r)); } }
            else if (IS_NUM(a) && IS_NUM(b)) { sp -= 2; push(FLOAT_VAL(AS_NUM(a) + AS_NUM(b))); }
            else if (IS_STR(a) && IS_STR(b)) { ObjStr *s = str_concat(AS_STR(a), AS_STR(b)); if (!s) goto err; sp -= 2; push(OBJ_VAL(s)); }
            else { runtime_error("operands of '+' must be numbers or strings"); goto err; }
            DISPATCH();
        }
        op_SUB: {
            Value b = peek(0), a = peek(1);
            if (IS_INT(a) && IS_INT(b))      { int64_t r; if (__builtin_sub_overflow(AS_INT(a), AS_INT(b), &r)) { sp -= 2; push(FLOAT_VAL((double)AS_INT(a) - (double)AS_INT(b))); } else { sp -= 2; push(INT_VAL(r)); } }
            else if (IS_NUM(a) && IS_NUM(b)) { sp -= 2; push(FLOAT_VAL(AS_NUM(a) - AS_NUM(b))); }
            else { runtime_error("operands of '-' must be numbers"); goto err; }
            DISPATCH();
        }
        op_MUL: {
            Value b = peek(0), a = peek(1);
            if (IS_INT(a) && IS_INT(b))      { int64_t r; if (__builtin_mul_overflow(AS_INT(a), AS_INT(b), &r)) { sp -= 2; push(FLOAT_VAL((double)AS_INT(a) * (double)AS_INT(b))); } else { sp -= 2; push(INT_VAL(r)); } }
            else if (IS_NUM(a) && IS_NUM(b)) { sp -= 2; push(FLOAT_VAL(AS_NUM(a) * AS_NUM(b))); }
            else { runtime_error("operands of '*' must be numbers"); goto err; }
            DISPATCH();
        }
        op_DIV: {
            Value b = peek(0), a = peek(1);
            if (!IS_NUM(a) || !IS_NUM(b)) { runtime_error("operands of '/' must be numbers"); goto err; }
            if (IS_INT(a) && IS_INT(b)) {
                if (AS_INT(b) == 0) { runtime_error("integer division by zero"); goto err; }
                if (AS_INT(a) == INT64_MIN && AS_INT(b) == -1) { runtime_error("integer overflow: INT64_MIN / -1"); goto err; }
                sp -= 2; push(INT_VAL(AS_INT(a) / AS_INT(b)));
            } else { sp -= 2; push(FLOAT_VAL(AS_NUM(a) / AS_NUM(b))); }
            DISPATCH();
        }
        op_MOD: {
            Value b = peek(0), a = peek(1);
            if (!IS_INT(a) || !IS_INT(b)) { runtime_error("operands of '%%' must be integers"); goto err; }
            if (AS_INT(b) == 0) { runtime_error("modulo by zero"); goto err; }
            if (AS_INT(a) == INT64_MIN && AS_INT(b) == -1) { runtime_error("integer overflow: INT64_MIN %% -1"); goto err; }
            sp -= 2; push(INT_VAL(AS_INT(a) % AS_INT(b)));
            DISPATCH();
        }
        op_BAND: {
            Value b = peek(0), a = peek(1);
            if (!IS_INT(a) || !IS_INT(b)) { runtime_error("operands of '&' must be integers"); goto err; }
            sp -= 2; push(INT_VAL(AS_INT(a) & AS_INT(b))); DISPATCH();
        }
        op_BOR: {
            Value b = peek(0), a = peek(1);
            if (!IS_INT(a) || !IS_INT(b)) { runtime_error("operands of '|' must be integers"); goto err; }
            sp -= 2; push(INT_VAL(AS_INT(a) | AS_INT(b))); DISPATCH();
        }
        op_BXOR: {
            Value b = peek(0), a = peek(1);
            if (!IS_INT(a) || !IS_INT(b)) { runtime_error("operands of '^' must be integers"); goto err; }
            sp -= 2; push(INT_VAL(AS_INT(a) ^ AS_INT(b))); DISPATCH();
        }
        op_BNOT: {
            Value a = peek(0);
            if (!IS_INT(a)) { runtime_error("operand of '~' must be an integer"); goto err; }
            sp--; push(INT_VAL(~AS_INT(a))); DISPATCH();
        }
        op_SHL: {
            Value b = peek(0), a = peek(1);
            if (!IS_INT(a) || !IS_INT(b)) { runtime_error("operands of '<<' must be integers"); goto err; }
            int64_t sh = AS_INT(b);
            if (sh < 0 || sh > 63) { runtime_error("shift amount out of range (0..63)"); goto err; }
            sp -= 2; push(INT_VAL((int64_t)((uint64_t)AS_INT(a) << sh))); DISPATCH();
        }
        op_SHR: {
            Value b = peek(0), a = peek(1);
            if (!IS_INT(a) || !IS_INT(b)) { runtime_error("operands of '>>' must be integers"); goto err; }
            int64_t sh = AS_INT(b);
            if (sh < 0 || sh > 63) { runtime_error("shift amount out of range (0..63)"); goto err; }
            sp -= 2; push(INT_VAL(AS_INT(a) >> sh)); DISPATCH();   /* arithmetic (sign-preserving) */
        }
        op_POW: {
            Value b = peek(0), a = peek(1);
            if (IS_INT(a) && IS_INT(b)) {
                int64_t base = AS_INT(a), e = AS_INT(b);
                if (e < 0) { runtime_error("** with a negative int exponent (no float pow in /bin/as)"); goto err; }
                int64_t r = 1, bb = base, ee = e; int ovf = 0;     /* promote to float on i64 overflow */
                while (ee > 0 && !ovf) {
                    if (ee & 1) { if (__builtin_mul_overflow(r, bb, &r)) ovf = 1; }
                    ee >>= 1;
                    if (ee > 0 && __builtin_mul_overflow(bb, bb, &bb)) ovf = 1;
                }
                if (ovf) { double fr = 1.0, fb = (double)base; int64_t fe = e; while (fe > 0) { if (fe & 1) fr *= fb; fb *= fb; fe >>= 1; } sp -= 2; push(FLOAT_VAL(fr)); }
                else { sp -= 2; push(INT_VAL(r)); }
            } else if (IS_NUM(a) && IS_NUM(b)) {
                if (!IS_INT(b)) { runtime_error("** float exponent unsupported (no libm in /bin/as)"); goto err; }
                double base = AS_NUM(a); int64_t e = AS_INT(b); int neg = e < 0; if (neg) { if (e == INT64_MIN) { runtime_error("** exponent overflow: cannot negate INT64_MIN"); goto err; } e = -e; }
                double r = 1.0; while (e > 0) { if (e & 1) r *= base; base *= base; e >>= 1; }
                if (neg) r = 1.0 / r;
                sp -= 2; push(FLOAT_VAL(r));
            } else { runtime_error("operands of '**' must be numbers"); goto err; }
            DISPATCH();
        }
        op_NEG: {
            Value a = peek(0);
            if (IS_INT(a)) {
                if (AS_INT(a) == INT64_MIN) { runtime_error("integer overflow: cannot negate INT64_MIN"); goto err; }
                sp--; push(INT_VAL(-AS_INT(a)));
            }
            else if (IS_FLOAT(a)) { sp--; push(FLOAT_VAL(-AS_FLOAT(a))); }
            else { runtime_error("operand of unary '-' must be a number"); goto err; }
            DISPATCH();
        }
        op_NOT: { Value a = pop(); push(BOOL_VAL(!as_truthy(a))); DISPATCH(); }

        op_EQ: { Value b = pop(), a = pop();
            int eq = (IS_NUM(a) && IS_NUM(b)) ? (AS_NUM(a) == AS_NUM(b)) : as_value_eq(a, b);
            push(BOOL_VAL(eq)); DISPATCH(); }
        op_NE: { Value b = pop(), a = pop();
            int eq = (IS_NUM(a) && IS_NUM(b)) ? (AS_NUM(a) == AS_NUM(b)) : as_value_eq(a, b);
            push(BOOL_VAL(!eq)); DISPATCH(); }
        op_LT: { Value b = pop(), a = pop(); if (!IS_NUM(a) || !IS_NUM(b)) { runtime_error("'<' needs numbers"); goto err; } push(BOOL_VAL(AS_NUM(a) <  AS_NUM(b))); DISPATCH(); }
        op_LE: { Value b = pop(), a = pop(); if (!IS_NUM(a) || !IS_NUM(b)) { runtime_error("'<=' needs numbers"); goto err; } push(BOOL_VAL(AS_NUM(a) <= AS_NUM(b))); DISPATCH(); }
        op_GT: { Value b = pop(), a = pop(); if (!IS_NUM(a) || !IS_NUM(b)) { runtime_error("'>' needs numbers"); goto err; } push(BOOL_VAL(AS_NUM(a) >  AS_NUM(b))); DISPATCH(); }
        op_GE: { Value b = pop(), a = pop(); if (!IS_NUM(a) || !IS_NUM(b)) { runtime_error("'>=' needs numbers"); goto err; } push(BOOL_VAL(AS_NUM(a) >= AS_NUM(b))); DISPATCH(); }

        op_JUMP:          { uint16_t o = READ_SHORT(); frame->ip += o; DISPATCH(); }
        op_JUMP_IF_FALSE: { uint16_t o = READ_SHORT(); if (!as_truthy(peek(0))) frame->ip += o; DISPATCH(); }
        op_LOOP:          { uint16_t o = READ_SHORT(); frame->ip -= o; DISPATCH(); }

        op_CALL: {
            int argc = READ_BYTE();
            if (call_value(peek(argc), argc)) goto err;
            frame = &frames[frame_count - 1];   /* native: same frame; fn: the new one */
            DISPATCH();
        }
        op_RET: {
            Value result = pop();
            /* Constructor (init) returns self (slots[0]) rather than init's computed value,
             * so that `Name(args)` evaluates to the new instance. */
            if (frame->is_init) result = frame->slots[0];
            close_upvalues(frame->slots);     /* close this fn's captured locals before unwinding */
            /* return-from-inside-try: discard handlers registered in this frame,
             * else a dead frame's handler would dangle and mis-catch later. */
            while (handler_count > 0 && handler_stack[handler_count - 1].frame_index >= frame_count)
                handler_count--;
            frame_count--;
            if (frame_count == floor) { pop(); return 0; }   /* this script/module is done */
            sp = frame->slots;                                /* discard callee + args + locals */
            push(result);
            frame = &frames[frame_count - 1];
            DISPATCH();
        }

        op_SETUP_TRY: {
            uint16_t off = READ_SHORT();                 /* forward offset to the except block */
            if (handler_count >= FRAMES_MAX) { runtime_error("too many nested try blocks"); goto err; }
            handler_stack[handler_count].handler_ip = frame->ip + off;
            handler_stack[handler_count].sp = sp;
            handler_stack[handler_count].frame_index = frame_count;
            handler_count++;
            DISPATCH();
        }
        op_POP_TRY: {
            uint16_t off = READ_SHORT();                 /* forward offset past the except block */
            if (handler_count > 0) handler_count--;      /* try body finished normally: drop handler */
            frame->ip += off;                            /* skip the except block */
            DISPATCH();
        }
        op_RAISE: {
            Value v = pop();
            throw_value(v);
            goto err;
        }

        op_MAKE_LIST: {
            int n = READ_BYTE();
            ObjList *l = as_list_new();
            if (!l) goto err;                     /* g_oom set */
            for (int i = 0; i < n; i++) as_list_push(l, sp[-n + i]);
            sp -= n;
            push(OBJ_VAL(l));
            DISPATCH();
        }
        op_MAKE_DICT: {
            int n = READ_BYTE();
            ObjDict *d = as_dict_new();
            if (!d) goto err;                     /* g_oom set */
            for (int i = 0; i < n; i++) {
                Value key = sp[-2 * n + 2 * i];
                Value val = sp[-2 * n + 2 * i + 1];
                if (!as_dict_set(d, key, val)) { runtime_error("dict key must be a string or int"); goto err; }
            }
            sp -= 2 * n;
            push(OBJ_VAL(d));
            DISPATCH();
        }
        op_INDEX_GET: {
            Value idx = pop(), obj = pop();
            if (IS_PTR(obj)) {
                if (!IS_INT(idx)) { runtime_error("pointer index must be an integer"); goto err; }
                ObjPtr *p = AS_PTR(obj);
                uint64_t raw = as_ll_peek(p->addr + (uint64_t)(AS_INT(idx) * p->width), p->width);
                int64_t v = (int64_t)raw;
                if (p->is_signed) {                       /* sign-extend from width bytes */
                    int bits = p->width * 8;
                    if (bits < 64) { int64_t m = (int64_t)1 << (bits - 1); v = (v ^ m) - m; }
                }
                push(INT_VAL(v));
            } else if (IS_LIST(obj)) {
                if (!IS_INT(idx)) { runtime_error("list index must be an integer"); goto err; }
                ObjList *l = AS_LIST(obj); int64_t i = AS_INT(idx); if (i < 0) i += l->count;
                if (i < 0 || i >= l->count) { runtime_error("list index out of range"); goto err; }
                push(l->items[i]);
            } else if (IS_RANGE(obj)) {
                if (!IS_INT(idx)) { runtime_error("list index must be an integer"); goto err; }
                ObjRange *r = AS_RANGE(obj); int64_t i = AS_INT(idx); if (i < 0) i += r->count;
                if (i < 0 || i >= r->count) { runtime_error("list index out of range"); goto err; }
                push(INT_VAL(RANGE_AT(r, i)));
            } else if (IS_STR(obj)) {
                if (!IS_INT(idx)) { runtime_error("string index must be an integer"); goto err; }
                ObjStr *s = AS_STR(obj); int64_t i = AS_INT(idx); if (i < 0) i += s->len;
                if (i < 0 || i >= s->len) { runtime_error("string index out of range"); goto err; }
                push(OBJ_VAL(as_str_copy(s->chars + i, 1)));
            } else if (IS_DICT(obj)) {
                if (!IS_STR(idx) && !IS_INT(idx)) { runtime_error("dict key must be a string or int"); goto err; }
                Value out;
                if (!as_dict_get(AS_DICT(obj), idx, &out)) { runtime_error("key not found"); goto err; }
                push(out);
            } else { runtime_error("only lists, strings, and dicts can be indexed"); goto err; }
            DISPATCH();
        }
        op_INDEX_SET: {
            Value val = pop(), idx = pop(), obj = pop();
            if (IS_PTR(obj)) {
                if (!IS_INT(idx) || !IS_INT(val)) { runtime_error("pointer index/value must be integers"); goto err; }
                ObjPtr *p = AS_PTR(obj);
                as_ll_poke(p->addr + (uint64_t)(AS_INT(idx) * p->width), p->width, (uint64_t)AS_INT(val));
                DISPATCH();
            }
            if (IS_DICT(obj)) {
                if (!IS_STR(idx) && !IS_INT(idx)) { runtime_error("dict key must be a string or int"); goto err; }
                as_dict_set(AS_DICT(obj), idx, val);   /* key type checked above, so it can't fail */
                DISPATCH();
            }
            if (!IS_LIST(obj)) { runtime_error("only lists support item assignment"); goto err; }
            if (!IS_INT(idx)) { runtime_error("list index must be an integer"); goto err; }
            ObjList *l = AS_LIST(obj); int64_t i = AS_INT(idx); if (i < 0) i += l->count;
            if (i < 0 || i >= l->count) { runtime_error("list assignment index out of range"); goto err; }
            l->items[i] = val;
            DISPATCH();
        }
        op_LEN: {
            Value v = pop();
            if (IS_LIST(v))       push(INT_VAL(AS_LIST(v)->count));
            else if (IS_STR(v))   push(INT_VAL(AS_STR(v)->len));
            else if (IS_DICT(v))  push(INT_VAL(AS_DICT(v)->live));
            else if (IS_RANGE(v)) push(INT_VAL(AS_RANGE(v)->count));
            else { runtime_error("object has no length"); goto err; }
            DISPATCH();
        }
        op_INVOKE: {
            ObjStr *name = AS_STR(READ_CONST());
            uint8_t argc = READ_BYTE();
            Value recv = peek(argc);
            if (IS_MODULE(recv)) {                          /* mod.fn(args) */
                Value *s = as_module_slot(AS_MODULE(recv), name, 0);
                if (!s) { runtime_error("module '%.*s' has no '%.*s'", AS_MODULE(recv)->name->len, AS_MODULE(recv)->name->chars, name->len, name->chars); goto err; }
                sp[-argc - 1] = *s;                         /* replace receiver with the callable */
                if (call_value(*s, argc)) goto err;
                frame = &frames[frame_count - 1];
            } else if (IS_LIST(recv)) {
                if (name_eq(name, "append")) {
                    if (argc != 1) { runtime_error("append() takes 1 argument"); goto err; }
                    as_list_push(AS_LIST(recv), peek(0));
                    sp -= argc + 1; push(NIL_VAL);
                } else { runtime_error("list has no method '%.*s'", name->len, name->chars); goto err; }
            } else if (IS_STR(recv)) {
                /* M23 string methods. GC discipline: the receiver + args stay on the
                 * stack (rooted) until the result exists; adjust sp LAST. ASCII-only
                 * case/whitespace, byte-oriented -- consistent with string indexing. */
                ObjStr *s = AS_STR(recv);
                if (name_eq(name, "join")) {
                    if (argc != 1 || !IS_LIST(peek(0))) { runtime_error("join() takes one list argument"); goto err; }
                    ObjList *l = AS_LIST(peek(0));
                    int total = 0;
                    for (int i = 0; i < l->count; i++) {
                        if (!IS_STR(l->items[i])) { runtime_error("join() list items must be strings"); goto err; }
                        total += AS_STR(l->items[i])->len;
                    }
                    if (l->count > 1) total += s->len * (l->count - 1);
                    char *buf = (char *)as_malloc((size_t)total + 1);
                    if (!buf) goto err;
                    int o = 0;
                    for (int i = 0; i < l->count; i++) {
                        if (i) { memcpy(buf + o, s->chars, (size_t)s->len); o += s->len; }
                        ObjStr *it = AS_STR(l->items[i]);
                        memcpy(buf + o, it->chars, (size_t)it->len); o += it->len;
                    }
                    buf[total] = 0;
                    ObjStr *r = as_str_take(buf, total);   /* inputs still stack-rooted */
                    if (!r) goto err;
                    sp -= argc + 1; push(OBJ_VAL(r));
                } else if (name_eq(name, "split")) {
                    ObjStr *sep = NULL;
                    if (argc == 1) {
                        if (!IS_STR(peek(0))) { runtime_error("split() separator must be a string"); goto err; }
                        sep = AS_STR(peek(0));
                        if (sep->len == 0) { runtime_error("split() separator must not be empty"); goto err; }
                    } else if (argc != 0) { runtime_error("split() takes 0 or 1 arguments"); goto err; }
                    /* Rooting the result list is enough, and is what lets the
                     * collector keep running through the whole split: every slice
                     * string is pushed into the list before the next allocation,
                     * so it is reachable by the time a collection could see it.
                     * This used to disable GC outright for the duration, which on
                     * a large input meant thousands of allocations with the heap
                     * growing unbounded. */
                    ObjList *l = as_list_new();
                    if (!l) goto err;                            /* g_oom set */
                    as_gc_protect((Obj *)l);
                    if (sep) {
                        int start = 0;
                        for (int i = 0; i + sep->len <= s->len; ) {
                            if (memcmp(s->chars + i, sep->chars, (size_t)sep->len) == 0) {
                                as_list_push(l, OBJ_VAL(as_str_copy(s->chars + start, i - start)));
                                i += sep->len; start = i;
                            } else i++;
                        }
                        as_list_push(l, OBJ_VAL(as_str_copy(s->chars + start, s->len - start)));
                    } else {
                        for (int i = 0; i < s->len; ) {        /* runs of ASCII whitespace */
                            while (i < s->len && (s->chars[i]==' '||s->chars[i]=='\t'||s->chars[i]=='\n'||s->chars[i]=='\r'||s->chars[i]=='\f'||s->chars[i]=='\v')) i++;
                            if (i >= s->len) break;
                            int st = i;
                            while (i < s->len && !(s->chars[i]==' '||s->chars[i]=='\t'||s->chars[i]=='\n'||s->chars[i]=='\r'||s->chars[i]=='\f'||s->chars[i]=='\v')) i++;
                            as_list_push(l, OBJ_VAL(as_str_copy(s->chars + st, i - st)));
                        }
                    }
                    as_gc_pop_disable();
                    if (g_oom) goto err;
                    sp -= argc + 1; push(OBJ_VAL(l));
                } else if (name_eq(name, "strip")) {
                    if (argc != 0) { runtime_error("strip() takes no arguments"); goto err; }
                    int a = 0, b = s->len;
                    while (a < b && (s->chars[a]==' '||s->chars[a]=='\t'||s->chars[a]=='\n'||s->chars[a]=='\r'||s->chars[a]=='\f'||s->chars[a]=='\v')) a++;
                    while (b > a && (s->chars[b-1]==' '||s->chars[b-1]=='\t'||s->chars[b-1]=='\n'||s->chars[b-1]=='\r'||s->chars[b-1]=='\f'||s->chars[b-1]=='\v')) b--;
                    ObjStr *r = as_str_copy(s->chars + a, b - a);
                    if (!r) goto err;
                    sp -= argc + 1; push(OBJ_VAL(r));
                } else if (name_eq(name, "upper") || name_eq(name, "lower")) {
                    if (argc != 0) { runtime_error("upper()/lower() take no arguments"); goto err; }
                    int up = name_eq(name, "upper");
                    char *buf = (char *)as_malloc((size_t)s->len + 1);
                    if (!buf) goto err;
                    for (int i = 0; i < s->len; i++) {
                        char ch = s->chars[i];
                        if (up)  { if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A'); }
                        else     { if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a'); }
                        buf[i] = ch;
                    }
                    buf[s->len] = 0;
                    ObjStr *r = as_str_take(buf, s->len);
                    if (!r) goto err;
                    sp -= argc + 1; push(OBJ_VAL(r));
                } else if (name_eq(name, "replace")) {
                    if (argc != 2 || !IS_STR(peek(1)) || !IS_STR(peek(0))) { runtime_error("replace() takes two string arguments"); goto err; }
                    ObjStr *old = AS_STR(peek(1)), *new_ = AS_STR(peek(0));
                    if (old->len == 0) { runtime_error("replace() old string must not be empty"); goto err; }
                    int count = 0;                              /* pass 1: count occurrences */
                    for (int i = 0; i + old->len <= s->len; )
                        if (memcmp(s->chars + i, old->chars, (size_t)old->len) == 0) { count++; i += old->len; }
                        else i++;
                    long total = (long)s->len + (long)count * (new_->len - old->len);
                    char *buf = (char *)as_malloc((size_t)total + 1);
                    if (!buf) goto err;
                    int o = 0;                                  /* pass 2: write */
                    for (int i = 0; i < s->len; ) {
                        if (i + old->len <= s->len && memcmp(s->chars + i, old->chars, (size_t)old->len) == 0) {
                            memcpy(buf + o, new_->chars, (size_t)new_->len); o += new_->len; i += old->len;
                        } else buf[o++] = s->chars[i++];
                    }
                    buf[total] = 0;
                    ObjStr *r = as_str_take(buf, (int)total);
                    if (!r) goto err;
                    sp -= argc + 1; push(OBJ_VAL(r));
                } else if (name_eq(name, "sub")) {
                    /* s.sub(a, b) -> s[a:b] (clamped; the compiler's substring) */
                    if (argc != 2 || !IS_INT(peek(1)) || !IS_INT(peek(0))) { runtime_error("sub() takes (start, end) integers"); goto err; }
                    int64_t a = AS_INT(peek(1)), b2 = AS_INT(peek(0));
                    if (a < 0) a = 0;
                    if (b2 > s->len) b2 = s->len;
                    if (b2 < a) b2 = a;
                    ObjStr *r = as_str_copy(s->chars + a, (int)(b2 - a));
                    if (!r) goto err;
                    sp -= argc + 1; push(OBJ_VAL(r));
                } else if (name_eq(name, "find")) {
                    if (argc != 1 || !IS_STR(peek(0))) { runtime_error("find() takes one string argument"); goto err; }
                    ObjStr *sub = AS_STR(peek(0));
                    int64_t at = sub->len == 0 ? 0 : -1;        /* empty sub -> 0 (Python) */
                    if (sub->len > 0)
                        for (int i = 0; i + sub->len <= s->len; i++)
                            if (memcmp(s->chars + i, sub->chars, (size_t)sub->len) == 0) { at = i; break; }
                    sp -= argc + 1; push(INT_VAL(at));
                } else { runtime_error("string has no method '%.*s'", name->len, name->chars); goto err; }
            } else if (IS_DICT(recv)) {
                ObjDict *d = AS_DICT(recv);
                if (name_eq(name, "has")) {
                    if (argc != 1) { runtime_error("has() takes 1 argument"); goto err; }
                    Value k = peek(0);
                    if (!IS_STR(k) && !IS_INT(k)) { runtime_error("dict key must be a string or int"); goto err; }
                    int h = as_dict_has(d, k);
                    sp -= argc + 1; push(BOOL_VAL(h));
                } else if (name_eq(name, "get")) {
                    if (argc != 1 && argc != 2) { runtime_error("get() takes 1 or 2 arguments"); goto err; }
                    Value k = peek(argc - 1);
                    if (!IS_STR(k) && !IS_INT(k)) { runtime_error("dict key must be a string or int"); goto err; }
                    Value out;
                    if (!as_dict_get(d, k, &out)) out = (argc == 2) ? peek(0) : NIL_VAL;
                    sp -= argc + 1; push(out);
                } else if (name_eq(name, "remove")) {
                    if (argc != 1) { runtime_error("remove() takes 1 argument"); goto err; }
                    Value k = peek(0);
                    if (!IS_STR(k) && !IS_INT(k)) { runtime_error("dict key must be a string or int"); goto err; }
                    int r = as_dict_remove(d, k);
                    sp -= argc + 1; push(BOOL_VAL(r));
                } else if (name_eq(name, "keys")) {
                    if (argc != 0) { runtime_error("keys() takes no arguments"); goto err; }
                    ObjList *l = as_dict_keys(d);
                    sp -= argc + 1; push(OBJ_VAL(l));
                } else if (name_eq(name, "values")) {
                    if (argc != 0) { runtime_error("values() takes no arguments"); goto err; }
                    ObjList *l = as_dict_values(d);
                    sp -= argc + 1; push(OBJ_VAL(l));
                } else { runtime_error("dict has no method '%.*s'", name->len, name->chars); goto err; }
            } else if (IS_INSTANCE(recv)) {
                ObjInstance *in = AS_INSTANCE(recv);
                Value field;
                if (as_instance_get(in, name, &field)) {   /* a callable field shadows a method */
                    sp[-argc - 1] = field;                              /* replace receiver with the callable */
                    if (call_value(field, argc)) goto err;
                    frame = &frames[frame_count - 1];
                } else {
                    Value m;
                    if (!as_dict_get(in->klass->methods, OBJ_VAL(name), &m)) {
                        runtime_error("'%.*s instance' has no method '%.*s'", in->klass->name->len, in->klass->name->chars, name->len, name->chars); goto err;
                    }
                    /* recv is already at the callee slot (peek(argc)) -> slot0 = self; arity excludes self */
                    if (call_closure(AS_CLOSURE(m), argc)) goto err;
                    frame = &frames[frame_count - 1];
                }
            } else { runtime_error("'%.*s' is not a method of this type", name->len, name->chars); goto err; }
            DISPATCH();
        }
        op_GET_PROPERTY: {
            int ki = READ_SHORT();
            Value recv = peek(0);                 /* keep recv rooted across bind_method's alloc */
            if (IS_INSTANCE(recv)) {
                ObjInstance *in = AS_INSTANCE(recv);
                Value field;
                if (instance_get_cached(frame->fn, ki, in, &field)) { pop(); push(field); DISPATCH(); }
                ObjStr *n = AS_STR(frame->fn->consts[ki]);
                int b = bind_method(in->klass, n, recv);          /* pushes the bound method */
                if (b < 0) goto err;                              /* stack overflow */
                if (b) {
                    Value bm = pop(); pop(); push(bm);            /* drop the receiver, keep bound method */
                    DISPATCH();
                }
                runtime_error("'%.*s instance' has no property '%.*s'", in->klass->name->len, in->klass->name->chars,
                              AS_STR(frame->fn->consts[ki])->len, AS_STR(frame->fn->consts[ki])->chars); goto err;
            }
            if (IS_MODULE(recv)) {
                ObjStr *n = AS_STR(frame->fn->consts[ki]);
                Value *s = as_module_slot(AS_MODULE(recv), n, 0);
                if (!s) { runtime_error("module '%.*s' has no '%.*s'", AS_MODULE(recv)->name->len, AS_MODULE(recv)->name->chars, n->len, n->chars); goto err; }
                pop(); push(*s); DISPATCH();
            }
            runtime_error("only instances and modules have properties"); goto err;
        }
        op_SET_PROPERTY: {
            int ki = READ_SHORT();
            Value val = peek(0), recv = peek(1);
            if (!IS_INSTANCE(recv)) { runtime_error("only instances have settable fields"); goto err; }
            ObjInstance *in = AS_INSTANCE(recv);
            struct PCache *pc = fn_pcache(frame->fn, ki);
            if (pc && pc->shape_id == in->shape->id) {           /* known field of a known shape */
                in->slots[pc->slot] = val;
            } else {
                ObjStr *n = AS_STR(frame->fn->consts[ki]);
                /* A first assignment transitions the shape, so the cache is only
                 * armed for the update case -- caching the pre-transition shape
                 * would point the next instance at the wrong slot. */
                int existing = as_shape_find(in->shape, n) >= 0;
                if (!as_instance_set(in, n, val)) goto err;      /* g_oom set */
                if (pc && existing) { pc->shape_id = in->shape->id; pc->slot = as_shape_find(in->shape, n); }
            }
            sp -= 2; push(val);                   /* leave the assigned value as the expression result */
            DISPATCH();
        }
        op_IMPORT: {
            ObjStr *n = AS_STR(READ_CONST());
            ObjModule *m = as_import(n);
            if (!m) goto err;                  /* as_err set */
            frame = &frames[frame_count - 1];  /* importing may have run nested module frames */
            push(OBJ_VAL(m));
            DISPATCH();
        }

        op_IN: {
            Value cont = pop(), item = pop();
            int found = 0;
            if (IS_DICT(cont)) {
                if (!IS_STR(item) && !IS_INT(item)) { runtime_error("dict key must be a string or int"); goto err; }
                found = as_dict_has(AS_DICT(cont), item);
            } else if (IS_LIST(cont)) {
                ObjList *l = AS_LIST(cont);
                for (int i = 0; i < l->count; i++) {
                    Value e = l->items[i];
                    int eq = (IS_NUM(item) && IS_NUM(e)) ? (AS_NUM(item) == AS_NUM(e)) : as_value_eq(item, e);
                    if (eq) { found = 1; break; }
                }
            } else if (IS_RANGE(cont)) {
                /* Membership by arithmetic rather than by walking: `x in range(0, N)`
                 * is O(1) where the list it replaced was O(N). */
                ObjRange *r = AS_RANGE(cont);
                if (IS_INT(item) && r->count > 0) {
                    int64_t d = AS_INT(item) - r->start;
                    found = (d % r->step == 0) && (d / r->step) >= 0 && (d / r->step) < r->count;
                }
            } else if (IS_STR(cont)) {
                if (!IS_STR(item)) { runtime_error("substring test needs a string on the left of 'in'"); goto err; }
                ObjStr *hay = AS_STR(cont), *needle = AS_STR(item);
                if (needle->len == 0) found = 1;
                else for (int i = 0; i + needle->len <= hay->len; i++)
                    if (memcmp(hay->chars + i, needle->chars, (size_t)needle->len) == 0) { found = 1; break; }
            } else { runtime_error("'in' needs a dict, list, or string"); goto err; }
            push(BOOL_VAL(found));
            DISPATCH();
        }

        op_ITER: {
            if (IS_DICT(peek(0))) { ObjList *ks = as_dict_keys(AS_DICT(peek(0))); sp[-1] = OBJ_VAL(ks); }
            DISPATCH();   /* list/range/string: iterate as-is */
        }

        op_CLOSURE: {
            ObjFn *fn = AS_FN(READ_CONST());
            ObjClosure *cl = as_closure_new(fn);
            if (!cl) goto err;                    /* g_oom set */
            checked_push(OBJ_VAL(cl));   /* root cl NOW: capture_upvalue allocates and may trigger GC */
            for (int i = 0; i < cl->upvalue_count; i++) {
                uint8_t is_local = READ_BYTE();
                uint8_t index = READ_BYTE();
                /* The .la verifier bounds these for well-formed files; re-check at
                 * runtime so a corrupt capture pair can never fabricate an upvalue
                 * pointing outside the live stack (a type-confusion primitive).
                 * non-local: inherit from the enclosing closure. frame->closure is
                 * non-NULL here because any fn with upvalue_count>0 is nested in a
                 * function, which is always invoked via call_closure (never call_fn). */
                if (is_local) {
                    if (frame->slots + index < stack || frame->slots + index >= sp) { runtime_error("bad upvalue capture"); goto err; }
                    cl->upvalues[i] = capture_upvalue(frame->slots + index);
                } else {
                    if (!frame->closure || index >= frame->closure->upvalue_count) { runtime_error("bad upvalue capture"); goto err; }
                    cl->upvalues[i] = frame->closure->upvalues[index];
                }
            }
            DISPATCH();   /* cl is already on the stack as the result */
        }
        op_GET_UPVALUE: { uint8_t s = READ_BYTE(); checked_push(*frame->closure->upvalues[s]->location); DISPATCH(); }
        op_SET_UPVALUE: { uint8_t s = READ_BYTE(); *frame->closure->upvalues[s]->location = peek(0); DISPATCH(); }
        op_CLOSE_UPVALUE: { close_upvalues(sp - 1); pop(); DISPATCH(); }
        op_CLASS: {
            ObjStr *n = AS_STR(READ_CONST());
            ObjClass *k = as_class_new(n);   /* self-roots its 2 allocs (push-disable) */
            checked_push(OBJ_VAL(k));         /* root it as this op's result */
            DISPATCH();
        }
        op_METHOD: {
            ObjStr *n = AS_STR(READ_CONST());
            Value method = peek(0);           /* the closure */
            ObjClass *k = AS_CLASS(peek(1));  /* the class, kept underneath */
            as_dict_set(k->methods, OBJ_VAL(n), method);
            pop();                            /* drop the closure; class stays */
            DISPATCH();
        }
        op_INHERIT: {
            Value superv = peek(1), subv = peek(0);   /* stack: [.. super class] */
            if (!IS_CLASS(superv)) { runtime_error("superclass must be a class"); goto err; }
            ObjClass *sup = AS_CLASS(superv), *sub = AS_CLASS(subv);
            sub->super = sup;
            /* copy-down: inherit the parent's methods before the subclass defines its
             * own (so a subclass override later overwrites the inherited entry). dict
             * ops use realloc (no alloc_obj) so no GC fires while super/sub are live. */
            for (int i = 0; i < sup->methods->cap; i++) {
                DictEntry *e = &sup->methods->entries[i];
                if (e->kind == AS_DK_STR) as_dict_set(sub->methods, OBJ_VAL(e->kstr), e->val);
            }
            sp[-2] = sp[-1];                   /* drop super (below class), keep class on top */
            sp--;
            DISPATCH();
        }
        op_GET_SUPER: {
            ObjStr *name = AS_STR(READ_CONST());
            Value klassv = peek(0);           /* the enclosing class (kept rooted across the alloc) */
            ObjClass *sup = IS_CLASS(klassv) ? AS_CLASS(klassv)->super : NULL;
            if (!sup) { runtime_error("'super' used in a class with no superclass"); goto err; }
            /* Bind the superclass method to the *current* self (slot 0 of this method
             * frame), so `super.m(args)` calls it with self implicit -- same convention
             * as `obj.m(args)`. */
            int b = bind_method(sup, name, frame->slots[0]);
            if (b < 0) goto err;                              /* stack overflow */
            if (!b) {
                runtime_error("superclass has no method '%.*s'", name->len, name->chars); goto err;
            }
            sp[-2] = sp[-1]; sp--;            /* drop the class beneath the new bound method */
            DISPATCH();
        }
        op_BAD: runtime_error("bad opcode %d", op); goto err;
    }
    {
    err:
        ensure_exc();              /* fold native/built-in errors into g_exc */
        /* find the nearest live handler: the topmost entry whose frame is still on
         * the stack (frame_index <= frame_count) and belongs to THIS run_until
         * invocation (frame_index > floor, so an outer module's handlers are not
         * grabbed). Discard handlers above it. */
        while (handler_count > 0 && handler_stack[handler_count - 1].frame_index > frame_count)
            handler_count--;
        if (handler_count > 0 && handler_stack[handler_count - 1].frame_index > floor) {
            Handler *h = &handler_stack[--handler_count];   /* pop the handler we'll use */
            close_upvalues(h->sp);                          /* close captives above the try's sp */
            frame_count = h->frame_index;
            frame = &frames[frame_count - 1];
            frame->ip = h->handler_ip;                      /* resume at the except block */
            sp = h->sp;
            /* RAW bounds check (not checked_push, which would re-arm g_stack_overflow
             * -> infinite err: loop). On a second overflow no handler above floor is
             * found and we fall to the uncaught path. */
            if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); goto err; }
            push(g_exc);                                    /* hand the value to the except block */
            g_has_exc = 0;
            DISPATCH();                                      /* RESUME dispatch at the handler */
        }
        /* uncaught: finalize as_err for the caller, then abort. */
        if (IS_STR(g_exc)) {
            ObjStr *s = AS_STR(g_exc);
            int n = s->len < (int)sizeof(as_err) - 1 ? s->len : (int)sizeof(as_err) - 1;
            memcpy(as_err, s->chars, (size_t)n); as_err[n] = 0;
        } else if (as_err[0] == 0) {
            snprintf(as_err, sizeof as_err, "uncaught exception");
        }
        g_has_exc = 0;
        /* uncaught: fall through to the return below (was: break out of the loop) */
    }
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONST
#undef DISPATCH
    return 1;
}

int as_run(ObjFn *script)
{
    reset_stack();
    nbuiltins = 0; nmodules = 0;        /* fresh builtins + module cache (objects freed between runs) */
    as_gc_push_disable();              /* setup allocates natives while `script` isn't yet rooted */
    as_define_native("print", native_print);
    as_define_native("len", native_len);
    as_define_native("range", native_range);
    as_define_native("str", native_str);
    as_define_native("chr", native_chr);
    as_define_native("ord", native_ord);
    as_define_native("f64bits", native_f64bits);
    as_define_native("parse_float", native_parse_float);
    as_define_native("file_read", native_file_read);
    as_define_native("file_write", native_file_write);
    as_define_native("args", native_args);
    as_define_native("gc_stats", native_gc_stats);
    as_define_native("gc", native_gc);
    as_install_indirection();          /* addr/peek/poke/iNptr/syscall + SYS_* */
    if (script->module) { modules[nmodules++] = script->module; script->module->state = 1; }
    as_gc_pop_disable();               /* run_module pushes `script` first -> rooted before any GC */
    return run_module(script);
}

int as_interpret(const char *src)
{
    ObjFn *fn = as_compile(src);
    if (!fn) return 1;            /* as_err set by the compiler/lexer */
    return as_run(fn);
}
