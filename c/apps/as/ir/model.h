/* SPDX-License-Identifier: MIT */
#ifndef AS_TYPED_INTERNAL_H
#define AS_TYPED_INTERNAL_H
#include "include/project.h"
#include "frontend/lexer.h"
#include "common/diagnostic.h"
#include "runtime/exception.h"
#include "runtime/class.h"
#include "runtime/buffer.h"
#include <stdint.h>

/* These are compiler resource limits, not limits on the language's eventual
 * runtime. Rejecting oversized input keeps an editor check bounded. */
#define AT_MODULES 64
#define AT_FUNCTIONS 512
#define AT_LOCALS 256
#define AT_ARGS 32
#define AT_ASSIGN_TARGETS 64
/* Generic parameters and their compound patterns occupy this table too: the
 * combined standard library exceeds the former 256 entries before lowering.
 * Keep addresses stable (recursive checking retains AtType pointers), with a
 * bounded 1024-entry arena rather than reallocating it under those pointers. */
#define AT_TYPES 1024
#define AT_TYPE_PARAMETERS 8
#define AT_GLOBALS 1024
#define AT_SOURCE_MAX (1024 * 1024)

/* Nonnegative call symbols are function-table indices. Built-ins occupy
 * negative IDs so checking resolves a call once and lowering uses that result.
 * Casts/constructors additionally encode their concrete type below a base. */
enum AtCallSymbol {
    AT_CALL_UNRESOLVED = -1,
    AT_CALL_PRINT = -2,
    AT_CALL_LEN = -3,
    AT_CALL_RANGE = -4,
    AT_CALL_WRAPPING_ADD = -5,
    AT_CALL_WRAPPING_SUB = -6,
    AT_CALL_WRAPPING_MUL = -7,
    AT_CALL_EXCEPTION = -8,
    AT_CALL_STR_FIND = -9,
    AT_CALL_STR_SLICE = -10,
    AT_CALL_LIST_APPEND = -11,
    AT_CALL_GC_COLLECT = -12,
    AT_CALL_GC_BYTES = -13,
    AT_CALL_STR_JOIN = -14,
    AT_CALL_STR_CASE = -15,
    AT_CALL_STR_STRIP = -16,
    AT_CALL_STR_SPLIT = -17,
    AT_CALL_STR_REPLACE = -18,
    AT_CALL_STR_SUB = -19,
    AT_CALL_WRAPPING_SHL = -20,
    AT_CALL_ANY_BOX = -21,
    AT_CALL_ANY_CAST = -22,
    AT_CALL_ANY_TEST = -23,
    AT_CALL_STR = -24,
    AT_CALL_PARSE_INT = -25,
    AT_CALL_PARSE_FLOAT = -26,
    AT_CALL_CHR = -27,
    AT_CALL_ORD = -28,
    AT_CALL_F64_BITS = -29,
    AT_CALL_DICT_GET = -30,
    AT_CALL_DICT_HAS = -31,
    AT_CALL_DICT_KEYS = -32,
    AT_CALL_DICT_VALUES = -33,
    AT_CALL_DICT_REMOVE = -34,
    AT_CALL_INDIRECT = -35,
    AT_CALL_CLASS = -36,
    AT_CALL_GC_OBJECTS = -37,
    AT_CALL_GC_RECLAIM = -38,
    AT_CALL_ARGS = -39,
    AT_CALL_BUFFER = -40,
    AT_CALL_CAPS = -41,
    AT_CALL_CAP_BITS = -42,
    AT_CALL_CAP_PATH = -43,
    AT_CALL_CAP_WITHOUT = -44,
    AT_CALL_CAP_SCOPE = -45,
    AT_CALL_ADDR = -46,
    AT_CALL_PEEK = -47,
    AT_CALL_POKE = -48,
    AT_CALL_BYTES = -49,
    AT_CALL_FILE_READ = -50,
    AT_CALL_FILE_WRITE = -51,
    AT_CALL_BYTES_DECODE = -52,
    AT_CALL_PORT_OPEN = -53,
    AT_CALL_PORT_METHOD = -54,
    AT_CALL_SYSCALL = -55,
    AT_CALL_MEMORY_TEXT = -56,
    AT_CALL_LAYOUT_NEW = -57,
    AT_CALL_PORT_BORROW = -58,
    AT_CALL_PORT_PIPE = -59,
    AT_CALL_COMMAND_NEW = -60,
    AT_CALL_COMMAND_METHOD = -61,
    AT_CALL_PORT_STATS = -62,
    AT_CALL_POINTER = -63,
    AT_CALL_ALLOC = -64,
    AT_CALL_DEALLOC = -65,
    AT_CALL_REGION = -66,
    AT_CALL_REGION_MOVE = -67,
    AT_CALL_REGION_BORROW = -68,
    AT_CALL_CAST_BASE = -100,
    /* Reserve the whole cast range. A fixed -1000 overlaps casts once the
     * type arena grows past 900 and silently lowers a cast as a constructor. */
    AT_CALL_STRUCT_BASE = AT_CALL_CAST_BASE - AT_TYPES
};

enum {
    AT_ERROR,
    AT_VOID,
    AT_BOOL,
    AT_I8,
    AT_I16,
    AT_I32,
    AT_I64,
    AT_U8,
    AT_U16,
    AT_U32,
    AT_U64,
    AT_F32,
    AT_F64,
    AT_STR,
    AT_ANY,
    AT_NONE,
    AT_RANGE,
    AT_BUFFER,
    AT_BYTES,
    AT_CAP,
    AT_PORT,
    AT_COMMAND,
    AT_PROCESS,
    AT_REGION,
    AT_BUILTIN_LAST = AT_REGION,
    AT_OPTIONAL,
    AT_ARRAY,
    AT_SLICE,
    AT_MUT_SLICE,
    AT_LIST,
    AT_DICT,
    AT_CALLABLE,
    AT_STRUCT,
    AT_CLASS,
    AT_LAYOUT,
    AT_POINTER,
    AT_PARAMETER
};

static inline int at_slice_kind(int kind)
{
    return kind == AT_SLICE || kind == AT_MUT_SLICE;
}

/* These three nominal/value types share the same bounded byte storage ABI.
 * Keep this predicate central so new consumers do not omit layout records. */
static inline int at_byte_storage_kind(int kind)
{
    return kind == AT_BUFFER || kind == AT_BYTES || kind == AT_LAYOUT;
}

enum AtConstraint {
    AT_CONSTRAINT_NONE,
    AT_CONSTRAINT_NUMBER,
    AT_CONSTRAINT_INTEGER,
    AT_CONSTRAINT_HASHABLE,
    AT_CONSTRAINT_EQUATABLE,
    AT_CONSTRAINT_ORDERED,
    AT_CONSTRAINT_BYTE_STORAGE,
    AT_CONSTRAINT_MUTABLE_BYTE_STORAGE
};

typedef struct {
    int kind;
    int module;     /* Nominal struct identity includes the declaring module. */
    int element;    /* Type-table index for Array/Slice; unused for scalars. */
    int key;        /* Dict key type; element is the independent value type. */
    int count;      /* Array length or number of struct fields. */
    int constraint; /* Operations guaranteed for a generic type parameter. */
    int base;       /* Single class base; its declared fields form our prefix. */
    char name[96];
    int fields[AT_ARGS];
    char names[AT_ARGS][64];
    /* Field declarations survive inheritance so tools can locate the source
     * that actually owns a field, rather than the derived type's name. */
    Token field_tokens[AT_ARGS];
    /* Explicit external layout records are reference-owned byte storage, not
     * ordinary value structs. Offsets include declared padding/overlap. */
    int layout_size;
    int offsets[AT_ARGS];
    int widths[AT_ARGS];
    char layout_name[96];
    Token declaration; /* Source snapshot location for nominal type tools. */
} AtType;

enum {
    AN_INT,
    AN_CONSTANT,
    AN_NONE,
    AN_FLOAT,
    AN_BOOL,
    AN_STR,
    AN_FORMAT,
    AN_NAME,
    AN_GLOBAL,
    AN_GLOBAL_DECL,
    AN_TYPE_APPLICATION,
    AN_FUNCTION,
    AN_METHOD,
    AN_SUPER,
    AN_BINARY,
    AN_UNARY,
    AN_CALL,
    AN_INDEX,
    AN_FIELD,
    AN_ARRAY,
    AN_COMPREHENSION,
    AN_DICT,
    AN_PAIR,
    AN_ASSIGN,
    AN_UNPACK,
    AN_CLOSURE,
    AN_EXPR,
    AN_RETURN,
    AN_IF,
    AN_WHILE,
    AN_FOR,
    AN_BREAK,
    AN_CONTINUE,
    AN_ASSERT,
    AN_TRY,
    AN_EXCEPT,
    AN_RAISE,
    AN_CONDITIONAL,
    AN_PASS,
    AN_UNSAFE,
    AN_WITH,
    AN_ERROR /* Recovered syntax has no value or inferred type; never lowered. */
};
typedef struct AtNode AtNode;

struct AtNode {
    Token member_separator; /* Original dot, retained when checking rewrites a field node. */
    int kind;
    int type;   /* Resolved type-table index; AT_ERROR suppresses cascades. */
    int op;     /* Lexer operator token for unary/binary/compound assignment. */
    int symbol; /* Local/function index, or a negative built-in call encoding. */
    int module;
    int function;     /* Owning function index for entry-block temporary storage. */
    int id;           /* Stable index in the project node registry. */
    int field;        /* Resolved struct field index, independent of its spelling. */
    int value_type;   /* Type before an Optional injection or proven local unwrap. */
    int conversion;   /* AT_OPTION_*; storage keeps its declared type after narrowing. */
    int class_ready;  /* Constructor self escape needs the dynamic object's full mask. */
    int field_store;  /* Constructor field store establishes one initialized bit. */
    Token token;      /* Borrows the owning module's immutable source buffer. */
    uint64_t integer; /* Checked literal magnitude; lowering must not reparse it. */

    /* Child layout is determined by kind. Keep parser, checker and lowering
     * consistent when introducing a node; next always links statements only.
     *
     * binary/index/assignment: a = left/base/target, b = right/index/value
     * unary/field/expr/return/format: a = operand/base/expression/value
     * call:                   a = callee; args = arguments
     * if:                     a = condition, b = then body, c = else body
     * while:                  a = condition, b = body
     * for:                    a = loop variable, b = iterable, c = body
     * with:                   a = owner name or AN_UNPACK names, b = acquisition, c = body
     * array:                  args = elements
     * comprehension:          a = element, b = iterable, c = guard; args[0] = binding
     * unpack:                 args = assignments; a = sequence, or NULL for comma RHS
     *                         inside with, args are owner names and a is NULL
     * dict:                   args = pairs; each pair has a = key, b = value
     * function:               symbol = concrete function; op = template id + 1
     * try:                    a = protected body, b = handlers, c = else body
     * except:                 a = optional binding, b = handler body; op = code
     * raise:                  a = Error value, or NULL for rethrow
     * conditional expression: a = condition, b = true value, c = false value
     */
    AtNode *a;
    AtNode *b;
    AtNode *c;
    AtNode *next;
    AtNode **args;
    int count;
};

typedef struct {
    char name[64];
    int type;
    int initialized;    /* Definite-assignment fact for the current control path. */
    int captured;       /* Storage is a shared native heap cell, even for scalar values. */
    int scoped_borrow;  /* A with-bound view has a separate lexical cleanup record. */
    int capture_parent; /* One plus the immediate parent's local slot; zero for owned locals. */
    int capture_index;  /* Environment field index when capture_parent is nonzero. */
    Token token;
} AtLocal;

enum AtOptionalConversion {
    AT_OPTION_IDENTITY,
    AT_OPTION_WRAP,
    AT_OPTION_UNWRAP,
    AT_CLASS_UPCAST
};

enum AtLocalFacts {
    AT_LOCAL_INITIALIZED = 1,
    AT_LOCAL_PRESENT = 2
};

typedef struct {
    char name[64];
    int target;      /* Module index, or -1 if loading failed. */
    char member[64]; /* Empty for 'import m'; member name for 'from m import f'. */
    Token token;
} AtImport;

typedef struct {
    char path[512];
    char *source; /* Owned until the entire project snapshot is freed. */
    Token *tokens;
    int count;
    int state;  /* 0 = unseen, 1 = on import DFS stack, 2 = finished. */
    int parent; /* DFS parent for complete cycle diagnostics; entry uses -1. */
    AsDiagnostics diagnostics;
    AtImport imports[AT_ARGS];
    int nimports;
    int initializer; /* Synthetic native function, or -1 for declaration-only modules. */
} AtModule;

typedef struct {
    char name[64];
    int module;
    int type;
    int initialized; /* Source-order fact during module initializer checking. */
    Token token;
} AtGlobal;

typedef struct {
    int modules[AT_MODULES + 1]; /* First and last IDs match to close the cycle. */
    int count;
    int diagnostic_module;
    int diagnostic_index;
} AtImportCycle;

struct AtChecker;

typedef struct {
    char name[64];
    int module;
    int result;
    int nparams;
    int checked;
    int checking;
    int infer_result; /* Private declaration omitted ->; solved from its body. */
    int module_initializer;
    int lexical_parent; /* One plus enclosing function ID; zero for module declarations. */
    int capture_count;
    struct AtChecker *active_checker; /* Borrowed only while this function is being checked. */
    int method_owner;                 /* Zero for module functions; class type ID for methods. */
    uint32_t initialized_fields;      /* Constructor flow fact, independent of local assignment. */
    int globals[AT_ARGS]; /* Explicit global declarations, resolved before local collection. */
    int nglobals;
    int generic_count;
    int type_parameters[AT_TYPE_PARAMETERS];
    int template_id; /* -1 for source declarations; otherwise the original template. */
    int type_arguments[AT_TYPE_PARAMETERS];
    /* Parameters occupy the first nparams entries. Indices remain stable
     * while checking branches, so emitted local slots refer to the same name. */
    AtLocal locals[AT_LOCALS];
    int nlocals;
    AtNode *body;
    Token token;
} AtFunction;

struct AsTypedProject {
    char library[512];
    /* Fixed tables keep indices and pointers stable during recursive imports.
     * The growable node registry owns nodes individually; child links borrow. */
    AtModule modules[AT_MODULES];
    int nmodules;
    int initialization_order[AT_MODULES];
    int ninitializers;
    AtGlobal globals[AT_GLOBALS];
    int nglobals;
    AtImportCycle cycles[AT_MODULES];
    int ncycles;
    AtFunction functions[AT_FUNCTIONS];
    int nfunctions;
    AtType types[AT_TYPES];
    int ntypes;
    int type_limit_reported; /* One capacity diagnostic per source snapshot. */
    AtNode **nodes;
    int nnodes;
    int nodecap;
    /* Overlays are borrowed only during construction, then cleared. Modules
     * copy their bytes, so Studio may edit its buffers after check() returns. */
    const AsSourceOverlay *overlays;
    int noverlays;
    int oom;
    int specialization_depth;
    int exception_type;
};

void at_error(AsTypedProject *p, int module, Token token, const char *code, const char *message);
/* Text interpolation reuses the expression parser. Fragment tokens are
 * rebased into the owning snapshot before temporary lexer buffers are freed. */
AtNode *at_parse_new_node(AsTypedProject *project, int module, int function, int kind, Token site);
AtNode *at_parse_fragment(AsTypedProject *project, int module, int function, Token text, int depth);
AtNode *at_parse_fstring(AsTypedProject *project, int module, int function, Token text, int depth);
void at_quote(FILE *f, const char *text);
void at_quote_bytes(FILE *f, const char *text, size_t bytes);
int at_integer(AsTypedProject *p, int type);
int at_signed(AsTypedProject *p, int type);
int at_bits(AsTypedProject *p, int type);
int at_global_lookup(AsTypedProject *project, int module, Token name, int imports);
int at_dict_type(AsTypedProject *project, int key, int value, int module, Token site);
int at_named_type(AsTypedProject *project, int module, const char *name);
int at_function_type(AsTypedProject *project, AtFunction *function, Token name);
int at_allocate_type(AsTypedProject *project, int module, Token site);
int at_compound_type(AsTypedProject *project, int kind, int element, int count, int module,
                     Token site);
int at_has_parameter(AsTypedProject *project, int type);
int at_satisfies_constraint(AsTypedProject *project, int type, int constraint);
int at_callable_type(AsTypedProject *project, const int *parameters, int count, int result,
                     int module, Token site);
int at_substitute_type(AsTypedProject *project, AtFunction *function, int type,
                       const int bindings[AT_TYPE_PARAMETERS]);
int at_bind_result_context(AsTypedProject *project, AtFunction *function, int pattern, int actual,
                           int bindings[AT_TYPE_PARAMETERS]);
int at_bind_type(AsTypedProject *project, AtFunction *function, int pattern, int actual,
                 int bindings[AT_TYPE_PARAMETERS], AtNode *site);
int at_specialize(AsTypedProject *project, int template_id, const int bindings[AT_TYPE_PARAMETERS],
                  AtNode *site, int *result_type);
void at_check_functions(AsTypedProject *p);
int at_exception_code(Token name);
void at_initialize_exceptions(AsTypedProject *project);
#endif
