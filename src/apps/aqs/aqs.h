#ifndef AQUASCRIPT_H
#define AQUASCRIPT_H

/* AquaScript: a small dynamically-typed language with a stack bytecode VM.
 * Python-ish (indentation) syntax; keeps speed (bytecode + computed-goto VM) and
 * indirection (raw memory + syscalls + typed pointers, added in A3). The core
 * (lexer/compiler/vm/object) is portable C so it unit-tests on the host; the
 * platform layer (output, syscalls) lives in aqs_main / aqs_ll.asm. */

#include <stdint.h>
#include <stddef.h>
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
typedef enum { O_STR, O_FN, O_NATIVE, O_LIST, O_PTR, O_MODULE, O_DICT } ObjType;
struct Obj { ObjType type; Obj *next; };   /* next: allocation list (for cleanup) */

typedef struct { Obj obj; int len; uint32_t hash; char *chars; } ObjStr;

typedef struct {           /* a compiled function (also the top-level "script") */
    Obj obj;
    int arity;
    uint8_t *code; int count, cap;        /* bytecode */
    Value *consts; int kcount, kcap;      /* constant pool */
    ObjStr *name;
    struct ObjModule *module;             /* the module this fn's globals resolve in */
} ObjFn;

typedef Value (*NativeFn)(int argc, Value *args);
typedef struct { Obj obj; NativeFn fn; const char *name; } ObjNative;

typedef struct { Obj obj; Value *items; int count, cap; } ObjList;   /* A2: dynamic array */

/* M21 dict: open-addressing hash table; keys are strings or 64-bit ints.
 * `kind` tags each slot; iteration/print are in hash order, not insertion order. */
enum { AQS_DK_EMPTY = 0, AQS_DK_STR = 1, AQS_DK_INT = 2, AQS_DK_TOMB = 3 };
typedef struct { uint8_t kind; ObjStr *kstr; int64_t kint; Value val; } DictEntry;
typedef struct { Obj obj; DictEntry *entries; int live, used, cap; } ObjDict;

/* A3 indirection: a typed pointer into raw memory. p[i] reads/writes `width`
 * bytes at addr + i*width (sign-extended on read when is_signed). */
typedef struct { Obj obj; uint64_t addr; int width; int is_signed; } ObjPtr;

/* A module: a name -> value namespace populated by running a .aqs file's top
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
} OpCode;

/* --- compile + run --- */
ObjFn *aqs_compile(const char *src);                       /* compile into a throwaway module */
ObjFn *aqs_compile_module(const char *src, ObjModule *m);  /* compile, stamping fns with module m */
int    aqs_run(ObjFn *script);         /* execute; 0 ok, 1 runtime error */
int    aqs_interpret(const char *src); /* compile + run; returns 0 ok */

/* modules (vm.c): the loader/cache + namespace access used by import. */
ObjModule *aqs_module_new(const char *name, int len);
Value     *aqs_module_slot(ObjModule *m, ObjStr *name, int create);  /* find/insert; NULL if absent */
void       aqs_add_module_source(const char *name, const char *src); /* in-memory module (tests) */

/* object/value helpers (object.c, value.c) */
ObjStr   *aqs_str_copy(const char *chars, int len);
ObjStr   *aqs_str_take(char *chars, int len);
ObjFn    *aqs_fn_new(void);
ObjNative*aqs_native_new(NativeFn fn, const char *name);
ObjList  *aqs_list_new(void);
void      aqs_list_push(ObjList *l, Value v);
ObjDict  *aqs_dict_new(void);
int       aqs_dict_set(ObjDict *d, Value key, Value val);   /* 1 ok; 0 = key not str/int */
int       aqs_dict_get(ObjDict *d, Value key, Value *out);  /* 1 found (out set), 0 missing */
int       aqs_dict_has(ObjDict *d, Value key);
int       aqs_dict_remove(ObjDict *d, Value key);           /* 1 removed, 0 absent */
ObjList  *aqs_dict_keys(ObjDict *d);
ObjList  *aqs_dict_values(ObjDict *d);
ObjPtr   *aqs_ptr_new(uint64_t addr, int width, int is_signed);
void      aqs_chunk_write(ObjFn *fn, uint8_t b);
int       aqs_chunk_const(ObjFn *fn, Value v);
int       aqs_value_eq(Value a, Value b);
int       aqs_truthy(Value v);
void      aqs_print_value(Value v);    /* for `print` + REPL echo */
void      aqs_free_objects(void);      /* free all heap objects (end of run) */

/* platform output (aqs_io.c): write raw bytes to stdout (fd 1). */
void      aqs_emit(const char *s, int n);
void      aqs_emit_cstr(const char *s);
void      aqs_capture(char *buf, int cap);   /* redirect aqs_emit to a buffer (tests); NULL = stdout */

/* native registration (vm.c) + A3 indirection builtins (aqs_native.c) */
void      aqs_define_native(const char *name, NativeFn fn);
void      aqs_define_int(const char *name, int64_t v);
Value     aqs_native_fail(const char *msg);  /* a native aborts the run with this message */
void      aqs_install_indirection(void);     /* registers peek/poke/addr/iNptr/syscall + SYS_* */

/* low-level bridge (aqs_ll.c): raw memory + the int 0x80 syscall (asm on Aqua). */
uint64_t  aqs_ll_peek(uint64_t addr, int width);
void      aqs_ll_poke(uint64_t addr, int width, uint64_t val);
long      aqs_ll_syscall(long n, long a, long b, long c);

/* error reporting (set by compiler/vm; aqs_main prints) */
extern char aqs_err[256];

#endif /* AQUASCRIPT_H */
