# AetherScript A4 feature plan — making A3 a more complete language

Owner: lang-lead. Written 2026-09-19 against HEAD `e5a551c6a`.

This document is a PLAN, not a changelog. Nothing here exists until the
implementation tasks land and their gates are green; the per-feature "gate plan"
sections say what green means. The language rules below are stated exactly,
because the two mirrors that must agree (`backend/llvm/*` and `cli/eval.c`)
do not read prose charitably.

**Naming.** The README pins "all future language development stays on the 3.x
line, with compatible minor releases" and the version header is
`# aether: 3.0`; unimplemented minors are refused. The features below are the
first candidates for a `3.1` declaration — but NO feature below may be gated
behind a `3.1` header until it actually works, because a header an old
compiler silently ignores is how "old compiler pretends to support future
capability" happens (AETHERSCRIPT_LANGUAGE_3.md §1 already refuses unknown
minors for exactly this reason). Until then, features land unversioned on the
3.0 line, one at a time, each with its own gate.

---

## 0. Verified current state (what NOT to re-propose)

Every candidate in the brief was checked against
`docs/AETHERSCRIPT_LANGUAGE_3.md`, `c/apps/as/README.md`,
`c/apps/as/frontend/lexer.h`, `c/apps/as/ir/model.h` and
`c/apps/as/cli/eval.c`. The following are ALREADY DONE and must not be
proposed again:

- **Ternary conditional** `a if cond else b` — `AN_CONDITIONAL`
  (ir/model.h), checked in `sema/optional.c` (`at_check_conditional_value`),
  lowered in `backend/llvm/emit.c`, mirrored in `eval.c`.
- **`pass`** — `AN_PASS` exists in the IR.
- **f-strings in the native pipeline** — `T_FSTR`, `AN_FORMAT`,
  `at_parse_fstring`, `frontend/text.c`, native fixture
  `tests/fixtures/astyped/fstring/main.as`. Absent ONLY in `cli/eval.c`
  (see F0 below).
- **`global` rebinding** — `AN_GLOBAL_DECL`/`AN_GLOBAL`; LANGUAGE_3 §2
  ("模块变量及 `global` 重绑定"); eval.c runs module initializers and reads
  globals.
- **Native closures / lambdas** —
  `frontend|sema|backend/llvm|runtime/closure.c` (README correction block).
- **Single inheritance** — `sema/inheritance.c`; `super`, vtable dispatch.
- **`str()` conversion builtin** — `AT_CALL_STR` (ir/model.h); also
  `parse_int/parse_float/chr/ord`. What remains is eval-side and formatting
  (F1/F2 below are the eval features, F1 also named `str()`).
- **Exceptions as values** — `AN_TRY/AN_EXCEPT/AN_RAISE`, typed exception
  codes, `runtime/exception.h` layout. What remains is user-defined exception
  CLASSES (F7).
- **Byte-index slicing of strings** — `AT_CALL_STR_SLICE`, `AT_CALL_STR_SUB`
  exist; byte-index semantics documented §2. What remains is bracket-slice
  SYNTAX sugar (refused for now, F8).
- **Bitwise/shift/power BINARY operators** — `& | ^ ~ << >> **` all exist,
  checked and mirrored in eval.c with the backend's overflow/count rules.
  Missing: the COMPOUND forms (F3) and unsigned-compound edge coverage (F4).
- **Conditional expression, `in`, `.find()`** on strings — §2.
- **Default argument on ONE builtin** — `mem2cstr(memory, limit=4096)`,
  explicitly scoped: "这是内建接口的可省略实参，不代表任意用户函数的默认参数
  已经实现" (§8). User-level default/keyword args are genuinely absent (F6).

### What the guest evaluator (`cli/eval.c`) covers today

Studio's Run button executes `as_typed_eval` on the checked tree. Present:
ints/bools/floats-as-literals/strings/None values; `+ - * / % & | ^ << >> **`;
comparisons `== != < <= > >=`; unary `- ~ not`; string concat and print;
if/while/break/continue; plain and compound assignment for `+= -= *= /= %=`
ONLY (via `as_compound_operator`); calls to user functions; globals and
module initializers; `a if c else b`. Refused BY NAME (honest, keep it that
way): `for`, `try`, `with`, `return`-value use, compound ops outside the
five, field/index assignment, and every other node kind. The file's header
comment is the contract: "unsupported nodes refuse instead of inventing a
VM."

---

## 1. House rules this plan enforces

1. **Absent beats present-and-wrong.** Every feature below either ships with
   its exact rule or stays refused BY NAME in eval.c. No silent fallback.
2. **One jar, two doors.** `frontend/lexer.h: as_compound_operator()` is the
   single mapping from a compound-assignment token to its binary operator;
   its comment already names F3 as the reason it exists ("the day `**=` or
   `&=` is added, a second table that did not follow turns `x **= 2` into a
   silent `x %= 2`"). New compound tokens MUST extend that function in the
   same commit as the parser change.
3. **Comments are the deliverable.** Each implementation task requires the
   WHY comment (rejected alternative + trap), not a restatement of the code.
4. **Gates with negative controls as prerequisites.** Pattern to copy:
   `tests/aseval.mk` (`test-as-eval: test-as-eval-negctl`), whose controls
   are `#ifdef` sabotages watched going red. Every new gate needs a control
   that breaks the new code path specifically, and the report must quote the
   red output.
5. **No site-fitting, no spoofing.** Language features are general; nothing
   branches on a program's name or a checker's expectation.
6. **Path-limited commits.** Role file boundaries from the mission message:
   grammar-dev owns `frontend/*` + ir node kinds; frontend-dev owns
   `sema/*`, `backend/llvm/*`, `ir/*`, `runtime/*`, `common/*`; eval-dev
   owns `cli/eval.c`, `tests/aseval.mk`, `tests/unit/as_eval_test.c`.
   lang-lead owns docs only. `lexer.h` is frontend/ territory (grammar-dev)
   but `as_compound_operator` is consumed by backend AND eval — changes to
   it are grammar-dev's commit, sequenced FIRST (see §4).
7. **Check `git status`/mtime before touching anything shared.** At writing:
   tree clean, `eval.c` last touched 2026-09-18 22:28 (HEAD commit).

---

## 2. The feature list

Priority order matches the first pass of the implementation order; §4 gives
the full dependency graph. Each entry: why / grammar / stages / EXACT
eval-mirror rule / conflicts / gate plan.

### F0 — f-strings in the guest evaluator (eval-only parity fix)

- **Why.** The native pipeline has f-strings (`T_FSTR` → `AN_FORMAT`,
  `frontend/text.c` parses holes through the expression parser), and
  `print("hello world")` is eval's reason to exist — but
  `print(f"x={x}")` refuses today, on the very first program a user writes
  after hello world. Highest user value in the plan; touches ONE file.
- **Grammar.** None. The parser already produces `AN_FORMAT` with `args[]`
  segments (string literals and hole expression nodes, per
  `at_parse_fstring`/`frontend/text.c`; the implementer must read
  `at_parse_fstring`'s child layout before coding — do not guess it).
- **Stages.** `cli/eval.c` only. No sema/backend change: sema already types
  holes; backend already lowers.
- **EXACT eval-mirror rule.** Concatenation left-to-right of decoded literal
  segments and hole values; each non-string hole value converts with the
  SAME text `print_value` produces for a single value (int `%lld`,
  uint `%llu`, bool `true`/`false`, float `%.17g`, None `None`), because
  backend emit.c "reuses precisely the str(value) conversion" for holes and
  print.c owns that spelling. `{{`/`}}` are literal braces (the lexer
  already handles the raw interior; `as_token_decode` handles escapes). A
  hole whose node kind eval refuses must refuse the WHOLE f-string by naming
  the hole's construct — never print a partial string (a truncated line is
  a wrong answer, the failure mode this evaluator exists to avoid).
- **Conflicts.** None; eval currently refuses the node, so nothing that
  previously ran changes behavior.
- **Gate.** New cases in `tests/unit/as_eval_test.c` (f-string with int,
  string, bool, expression hole, nested quotes, `{{ }}`, empty f-string, and
  a hole containing a refused construct → must name it). Negative control:
  `#ifdef AS_EVAL_NO_FSTR` restoring the generic refusal; watch it go red on
  the new cases. The control stays a prerequisite of the positive target,
  per aseval.mk's existing pattern.


### F1 — `str()` in the guest evaluator (and the shared `ev_to_str`)

- **Why.** `str(x)` is how programs format without f-strings, and eval's
  refusal today falls into the generic "this expression" bucket. It is also
  the cheapest way to make F0's holes exact: eval needs a single
  `ev_to_str()` that print, f-strings and `str()` all share (one jar —
  without it, F0's hole formatting and `str()` formatting are two tables
  that will drift).
- **Grammar.** None; calls already parse and sema already resolves
  `AT_CALL_STR`.
- **Stages.** `cli/eval.c` (`eval_call` dispatch). `AT_CALL_PARSE_INT` /
  `AT_CALL_PARSE_FLOAT` are NOT in this batch; their failure semantics
  (ConversionError vs ValueError edges) need a doc lookup first.
- **EXACT eval-mirror rule.** `str(v)` for the value kinds eval holds
  produces byte-identically what `print_value(v)` prints for one value
  (`runtime/print.c` is the oracle). `str` of a bytes-like is the ESCAPED
  representation, never a decode (README: "Do not turn str(Bytes) formatting
  into implicit decoding") — eval holds no Bytes values today, so
  encountering one is a refuse-by-name, not a format.
- **Conflicts.** None.
- **Gate.** as_eval_test cases asserting exact stdout; negative control
  `#ifdef AS_EVAL_NO_STRCALL` restoring refusal; watch red. The control
  lives beside F0's so deleting `ev_to_str` reddens both batteries.

### F2 — `for` loops and `range()` in the guest evaluator

- **Why.** After print and arithmetic, iteration is the third thing every
  script does; eval refusing `for` means any demo with a loop cannot run
  from Studio at all.
- **Grammar.** None. `AN_FOR` exists; `AT_CALL_RANGE` exists;
  `runtime/range.c` + `backend/llvm/range.c` define the semantics.
- **Stages.** `cli/eval.c`: an `eval_stmts` `AN_FOR` branch plus range
  values. The value model grows one kind — propose
  `EV_RANGE {start, stop, step, cur}` with the bounds computed ONCE at loop
  entry (mirrors "for 的 source 在进入循环前只求值一次", §2 loop rules).
- **EXACT eval-mirror rule.** `range(stop)` ≡ `range(0, stop, 1)`; stop is
  EXCLUSIVE; negative step iterates downward; `step == 0` → `ValueError` at
  the `range(...)` call site, not at loop entry (the backend raises where
  the call is); the loop variable is assigned BEFORE each body execution;
  `break`/`continue` use the existing UN_BREAK/UN_CONTINUE machinery; the
  counter arithmetic must tolerate the last step passing the stop value
  without wrapping (§3: "内部宽计数器允许最后一步越过停止值" — copy the
  full-width-count approach of `runtime/range.c`, not a naive
  `i += step; i < stop`, because stops near INT64_MAX are where naive loops
  wrap into infinite loops). Optional non-null facts not crossing the back
  edge (§2) is sema's rule, not eval's; eval simply re-evaluates the
  condition each iteration.
- **Conflicts.** `Ev` gains a kind; every switch on `kind` (print_value,
  ev_clear, ev_export) needs an explicit case. Printing a bare range must
  print exactly what the backend prints or refuse by name — CHECK
  `runtime/range.c`'s str/print form first and mirror it, do not invent
  `[0..10)`.
- **Gate.** as_eval_test: basic counts, negative step, step-0 refusal,
  break/continue inside for, a big-stop case near INT64_MAX with an early
  break (proves no wrap-to-infinite). Negative control
  `#ifdef AS_EVAL_NO_FOR` keeping the refusal; watch red. This guards the
  "loop consumed break as return" shape the existing AS_EVAL_NO_UNWIND
  control documents.


### F3 — augmented bitwise/shift/power assignment: `**= &= |= ^= <<= >>=`

- **Why.** Users write `x <<= 3` instinctively; today it fails while
  `x += 1` works — an unexplained hole in an otherwise complete compound
  set. `lexer.h`'s `as_compound_operator` comment pre-registers this exact
  feature as the reason the mapping is centralized.
- **Grammar.** Six tokens added to `TokType` after `T_PERCENTEQ`, following
  the existing spelling pattern (`T_POWEQ T_AMPEQ T_PIPEEQ T_CARETEQ
  T_SHLEQ T_SHREQ`). Lexer longest-match: `>>=` beats `>>` + `=`; `**=`
  beats `**` + `=`. Parser: wherever `T_PLUSEQ` is accepted today, accept
  the six.
- **Stages.** frontend/lexer.c + lexer.h (tokens AND the
  `as_compound_operator` extension — SAME COMMIT, house rule 2);
  frontend/parser.c / frontend/assignment.c; ir: no new node kind
  (AN_ASSIGN with `op` = new token; the child-layout contract in model.h is
  unchanged); sema (type rules below); backend/llvm/compound.c + numeric.c;
  cli/eval.c (the `as_compound_operator` path already exists — extending the
  table routes automatically, and every binary op it can return is already
  implemented in eval_binary); runtime: none.
- **EXACT eval-mirror rules** (these ARE the binary rules of LANGUAGE_3 §3
  and eval.c's switch, restated so the two doors cannot drift):
  - `x **= e` ≡ `x = x ** e`: exponent must be >= 0 else `ValueError`;
    exponentiation by squaring with a CHECKED multiply at every step,
    `OverflowError` on overflow; `0 ** 0 == 1`; the final unnecessary square
    is never computed (so `i8: 100 ** 1` succeeds — do not "simplify" this).
  - `x &= e`, `x |= e`, `x ^= e`: no overflow possible; both operands the
    same integer type; result type is x's type.
  - `x <<= e`: count must be in `[0, bitwidth)` — tested as UNSIGNED
    (`(uint64_t)e >= width`) so a negative count is refused by the same
    compare; the result must round-trip (`(r >> e) == x`) else
    `OverflowError`.
  - `x >>= e`: same count rule; SIGNED types shift arithmetically, UNSIGNED
    logically (eval.c's int64 `>>` is already arithmetic; u8..u64 need the
    unsigned form).
  - Compound assignment is a STATEMENT, not an expression: `a = (x <<= 1)`
    stays a parse error. `x op= e` evaluates x's storage location once, e
    once, in that order.
  - Sema rule: compound bitwise/shift/power ops require INTEGER operands
    (the `Integer` constraint set of §2), not floats; `**=` on floats is
    refused until float `**` exists (§3: "浮点乘方尚未实现").
- **Conflicts.** The lexer documents the `<-` vs `<` `-` disambiguation;
  confirm `>>=` cannot merge the `>>` that closes nested generics
  (`Dict[str, List[i64]]`) with a following `=` — there is no such
  construct, but the lexer test must PROVE it rather than assume it.
- **Gate.** (a) Host: extend the numeric gate with host-native runs of each
  op including every overflow/count edge above. (b) Eval: as_eval_test cases
  for each op through the guest. (c) Negative control: temporarily REMOVE
  one line of the `as_compound_operator` extension and watch BOTH the
  backend gate and the eval gate redden — this is the one-jar-two-doors
  control, and it exists to prove the second door follows the jar.
  (d) `make test-mk-wired` green.

### F4 — unsigned compound arithmetic: parity, or an honest refusal list

- **Why.** eval.c's compound path computes in `int64_t` only (EV_INT); the
  language has `u8..u64`. Host unsigned compound may already work; the gap
  is eval parity and an HONEST statement of what eval refuses.
- **Grammar.** None.
- **Stages.** Verify sema/backend acceptance of `u8 x += e` etc. FIRST;
  only then decide what eval does.
- **EXACT eval-mirror rule.** Whatever sema/backend accept, eval mirrors
  bit-for-bit, truncating on STORE to the destination width, not
  per-operation. Whatever the host does NOT accept, eval must also refuse —
  eval may never be MORE permissive than the real compiler.
- **Conflicts / recommendation.** EV_UINT exists but is unused in eval
  arithmetic today; adding per-width arithmetic to eval duplicates the
  backend a third time. RECOMMEND: eval keeps refusing non-i64 widths by
  name, and this task instead delivers the parity harness — a corpus of
  small programs run through BOTH host `as run` and the eval battery,
  asserting identical stdout or identical refusal. Today parity is per-case
  and manual; the harness is the drift control this boundary lacks. Decide
  in implementation review.
- **Gate.** The parity harness itself, with a seeded disagreement case as
  its negative control (watch it catch a deliberately wrong eval result).


### F5 — chained comparisons `a < b < c`

- **Why.** Bounds checks read naturally; Python users expect it. Medium
  value: `a < b and b < c` already works.
- **Grammar.** `comparison := arithmetic (comp_op arithmetic)*`; a chain of
  length > 1 forms a NEW node kind (propose `AN_CHAIN`) rather than
  left-nested AN_BINARY, because each middle operand must evaluate ONCE.
- **Stages.** frontend/expression.c (chains bind at the comparison level;
  `a < b < c < d` legal, mixed ops pair left-to-right); ir/model.h (AN_CHAIN
  + its child-layout comment — model.h's layout contract requires it);
  sema (operand types pairwise, whole expression bool); backend (short-
  circuit blocks); cli/eval.c.
- **EXACT eval-mirror rule.** `a op1 b op2 c ...` evaluates `a` once, then
  each following operand ONCE, left-to-right; evaluation SHORT-CIRCUITS at
  the first false comparison (later operands never evaluate — same as
  `and`); result is bool. Pairwise rules are exactly the existing binary
  comparison rules; mixed int/float stays a sema error ("不隐式转换" §2).
  `a < b < c` is NOT `(a < b) < c` — the bool-vs-int comparison a naive
  lowering produces must be a sema error, and the negative control
  implements exactly that naive lowering.
- **Conflicts.** `a == b == c` is a type error today, so nothing breaks;
  confirm no fixture relies on the current diagnostic text (as-diag
  baseline — update in the same commit).
- **Gate.** Host parity cases including a side-effecting middle operand
  (global counter proves single evaluation), a short-circuit proof (the
  skipped operand prints/raises if evaluated), the naive-lowering negative
  control watched red, eval mirror cases, diag baseline update.

### F6 — default and keyword arguments (user functions)

- **Why.** Real library ergonomics; the stdlib already wants it
  (`mem2cstr(limit=4096)` cheats at the builtin level and its doc says so).
  HIGH cost: interacts with private-function inference, generics, Callable
  signatures and overrides.
- **Grammar.** Parameter `name: Type = expr`; call site `name = expr` in
  argument position. `*`/`**` are NOT part of this feature (F8 refusal).
- **Stages.** frontend/parser.c + expression.c; ir (AtFunction gains a
  default-value node array); sema/check.c, sema/callable.c (signature
  identity INCLUDING defaults for overrides), sema/generic.c (defaults
  under substitution); backend/llvm/calls.c (fill missing args); cli/eval.c
  (eval_call argument fill); runtime: none.
- **EXACT eval-mirror rule.** Defaults evaluate ONCE PER CALL at the call
  site, in declaration order, ONLY for parameters not supplied positionally
  or by keyword (the `mem2cstr` precedent generalized; explicitly NOT
  Python's define-time singleton — AS values are copyable scalars, so
  per-call evaluation is the rule that cannot produce aliasing surprises).
  A parameter with a default may not be followed by one without. Duplicate
  (positional + keyword) binding is a sema error; unknown keyword is a sema
  error; keyword args follow positional ones. Overrides keep parameter names
  and defaults identical (§2 override rule extends).
- **Conflicts.** Private functions keep "只有一个静态签名，不按调用自动产生
  重载" — defaults add an arity RANGE at call sites, not overloads. Generic
  cloning must clone default expressions too.
- **Gate.** Host matrix (missing/positional/keyword/dup/unknown/override
  mismatch), eval mirror, negative control that evaluates defaults at
  DEFINITION time (a counter case must go red), diag baseline.


### F7 — user-defined exception classes

- **Why.** README's unfinished list names it; users cannot define domain
  error kinds without abusing ValueError.
- **Grammar.** `class MyError(Error):` — reuse class syntax with a sealed
  base; no new keywords.
- **Stages.** sema/exception.c + inheritance.c (seal the hierarchy to the
  builtin error set), backend/llvm/emit.c + runtime/exception.* (value
  layout), cli/eval.c keeps REFUSING try/except — but the refusal must NAME
  the user class where it can, so a Studio user reads "host-only", not
  "broken".
- **EXACT eval-mirror rule.** None beyond the named refusal (eval grows no
  exception machinery in A4).
- **Conflicts.** Exception codes are a fixed enum today
  (`at_exception_code`); user classes need dynamic identity without breaking
  the code-based fast path — the layout spec (lang-lead → frontend-dev)
  lands BEFORE any code.
- **Gate.** raise/catch/except-order/base-class catch cases; negative
  control swapping dynamic identity for name-string compare (two same-named
  classes in different modules must go red).

### F8 — refused-for-now list (absent beats present-and-wrong)

These are explicitly NOT in A4. Each refusal is a decision; if one lands
later it needs its own spec with the same rigor:

- **`*args` / `**kwargs`** — collides with the static single-signature rule
  and Callable typing; no honest version exists short of a variadic type
  system. `f(*xs)` stays a parse error.
- **`match/case` or `switch`** — pattern semantics need enums/tagged unions
  that don't exist; a C-style integer switch is not worth a keyword.
  Revisit after the Optional/enum design.
- **`for..else` / `while..else`** — trivially expressed with a flag; adds a
  second meaning of `else` that binds to loop completion. Refused pending a
  demonstrated stdlib need.
- **`del`** — GC'd language; name deletion fights definite-assignment
  analysis and closure cells (captured names share storage).
- **`nonlocal`** — closure cells already make captured variables assignable;
  a declaration would be a second binding mechanism for what capture
  discovery does implicitly. Refused; revisit if ambiguity reports appear.
- **`...` (ellipsis)** — no protocol/typing surface needs it; `pass` covers
  the empty body.
- **Bracket-slice syntax on str/Bytes/Buffer** — methods exist; sugar that
  implies Python's negative-index tail semantics would contradict §4
  ("负索引当前视为越界，不采用 Python 的尾部索引语义"). A plausible 3.2
  candidate once a negative-index policy is decided.
- **Walrus / assignment expressions** — definite-assignment analysis is
  flow-based over statement merge points; assignment-in-expression breaks
  them.

---

## 3. Guest-evaluator boundary analysis

`cli/eval.c` is a SECOND EXECUTION ENGINE for a language whose spec lives
in the LLVM backend. Every feature that lands natively creates a three-way
choice, in this order of preference:

1. **Mirror exactly** (scalars, arithmetic, control flow — F0/F1/F2/F3/F5)
   where the semantics are total functions over eval's value set.
2. **Refuse by name** (classes, exceptions, resources, Any, containers —
   anything needing GC or ownership) with a message that names the
   construct and says the host runs it. The refusal list is user-facing
   documentation; it must stay honest and specific.
3. **Never** approximate. The file's own history says why: unguarded
   fallbacks turned `x **= 2` into `x %= 2`, OOM into the string "None",
   and a missing unary case into the operand echoed back. Each was found
   by a person, not a test; the aseval.mk controls exist so the next one
   is found by a gate.

The drift control that does not exist yet: a parity harness running a
corpus through BOTH host `as run` and the eval battery, asserting identical
stdout or identical refusal (F4's deliverable even if F4 changes no
semantics).



## 4. Implementation order and dependencies

```text
Batch 1 (highest value, lowest risk — tasks created from this plan):
  G1 grammar-dev : lexer tokens + as_compound_operator extension + parser
                   accept for **= &= |= ^= <<= >>=            (F3 frontend)
  F1 frontend-dev: sema + backend lowering for the six ops    (dependsOn G1)
  E1 eval-dev    : eval mirror for the six ops + str() + f-strings
                   (F3-eval, F1, F0)                          (dependsOn G1;
                   MUST NOT MERGE before F1's gate is green — eval accepting
                   what the host rejects is drift in the wrong direction)

Batch 2 (after batch 1 merges):
  E2 eval-dev    : for/range in eval (F2) + parity harness skeleton (F4)
  G2 grammar-dev : chained comparisons grammar + AN_CHAIN     (F5 frontend)
  F2 frontend-dev: sema + backend for chained comparisons     (dependsOn G2)
  E3 eval-dev    : eval mirror for chained comparisons        (dependsOn G2, F2)

Batch 3 (design-heavy, spec fragments first):
  F6 default/keyword args — lang-lead writes the spec fragment, then G3/F3/E4
  F7 exception classes    — layout spec first (runtime/exception.h), then
                            sema/backend; eval keeps refusing with names
```

Batch-1 rationale: F3's lexer table comment already promises the feature;
F0/F1 are one file each with the oracle (`runtime/print.c`) already in the
tree; none of the three touches the type system. F2 (eval `for`) is medium
risk (a new value kind) — kept out of batch 1 so eval-dev's first merge is
small.

Every task's "done" bar (AGENTS.md §7): builds, and `make build/disk.img`
if ring-3; gate wired and `make test-mk-wired` green; negative control
watched going red with the output quoted; WHY comment with the rejected
alternative; path-limited commit.