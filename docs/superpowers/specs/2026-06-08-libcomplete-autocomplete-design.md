# libcomplete — semantic autocomplete for Code Studio (AetherScript)

> **Status: SHIPPED (2026-06-08).** Engine in `src/apps/as/complete.{c,h}` (44/44
> host tests via `make test-complete`); popup wired into Code Studio. Verified in
> QEMU: `im`→`import`, `import ma`→`math`/`mathx`, `math.`→`sq`/`abs`/`gcd`/…
> **Realized deviation:** the engine uses its own dependency-free tolerant
> tokenizer (not `lexer.c`) — `lexer.c` uses `snprintf` (no libc in the GUI app)
> and aborts on mid-edit input; a self-contained scanner removes both problems
> while keeping real token-level syntax awareness. Plan:
> `docs/superpowers/plans/2026-06-08-libcomplete-autocomplete.md`.

## Why

"Why VS Code is VS Code" is syntax-aware completion: type `im` → `import`, type
`tor` → `torch`, type `math.` → the module's functions. Code Studio
([[ide-as-editor]]) today only edits + highlights + runs `.as`; the next leap is
making it *feel* like an IDE via context-aware completion. Target: **match VS Code**
behavior within what our stack allows.

This is the first consumer of a new, reusable, OS-independent completion engine
(`libcomplete`) built on the existing AetherScript lexer. The same engine later
backs a JS IDE and can be wrapped as a standalone language-server process.

## Settled decisions (from brainstorming)

1. **Depth = Tier 3, semantic / scope-aware** — not flat prefix matching. The engine
   knows what names are *in scope* at the caret and what *type* a value has.
2. **Architecture = library first, server later** — ship `libcomplete` as a C module
   linked into Code Studio (synchronous, in-process; files are small → sub-ms). Keep
   it OS-independent and host-testable so it can later gain a process/IPC shell
   (true LSP) without a rewrite.
3. **Type inference = literals + class instances + builtin types** (no interprocedural
   return-type tracking). Single-assignment, last-write-before-caret wins. `x = []`→
   list, `x = {}`→dict, `x = "…"`→str, `x = N`→int/float, `x = Foo(...)`→instance of
   class `Foo`. `x = foo()` / params / loop vars → unknown (degrade gracefully).
4. **UX = VS Code-aligned** (decided by discretion): auto-trigger as you type **plus**
   explicit Ctrl+Space; Tab/Enter accept; Esc dismiss; ↑/↓ navigate; **fuzzy
   subsequence matching** with relevance ranking (not prefix-only).

## Goals / Non-goals

**In:** an in-process completion engine + a completion popup in Code Studio covering
(a) in-scope identifiers + keywords + builtins, (b) module-name completion after
`import`/`from`, (c) module-export completion after `module.`, (d) member completion
after `value.` for list/dict/str/class-instance. Host unit tests.

**Out (deferred):** function return-type inference; a side doc/signature panel;
parameter-hint (signature help) popups; go-to-definition / rename / outline (the
engine's scope model enables these later, but they are separate features); a
standalone language-server process; completion for `.js`.

## Architecture

New module colocated with the AS toolchain (shares `lexer.h` tokens + `as.h` +
AS_BC version, and is OS-independent like `value.c`/`compiler.c`):

```
src/apps/as/complete.h     public API + Completion struct + kind enum
src/apps/as/complete.c     the engine (lex-tolerant context + scope + type + index)
tools/t/complete_test.c    host unit tests
```

Public API — stateless, one call per query:

```c
enum {
    CMP_KEYWORD, CMP_BUILTIN, CMP_LOCAL, CMP_PARAM, CMP_GLOBAL,
    CMP_IMPORT, CMP_MODULE, CMP_FUNC, CMP_CLASS, CMP_METHOD, CMP_FIELD
};
typedef struct {
    char  label[48];   /* shown in the popup            */
    char  insert[48];  /* inserted on accept (usually == label) */
    int   kind;        /* CMP_*  -> icon/color          */
    int   score;       /* ranking; higher = better      */
} Completion;

/* Up to `max` ranked candidates for the caret (byte offset) in src[0..len).
 * Returns the count. Never aborts on malformed/mid-edit input. */
int as_complete(const char *src, int len, int caret, Completion *out, int max);
```

Code Studio links `complete.o` (Makefile `studio` rule) and calls `as_complete`
when the buffer changes near an identifier or after `.`.

## Pipeline (inside `as_complete`)

`as_lex` aborts on mid-edit input (verified: an unterminated string → "unterminated
string", which is constant while typing). So the engine never lexes the raw buffer
whole. Instead:

1. **Local context scan** — a small hand scan of the **current line up to the caret**
   (no full lex needed; the incomplete part is almost always the current line):
   - `prefix` = the partial identifier ending at the caret (e.g. `im`, `sq`).
   - `receiver` = if the chars before `prefix` are `IDENT .`, capture `IDENT`.
   - `after_import` = the line (before the caret) starts with `import` / `from … `.
   This classifies the request into one of three contexts: **MEMBER** (receiver set),
   **MODULE** (after_import), or **IDENTIFIER** (default).

2. **Scope/symbol pass** — `as_lex` the buffer **up to the start of the current line**
   (that region is normally complete). On lex failure, retry on the largest valid
   prefix; if still failing, degrade to keywords + builtins + a best-effort regex-ish
   identifier scrape. Walk the tokens once to build the model below. This pass is
   shared by IDENTIFIER (names in scope) and MEMBER (the receiver's type + class defs).

3. **Candidate build + rank** per context (below), fuzzy-filtered by `prefix`.

### Scope model (IDENTIFIER context)

Names visible at the caret, with a proximity tier used in ranking:
- **module level** — top-level `def NAME`, `class NAME`, `NAME =` at indent 0.
- **enclosing function** — find the nearest `def` whose body encloses the caret (by
  indentation); collect its params, its locals assigned before the caret, and
  `for X in …` targets.
- **imports** — `import M` → name `M` (CMP_IMPORT); `from M import a, b` → `a`,`b`.
- **builtins** — the VM/compiler builtin table (print, len, range, …) — enumerated
  from the single source of truth, not hand-duplicated.
- **keywords** — the full set from `lexer.h`: def return if elif else class super try
  except raise while for in and or not lambda import from true false nil break
  continue.

Ranking tier (high→low): local > param > module global > import > builtin > keyword.

### Type table (MEMBER context)

For the receiver name, take its **last assignment before the caret**:
- `recv = [ … ]` → LIST; `recv = { … }` → DICT; `recv = "…"`/`'…'` → STR;
  `recv = <int>` → INT; `recv = <float>` → FLOAT.
- `recv = Name( … )` where `Name` is a class in scope → INSTANCE(Name).
- `recv = foo(...)` / `recv = mod.fn(...)` / param / loop var → UNKNOWN.
- `recv` is an imported module name → MODULE (handled by cross-file index below).

Members by type (concrete, from the VM's OP_INVOKE dispatch):
- LIST → `append`
- DICT → `get`, `has`, `keys`, `values`, `remove`
- STR → (none; `len()`/indexing are builtins, not methods) → fall back to nothing
- INSTANCE(Name) → `Name`'s methods (the `def`s inside `class Name`) + fields
  discovered from `self.x = …` assignments in those methods; plus `init`.
- INT/FLOAT/UNKNOWN → no members (empty; popup simply doesn't show).

### Cross-file index (MODULE context + module receiver)

- After `import`/`from` → candidate modules = basenames of `/usr/as/lib/*.as`
  (resolved the same way the VM's import does: cwd, `/usr/as/lib/`, `/usr/as/`) plus
  any in-buffer-importable names. (On host tests, the lib dir is the cwd.)
- `module.` where `module` was imported → read+lex `<module>.as`, collect its top-level
  `def`/`class`/`NAME =` as exports (CMP_FUNC/CMP_CLASS/CMP_GLOBAL). This is the
  library-symbol completion (`math.` → `sqrt gcd lcm isqrt …`). Cache by filename so
  repeated keystrokes don't re-read.

### Ranking / fuzzy matching (VS Code-like)

Match `prefix` against each candidate by **subsequence** (chars of `prefix` appear in
order), not just prefix. Score = base(scope/kind tier) + match-quality bonuses
(exact-prefix > word-boundary/camel hits > contiguous run > scattered) − gap penalty.
Ties broken by shorter label then alphabetical. Empty `prefix` (e.g. right after `.`)
lists all candidates by tier.

## IDE integration (Code Studio / studio.c)

```
┌ untitled.as                                       Run ┐
│ 1  import ma▏                                         │
│         ┌──────────────────┐                          │
│         │ ▸ math      module│   ← popup at caret pixel │
│         │ ▸ mathx     module│                          │
│         └──────────────────┘                          │
│ 2  total = 0                                          │
```

- **Trigger** (VS Code-style): after a buffer edit, if the caret is in/after an
  identifier (or immediately after `.`), call `as_complete`; show the popup if it
  returns candidates. Ctrl+Space forces it even on an empty prefix. Esc, caret move
  off the word, or no candidates → hide.
- **Keys while open**: ↑/↓ move selection, Tab/Enter accept (replace the partial word
  `[word_start, caret)` with `insert`, place caret at its end), Esc close, any other
  edit re-queries + re-filters live. These are intercepted *before* the normal editor
  key handling while the popup is visible.
- **Rendering**: a floating box drawn after the code each frame (so it overlays),
  `gui_rect` background/border + `gui_text_mono` (16px) rows; ≤8 visible rows with
  scroll; each row = a kind marker (colored glyph) + label (+ a faint kind word).
  Position: `x = GUTTER+2 + word_col*CELL`, `y = caret_row line + ROWH`; clamp to the
  window, flip above the caret if it would overflow the bottom.
- **State** in studio.c: `cmp_items[N]`, `cmp_count`, `cmp_sel`, `cmp_open`,
  `cmp_word_start`. No new syscalls — pure in-app drawing + the linked engine.

## Testing

`tools/t/complete_test.c` (host, like the dom/css/as suites), `make test-complete`:
feed `(source, caret)` and assert membership/ordering of the returned candidates —
- `im|` → contains `import` (keyword)
- `from ma|` and `import ma|` → contains `math`, `mathx` (module)
- `math.sq|` → contains `sqrt`; `math.|` → contains `gcd`,`lcm`,`isqrt` (cross-file)
- `x = []` then `x.|` → `append`; `d = {}` then `d.|` → `get`,`has`,`keys`
- inside `def f(n):` … `n|` → `n` offered (param); a module global offered; a name
  out of scope *not* offered
- `p = Point(2)` (class in buffer) then `p.|` → Point's methods + fields
- fuzzy: `imprt`/`iprt`-style subsequence still ranks `import`
- **robustness**: an unterminated string / open `[` on the current line → returns
  sensible candidates, never aborts.
The engine is OS-independent, so this is full coverage without QEMU. Each phase also
gets a QEMU screenshot of the live popup.

## Phasing (each: host-tested + QEMU screenshot)

- **P1 — engine skeleton + IDENTIFIER completion + popup UI.** Tolerant context scan,
  scope model, keyword/builtin/in-scope-identifier candidates, fuzzy ranking; the
  popup with trigger/navigate/accept/dismiss wired into studio.c. Delivers `im`→
  `import` and in-scope `tor`→`torch`.
- **P2 — MODULE context.** `import`/`from` → module names; `module.` → cross-file
  exports (read+lex the module). Delivers library-symbol completion.
- **P3 — MEMBER context + type table.** `value.` for list/dict/str/class-instance via
  the last-assignment type guess.

## Risks & mitigations

- **Mid-edit lexer aborts (top risk).** Mitigated by the current-line local scan +
  lexing only the pre-line region + graceful degradation. Hardened first in P1, with
  explicit robustness tests.
- **Multi-line incomplete constructs** (an open `[`/string from a prior line). Rare;
  the pre-line lex may fail → degrade to keywords + identifier scrape. Acceptable.
- **Popup ↔ editor focus/key routing.** The popup intercepts keys only while visible;
  P1 nails Esc/arrows/Tab/Enter precedence and the "edit re-queries" loop.
- **Builtin/method tables drifting from the language.** Source them from the lexer
  keyword enum and the VM's method dispatch (single source of truth), with a test
  that fails if a known method (e.g. dict `keys`) stops being offered.
```
