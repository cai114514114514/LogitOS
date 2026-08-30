/* WebAssembly MVP: binary decoder + validator.
 *
 * WHY THIS EXISTS, AND IT IS NOT "THE WEB NEEDS IT".  It was measured:
 * across the 192 distinct real JavaScript bundles in tests/fixtures the
 * string "WebAssembly" appears in TWO, both behind a working feature test,
 * and the string ".wasm" appears ZERO times anywhere in the corpus.  The
 * argument is CLAUDE.md structural gap #2 -- "there is no loading contract
 * for a foreign binary": PT_INTERP, PT_DYNAMIC and ET_DYN are refused by
 * name, there are zero relocations, no dlopen, and every ring-3 link base is
 * assigned by hand in the Makefile.  A wasm module has no relocations, no
 * link base, no interpreter and no dynamic linking; every address in it is an
 * index into its own tables or an offset into its own linear memory.  It is
 * the one code format for which gap #2 does not exist.
 *
 * And it is the reason this is an interpreter's front end rather than a port.
 * V8 reserves 4-8 GiB of address space up front so that a bounds check
 * becomes a guard page.  The user address space on this machine is one PDPT
 * entry, [0x40000000, 0x80000000).  That reservation is gap #2 verbatim.
 *
 * FREESTANDING, the way c/lib/ime/pinyin.c and c/kernel/module/modelf.c are
 * freestanding, and for the reason modelf.c states: it calls nothing -- no
 * kmalloc, no kprintf, no VFS, no locks -- so its arithmetic is testable on
 * the host.  That property is what makes the official WebAssembly spec test
 * suite runnable as an ordinary host gate instead of a QEMU boot.  The single
 * injected resource is a caller-owned bump arena (struct wasm_arena).
 *
 * SCOPE IS MVP AND THE REFUSAL IS BY NAME.  Sign-extension operators, the
 * 0xFC/0xFD/0xFE prefixes (saturating truncation, bulk memory, SIMD, atomics),
 * reference types, multi-value block types, multiple tables and multiple
 * memories are all refused with WASM_E_UNSUPPORTED, which names the feature.
 * They are never accepted and never silently mis-decoded.  CLAUDE.md rule:
 * absent beats present-and-wrong; an unimplemented opcode must be a named,
 * thrown error, never a silent wrong answer.
 *
 * TWO ERROR CLASSES ARE NOT REJECTIONS AND CALLERS MUST NOT TREAT THEM AS
 * ONE.  WASM_E_NOMEM (the arena ran out) and WASM_E_LIMIT (an implementation
 * bound was hit) mean THIS DECODER gave up, not that the module was bad.  A
 * gate that counts them as "correctly rejected" is a control that cannot be
 * watched failing -- it would report a green suite on an arena of zero bytes.
 * wasm_err_is_rejection() exists so that distinction has exactly one spelling.
 */
#ifndef C_LIB_WASM_WASM_H_
#define C_LIB_WASM_WASM_H_

#include <stdint.h>
#include <stddef.h>

/* ---- errors ---------------------------------------------------------- */

enum {
	WASM_OK = 0,

	/* malformed: the byte string is not a module at all */
	WASM_E_MAGIC = 1,          /* magic header not detected */
	WASM_E_VERSION,            /* unknown binary version */
	WASM_E_END,                /* unexpected end of input / of section */
	WASM_E_LEB_LONG,           /* integer representation too long */
	WASM_E_LEB_LARGE,          /* integer too large */
	WASM_E_SECTION_ID,         /* malformed section id */
	WASM_E_SECTION_ORDER,      /* junk after last section / out of order */
	WASM_E_SECTION_SIZE,       /* section size mismatch */
	WASM_E_UTF8,               /* malformed UTF-8 encoding */
	WASM_E_FORM,               /* wrong magic byte for a form (functype etc) */
	WASM_E_VALTYPE,            /* malformed value type */
	WASM_E_MUT,                /* malformed mutability */
	WASM_E_FLAGS,              /* malformed limits flags */
	WASM_E_IMPORT_KIND,        /* malformed import/export kind */
	WASM_E_RESERVED,           /* zero byte expected */
	WASM_E_OPCODE,             /* illegal opcode */
	WASM_E_LENGTHS,            /* function and code section inconsistent */
	WASM_E_LOCALS,             /* too many locals */
	WASM_E_JUNK,               /* junk after the last section / after `end` */

	/* invalid: it decodes, and it is not a well-formed program */
	WASM_E_TYPE = 40,          /* type mismatch */
	WASM_E_INDEX,              /* unknown type/function/table/memory/global/local/label */
	WASM_E_LIMITS,             /* size minimum must not be greater than maximum */
	WASM_E_MEMSIZE,            /* memory size must be at most 65536 pages */
	WASM_E_DUP_EXPORT,         /* duplicate export name */
	WASM_E_START,              /* start function must have type [] -> [] */
	WASM_E_CONSTEXPR,          /* constant expression required */
	WASM_E_MUT_GLOBAL,         /* global is immutable */
	WASM_E_ALIGN,              /* alignment must not be larger than natural */
	WASM_E_MULTI_TABLE,        /* multiple tables */
	WASM_E_MULTI_MEMORY,       /* multiple memories */

	/* refusals that are OURS, named rather than silent */
	WASM_E_UNSUPPORTED = 80,   /* a real wasm feature this MVP decoder does not do */

	/* apparatus, NOT a verdict about the module */
	WASM_E_NOMEM = 90,         /* the caller's arena is exhausted */
	WASM_E_LIMIT               /* an implementation bound was reached */
};

const char *wasm_errstr(int err);

/* True when `err` is this decoder saying "that module is bad".  False for
 * WASM_OK and false for WASM_E_NOMEM / WASM_E_LIMIT / WASM_E_UNSUPPORTED --
 * see the header comment: those three are statements about US. */
int wasm_err_is_rejection(int err);

/* ---- the injected allocator ------------------------------------------ */

struct wasm_arena {
	unsigned char *base;
	uint32_t size;
	uint32_t used;
};

void  wasm_arena_init(struct wasm_arena *a, void *base, uint32_t size);
/* count*elemsize bytes, 8-aligned, zeroed.  Returns NULL on overflow or
 * exhaustion; the multiply is checked, not hoped for. */
void *wasm_arena_alloc(struct wasm_arena *a, uint32_t count, uint32_t elemsize);

/* ---- types ----------------------------------------------------------- */

#define WASM_VT_I32     0x7F
#define WASM_VT_I64     0x7E
#define WASM_VT_F32     0x7D
#define WASM_VT_F64     0x7C
#define WASM_ET_FUNCREF 0x70
#define WASM_FORM_FUNC  0x60
#define WASM_BT_EMPTY   0x40

/* The validator's polymorphic "any type", from the spec's validation
 * algorithm.
 *
 * IT WAS 0x00 AND THAT WAS A SILENT BUG, kept here as the worked example
 * because the reasoning that produced it is correct and still wrong.  "0x00 is
 * not a legal valtype encoding, so it cannot collide" -- true about the BINARY
 * FORMAT, and the collision was never with the format.  wasm_valid.c also
 * spells "this block produces no result" as 0 (`struct ctrl.out`), so
 * `push_val` opens with `if (t == 0) return;`.  Unknown and no-result were the
 * same number through the same door: `select` with both operands polymorphic
 * pushes Unknown, push_val dropped it, the operand stack came back one slot
 * short, and the leftover-operand check at `end` therefore did not fire.
 * `(func (unreachable) (select))` -- unreached-invalid.wast:56, asserted
 * "type mismatch" -- validated CLEAN.  CLAUDE.md rule 3, one jar two doors.
 *
 * 0x01 is likewise not a valtype byte, and it is not the no-result sentinel
 * either, which is the whole property being bought. */
#define WASM_VT_UNKNOWN 0x01
#define WASM_VT_NORESULT 0x00   /* `ctrl.out` only: a block that yields nothing */

enum { WASM_EXT_FUNC = 0, WASM_EXT_TABLE = 1, WASM_EXT_MEM = 2, WASM_EXT_GLOBAL = 3 };

struct wasm_span { uint32_t off, len; };   /* into wasm_module.bytes */

struct wasm_functype {
	uint32_t nparams, nresults;
	const uint8_t *params;     /* into the module bytes */
	const uint8_t *results;
};

struct wasm_limits { uint32_t min, max; uint8_t has_max; };

struct wasm_tabletype { uint8_t elemtype; struct wasm_limits lim; };
struct wasm_globaltype { uint8_t valtype; uint8_t mut; };

struct wasm_import {
	struct wasm_span module_name, field_name;
	uint8_t kind;
	uint32_t typeidx;                  /* WASM_EXT_FUNC */
	struct wasm_tabletype tt;          /* WASM_EXT_TABLE */
	struct wasm_limits mem;            /* WASM_EXT_MEM */
	struct wasm_globaltype gt;         /* WASM_EXT_GLOBAL */
};

struct wasm_export { struct wasm_span name; uint8_t kind; uint32_t index; };

struct wasm_global { struct wasm_globaltype gt; struct wasm_span init; };

struct wasm_elem {
	uint32_t tableidx;
	struct wasm_span offset;
	uint32_t nfunc;
	const uint8_t *funcs_raw;   /* re-decoded in validate; LEBs, not fixed width */
	uint32_t funcs_raw_len;
};

struct wasm_data {
	uint32_t memidx;
	struct wasm_span offset;
	struct wasm_span bytes;
};

struct wasm_localdecl { uint32_t count; uint8_t type; };

struct wasm_code {
	struct wasm_span body;       /* the whole entry after its size prefix */
	struct wasm_span expr;       /* just the instructions */
	uint32_t ndecl;
	struct wasm_localdecl *decl;
	uint32_t nlocals;            /* total, params NOT included */
};

struct wasm_module {
	const uint8_t *bytes;
	uint32_t len;

	uint32_t ntypes;    struct wasm_functype  *types;
	uint32_t nimports;  struct wasm_import    *imports;
	uint32_t nfuncs;    uint32_t              *funcs;      /* defined: type idx */
	uint32_t ntables;   struct wasm_tabletype *tables;     /* defined */
	uint32_t nmems;     struct wasm_limits    *mems;       /* defined */
	uint32_t nglobals;  struct wasm_global    *globals;    /* defined */
	uint32_t nexports;  struct wasm_export    *exports;
	uint32_t nelems;    struct wasm_elem      *elems;
	uint32_t ncode;     struct wasm_code      *code;
	uint32_t ndatas;    struct wasm_data      *datas;

	uint8_t  has_start; uint32_t start;

	/* index spaces: imports first, then definitions (spec 2.5.1) */
	uint32_t nimp_funcs, nimp_tables, nimp_mems, nimp_globals;
	uint32_t total_funcs, total_tables, total_mems, total_globals;
	uint32_t *functype_of;                 /* total_funcs entries */
	struct wasm_globaltype *globaltype_of; /* total_globals entries */
};

/* ---- the two passes -------------------------------------------------- */

/* Decode: structure only.  Everything the spec calls "malformed" is caught
 * here, including a full syntactic walk of every function body (opcodes and
 * immediates), which is why a truncated br_table is a decode error and a
 * br_table to a label that does not exist is not. */
int wasm_decode(const uint8_t *bytes, uint32_t len,
                struct wasm_module *m, struct wasm_arena *a);

/* Validate: the stack-machine type check over every function body, plus the
 * module-level conditions (index ranges, limits, export-name uniqueness, the
 * start signature, constant expressions).  Everything the spec calls
 * "invalid". */
int wasm_validate(struct wasm_module *m, struct wasm_arena *a);

/* decode then validate */
int wasm_load(const uint8_t *bytes, uint32_t len,
              struct wasm_module *m, struct wasm_arena *a);

/* Exposed for the gate: the utf8 corpus in the spec suite is 528 cases and
 * they are worth being able to drive directly. */
int wasm_utf8_ok(const uint8_t *p, uint32_t n);

#endif /* C_LIB_WASM_WASM_H_ */
