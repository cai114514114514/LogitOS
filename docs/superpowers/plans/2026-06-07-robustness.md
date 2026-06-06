---
title: robustness batch — implementation plan
status: plan
date: 2026-06-07
---

# Staged Implementation Plan — AquaScript Robustness Batch

All paths absolute. Each stage MUST build and pass BOTH `make test-aqs` and `make test-aqs-gcstress` before the next stage starts. No commit unless the user asks; if needed, branch off `m13-html-css` first.

Gating command (run after every stage):
`cd /Users/wangzhe/ststem && make test-aqs && make test-aqs-gcstress`
Expect "all N AquaScript checks passed" (N=135 until Stage 5 adds tests).

---

## Stage 1 — Value-stack overflow guard (Item A)

File: `/Users/wangzhe/ststem/src/apps/aqs/vm.c`

1. After `static int g_native_err;` (vm.c:34) add: `static int g_stack_overflow;`.
2. In `reset_stack()` (vm.c:60-61), append `g_stack_overflow = 0;` to the initializer list.
3. After `push`/`pop`/`peek` (vm.c:57-59) add:
   `static void checked_push(Value v) { if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); g_stack_overflow = 1; return; } *sp++ = v; }`
   (place AFTER `runtime_error` is declared/defined, or add a forward `static int runtime_error(const char*,...);` near the top — `runtime_error` is at vm.c:90, `checked_push` is used only inside `run_until` at vm.c:347+, so defining `checked_push` anywhere before line 347 with a forward decl is fine).
4. `DISPATCH()` macro (vm.c:377-379): insert as the FIRST statement inside the `do { ... }`:
   `if (g_stack_overflow) { g_stack_overflow = 0; goto err; }`
5. Swap `push` → `checked_push` at exactly these run_until sites: 383, 384, 385, 386, 389, 395, 725, 737, 743. Leave all other `push` calls as-is.
6. `call_value` native branch (vm.c:261-267): before `push(r)` add `if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); g_stack_overflow = 1; return 1; } push(r);` (or `checked_push(r); return g_native_err || g_stack_overflow;`). Keep returning the error to `op_CALL`/`op_INVOKE`'s existing `goto err`.
7. `bind_method` (vm.c:305-312): change return type understanding — add at top of the push: `if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); g_stack_overflow = 1; return -1; }`. At callers `op_GET_PROPERTY` (vm.c:663) and `op_GET_SUPER` (vm.c:778), the call is `if (bind_method(...))` / `if (!bind_method(...))`; adjust to treat a negative return as `goto err`. Concretely: in op_GET_PROPERTY, capture `int b = bind_method(...); if (b < 0) goto err; if (b) { ... }`. In op_GET_SUPER, `int b = bind_method(...); if (b < 0) goto err; if (!b) { runtime_error(...); goto err; }`.
8. `run_module` (vm.c:319-325): before `push(OBJ_VAL(script))` (line 322) add `if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); return 1; }`.
9. `err:` resume push (vm.c:801-802): before `push(g_exc);` add `if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); goto err; }` (RAW check, not checked_push).

Gate: `make test-aqs && make test-aqs-gcstress` (135 green; behavior unchanged for all existing programs).

---

## Stage 2 — Wide constant index, 16-bit uniform (Item B) — the meaty one

Files: `/Users/wangzhe/ststem/src/apps/aqs/aqs.h`, `compiler.c`, `vm.c`

1. `aqs.h:162`: `#define AQS_BC_VERSION 1u` → `2u`.
2. `vm.c:352`: `#define READ_CONST() (frame->fn->consts[READ_BYTE()])` → `(frame->fn->consts[READ_SHORT()])`. (Single line; fixes all 12 reads.)
3. `compiler.c`:
   a. `makeConst` (55-60): delete the `if (k > 255) {...}` block; body becomes `int k = aqs_chunk_const(current->fn, v); return k;`.
   b. Add after `emit2` (compiler.c:54): `static void emit16(int k) { emit((uint8_t)((k >> 8) & 0xff)); emit((uint8_t)(k & 0xff)); }`.
   c. `emitConst` (61): `emit(OP_CONST); emit16(makeConst(v));`.
   d. `dot` (253): `if (match(T_LPAREN)) { uint8_t argc = arg_list(); emit(OP_INVOKE); emit16(name); emit(argc); } else { emit(OP_GET_PROPERTY); emit16(name); }`.
   e. `named_variable` (271): `emit(OP_GET_GLOBAL); emit16(identConst(t.start, t.len));`.
   f. `store_name` (377): `emit(OP_DEF_GLOBAL); emit16(identConst(name.start, name.len));`.
   g. `assignment` property (406): `emit(OP_SET_PROPERTY); emit16(fk);`.
   h. `assignment` indexed-global (416): `emit(OP_GET_GLOBAL); emit16(identConst(name.start, name.len));`.
   i. `for_statement` global var (489): `emit(OP_DEF_GLOBAL); emit16(identConst(var.start, var.len));`.
   j. `import_statement` (506): `emit(OP_IMPORT); emit16(identConst(name.start, name.len));`.
   k. `from_statement` (520-521): `emit(OP_IMPORT); emit16(modk);` and `emit(OP_GET_PROPERTY); emit16(identConst(nm.start, nm.len));`.
   l. `compile_function` OP_CLOSURE (613): `emit(OP_CLOSURE); emit16(makeConst(OBJ_VAL(fn)));` (the upvalue-pair loop at 614 stays 1-byte).
   m. `fun_declaration` (627): `emit(OP_DEF_GLOBAL); emit16(identConst(name.start, name.len));`.
   n. `super_` (657-658): `if (up >= 0) { emit(OP_GET_UPVALUE); emit((uint8_t)up); }` (upvalue index stays 1-byte!) `else { emit(OP_GET_GLOBAL); emit16(identConst(current_class.name, current_class.namelen)); }`; then `emit(OP_GET_SUPER); emit16(name);` (659). CAUTION: line 657 emits OP_GET_UPVALUE (slot index, 1-byte — keep emit2/1-byte); only line 658's OP_GET_GLOBAL and 659's OP_GET_SUPER are const indices.
   o. `class_declaration`: 668 `emit(OP_CLASS); emit16(namek);`, 671 `emit(OP_DEF_GLOBAL); emit16(namek);`, 703 `emit(OP_METHOD); emit16(mk);`.
4. DO NOT touch: `OP_GET_LOCAL`/`OP_SET_LOCAL`/`OP_GET_UPVALUE`/`OP_SET_UPVALUE` (slot indices, 1-byte), `OP_CALL` argc, `OP_MAKE_LIST`/`OP_MAKE_DICT` counts, OP_CLOSURE upvalue pairs, `emitJump`/`emitLoop`/`patchJump`.

Gate: `make test-aqs && make test-aqs-gcstress`. The `bc_tests()` roundtrip suite (dump→load→run) proves the wider encoding round-trips; `bc_reject` proves version rejection. 135 green.

---

## Stage 3 — OOM wrapper + sites (Item C)

Files: `/Users/wangzhe/ststem/src/apps/aqs/object.c`, `vm.c`, `compiler.c`, `lexer.c`, `aqs.h`

1. `aqs.h`: add `extern int g_oom;` near the other externs (e.g. after `extern char aqs_err[256];`, line 236).
2. `object.c`:
   a. Top of file: `int g_oom = 0;` and
      `static void *aqs_malloc(size_t n){ void *p = malloc(n); if (!p){ snprintf(aqs_err, sizeof aqs_err, "out of memory"); g_oom = 1; } return p; }`
      `static void *aqs_realloc(void *p, size_t n){ void *q = realloc(p, n); if (!q){ snprintf(aqs_err, sizeof aqs_err, "out of memory"); g_oom = 1; } return q; }`
      (needs `#include <stdio.h>` for snprintf — add if absent; or use a fixed `strcpy(aqs_err, "out of memory")`).
   b. Replace `malloc`/`realloc` with wrappers at: alloc_obj (23), aqs_str_copy (48), aqs_closure_new (68), aqs_module_slot (123), aqs_list_push (148), dict_grow (194), aqs_chunk_write (271), aqs_chunk_const (283), gc_mark_obj gray (295).
   c. alloc_obj (23-29): `Obj *o = (Obj*)aqs_malloc(size); if (!o) return NULL;` then the field writes.
   d. aqs_str_copy (46-51): `char *buf = aqs_malloc(len+1); if (!buf) return NULL;` ... then `return aqs_str_take(buf, len);` (aqs_str_take calls alloc_obj which may also fail → returns NULL; callers already vary, but the run loop's DISPATCH g_oom check catches it).
   e. aqs_closure_new (64-74): after `ups = aqs_realloc/aqs_malloc(...)`, `if (!ups) return NULL;` before the loop.
   f. aqs_module_slot (121-124): after the grow realloc, `if (g_oom) return NULL;` before writing `m->vars[...]`.
   g. aqs_list_push (144-151): after the grow realloc, `if (g_oom) return;` before `l->items[l->count++] = v;`.
   h. dict_grow (191-206): after `ne = aqs_malloc(...)`, `if (!ne) return;` before memset/rehash.
   i. aqs_chunk_write (267-274): after the realloc, `if (g_oom) return;` before `fn->code[fn->count++] = b;`.
   j. aqs_chunk_const (276-287): after the realloc, `if (g_oom) return fn->kcount;` (or return 0) before the store; the compiler's post-loop g_oom check handles the failure.
   k. gc_mark_obj (289-298): rewrite the grow block:
      `if (gray_count + 1 > gray_cap) { int nc = gray_cap < 16 ? 16 : gray_cap * 2; Obj **ng = (Obj**)aqs_realloc(gray, (size_t)nc * sizeof(Obj*)); if (!ng) { o->marked = 0; g_oom = 1; return; } gray = ng; gray_cap = nc; } gray[gray_count++] = o;`
3. `vm.c`:
   a. `reset_stack()` (61): append `g_oom = 0;`.
   b. DISPATCH macro (377): add `if (g_oom) { g_oom = 0; runtime_error("out of memory"); goto err; }` (after the g_stack_overflow line from Stage 1).
   c. str_concat (103-109): `char *buf = (char*)aqs_malloc((size_t)n+1); if (!buf) return NULL;` and at op_ADD (vm.c:410) where it's used: `ObjStr *s = str_concat(...); if (!s) goto err;` then `sp -= 2; push(OBJ_VAL(s));`.
   d. module_source (134, 148, 150): use aqs_malloc/aqs_realloc; the existing `if (buf)` (135) and the `!src` caller check (338) cover NULL. For the file path add `if (!buf) { fclose(f); return NULL; }` after line 148.
   e. runtime_error (90-96) + ensure_exc (85-88): guard the aqs_str_copy result — `ObjStr *s = aqs_str_copy(...); return throw_value(s ? OBJ_VAL(s) : NIL_VAL);` and in ensure_exc `if (!g_has_exc) { ObjStr *s = aqs_str_copy(aqs_err, (int)strlen(aqs_err)); g_exc = s ? OBJ_VAL(s) : NIL_VAL; g_has_exc = 1; }`.
   f. Note: `aqs_malloc`/`aqs_realloc` are static in object.c; str_concat/module_source live in vm.c and can't see them. Resolution: either (i) make the two wrappers non-static and declare them in `aqs.h`, OR (ii) keep vm.c's two mallocs using raw malloc but set g_oom + aqs_err manually on NULL. CHOICE: expose them — add `void *aqs_malloc(size_t);` `void *aqs_realloc(void*, size_t);` to aqs.h, drop `static` in object.c. Cleaner and reused by lexer.c/compiler.c too.
4. `compiler.c`:
   a. string() (179): `char *buf = (char*)aqs_malloc((size_t)t.len+1); if (!buf) { error("out of memory"); return; }`.
   b. aqs_compile_module (775+): set `g_oom = 0;` near `had_error = 0;` (784); after the parse loop (799) / before `return had_error ? NULL : script;` (805) add `if (g_oom) return NULL;` (or fold: `return (had_error || g_oom) ? NULL : script;`).
5. `lexer.c`:
   a. push() (13-21): `b->t = (Token*)aqs_realloc(b->t, b->cap * sizeof(Token)); if (!b->t) return;` (guards the deref on 19). Since push is void, also have `aqs_lex` poll `g_oom`: in the scan loop add a check that breaks to the existing error-exit (the `err`/`free(b.t); return NULL` path) when `g_oom` is set, so a NULL token buffer never gets indexed. Confirm by reading lexer.c's `aqs_lex` body (lines ~23-150) for the exact `err`/return-NULL site and mirror it.

Gate: `make test-aqs && make test-aqs-gcstress`. 135 green (no new tests — see spec testability note). gcstress is especially important here: it collects before every alloc, exercising the gray-stack path and the wrapper plumbing under load.

---

## Stage 4 — Import-failure cleanup (Item D)

File: `/Users/wangzhe/ststem/src/apps/aqs/vm.c`, `aqs_import` (328-345)

1. Line 338 `!src` branch: `if (!src) { if (nmodules > 0 && modules[nmodules-1] == m) nmodules--; runtime_error("cannot import module '%.*s'", name->len, name->chars); return NULL; }`.
2. Line 341: `if (!script) { if (nmodules > 0 && modules[nmodules-1] == m) nmodules--; return NULL; }`.
3. Line 342: `if (run_module(script)) { if (nmodules > 0 && modules[nmodules-1] == m) nmodules--; return NULL; }`.
4. Lines 333-336 (early register) and 343-344 (success) untouched.

Gate: `make test-aqs && make test-aqs-gcstress` (135 green).

---

## Stage 5 — Tests (Item T)

File: `/Users/wangzhe/ststem/tools/t/aqs_test.c`

1. **A — overflow raises (err):** in `main` (before line 712):
   `err("stack_overflow_recurse", "def f(n):\n    return f(n + 1)\nf(0)\n");`
2. **A — overflow catchable (ok):**
   `ok("overflow_caught", "def f(n):\n    return f(n + 1)\ntry:\n    f(0)\nexcept e:\n    print(\"caught\")\n", "caught\n");`
   (verify the message/flow; if the recursion hits FRAMES_MAX "call depth exceeded" instead of "stack overflow", the test still passes since both are caught — assert only on "caught").
3. **B — >255 consts succeed (ok):** in `main`, build the source with snprintf:
   `{ char s[12000]; int p = 0; for (int i = 0; i < 300; i++) p += snprintf(s+p, sizeof s - p, "x%d = %d\n", i, i); snprintf(s+p, sizeof s - p, "print(x299)\n"); ok("wide_const_300", s, "299\n"); }`
4. **B — >255 const .la round-trip:** inside `bc_tests()` (before its closing brace ~191), build the same 300-const source into a local buffer and call `roundtrip("bc_wide_const", s);`. (roundtrip needs a `const char*`; build the buffer locally in bc_tests.)
5. **D — import retry:** in `main`, alongside the existing `aqs_add_module_source("mathx", MATHX)` (line 204), register a deliberately-failing module and a good one, then:
   `aqs_add_module_source("badmod", "x = 1 / 0\n");`  (runtime raise on import)
   `aqs_add_module_source("goodmod", "v = 42\n");`
   `err("import_fails", "import badmod\n");`
   `ok("import_after_fail", "import goodmod\nprint(goodmod.v)\n", "42\n");`
   This proves a failed import does not corrupt the module table / exhaust the cache (the `nmodules--` fix). (Same-name retry isn't testable via the append-only registry, so distinct names + a subsequent good import is the assertion.)

Gate (final): `make test-aqs && make test-aqs-gcstress` — expect "all ~139 AquaScript checks passed" on BOTH. Then optionally `make test-aqs-os` to confirm on-Aqua examples still run.

---

## Cross-stage invariants
- The DISPATCH() flag-check ordering: g_stack_overflow check (Stage 1) then g_oom check (Stage 3), both before `op = READ_BYTE()`.
- emit16 byte order MUST equal READ_SHORT (big-endian: hi, lo) — same as emitJump/patchJump.
- AQS_BC_VERSION bump is mandatory and the ONLY .la format change.
- No public AquaScript behavior changes for any program that previously ran; the only observable new behavior is errors (overflow / OOM) replacing crashes, and >255-const modules compiling.
