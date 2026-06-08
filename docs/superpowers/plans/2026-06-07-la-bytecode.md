---
title: .la compiled-bytecode format — implementation plan
status: plan
date: 2026-06-07
---

# Implementation Plan — LibAether `.la` bytecode format (M21 Phase 3)

Ordered, staged. Each stage must build + its gating test pass before the next.
All paths absolute. Line anchors are from the current tree.

---

## Stage 1 — `as_dump` / `as_load` + `AS_BC_VERSION` + host round-trip test

**Gating test:** new `make test-as-bc` (host round-trip, normal + GC-stress) PASSES.

### 1.1 `as.h` — declarations
- File `/Users/wangzhe/ststem/src/apps/as/as.h`.
- After the `OpCode` enum closes (line 157), add:
  `#define AS_BC_VERSION 1u   /* bump on any opcode/.la-format change */`
- In the compile/run API block (after `as_compile` decls, ~line 163), add:
  `int as_dump(ObjFn *fn, FILE *out);` and
  `ObjFn *as_load(const uint8_t *buf, int len);`
  (Add `#include <stdio.h>` at top of as.h for the `FILE` type, or forward via
  the existing includers — simplest: add `#include <stdio.h>` near as.h:10-13.)

### 1.2 New TU `/Users/wangzhe/ststem/src/apps/as/as_bc.c`
- `#include "as.h"`, `#include <stdio.h>`, `#include <string.h>`, `#include <stdlib.h>`.
- DUMP side (static helpers over `FILE*`):
  - `wr_u32(FILE*, uint32_t)` (LE), `wr_i64(FILE*, int64_t)` (LE),
    `wr_f64(FILE*, double)` (memcpy to u64, LE), `wr_bytes(FILE*, const void*, int)`.
  - `dump_fn(FILE *out, ObjFn *fn)`:
    arity (u32), upvalue_count (u32), name_len (u32 = fn->name?fn->name->len:0) +
    name bytes, code_len (u32 = fn->count) + fn->code bytes, kcount (u32) + each
    const. Const dispatch on `fn->consts[i].type`:
    V_NIL→tag0; V_BOOL→tag1+u8(as.i!=0); V_INT→tag2+i64(as.i);
    V_FLOAT→tag3+f64(as.f); V_OBJ→ if `IS_STR` tag4+u32 len+chars; else if
    `IS_FN` tag5 + recurse `dump_fn`; else set `as_err`="as_dump: unserializable
    constant" and return error. Propagate errors up.
  - `as_dump(ObjFn *fn, FILE *out)`: `fwrite("LAQ1",1,4,out)`, `wr_u32(out,
    AS_BC_VERSION)`, `return dump_fn(out, fn);` (return nonzero on any failure).
- LOAD side (static cursor `{const uint8_t *p; int rem;}` or `pos`/`len`):
  - bounds-checked `rd_u8/rd_u32/rd_i64/rd_f64/rd_bytes` returning a fail flag.
  - `load_fn(cursor*, int *err) -> ObjFn*`: `ObjFn *fn = as_fn_new();` (object.c:53),
    read arity→fn->arity; read upvalue_count→fn->upvalue_count; read name_len, if
    >0 read into a temp + `fn->name = as_str_copy(tmp,len)` (object.c:46); read
    code_len, `fn->code = malloc(len)`, memcpy, `fn->count = fn->cap = len`; read
    kcount, `fn->consts = malloc(kcount*sizeof(Value))`, `fn->kcount = fn->kcap =
    kcount`, loop reading each const by tag (tag5 → `OBJ_VAL(load_fn(...))`,
    tag4 → `OBJ_VAL(as_str_copy(...))`). On any read failure set `*err=1`.
  - `as_load(const uint8_t *buf, int len)`: if `len<8` return NULL; if
    `memcmp(buf,"LAQ1",4)` return NULL; read version (LE) at offset 4, if
    `!= AS_BC_VERSION` return NULL. `as_gc_push_disable();` (mirror
    compiler.c:780); `int err=0; ObjFn *fn = load_fn(&cur,&err);`
    `as_gc_pop_disable();` return `err ? NULL : fn`.
- Note: `fn->module` left NULL by `as_fn_new` (object.c:59) — caller stamps.

### 1.3 Makefile — add to core
- File `/Users/wangzhe/ststem/Makefile`, `AS_CORE` (line 248-250): append
  `src/apps/as/as_bc.c`. (The ring-3 `/bin/as` link uses `AS_C :=
  $(wildcard src/apps/as/*.c)` at line 187 — it picks up `as_bc.c`
  automatically; no change there.)

### 1.4 Host round-trip test
- New file `/Users/wangzhe/ststem/tools/t/as_bc_test.c` modeled on
  `/Users/wangzhe/ststem/tools/t/as_test.c` (its `ok`/`as_capture`/
  `as_free_objects` pattern, lines 11-24). For each case:
  1. `as_capture(bufA)`, `as_interpret(src)`, `as_capture(NULL)`; save bufA;
     `as_free_objects()`.
  2. `ObjFn *fn = as_compile(src);` `FILE *t = tmpfile();` `as_dump(fn,t);`
     `as_free_objects();` (free compile objects — proves load is independent).
  3. `fseek(t,0,SEEK_END)`/`ftell`/rewind; read all bytes into a malloc buffer;
     `ObjFn *fn2 = as_load(buf,len);` assert non-NULL.
  4. `as_capture(bufB)`, `as_run(fn2)`, `as_capture(NULL)`; assert
     `strcmp(bufA,bufB)==0`; `as_free_objects()`.
  Cases: arithmetic; fib recursion; list build+index; string concat/len; float
  literal print; nested `def` (FN const); a closure (`closure.as`-style counter);
  a dict literal. Plus negative: corrupt magic → `as_load` NULL; tamper version
  byte → NULL.
- Makefile: add target `test-as-bc` (model on `test-as`, lines 251-254):
  build `tools/t/as_bc_test.c` + `$(AS_CORE)` with the same flags, run it; and a
  `-DAS_GC_STRESS` variant (model on `test-as-gcstress`, 258-261). Wire both
  into the round-trip gate.

**Verify:** `make test-as-bc` and the gc-stress variant green; `make test-as` +
`make test-as-gcstress` still green (as_bc.c added to AS_CORE must not break
the existing link).

---

## Stage 2 — `as -c in.as -o out.la` CLI mode

**Gating test:** build host `asc` (Stage 3 rule, or a throwaway local `cc`), run
`asc -c fsroot/as/seq.as -o /tmp/seq.la`; assert the file is non-empty and
starts with "LAQ1"; then `as_load` it in a tiny host check (or extend
as_bc_test) and run — output sane.

### 2.1 `as.c` — `-c` branch
- File `/Users/wangzhe/ststem/src/apps/as/as.c`, in `main` (lines 24-42), insert
  the `argc==5 && argv[1]=="-c" && argv[3]=="-o"` branch from the spec BEFORE the
  existing run path. Uses the file-local `slurp` (as.c:9), `as_compile`,
  `as_dump` to `fopen(argv[4],"wb")`. Returns the dump rc.

**Verify:** the CLI path is exercised by the Stage 3 `.la` generation and the host
smoke check above.

---

## Stage 3 — host `asc` + stdlib `.la` generation + import `.la`-first + pack

**Gating test:** `make` builds clean; `make test-as-os` PASSES (greps `stdlib ok`
plus `squares`/`sorted`/`gcd/lcm`/`merged`), proving `import seq/math/dicts`
loaded `.la` on Aether.

### 3.1 import loader (`vm.c`)
- File `/Users/wangzhe/ststem/src/apps/as/vm.c`.
- Add static `module_try_la(ObjStr*)` and `stamp_module(ObjFn*, ObjModule*)` near
  `module_source` (line 129) per spec.
- Rewrite the body of `as_import` between line 336 (`as_gc_pop_disable()`) and
  line 343 (`m->state = 1`) to the `.la`-first / `.as`-fallback flow in the spec.
  Keep `module_find` cache check (330-331), the `nmodules>=64` guard (332), and
  the `as_module_new`+register+push/pop_disable block (333-336) unchanged.

### 3.2 host `asc` compiler (Makefile)
- File `/Users/wangzhe/ststem/Makefile`, after the `test-as` rule (line 254):
  ```
  ASC := $(BUILD)/asc
  $(ASC): $(AS_CORE) src/apps/as/as.c
  	@mkdir -p $(BUILD)
  	$(CC) -O2 -o $@ src/apps/as/as.c $(AS_CORE) -Isrc/apps/as -Iinclude/abi
  ```
  (`$(CC)` is `clang`, Makefile:26; with no `--target` it builds a native arm64
  host binary. `as_ll.c` provides the arm64 syscall stub; `-c` mode never calls
  it. `as_bc.c` is in `AS_CORE` from Stage 1.)

### 3.3 stdlib `.la` rules (Makefile)
- After the `$(ASC)` rule:
  ```
  AS_LA_SRCS := fsroot/as/math.as fsroot/as/seq.as \
                 fsroot/as/dicts.as fsroot/as/test.as
  AS_LA      := $(patsubst fsroot/as/%.as,$(BUILD)/%.la,$(AS_LA_SRCS))
  $(BUILD)/%.la: fsroot/as/%.as $(ASC)
  	$(ASC) -c $< -o $@
  ```
  (None of these four `import` anything — verified — so `-c` compile-only has no
  circular-import issue.)

### 3.4 pack `.la` into the disk (Makefile)
- `$(DISK)` rule (lines 213-219):
  - add `$(AS_LA)` to prerequisites (line 213).
  - append to the `mkfs.py` invocation (after the `$(AS_EXAMPLES)` foreach, 219):
    `$(foreach l,$(AS_LA),$(l):/usr/as/$(notdir $(l)))`
  - `.as` examples stay packed (unchanged foreach at 219) — demos + fallback.
- `clean` already does `rm -rf $(BUILD)` so `asc`/`*.la` are covered.

### 3.5 build the disk image
- Per project memory: `make` rebuilds only the kernel ISO; app/font/fsroot changes
  need `make build/disk.img` (the `$(DISK)` target). Ensure the gating test
  (`make test-as-os`) depends on `$(DISK)` (it does, Makefile:242) so `.la` files
  are regenerated and packed before the on-Aether run.

**Verify:**
- `make $(BUILD)/asc` builds; `$(ASC) -c fsroot/as/seq.as -o build/seq.la`
  emits "LAQ1…".
- `make build/disk.img` packs `/usr/as/{math,seq,dicts,test}.la`.
- `make test-as-os` green (stdlib loads via `.la`).

---

## Stage 4 — full regression + docs

**Gating:** all as tests + a full build green.

- Run: `make test-as`, `make test-as-gcstress`, `make test-as-bc`
  (+ gc-stress variant), `make test-as-os`. All green.
- `make` (full ISO+disk) builds clean, no warnings from `as_bc.c`
  (`-Wall -Wextra` in the host test rules).
- Optional: temporarily delete a `/usr/as/*.la` (or rename) and confirm the
  `.as` fallback still runs `make test-as-os` (proves graceful degradation).
- Update `/Users/wangzhe/ststem/CLAUDE.md` AetherScript notes + the user memory
  `aetherscript-design.md` to record M21 Phase 3 / LibAether `.la` (format, `as -c`,
  `.la`-first import, `AS_BC_VERSION`, host `asc` build step).

---

## Cross-stage invariants (do not regress)
- GC rooting: `as_load` keeps GC disabled across the whole load (object.c:14-30
  fires GC on alloc); `stamp_module` runs before `run_module` (NULL `->module`
  crashes OP_DEF_GLOBAL); `script` rooted by `run_module`'s `push` (vm.c:322).
- `->module` never serialized, re-stamped on load (object.c:311-312 — not GC-traced).
- Upvalue `{is_local,index}` pairs ride in the parent code stream (compiler.c:614,
  vm.c:726-734) — never a separate `.la` section.
- Endianness: host arm64 == target x86_64 == LE; raw `as.i`/`as.f` memcpy
  (document in an as_bc.c comment).
- `AS_BC_VERSION` bump invalidates all `.la` via the asc rebuild → regen chain;
  load-time guard rejects stale on-disk `.la`.
