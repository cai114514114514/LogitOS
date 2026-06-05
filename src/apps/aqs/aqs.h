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
typedef enum { O_STR, O_FN, O_NATIVE, O_LIST, O_PTR } ObjType;
struct Obj { ObjType type; Obj *next; };   /* next: allocation list (for cleanup) */

typedef struct { Obj obj; int len; uint32_t hash; char *chars; } ObjStr;

typedef struct {           /* a compiled function (also the top-level "script") */
    Obj obj;
    int arity;
    uint8_t *code; int count, cap;        /* bytecode */
    Value *consts; int kcount, kcap;      /* constant pool */
    ObjStr *name;
} ObjFn;

typedef Value (*NativeFn)(int argc, Value *args);
typedef struct { Obj obj; NativeFn fn; const char *name; } ObjNative;

#define IS_STR(v)    (IS_OBJ(v) && AS_OBJ(v)->type == O_STR)
#define IS_FN(v)     (IS_OBJ(v) && AS_OBJ(v)->type == O_FN)
#define IS_NATIVE(v) (IS_OBJ(v) && AS_OBJ(v)->type == O_NATIVE)
#define IS_LIST(v)   (IS_OBJ(v) && AS_OBJ(v)->type == O_LIST)
#define AS_STR(v)    ((ObjStr *)AS_OBJ(v))
#define AS_FN(v)     ((ObjFn *)AS_OBJ(v))

/* --- opcodes --- */
typedef enum {
    OP_CONST, OP_NIL, OP_TRUE, OP_FALSE, OP_POP,
    OP_GET_LOCAL, OP_SET_LOCAL, OP_GET_GLOBAL, OP_SET_GLOBAL, OP_DEF_GLOBAL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_NOT,
    OP_JUMP, OP_JUMP_IF_FALSE, OP_LOOP,
    OP_CALL, OP_RET,
    OP_MAKE_LIST, OP_INDEX_GET, OP_INDEX_SET,   /* A2 */
} OpCode;

/* --- compile + run --- */
ObjFn *aqs_compile(const char *src);   /* source -> top-level function, or NULL on error */
int    aqs_run(ObjFn *script);         /* execute; 0 ok, 1 runtime error */
int    aqs_interpret(const char *src); /* compile + run; returns 0 ok */

/* object/value helpers (object.c, value.c) */
ObjStr   *aqs_str_copy(const char *chars, int len);
ObjStr   *aqs_str_take(char *chars, int len);
ObjFn    *aqs_fn_new(void);
ObjNative*aqs_native_new(NativeFn fn, const char *name);
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

/* error reporting (set by compiler/vm; aqs_main prints) */
extern char aqs_err[256];

#endif /* AQUASCRIPT_H */
