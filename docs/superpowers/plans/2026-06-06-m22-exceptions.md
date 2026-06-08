---
title: M22.4 AetherScript Exceptions — implementation plan
status: plan
milestone: M22.4
date: 2026-06-06
---

# M22.4 — AetherScript Exceptions: implementation plan

Ordered, file-by-file. Implement steps 1-3 (lexer + opcodes), then 4-6 (VM),
then 7 (compiler), then 8 (tests). Build/verify after the VM steps and again at
the end. All line anchors are against the current tree (verified by reading the
files). Symbols are real.

The locked design is implemented exactly; see `feasibility_notes` for the small
clarifications that drove the precise form of steps 4-6.

---

## Step 1 — Lexer: token types (`src/apps/as/lexer.h`)

In the `TokType` enum (lexer.h:8-20), add three keyword tokens after `T_SUPER` on
line 11. Change:

```c
    T_DEF, T_RETURN, T_IF, T_ELIF, T_ELSE, T_CLASS, T_SUPER,   /* keywords */
```
to:
```c
    T_DEF, T_RETURN, T_IF, T_ELIF, T_ELSE, T_CLASS, T_SUPER,   /* keywords */
    T_TRY, T_EXCEPT, T_RAISE,                                   /* M22.4 exceptions */
```

(Appending mid-enum is fine: TokType values are never serialized; only `T_ERROR`
being last matters for `rules[T_ERROR+1]` in compiler.c:318, and that still holds.)

## Step 2 — Lexer: keyword recognition (`src/apps/as/lexer.c`)

In `keyword()` (lexer.c:23-47), add three entries by length:

- case 3 (after the `nil` line, lexer.c:33): add
  `if (!memcmp(s, "try", 3)) return T_TRY;`
- case 5 (after the `super` line, lexer.c:41): add
  `if (!memcmp(s, "raise", 5)) return T_RAISE;`
- case 6 (after the `lambda` line, lexer.c:44): add
  `if (!memcmp(s, "except", 6)) return T_EXCEPT;`

## Step 3 — Opcodes (`src/apps/as/as.h`)

In the `OpCode` enum (as.h:143-156), append the three new opcodes at the END, after
`OP_GET_SUPER,` on line 155 (keeps every existing opcode number stable):

```c
    OP_GET_PROPERTY, OP_SET_PROPERTY, OP_GET_SUPER,
    OP_SETUP_TRY, OP_POP_TRY, OP_RAISE,                         /* M22.4 exceptions */
} OpCode;
```

---

## Step 4 — VM globals + throw/ensure helpers + rerouted runtime_error (`src/apps/as/vm.c`)

### 4a. New globals

After the open-upvalues / builtins globals (insert after vm.c:24, the
`static int nbuiltins;` line is at 24; place this block right after it, before the
`modules[]` block at 26, OR equivalently after line 29 — anywhere in file-scope
before `as_vm_mark_roots`). Add:

```c
/* M22.4 exceptions: a handler stack + a pending-exception slot.
 * Each handler entry is captured when OP_SETUP_TRY runs; it records where to
 * resume (handler_ip), the value-stack level to restore (sp), and which frame
 * owned the try (frame_index = frame_count at setup time). The entries hold no
 * Obj pointers, so the GC never traces them. */
typedef struct { uint8_t *handler_ip; Value *sp; int frame_index; } Handler;
static Handler handler_stack[FRAMES_MAX];
static int     handler_count;
static Value   g_exc;       /* the pending thrown value (valid iff g_has_exc) */
static int     g_has_exc;   /* 1 while an exception is propagating */
```

`g_native_err` already exists at vm.c:69 — keep it.

### 4b. Initialize in `reset_stack` (vm.c:44)

Change:
```c
static void reset_stack(void) { sp = stack; frame_count = 0; open_upvalues = NULL; }
```
to:
```c
static void reset_stack(void) { sp = stack; frame_count = 0; open_upvalues = NULL;
                                handler_count = 0; g_has_exc = 0; g_exc = NIL_VAL; g_native_err = 0; }
```

(`g_native_err` is declared at vm.c:69, AFTER reset_stack at vm.c:44. Move the
declaration `static int g_native_err;` up to the new-globals block in 4a so it is in
scope for reset_stack, and delete the duplicate declaration at vm.c:69 — keep
`as_native_fail` at vm.c:70 which assigns it.)

### 4c. GC root (vm.c:47-57, `as_vm_mark_roots`)

After the modules loop (vm.c:56), before the closing brace at vm.c:57, add:
```c
    if (g_has_exc) gc_mark_value(g_exc);
```
No marking for `handler_stack` (no Obj refs). Confirmed against object.c blacken()
(O_STR/O_INSTANCE already fully traced) — no new ObjType, no blacken/free_object
changes.

### 4d. `throw_value` + `ensure_exc` helpers, and reroute `runtime_error`

`runtime_error` is at vm.c:59-65 and calls (will call) `throw_value`, so declare
`throw_value` ABOVE it. Insert immediately before `runtime_error` (before vm.c:59):

```c
/* Set the pending exception. Returns 1 (the uniform error signal) so callers can
 * `return throw_value(v)` or fall into `goto err`. */
static int throw_value(Value v) { g_exc = v; g_has_exc = 1; return 1; }

/* At the error label: if a C-level error (built-in or native) left a message in
 * as_err but no exception value was set, wrap as_err into a catchable string
 * exception. Keyed on g_has_exc (NOT g_native_err) so a native failure reaching
 * `err:` via call_value's return value is converted here. */
static void ensure_exc(void)
{
    if (!g_has_exc) g_exc = OBJ_VAL(as_str_copy(as_err, (int)strlen(as_err))), g_has_exc = 1;
}
```

Then change `runtime_error` (vm.c:59-65) so that, after formatting as_err, it
ALSO sets the pending exception (so internal errors become catchable). Replace the
body's `return 1;` (vm.c:64) with:
```c
    return throw_value(OBJ_VAL(as_str_copy(as_err, (int)strlen(as_err))));
```
Keep the `vsnprintf(as_err, ...)` line so the uncaught path and the host `err()`
test still observe the message in `as_err`.

Note: `runtime_error` is called from `call_fn`/`call_closure`/`call_value`
(vm.c:190-243) and ~59 sites inside `run_until`. At all of them it now sets g_exc
in addition to returning 1; the existing `goto err` / `return 1` flow is unchanged
— `goto err` simply becomes "start unwinding". `as_str_copy` is already used in
vm.c (e.g. line 218) and is GC-safe (object.c:14-30 collects BEFORE allocating, so
the fresh exception string is never swept on its own creation).

---

## Step 5 — VM run loop: error-label restructure + new opcodes (`src/apps/as/vm.c`)

### 5a. Restructure the error label to allow RESUME

Current structure (vm.c:326-712):
```c
    for (;;) {
        uint8_t op = READ_BYTE();
        switch (op) {
        ... all cases ...
        default: runtime_error("bad opcode %d", op); goto err;
        }
    }                              /* <-- closes the for loop, vm.c:707 */
err:                               /* <-- OUTSIDE the loop, vm.c:708 */
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONST
    return 1;
```

Restructure so `err:` is the LAST statement INSIDE the loop body, after the switch's
closing brace (after vm.c:706) but before the for-loop's closing brace (vm.c:707):

```c
    for (;;) {
        uint8_t op = READ_BYTE();
        switch (op) {
        ... all cases unchanged, incl. the new SETUP_TRY/POP_TRY/RAISE from 5b ...
        default: runtime_error("bad opcode %d", op); goto err;
        }
        continue;                  /* normal completion of an opcode -> next op */
    err:
        ensure_exc();              /* fold native/built-in errors into g_exc */
        /* find the nearest live handler: the topmost entry whose frame is still on
         * the stack (frame_index <= frame_count). Discard handlers above it. */
        while (handler_count > 0 && handler_stack[handler_count - 1].frame_index > frame_count)
            handler_count--;
        if (handler_count > 0) {
            Handler *h = &handler_stack[--handler_count];   /* pop the handler we'll use */
            close_upvalues(h->sp);                          /* close captives above the try's sp */
            frame_count = h->frame_index;
            frame = &frames[frame_count - 1];
            frame->ip = h->handler_ip;                      /* resume at the except block */
            sp = h->sp;
            push(g_exc);                                    /* hand the value to the except block */
            g_has_exc = 0;
            continue;                                       /* RESUME dispatch */
        }
        /* uncaught: finalize as_err for the caller, then abort. */
        if (IS_STR(g_exc)) {
            ObjStr *s = AS_STR(g_exc);
            int n = s->len < (int)sizeof(as_err) - 1 ? s->len : (int)sizeof(as_err) - 1;
            memcpy(as_err, s->chars, (size_t)n); as_err[n] = 0;
        } else if (as_err[0] == 0) {
            snprintf(as_err, sizeof as_err, "uncaught exception");
        }
        g_has_exc = 0;
        break;                                              /* leave the for loop -> return 1 */
    }
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONST
    return 1;
```

Key points:
- The `continue;` before `err:` ensures normal opcode completion never falls into
  the error label (each case still ends in `break`, which breaks the switch, then
  hits the `continue`).
- The handler search uses `frame_index > frame_count` to discard handlers belonging
  to frames already unwound (none, on a fresh throw, since frame_count hasn't
  changed yet — but this keeps it correct if the loop is re-entered). The chosen
  handler is the topmost with `frame_index <= frame_count`.
- `close_upvalues(h->sp)` mirrors OP_RET's `close_upvalues(frame->slots)`: it must
  run BEFORE `sp = h->sp` so captured locals above the try's sp are closed.
- `frame = &frames[frame_count - 1]` re-seats the local `frame` pointer after
  `frame_count` changes; READ_BYTE reads `frame->ip`, so setting `frame->ip` then
  `continue` resumes at the except block.
- For uncaught NON-string g_exc (e.g. `raise 42` with no handler), as_err keeps
  whatever the last C error wrote, or a generic "uncaught exception" — acceptable;
  the host `err()` test only checks the return code is nonzero.

### 5b. New opcode cases (inside the switch, place after OP_RET's case at vm.c:432, before OP_MAKE_LIST at vm.c:434)

```c
        case OP_SETUP_TRY: {
            uint16_t off = READ_SHORT();                 /* forward offset to the except block */
            if (handler_count >= FRAMES_MAX) { runtime_error("too many nested try blocks"); goto err; }
            handler_stack[handler_count].handler_ip = frame->ip + off;
            handler_stack[handler_count].sp = sp;
            handler_stack[handler_count].frame_index = frame_count;
            handler_count++;
            break;
        }
        case OP_POP_TRY: {
            uint16_t off = READ_SHORT();                 /* forward offset past the except block */
            if (handler_count > 0) handler_count--;      /* try body finished normally: drop handler */
            frame->ip += off;                            /* skip the except block */
            break;
        }
        case OP_RAISE: {
            Value v = pop();
            throw_value(v);
            goto err;
        }
```

`READ_SHORT()` (vm.c:323) advances `frame->ip` past the 2 operand bytes, so the
recorded `frame->ip + off` / `frame->ip += off` use offsets relative to AFTER the
operand — identical to OP_JUMP at vm.c:410. The compiler's `emitJump`/`patchJump`
(compiler.c:64-71) produce exactly this convention, so OP_SETUP_TRY/OP_POP_TRY are
patched with the same helpers.

---

## Step 6 — OP_RET: drop handlers owned by the returning frame (`src/apps/as/vm.c`)

OP_RET is at vm.c:420-432. After `close_upvalues(frame->slots);` (vm.c:425) and
BEFORE `frame_count--;` (vm.c:426), insert:

```c
            /* return-from-inside-try: discard handlers registered in this frame,
             * else a dead frame's handler would dangle and mis-catch later. */
            while (handler_count > 0 && handler_stack[handler_count - 1].frame_index >= frame_count)
                handler_count--;
```

At this point `frame_count` is still the returning frame's index (pre-decrement), so
`frame_index >= frame_count` matches handlers set up in this frame (the same frame)
or any deeper — there are none deeper, since deeper frames already returned and
cleaned theirs. This is the locked design's "OP_RET must pop any handlers belonging
to the frame being returned".

---

## Step 7 — Compiler: raise + try/except (`src/apps/as/compiler.c`)

### 7a. Statement dispatch (compiler.c:721-731, `statement`)

Add two cases. After the `else if (match(T_FROM)) from_statement();` line
(compiler.c:728), add:
```c
    else if (match(T_TRY)) try_statement();
    else if (match(T_RAISE)) raise_statement();
```

Forward-declare both with the other statement forward decls near compiler.c:144-149
(add `static void try_statement(void);` and `static void raise_statement(void);`),
or define them above `statement`. Simplest: define them just before `statement`
(after `class_declaration`/`block`, before compiler.c:721). Pick the define-above
approach to avoid extra forward decls.

### 7b. `raise_statement`

```c
static void raise_statement(void)   /* entered after `raise` */
{
    expression();                                   /* the value to throw */
    emit(OP_RAISE);
    consume(T_NEWLINE, "expected a newline after 'raise'");
}
```

`expression()` (compiler.c:366) parses the operand; OP_RAISE pops it at run time.

### 7c. `try_statement`

Mirror `if_statement` (compiler.c:434-447) for the jump bookkeeping and `block`
(compiler.c:711-719) for the body scope. The except-binding reuses `begin_scope` /
`add_local` / `end_scope` (compiler.c:123-132, 87-97):

```c
static void try_statement(void)   /* entered after `try` */
{
    consume(T_COLON, "expected ':' after 'try'");
    int setup = emitJump(OP_SETUP_TRY);   /* operand = forward offset to the except block */
    block();                              /* try body in its own scope (locals popped at block end) */
    int done = emitJump(OP_POP_TRY);      /* body ok: pop handler, jump past the except block */
    patchJump(setup);                     /* SETUP_TRY -> here = start of the except block */

    consume(T_EXCEPT, "expected 'except' after the 'try' block");
    /* At runtime the thrown value sits on the stack at handler->sp (i.e. the top). */
    int bound = 0;
    if (check(T_IDENT)) {                 /* except NAME:  -> bind the value to a local */
        Token name = tk_cur(); advance();
        begin_scope();
        add_local(name.start, name.len);  /* the value already occupies this new slot */
        bound = 1;
    } else {
        emit(OP_POP);                     /* except:  -> discard the value */
    }
    consume(T_COLON, "expected ':' after 'except'");
    block();                              /* the handler body */
    if (bound) end_scope();              /* pop the bound exception local (OP_POP/OP_CLOSE_UPVALUE) */

    patchJump(done);                      /* POP_TRY -> here = past the whole try/except */
}
```

Why the slot accounting agrees with the runtime sp restore:
- OP_SETUP_TRY captures `handler->sp = sp` at the point the try is entered, which is
  the base for the try body's locals. The body's `block()` opens its own scope and
  `end_scope` emits OP_POP/OP_CLOSE_UPVALUE for each body local, so on NORMAL
  completion sp is back to handler->sp before OP_POP_TRY.
- On a THROW, the run loop hard-restores `sp = h->sp` then `push(g_exc)`, so the
  value lands exactly where the compiler expects: for `except NAME:`, the new local
  added by `add_local` resolves to that slot (it is `current->local_count` at this
  point, which equals the count at SETUP_TRY since body locals were popped); for
  `except:`, OP_POP discards it. The compiler's `local_count` at the except label
  equals what it was at SETUP_TRY, so slot math and runtime sp agree.
- `begin_scope`/`end_scope` around the bound name guarantee the handler's local is
  cleaned up (its OP_POP runs on normal handler exit), leaving the stack balanced
  for the code after the construct.

Constraint to honor (no extra code needed, just don't break it): `block()` already
pops try-body locals; do NOT add an explicit pop. The single `except` clause is
mandatory — `consume(T_EXCEPT, ...)` enforces it (matches the locked "must be
followed by exactly one except").

---

## Step 8 — Tests

### 8a. Host unit tests (`tools/t/as_test.c`)

Add the following `ok()`/`err()` cases (use the existing `ok(name, src, want)` and
`err(name, src)` helpers, near the class tests at the tail of `main`). Each is also
exercised under `-DAS_GC_STRESS` automatically by `make test-as-gcstress`.

```c
/* M22.4 exceptions */
ok("raise_catch_str",
   "try:\n    raise \"x\"\nexcept e:\n    print(e)\n", "x\n");
ok("catch_runtime",
   "try:\n    print(1 / 0)\nexcept e:\n    print(\"caught\")\n", "caught\n");
ok("raise_across_call",
   "def boom():\n    raise \"kaboom\"\n"
   "try:\n    boom()\nexcept e:\n    print(\"caught:\", e)\n",
   "caught: kaboom\n");
ok("nested_try_inner",
   "try:\n    try:\n        raise \"a\"\n    except i:\n        print(\"inner\", i)\n"
   "except o:\n    print(\"outer\", o)\n",
   "inner a\n");
ok("nested_try_outer",                 /* inner try, no error there; outer catches */
   "try:\n    try:\n        print(\"ok\")\n    except i:\n        print(\"inner\")\n"
   "    raise \"b\"\n"
   "except o:\n    print(\"outer\", o)\n",
   "ok\nouter b\n");
ok("except_binding_int",
   "try:\n    raise 42\nexcept e:\n    print(e + 1)\n", "43\n");
ok("except_no_binding",
   "try:\n    raise \"z\"\nexcept:\n    print(\"handled\")\n", "handled\n");
ok("try_no_error",
   "try:\n    print(\"body\")\nexcept e:\n    print(\"skip\")\nprint(\"end\")\n",
   "body\nend\n");
ok("control_after",
   "try:\n    raise \"q\"\nexcept e:\n    print(\"h\")\nprint(\"after\")\n",
   "h\nafter\n");
ok("raise_instance_field",
   "class Err:\n    def init(self, m):\n        self.msg = m\n"
   "try:\n    raise Err(\"bad\")\nexcept e:\n    print(e.msg)\n",
   "bad\n");
ok("return_from_try",                  /* return inside try cleans its handler */
   "def f():\n    try:\n        return 1\n    except e:\n        return 2\n"
   "print(f())\n", "1\n");
err("uncaught_raise", "raise \"boom\"\n");
err("uncaught_in_try_body_no_match",   /* throw inside except handler is not re-caught */
    "try:\n    raise \"a\"\nexcept e:\n    raise \"b\"\n");
```

(`nested_try_outer`'s `raise "b"` is in the OUTER try body, after the inner
try/except completes, proving the inner handler was popped and the outer one
catches.)

### 8b. On-Aether end-to-end

Create `fsroot/as/exc.as`:
```
# exceptions demo (M22.4): raise+catch, and catch a built-in runtime error
try:
    raise "boom"
except e:
    print("caught:", e)
try:
    x = 1 / 0
except e:
    print("runtime caught")
print("exc ok")
```

In `scripts/run-as-test.sh`:
- Add `as /usr/as/exc.as\n` to the here-doc command list (the
  `printf 'as /usr/as/...\n...exit\n'` block).
- Add a grep marker to the success condition (the big `if grep -aq ... ` chain):
  `&& grep -aq "exc ok" "$LOG"` and `&& grep -aq "caught: boom" "$LOG"` and
  `&& grep -aq "runtime caught" "$LOG"`.
- Update the PASS echo string to mention `+exceptions`.

The packaging note: `/usr/as/*.as` on the disk is produced by `tools/mkfs.py`
from `fsroot/as/` (per CLAUDE.md the disk maps fsroot). Confirm `exc.as` lands at
`/usr/as/exc.as` (the existing examples like `gc.as` are referenced as
`/usr/as/gc.as`); if the mkfs mapping differs, mirror exactly how `gc.as` is
placed. Rebuild the disk image (`make build/disk.img`, per MEMORY: app/font/fsroot
changes need the disk rebuilt, not just the ISO) before running `make test-as-os`.

---

## Build / verify sequence

1. After steps 1-7: `make test-as` (host unit suite — must pass, incl. the 13 new
   cases).
2. `make test-as-gcstress` (same suite with collect-before-every-alloc — proves
   `g_exc` rooting and the fresh-exception-string allocation are GC-safe; this is
   the key regression guard for the new GC root in step 4c).
3. After step 8b: `make build/disk.img && make test-as-os` (boots Aether, runs
   `/bin/as` on the examples incl. `exc.as`, asserts markers over serial).
4. `make` / `make test` to confirm the kernel still builds and boots (the as core
   is also linked into `/bin/as`; no kernel-side change, but verify nothing
   regressed).

## File-change summary

| File | Change |
|------|--------|
| `src/apps/as/lexer.h` | +3 token types (`T_TRY`, `T_EXCEPT`, `T_RAISE`) after `T_SUPER` |
| `src/apps/as/lexer.c` | +3 keyword entries in `keyword()` (cases 3/5/6) |
| `src/apps/as/as.h` | +3 opcodes at end of `OpCode` (`OP_SETUP_TRY`,`OP_POP_TRY`,`OP_RAISE`) |
| `src/apps/as/vm.c` | new globals (Handler stack, g_exc/g_has_exc; move g_native_err up); reset_stack init; mark-roots +1 line; throw_value/ensure_exc; reroute runtime_error; restructure err: label inside loop; 3 new opcode cases; OP_RET handler cleanup |
| `src/apps/as/compiler.c` | `raise_statement` + `try_statement`; dispatch in `statement()` |
| `tools/t/as_test.c` | +13 ok()/err() exception cases |
| `fsroot/as/exc.as` | new on-Aether example |
| `scripts/run-as-test.sh` | run exc.as + grep markers + PASS string |

No changes to `object.c` (no new ObjType; existing O_STR/O_INSTANCE GC tracing and
alloc_obj collect-before-alloc cover exceptions), `value.c`, or the Makefile.
