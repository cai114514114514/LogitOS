/* The gate for the WebAssembly JavaScript API (c/apps/browser/js_wasm.c).
 *
 * IT RUNS THE API, IT DOES NOT LINK IT.  That distinction is the most
 * expensive lesson in this repository: `make test-wpt ONLY=css/css-grid` read
 * 531/11152 WITH AND WITHOUT the grid implementation, because the runner
 * linked layout.c and then never called it.  So every check below goes through
 * real JavaScript evaluated in a real QuickJS context with js_wasm_install()
 * having run -- there is no path here that a no-op implementation survives.
 * `make test-wasm-js-negctl` is the proof: it breaks one line of the value
 * conversion and requires this file to redden.
 *
 * THE MODULES ARE ASSEMBLED FROM .wat AND CHECKED IN AS BYTES.  See
 * tools/wasm_js_modules.py for why (a gate whose only witness is an optional
 * download stops being a gate the day the download fails) and for the --check
 * that stops the pair from drifting.
 *
 * WHAT IS ASSERTED HERE THAT THE SPEC SUITE CANNOT BE.  The suite next door
 * (tests/unit/wasm_exec_test.c, 14020 assertions) proves the INTERPRETER.
 * Nothing in it touches JavaScript, so it says nothing about the things this
 * boundary gets wrong: that an i64 is a BigInt and a Number must throw rather
 * than round, that a grow detaches the old ArrayBuffer instead of leaving a
 * view over stale bytes, that a JS exception from an import arrives as itself
 * rather than wrapped in a RuntimeError, and that a missing import is a
 * LinkError rather than a TypeError.  Those four are the reason this file
 * exists; each is asserted by name below.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "js_wasm.h"
#include "wasm_exec.h"

#include "wasm_js_modules.inc"

static int g_fail, g_checks;

static void ck(int cond, const char *what)
{
	g_checks++;
	if (!cond) { printf("  FAIL %s\n", what); g_fail++; }
}

/* Make each module's bytes reachable from JavaScript as a Uint8Array, so the
 * assertions below read as the JS they are.  A COPY into a fresh ArrayBuffer,
 * because the API is entitled to be handed a buffer the page owns. */
static void put_module(JSContext *ctx, JSValue g, const char *name,
                       const unsigned char *b, size_t n)
{
	JSValue ab = JS_NewArrayBufferCopy(ctx, b, n);
	JSValue u8, ctor, global;
	global = JS_GetGlobalObject(ctx);
	ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
	u8 = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)&ab);
	JS_FreeValue(ctx, ctor);
	JS_FreeValue(ctx, global);
	JS_FreeValue(ctx, ab);
	JS_SetPropertyStr(ctx, g, name, u8);
}

/* Run one snippet.  It must evaluate to the string "ok"; anything else -- a
 * different string, or a thrown error -- is a failure and is PRINTED, because
 * a gate that says "3 failed" without saying what is a gate people learn to
 * ignore. */
static void js_ck(JSContext *ctx, const char *what, const char *src)
{
	JSValue r;
	const char *s;
	g_checks++;
	/* EVERY SNIPPET IS A FUNCTION BODY and must `return`.  Wrapping here
	 * rather than at each call site is not tidiness: `return` at the top
	 * level of a script is a SyntaxError, so a snippet written the obvious
	 * way would fail to PARSE -- and a parse failure reports as the check
	 * failing, which reads like the API being wrong. */
	{
		char *wrapped = malloc(strlen(src) + 32);
		if (!wrapped) { printf("  FAIL %s (out of memory)\n", what); g_fail++; return; }
		sprintf(wrapped, "(function(){%s})()", src);
		r = JS_Eval(ctx, wrapped, strlen(wrapped), "<check>", JS_EVAL_TYPE_GLOBAL);
		free(wrapped);
	}
	if (JS_IsException(r)) {
		JSValue e = JS_GetException(ctx);
		JSValue stk = JS_GetPropertyStr(ctx, e, "stack");
		const char *m = JS_ToCString(ctx, e);
		const char *st = JS_ToCString(ctx, stk);
		printf("  FAIL %s\n        threw: %s\n", what, m ? m : "?");
		if (st && *st) printf("        %s", st);
		if (m) JS_FreeCString(ctx, m);
		if (st) JS_FreeCString(ctx, st);
		JS_FreeValue(ctx, stk);
		JS_FreeValue(ctx, e);
		JS_FreeValue(ctx, r);
		g_fail++;
		return;
	}
	s = JS_ToCString(ctx, r);
	if (!s || strcmp(s, "ok") != 0) {
		printf("  FAIL %s\n        got: %s\n", what, s ? s : "(not a string)");
		g_fail++;
	}
	if (s) JS_FreeCString(ctx, s);
	JS_FreeValue(ctx, r);
}

/* Drain the promise job queue.  compile()/instantiate() are asynchronous in
 * the API, and a test that checked their results without running the jobs
 * would be asserting about a pending promise -- which is to say, about
 * nothing. */
static void drain(JSContext *ctx)
{
	JSContext *c1;
	int n;
	for (n = 0; n < 1000; n++) {
		int r = JS_ExecutePendingJob(JS_GetRuntime(ctx), &c1);
		if (r <= 0) break;
	}
}

/* A C-LEVEL CHECK, AND IT IS HERE BECAUSE THE JAVASCRIPT ONE CANNOT FAIL.
 *
 * `memory.grow` inside a module relocates the linear memory, and the host that
 * lent that memory must be told the new base -- wasm_exec.h's `memp`.  The
 * obvious place to gate that is the JS check "a grow INSIDE wasm keeps an
 * imported Memory coherent" a few dozen lines below, and it DOES pass.  It
 * also passes with the write-back deleted: `jw_mem_live` asks the INSTANCE for
 * the live base rather than trusting its own copy, so the JavaScript path is
 * immune to the defect by construction.
 *
 * Measured, not reasoned about -- `-DWASM_NEGCTL_MEMP` left that gate green at
 * 57/57, which is the definition of a control that cannot be watched failing.
 * So the control lives where the contract actually is: a host holding
 * `struct wasm_hostimport` and reading `.mem` after the module grew itself.
 * With the write-back removed this reads back a buffer of exactly the right
 * NEW length full of the OLD bytes, and reports it. */
static void c_level_memp_check(void)
{
	static unsigned char arena[24u << 20];
	struct wasm_module m;
	struct wasm_arena a;
	struct wasm_instance *in;
	struct wasm_hostimport imp;
	uint8_t *hostmem;
	uint32_t pages = 1, maxp = 4, idx;
	union wasm_val rets[1], args[1];
	int e;

	hostmem = calloc(1, 65536u * 4);
	if (!hostmem) { ck(0, "memp: out of memory"); return; }
	wasm_arena_init(&a, arena, sizeof arena);
	e = wasm_load(W_impmem, sizeof W_impmem, &m, &a);
	ck(e == WASM_OK, "memp: the fixture loads");
	if (e) { free(hostmem); return; }

	memset(&imp, 0, sizeof imp);
	imp.module = "env"; imp.name = "m"; imp.kind = WASM_IMP_MEMORY;
	imp.mem = hostmem; imp.mpages = &pages; imp.mmaxpages = &maxp;
	imp.memp = &hostmem;          /* the write-back under test */
	e = wasm_instantiate(&in, &m, &a, &imp, 1);
	ck(e == WASM_TRAP_NONE, "memp: the module instantiates over a host memory");
	if (e) { free(hostmem); return; }

	/* NOTE THE CONVENTION: wasm_export_index returns 1 for FOUND and 0 for
	 * absent -- not the 0-is-success this file's other calls use.  Reading it
	 * the other way round produced "growstore is exported: FAIL" against a
	 * module that plainly exports it, which is the apparatus lying about the
	 * subject.  Suspect the apparatus first. */
	e = wasm_export_index(in, "growstore", 9, WASM_EXT_FUNC, &idx);
	ck(e == 1, "memp: growstore is exported");
	if (e != 1) return;
	args[0].bits = 0; args[0].i32 = 4242;
	e = wasm_invoke(in, idx, args, rets);
	ck(e == WASM_TRAP_NONE, "memp: growstore runs");
	ck(pages == 2, "memp: the host's page count follows the grow");
	/* THE CHECK.  The module grew the memory and stored 4242 at offset 0; a
	 * host reading through its OWN pointer must see it. */
	ck(hostmem == wasm_mem_bytes(in, NULL),
	   "memp: the host's base pointer follows the grow");
	ck(*(const uint32_t *)hostmem == 4242u,
	   "memp: the host reads the bytes the module just wrote (a stale base "
	   "reads the right LENGTH of the WRONG block)");
}

int main(void)
{
	JSRuntime *rt = JS_NewRuntime();
	JSContext *ctx = JS_NewContext(rt);
	JSValue g = JS_GetGlobalObject(ctx);

	printf("WebAssembly JavaScript API\n\n");

	c_level_memp_check();

	put_module(ctx, g, "M_ADD",     W_add,     sizeof W_add);
	put_module(ctx, g, "M_I64",     W_i64,     sizeof W_i64);
	put_module(ctx, g, "M_F32",     W_f32,     sizeof W_f32);
	put_module(ctx, g, "M_MEM",     W_mem,     sizeof W_mem);
	put_module(ctx, g, "M_IMP",     W_imp,     sizeof W_imp);
	put_module(ctx, g, "M_IMPMEM",  W_impmem,  sizeof W_impmem);
	put_module(ctx, g, "M_IMPGLOB", W_impglob, sizeof W_impglob);
	put_module(ctx, g, "M_GLOB",    W_glob,    sizeof W_glob);
	put_module(ctx, g, "M_TAB",     W_tab,     sizeof W_tab);
	put_module(ctx, g, "M_TRAP",    W_trap,    sizeof W_trap);
	JS_FreeValue(ctx, g);

	/* THE CONTROL FOR THE WHOLE FILE.  Before install, WebAssembly must be
	 * undefined -- if it were defined by something else, every check below
	 * would be measuring that instead, and would pass while proving nothing
	 * about this implementation. */
	js_ck(ctx, "control: WebAssembly is absent before install",
	      "return typeof WebAssembly === 'undefined' ? 'ok' : 'PRESENT ALREADY'");

	js_wasm_install(ctx);

	/* ---- the object itself ---- */
	js_ck(ctx, "the namespace exists and names its members",
	      "return ['Module','Instance','Memory','Table','Global','CompileError','LinkError','RuntimeError','validate','compile','instantiate'].every(k => k in WebAssembly) ? 'ok' : 'missing'");

	/* ---- validate ---- */
	js_ck(ctx, "validate accepts a real module",
	      "return WebAssembly.validate(M_ADD) ? 'ok' : 'rejected a good module'");
	js_ck(ctx, "validate rejects a truncated module",
	      "return WebAssembly.validate(M_ADD.slice(0, 12)) ? 'accepted a truncated module' : 'ok'");
	js_ck(ctx, "validate rejects a non-module",
	      "return WebAssembly.validate(new Uint8Array([1,2,3,4])) ? 'accepted junk' : 'ok'");
	/* The oracle for validate is the same corpus the interpreter is held to;
	 * here the point is only that the JS entry reaches it at all.  A
	 * `validate` that returned true unconditionally would pass the first
	 * check and fail these two -- which is why both directions are here. */

	/* ---- Module / Instance, the basic path ---- */
	js_ck(ctx, "new Module + new Instance + call an export",
	      "var m = new WebAssembly.Module(M_ADD);var i = new WebAssembly.Instance(m, {}); return i.exports.add(2, 3) === 5 ? 'ok' : 'got ' + i.exports.add(2,3)");
	js_ck(ctx, "i32 results are SIGNED, as the specification says",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_ADD), {}); return i.exports.add(-1, 0) === -1 ? 'ok' : 'got ' + i.exports.add(-1,0)");
	js_ck(ctx, "i32 arguments wrap modulo 2^32 rather than being refused",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_ADD), {}); return i.exports.add(4294967295, 2) === 1 ? 'ok' : 'got ' + i.exports.add(4294967295,2)");
	js_ck(ctx, "a CompileError names its class, not just Error",
	      "try { new WebAssembly.Module(M_ADD.slice(0,10)); return 'accepted'; }"
	      "catch (e) { return (e instanceof WebAssembly.CompileError) && "
	      "(e instanceof Error) ? 'ok' : 'wrong class: ' + e.name; }");
	js_ck(ctx, "a non-BufferSource is a TypeError",
	      "try { new WebAssembly.Module('not bytes'); return 'accepted'; }"
	      "catch (e) { return e instanceof TypeError ? 'ok' : e.name; }");
	js_ck(ctx, "Module.exports / Module.imports describe the module",
	      "var m = new WebAssembly.Module(M_IMP);var im = WebAssembly.Module.imports(m), ex = WebAssembly.Module.exports(m); return (im.length === 1 && im[0].module === 'env' && im[0].name === 'twice' && im[0].kind === 'function' && ex.length === 1 && ex[0].name === 'call4' && ex[0].kind === 'function') ? 'ok' : JSON.stringify({im:im, ex:ex})");
	js_ck(ctx, "customSections is ABSENT rather than a false []",
	      "return 'customSections' in WebAssembly.Module ? 'present, and it would be claiming there are none' : 'ok'");
	js_ck(ctx, "exports is frozen and has a null prototype",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_ADD), {}); return (Object.isFrozen(i.exports) && Object.getPrototypeOf(i.exports) === null) ? 'ok' : 'not frozen / has a prototype'");

	/* ---- i64 is a BigInt, in BOTH directions ---- */
	js_ck(ctx, "i64 argument and result are BigInt",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_I64), {});var r = i.exports.inc(41n); return (typeof r === 'bigint' && r === 42n) ? 'ok' : 'got ' + typeof r + ' ' + r");
	js_ck(ctx, "i64 carries values a double cannot hold",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_I64), {}); return i.exports.inc(9007199254740992n) === 9007199254740993n ? 'ok' : 'lost precision: ' + i.exports.inc(9007199254740992n)");
	js_ck(ctx, "a Number where an i64 is due THROWS rather than rounding",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_I64), {});"
	      "try { i.exports.inc(41); return 'accepted a Number'; }"
	      "catch (e) { return e instanceof TypeError ? 'ok' : e.name; }");

	/* ---- f32 really narrows ---- */
	js_ck(ctx, "f32 is single precision, not a double in disguise",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_F32), {});var r = i.exports.half(0.1); return r === Math.fround(Math.fround(0.1) / 2) ? 'ok' : 'got ' + r");

	/* ---- imports ---- */
	js_ck(ctx, "an imported JS function is callable from wasm",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_IMP),  { env: { twice: x => x * 2 } }); return i.exports.call4() === 8 ? 'ok' : 'got ' + i.exports.call4()");
	js_ck(ctx, "a MISSING import is a LinkError",
	      "try { new WebAssembly.Instance(new WebAssembly.Module(M_IMP), "
	      "{ env: {} }); return 'accepted'; }"
	      "catch (e) { return (e instanceof WebAssembly.LinkError) ? 'ok' : "
	      "'wrong class: ' + e.name; }");
	js_ck(ctx, "an import of the WRONG SHAPE is a LinkError",
	      "try { new WebAssembly.Instance(new WebAssembly.Module(M_IMP), "
	      "{ env: { twice: 7 } }); return 'accepted a number as a function'; }"
	      "catch (e) { return (e instanceof WebAssembly.LinkError) ? 'ok' : "
	      "'wrong class: ' + e.name; }");
	js_ck(ctx, "a missing NAMESPACE is a TypeError, not a LinkError",
	      "try { new WebAssembly.Instance(new WebAssembly.Module(M_IMP), {}); "
	      "return 'accepted'; }"
	      "catch (e) { return (e instanceof TypeError && "
	      "!(e instanceof WebAssembly.LinkError)) ? 'ok' : 'wrong: ' + e.name; }");
	js_ck(ctx, "an exception thrown by an import propagates AS ITSELF",
	      "class Mine extends Error {}\n"
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_IMP),"
	      "  { env: { twice: () => { throw new Mine('boom'); } } });"
	      "try { i.exports.call4(); return 'did not throw'; }"
	      "catch (e) { return (e instanceof Mine && e.message === 'boom') ? "
	      "'ok' : 'got ' + e.name + ': ' + e.message; }");
	js_ck(ctx, "an imported global reaches the module",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_IMPGLOB),  { env: { base: 1234 } }); return i.exports.get() === 1234 ? 'ok' : 'got ' + i.exports.get()");
	js_ck(ctx, "an imported WebAssembly.Global reaches the module",
	      "var gl = new WebAssembly.Global({ value: 'i32' }, 55);var i = new WebAssembly.Instance(new WebAssembly.Module(M_IMPGLOB),  { env: { base: gl } }); return i.exports.get() === 55 ? 'ok' : 'got ' + i.exports.get()");

	/* ---- traps ---- */
	js_ck(ctx, "unreachable is a RuntimeError",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_TRAP), {});"
	      "try { i.exports.boom(); return 'did not trap'; }"
	      "catch (e) { return (e instanceof WebAssembly.RuntimeError) ? 'ok' : "
	      "'wrong class: ' + e.name; }");
	js_ck(ctx, "the trap MESSAGE names the trap",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_TRAP), {});"
	      "try { i.exports.divz(0); return 'did not trap'; }"
	      "catch (e) { return /divide by zero/.test(e.message) ? 'ok' : "
	      "'unhelpful message: ' + e.message; }");
	js_ck(ctx, "a trap does not poison the instance",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_TRAP), {});"
	      "try { i.exports.boom(); } catch (e) {}"
	      "return i.exports.divz(2) === 0 ? 'ok' : 'got ' + i.exports.divz(2)");

	/* ---- memory ---- */
	js_ck(ctx, "an exported memory is a WebAssembly.Memory over the real bytes",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_MEM), {});i.exports.poke(0, 0x41424344);var v = new DataView(i.exports.mem.buffer); return v.getUint32(0, true) === 0x41424344 ? 'ok' : 'got ' + v.getUint32(0, true).toString(16)");
	js_ck(ctx, "writes through the ArrayBuffer are visible to wasm",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_MEM), {});new DataView(i.exports.mem.buffer).setUint32(8, 12345, true); return i.exports.peek(8) === 12345 ? 'ok' : 'got ' + i.exports.peek(8)");
	js_ck(ctx, "Memory.buffer.byteLength is the page count times 65536",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_MEM), {}); return i.exports.mem.buffer.byteLength === 65536 ? 'ok' : 'got ' + i.exports.mem.buffer.byteLength");

	/* THE ALIASING RULE, and it is the one this whole file is most worth
	 * running for: growing MOVES the bytes, so a view taken before the grow
	 * must become unusable rather than silently reading the old block. */
	js_ck(ctx, "Memory.grow DETACHES the old buffer",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_MEM), {});var before = i.exports.mem.buffer;i.exports.mem.grow(1); return before.byteLength === 0 ? 'ok' : 'the old buffer is still usable: ' + before.byteLength");
	js_ck(ctx, "a VIEW taken before a grow is detached too",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_MEM), {});"
	      "var u = new Uint8Array(i.exports.mem.buffer);"
	      "i.exports.mem.grow(1);"
	      "try { u[0] = 1; } catch (e) {}"
	      "return (u.length === 0 || u.byteLength === 0) ? 'ok' : "
	      "'the stale view is still ' + u.length + ' bytes long'");
	js_ck(ctx, "after a grow the NEW buffer is the new size and keeps the data",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_MEM), {});i.exports.poke(4, 777);var old = i.exports.mem.grow(1);var b = i.exports.mem.buffer; return (old === 1 && b.byteLength === 131072 && i.exports.peek(4) === 777) ? 'ok' : 'old=' + old + ' len=' + b.byteLength + ' v=' + i.exports.peek(4)");
	/* AND THE HARD ONE: the grow happens INSIDE wasm, during a call the page
	 * made.  Nothing in JavaScript touched the memory, and the old buffer
	 * still has to be detached -- otherwise a page holding a view reads the
	 * pre-grow block with no error anywhere.  This is the JS face of the
	 * defect the interpreter's `memp` write-back closes. */
	js_ck(ctx, "the memory.grow OPCODE detaches the buffer too",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_MEM), {});var before = i.exports.mem.buffer;i.exports.growit(1); return before.byteLength === 0 ? 'ok' : 'a grow from INSIDE wasm left a live buffer of ' + before.byteLength + ' bytes over stale memory'");
	js_ck(ctx, "grow past the declared maximum is a RangeError",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_MEM), {});"
	      "try { i.exports.mem.grow(99); return 'grew past the maximum'; }"
	      "catch (e) { return e instanceof RangeError ? 'ok' : e.name; }");
	js_ck(ctx, "a standalone Memory can be constructed and grown",
	      "var m = new WebAssembly.Memory({ initial: 1, maximum: 4 });var a = m.buffer.byteLength; var old = m.grow(2); return (a === 65536 && old === 1 && m.buffer.byteLength === 196608) ? 'ok' : 'a=' + a + ' old=' + old + ' now=' + m.buffer.byteLength");
	js_ck(ctx, "an imported Memory is SHARED with the module, not copied",
	      "var m = new WebAssembly.Memory({ initial: 1, maximum: 4 });var i = new WebAssembly.Instance(new WebAssembly.Module(M_IMPMEM),  { env: { m: m } });i.exports.store(0, 0xdeadbeef | 0); return new DataView(m.buffer).getUint32(0, true) === 0xdeadbeef ? 'ok' : 'the host saw ' + new DataView(m.buffer).getUint32(0,true).toString(16)");
	/* The measured defect, in its JavaScript form: the module grows the
	 * HOST's memory from inside, and the host must then see the new bytes.
	 * Before the `memp` write-back this read back zero -- a buffer of exactly
	 * the right new length over the pre-grow block. */
	js_ck(ctx, "a grow INSIDE wasm keeps an imported Memory coherent",
	      "var m = new WebAssembly.Memory({ initial: 1, maximum: 4 });var i = new WebAssembly.Instance(new WebAssembly.Module(M_IMPMEM),  { env: { m: m } });var pages = i.exports.growstore(4242);var got = new DataView(m.buffer).getUint32(0, true); return (pages === 2 && m.buffer.byteLength === 131072 && got === 4242) ? 'ok' : 'pages=' + pages + ' len=' + m.buffer.byteLength + ' value=' + got");

	/* ---- globals ---- */
	js_ck(ctx, "an exported mutable global reads, writes and round-trips",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_GLOB), {});var a = i.exports.g.value;i.exports.g.value = 100;var b = i.exports.bump(); return (a === 7 && b === 101 && i.exports.g.value === 101) ? 'ok' : 'a=' + a + ' b=' + b + ' now=' + i.exports.g.value");
	js_ck(ctx, "an IMMUTABLE global refuses assignment",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_GLOB), {});"
	      "try { i.exports.c.value = 5; return 'accepted a write'; }"
	      "catch (e) { return e instanceof TypeError ? 'ok' : e.name; }");
	js_ck(ctx, "a standalone Global holds and coerces its value",
	      "var g = new WebAssembly.Global({ value: 'i32', mutable: true }, 5);g.value = 4294967296 + 9; return (g.value === 9 && g.valueOf() === 9) ? 'ok' : 'got ' + g.value");
	js_ck(ctx, "a fresh i64 Global defaults to a BIGINT zero, not 0",
	      "var g = new WebAssembly.Global({ value: 'i64', mutable: true }); return typeof g.value === 'bigint' ? 'ok' : 'got ' + typeof g.value");
	js_ck(ctx, "a post-MVP global type is refused by name",
	      "try { new WebAssembly.Global({ value: 'externref' }); "
	      "return 'accepted a post-MVP type'; }"
	      "catch (e) { return e instanceof TypeError ? 'ok' : e.name; }");

	/* ---- tables ---- */
	js_ck(ctx, "an exported table has a length and call_indirect works",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_TAB), {}); return (i.exports.tbl.length === 4 && i.exports.callidx(0, 5) === 15 && i.exports.callidx(1, 5) === 15) ? 'ok' : 'len=' + i.exports.tbl.length + ' a=' + i.exports.callidx(0,5) + ' b=' + i.exports.callidx(1,5)");
	js_ck(ctx, "Table.get returns a callable for a filled slot, null for empty",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_TAB), {});var f = i.exports.tbl.get(0); return (typeof f === 'function' && f(1) === 11 && i.exports.tbl.get(3) === null) ? 'ok' : 'f=' + typeof f + ' empty=' + i.exports.tbl.get(3)");
	js_ck(ctx, "Table.set writes a slot that call_indirect then reaches",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_TAB), {});i.exports.tbl.set(2, i.exports.tbl.get(1)); return i.exports.callidx(2, 5) === 15 ? 'ok' : 'got ' + i.exports.callidx(2, 5)");
	js_ck(ctx, "Table.set of a non-function is a TypeError",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_TAB), {});"
	      "try { i.exports.tbl.set(2, () => 1); return 'accepted a plain "
	      "JS function'; }"
	      "catch (e) { return e instanceof TypeError ? 'ok' : e.name; }");
	js_ck(ctx, "Table index out of range is a RangeError",
	      "var i = new WebAssembly.Instance(new WebAssembly.Module(M_TAB), {});"
	      "try { i.exports.tbl.get(99); return 'accepted'; }"
	      "catch (e) { return e instanceof RangeError ? 'ok' : e.name; }");
	js_ck(ctx, "a post-MVP table element type is refused by name",
	      "try { new WebAssembly.Table({ element: 'externref', initial: 1 }); "
	      "return 'accepted'; }"
	      "catch (e) { return e instanceof TypeError ? 'ok' : e.name; }");

	/* ---- the asynchronous entry points ---- */
	js_ck(ctx, "compile and instantiate return promises",
	      "var a = WebAssembly.compile(M_ADD), b = WebAssembly.instantiate(M_ADD); return (a instanceof Promise && b instanceof Promise) ? 'ok' : 'not promises'");
	js_ck(ctx, "instantiate resolves to {module, instance}",
	      "globalThis.__r = 'pending';WebAssembly.instantiate(M_ADD, {}).then(function (p) {  globalThis.__r = (p.module instanceof WebAssembly.Module &&     p.instance instanceof WebAssembly.Instance &&     p.instance.exports.add(20, 22) === 42) ? 'ok' : 'wrong shape';}, function (e) { globalThis.__r = 'rejected: ' + e; }); return 'ok'");
	drain(ctx);
	js_ck(ctx, "  ... and that promise actually settled correctly",
	      "return globalThis.__r");
	js_ck(ctx, "instantiate(Module) resolves to the Instance ALONE",
	      "globalThis.__r2 = 'pending';WebAssembly.instantiate(new WebAssembly.Module(M_ADD), {}).then(function (p) { globalThis.__r2 =   (p instanceof WebAssembly.Instance) ? 'ok' : 'got a pair'; }, function (e) { globalThis.__r2 = 'rejected: ' + e; }); return 'ok'");
	drain(ctx);
	js_ck(ctx, "  ... and that promise settled correctly too",
	      "return globalThis.__r2");
	js_ck(ctx, "a bad module REJECTS the promise rather than throwing",
	      "globalThis.__r3 = 'pending';WebAssembly.compile(M_ADD.slice(0, 9)).then(  function () { globalThis.__r3 = 'resolved'; },  function (e) { globalThis.__r3 =     (e instanceof WebAssembly.CompileError) ? 'ok' : 'wrong: ' + e.name; }); return 'ok'");
	drain(ctx);
	js_ck(ctx, "  ... with a CompileError", "return globalThis.__r3");

	/* ---- the bytes are OURS after compile ---- */
	js_ck(ctx, "overwriting the source buffer after compiling changes nothing",
	      "var src = M_ADD.slice();var m = new WebAssembly.Module(src);src.fill(0);var i = new WebAssembly.Instance(m, {}); return i.exports.add(1, 2) === 3 ? 'ok' : 'the module read freed bytes'");

	/* BEFORE JS_FreeContext -- see js_wasm.h.  The other order leaves the
	 * cached ArrayBuffers alive and QuickJS asserts at JS_FreeRuntime. */
	js_wasm_reset(ctx);
	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);

	printf("\nwasm-js: %d checks, %d failed\n", g_checks, g_fail);
	if (g_fail) printf("wasm-js: FAILED\n");
	return g_fail ? 1 : 0;
}
