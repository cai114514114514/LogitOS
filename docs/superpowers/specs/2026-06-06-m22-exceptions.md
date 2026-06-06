---
title: M22.4 AquaScript Exceptions
status: spec
milestone: M22.4
date: 2026-06-06
---

# M22.4 — AquaScript Exceptions (raise / try / except)

Status: spec. Milestone M22.4 (the "advanced language features" arc: closures done,
GC done, classes done, now exceptions). Targets the from-scratch AquaScript stack
bytecode VM in `src/apps/aqs/`.

## Goal

Add catchable exceptions to AquaScript so a script can `raise` an arbitrary value,
catch it with `try/except`, and so that BUILT-IN runtime errors (e.g. division by
zero, undefined variable, bad native argument) become catchable instead of always
aborting the program. This is the systems-language counterpart to Python's
`try/except`: control transfer across the call stack, with the thrown value
available to the handler.

## Syntax (Python-ish, indentation blocks)

```
raise EXPR                 # throw any Value (string, instance, int, ...)

try:
    BODY
except:                    # catch-all, no binding
    HANDLER

try:
    BODY
except NAME:               # catch-all, binds the thrown value to local NAME
    HANDLER
```

- `raise EXPR` evaluates EXPR and throws the resulting Value. Any Value is throwable
  (string, instance, int, float, nil, list, ...). There is no required base class.
- `try:` introduces a guarded block (an indented block, exactly like `if`/`while`
  bodies). It MUST be followed by exactly one `except` clause.
- `except:` catches any thrown value; the value is discarded.
- `except NAME:` catches any thrown value and binds it to a fresh local `NAME`
  visible only inside the handler block.

## Semantics

1. Entering a `try` registers a handler. If the BODY completes normally, the handler
   is discarded and execution continues after the `except` block (the except block
   is skipped).
2. If a value is thrown anywhere during BODY — by an explicit `raise`, by a built-in
   runtime error, by a native-function error, or by a `raise`/error in a function
   CALLED (transitively) from BODY — execution unwinds the call stack to the nearest
   enclosing `try` whose BODY was active, restores the value stack to its state at
   that `try`, and transfers control to that `try`'s `except` block.
3. In the handler, for `except NAME:` the thrown value is bound to `NAME`; for
   `except:` it is discarded. After the handler block runs, execution continues with
   the code following the whole `try/except` construct.
4. Handlers nest: an inner `try` is searched before an outer one. A throw inside an
   `except` handler is NOT caught by that same `try` (its handler was already popped
   when control entered the handler); it propagates outward.
5. If no enclosing handler exists, the throw is uncaught and the program aborts with
   an error, exactly as built-in runtime errors do today. The uncaught value's text
   (its string content, or a generic message for non-strings) is reported via the
   existing `aqs_err` channel.
6. Built-in runtime errors are unified with `raise`: every internal `runtime_error()`
   (division by zero, undefined variable, type mismatch, index out of range, bad
   opcode, call-arity mismatch, etc.) becomes a thrown string exception that a
   `try/except` can catch. Native-function failures (`aqs_native_fail`, e.g.
   `len()` with a bad argument, `range()` step 0) likewise become catchable string
   exceptions.
7. A `return` inside a `try` body (returning out of the enclosing function) discards
   that function's handlers and returns normally — the exception machinery is not
   triggered.

### Worked example

```
try:
    x = 1 / 0
except err:
    print(err)
print("after")
```

prints (the built-in division message, then the post-try line):

```
integer division by zero
after
```

```
def boom():
    raise "kaboom"
try:
    boom()
except e:
    print("caught:", e)
```

prints `caught: kaboom` (raise in a callee, caught in the caller).

## Mechanics (overview — full detail in the plan)

- A VM-global fixed-size handler stack; each entry records `{ handler_ip, sp,
  frame_index }` captured when a `try` is entered.
- A VM-global pending-exception slot: `Value g_exc` + `int g_has_exc`.
- Three new opcodes appended at the END of the `OpCode` enum (so existing bytecode
  numbers are stable): `OP_SETUP_TRY` (16-bit operand), `OP_POP_TRY` (16-bit
  operand), `OP_RAISE` (no operand).
- Throwing sets `g_exc`/`g_has_exc` and uses the existing uniform error signal
  (`goto err`). Unwinding happens at the run loop's error label, which is
  restructured to either RESUME at a found handler (`continue`) or, if none, abort
  (`return 1`).

## Scope and explicit deferrals

In scope (M22.4):
- `raise EXPR`.
- `try` / `except` / `except NAME` (single catch-all clause, optionally named).
- Catchability of built-in runtime errors and native errors.
- GC-correctness of the pending exception value.

Explicitly OUT of scope (future work, do NOT implement):
- `finally` blocks.
- Multiple `except` clauses on one `try`.
- Typed `except` (e.g. `except TypeError:`) / matching by class.
- An exception base-class hierarchy / built-in exception types.
- `raise` re-raise with no operand (bare `raise` inside a handler).
- `else` clause on `try`.

## Tests (assert list — full wiring in the plan)

Host unit tests (`tools/t/aqs_test.c`, via `ok()`/`err()`), each also run under
`-DAQS_GC_STRESS` by `make test-aqs-gcstress`:

1. `raise`+catch a string: `try: raise "x"` / `except e: print(e)` -> `x`.
2. Catch a built-in runtime error: `try: print(1/0)` / `except e: print("caught")`
   -> `caught` (program does NOT abort).
3. `raise` across a function call: raise in a callee, caught in the caller.
4. Nested try: inner try catches; outer try unaffected. And: inner try without a
   matching error lets the throw reach the outer handler.
5. `except NAME` binding sees the thrown value (and a non-string value, e.g. an int).
6. `except` without binding runs the handler and discards the value.
7. `try` with no error: BODY runs, `except` block is skipped, control continues.
8. Uncaught `raise` -> `err()` (the interpret call returns nonzero).
9. `raise` an instance and read its field in the `except` block.
10. Control returns to code after the `try/except` (a statement after the construct
    runs in both the no-error and the caught-error case).
11. `return` from inside a `try` body returns normally (handler is cleaned up; a
    later throw in the caller is not mis-caught by the dead handler).

On-Aqua end-to-end (`fsroot/aqs/exc.aqs` + `scripts/run-aqs-test.sh`): a script that
raises+catches a string, catches a `1/0` runtime error, and prints a stable marker
line, asserted over serial after booting Aqua and running `/bin/aqs`.
