---
title: LibAqua .la compiled-bytecode format (M21 phase 3)
status: spec
date: 2026-06-07
---

# LibAqua: the `.la` Compiled-Bytecode Library Format (M21 Phase 3)

## Goal

`import seq` loads a precompiled `seq.la` (header + serialized bytecode) and runs
it **identically** to running `seq.aqs` from source — no lex, no compile. A new
`aqs -c in.aqs -o out.la` produces it. The stdlib `.aqs` modules are precompiled
to `.la` at `make` time and packed to `/usr/aqs/*.la`; `import` prefers `.la`.

This is the bytecode-serialization step of self-hosting: a module compiles to one
top-level `ObjFn` whose body defines the module's globals + functions (nested
`def`s are `FN` constants in its const pool). Serialize that tree; deserialize it
back into a runnable `ObjFn`.

## File format (`.la`, little-endian throughout)

Both the host (arm64) and target (x86_64) are little-endian, so all integers and
the IEEE-754 double are written as raw LE bytes (memcpy of `Value.as.i` /
`Value.as.f`, aqs.h:21).

### Header
```
offset 0:  4 bytes   magic  "LAQ1"            (0x4C 0x41 0x51 0x31)
offset 4:  u32       AQS_BC_VERSION            (LE)
offset 8:  <serialized top-level ObjFn>        (recursive, see below)
```
`aqs_load` returns NULL unless the magic matches AND the version equals the
compiled-in `AQS_BC_VERSION`. There is no length field; the reader is structural.

### Serialized ObjFn (recursive)
```
u32   arity                              (fn->arity)
u32   upvalue_count                      (fn->upvalue_count — METADATA ONLY)
u32   name_len                           (0 if fn->name == NULL)
u8[]  name_bytes                         (name_len bytes; ObjStr->chars, no NUL)
u32   code_len                           (fn->count)
u8[]  code_bytes                         (fn->code[0..count-1] verbatim)
u32   const_count                        (fn->kcount)
<const_count constants>                  (each: tag byte + payload)
```

**Upvalue descriptors are NOT serialized separately.** The `{is_local u8,
index u8}` capture pairs are emitted inline into the *parent* function's code
stream immediately after each `OP_CLOSURE` + const-index byte (compiler.c:613-614)
and are read from the code stream by the VM at runtime (vm.c:726-734). Because the
parent's `code` bytes are serialized verbatim, they round-trip automatically.
`upvalue_count` is written only as metadata; `aqs_load` sets `fn->upvalue_count`
from it (needed so `aqs_closure_new` sizes the upvalue array, object.c:64-74) and
does **not** reconstruct any descriptor array (none exists on `ObjFn`).

### Constant entries (tag byte + payload)
| tag | meaning | payload |
|-----|---------|---------|
| 0 `NIL`   | V_NIL   | (none) |
| 1 `BOOL`  | V_BOOL  | u8 (0/1, from `as.i`) |
| 2 `INT`   | V_INT   | i64 LE (`as.i`) |
| 3 `FLOAT` | V_FLOAT | 8-byte double LE (`as.f`) |
| 4 `STR`   | V_OBJ/O_STR | u32 len + len bytes (`ObjStr->chars`; hash recomputed on load) |
| 5 `FN`    | V_OBJ/O_FN  | recurse (serialized nested ObjFn) |

The compiler only ever emits these six value kinds into a const pool (literals via
`makeConst`, nested `def`s as `FN` via OP_CLOSURE). `aqs_dump` treats any other
`V_OBJ` subtype (O_NATIVE/O_LIST/O_DICT/O_CLOSURE/etc.) as a hard error — natives
hold raw C function pointers (aqs.h:76) and are unserializable. These never occur
today; the guard protects against future churn.

`ObjStr->hash` is **not** serialized (object.c:48); `aqs_load` rebuilds strings
via `aqs_str_copy(chars, len)` which recomputes the FNV hash (object.c:46-51,
32-37). `ObjFn->module`, `cap`/`kcap`, and `Obj` GC internals are **not**
serialized.

## API additions (`aqs.h`)

Near the `OpCode` enum (aqs.h:143), add:
```c
#define AQS_BC_VERSION 1u   /* bump on ANY opcode add/reorder or .la-format change */
```
In the public API section (near aqs.h:160-163, alongside `aqs_compile`), add:
```c
int    aqs_dump(ObjFn *fn, FILE *out);            /* write header + tree; 0=ok, nonzero=error */
ObjFn *aqs_load(const uint8_t *buf, int len);     /* deserialize; NULL on bad magic/version/EOF */
```
`<stdio.h>` is already pulled in by the TUs that use these (aqs.c, vm.c); the new
implementation TU includes it. Both are **non-static** (used by aqs.c CLI + vm.c
loader + the host test).

### `aqs_dump(ObjFn *fn, FILE *out)`
1. `fwrite` magic "LAQ1" (4 bytes) + `AQS_BC_VERSION` (u32 LE).
2. Recursively serialize `fn` per the layout above. Helper writers: `wr_u32`,
   `wr_i64`, `wr_f64`, `wr_bytes` over the `FILE*`. The const dispatch maps
   `Value.type`/object type to a tag; for `FN` it recurses; for any
   non-O_STR/O_FN object it returns an error (sets `aqs_err`).
3. Returns 0 on success, nonzero on a write error or an unserializable constant.

### `aqs_load(const uint8_t *buf, int len)`
1. Bounds-checked cursor over `buf[0..len)`. If <8 bytes, or magic != "LAQ1", or
   version != `AQS_BC_VERSION`, return NULL.
2. `aqs_gc_push_disable()` (mirrors compiler.c:780 — the tree is built incrementally
   and is unrooted until returned). Wrap the entire load.
3. Recursively `load_fn`: `aqs_fn_new()` (object.c:53), then set `arity`,
   `upvalue_count`, `name` (via `aqs_str_copy` if name_len>0, else NULL),
   allocate+memcpy `code`/`count` (also set `cap=count`), build `consts`/`kcount`
   (also set `kcap=kcount`) by reading each tagged const — `FN` recurses,
   `STR` via `aqs_str_copy`. `fn->module` stays NULL (stamped by the caller).
   `consts`/`code` are `malloc`'d (freed by `free_object`, object.c:386).
4. Any short read / bad tag / unexpected EOF => abort: `aqs_gc_pop_disable()` and
   return NULL. (Partially built nodes are on `g_objs` and reclaimed by the next
   `aqs_free_objects`/GC — acceptable; the run aborts.)
5. On success: `aqs_gc_pop_disable()`, return the top-level ObjFn (module==NULL).

## CLI: `aqs -c in.aqs -o out.la`

In `src/apps/aqs/aqs.c` `main` (aqs.c:24-42), before the run fall-through, add:
```c
if (argc == 5 && strcmp(argv[1], "-c") == 0 && strcmp(argv[3], "-o") == 0) {
    FILE *f = fopen(argv[2], "r");
    if (!f) { aqs_emit_cstr("aqs: cannot open "); aqs_emit_cstr(argv[2]); aqs_emit_cstr("\n"); return 1; }
    char *src = slurp(f); fclose(f);
    if (!src) { aqs_emit_cstr("aqs: out of memory\n"); return 1; }
    ObjFn *fn = aqs_compile(src);            /* canonical standalone ObjFn (compiler.c:808) */
    free(src);
    if (!fn) { aqs_emit_cstr("aqs: "); aqs_emit_cstr(aqs_err); aqs_emit_cstr("\n"); aqs_free_objects(); return 1; }
    FILE *out = fopen(argv[4], "wb");
    if (!out) { aqs_emit_cstr("aqs: cannot create "); aqs_emit_cstr(argv[4]); aqs_emit_cstr("\n"); aqs_free_objects(); return 1; }
    int rc = aqs_dump(fn, out);
    fclose(out);
    aqs_free_objects();
    return rc;
}
```
`aqs.c` compiles into BOTH the host `aqsc` binary and the on-Aqua `/bin/aqs`, so
both get this mode. Uses `aqs_compile` (not `aqs_compile_module`) so `->module`
is the throwaway `__main__`, which `aqs_dump` skips.

## Import: `.la`-first loader (`vm.c`)

In `aqs_import` (vm.c:328-345), insert a `.la` probe between module registration
(vm.c:336, after `aqs_gc_pop_disable`) and the existing `module_source` call
(vm.c:337). The in-memory test registry (vm.c:131-137, inside `module_source`)
stays untouched and is reached only on the `.aqs` fallback.

New static helpers in vm.c (near `module_source`, vm.c:129):
```c
/* Try NAME.la then /usr/aqs/NAME.la; on success aqs_load and return the ObjFn.
 * GC stays disabled across the load (mirrors aqs_compile_module). NULL if absent/bad. */
static ObjFn *module_try_la(ObjStr *name) {
    char path[160];
    const char *dirs[] = { "", "/usr/aqs/" };
    for (int d = 0; d < 2; d++) {
        int p = 0;
        for (const char *pre = dirs[d]; *pre && p < 120; pre++) path[p++] = *pre;
        for (int i = 0; i < name->len && p < 150; i++) path[p++] = name->chars[i];
        const char *ext = ".la"; for (int i = 0; i < 3 && p < 156; i++) path[p++] = ext[i];
        path[p] = 0;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t cap = 4096, len = 0; uint8_t *buf = malloc(cap);
        for (;;) { if (len + 4096 + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
                   size_t r = fread(buf + len, 1, 4096, f); len += r; if (r < 4096) break; }
        fclose(f);
        ObjFn *fn = aqs_load(buf, (int)len);   /* aqs_load does its own push/pop_disable */
        free(buf);
        if (fn) return fn;                      /* bad magic/version -> NULL -> fall through to .aqs */
    }
    return NULL;
}
/* Recursively stamp ->module on fn and every O_FN constant (mirrors compiler g_module). */
static void stamp_module(ObjFn *fn, ObjModule *m) {
    fn->module = m;
    for (int i = 0; i < fn->kcount; i++)
        if (IS_FN(fn->consts[i])) stamp_module(AS_FN(fn->consts[i]), m);
}
```
`aqs_import` new flow (replacing vm.c:337-342):
```c
ObjFn *script = module_try_la(name);          /* .la first */
if (script) {
    stamp_module(script, m);                  /* pointer writes only — GC-safe */
} else {
    char *src = module_source(name);          /* .aqs fallback (+ in-memory registry) */
    if (!src) { runtime_error("cannot import module '%.*s'", name->len, name->chars); return NULL; }
    script = aqs_compile_module(src, m);       /* stamps ->module via g_module */
    free(src);
    if (!script) return NULL;
}
if (run_module(script)) return NULL;          /* run_module pushes script -> rooted (vm.c:322) */
m->state = 1;
return m;
```
Rooting: `m` is already in `modules[]` (vm.c:335) so it is a GC root before the
probe. `stamp_module` allocates nothing. `script` becomes rooted the instant
`run_module` runs `push(OBJ_VAL(script))` (vm.c:322), and no allocation occurs
between `aqs_load` returning and that push — same safety profile as the compile
path. `stamp_module` MUST run before `run_module` because the first
`OP_DEF_GLOBAL` resolves through `frame->fn->module` (a NULL module crashes).

## Build pipeline

`fsroot/aqs/{math,seq,dicts,test}.aqs` are precompiled to `build/*.la` by a HOST
`aqsc` binary at `make` time, then packed to `/usr/aqs/*.la`. The host (arm64 LE)
and target (x86_64 LE) compile from the same `AQS_CORE` source => identical
`AQS_BC_VERSION` and opcode enum, so the host-produced `.la` loads on Aqua. The
`.aqs` source files stay packed too (demos; also the fallback). Because `import`
prefers `.la`, `stdlib.aqs`'s `import seq`/`math`/`dicts` load the `.la` files.

`.la` is a pure build artifact: any opcode/format change rebuilds `aqsc` (it
depends on all of `AQS_CORE`, which includes `aqs.h`), which regenerates every
`.la`; the version guard rejects any stale on-disk `.la`.

## Scope / deferrals (locked — do not expand)

- Only the top-level module `ObjFn` tree is serialized. No module *state*
  (resolved globals/imports) — that is rebuilt by running the loaded bytecode.
- Const tags 0–5 only. Native/list/dict/closure/class constants are a hard dump
  error (they never appear from the compiler).
- No separate upvalue-descriptor section (redundant with the code stream).
- No cross-endian support (host==target==LE; documented in a code comment).
- No `.la` for `stdlib.aqs` (it's a demo, kept as source).
- No `fmemopen` dependency; `aqs_dump` takes `FILE*`, host test uses a tmpfile.

## Test plan

1. **Host round-trip / fixpoint** (gates stages 1–3): for representative sources
   (arithmetic, `def`+recursion e.g. fib, lists, closures, dict, nested defs,
   strings, float consts), (a) `aqs_compile` + run-and-capture output A, free
   objects; (b) `aqs_compile` + `aqs_dump` to a tmpfile, free objects, read file
   back, `aqs_load`, `aqs_run`-and-capture output B; assert A == B. Run under both
   normal and `-DAQS_GC_STRESS` (proves load-time GC rooting). Also assert
   `aqs_load` returns NULL on a corrupted magic and on a bumped version byte.
2. **CLI smoke** (host): `aqsc -c fsroot/aqs/seq.aqs -o build/seq.la` produces a
   non-empty file starting with "LAQ1"; `aqsc -c` of every stdlib module succeeds.
3. **On-Aqua** (`make test-aqs-os` => `scripts/run-aqs-test.sh`): unchanged — it
   already runs `stdlib.aqs` and greps `"stdlib ok"`. Once `/usr/aqs/*.la` exist,
   `import seq`/`math`/`dicts` load the `.la` path; the marker validates the whole
   on-Aqua `.la` chain end-to-end. The other greps (`squares`, `sorted`,
   `gcd/lcm`, `merged`) confirm the loaded modules behave identically to source.
4. **Regression**: `make test-aqs` (existing 100 checks) + `make test-aqs-gcstress`
   stay green; `make` builds the full ISO+disk with `.la` packed.
