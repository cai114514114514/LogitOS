---
title: AetherScript robustness batch (stack guard / OOM / wide const / import cleanup)
status: spec
date: 2026-06-07
---

# AetherScript Robustness Batch — Spec

External code-review hardening of the AetherScript bytecode VM (`src/apps/as/`). Four LOCKED items, no feature creep. Every change is grounded in verified line anchors in the real tree.

## Scope (locked)

| # | Item | Files |
|---|------|-------|
| A | Value-stack overflow guard | `vm.c` |
| B | Wide constant index (1-byte → 2-byte) | `compiler.c`, `vm.c`, `as.h` (version bump) |
| C | OOM checks + propagation | `object.c`, `vm.c`, `compiler.c`, `lexer.c`, `as.h` |
| D | Import-failure cleanup | `vm.c` |
| T | Tests | `tools/t/as_test.c` |

Out of scope: any new opcode semantics, dict/closure/class behavior, the `.la` format beyond the version bump, performance work, a malloc-injection harness.

---

## Item A — Value-stack overflow guard

**Bug.** `push()` (`vm.c:57` `static void push(Value v){*sp++=v;}`) is unbounded. Call DEPTH is guarded (`frame_count==FRAMES_MAX` at `vm.c:220,230`) but the VALUE stack (`STACK_MAX=4096`, `vm.c:9`) is not: deep recursion with locals, or many net-increasing pushes, overruns `stack[]` and corrupts adjacent statics.

**Constraint.** `push()` is a file-static function; the `err:` label lives inside `run_until()` (`vm.c:787`). A static function cannot `goto err`. The guard therefore uses a flag the dispatch loop polls.

**Fix.**
1. `static int g_stack_overflow;` near `vm.c:34` (mirrors `g_native_err`).
2. Reset it in `reset_stack()` (`vm.c:60-61`).
3. New `checked_push(Value v)`: if `sp >= stack + STACK_MAX`, call `runtime_error("stack overflow")`, set `g_stack_overflow = 1`, and `return` WITHOUT advancing `sp` (stack stays valid). Else `*sp++ = v`.
4. `DISPATCH()` macro (`vm.c:377-379`) gains a first line: `if (g_stack_overflow) { g_stack_overflow = 0; goto err; }`.
5. Replace `push` with `checked_push` ONLY at the 9 net-increasing sites inside `run_until`: `op_CONST` (383), `op_NIL` (384), `op_TRUE` (385), `op_FALSE` (386), `op_GET_LOCAL` (389), `op_GET_GLOBAL` (395), `op_CLOSURE` (725), `op_GET_UPVALUE` (737), `op_CLASS` (743). All arithmetic/comparison/`MAKE_LIST`/`MAKE_DICT`/`INVOKE`/`INDEX_GET`/`RET`/etc. are net-neutral or net-decreasing — they keep plain `push`.
6. Out-of-`run_until` sites (no `err:`/`DISPATCH` in scope):
   - `call_value` native branch (`vm.c:261-266`): native result push — return `g_native_err || g_stack_overflow` so the existing `if (call_value(...)) goto err` at `op_CALL` (470) / `op_INVOKE` (599,642) unwinds. Pre-check `sp` before `push(r)`; set `g_stack_overflow` + skip the push on overflow.
   - `bind_method` (`vm.c:305-312`): add a pre-check; on overflow set `g_stack_overflow` and return a `-1` sentinel; callers `op_GET_PROPERTY` (663) and `op_GET_SUPER` (778) treat `<0` as `goto err` (they already special-case its `0` vs `1` return).
   - `run_module` (`vm.c:322`): raw pre-check before `push(OBJ_VAL(script))`; on overflow `runtime_error(...)` + `return 1`.
7. The `err:` handler-resume push (`vm.c:802`): a RAW bounds check (NOT `checked_push`, which would re-arm the flag → infinite `err:` loop) — `if (sp >= stack + STACK_MAX) { runtime_error("stack overflow"); goto err; }` immediately before `push(g_exc)`. On the second pass no handler above `floor` is found → uncaught path.

**Behavior.** Stack overflow becomes a CATCHABLE runtime error (folds through `ensure_exc` into `g_exc`); a `try/except` around deep recursion catches it; uncaught it leaves "stack overflow" in `as_err` and `run_until` returns 1.

---

## Item B — Wide constant index (CHOSEN: uniform 16-bit)

**Bug.** `makeConst` errors at `k > 255` (`compiler.c:58`) because const-pool operands are one `uint8_t`. A large top-level module pools all its constants in one `ObjFn`, exceeding 255 distinct entries (every unique identifier/method/global/string + each nested `ObjFn` burns a slot; dedup is by `as_value_eq`, `object.c:279).

**Decision: widen ALL const-index operands to 16-bit big-endian** (Approach A), NOT paired `_LONG` opcodes (Approach B). Justification in feasibility notes — one-line VM change (`READ_CONST` uses the already-existing `READ_SHORT`), 13 mechanical compiler edits, structural correctness, vs 12 new opcodes + 12 duplicate handlers + per-site threshold testing for B.

**VM (`vm.c`).** Redefine `READ_CONST()` (`vm.c:352`): `#define READ_CONST() (frame->fn->consts[READ_SHORT()])`. `READ_SHORT()` (`vm.c:351`) is big-endian and already used for jumps. This fixes all 12 const-reading opcodes at once: `OP_CONST`, `OP_GET_GLOBAL`, `OP_DEF_GLOBAL`, `OP_SET_GLOBAL`, `OP_INVOKE`, `OP_GET_PROPERTY`, `OP_SET_PROPERTY`, `OP_IMPORT`, `OP_CLOSURE`, `OP_CLASS`, `OP_METHOD`, `OP_GET_SUPER`.

**Compiler (`compiler.c`).**
- Delete the `k > 255` error in `makeConst` (`compiler.c:55-60`); return `k`.
- Add `static void emit16(int k){ emit((uint8_t)((k>>8)&0xff)); emit((uint8_t)(k&0xff)); }` (big-endian, matching `READ_SHORT` and `emitJump`/`patchJump` byte order).
- Convert every `emit2(OP_X, (uint8_t)k)` const-operand site to `emit(OP_X); emit16(k)`:
  `emitConst` (61), `dot` (253: `emit(OP_INVOKE); emit16(name); emit(argc)` and `emit(OP_GET_PROPERTY); emit16(name)`), `named_variable` (271), `store_name` (377), `assignment` property (406) + indexed-global (416), `import_statement` (506), `from_statement` (520-521), `compile_function` `OP_CLOSURE` (613), `fun_declaration` (627), `super_` (657-658) + `OP_GET_SUPER` (659), `for_statement` global var (489), `class_declaration` (668 `OP_CLASS`, 671 `OP_DEF_GLOBAL`, 703 `OP_METHOD`).
- Unchanged: `OP_CLOSURE` upvalue pairs (`compiler.c:614`) and `arg_list` argc (operand counts/slots, not const indices); `emitJump`/`emitLoop`/`patchJump` (separate 16-bit operands).

**.la round-trip + version (`as.h`, `as_bc.c`).** `as_bc.c` serializes `fn->code` verbatim (`wr_bytes`/`rd_bytes`, lines 63/176) and `fn->consts` as tagged values — the operand bytes live inside `fn->code`, so a 1→2 byte operand round-trips for free. BUT the on-disk `code` layout changes, so bump `AS_BC_VERSION` `1u → 2u` (`as.h:162`). `as_load` already rejects mismatched versions (`as_bc.c:245`); the `bc_reject` negative test (`as_test.c:157-190`) tampers `blob[4]` relative to the current version, so it stays green. The `bc_tests()` roundtrip suite (dump→free→load→run, compares output) is the correctness gate; a `>255`-const roundtrip case is added.

**OP_INVOKE / OP_CLOSURE encodings after widening.** `OP_INVOKE` = `[op][idx_hi][idx_lo][argc]` (VM: `READ_CONST()` 2 bytes, then `READ_BYTE()` argc — `vm.c:592-593`). `OP_CLOSURE` = `[op][idx_hi][idx_lo]` then `upvalue_count*2` 1-byte pairs (`vm.c:723-734`) — ordering unchanged, safe.

---

## Item C — OOM checks + propagation

**Bug.** Every `malloc`/`realloc` in `object.c` (alloc_obj 23, str_copy 48, closure upvalues 68, module_slot 123, list_push 148, dict_grow 194, chunk_write 271, chunk_const 283, gray stack 295), `lexer.c` (push 17), `compiler.c` (string 179), `vm.c` (str_concat 106, module_source 134/148/150) is unchecked → NULL deref on OOM. The only existing NULL check is `module_source`'s in-memory path (`vm.c:135`).

**Fix.**
1. `as_malloc(size_t)` / `as_realloc(void*,size_t)` in `object.c`: on NULL set `as_err = "out of memory"` and `g_oom = 1`; return the (NULL) pointer. `static int g_oom;` in `object.c`; declare `extern int g_oom;` in `as.h`; reset in `reset_stack()` (`vm.c:61`) AND at the top of `as_compile_module` (`compiler.c:775`).
2. `DISPATCH()` macro (`vm.c:377`) checks it (same spot as `g_stack_overflow`): `if (g_oom) { g_oom = 0; runtime_error("out of memory"); goto err; }`.
3. Replace all listed `malloc`/`realloc` with the wrappers.
4. Propagation by layer:
   - **VM-runtime object.c allocs:** `alloc_obj` returns NULL on fail (g_oom set); DISPATCH catches at the next boundary. Guard every "store after grow" with `if (g_oom) return;` (e.g. `as_list_push`, `as_module_slot`, `dict_grow`) so no NULL is dereferenced before the unwind. No public signature changes required.
   - **`str_concat` (vm.c:103-109):** `if (!buf) goto err;` (called from `op_ADD`, which has `err:` in scope; g_oom already set).
   - **`module_source` (vm.c:129-158):** wrappers set g_oom and return NULL; `as_import` (338) already checks `!src`.
   - **Compile-time:** `string()` (compiler.c:179) → `if (!buf) { error("out of memory"); return; }` (sets `had_error`). `as_chunk_write`/`as_chunk_const` (object.c) set `g_oom` + SKIP the write on realloc-fail; `as_compile_module` checks `g_oom` after the parse loop and returns NULL if set. Incomplete, never-executed bytecode is reclaimed by `as_free_objects`.
   - **Lexer `push` (lexer.c:13-21):** `as_lex` polls `g_oom` in its scan loop and treats it like its existing `err` flag → returns NULL → `as_compile_module:779` `if (!toks) return NULL`.
   - **GC gray stack (object.c:293-297) — hard case:** on `as_realloc` NULL during mark: do NOT advance `gray_cap`, set `o->marked = 0` (un-mark → sweep treats it conservatively LIVE; a leaked object is recoverable next GC, a freed-live object is UAF), set `g_oom = 1`, return without enqueuing. Collection finishes with a possibly-incomplete worklist but NO freed-live objects and NO NULL deref; control returns to `alloc_obj` → opcode → DISPATCH → unwind.
5. **Harden the error-reporting allocs:** `runtime_error` (vm.c:90-96) and `ensure_exc` (vm.c:85-88) call `as_str_copy`, which can OOM. If it returns NULL, fall back to `throw_value(NIL_VAL)` rather than storing `OBJ_VAL(NULL)` (which a catcher could deref). `gc_mark_obj` is already NULL-safe (vm.c:291) and the uncaught `IS_STR(g_exc)` path (vm.c:807) handles a non-string `g_exc`.

**Testability note (explicit).** There is no malloc-injection harness, so real OOM cannot be triggered in the host unit test. Item C ships with NO new triggering test; it is validated by the FULL existing suite + gcstress staying green (proves the wrappers/flow don't regress) plus review of the propagation paths. No fabricated OOM test.

---

## Item D — Import-failure cleanup

**Bug.** `as_import` (`vm.c:328-345`) registers `modules[nmodules++] = m` at line 335 BEFORE compile/run (intentional, circular-safe), but the three failure paths (`!src` 338, `!script` 341, `run_module` fail 342) `return NULL` leaving the broken partial module in `modules[]` — a later `import` returns the dud.

**Fix.** On each of the 3 failure paths, remove the just-registered entry (it is always `modules[nmodules-1]` by the top-entry invariant; recursive imports push higher indices and fully unwind first). Defensive form: `if (nmodules > 0 && modules[nmodules-1] == m) nmodules--;`. Success path (343-344) untouched, so circular-import safety is preserved and a retry re-attempts (`module_find` at 330 then misses).

---

## Test plan (`tools/t/as_test.c`; current count 135)

Add (registered in `main`, before the final `printf` at line 712; B's roundtrip case inside `bc_tests`):
- **A:** `err("stack_overflow_recurse", "def f(n):\n    return f(n + 1)\nf(0)\n")` — asserts a runtime error (stack/frame overflow) instead of a crash.
- **A (catchable):** an `ok(...)` wrapping deep recursion in `try/except` and printing on catch, proving overflow is catchable.
- **B:** `ok("wide_const_300", <300 distinct `xN = N` assignments + print(x299)>, "299\n")` — generated with `snprintf` into a buffer in `main`; was previously a compile error ("too many constants"), now succeeds.
- **B (.la):** a `roundtrip("bc_wide_const", <same 300-const source>)` inside `bc_tests()` — proves the wider operand round-trips through dump/load and `AS_BC_VERSION 2`.
- **D:** import-after-failure retry — register a bad in-memory module + a good one via `as_add_module_source`; assert `import bad` fails, then a DIFFERENT good `import` still succeeds (proves the slot was freed and the table is not corrupted). The registry is append-only, so use distinct names rather than same-name replacement.

Gates: `make test-as` AND `make test-as-gcstress` both green at every stage. Final count ~139.
