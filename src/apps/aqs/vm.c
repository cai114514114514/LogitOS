#include "aqs.h"
#include <stdio.h>      /* vsnprintf */
#include <stdarg.h>

/* Stack bytecode VM. A1 uses a switch dispatch (correctness first); the design
 * earmarks computed-goto as a later speed pass. Locals live on the value stack
 * addressed off each call frame's base; globals are a small name->value table. */

#define STACK_MAX  (4096)
#define FRAMES_MAX (256)

typedef struct { ObjFn *fn; uint8_t *ip; Value *slots; } Frame;

static Value  stack[STACK_MAX];
static Value *sp;
static Frame  frames[FRAMES_MAX];
static int    frame_count;

typedef struct { ObjStr *name; Value val; } Global;
static Global globals[512];
static int    nglobals;

static void push(Value v) { *sp++ = v; }
static Value pop(void)    { return *--sp; }
static Value peek(int d)  { return sp[-1 - d]; }
static void reset_stack(void) { sp = stack; frame_count = 0; }

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

static int find_global(ObjStr *name)
{
    for (int i = 0; i < nglobals; i++)
        if (globals[i].name->hash == name->hash && globals[i].name->len == name->len
            && memcmp(globals[i].name->chars, name->chars, name->len) == 0) return i;
    return -1;
}
static void set_global(ObjStr *name, Value v)
{
    int i = find_global(name);
    if (i >= 0) { globals[i].val = v; return; }
    if (nglobals < 512) { globals[nglobals].name = name; globals[nglobals].val = v; nglobals++; }
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
    return aqs_native_fail("len() needs a list or string");
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
    set_global(aqs_str_copy(name, len), OBJ_VAL(aqs_native_new(fn, name)));
}
void aqs_define_int(const char *name, int64_t v)
{
    set_global(aqs_str_copy(name, (int)strlen(name)), INT_VAL(v));
}

/* ---- calls ---- */
static int call_fn(ObjFn *fn, int argc)
{
    if (argc != fn->arity) return runtime_error("%s expected %d argument(s) but got %d",
                                                 fn->name ? fn->name->chars : "script", fn->arity, argc);
    if (frame_count == FRAMES_MAX) return runtime_error("call depth exceeded");
    Frame *f = &frames[frame_count++];
    f->fn = fn; f->ip = fn->code; f->slots = sp - argc - 1;   /* slot 0 = the callee */
    return 0;
}
static int call_value(Value callee, int argc)   /* 0 ok, 1 error */
{
    if (IS_OBJ(callee)) {
        switch (AS_OBJ(callee)->type) {
        case O_FN: return call_fn((ObjFn *)AS_OBJ(callee), argc);
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

static int run(void)
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
            int i = find_global(n);
            if (i < 0) { runtime_error("undefined variable '%.*s'", n->len, n->chars); goto err; }
            push(globals[i].val); break;
        }
        case OP_DEF_GLOBAL: { ObjStr *n = AS_STR(READ_CONST()); set_global(n, peek(0)); pop(); break; }
        case OP_SET_GLOBAL: {
            ObjStr *n = AS_STR(READ_CONST());
            int i = find_global(n);
            if (i < 0) { runtime_error("undefined variable '%.*s'", n->len, n->chars); goto err; }
            globals[i].val = peek(0); break;
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
            frame_count--;
            if (frame_count == 0) { pop(); return 0; }   /* finished the script */
            sp = frame->slots;                            /* discard callee + args + locals */
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
            } else { runtime_error("only lists and strings can be indexed"); goto err; }
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
            if (!IS_LIST(obj)) { runtime_error("only lists support item assignment"); goto err; }
            if (!IS_INT(idx)) { runtime_error("list index must be an integer"); goto err; }
            ObjList *l = AS_LIST(obj); int64_t i = AS_INT(idx); if (i < 0) i += l->count;
            if (i < 0 || i >= l->count) { runtime_error("list assignment index out of range"); goto err; }
            l->items[i] = val;
            break;
        }
        case OP_LEN: {
            Value v = pop();
            if (IS_LIST(v))     push(INT_VAL(AS_LIST(v)->count));
            else if (IS_STR(v)) push(INT_VAL(AS_STR(v)->len));
            else { runtime_error("object has no length"); goto err; }
            break;
        }
        case OP_INVOKE: {
            ObjStr *name = AS_STR(READ_CONST());
            uint8_t argc = READ_BYTE();
            Value recv = peek(argc);
            if (IS_LIST(recv)) {
                if (name_eq(name, "append")) {
                    if (argc != 1) { runtime_error("append() takes 1 argument"); goto err; }
                    aqs_list_push(AS_LIST(recv), peek(0));
                    sp -= argc + 1; push(NIL_VAL);
                } else { runtime_error("list has no method '%.*s'", name->len, name->chars); goto err; }
            } else { runtime_error("'%.*s' is not a method of this type", name->len, name->chars); goto err; }
            break;
        }

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
    nglobals = 0;
    aqs_define_native("print", native_print);
    aqs_define_native("len", native_len);
    aqs_define_native("range", native_range);
    aqs_install_indirection();          /* addr/peek/poke/iNptr/syscall + SYS_* */
    push(OBJ_VAL(script));
    if (call_fn(script, 0)) return 1;
    return run();
}

int aqs_interpret(const char *src)
{
    ObjFn *fn = aqs_compile(src);
    if (!fn) return 1;            /* aqs_err set by the compiler/lexer */
    return aqs_run(fn);
}
