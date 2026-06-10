#ifndef AETHERSCRIPT_H
#define AETHERSCRIPT_H

/* AetherScript: a small dynamically-typed language with a stack bytecode VM.
 * Python-ish (indentation) syntax; keeps speed (bytecode + computed-goto VM) and
 * indirection (raw memory + syscalls + typed pointers, added in A3). The core
 * (lexer/compiler/vm/object) is portable C so it unit-tests on the host; the
 * platform layer (output, syscalls) lives in as_main / as_ll.asm. */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>      /* FILE (as_dump) */
#include <stdlib.h>     /* malloc/free/realloc/strtoll/strtod (host libc or mini-libc) */
#include <string.h>     /* memcpy/memset/strlen/strcmp */

/* --- values --- */
typedef enum { V_NIL, V_BOOL, V_INT, V_FLOAT, V_OBJ } VType;
typedef struct Obj Obj;

typedef struct {
    VType type;
    union { int64_t i; double f; Obj *obj; } as;
} Value;

#define NIL_VAL        ((Value){ V_NIL,   { .i = 0 } })
#define BOOL_VAL(b)    ((Value){ V_BOOL,  { .i = (b) } })
#define INT_VAL(n)     ((Value){ V_INT,   { .i = (n) } })
#define FLOAT_VAL(x)   ((Value){ V_FLOAT, { .f = (x) } })
#define OBJ_VAL(o)     ((Value){ V_OBJ,   { .obj = (Obj *)(o) } })

#define IS_NIL(v)   ((v).type == V_NIL)
#define IS_BOOL(v)  ((v).type == V_BOOL)
#define IS_INT(v)   ((v).type == V_INT)
#define IS_FLOAT(v) ((v).type == V_FLOAT)
#define IS_OBJ(v)   ((v).type == V_OBJ)
#define AS_BOOL(v)  ((v).as.i != 0)
#define AS_INT(v)   ((v).as.i)
#define AS_FLOAT(v) ((v).as.f)
#define AS_OBJ(v)   ((v).as.obj)

/* number-as-double for mixed int/float arithmetic */
#define AS_NUM(v)   (IS_FLOAT(v) ? AS_FLOAT(v) : (double)AS_INT(v))

/* --- objects --- */
typedef enum { O_STR, O_FN, O_NATIVE, O_LIST, O_PTR, O_MODULE, O_DICT, O_CLOSURE, O_UPVALUE,
               O_CLASS, O_INSTANCE, O_BOUND_METHOD } ObjType;
struct Obj { ObjType type; uint8_t marked; Obj *next; };   /* next: alloc list; marked: GC */

/* M23 lazy hash: `hash` is computed on first use (as_str_hash), not at creation.
 * Eager hashing made every str_concat O(result) twice -- read ->hash only through
 * as_str_hash(). */
typedef struct { Obj obj; int len; uint8_t hashed; uint32_t hash; char *chars; } ObjStr;

typedef struct {           /* a compiled function (also the top-level "script") */
    Obj obj;
    int arity;
    uint8_t *code; int count, cap;        /* bytecode */
    Value *consts; int kcount, kcap;      /* constant pool */
    ObjStr *name;
    struct ObjModule *module;             /* the module this fn's globals resolve in */
    int upvalue_count;                    /* how many enclosing vars this fn captures */
} ObjFn;

/* M22 closures: an upvalue points at a captured variable — a live stack slot
 * while "open", or its own `closed` copy once the slot leaves the stack. */
typedef struct ObjUpvalue {
    Obj obj;
    Value *location;
    Value closed;
    struct ObjUpvalue *next;   /* VM open-upvalue list, sorted by stack addr descending */
} ObjUpvalue;
typedef struct {               /* a function paired with its captured upvalues */
    Obj obj;
    ObjFn *fn;
    ObjUpvalue **upvalues;
    int upvalue_count;
} ObjClosure;

typedef Value (*NativeFn)(int argc, Value *args);
typedef struct { Obj obj; NativeFn fn; const char *name; } ObjNative;

typedef struct { Obj obj; Value *items; int count, cap; } ObjList;   /* A2: dynamic array */

/* M21 dict: open-addressing hash table; keys are strings or 64-bit ints.
 * `kind` tags each slot; iteration/print are in hash order, not insertion order. */
enum { AS_DK_EMPTY = 0, AS_DK_STR = 1, AS_DK_INT = 2, AS_DK_TOMB = 3 };
typedef struct { uint8_t kind; ObjStr *kstr; int64_t kint; Value val; } DictEntry;
typedef struct { Obj obj; DictEntry *entries; int live, used, cap; } ObjDict;

/* M22.3 classes. A class's method table is an ObjDict (name(str) -> ObjClosure);
 * an instance's fields are an ObjDict (name(str) -> Value) -- reusing ObjDict keeps
 * GC tracing DRY. `super` is kept only for super.method; method LOOKUP is copy-down
 * (OP_INHERIT copies the parent's methods into the child), so normal lookup is one
 * dict. Methods are ordinary closures taking an explicit `self` first parameter. */
typedef struct ObjClass {
    Obj obj;
    ObjStr *name;
    struct ObjClass *super;     /* parent class, or NULL */
    ObjDict *methods;           /* name(str) -> ObjClosure */
} ObjClass;
typedef struct {
    Obj obj;
    ObjClass *klass;
    ObjDict *fields;            /* name(str) -> Value */
} ObjInstance;
typedef struct {
    Obj obj;
    Value receiver;             /* the bound self (an instance) */
    ObjClosure *method;
} ObjBoundMethod;

/* A3 indirection: a typed pointer into raw memory. p[i] reads/writes `width`
 * bytes at addr + i*width (sign-extended on read when is_signed). */
typedef struct { Obj obj; uint64_t addr; int width; int is_signed; } ObjPtr;

/* A module: a name -> value namespace populated by running a .as file's top
 * level. `mod.x` reads `vars`; functions defined in it resolve globals here. */
typedef struct { ObjStr *name; Value val; } NameVal;
typedef struct ObjModule {
    Obj obj; ObjStr *name; NameVal *vars; int count, cap;
    int state;             /* 0 = loading, 1 = loaded (guards circular imports) */
} ObjModule;

#define IS_STR(v)    (IS_OBJ(v) && AS_OBJ(v)->type == O_STR)
#define IS_FN(v)     (IS_OBJ(v) && AS_OBJ(v)->type == O_FN)
#define IS_NATIVE(v) (IS_OBJ(v) && AS_OBJ(v)->type == O_NATIVE)
#define IS_LIST(v)   (IS_OBJ(v) && AS_OBJ(v)->type == O_LIST)
#define IS_PTR(v)    (IS_OBJ(v) && AS_OBJ(v)->type == O_PTR)
#define IS_MODULE(v) (IS_OBJ(v) && AS_OBJ(v)->type == O_MODULE)
#define AS_STR(v)    ((ObjStr *)AS_OBJ(v))
#define AS_FN(v)     ((ObjFn *)AS_OBJ(v))
#define AS_LIST(v)   ((ObjList *)AS_OBJ(v))
#define AS_PTR(v)    ((ObjPtr *)AS_OBJ(v))
#define AS_MODULE(v) ((ObjModule *)AS_OBJ(v))
#define IS_DICT(v)   (IS_OBJ(v) && AS_OBJ(v)->type == O_DICT)
#define AS_DICT(v)   ((ObjDict *)AS_OBJ(v))
#define IS_CLOSURE(v) (IS_OBJ(v) && AS_OBJ(v)->type == O_CLOSURE)
#define AS_CLOSURE(v) ((ObjClosure *)AS_OBJ(v))
#define IS_CLASS(v)        (IS_OBJ(v) && AS_OBJ(v)->type == O_CLASS)
#define AS_CLASS(v)        ((ObjClass *)AS_OBJ(v))
#define IS_INSTANCE(v)     (IS_OBJ(v) && AS_OBJ(v)->type == O_INSTANCE)
#define AS_INSTANCE(v)     ((ObjInstance *)AS_OBJ(v))
#define IS_BOUND_METHOD(v) (IS_OBJ(v) && AS_OBJ(v)->type == O_BOUND_METHOD)
#define AS_BOUND_METHOD(v) ((ObjBoundMethod *)AS_OBJ(v))

/* --- opcodes --- */
typedef enum {
    OP_CONST, OP_NIL, OP_TRUE, OP_FALSE, OP_POP,
    OP_GET_LOCAL, OP_SET_LOCAL, OP_GET_GLOBAL, OP_SET_GLOBAL, OP_DEF_GLOBAL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_NOT,
    OP_JUMP, OP_JUMP_IF_FALSE, OP_LOOP,
    OP_CALL, OP_RET,
    OP_MAKE_LIST, OP_INDEX_GET, OP_INDEX_SET, OP_LEN, OP_INVOKE,   /* A2 */
    OP_GET_ATTR, OP_IMPORT,                                        /* modules */
    OP_MAKE_DICT, OP_IN, OP_ITER,                                  /* M21 dict */
    OP_CLOSURE, OP_GET_UPVALUE, OP_SET_UPVALUE, OP_CLOSE_UPVALUE,  /* M22 closures */
    OP_CLASS, OP_INHERIT, OP_METHOD,                               /* M22.3 classes */
    OP_GET_PROPERTY, OP_SET_PROPERTY, OP_GET_SUPER,
    OP_SETUP_TRY, OP_POP_TRY, OP_RAISE,                         /* M22.4 exceptions */
    OP_BAND, OP_BOR, OP_BXOR, OP_BNOT, OP_SHL, OP_SHR, OP_POW,  /* M23: bitwise / shift / power */
} OpCode;

/* .la compiled-bytecode format version. Bump on ANY opcode add/reorder or any
 * change to the .la byte layout -- as_load rejects a mismatching version. */
#define AS_BC_VERSION 3u

/* --- compile + run --- */
ObjFn *as_compile(const char *src);                       /* compile into a throwaway module */
ObjFn *as_compile_module(const char *src, ObjModule *m);  /* compile, stamping fns with module m */
int    as_run(ObjFn *script);         /* execute; 0 ok, 1 runtime error */
int    as_interpret(const char *src); /* compile + run; returns 0 ok */

/* .la serialize/deserialize (as_bc.c). as_dump writes the LAQ1 header + the
 * recursive ObjFn tree to `out` (0 ok, nonzero on a write/unserializable error);
 * as_load deserializes a buffer into a runnable ObjFn (->module left NULL for the
 * caller to stamp), returning NULL on bad magic/version/EOF. */
int    as_dump(ObjFn *fn, FILE *out);
ObjFn *as_load(const uint8_t *buf, int len);

/* modules (vm.c): the loader/cache + namespace access used by import. */
ObjModule *as_module_new(const char *name, int len);
Value     *as_module_slot(ObjModule *m, ObjStr *name, int create);  /* find/insert; NULL if absent */
void       as_add_module_source(const char *name, const char *src); /* in-memory module (tests) */

/* object/value helpers (object.c, value.c) */
ObjStr   *as_str_copy(const char *chars, int len);
ObjStr   *as_str_take(char *chars, int len);
uint32_t  as_str_hash(ObjStr *s);     /* lazy: computes + caches on first call */
ObjFn    *as_fn_new(void);
ObjClosure *as_closure_new(ObjFn *fn);
ObjUpvalue *as_upvalue_new(Value *slot);
ObjClass       *as_class_new(ObjStr *name);
ObjInstance    *as_instance_new(ObjClass *klass);
ObjBoundMethod *as_bound_method_new(Value receiver, ObjClosure *method);
ObjNative*as_native_new(NativeFn fn, const char *name);
ObjList  *as_list_new(void);
void      as_list_push(ObjList *l, Value v);
ObjDict  *as_dict_new(void);
int       as_dict_set(ObjDict *d, Value key, Value val);   /* 1 ok; 0 = key not str/int */
int       as_dict_get(ObjDict *d, Value key, Value *out);  /* 1 found (out set), 0 missing */
int       as_dict_has(ObjDict *d, Value key);
int       as_dict_remove(ObjDict *d, Value key);           /* 1 removed, 0 absent */
ObjList  *as_dict_keys(ObjDict *d);
ObjList  *as_dict_values(ObjDict *d);
ObjPtr   *as_ptr_new(uint64_t addr, int width, int is_signed);
void      as_chunk_write(ObjFn *fn, uint8_t b);
int       as_chunk_const(ObjFn *fn, Value v);
int       as_value_eq(Value a, Value b);
int       as_truthy(Value v);
void      as_print_value(Value v);    /* for `print` + REPL echo */
int       value_to_cstr(Value v, char *buf, int cap);  /* print-form into buf; returns length */
int       as_fmt_float(double d, char *buf, int cap);  /* shortest round-trip; ".0" for whole floats */
void      as_free_objects(void);      /* free all heap objects (end of run) */

/* mark-sweep GC (object.c + vm.c) */
void gc_collect(void);
void gc_mark_obj(Obj *o);          /* mark + push to the gray worklist */
void gc_mark_value(Value v);
void as_vm_mark_roots(void);      /* mark the VM roots (implemented in vm.c) */
long as_gc_live(void);            /* current live object count */
void as_gc_push_disable(void);    /* disable GC (re-entrant counter) -- use around compile/setup */
void as_gc_pop_disable(void);

/* platform output (as_io.c): write raw bytes to stdout (fd 1). */
void      as_emit(const char *s, int n);
void      as_emit_cstr(const char *s);
void      as_capture(char *buf, int cap);   /* redirect as_emit to a buffer (tests); NULL = stdout */

/* native registration (vm.c) + A3 indirection builtins (as_native.c) */
void      as_define_native(const char *name, NativeFn fn);
void      as_define_int(const char *name, int64_t v);
Value     as_native_fail(const char *msg);  /* a native aborts the run with this message */
void      as_install_indirection(void);     /* registers peek/poke/addr/iNptr/syscall + SYS_* */

/* low-level bridge (as_ll.c): raw memory + the int 0x80 syscall (asm on Aether). */
uint64_t  as_ll_peek(uint64_t addr, int width);
void      as_ll_poke(uint64_t addr, int width, uint64_t val);
long      as_ll_syscall(long n, long a, long b, long c);

/* error reporting (set by compiler/vm; as_main prints) */
extern char as_err[256];

/* OOM plumbing (object.c): on a NULL malloc/realloc the wrappers set as_err to
 * "out of memory" + raise g_oom; the dispatch loop polls g_oom (like
 * g_stack_overflow) and unwinds to a catchable runtime error. compile-/lex-time
 * sites poll g_oom directly. Exposed (non-static) so str_concat/module_source
 * (vm.c), string() (compiler.c) and push() (lexer.c) can use them too. */
extern int g_oom;
void *as_malloc(size_t n);
void *as_realloc(void *p, size_t n);

#endif /* AETHERSCRIPT_H */
