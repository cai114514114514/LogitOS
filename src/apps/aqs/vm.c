#include "aqs.h"
#include <stdio.h>      /* vsnprintf */
#include <stdarg.h>

/* Stack bytecode VM. A1 uses a switch dispatch (correctness first); the design
 * earmarks computed-goto as a later speed pass. Locals live on the value stack
 * addressed off each call frame's base; globals are a small name->value table. */

#define STACK_MAX  (4096)
#define FRAMES_MAX (256)

typedef struct { ObjFn *fn; ObjClosure *closure; uint8_t *ip; Value *slots; } Frame;

static Value  stack[STACK_MAX];
static Value *sp;
static Frame  frames[FRAMES_MAX];
static int    frame_count;
static ObjUpvalue *open_upvalues;   /* live captured locals, sorted by stack addr desc */

/* Builtins (print/len/range/peek/.../SYS_*) are visible from every module; each
 * module has its own namespace (ObjModule.vars). GET_GLOBAL tries the running
 * function's module first, then falls back to builtins. */
static NameVal builtins[128];
static int     nbuiltins;

static ObjModule *modules[64];      /* loaded-module cache (by name) */
static int        nmodules;
static struct { const char *name; const char *src; } msrc[32];   /* in-memory module sources (tests) */
static int        nmsrc;

static Value *builtin_slot(ObjStr *name, int create)
{
    for (int i = 0; i < nbuiltins; i++)
        if (builtins[i].name->hash == name->hash && builtins[i].name->len == name->len
            && memcmp(builtins[i].name->chars, name->chars, name->len) == 0) return &builtins[i].val;
    if (!create || nbuiltins >= 128) return NULL;
    builtins[nbuiltins].name = name; builtins[nbuiltins].val = NIL_VAL;
    return &builtins[nbuiltins++].val;
}

static void push(Value v) { *sp++ = v; }
static Value pop(void)    { return *--sp; }
static Value peek(int d)  { return sp[-1 - d]; }
static void reset_stack(void) { sp = stack; frame_count = 0; open_upvalues = NULL; }

static int runtime_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(aqs_err, sizeof aqs_err, fmt, ap);
    va_end(ap);
    return 1;
}

/* Natives report failure by setting this flag (+ aqs_err) and returning nil;
 * call_value aborts the run when it's set. */
static int g_native_err;
Value aqs_native_fail(const char *msg) { snprintf(aqs_err, sizeof aqs_err, "%s", msg); g_native_err = 1; return NIL_VAL; }

static int name_eq(ObjStr *s, const char *lit)
{ int n = (int)strlen(lit); return s->len == n && memcmp(s->chars, lit, n) == 0; }

static ObjStr *str_concat(ObjStr *a, ObjStr *b)
{
    int n = a->len + b->len;
    char *buf = (char *)malloc((size_t)n + 1);
    memcpy(buf, a->chars, a->len); memcpy(buf + a->len, b->chars, b->len); buf[n] = 0;
    return aqs_str_take(buf, n);
}

/* Resolve a global read: the running function's module, then builtins. */
static Value *resolve_global(ObjFn *fn, ObjStr *name)
{
    Value *s = fn->module ? aqs_module_slot(fn->module, name, 0) : NULL;
    return s ? s : builtin_slot(name, 0);
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
 * in-memory registry first (tests), then NAME.aqs and /usr/aqs/NAME.aqs on disk. */
static char *module_source(ObjStr *name)
{
    for (int i = 0; i < nmsrc; i++)
        if ((int)strlen(msrc[i].name) == name->len && memcmp(msrc[i].name, name->chars, name->len) == 0) {
            int n = (int)strlen(msrc[i].src);
            char *buf = (char *)malloc((size_t)n + 1);
            if (buf) memcpy(buf, msrc[i].src, (size_t)n + 1);
            return buf;
        }
    char path[160];
    const char *dirs[] = { "", "/usr/aqs/" };
    for (int d = 0; d < 2; d++) {
        int p = 0;
        for (const char *pre = dirs[d]; *pre && p < 120; pre++) path[p++] = *pre;
        for (int i = 0; i < name->len && p < 150; i++) path[p++] = name->chars[i];
        const char *ext = ".aqs"; for (int i = 0; i < 4 && p < 156; i++) path[p++] = ext[i];
        path[p] = 0;
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t cap = 4096, len = 0; char *buf = (char *)malloc(cap);
        for (;;) {
            if (len + 4096 + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
            size_t r = fread(buf + len, 1, 4096, f);
            len += r; if (r < 4096) break;
        }
        buf[len] = 0; fclose(f);
        return buf;
    }
    return NULL;
}

/* ---- natives ---- */
static Value native_print(int argc, Value *args)
{
    for (int i = 0; i < argc; i++) { if (i) aqs_emit(" ", 1); aqs_print_value(args[i]); }
    aqs_emit("\n", 1);
    return NIL_VAL;
}
static Value native_len(int argc, Value *args)
{
    if (argc != 1) return aqs_native_fail("len() takes exactly 1 argument");
    if (IS_LIST(args[0])) return INT_VAL(AS_LIST(args[0])->count);
    if (IS_STR(args[0]))  return INT_VAL(AS_STR(args[0])->len);
    if (IS_DICT(args[0])) return INT_VAL(AS_DICT(args[0])->live);
    return aqs_native_fail("len() needs a list, string, or dict");
}
static Value native_gc_stats(int argc, Value *args)
{
    (void)argc; (void)args;
    return INT_VAL(aqs_gc_live());
}
static Value native_range(int argc, Value *args)
{
    int64_t start = 0, stop, step = 1;
    for (int i = 0; i < argc; i++) if (!IS_INT(args[i])) return aqs_native_fail("range() needs integer arguments");
    if (argc == 1)      { stop = AS_INT(args[0]); }
    else if (argc == 2) { start = AS_INT(args[0]); stop = AS_INT(args[1]); }
    else if (argc == 3) { start = AS_INT(args[0]); stop = AS_INT(args[1]); step = AS_INT(args[2]); }
    else return aqs_native_fail("range() takes 1 to 3 arguments");
    if (step == 0) return aqs_native_fail("range() step must not be zero");
    ObjList *l = aqs_list_new();
    if (step > 0) for (int64_t i = start; i < stop; i += step) aqs_list_push(l, INT_VAL(i));
    else          for (int64_t i = start; i > stop; i += step) aqs_list_push(l, INT_VAL(i));
    return OBJ_VAL(l);
}
void aqs_define_native(const char *name, NativeFn fn)
{
    int len = (int)strlen(name);
    *builtin_slot(aqs_str_copy(name, len), 1) = OBJ_VAL(aqs_native_new(fn, name));
}
void aqs_define_int(const char *name, int64_t v)
{
    *builtin_slot(aqs_str_copy(name, (int)strlen(name)), 1) = INT_VAL(v);
}
void aqs_add_module_source(const char *name, const char *src)
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
    f->fn = fn; f->closure = NULL; f->ip = fn->code; f->slots = sp - argc - 1;   /* slot 0 = the callee */
    return 0;
}
static int call_closure(ObjClosure *cl, int argc)
{
    ObjFn *fn = cl->fn;
    if (argc != fn->arity) return runtime_error("%s expected %d argument(s) but got %d",
                                                 fn->name ? fn->name->chars : "fn", fn->arity, argc);
    if (frame_count == FRAMES_MAX) return runtime_error("call depth exceeded");
    Frame *f = &frames[frame_count++];
    f->fn = fn; f->closure = cl; f->ip = fn->code; f->slots = sp - argc - 1;
    return 0;
}
static int call_value(Value callee, int argc)   /* 0 ok, 1 error */
{
    if (IS_OBJ(callee)) {
        switch (AS_OBJ(callee)->type) {
        case O_FN: return call_fn((ObjFn *)AS_OBJ(callee), argc);
        case O_CLOSURE: return call_closure((ObjClosure *)AS_OBJ(callee), argc);
        case O_NATIVE: {
            g_native_err = 0;
            Value r = ((ObjNative *)AS_OBJ(callee))->fn(argc, sp - argc);
            sp -= argc + 1;            /* drop args + callee */
            push(r);
            return g_native_err;       /* native set aqs_err -> abort */
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
    ObjUpvalue *uv = aqs_upvalue_new(slot);
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

static int run_until(int floor);
static ObjModule *aqs_import(ObjStr *name);

/* Run `script` to completion on the shared stack (used for the main program and
 * for each imported module); returns when its frame returns. */
static int run_module(ObjFn *script)
{
    int floor = frame_count;
    push(OBJ_VAL(script));
    if (call_fn(script, 0)) return 1;
    return run_until(floor);
}

/* Load (compile + run) a module by name, with caching; returns its namespace. */
static ObjModule *aqs_import(ObjStr *name)
{
    ObjModule *m = module_find(name);
    if (m) return m;                              /* cached (loaded, or mid-load == partial) */
    if (nmodules >= 64) { runtime_error("too many modules"); return NULL; }
    m = aqs_module_new(name->chars, name->len);
    modules[nmodules++] = m;                       /* register before running -> circular-safe */
    char *src = module_source(name);
    if (!src) { runtime_error("cannot import module '%.*s'", name->len, name->chars); return NULL; }
    ObjFn *script = aqs_compile_module(src, m);
    free(src);
    if (!script) return NULL;                      /* compiler set aqs_err */
    if (run_module(script)) return NULL;
    m->state = 1;
    return m;
}

static int run_until(int floor)
{
    Frame *frame = &frames[frame_count - 1];
#define READ_BYTE()  (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONST() (frame->fn->consts[READ_BYTE()])

    for (;;) {
        uint8_t op = READ_BYTE();
        switch (op) {
        case OP_CONST: push(READ_CONST()); break;
        case OP_NIL:   push(NIL_VAL); break;
        case OP_TRUE:  push(BOOL_VAL(1)); break;
        case OP_FALSE: push(BOOL_VAL(0)); break;
        case OP_POP:   pop(); break;

        case OP_GET_LOCAL: { uint8_t s = READ_BYTE(); push(frame->slots[s]); break; }
        case OP_SET_LOCAL: { uint8_t s = READ_BYTE(); frame->slots[s] = peek(0); break; }
        case OP_GET_GLOBAL: {
            ObjStr *n = AS_STR(READ_CONST());
            Value *s = resolve_global(frame->fn, n);
            if (!s) { runtime_error("undefined variable '%.*s'", n->len, n->chars); goto err; }
            push(*s); break;
        }
        case OP_DEF_GLOBAL: { ObjStr *n = AS_STR(READ_CONST()); *aqs_module_slot(frame->fn->module, n, 1) = peek(0); pop(); break; }
        case OP_SET_GLOBAL: {
            ObjStr *n = AS_STR(READ_CONST());
            Value *s = frame->fn->module ? aqs_module_slot(frame->fn->module, n, 0) : NULL;
            if (!s) s = builtin_slot(n, 0);
            if (!s) { runtime_error("undefined variable '%.*s'", n->len, n->chars); goto err; }
            *s = peek(0); break;
        }

        case OP_ADD: {
            Value b = peek(0), a = peek(1);
            if (IS_INT(a) && IS_INT(b))      { sp -= 2; push(INT_VAL(AS_INT(a) + AS_INT(b))); }
            else if (IS_NUM(a) && IS_NUM(b)) { sp -= 2; push(FLOAT_VAL(AS_NUM(a) + AS_NUM(b))); }
            else if (IS_STR(a) && IS_STR(b)) { ObjStr *s = str_concat(AS_STR(a), AS_STR(b)); sp -= 2; push(OBJ_VAL(s)); }
            else { runtime_error("operands of '+' must be numbers or strings"); goto err; }
            break;
        }
        case OP_SUB: {
            Value b = peek(0), a = peek(1);
            if (IS_INT(a) && IS_INT(b))      { sp -= 2; push(INT_VAL(AS_INT(a) - AS_INT(b))); }
            else if (IS_NUM(a) && IS_NUM(b)) { sp -= 2; push(FLOAT_VAL(AS_NUM(a) - AS_NUM(b))); }
            else { runtime_error("operands of '-' must be numbers"); goto err; }
            break;
        }
        case OP_MUL: {
            Value b = peek(0), a = peek(1);
            if (IS_INT(a) && IS_INT(b))      { sp -= 2; push(INT_VAL(AS_INT(a) * AS_INT(b))); }
            else if (IS_NUM(a) && IS_NUM(b)) { sp -= 2; push(FLOAT_VAL(AS_NUM(a) * AS_NUM(b))); }
            else { runtime_error("operands of '*' must be numbers"); goto err; }
            break;
        }
        case OP_DIV: {
            Value b = peek(0), a = peek(1);
            if (!IS_NUM(a) || !IS_NUM(b)) { runtime_error("operands of '/' must be numbers"); goto err; }
            if (IS_INT(a) && IS_INT(b)) {
                if (AS_INT(b) == 0) { runtime_error("integer division by zero"); goto err; }
                sp -= 2; push(INT_VAL(AS_INT(a) / AS_INT(b)));
            } else { sp -= 2; push(FLOAT_VAL(AS_NUM(a) / AS_NUM(b))); }
            break;
        }
        case OP_MOD: {
            Value b = peek(0), a = peek(1);
            if (!IS_INT(a) || !IS_INT(b)) { runtime_error("operands of '%%' must be integers"); goto err; }
            if (AS_INT(b) == 0) { runtime_error("modulo by zero"); goto err; }
            sp -= 2; push(INT_VAL(AS_INT(a) % AS_INT(b)));
            break;
        }
        case OP_NEG: {
            Value a = peek(0);
            if (IS_INT(a))        { sp--; push(INT_VAL(-AS_INT(a))); }
            else if (IS_FLOAT(a)) { sp--; push(FLOAT_VAL(-AS_FLOAT(a))); }
            else { runtime_error("operand of unary '-' must be a number"); goto err; }
            break;
        }
        case OP_NOT: { Value a = pop(); push(BOOL_VAL(!aqs_truthy(a))); break; }

        case OP_EQ: { Value b = pop(), a = pop();
            int eq = (IS_NUM(a) && IS_NUM(b)) ? (AS_NUM(a) == AS_NUM(b)) : aqs_value_eq(a, b);
            push(BOOL_VAL(eq)); break; }
        case OP_NE: { Value b = pop(), a = pop();
            int eq = (IS_NUM(a) && IS_NUM(b)) ? (AS_NUM(a) == AS_NUM(b)) : aqs_value_eq(a, b);
            push(BOOL_VAL(!eq)); break; }
        case OP_LT: { Value b = pop(), a = pop(); if (!IS_NUM(a) || !IS_NUM(b)) { runtime_error("'<' needs numbers"); goto err; } push(BOOL_VAL(AS_NUM(a) <  AS_NUM(b))); break; }
        case OP_LE: { Value b = pop(), a = pop(); if (!IS_NUM(a) || !IS_NUM(b)) { runtime_error("'<=' needs numbers"); goto err; } push(BOOL_VAL(AS_NUM(a) <= AS_NUM(b))); break; }
        case OP_GT: { Value b = pop(), a = pop(); if (!IS_NUM(a) || !IS_NUM(b)) { runtime_error("'>' needs numbers"); goto err; } push(BOOL_VAL(AS_NUM(a) >  AS_NUM(b))); break; }
        case OP_GE: { Value b = pop(), a = pop(); if (!IS_NUM(a) || !IS_NUM(b)) { runtime_error("'>=' needs numbers"); goto err; } push(BOOL_VAL(AS_NUM(a) >= AS_NUM(b))); break; }

        case OP_JUMP:          { uint16_t o = READ_SHORT(); frame->ip += o; break; }
        case OP_JUMP_IF_FALSE: { uint16_t o = READ_SHORT(); if (!aqs_truthy(peek(0))) frame->ip += o; break; }
        case OP_LOOP:          { uint16_t o = READ_SHORT(); frame->ip -= o; break; }

        case OP_CALL: {
            int argc = READ_BYTE();
            if (call_value(peek(argc), argc)) goto err;
            frame = &frames[frame_count - 1];   /* native: same frame; fn: the new one */
            break;
        }
        case OP_RET: {
            Value result = pop();
            close_upvalues(frame->slots);     /* close this fn's captured locals before unwinding */
            frame_count--;
            if (frame_count == floor) { pop(); return 0; }   /* this script/module is done */
            sp = frame->slots;                                /* discard callee + args + locals */
            push(result);
            frame = &frames[frame_count - 1];
            break;
        }

        case OP_MAKE_LIST: {
            int n = READ_BYTE();
            ObjList *l = aqs_list_new();
            for (int i = 0; i < n; i++) aqs_list_push(l, sp[-n + i]);
            sp -= n;
            push(OBJ_VAL(l));
            break;
        }
        case OP_MAKE_DICT: {
            int n = READ_BYTE();
            ObjDict *d = aqs_dict_new();
            for (int i = 0; i < n; i++) {
                Value key = sp[-2 * n + 2 * i];
                Value val = sp[-2 * n + 2 * i + 1];
                if (!aqs_dict_set(d, key, val)) { runtime_error("dict key must be a string or int"); goto err; }
            }
            sp -= 2 * n;
            push(OBJ_VAL(d));
            break;
        }
        case OP_INDEX_GET: {
            Value idx = pop(), obj = pop();
            if (IS_PTR(obj)) {
                if (!IS_INT(idx)) { runtime_error("pointer index must be an integer"); goto err; }
                ObjPtr *p = AS_PTR(obj);
                uint64_t raw = aqs_ll_peek(p->addr + (uint64_t)(AS_INT(idx) * p->width), p->width);
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
            } else if (IS_STR(obj)) {
                if (!IS_INT(idx)) { runtime_error("string index must be an integer"); goto err; }
                ObjStr *s = AS_STR(obj); int64_t i = AS_INT(idx); if (i < 0) i += s->len;
                if (i < 0 || i >= s->len) { runtime_error("string index out of range"); goto err; }
                push(OBJ_VAL(aqs_str_copy(s->chars + i, 1)));
            } else if (IS_DICT(obj)) {
                if (!IS_STR(idx) && !IS_INT(idx)) { runtime_error("dict key must be a string or int"); goto err; }
                Value out;
                if (!aqs_dict_get(AS_DICT(obj), idx, &out)) { runtime_error("key not found"); goto err; }
                push(out);
            } else { runtime_error("only lists, strings, and dicts can be indexed"); goto err; }
            break;
        }
        case OP_INDEX_SET: {
            Value val = pop(), idx = pop(), obj = pop();
            if (IS_PTR(obj)) {
                if (!IS_INT(idx) || !IS_INT(val)) { runtime_error("pointer index/value must be integers"); goto err; }
                ObjPtr *p = AS_PTR(obj);
                aqs_ll_poke(p->addr + (uint64_t)(AS_INT(idx) * p->width), p->width, (uint64_t)AS_INT(val));
                break;
            }
            if (IS_DICT(obj)) {
                if (!IS_STR(idx) && !IS_INT(idx)) { runtime_error("dict key must be a string or int"); goto err; }
                aqs_dict_set(AS_DICT(obj), idx, val);   /* key type checked above, so it can't fail */
                break;
            }
            if (!IS_LIST(obj)) { runtime_error("only lists support item assignment"); goto err; }
            if (!IS_INT(idx)) { runtime_error("list index must be an integer"); goto err; }
            ObjList *l = AS_LIST(obj); int64_t i = AS_INT(idx); if (i < 0) i += l->count;
            if (i < 0 || i >= l->count) { runtime_error("list assignment index out of range"); goto err; }
            l->items[i] = val;
            break;
        }
        case OP_LEN: {
            Value v = pop();
            if (IS_LIST(v))      push(INT_VAL(AS_LIST(v)->count));
            else if (IS_STR(v))  push(INT_VAL(AS_STR(v)->len));
            else if (IS_DICT(v)) push(INT_VAL(AS_DICT(v)->live));
            else { runtime_error("object has no length"); goto err; }
            break;
        }
        case OP_INVOKE: {
            ObjStr *name = AS_STR(READ_CONST());
            uint8_t argc = READ_BYTE();
            Value recv = peek(argc);
            if (IS_MODULE(recv)) {                          /* mod.fn(args) */
                Value *s = aqs_module_slot(AS_MODULE(recv), name, 0);
                if (!s) { runtime_error("module '%.*s' has no '%.*s'", AS_MODULE(recv)->name->len, AS_MODULE(recv)->name->chars, name->len, name->chars); goto err; }
                sp[-argc - 1] = *s;                         /* replace receiver with the callable */
                if (call_value(*s, argc)) goto err;
                frame = &frames[frame_count - 1];
            } else if (IS_LIST(recv)) {
                if (name_eq(name, "append")) {
                    if (argc != 1) { runtime_error("append() takes 1 argument"); goto err; }
                    aqs_list_push(AS_LIST(recv), peek(0));
                    sp -= argc + 1; push(NIL_VAL);
                } else { runtime_error("list has no method '%.*s'", name->len, name->chars); goto err; }
            } else if (IS_DICT(recv)) {
                ObjDict *d = AS_DICT(recv);
                if (name_eq(name, "has")) {
                    if (argc != 1) { runtime_error("has() takes 1 argument"); goto err; }
                    Value k = peek(0);
                    if (!IS_STR(k) && !IS_INT(k)) { runtime_error("dict key must be a string or int"); goto err; }
                    int h = aqs_dict_has(d, k);
                    sp -= argc + 1; push(BOOL_VAL(h));
                } else if (name_eq(name, "get")) {
                    if (argc != 1 && argc != 2) { runtime_error("get() takes 1 or 2 arguments"); goto err; }
                    Value k = peek(argc - 1);
                    if (!IS_STR(k) && !IS_INT(k)) { runtime_error("dict key must be a string or int"); goto err; }
                    Value out;
                    if (!aqs_dict_get(d, k, &out)) out = (argc == 2) ? peek(0) : NIL_VAL;
                    sp -= argc + 1; push(out);
                } else if (name_eq(name, "remove")) {
                    if (argc != 1) { runtime_error("remove() takes 1 argument"); goto err; }
                    Value k = peek(0);
                    if (!IS_STR(k) && !IS_INT(k)) { runtime_error("dict key must be a string or int"); goto err; }
                    int r = aqs_dict_remove(d, k);
                    sp -= argc + 1; push(BOOL_VAL(r));
                } else if (name_eq(name, "keys")) {
                    if (argc != 0) { runtime_error("keys() takes no arguments"); goto err; }
                    ObjList *l = aqs_dict_keys(d);
                    sp -= argc + 1; push(OBJ_VAL(l));
                } else if (name_eq(name, "values")) {
                    if (argc != 0) { runtime_error("values() takes no arguments"); goto err; }
                    ObjList *l = aqs_dict_values(d);
                    sp -= argc + 1; push(OBJ_VAL(l));
                } else { runtime_error("dict has no method '%.*s'", name->len, name->chars); goto err; }
            } else { runtime_error("'%.*s' is not a method of this type", name->len, name->chars); goto err; }
            break;
        }
        case OP_GET_ATTR: {
            ObjStr *n = AS_STR(READ_CONST());
            Value recv = pop();
            if (!IS_MODULE(recv)) { runtime_error("only modules have attributes"); goto err; }
            Value *s = aqs_module_slot(AS_MODULE(recv), n, 0);
            if (!s) { runtime_error("module '%.*s' has no '%.*s'", AS_MODULE(recv)->name->len, AS_MODULE(recv)->name->chars, n->len, n->chars); goto err; }
            push(*s);
            break;
        }
        case OP_IMPORT: {
            ObjStr *n = AS_STR(READ_CONST());
            ObjModule *m = aqs_import(n);
            if (!m) goto err;                  /* aqs_err set */
            frame = &frames[frame_count - 1];  /* importing may have run nested module frames */
            push(OBJ_VAL(m));
            break;
        }

        case OP_IN: {
            Value cont = pop(), item = pop();
            int found = 0;
            if (IS_DICT(cont)) {
                if (!IS_STR(item) && !IS_INT(item)) { runtime_error("dict key must be a string or int"); goto err; }
                found = aqs_dict_has(AS_DICT(cont), item);
            } else if (IS_LIST(cont)) {
                ObjList *l = AS_LIST(cont);
                for (int i = 0; i < l->count; i++) {
                    Value e = l->items[i];
                    int eq = (IS_NUM(item) && IS_NUM(e)) ? (AS_NUM(item) == AS_NUM(e)) : aqs_value_eq(item, e);
                    if (eq) { found = 1; break; }
                }
            } else if (IS_STR(cont)) {
                if (!IS_STR(item)) { runtime_error("substring test needs a string on the left of 'in'"); goto err; }
                ObjStr *hay = AS_STR(cont), *needle = AS_STR(item);
                if (needle->len == 0) found = 1;
                else for (int i = 0; i + needle->len <= hay->len; i++)
                    if (memcmp(hay->chars + i, needle->chars, (size_t)needle->len) == 0) { found = 1; break; }
            } else { runtime_error("'in' needs a dict, list, or string"); goto err; }
            push(BOOL_VAL(found));
            break;
        }

        case OP_ITER: {
            if (IS_DICT(peek(0))) { ObjList *ks = aqs_dict_keys(AS_DICT(peek(0))); sp[-1] = OBJ_VAL(ks); }
            break;   /* list/range/string: iterate as-is */
        }

        case OP_CLOSURE: {
            ObjFn *fn = AS_FN(READ_CONST());
            ObjClosure *cl = aqs_closure_new(fn);
            for (int i = 0; i < cl->upvalue_count; i++) {
                uint8_t is_local = READ_BYTE();
                uint8_t index = READ_BYTE();
                /* non-local: inherit from the enclosing closure. frame->closure is
                 * non-NULL here because any fn with upvalue_count>0 is nested in a
                 * function, which is always invoked via call_closure (never call_fn). */
                cl->upvalues[i] = is_local ? capture_upvalue(frame->slots + index)
                                           : frame->closure->upvalues[index];
            }
            push(OBJ_VAL(cl));
            break;
        }
        case OP_GET_UPVALUE: { uint8_t s = READ_BYTE(); push(*frame->closure->upvalues[s]->location); break; }
        case OP_SET_UPVALUE: { uint8_t s = READ_BYTE(); *frame->closure->upvalues[s]->location = peek(0); break; }
        case OP_CLOSE_UPVALUE: { close_upvalues(sp - 1); pop(); break; }
        default: runtime_error("bad opcode %d", op); goto err;
        }
    }
err:
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONST
    return 1;
}

int aqs_run(ObjFn *script)
{
    reset_stack();
    nbuiltins = 0; nmodules = 0;        /* fresh builtins + module cache (objects freed between runs) */
    aqs_define_native("print", native_print);
    aqs_define_native("len", native_len);
    aqs_define_native("range", native_range);
    aqs_define_native("gc_stats", native_gc_stats);
    aqs_install_indirection();          /* addr/peek/poke/iNptr/syscall + SYS_* */
    if (script->module) { modules[nmodules++] = script->module; script->module->state = 1; }
    return run_module(script);
}

int aqs_interpret(const char *src)
{
    ObjFn *fn = aqs_compile(src);
    if (!fn) return 1;            /* aqs_err set by the compiler/lexer */
    return aqs_run(fn);
}
