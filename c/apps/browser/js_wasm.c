/* The WebAssembly JavaScript API, over c/lib/wasm's MVP interpreter.
 *
 * WHY THIS EXISTS, AND IT IS NOT "THE WEB NEEDS IT".  Measured over
 * tests/fixtures: the string "WebAssembly" appears in TWO of the real
 * JavaScript bundles captured from live sites, both behind a working feature
 * test, and ".wasm" appears in none of them.  Web compatibility is not the
 * argument and a sentence claiming it would be false.
 *
 * The argument is CLAUDE.md structural gap #2 -- "there is no loading contract
 * for a foreign binary": PT_INTERP, PT_DYNAMIC and ET_DYN refused by name,
 * zero relocations, no dlopen, one PDPT entry of user address space, every
 * ring-3 link base assigned by hand in the Makefile.  A wasm module has no
 * relocations, no link base, no interpreter and no dynamic linking; every
 * address in it is an index into its own tables or an offset into its own
 * linear memory.  It is the one code format for which that gap does not
 * exist, and this file is the door a page reaches it through.
 *
 * ---- the rule this file is written against ----
 *
 * A PAGE FEATURE-TESTS THE CONSTRUCTOR AND THEN TRUSTS THE ANSWER.  That
 * makes a present-and-wrong WebAssembly strictly worse than an absent one --
 * `typeof WebAssembly === 'undefined'` is a sentence every bundler already
 * understands, and a Module that instantiates incorrectly is one nothing
 * downstream can detect.  So: nothing here fabricates a plausible value.  A
 * trap becomes a RuntimeError, an import this host cannot satisfy becomes a
 * LinkError, and `Module.customSections` is LEFT UNDEFINED rather than
 * returning `[]`, because `[]` is a claim that the module has no custom
 * section and we do not retain them to know.  That is the same reason
 * c/apps/libc returns ENOSYS from flock instead of 0.
 *
 * ---- three things that are easy to get silently wrong ----
 *
 * 1. GROWING A MEMORY MOVES IT.  c/lib/wasm's `mem_grow` is allocate-and-copy
 *    out of a bump arena, so after a grow the old base is stale -- and this is
 *    NOT only the `WebAssembly.Memory.prototype.grow` path.  The `memory.grow`
 *    OPCODE, executed deep inside a call the page made, moves it too.  A JS
 *    view taken before that call would keep reading the pre-grow block: right
 *    length, wrong bytes, no error anywhere.  The specification's answer is
 *    the same as the safe one -- a grow DETACHES the buffer -- so `jw_mem_sync`
 *    runs after every call as well as on every `.buffer` read, and detaching
 *    is what turns a silent stale read into a loud TypeError.  It is a
 *    use-after-free with a JavaScript face on it, and the spec rule is the fix.
 *
 * 2. AN i64 IS A BIGINT AT THE BOUNDARY, both directions.  A Number cannot
 *    hold every i64, so the JS-to-wasm direction must go through ToBigInt and
 *    THROW for a Number rather than round one.  QuickJS installs BigInt
 *    unconditionally in JS_NewContext (verified, not assumed -- it is not
 *    behind CONFIG_BIGNUM here), so this is available rather than emulated.
 *
 * 3. AN EXCEPTION FROM AN IMPORTED JS FUNCTION IS NOT A TRAP.  It propagates
 *    out of the wasm frames as ITSELF -- not wrapped in a RuntimeError -- so
 *    `try { wasmFn() } catch (e) { e instanceof MyError }` holds.  The host
 *    callback returns WASM_TRAP_HOST to unwind the interpreter and sets
 *    `g_host_threw`, and the call primitive then hands QuickJS's already
 *    pending exception straight back.  Wrapping it would be a plausible,
 *    testable, wrong answer.
 *
 * ---- what is deliberately refused ----
 *
 * MVP only.  The decoder refuses every post-MVP proposal by name, so SIMD,
 * threads, reference types, GC, exceptions, tail calls, multi-memory and
 * wasm64 arrive here as a CompileError naming the feature rather than as a
 * mis-decode.  Above that, ONE limit is this file's own and is named where a
 * caller meets it: a funcref may not cross instances (see jw_tab_set_idx).
 *
 * The C half below is deliberately thin -- handles, byte copying, value
 * conversion and the calls into c/lib/wasm.  Everything with JavaScript
 * semantics in it (constructor shapes, `new.target`, getters, promise
 * ordering, the LinkError rules) lives in js_wasm_prelude.inc, which is the
 * convention js_websocket.c established here: those rules are much harder to
 * get right against the C API than they are to write in the language that
 * defines them.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "js_wasm.h"

/* ---- c/lib/wasm, TEXTUALLY, and that is a deliberate choice ----------------
 *
 * The interpreter is three .c files under c/lib/wasm, and they are #included
 * here rather than named in a source list.  The reason is the shape CLAUDE.md
 * lists as one of the five that silently break this tree's gates:
 * hand-copied source lists.
 *
 * `BROWSER_JS_SRC` is `$(wildcard c/apps/browser/js_*.c)`, so this file is
 * picked up automatically by NINE test fragments that each build "the browser
 * minus something" from their own subtraction over that wildcard -- and each
 * has its own narrow -I list and its own link line.  Adding the library to
 * $(BROWSER_PIPE) compiled and linked the browser itself perfectly and broke
 * test-canvas immediately: `fatal error: 'wasm.h' file not found`, because
 * that fragment's include path is five directories and c/lib/wasm is not one
 * of them.  Fixing that in nine fragments owned by nine other lines of work is
 * exactly the drift this arrangement exists to prevent, and every one of them
 * would have to be edited again the next time this directory grows a file.
 *
 * So the dependency travels WITH the file.  Anything that links js_wasm.c gets
 * a working WebAssembly; nothing has to know why.  It is the same argument
 * layout.c makes for #including layout_flex.c and layout_grid.c -- "a source
 * list is exactly the thing a measured line must not be able to edit" -- and
 * canvas.mk's own note that supplying js_websocket.c's dependencies beat
 * excluding js_websocket.c is the same instinct one step short of this.
 *
 * The three merge cleanly into one translation unit: checked, not assumed
 * (-Wall -Wextra, no redefinition and no warning).  They remain SEPARATE TUs
 * for their own gates -- tests/wasm.mk compiles them directly -- so the
 * freestanding claim is still actually tested rather than only asserted.
 *
 * BY RELATIVE PATH, for the reason include/weaksym.h gives at length: the
 * kernel's INCDIRS would resolve a bare basename, but every host gate builds
 * with its own narrow -I list, and those live in fragments this file does not
 * own. */
#include "../../lib/wasm/wasm.h"
#include "../../lib/wasm/wasm_exec.h"
#include "../../lib/wasm/wasm_parse.c"
#include "../../lib/wasm/wasm_valid.c"
#include "../../lib/wasm/wasm_exec.c"

/* ------------------------------------------------------------------ tables
 *
 * Handles are small integers.  Lifetime is a QuickJS finalizer on an opaque
 * "token" object the prelude keeps alive from each public object, because the
 * arenas below are malloc'd C memory the JS heap cannot see and a page that
 * compiles in a loop would otherwise leak every one of them. */

enum { JW_MOD = 1, JW_INST = 2, JW_MEM = 3, JW_TAB = 4 };

#define JW_MAX 128

struct jw_module {
	int used;
	struct wasm_module m;
	struct wasm_arena a;
	unsigned char *arena;
	uint8_t *bytes;            /* OUR copy: every span in `m` points into it */
	uint32_t len;
};

struct jw_hostctx {
	JSContext *ctx;
	JSValue fn;
	uint8_t nparams, nresults;
	uint8_t ptype[16], rtype[1];
};

struct jw_inst {
	int used;
	struct wasm_instance *in;
	struct wasm_arena a;
	unsigned char *arena;
	int modh;
	int memh;                  /* the memory this instance uses, or -1 */
	int tabh;                  /* likewise the table */
	struct wasm_hostimport *imps;
	uint32_t nimps;
	struct jw_hostctx *hcs;
	uint32_t nhcs;
	/* NUL-terminated copies of each import's (module, name), owned here.
	 * `imps[i].module/.name` point into this block, so it must outlive the
	 * hostimport array rather than being a scratch buffer -- find_import
	 * compares C strings and a shared buffer would make two imports in the
	 * same module resolve to whichever was copied last. */
	char *names;
};

struct jw_mem {
	int used;
	/* `mallocbase` is what we must free; `base` is where the bytes are NOW.
	 * They differ after a grow has moved the memory into an instance arena
	 * -- see the `memp` write-back in wasm_exec.h. */
	uint8_t *mallocbase;
	uint8_t *base;
	uint32_t pages, maxpages;
	int inst;                  /* >=0 once an instance is using this memory */
	JSValue buf;               /* the cached ArrayBuffer, or JS_UNDEFINED */
	uint8_t *bufbase;          /* what that ArrayBuffer was built over ... */
	uint32_t buflen;           /* ... and how long it was then */
};

struct jw_tab {
	int used;
	uint32_t *slots;
	uint32_t size, max;
	int inst;                  /* >=0 once an instance is using this table */
};

/* PREFIXED, and it is not style.  These live in the same translation unit as
 * c/lib/wasm now (see the #includes above), and wasm_valid.c already has a
 * file-static `g_mem` -- its opcode table.  The collision was a hard error
 * with a misleading first symptom: twenty lines of Apple's fortified
 * memset/memcpy macros, because the redefinition derailed the parse.  A merged
 * TU makes every file-static in it a shared namespace. */
static struct jw_module g_jwmod[JW_MAX];
static struct jw_inst   g_jwinst[JW_MAX];
static struct jw_mem    g_jwmem[JW_MAX];
static struct jw_tab    g_jwtab[JW_MAX];

static JSClassID g_tok_class;
static int g_host_threw;          /* see rule 3 in the header */

struct jw_tok { uint8_t kind; int idx; };

/* ---------------------------------------------------------------- helpers */

static JSValue jw_errobj(JSContext *ctx, const char *msg)
{
	JSValue o = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, o, "err", JS_NewString(ctx, msg));
	return o;
}

/* An error object that also says WHOSE fault it was.  WASM_E_NOMEM /
 * WASM_E_LIMIT / WASM_E_UNSUPPORTED are statements about THIS implementation,
 * not verdicts about the module -- wasm.h draws that line and
 * wasm_err_is_rejection() is its single spelling.  Reporting an exhausted
 * arena as "your module is malformed" is the apparatus lying about the
 * subject, which is the first rule in CLAUDE.md. */
static JSValue jw_errobj_code(JSContext *ctx, int code, const char *what)
{
	char buf[192];
	const char *s = wasm_errstr(code);
	if (wasm_err_is_rejection(code))
		snprintf(buf, sizeof buf, "%s: %s", what, s);
	else
		snprintf(buf, sizeof buf, "%s: %s (this is a limit of THIS "
		         "implementation, not a defect in the module)", what, s);
	return jw_errobj(ctx, buf);
}

static int jw_slot_mod(void)  { int i; for (i=0;i<JW_MAX;i++) if(!g_jwmod[i].used)  return i; return -1; }
static int jw_slot_inst(void) { int i; for (i=0;i<JW_MAX;i++) if(!g_jwinst[i].used) return i; return -1; }
static int jw_slot_mem(void)  { int i; for (i=0;i<JW_MAX;i++) if(!g_jwmem[i].used)  return i; return -1; }
static int jw_slot_tab(void)  { int i; for (i=0;i<JW_MAX;i++) if(!g_jwtab[i].used)  return i; return -1; }

static void jw_free_mod(int h)
{
	if (h < 0 || h >= JW_MAX || !g_jwmod[h].used) return;
	free(g_jwmod[h].arena); free(g_jwmod[h].bytes);
	memset(&g_jwmod[h], 0, sizeof g_jwmod[h]);
}

static void jw_free_inst(JSRuntime *rt, int h)
{
	uint32_t i;
	if (h < 0 || h >= JW_MAX || !g_jwinst[h].used) return;
	for (i = 0; i < g_jwinst[h].nhcs; i++)
		JS_FreeValueRT(rt, g_jwinst[h].hcs[i].fn);
	free(g_jwinst[h].hcs);
	free(g_jwinst[h].imps);
	free(g_jwinst[h].names);
	free(g_jwinst[h].arena);
	memset(&g_jwinst[h], 0, sizeof g_jwinst[h]);
}

static void jw_free_mem(JSRuntime *rt, int h)
{
	if (h < 0 || h >= JW_MAX || !g_jwmem[h].used) return;
	if (!JS_IsUndefined(g_jwmem[h].buf)) JS_FreeValueRT(rt, g_jwmem[h].buf);
	free(g_jwmem[h].mallocbase);
	memset(&g_jwmem[h], 0, sizeof g_jwmem[h]);
	g_jwmem[h].buf = JS_UNDEFINED;
}

static void jw_free_tab(int h)
{
	if (h < 0 || h >= JW_MAX || !g_jwtab[h].used) return;
	free(g_jwtab[h].slots);
	memset(&g_jwtab[h], 0, sizeof g_jwtab[h]);
}

static void jw_tok_finalizer(JSRuntime *rt, JSValue v)
{
	struct jw_tok *t = JS_GetOpaque(v, g_tok_class);
	if (!t) return;
	switch (t->kind) {
	case JW_MOD:  jw_free_mod(t->idx); break;
	case JW_INST: jw_free_inst(rt, t->idx); break;
	case JW_MEM:  jw_free_mem(rt, t->idx); break;
	case JW_TAB:  jw_free_tab(t->idx); break;
	default: break;
	}
	js_free_rt(rt, t);
}

static JSClassDef g_tok_def = { "WasmHandle", jw_tok_finalizer, NULL, NULL, NULL };

static JSValue jw_token(JSContext *ctx, uint8_t kind, int idx)
{
	JSValue o = JS_NewObjectClass(ctx, (int)g_tok_class);
	struct jw_tok *t;
	if (JS_IsException(o)) return o;
	t = js_malloc(ctx, sizeof *t);
	if (!t) { JS_FreeValue(ctx, o); return JS_EXCEPTION; }
	t->kind = kind; t->idx = idx;
	JS_SetOpaque(o, t);
	return o;
}

/* ------------------------------------------------------- value conversion */

static const char *jw_tname(uint8_t vt)
{
	switch (vt) {
	case WASM_VT_I32: return "i32";
	case WASM_VT_I64: return "i64";
	case WASM_VT_F32: return "f32";
	case WASM_VT_F64: return "f64";
	default: return "?";
	}
}

static uint8_t jw_tcode(const char *s)
{
	if (!s) return 0;
	if (!strcmp(s, "i32")) return WASM_VT_I32;
	if (!strcmp(s, "i64")) return WASM_VT_I64;
	if (!strcmp(s, "f32")) return WASM_VT_F32;
	if (!strcmp(s, "f64")) return WASM_VT_F64;
	return 0;
}

/* ToWebAssemblyValue.  Returns 0 on success, -1 with a pending exception.
 *
 * i64 goes through JS_ToBigInt64 and NOT through a double: a Number cannot
 * represent every i64, so accepting one would silently round the caller's
 * argument.  Throwing is the specified behaviour and it is also the only
 * honest one. */
static int jw_to_wasm(JSContext *ctx, uint8_t vt, JSValueConst v, union wasm_val *out)
{
	out->bits = 0;
	switch (vt) {
	case WASM_VT_I32: {
		int32_t x;
		if (JS_ToInt32(ctx, &x, v)) return -1;
		out->i32 = (uint32_t)x;
		return 0;
	}
	case WASM_VT_I64: {
		int64_t x;
#ifdef JS_WASM_NEGCTL_I64
		/* NEGATIVE CONTROL, and it is the implementation somebody would
		 * actually write: take the i64 through a double, so a Number is
		 * accepted "helpfully" instead of throwing.  Every small value still
		 * round-trips perfectly -- which is exactly why this has to be
		 * watched failing rather than reasoned about. */
		double d;
		if (JS_ToFloat64(ctx, &d, v)) return -1;
		x = (int64_t)d;
#else
		if (JS_ToBigInt64(ctx, &x, v)) return -1;
#endif
		out->i64 = (uint64_t)x;
		return 0;
	}
	case WASM_VT_F32: {
		double d;
		if (JS_ToFloat64(ctx, &d, v)) return -1;
		out->bits = 0;
		out->f32 = (float)d;      /* the narrowing IS the conversion */
		return 0;
	}
	case WASM_VT_F64: {
		double d;
		if (JS_ToFloat64(ctx, &d, v)) return -1;
		out->f64 = d;
		return 0;
	}
	default:
		JS_ThrowTypeError(ctx, "WebAssembly: unsupported value type");
		return -1;
	}
}

/* ToJSValue.  i32 is SIGNED on the way out -- the specification says so, and
 * an unsigned answer differs for exactly half of the range while looking
 * perfectly reasonable for the other half. */
static JSValue jw_to_js(JSContext *ctx, uint8_t vt, union wasm_val v)
{
	switch (vt) {
	case WASM_VT_I32: return JS_NewInt32(ctx, (int32_t)v.i32);
	case WASM_VT_I64: return JS_NewBigInt64(ctx, (int64_t)v.i64);
	case WASM_VT_F32: return JS_NewFloat64(ctx, (double)v.f32);
	case WASM_VT_F64: return JS_NewFloat64(ctx, v.f64);
	default: return JS_UNDEFINED;
	}
}

/* ------------------------------------------------------------ memory sync
 *
 * The one function that keeps the JavaScript view honest.  See rule 1 in the
 * header: the base moves under us, and the specification's detach is what
 * turns a stale read into a thrown TypeError. */

static void jw_mem_live(int h, uint8_t **base, uint32_t *len)
{
	struct jw_mem *m = &g_jwmem[h];
	if (m->inst >= 0 && g_jwinst[m->inst].used && g_jwinst[m->inst].in) {
		uint32_t n = 0;
		uint8_t *b = wasm_mem_bytes(g_jwinst[m->inst].in, &n);
		*base = b; *len = n;
		m->base = b;
		m->pages = wasm_mem_pages(g_jwinst[m->inst].in);
		return;
	}
	*base = m->base;
	*len = m->pages * 65536u;
}

/* Detach the cached ArrayBuffer if the memory has moved or resized since it
 * was handed out.  Idempotent, and cheap enough to run after every call. */
static void jw_mem_sync(JSContext *ctx, int h)
{
	uint8_t *base; uint32_t len;
	if (h < 0 || h >= JW_MAX || !g_jwmem[h].used) return;
	jw_mem_live(h, &base, &len);
	if (JS_IsUndefined(g_jwmem[h].buf)) return;
	if (base == g_jwmem[h].bufbase && len == g_jwmem[h].buflen) return;
#ifdef JS_WASM_NEGCTL_DETACH
	/* NEGATIVE CONTROL: notice that the memory moved and DO NOT detach --
	 * just drop our cache so the next `.buffer` builds a fresh, correct one.
	 * That reads as an optimisation and passes every check about current
	 * contents; what it loses is the view a page took BEFORE the grow, which
	 * silently keeps reading the pre-grow block. */
	JS_FreeValue(ctx, g_jwmem[h].buf);
	g_jwmem[h].buf = JS_UNDEFINED;
	g_jwmem[h].bufbase = NULL;
	g_jwmem[h].buflen = 0;
	return;
#endif
	JS_DetachArrayBuffer(ctx, g_jwmem[h].buf);
	JS_FreeValue(ctx, g_jwmem[h].buf);
	g_jwmem[h].buf = JS_UNDEFINED;
	g_jwmem[h].bufbase = NULL;
	g_jwmem[h].buflen = 0;
}

/* --------------------------------------------------------- host functions */

static int jw_hostcall(void *vctx, const union wasm_val *args, union wasm_val *rets)
{
	struct jw_hostctx *hc = (struct jw_hostctx *)vctx;
	JSContext *ctx = hc->ctx;
	JSValue argv[16], r;
	uint32_t i;
	int rc = WASM_TRAP_NONE;

	for (i = 0; i < hc->nparams; i++)
		argv[i] = jw_to_js(ctx, hc->ptype[i], args[i]);

	r = JS_Call(ctx, hc->fn, JS_UNDEFINED, (int)hc->nparams, (JSValueConst *)argv);

	for (i = 0; i < hc->nparams; i++) JS_FreeValue(ctx, argv[i]);

	if (JS_IsException(r)) {
		/* Leave QuickJS's exception PENDING and unwind the interpreter.
		 * jw_call hands the same exception back untouched, so a JS error
		 * thrown by an import reaches the page as itself (header, rule 3). */
		g_host_threw = 1;
		return WASM_TRAP_HOST;
	}
	if (hc->nresults) {
		if (jw_to_wasm(ctx, hc->rtype[0], r, &rets[0])) {
			g_host_threw = 1;      /* the CONVERSION threw; same path */
			rc = WASM_TRAP_HOST;
		}
	}
	JS_FreeValue(ctx, r);
	return rc;
}

/* ------------------------------------------------------------- primitives */

static uint8_t *jw_bufsrc(JSContext *ctx, JSValueConst v, uint32_t *len)
{
	size_t n = 0;
	uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
	if (!p) return NULL;
	*len = (uint32_t)n;
	return p;
}

static JSValue jw_validate(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
	uint32_t len = 0;
	uint8_t *p;
	unsigned char *arena;
	struct wasm_module m;
	struct wasm_arena a;
	int e;
	(void)this_val; (void)argc;

	p = jw_bufsrc(ctx, argv[0], &len);
	if (!p) return JS_EXCEPTION;
	arena = malloc(8u << 20);
	if (!arena) return JS_ThrowOutOfMemory(ctx);
	wasm_arena_init(&a, arena, 8u << 20);
	/* The bytes are only read during the load, so validate needs no copy. */
	e = wasm_load(p, len, &m, &a);
	free(arena);
	/* WASM_E_NOMEM / WASM_E_LIMIT / WASM_E_UNSUPPORTED are OUR limits.  A
	 * `validate` that answered false for them would be reporting a module
	 * this implementation cannot judge as one the specification rejects --
	 * the same conflation wasm.h refuses at the layer below.  False is still
	 * the only answer the API can give, so the distinction is logged rather
	 * than lost. */
	if (e && !wasm_err_is_rejection(e))
		printf("[wasm] validate: not a verdict about the module: %s\n",
		       wasm_errstr(e));
	return JS_NewBool(ctx, e == WASM_OK);
}

static JSValue jw_compile(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
	uint32_t len = 0;
	uint8_t *p;
	int h, e;
	uint32_t asz;
	JSValue o;
	(void)this_val; (void)argc;

	p = jw_bufsrc(ctx, argv[0], &len);
	if (!p) return JS_EXCEPTION;
	h = jw_slot_mod();
	if (h < 0) return jw_errobj(ctx, "too many live WebAssembly.Module objects "
	                            "(a limit of THIS implementation)");

	memset(&g_jwmod[h], 0, sizeof g_jwmod[h]);
	/* OUR OWN COPY, and it is not an optimisation to skip it: a
	 * `struct wasm_module` is a set of SPANS INTO THE CALLER'S BUFFER --
	 * every function body, every constant expression and every export name
	 * is an offset.  Holding the JS ArrayBuffer's pointer would dangle the
	 * moment that buffer was collected or detached. */
	g_jwmod[h].bytes = malloc(len ? len : 1);
	if (!g_jwmod[h].bytes) return JS_ThrowOutOfMemory(ctx);
	memcpy(g_jwmod[h].bytes, p, len);
	g_jwmod[h].len = len;

	asz = len * 48u + (1u << 20);
	if (asz > (48u << 20)) asz = 48u << 20;
	g_jwmod[h].arena = malloc(asz);
	if (!g_jwmod[h].arena) { free(g_jwmod[h].bytes); return JS_ThrowOutOfMemory(ctx); }
	wasm_arena_init(&g_jwmod[h].a, g_jwmod[h].arena, asz);

	e = wasm_load(g_jwmod[h].bytes, len, &g_jwmod[h].m, &g_jwmod[h].a);
	if (e) {
		free(g_jwmod[h].arena); free(g_jwmod[h].bytes);
		memset(&g_jwmod[h], 0, sizeof g_jwmod[h]);
		return jw_errobj_code(ctx, e, "WebAssembly.Module");
	}
	g_jwmod[h].used = 1;

	o = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, o, "h", JS_NewInt32(ctx, h));
	JS_SetPropertyStr(ctx, o, "tok", jw_token(ctx, JW_MOD, h));
	return o;
}

static JSValue jw_name(JSContext *ctx, const struct jw_module *M, struct wasm_span s)
{
	return JS_NewStringLen(ctx, (const char *)M->bytes + s.off, s.len);
}

static const char *jw_kindname(uint8_t k)
{
	switch (k) {
	case WASM_EXT_FUNC:   return "function";
	case WASM_EXT_TABLE:  return "table";
	case WASM_EXT_MEM:    return "memory";
	case WASM_EXT_GLOBAL: return "global";
	default: return "?";
	}
}

static JSValue jw_sigobj(JSContext *ctx, const struct wasm_functype *ft)
{
	JSValue o = JS_NewObject(ctx), p = JS_NewArray(ctx), r = JS_NewArray(ctx);
	uint32_t i;
	for (i = 0; i < ft->nparams; i++)
		JS_SetPropertyUint32(ctx, p, i, JS_NewString(ctx, jw_tname(ft->params[i])));
	for (i = 0; i < ft->nresults; i++)
		JS_SetPropertyUint32(ctx, r, i, JS_NewString(ctx, jw_tname(ft->results[i])));
	JS_SetPropertyStr(ctx, o, "params", p);
	JS_SetPropertyStr(ctx, o, "results", r);
	return o;
}

static JSValue jw_modinfo(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
	int h; uint32_t i;
	struct jw_module *M;
	JSValue o, imps, exps;
	uint32_t nf = 0, nt = 0, nm = 0, ng = 0;
	(void)this_val; (void)argc;

	if (JS_ToInt32(ctx, &h, argv[0])) return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwmod[h].used)
		return JS_ThrowInternalError(ctx, "wasm: stale module handle");
	M = &g_jwmod[h];

	imps = JS_NewArray(ctx);
	for (i = 0; i < M->m.nimports; i++) {
		const struct wasm_import *im = &M->m.imports[i];
		JSValue d = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, d, "module", jw_name(ctx, M, im->module_name));
		JS_SetPropertyStr(ctx, d, "name",   jw_name(ctx, M, im->field_name));
		JS_SetPropertyStr(ctx, d, "kind",   JS_NewString(ctx, jw_kindname(im->kind)));
		switch (im->kind) {
		case WASM_EXT_FUNC:
			JS_SetPropertyStr(ctx, d, "sig",
			                  jw_sigobj(ctx, &M->m.types[im->typeidx]));
			JS_SetPropertyStr(ctx, d, "idx", JS_NewInt32(ctx, (int)nf++));
			break;
		case WASM_EXT_GLOBAL:
			JS_SetPropertyStr(ctx, d, "vt", JS_NewString(ctx, jw_tname(im->gt.valtype)));
			JS_SetPropertyStr(ctx, d, "mutable", JS_NewBool(ctx, im->gt.mut != 0));
			JS_SetPropertyStr(ctx, d, "idx", JS_NewInt32(ctx, (int)ng++));
			break;
		case WASM_EXT_MEM:
			JS_SetPropertyStr(ctx, d, "min", JS_NewUint32(ctx, im->mem.min));
			if (im->mem.has_max)
				JS_SetPropertyStr(ctx, d, "max", JS_NewUint32(ctx, im->mem.max));
			JS_SetPropertyStr(ctx, d, "idx", JS_NewInt32(ctx, (int)nm++));
			break;
		case WASM_EXT_TABLE:
			JS_SetPropertyStr(ctx, d, "min", JS_NewUint32(ctx, im->tt.lim.min));
			if (im->tt.lim.has_max)
				JS_SetPropertyStr(ctx, d, "max", JS_NewUint32(ctx, im->tt.lim.max));
			JS_SetPropertyStr(ctx, d, "idx", JS_NewInt32(ctx, (int)nt++));
			break;
		default: break;
		}
		JS_SetPropertyUint32(ctx, imps, i, d);
	}

	exps = JS_NewArray(ctx);
	for (i = 0; i < M->m.nexports; i++) {
		const struct wasm_export *ex = &M->m.exports[i];
		JSValue d = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, d, "name", jw_name(ctx, M, ex->name));
		JS_SetPropertyStr(ctx, d, "kind", JS_NewString(ctx, jw_kindname(ex->kind)));
		JS_SetPropertyStr(ctx, d, "index", JS_NewUint32(ctx, ex->index));
		if (ex->kind == WASM_EXT_GLOBAL && ex->index < M->m.total_globals) {
			JS_SetPropertyStr(ctx, d, "vt",
			    JS_NewString(ctx, jw_tname(M->m.globaltype_of[ex->index].valtype)));
			JS_SetPropertyStr(ctx, d, "mutable",
			    JS_NewBool(ctx, M->m.globaltype_of[ex->index].mut != 0));
		}
		JS_SetPropertyUint32(ctx, exps, i, d);
	}

	o = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, o, "imports", imps);
	JS_SetPropertyStr(ctx, o, "exports", exps);
	JS_SetPropertyStr(ctx, o, "hasMemory",
	                  JS_NewBool(ctx, M->m.total_mems > 0));
	JS_SetPropertyStr(ctx, o, "hasTable",
	                  JS_NewBool(ctx, M->m.total_tables > 0));
	return o;
}

/* A NUL-terminated copy of a module name, for find_import's C-string compare.
 * A name containing an embedded NUL therefore fails to match and the module
 * gets a LinkError -- wrong, but LOUDLY wrong, which is the side of this to
 * be on.  It is noted rather than silently tolerated. */
static void jw_cstr(char *dst, size_t cap, const struct jw_module *M,
                    struct wasm_span s)
{
	size_t n = s.len < cap - 1 ? s.len : cap - 1;
	memcpy(dst, M->bytes + s.off, n);
	dst[n] = 0;
}

static JSValue jw_instantiate(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
	int modh, h;
	uint32_t i, nimp, asz64;
	struct jw_module *M;
	struct jw_inst *I;
	uint64_t asz;
	uint32_t minp = 0, maxp = 0;
	int e, newmem = 0, newtab = 0;
	JSValue o;
	(void)this_val; (void)argc;

	if (JS_ToInt32(ctx, &modh, argv[0])) return JS_EXCEPTION;
	if (modh < 0 || modh >= JW_MAX || !g_jwmod[modh].used)
		return JS_ThrowInternalError(ctx, "wasm: stale module handle");
	M = &g_jwmod[modh];

	h = jw_slot_inst();
	if (h < 0) return jw_errobj(ctx, "too many live WebAssembly.Instance objects "
	                            "(a limit of THIS implementation)");
	I = &g_jwinst[h];
	memset(I, 0, sizeof *I);
	I->modh = modh; I->memh = -1; I->tabh = -1;

	nimp = M->m.nimports;
	I->imps  = nimp ? calloc(nimp, sizeof *I->imps) : NULL;
	I->hcs   = nimp ? calloc(nimp, sizeof *I->hcs)  : NULL;
	I->names = nimp ? calloc(nimp, 256)             : NULL;
	if (nimp && (!I->imps || !I->hcs || !I->names)) {
		free(I->imps); free(I->hcs); free(I->names);
		return JS_ThrowOutOfMemory(ctx);
	}
	I->nimps = nimp;

	for (i = 0; i < nimp; i++) {
		const struct wasm_import *im = &M->m.imports[i];
		struct wasm_hostimport *hi = &I->imps[i];
		JSValue d = JS_GetPropertyUint32(ctx, argv[1], i);
		int k = 0;

		/* The module's OWN name bytes, so the match in find_import cannot
		 * disagree with what the module asked for.  Per-import storage, out
		 * of the block this instance owns. */
		{
			char *slot = I->names + (size_t)i * 256;
			jw_cstr(slot,       128, M, im->module_name);
			jw_cstr(slot + 128, 128, M, im->field_name);
			hi->module = slot;
			hi->name   = slot + 128;
		}

		{
			JSValue kv = JS_GetPropertyStr(ctx, d, "k");
			JS_ToInt32(ctx, &k, kv);
			JS_FreeValue(ctx, kv);
		}
		switch (k) {
		case 0: {                       /* an imported JS function */
			struct jw_hostctx *hc = &I->hcs[I->nhcs++];
			const struct wasm_functype *ft = &M->m.types[im->typeidx];
			uint32_t q;
			/* REFUSED, not truncated.  Clamping to 16 would call the page's
			 * function with the first sixteen arguments and no indication
			 * that the rest were dropped -- a plausible wrong answer, which
			 * is the one thing this file may not produce. */
			if (ft->nparams > 16) {
				JS_FreeValue(ctx, d);
				free(I->imps); free(I->hcs); free(I->names);
				return jw_errobj(ctx, "an imported function of more than 16 "
				                 "parameters is a limit of THIS implementation");
			}
			hc->ctx = ctx;
			hc->fn = JS_GetPropertyStr(ctx, d, "f");
			hc->nparams = (uint8_t)ft->nparams;
			hc->nresults = (uint8_t)(ft->nresults ? 1 : 0);
			for (q = 0; q < hc->nparams; q++) hc->ptype[q] = ft->params[q];
			if (hc->nresults) hc->rtype[0] = ft->results[0];
			hi->kind = WASM_IMP_FUNC;
			hi->fn = jw_hostcall;
			hi->ctx = hc;
			break;
		}
		case 1: {                       /* an imported global */
			JSValue tv = JS_GetPropertyStr(ctx, d, "t");
			JSValue vv = JS_GetPropertyStr(ctx, d, "v");
			const char *ts = JS_ToCString(ctx, tv);
			hi->kind = WASM_IMP_GLOBAL;
			hi->gtype = jw_tcode(ts);
			hi->gmut = im->gt.mut;
			if (jw_to_wasm(ctx, hi->gtype ? hi->gtype : im->gt.valtype,
			               vv, &hi->gval)) {
				if (ts) JS_FreeCString(ctx, ts);
				JS_FreeValue(ctx, tv); JS_FreeValue(ctx, vv); JS_FreeValue(ctx, d);
				free(I->imps); free(I->hcs); free(I->names);
				return JS_EXCEPTION;
			}
			if (ts) JS_FreeCString(ctx, ts);
			JS_FreeValue(ctx, tv); JS_FreeValue(ctx, vv);
			break;
		}
		case 2: {                       /* an imported WebAssembly.Memory */
			int mh = -1;
			JSValue hv = JS_GetPropertyStr(ctx, d, "h");
			JS_ToInt32(ctx, &mh, hv);
			JS_FreeValue(ctx, hv);
			if (mh < 0 || mh >= JW_MAX || !g_jwmem[mh].used) {
				JS_FreeValue(ctx, d);
				free(I->imps); free(I->hcs); free(I->names);
				return jw_errobj(ctx, "imported memory is not a live "
				                 "WebAssembly.Memory");
			}
			hi->kind = WASM_IMP_MEMORY;
			hi->mem = g_jwmem[mh].base;
			hi->mpages = &g_jwmem[mh].pages;
			hi->mmaxpages = &g_jwmem[mh].maxpages;
			/* THE WRITE-BACK, and it is the whole reason wasm_exec.h grew
			 * this field: a `memory.grow` inside the module relocates the
			 * linear memory, and without this the Memory object would keep
			 * handing out an ArrayBuffer over the pre-grow block. */
			hi->memp = &g_jwmem[mh].base;
			I->memh = mh;
			g_jwmem[mh].inst = h;
			break;
		}
		case 3: {                       /* an imported WebAssembly.Table */
			int th = -1;
			JSValue hv = JS_GetPropertyStr(ctx, d, "h");
			JS_ToInt32(ctx, &th, hv);
			JS_FreeValue(ctx, hv);
			if (th < 0 || th >= JW_MAX || !g_jwtab[th].used) {
				JS_FreeValue(ctx, d);
				free(I->imps); free(I->hcs); free(I->names);
				return jw_errobj(ctx, "imported table is not a live "
				                 "WebAssembly.Table");
			}
			hi->kind = WASM_IMP_TABLE;
			hi->table = g_jwtab[th].slots;
			hi->tsize = &g_jwtab[th].size;
			hi->tmax = &g_jwtab[th].max;
			I->tabh = th;
			g_jwtab[th].inst = h;
			break;
		}
		default:
			JS_FreeValue(ctx, d);
			free(I->imps); free(I->hcs); free(I->names);
			return jw_errobj(ctx, "unsupported import kind");
		}
		JS_FreeValue(ctx, d);
	}

	/* Arena size.  Linear memory dominates, and growth is allocate-and-copy,
	 * so a module that grows in many small steps needs headroom rather than
	 * exactly its maximum.  Three times the ceiling is the compromise; when
	 * it is not enough the failure is REPORTED as ours (wasm_apparatus_count)
	 * rather than passed off as the module's. */
	if (M->m.nmems) { minp = M->m.mems[0].min;
	                  maxp = M->m.mems[0].has_max ? M->m.mems[0].max : minp + 16; }
	if (maxp > 512u) maxp = 512u;
	if (maxp < minp) maxp = minp;
	asz = (uint64_t)(2u << 20) + (uint64_t)maxp * 65536u * 3u
	    + (uint64_t)M->len * 8u;
	if (asz > (160ull << 20)) asz = 160ull << 20;
	asz64 = (uint32_t)asz;
	I->arena = malloc(asz64);
	if (!I->arena) { free(I->imps); free(I->hcs); free(I->names); return JS_ThrowOutOfMemory(ctx); }
	wasm_arena_init(&I->a, I->arena, asz64);

	e = wasm_instantiate(&I->in, &M->m, &I->a, I->imps, nimp);
	if (e) {
		JSValue r;
		char buf[224];
		int link = (e == WASM_TRAP_UNLINKABLE);
		if (e == WASM_TRAP_NOMEM || e == WASM_TRAP_UNIMPLEMENTED
		    || e == WASM_TRAP_DESYNC)
			snprintf(buf, sizeof buf, "WebAssembly.Instance: %s (this is a "
			         "limit of THIS implementation, not a defect in the "
			         "module)", wasm_trapstr(e));
		else
			snprintf(buf, sizeof buf, "WebAssembly.Instance: %s", wasm_trapstr(e));
		for (i = 0; i < I->nhcs; i++) JS_FreeValueRT(JS_GetRuntime(ctx), I->hcs[i].fn);
		free(I->arena); free(I->imps); free(I->hcs); free(I->names);
		memset(I, 0, sizeof *I);
		r = jw_errobj(ctx, buf);
		JS_SetPropertyStr(ctx, r, "link", JS_NewBool(ctx, link));
		return r;
	}
	I->used = 1;

	/* A memory the module DEFINED (rather than imported) still has to be
	 * reachable as a WebAssembly.Memory if it is exported, and it lives in
	 * this instance's arena -- so the handle carries no storage of its own
	 * and reads through the instance every time.  That is what makes it
	 * correct across a grow for free. */
	if (I->memh < 0 && M->m.total_mems > 0) {
		int mh = jw_slot_mem();
		newmem = 1;
		if (mh >= 0) {
			memset(&g_jwmem[mh], 0, sizeof g_jwmem[mh]);
			g_jwmem[mh].used = 1;
			g_jwmem[mh].buf = JS_UNDEFINED;
			g_jwmem[mh].inst = h;
			g_jwmem[mh].mallocbase = NULL;
			g_jwmem[mh].pages = wasm_mem_pages(I->in);
			g_jwmem[mh].maxpages = M->m.nmems && M->m.mems[0].has_max
			                   ? M->m.mems[0].max : 65536u;
			I->memh = mh;
		}
	}
	if (I->tabh < 0 && M->m.total_tables > 0) {
		int th = jw_slot_tab();
		newtab = 1;
		if (th >= 0) {
			memset(&g_jwtab[th], 0, sizeof g_jwtab[th]);
			g_jwtab[th].used = 1;
			g_jwtab[th].slots = NULL;      /* lives in the instance */
			g_jwtab[th].inst = h;
			g_jwtab[th].size = wasm_table_size(I->in);
			g_jwtab[th].max = M->m.ntables && M->m.tables[0].lim.has_max
			              ? M->m.tables[0].lim.max : 0xFFFFFFFFu;
			I->tabh = th;
		}
	}

	o = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, o, "h", JS_NewInt32(ctx, h));
	JS_SetPropertyStr(ctx, o, "tok", jw_token(ctx, JW_INST, h));
	JS_SetPropertyStr(ctx, o, "memh", JS_NewInt32(ctx, I->memh));
	JS_SetPropertyStr(ctx, o, "tabh", JS_NewInt32(ctx, I->tabh));
	/* A TOKEN IS OWNERSHIP, SO IT IS MINTED ONLY FOR A SLOT CREATED HERE.
	 * Minting one for an IMPORTED memory was a real bug and an instructive
	 * one: the page's WebAssembly.Memory already holds a token for that slot,
	 * this object is a temporary the prelude drops immediately, and its
	 * finalizer then freed the memory out from under the still-live Memory
	 * object.  The symptom was not a crash -- it was `m.buffer` answering
	 * "stale memory handle" several checks later, and an import failing the
	 * minimum-size test because the slot it named was gone.  Two owners of one
	 * handle, which is CLAUDE.md's "one jar, two doors" wearing a finalizer. */
	if (I->memh >= 0 && newmem)
		JS_SetPropertyStr(ctx, o, "memtok", jw_token(ctx, JW_MEM, I->memh));
	if (I->tabh >= 0 && newtab)
		JS_SetPropertyStr(ctx, o, "tabtok", jw_token(ctx, JW_TAB, I->tabh));
	return o;
}

static JSValue jw_sig(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
	int h, fidx;
	const struct wasm_functype *ft;
	(void)this_val; (void)argc;
	if (JS_ToInt32(ctx, &h, argv[0])) return JS_EXCEPTION;
	if (JS_ToInt32(ctx, &fidx, argv[1])) return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwinst[h].used)
		return JS_ThrowInternalError(ctx, "wasm: stale instance handle");
	ft = wasm_func_signature(g_jwinst[h].in, (uint32_t)fidx);
	if (!ft) return JS_ThrowInternalError(ctx, "wasm: no such function");
	return jw_sigobj(ctx, ft);
}

static JSValue jw_call(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
	int h, fidx;
	uint32_t i;
	const struct wasm_functype *ft;
	union wasm_val args[16], rets[1];
	int trap;
	JSValue o;
	(void)this_val; (void)argc;

	if (JS_ToInt32(ctx, &h, argv[0])) return JS_EXCEPTION;
	if (JS_ToInt32(ctx, &fidx, argv[1])) return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwinst[h].used)
		return JS_ThrowInternalError(ctx, "wasm: stale instance handle");
	ft = wasm_func_signature(g_jwinst[h].in, (uint32_t)fidx);
	if (!ft) return JS_ThrowInternalError(ctx, "wasm: no such function");
	if (ft->nparams > 16)
		return JS_ThrowInternalError(ctx, "wasm: more than 16 parameters is a "
		                             "limit of THIS implementation");

	for (i = 0; i < ft->nparams; i++) {
		JSValue a = JS_GetPropertyUint32(ctx, argv[2], i);
		int bad = jw_to_wasm(ctx, ft->params[i], a, &args[i]);
		JS_FreeValue(ctx, a);
		if (bad) return JS_EXCEPTION;      /* e.g. a Number where i64 is due */
	}

	g_host_threw = 0;
	trap = wasm_invoke(g_jwinst[h].in, (uint32_t)fidx, args, rets);

	/* The memory may have MOVED during that call -- `memory.grow` is an
	 * ordinary instruction.  Detach before anything can read a stale view;
	 * see rule 1 in the header. */
	if (g_jwinst[h].memh >= 0) jw_mem_sync(ctx, g_jwinst[h].memh);
	if (g_jwinst[h].tabh >= 0 && g_jwinst[h].in)
		g_jwtab[g_jwinst[h].tabh].size = wasm_table_size(g_jwinst[h].in);

	if (trap != WASM_TRAP_NONE) {
		char buf[224];
		if (g_host_threw) {
			/* An imported JS function threw.  QuickJS's exception is still
			 * pending; hand it back untouched so the page catches ITS error
			 * rather than a RuntimeError wrapping it. */
			g_host_threw = 0;
			return JS_EXCEPTION;
		}
		if (!wasm_trap_is_module_fault(trap))
			snprintf(buf, sizeof buf, "%s (this is a limit of THIS "
			         "implementation, not a defect in the module)",
			         wasm_trapstr(trap));
		else
			snprintf(buf, sizeof buf, "%s", wasm_trapstr(trap));
		return jw_errobj(ctx, buf);
	}

	o = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, o, "v", ft->nresults
	                  ? jw_to_js(ctx, ft->results[0], rets[0])
	                  : JS_UNDEFINED);
	return o;
}

/* ---- globals ------------------------------------------------------------ */

static JSValue jw_gget(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
	int h, g;
	const char *t;
	uint8_t vt;
	JSValue r;
	(void)this_val; (void)argc;
	if (JS_ToInt32(ctx, &h, argv[0]) || JS_ToInt32(ctx, &g, argv[1]))
		return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwinst[h].used)
		return JS_ThrowInternalError(ctx, "wasm: stale instance handle");
	t = JS_ToCString(ctx, argv[2]);
	vt = jw_tcode(t);
	if (t) JS_FreeCString(ctx, t);
	r = jw_to_js(ctx, vt, wasm_global_get(g_jwinst[h].in, (uint32_t)g));
	return r;
}

static JSValue jw_gset(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
	int h, g;
	const char *t;
	uint8_t vt;
	union wasm_val v;
	(void)this_val; (void)argc;
	if (JS_ToInt32(ctx, &h, argv[0]) || JS_ToInt32(ctx, &g, argv[1]))
		return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwinst[h].used)
		return JS_ThrowInternalError(ctx, "wasm: stale instance handle");
	t = JS_ToCString(ctx, argv[2]);
	vt = jw_tcode(t);
	if (t) JS_FreeCString(ctx, t);
	if (jw_to_wasm(ctx, vt, argv[3], &v)) return JS_EXCEPTION;
	if (wasm_global_set(g_jwinst[h].in, (uint32_t)g, v))
		return JS_ThrowRangeError(ctx, "wasm: global index out of range");
	return JS_UNDEFINED;
}

/* ---- memory ------------------------------------------------------------- */

static JSValue jw_mem_new(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
	int64_t initial = 0, maximum = -1;
	int h;
	JSValue o;
	(void)this_val; (void)argc;
	if (JS_ToInt64(ctx, &initial, argv[0])) return JS_EXCEPTION;
	if (JS_ToInt64(ctx, &maximum, argv[1])) return JS_EXCEPTION;
	if (initial < 0 || initial > 65536)
		return jw_errobj(ctx, "WebAssembly.Memory: initial is out of range");
	if (maximum >= 0 && maximum < initial)
		return jw_errobj(ctx, "WebAssembly.Memory: maximum is less than initial");
	if (maximum > 65536)
		return jw_errobj(ctx, "WebAssembly.Memory: maximum is out of range");
	/* Our own ceiling, named as ours.  c/lib/wasm caps a linear memory at
	 * WASM_MEM_PAGES_CAP; a page asking for more gets a RangeError that says
	 * whose limit it is rather than a silently smaller memory. */
	if (initial > 512)
		return jw_errobj(ctx, "WebAssembly.Memory: this implementation caps a "
		                 "linear memory at 512 pages (32 MiB)");
	h = jw_slot_mem();
	if (h < 0) return jw_errobj(ctx, "too many live WebAssembly.Memory objects "
	                            "(a limit of THIS implementation)");
	memset(&g_jwmem[h], 0, sizeof g_jwmem[h]);
	g_jwmem[h].mallocbase = calloc(initial ? (size_t)initial : 1, 65536);
	if (!g_jwmem[h].mallocbase) return JS_ThrowOutOfMemory(ctx);
	g_jwmem[h].base = g_jwmem[h].mallocbase;
	g_jwmem[h].pages = (uint32_t)initial;
	g_jwmem[h].maxpages = maximum >= 0 ? (uint32_t)maximum : 65536u;
	g_jwmem[h].inst = -1;
	g_jwmem[h].buf = JS_UNDEFINED;
	g_jwmem[h].used = 1;

	o = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, o, "h", JS_NewInt32(ctx, h));
	JS_SetPropertyStr(ctx, o, "tok", jw_token(ctx, JW_MEM, h));
	return o;
}

static JSValue jw_mem_buffer(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
	int h;
	uint8_t *base; uint32_t len;
	(void)this_val; (void)argc;
	if (JS_ToInt32(ctx, &h, argv[0])) return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwmem[h].used)
		return JS_ThrowInternalError(ctx, "wasm: stale memory handle");
	jw_mem_sync(ctx, h);
	if (!JS_IsUndefined(g_jwmem[h].buf))
		return JS_DupValue(ctx, g_jwmem[h].buf);
	jw_mem_live(h, &base, &len);
	if (!base) return JS_ThrowInternalError(ctx, "wasm: memory has no storage");
	/* ALIASED, not copied -- an ArrayBuffer over the module's own linear
	 * memory is the entire point, and it is why the detach in jw_mem_sync is
	 * load-bearing rather than ceremonial.  No free_func: the bytes belong to
	 * the arena or to `mallocbase`, never to QuickJS. */
	g_jwmem[h].buf = JS_NewArrayBuffer(ctx, base, len, NULL, NULL, 0);
	g_jwmem[h].bufbase = base;
	g_jwmem[h].buflen = len;
	return JS_DupValue(ctx, g_jwmem[h].buf);
}

static JSValue jw_mem_pages_js(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
	int h;
	uint8_t *base; uint32_t len;
	(void)this_val; (void)argc;
	if (JS_ToInt32(ctx, &h, argv[0])) return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwmem[h].used) return JS_NewInt32(ctx, -1);
	jw_mem_live(h, &base, &len);
	return JS_NewUint32(ctx, g_jwmem[h].pages);
}

static JSValue jw_mem_grow(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
	int h; int64_t delta = 0;
	uint32_t old;
	(void)this_val; (void)argc;
	if (JS_ToInt32(ctx, &h, argv[0])) return JS_EXCEPTION;
	if (JS_ToInt64(ctx, &delta, argv[1])) return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwmem[h].used)
		return JS_ThrowInternalError(ctx, "wasm: stale memory handle");
	if (delta < 0 || delta > 65536) return JS_NewInt32(ctx, -1);

	if (g_jwmem[h].inst >= 0 && g_jwinst[g_jwmem[h].inst].used) {
		/* Through the interpreter, so that a JS grow and the memory.grow
		 * OPCODE move the same bytes and update the same page count.  A
		 * host-side realloc here would leave the instance on the old base --
		 * the same defect as `memp`, in the other direction. */
		if (wasm_mem_grow(g_jwinst[g_jwmem[h].inst].in, (uint32_t)delta, &old))
			return JS_NewInt32(ctx, -1);
	} else {
		uint64_t np = (uint64_t)g_jwmem[h].pages + (uint64_t)delta;
		uint8_t *nb;
		if (np > g_jwmem[h].maxpages || np > 512u) return JS_NewInt32(ctx, -1);
		nb = calloc((size_t)(np ? np : 1), 65536);
		if (!nb) return JS_NewInt32(ctx, -1);
		memcpy(nb, g_jwmem[h].base, (size_t)g_jwmem[h].pages * 65536u);
		free(g_jwmem[h].mallocbase);
		g_jwmem[h].mallocbase = nb;
		g_jwmem[h].base = nb;
		old = g_jwmem[h].pages;
		g_jwmem[h].pages = (uint32_t)np;
	}
	/* The buffer is now over the wrong bytes.  Detach it: that is what the
	 * specification requires AND what stops a view taken before the grow from
	 * quietly reading the pre-grow block. */
	jw_mem_sync(ctx, h);
	return JS_NewUint32(ctx, old);
}

/* ---- tables ------------------------------------------------------------- */

static JSValue jw_tab_new(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
	int64_t initial = 0, maximum = -1;
	int h; uint32_t i;
	JSValue o;
	(void)this_val; (void)argc;
	if (JS_ToInt64(ctx, &initial, argv[0])) return JS_EXCEPTION;
	if (JS_ToInt64(ctx, &maximum, argv[1])) return JS_EXCEPTION;
	if (initial < 0 || initial > 10000000)
		return jw_errobj(ctx, "WebAssembly.Table: initial is out of range");
	if (maximum >= 0 && maximum < initial)
		return jw_errobj(ctx, "WebAssembly.Table: maximum is less than initial");
	h = jw_slot_tab();
	if (h < 0) return jw_errobj(ctx, "too many live WebAssembly.Table objects "
	                            "(a limit of THIS implementation)");
	memset(&g_jwtab[h], 0, sizeof g_jwtab[h]);
	g_jwtab[h].slots = calloc((size_t)(initial ? initial : 1), sizeof(uint32_t));
	if (!g_jwtab[h].slots) return JS_ThrowOutOfMemory(ctx);
	for (i = 0; i < (uint32_t)initial; i++) g_jwtab[h].slots[i] = WASM_NOFUNC;
	g_jwtab[h].size = (uint32_t)initial;
	g_jwtab[h].max = maximum >= 0 ? (uint32_t)maximum : 0xFFFFFFFFu;
	g_jwtab[h].inst = -1;
	g_jwtab[h].used = 1;

	o = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, o, "h", JS_NewInt32(ctx, h));
	JS_SetPropertyStr(ctx, o, "tok", jw_token(ctx, JW_TAB, h));
	return o;
}

static JSValue jw_tab_len(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
	int h;
	(void)this_val; (void)argc;
	if (JS_ToInt32(ctx, &h, argv[0])) return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwtab[h].used) return JS_NewInt32(ctx, -1);
	if (g_jwtab[h].inst >= 0 && g_jwinst[g_jwtab[h].inst].used)
		g_jwtab[h].size = wasm_table_size(g_jwinst[g_jwtab[h].inst].in);
	return JS_NewUint32(ctx, g_jwtab[h].size);
}

/* Returns the FUNCTION INDEX in the slot, or -1 for a null slot; throws for an
 * index past the end (the specification's RangeError is raised in the
 * prelude). */
static JSValue jw_tab_get_idx(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
	int h; int64_t i = 0;
	uint32_t f = WASM_NOFUNC;
	(void)this_val; (void)argc;
	if (JS_ToInt32(ctx, &h, argv[0])) return JS_EXCEPTION;
	if (JS_ToInt64(ctx, &i, argv[1])) return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwtab[h].used)
		return JS_ThrowInternalError(ctx, "wasm: stale table handle");
	if (g_jwtab[h].inst >= 0 && g_jwinst[g_jwtab[h].inst].used) {
		if (i < 0 || wasm_table_get(g_jwinst[g_jwtab[h].inst].in, (uint32_t)i, &f))
			return JS_NewInt32(ctx, -2);      /* out of range */
	} else {
		if (i < 0 || (uint32_t)i >= g_jwtab[h].size) return JS_NewInt32(ctx, -2);
		f = g_jwtab[h].slots[i];
	}
	return f == WASM_NOFUNC ? JS_NewInt32(ctx, -1)
	                        : JS_NewUint32(ctx, f);
}

static JSValue jw_tab_set_idx(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
	int h; int64_t i = 0; int64_t f = -1;
	uint32_t fi;
	(void)this_val; (void)argc;
	if (JS_ToInt32(ctx, &h, argv[0])) return JS_EXCEPTION;
	if (JS_ToInt64(ctx, &i, argv[1])) return JS_EXCEPTION;
	if (JS_ToInt64(ctx, &f, argv[2])) return JS_EXCEPTION;
	if (h < 0 || h >= JW_MAX || !g_jwtab[h].used)
		return JS_ThrowInternalError(ctx, "wasm: stale table handle");
	fi = f < 0 ? WASM_NOFUNC : (uint32_t)f;
	if (g_jwtab[h].inst >= 0 && g_jwinst[g_jwtab[h].inst].used) {
		if (i < 0 || wasm_table_set(g_jwinst[g_jwtab[h].inst].in, (uint32_t)i, fi))
			return JS_NewInt32(ctx, -2);
	} else {
		if (i < 0 || (uint32_t)i >= g_jwtab[h].size) return JS_NewInt32(ctx, -2);
		g_jwtab[h].slots[i] = fi;
	}
	return JS_NewInt32(ctx, 0);
}

/* --------------------------------------------------------------- install */

static const JSCFunctionListEntry g_prims[] = {
	JS_CFUNC_DEF("validate",    1, jw_validate),
	JS_CFUNC_DEF("compile",     1, jw_compile),
	JS_CFUNC_DEF("modInfo",     1, jw_modinfo),
	JS_CFUNC_DEF("instantiate", 2, jw_instantiate),
	JS_CFUNC_DEF("sig",         2, jw_sig),
	JS_CFUNC_DEF("call",        3, jw_call),
	JS_CFUNC_DEF("gget",        3, jw_gget),
	JS_CFUNC_DEF("gset",        4, jw_gset),
	JS_CFUNC_DEF("memNew",      2, jw_mem_new),
	JS_CFUNC_DEF("memBuffer",   1, jw_mem_buffer),
	JS_CFUNC_DEF("memPages",    1, jw_mem_pages_js),
	JS_CFUNC_DEF("memGrow",     2, jw_mem_grow),
	JS_CFUNC_DEF("tabNew",      2, jw_tab_new),
	JS_CFUNC_DEF("tabLen",      1, jw_tab_len),
	JS_CFUNC_DEF("tabGet",      2, jw_tab_get_idx),
	JS_CFUNC_DEF("tabSet",      3, jw_tab_set_idx),
};

static const char JW_PRELUDE[] =
#include "js_wasm_prelude.inc"
;

void js_wasm_install(JSContext *ctx)
{
	JSRuntime *rt = JS_GetRuntime(ctx);
	JSValue g, fn, prims, r;
	int i;

	g = JS_GetGlobalObject(ctx);
	/* "Only if absent", the same rule js_platform.c states: something that
	 * already published a WebAssembly is a better answer than ours. */
	{
		JSValue cur = JS_GetPropertyStr(ctx, g, "WebAssembly");
		int have = !JS_IsUndefined(cur);
		JS_FreeValue(ctx, cur);
		if (have) { JS_FreeValue(ctx, g); return; }
	}

	if (!g_tok_class) {
		JS_NewClassID(&g_tok_class);
		JS_NewClass(rt, g_tok_class, &g_tok_def);
	}
	for (i = 0; i < JW_MAX; i++) { g_jwmem[i].buf = JS_UNDEFINED; g_jwmem[i].inst = -1;
	                               g_jwtab[i].inst = -1; }

	prims = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, prims, g_prims,
	                           (int)(sizeof g_prims / sizeof g_prims[0]));

	fn = JS_Eval(ctx, JW_PRELUDE, sizeof(JW_PRELUDE) - 1, "<wasm>",
	             JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(fn)) {
		JSValue e = JS_GetException(ctx);
		const char *m = JS_ToCString(ctx, e);
		printf("[wasm] prelude failed: %s\n", m ? m : "?");
		if (m) JS_FreeCString(ctx, m);
		JS_FreeValue(ctx, e); JS_FreeValue(ctx, fn);
		JS_FreeValue(ctx, prims); JS_FreeValue(ctx, g);
		return;
	}
	r = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&prims);
	if (JS_IsException(r)) {
		JSValue e = JS_GetException(ctx);
		const char *m = JS_ToCString(ctx, e);
		printf("[wasm] prelude call failed: %s\n", m ? m : "?");
		if (m) JS_FreeCString(ctx, m);
		JS_FreeValue(ctx, e);
	}
	JS_FreeValue(ctx, r);
	JS_FreeValue(ctx, fn);
	JS_FreeValue(ctx, prims);
	JS_FreeValue(ctx, g);
}

void js_wasm_reset(JSContext *ctx)
{
	int i;
	/* Page teardown, and it MUST run before the context is freed rather than
	 * after.  Two kinds of thing are held here: malloc'd arenas the JS heap
	 * cannot see, and JSValues (each memory's cached ArrayBuffer, each
	 * imported function) that the JS heap very much can.  Dropping only the
	 * first leaves QuickJS with live objects at JS_FreeRuntime, which trips
	 * its `list_empty(&rt->gc_obj_list)` assertion -- measured, not
	 * anticipated: that assertion is what caught this. */
	for (i = 0; i < JW_MAX; i++) {
		uint32_t k;
		if (g_jwmod[i].used)  { free(g_jwmod[i].arena); free(g_jwmod[i].bytes); }
		if (g_jwinst[i].used) {
			for (k = 0; k < g_jwinst[i].nhcs; k++)
				JS_FreeValue(ctx, g_jwinst[i].hcs[k].fn);
			free(g_jwinst[i].arena); free(g_jwinst[i].imps);
			free(g_jwinst[i].hcs); free(g_jwinst[i].names);
		}
		if (g_jwmem[i].used) {
			if (!JS_IsUndefined(g_jwmem[i].buf)) JS_FreeValue(ctx, g_jwmem[i].buf);
			free(g_jwmem[i].mallocbase);
		}
		if (g_jwtab[i].used)  free(g_jwtab[i].slots);
	}
	memset(g_jwmod, 0, sizeof g_jwmod);
	memset(g_jwinst, 0, sizeof g_jwinst);
	memset(g_jwmem, 0, sizeof g_jwmem);
	memset(g_jwtab, 0, sizeof g_jwtab);
	for (i = 0; i < JW_MAX; i++) { g_jwmem[i].buf = JS_UNDEFINED; g_jwmem[i].inst = -1;
	                               g_jwtab[i].inst = -1; }
}
