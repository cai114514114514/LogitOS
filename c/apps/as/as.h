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

/* --- values ---
 * A Value is a 32-bit tag plus a 64-bit payload. The tag's low byte is the
 * VType; for heap values its high byte is the ObjType, so IS_STR / IS_LIST /
 * ... are a single register compare rather than a load of the object header.
 * Type tests are the most frequent operation in the VM and they used to be a
 * pointer chase each.
 *
 * Deliberately 16 bytes, not NaN-boxed: NaN-boxing hides the tag in the unused
 * payloads of a double, but an AetherScript int is a full int64 (f64bits(2.0)
 * == 2^62 is asserted in-tree), so there are no spare bits in 8. The cost worth
 * removing was never the 8 bytes of footprint, it was the memory round-trip. */
typedef enum { V_NIL, V_BOOL, V_INT, V_FLOAT, V_OBJ } VType;
typedef struct Obj Obj;

typedef struct {
    uint32_t tag;                                  /* VType | ObjType << 8 */
    union { int64_t i; double f; Obj *obj; } as;
} Value;

#define OBJ_TAG(ot) ((uint32_t)V_OBJ | ((uint32_t)(ot) << 8))
#define VTYPE(v)    ((VType)((v).tag & 0xFFu))
#define OBJ_TYPE(v) ((ObjType)((v).tag >> 8))      /* meaningful only when IS_OBJ(v) */

#define NIL_VAL        ((Value){ V_NIL,   { .i = 0 } })
#define BOOL_VAL(b)    ((Value){ V_BOOL,  { .i = (b) } })
#define INT_VAL(n)     ((Value){ V_INT,   { .i = (n) } })
#define FLOAT_VAL(x)   ((Value){ V_FLOAT, { .f = (x) } })
/* OBJ_VAL is defined below, once struct Obj is complete: it reads o->type. */

#define IS_NIL(v)   ((v).tag == V_NIL)
#define IS_BOOL(v)  ((v).tag == V_BOOL)
#define IS_INT(v)   ((v).tag == V_INT)
#define IS_FLOAT(v) ((v).tag == V_FLOAT)
#define IS_OBJ(v)   (VTYPE(v) == V_OBJ)
#define AS_BOOL(v)  ((v).as.i != 0)
#define AS_INT(v)   ((v).as.i)
#define AS_FLOAT(v) ((v).as.f)
#define AS_OBJ(v)   ((v).as.obj)

/* number-as-double for mixed int/float arithmetic */
#define AS_NUM(v)   (IS_FLOAT(v) ? AS_FLOAT(v) : (double)AS_INT(v))

/* --- objects --- */
typedef enum { O_STR, O_FN, O_NATIVE, O_LIST, O_PTR, O_MODULE, O_DICT, O_CLOSURE, O_UPVALUE,
               O_CLASS, O_INSTANCE, O_BOUND_METHOD, O_RANGE, O_SHAPE, O_BUF,
               O__COUNT } ObjType;   /* sentinel: sizes the descriptor table in object.c */
/* Two bytes, where an intrusive allocation list cost sixteen. The `next` pointer
 * moved out into a contiguous registry (object.c): sweeping a dense array beats
 * chasing a linked list through the whole heap, and the eight bytes it cost sat
 * in every object, cold, purely so the GC could find it. `type` is a uint8_t for
 * the same reason -- as an enum it was an int, and the padding around it was
 * bigger than the field. */
struct Obj { uint8_t type; uint8_t marked; };

/* Wrapping an object copies its type into the tag -- valid because an object's
 * type is fixed at allocation. NULL yields nil rather than an object value with
 * a NULL payload: allocation failures reach here (`push(OBJ_VAL(as_str_copy(..)))`
 * with g_oom already set), and nil is inert where the old NULL-carrying object
 * value made the very next IS_STR() a NULL dereference. */
static inline Value as_obj_val(Obj *o)
{
    Value v;
    v.tag = o ? OBJ_TAG(o->type) : (uint32_t)V_NIL;
    v.as.obj = o;
    return v;
}
#define OBJ_VAL(o) as_obj_val((Obj *)(o))

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
    /* Global-lookup cache, parallel to consts (runtime only, never serialized).
     * A global read used to scan the module's name list and then the builtin
     * list, comparing hash + bytes -- linear in how late the name was defined,
     * and measurably so: 10M reads of a name at index 90 of 100 cost 5x the same
     * reads of a name at index 0. Each entry memoises where the name resolved:
     * 0 = not cached, +i+1 = module->vars[i], -i-1 = builtins[i]. Indices, not
     * pointers, so the module's var array stays free to grow by realloc.
     * as_globals_gen invalidates the whole cache whenever a NEW global name
     * appears anywhere, which is the only event that can change where an
     * already-resolved name points (a module-level `def range(xs)` taking over
     * from the builtin of that name). */
    int32_t *gcache; int gcache_n; uint32_t gcache_gen;
    /* Property inline cache, also parallel to consts. A property access on an
     * instance remembers which shape it saw and which slot the name was in;
     * repeating it is then an integer compare and an array index instead of a
     * search. Keyed by constant index, so two sites naming the same property in
     * one function share an entry -- fine while code is monomorphic, which is
     * the case the cache exists for. Runtime only, never serialized. */
    struct PCache *pcache; int pcache_n;
} ObjFn;
/* shape_id 0 == empty. `kind` is SK_VALUE for an ordinary object (a == the slot
 * index) and a machine kind for a layout (a == the byte offset, width from the
 * shape). One cache serves both: shape ids are globally unique and never reused,
 * so an entry armed by an instance can never be hit by a buffer. */
struct PCache { uint32_t shape_id; int32_t a; uint16_t width; uint8_t kind; };

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
/* A Shape is the field layout an instance currently has: names[i] lives in
 * slots[i]. Instances that were built the same way share one Shape, so the
 * layout is stored once per shape rather than once per instance -- an instance
 * is now a flat Value array instead of a hash table.
 *
 * Shapes are immutable and form a tree: adding a field to a shape follows (or
 * creates) an edge to the shape that has that field appended. Two instances
 * built by the same `init` therefore end up on the same Shape by construction,
 * which is what makes a property access cacheable: the call site remembers a
 * shape and a slot index, and one integer compare decides whether the memo
 * holds. `id` is what the caches compare -- never reused, so a collected shape
 * whose address is recycled cannot be mistaken for the one that was cached.
 *
 * This is also the groundwork for declared fields: `field a, b, c` gives the
 * shape at class-definition time instead of discovering it during init. */
/* How one slot of a machine-typed shape is stored. Absent (specs == NULL) for an
 * ordinary object shape, whose slots are Values. */
enum { SK_VALUE = 0, SK_I = 1, SK_U = 2, SK_PTR = 3, SK_SPAN = 4 };
typedef struct { uint16_t off; uint16_t width; uint8_t kind; } SlotSpec;

typedef struct Shape {
    Obj obj;
    uint32_t id;                        /* identity for inline caches; never 0, never reused */
    int nslots;
    ObjStr **names;                     /* names[i] -> slot i */
    struct ShapeEdge *edges;            /* name -> the shape with that field appended */
    int nedges, edgecap;
    /* A machine-typed shape describes bytes rather than Values: slot i lives at
     * specs[i].off for specs[i].width bytes. It is complete and immutable the
     * moment it is built (a kernel struct does not grow fields), so it never has
     * edges. `bytes` is the whole struct's size. */
    SlotSpec *specs;                    /* NULL for an ordinary object shape */
    ObjStr *sname;                      /* the C struct's name, for diagnostics */
    int bytes;
} Shape;
struct ShapeEdge { ObjStr *name; Shape *to; };

/* A byte buffer the collector owns, optionally carrying a shape that names
 * fields at fixed offsets.
 *
 * Shapeless it replaces alloc()/dealloc(): the same raw memory, minus the
 * pairing -- and `read_file_max` really did leak its buffer forever if anything
 * between the two raised, because that memory was plain malloc the GC never saw.
 *
 * Shaped it is a kernel struct. The offsets do not come from counting bytes in a
 * header comment; they are generated from include/abi/logit_abi.h and asserted
 * against that header by the compiler that builds /bin/as (see abi_layout.inc),
 * so a struct the kernel changes is a build failure rather than a script quietly
 * reading the wrong field. addr(buf) is exactly the pointer the kernel wants. */
typedef struct { Obj obj; Shape *shape; uint8_t *raw; int nbytes; } ObjBuf;

typedef struct {
    Obj obj;
    ObjClass *klass;
    Shape *shape;               /* which field lives in which slot */
    Value *slots; int slotcap;  /* slots[0 .. shape->nslots) */
} ObjInstance;
typedef struct {
    Obj obj;
    Value receiver;             /* the bound self (an instance) */
    ObjClosure *method;
} ObjBoundMethod;

/* A3 indirection: a typed pointer into raw memory. p[i] reads/writes `width`
 * bytes at addr + i*width (sign-extended on read when is_signed). */
typedef struct { Obj obj; uint64_t addr; int width; int is_signed; } ObjPtr;

/* range() -- an arithmetic sequence computed on demand, never materialised.
 * It used to build an ObjList, so `for i in range(1000000)` cost 16 MB of Value
 * array; on Logit /bin/as runs in a 24 MiB static arena, which made big loops a
 * correctness problem rather than a slow path. Nothing in the bytecode had to
 * change for this: `for x in seq` compiles to OP_LEN + OP_INDEX_GET, so a type
 * that answers both is iterable, and print/str/len/`in`/indexing are the rest of
 * the surface a list exposed. It is immutable: item assignment and .append()
 * fail, as they do for a string. */
typedef struct { Obj obj; int64_t start, step, count; } ObjRange;

/* A module: a name -> value namespace populated by running a .as file's top
 * level. `mod.x` reads `vars`; functions defined in it resolve globals here. */
typedef struct { ObjStr *name; Value val; } NameVal;
typedef struct ObjModule {
    Obj obj; ObjStr *name; NameVal *vars; int count, cap;
    int state;             /* 0 = loading, 1 = loaded (guards circular imports) */
} ObjModule;

#define IS_STR(v)    ((v).tag == OBJ_TAG(O_STR))
#define IS_FN(v)     ((v).tag == OBJ_TAG(O_FN))
#define IS_NATIVE(v) ((v).tag == OBJ_TAG(O_NATIVE))
#define IS_LIST(v)   ((v).tag == OBJ_TAG(O_LIST))
#define IS_PTR(v)    ((v).tag == OBJ_TAG(O_PTR))
#define IS_MODULE(v) ((v).tag == OBJ_TAG(O_MODULE))
#define AS_STR(v)    ((ObjStr *)AS_OBJ(v))
#define AS_FN(v)     ((ObjFn *)AS_OBJ(v))
#define AS_LIST(v)   ((ObjList *)AS_OBJ(v))
#define AS_PTR(v)    ((ObjPtr *)AS_OBJ(v))
#define AS_MODULE(v) ((ObjModule *)AS_OBJ(v))
#define IS_DICT(v)   ((v).tag == OBJ_TAG(O_DICT))
#define AS_DICT(v)   ((ObjDict *)AS_OBJ(v))
#define IS_CLOSURE(v) ((v).tag == OBJ_TAG(O_CLOSURE))
#define AS_CLOSURE(v) ((ObjClosure *)AS_OBJ(v))
#define IS_CLASS(v)        ((v).tag == OBJ_TAG(O_CLASS))
#define AS_CLASS(v)        ((ObjClass *)AS_OBJ(v))
#define IS_INSTANCE(v)     ((v).tag == OBJ_TAG(O_INSTANCE))
#define AS_INSTANCE(v)     ((ObjInstance *)AS_OBJ(v))
#define IS_BUF(v)          ((v).tag == OBJ_TAG(O_BUF))
#define AS_BUF(v)          ((ObjBuf *)AS_OBJ(v))
#define IS_SHAPE(v)        ((v).tag == OBJ_TAG(O_SHAPE))
#define AS_SHAPE(v)        ((Shape *)AS_OBJ(v))
#define IS_RANGE(v)        ((v).tag == OBJ_TAG(O_RANGE))
#define AS_RANGE(v)        ((ObjRange *)AS_OBJ(v))
/* i-th element; the caller bounds-checks against ->count. */
#define RANGE_AT(r, i)     ((r)->start + (r)->step * (i))
#define IS_BOUND_METHOD(v) ((v).tag == OBJ_TAG(O_BOUND_METHOD))
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
 * caller to stamp), returning NULL on bad magic/version/EOF or when the bytecode
 * fails the post-load verifier (as_bc.c -- a .la is untrusted input). */
int    as_dump(ObjFn *fn, FILE *out);
ObjFn *as_load(const uint8_t *buf, int len);
/* Print a loaded ObjFn tree as one line per instruction (as -dis). This is the
 * debugging tool for a broken self-hosting fixpoint: disassemble both .la files
 * and diff, instead of staring at two 37 KB binaries. */
void   as_disasm(ObjFn *fn);

/* modules (vm.c): the loader/cache + namespace access used by import. */
ObjModule *as_module_new(const char *name, int len);
Value     *as_module_slot(ObjModule *m, ObjStr *name, int create);  /* find/insert; NULL if absent */
int        as_module_index(ObjModule *m, ObjStr *name);   /* position in vars[], or -1 */
/* Incremented whenever a new global name is created anywhere; ObjFn.gcache is
 * valid only while it matches. */
extern uint32_t as_globals_gen;
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
ObjRange *as_range_new(int64_t start, int64_t step, int64_t count);
/* Instance fields, by shape. as_instance_get returns 0 if the field is absent;
 * as_instance_set adds it (transitioning the shape) and returns 0 only on OOM. */
int  as_shape_find(Shape *s, ObjStr *name);        /* slot index, or -1 */
/* Machine-typed shapes + the buffers that carry them. as_layout_shape_new takes
 * ownership of neither array; it copies. */
Shape  *as_layout_shape_new(ObjStr *sname, int bytes, ObjStr **names, SlotSpec *specs, int n);
ObjBuf *as_buf_new(Shape *shape, int nbytes);      /* zeroed; shape may be NULL */
int  as_buf_get(ObjBuf *b, const SlotSpec *sp, Value *out);
int  as_buf_set(ObjBuf *b, const SlotSpec *sp, Value v);   /* 0 = type/range error */
int  as_instance_get(ObjInstance *in, ObjStr *name, Value *out);
int  as_instance_set(ObjInstance *in, ObjStr *name, Value val);
void      as_chunk_write(ObjFn *fn, uint8_t b);
int       as_chunk_const(ObjFn *fn, Value v);
int       as_value_eq(Value a, Value b);
int       as_truthy(Value v);
void      as_print_value(Value v);    /* for `print` + REPL echo */
int       value_to_cstr(Value v, char *buf, int cap);  /* print-form into buf; returns length */
int       as_fmt_float(double d, char *buf, int cap);  /* shortest round-trip; ".0" for whole floats */
void      as_free_objects(void);      /* free all heap objects (end of run) */

/* Human-readable type name from the object descriptor table (object.c). Closures
 * report "fn" like functions do -- the distinction is an implementation detail. */
const char *as_obj_type_name(Obj *o);

/* mark-sweep GC (object.c + vm.c) */
void gc_collect(void);
void gc_mark_obj(Obj *o);          /* mark + push to the gray worklist */
void gc_mark_value(Value v);
void as_vm_mark_roots(void);      /* mark the VM roots (implemented in vm.c) */
long as_gc_live(void);            /* current live object count */
/* Temporary roots for objects that exist but aren't reachable from the VM yet.
 * Strictly LIFO: as_gc_release(n) drops the n most recent. Prefer these to
 * disabling the collector -- a disable that spans many allocations lets the heap
 * grow without bound, which on a 24 MiB arena is a real failure and not just a
 * missed collection. */
void as_gc_protect(Obj *o);
void as_gc_release(int n);
/* The blunt instrument. Only compile and .la load may use it: they build an
 * ObjFn tree that is unreachable until returned, and the object the builder is
 * currently inside cannot be named as a root. */
void as_gc_push_disable(void);
void as_gc_pop_disable(void);

/* platform output (as_io.c): write raw bytes to stdout (fd 1). */
void      as_emit(const char *s, int n);
void      as_emit_cstr(const char *s);
void      as_capture(char *buf, int cap);   /* redirect as_emit to a buffer (tests); NULL = stdout */

/* script argv (vm.c): as.c stashes argv[1..] so the args() native can return it */
void      as_set_args(int argc, char **argv);

/* native registration (vm.c) + A3 indirection builtins (as_native.c) */
void      as_define_native(const char *name, NativeFn fn);
void      as_define_int(const char *name, int64_t v);
Value     as_native_fail(const char *msg);  /* a native aborts the run with this message */
void      as_install_indirection(void);     /* registers peek/poke/addr/iNptr/syscall + SYS_* */

/* low-level bridge (as_ll.c): raw memory + the int 0x80 syscall (asm on Logit). */
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
