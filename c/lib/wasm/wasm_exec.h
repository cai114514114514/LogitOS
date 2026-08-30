/* WebAssembly MVP interpreter: instantiation and execution.
 *
 * WHY AN INTERPRETER AND NOT A PORT -- the same argument wasm.h makes about
 * the decoder, only sharper here.  V8 reserves four to eight GiB of address
 * space per wasm memory so that a bounds check becomes a guard-page fault.
 * This machine's whole user address space is one PDPT entry,
 * [0x40000000, 0x80000000) -- CLAUDE.md structural gap #2 verbatim, and the
 * reservation dies on its first call.  An interpreter does the bounds check
 * with an explicit compare and allocates exactly the pages the module asked
 * for.  The constraint that blocks the port is what the implementation
 * sidesteps.
 *
 * A TRAP IS A VALUE, NOT A FAULT.  Every trapping condition in the MVP --
 * `unreachable`, out-of-bounds memory, an out-of-range or NaN float-to-int
 * conversion, division by zero, INT_MIN / -1, an indirect call through an
 * empty or wrongly-typed table slot -- is checked BEFORE the operation that
 * would fault, and returns a WASM_TRAP_* code up through every frame.  Nothing
 * in this file can raise SIGFPE or SIGSEGV on the host, which is the property
 * `make test-wasm-exec-asan` exists to hold: a trap that reaches the host as a
 * signal is a wasm module killing the browser.
 *
 * AND AN OPCODE THAT IS NOT IMPLEMENTED IS A NAMED TRAP, NEVER A VALUE.
 * CLAUDE.md rule 3 -- absent beats present-and-wrong.  A page feature-tests
 * WebAssembly and then trusts the answer, so an instruction that quietly
 * yields a plausible number is strictly worse than one that refuses.  The
 * decoder already refuses every post-MVP opcode by name (WASM_E_UNSUPPORTED),
 * so the interpreter's default arm should be unreachable -- it is still
 * written as WASM_TRAP_UNIMPLEMENTED carrying the opcode byte, because "should
 * be unreachable" is how silent fallthroughs are argued for.
 *
 * FREESTANDING, like the rest of c/lib/wasm: no libc, no allocator but the
 * caller's arena.  The one compiler dependency is __builtin_sqrt{,f}, which is
 * a hardware instruction on both targets -- build with -fno-math-errno or
 * clang emits a libcall to sqrt() for the errno path (measured: it does for
 * --target=x86_64-elf, and does not on darwin/arm64).
 */
#ifndef C_LIB_WASM_WASM_EXEC_H_
#define C_LIB_WASM_WASM_EXEC_H_

#include "wasm.h"

/* ---- traps and other run-time outcomes --------------------------------
 * Numbered from 100 so that one int can carry either a WASM_E_* verdict about
 * a module or a WASM_TRAP_* outcome of running one, and wasm_trapstr() can
 * name both without the caller having to know which space it is holding. */
enum {
	WASM_TRAP_NONE = 0,

	/* the MVP's trapping conditions */
	WASM_TRAP_UNREACHABLE = 100,
	WASM_TRAP_MEM_OOB,          /* out of bounds memory access */
	WASM_TRAP_TABLE_OOB,        /* undefined element (index past the table) */
	WASM_TRAP_UNINIT_ELEM,      /* uninitialized element */
	WASM_TRAP_INDIRECT_TYPE,    /* indirect call type mismatch */
	WASM_TRAP_DIV_ZERO,         /* integer divide by zero */
	WASM_TRAP_INT_OVERFLOW,     /* integer overflow */
	WASM_TRAP_BAD_CONVERSION,   /* invalid conversion to integer (NaN) */
	WASM_TRAP_EXHAUSTED,        /* call stack or operand stack exhausted */
	WASM_TRAP_HOST,             /* an imported host function trapped */

	/* refusals that are OURS, named rather than silent */
	WASM_TRAP_UNIMPLEMENTED = 130,  /* opcode not implemented -- never a value */
	WASM_TRAP_UNLINKABLE,           /* an import this host cannot satisfy */
	WASM_TRAP_DESYNC,               /* the instruction-boundary cross-check fired */

	/* apparatus, NOT a verdict about the module (see wasm.h on the same
	 * distinction for WASM_E_NOMEM): our arena ran out. */
	WASM_TRAP_NOMEM = 140
};

const char *wasm_trapstr(int code);   /* also names WASM_E_* codes */

/* True when `code` is the module's fault.  False for WASM_TRAP_NONE and false
 * for UNIMPLEMENTED / UNLINKABLE / DESYNC / NOMEM, which are statements about
 * this implementation.  A gate that counts those as "correctly trapped" is a
 * control that cannot be watched failing -- it reports green on an empty
 * arena. */
int wasm_trap_is_module_fault(int code);

/* ---- values -----------------------------------------------------------
 * Untagged: validation has already proved what is in each slot, and every
 * member is at most 8 bytes.  `bits` is the raw view, which is what a
 * differential gate compares -- a float printed as a decimal cannot
 * distinguish -0.0 from 0.0 or one NaN payload from another. */
union wasm_val {
	uint32_t i32;
	uint64_t i64;
	float    f32;
	double   f64;
	uint64_t bits;
};

/* ---- host imports ------------------------------------------------------
 * A host function returns WASM_TRAP_NONE, or a trap code which propagates as
 * WASM_TRAP_HOST would.  `args` holds nparams values; results are written to
 * `rets`.  Both are borrowed. */
struct wasm_instance;
typedef int (*wasm_hostfn)(void *ctx, const union wasm_val *args,
                           union wasm_val *rets);

enum { WASM_IMP_FUNC = 0, WASM_IMP_GLOBAL = 1, WASM_IMP_MEMORY = 2, WASM_IMP_TABLE = 3 };

struct wasm_hostimport {
	const char *module;      /* NUL-terminated */
	const char *name;
	uint8_t kind;            /* WASM_IMP_* */

	wasm_hostfn fn;          /* WASM_IMP_FUNC */
	void *ctx;

	union wasm_val gval;     /* WASM_IMP_GLOBAL: the value */
	uint8_t gtype, gmut;     /* its valtype and mutability */

	/* WASM_IMP_MEMORY / WASM_IMP_TABLE: storage owned by the HOST, shared
	 * with the instance.  Sharing rather than copying is the point -- a
	 * memory that were copied in would silently stop being the same memory
	 * the moment the module wrote to it. */
	uint8_t *mem; uint32_t *mpages, *mmaxpages;
	uint32_t *table; uint32_t *tsize, *tmax;

	/* AND SHARING IS NOT ENOUGH, BECAUSE THE MEMORY MOVES.  `mem_grow` is
	 * allocate-and-copy out of a bump arena (see its comment), so a
	 * `memory.grow` executed INSIDE the module relocates the linear memory
	 * and leaves `mem` above pointing at the pre-grow block -- while
	 * `*mpages` is faithfully updated to the NEW page count.  That pair is
	 * the silent-wrong-answer shape: measured 2026-08-30, a host that kept
	 * reading `mem` saw a buffer of exactly the right new length holding
	 * the old bytes, and a store the module had just made was invisible.
	 * With a JS API on top that is a WebAssembly.Memory whose ArrayBuffer
	 * reads zeroes after the module grows itself, with nothing anywhere
	 * reporting an error.
	 *
	 * `memp` closes it: pass the ADDRESS of your own pointer and the
	 * interpreter writes the new base back through it on every grow.  It is
	 * optional -- NULL means "this host never looks at the memory again
	 * after instantiation", which is true of the spec-suite runner -- so the
	 * field is additive and no existing caller changes.  A host that DOES
	 * read the memory later and leaves this NULL gets the stale block, which
	 * is why js_wasm.c sets it unconditionally.
	 *
	 * There is no table equivalent on purpose: `table.grow` is 0xFC-prefixed
	 * reference-types, refused by name as post-MVP, so a table never moves. */
	uint8_t **memp;
};

/* ---- instances ---------------------------------------------------------- */

struct wasm_instance;

/* Instantiate `m` (which MUST already have passed wasm_validate).  Allocates
 * everything -- linear memory, the table, globals, the per-function control
 * map -- out of `a`, which the instance borrows for its whole life.  Runs the
 * data and element segments and then the start function, so a trap here is a
 * real outcome the spec suite asserts (assert_uninstantiable). */
int wasm_instantiate(struct wasm_instance **out, const struct wasm_module *m,
                     struct wasm_arena *a,
                     const struct wasm_hostimport *imps, uint32_t nimps);

/* Call function `fidx` (a FUNCTION INDEX, imports included).  `args` holds the
 * declared parameters; `rets` receives the declared results.  Returns
 * WASM_TRAP_NONE or a trap code. */
int wasm_invoke(struct wasm_instance *in, uint32_t fidx,
                const union wasm_val *args, union wasm_val *rets);

/* Look an export up by name (bytes, not NUL-terminated -- an export name may
 * contain a NUL and comparing as C strings would make two distinct exports
 * equal).  kind is WASM_EXT_*. */
int wasm_export_index(const struct wasm_instance *in, const char *name,
                      uint32_t namelen, uint8_t kind, uint32_t *idx);

const struct wasm_functype *wasm_func_signature(const struct wasm_instance *in,
                                                uint32_t fidx);
union wasm_val wasm_global_get(const struct wasm_instance *in, uint32_t g);

/* ---- state the JS API reads and writes ---------------------------------
 * WebAssembly.Table.prototype.get/set, .Global.prototype.value and
 * .Memory.prototype.grow are reads and writes of state this interpreter
 * already owns.  Exposing it is what stops the binding from keeping a second
 * copy alongside -- one jar, two doors.  Every one bounds-checks and returns a
 * status; none of them clamps. */

/* A table slot holding no function.  Table.prototype.get returns null for it
 * and Table.prototype.set writes it for a null argument. */
#define WASM_NOFUNC 0xFFFFFFFFu

uint32_t wasm_table_size(const struct wasm_instance *in);
int wasm_table_get(const struct wasm_instance *in, uint32_t i, uint32_t *fidx);
int wasm_table_set(struct wasm_instance *in, uint32_t i, uint32_t fidx);

/* Mutability is the CALLER's check -- see the note in the implementation:
 * the JS API owes a TypeError for an immutable global, and a numeric refusal
 * here would be a second, worse spelling of the same rule. */
int wasm_global_set(struct wasm_instance *in, uint32_t g, union wasm_val v);

uint32_t wasm_mem_pages(const struct wasm_instance *in);

/* Grow by `delta` pages through the interpreter's own path, so that a JS
 * memory.grow and the memory.grow OPCODE move the same bytes and write the
 * same base back through `memp`.  0 on success (*oldpages = the previous page
 * count), -1 if the module's own maximum, the 65536-page ceiling or this
 * implementation's arena refused it. */
int wasm_mem_grow(struct wasm_instance *in, uint32_t delta, uint32_t *oldpages);

uint8_t *wasm_mem_bytes(const struct wasm_instance *in, uint32_t *nbytes);

/* How many times this instance answered "no" because OUR arena ran out rather
 * than because the module said so -- a memory.grow that could have succeeded,
 * for instance.  Nonzero means the numbers above it are about the harness. */
uint32_t wasm_apparatus_count(const struct wasm_instance *in);

/* The opcode byte that produced WASM_TRAP_UNIMPLEMENTED or WASM_TRAP_DESYNC.
 * An unimplemented opcode that cannot be named is most of the way back to a
 * silent one. */
uint8_t wasm_trap_opcode(const struct wasm_instance *in);

/* Bound on the interpreter's recursion, exposed because it is a property of
 * THIS implementation and not of WebAssembly: a test that recurses deeper is
 * measuring our C stack budget, not the module. */
#define WASM_MAX_CALL_DEPTH 1024u

#endif /* C_LIB_WASM_WASM_EXEC_H_ */
