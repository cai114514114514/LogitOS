# libcomplete — Code Studio Semantic Autocomplete — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give Code Studio VS Code-style, syntax-aware autocomplete for AetherScript — keyword/identifier/module/member completion driven by a reusable, host-testable engine.

**Architecture:** A self-contained C module `src/apps/as/complete.c` exposing one function `as_complete(src, len, caret, out, max)`. It carries its **own dependency-free tolerant tokenizer** (NOT the production `lexer.c`, which uses `snprintf` and aborts on mid-edit input), a single-pass scope/symbol model, a last-assignment type table, and fuzzy ranking. Code Studio links `complete.o` and renders a popup. The engine compiles both freestanding (into the app) and natively (for host unit tests).

**Tech Stack:** C11 freestanding (no libc in the engine; only `<stdint.h>`), the existing `aui`/`gui_*` drawing primitives + `aether.h` syscalls for the popup, host `cc` for unit tests (mirrors `make test-as`).

**Spec:** `docs/superpowers/specs/2026-06-08-libcomplete-autocomplete-design.md`. Deviation from spec: the engine uses its own tokenizer instead of `as_lex` (the spec anticipated needing tolerance; this is the concrete realization — it removes the libc-link and abort-on-edit problems while keeping real, token-level syntax awareness).

---

## File structure

- `src/apps/as/complete.h` — public API: `Completion` struct, `CMP_*` kind enum, `as_complete()`.
- `src/apps/as/complete.c` — the engine: tolerant tokenizer → context detection → scope model → type table → cross-file index → ranking. Self-contained (static helpers; no libc, no lexer.c).
- `tools/t/complete_test.c` — host unit tests (asserts candidate membership/order).
- `src/apps/gui/studio.c` — MODIFY: popup state + draw + key routing + trigger/accept calling `as_complete`.
- `Makefile` — MODIFY: replace the generic `studio` APP_RULE with a dedicated rule that also compiles+links `complete.o`; add a `test-complete` target; add `test-complete` to `.PHONY`.

Constant builtin/keyword tables (used throughout, defined once in `complete.c`):

```c
/* the full keyword set (mirrors lexer.h TokType keyword tokens) */
static const char *const KEYWORDS[] = {
    "def","return","if","elif","else","class","super","try","except","raise",
    "while","for","in","and","or","not","lambda","import","from",
    "true","false","nil","break","continue", 0
};
/* builtins: vm/compiler natives + as_native.c indirection natives */
static const char *const BUILTINS[] = {
    "print","len","range","gc","gc_stats",
    "addr","syscall","peek8","peek16","peek32","peek64",
    "poke8","poke16","poke32","poke64","i8ptr","i16ptr","i32ptr","i64ptr", 0
};
/* fixed builtin-type method sets (from vm.c OP_INVOKE dispatch) */
static const char *const LIST_METHODS[] = { "append", 0 };
static const char *const DICT_METHODS[] = { "get","has","keys","values","remove", 0 };
/* str has no methods (len()/indexing are builtins) */
```

---

# PHASE 1 — Engine skeleton + IDENTIFIER completion + popup UI

Delivers: `im`→`import`, and an in-scope `tor`→`torch`, shown in a working popup.

## Task 1: Public API + dependency-free tolerant tokenizer

**Files:**
- Create: `src/apps/as/complete.h`
- Create: `src/apps/as/complete.c`
- Create: `tools/t/complete_test.c`
- Modify: `Makefile` (add `test-complete`)

- [ ] **Step 1: Write `complete.h`**

```c
#ifndef AS_COMPLETE_H
#define AS_COMPLETE_H

enum {                       /* candidate kind -> popup icon/color */
    CMP_KEYWORD, CMP_BUILTIN, CMP_LOCAL, CMP_PARAM, CMP_GLOBAL,
    CMP_IMPORT, CMP_MODULE, CMP_FUNC, CMP_CLASS, CMP_METHOD, CMP_FIELD
};

typedef struct {
    char label[48];          /* shown in the popup            */
    char insert[48];         /* inserted on accept            */
    int  kind;               /* CMP_*                         */
    int  score;              /* ranking; higher = better      */
} Completion;

/* Up to `max` ranked candidates for the caret (byte offset) in src[0..len).
 * Never aborts on malformed / mid-edit input. Returns the count. */
int as_complete(const char *src, int len, int caret, Completion *out, int max);

#endif /* AS_COMPLETE_H */
```

- [ ] **Step 2: Write the failing test for the tokenizer (via a test hook)**

In `complete.c` the tokenizer is internal, so test it through a thin exported helper guarded for tests. Add to `complete.h`:

```c
#ifdef AS_COMPLETE_TEST
/* test-only: tokenize src[0..len) into kinds; returns token count. */
int as__lex_test(const char *src, int len, int *kinds, int max);
enum { TK_EOF, TK_IDENT, TK_KW, TK_NUM, TK_STR, TK_DOT, TK_OP, TK_NL };
#endif
```

`tools/t/complete_test.c` (start the file):

```c
#include <stdio.h>
#include <string.h>
#define AS_COMPLETE_TEST
#include "complete.h"

static int fails = 0, checks = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s\n", msg); } } while (0)

/* helper: does the candidate list contain `label`? */
static int has(Completion *c, int n, const char *label) {
    for (int i = 0; i < n; i++) if (strcmp(c[i].label, label) == 0) return 1;
    return 0;
}
/* helper: index of `label` in the list, or -1 (for order checks) */
static int idx(Completion *c, int n, const char *label) {
    for (int i = 0; i < n; i++) if (strcmp(c[i].label, label) == 0) return i;
    return -1;
}

static void test_tokenizer(void) {
    int k[64];
    int n = as__lex_test("foo.bar = 3", 11, k, 64);
    CHECK(n >= 5, "tok count");
    CHECK(k[0] == TK_IDENT, "tok0 ident");
    CHECK(k[1] == TK_DOT,   "tok1 dot");
    CHECK(k[2] == TK_IDENT, "tok2 ident");
    /* keyword classification */
    int n2 = as__lex_test("import math", 11, k, 64);
    CHECK(k[0] == TK_KW, "import is kw");
    /* tolerance: unterminated string must not hang/crash, just yields a TK_STR */
    int n3 = as__lex_test("x = \"unterm", 11, k, 64);
    CHECK(n3 >= 3, "unterminated string tolerated");
}

int main(void) {
    test_tokenizer();
    printf("%d/%d complete checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Add the `test-complete` Makefile target + `.PHONY`**

Find the `.PHONY:` line (currently `... test-tcp-host`) and append `test-complete`. After the `test-as` block add:

```make
# libcomplete host unit tests: the completion engine is self-contained C, so it
# builds and runs natively -- no QEMU.
test-complete:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DAS_COMPLETE_TEST -o $(BUILD)/complete_test tools/t/complete_test.c src/apps/as/complete.c -Isrc/apps/as
	@$(BUILD)/complete_test
```

- [ ] **Step 4: Run the test — verify it fails to link/compile**

Run: `make test-complete`
Expected: FAIL — `as__lex_test` and `as_complete` undefined (complete.c is empty).

- [ ] **Step 5: Implement the tolerant tokenizer in `complete.c`**

```c
#include <stdint.h>
#include "complete.h"

/* ---- dependency-free char helpers (no libc; works freestanding + host) ---- */
static int c_isd(char c){ return c >= '0' && c <= '9'; }
static int c_isa(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int c_isan(char c){ return c_isa(c)||c_isd(c); }
static int c_issp(char c){ return c==' '||c=='\t'; }
static int c_neq(const char *a, const char *b, int n){ for(int i=0;i<n;i++){ if(a[i]!=b[i]) return 0; if(!b[i]) return 0; } return b[n]==0; }
static int c_slen(const char *s){ int n=0; while(s[n]) n++; return n; }

enum { TK_EOF, TK_IDENT, TK_KW, TK_NUM, TK_STR, TK_DOT, TK_OP, TK_NL };

typedef struct { int kind; int start; int len; } Tok;

extern const char *const KEYWORDS[];   /* fwd; defined below */
static int is_kw(const char *s, int n){
    for (int i=0; KEYWORDS[i]; i++) if (c_neq(s, KEYWORDS[i], n)) return 1;
    return 0;
}

/* Tokenize src[0..len) into toks[0..max). Tolerant: an unterminated string ends
 * at EOL/EOF; '#' comments run to EOL; never reads past len. Returns count. */
static int lex(const char *src, int len, Tok *toks, int max){
    int n = 0, i = 0;
    while (i < len && n < max) {
        char c = src[i];
        if (c == '\n') { toks[n].kind=TK_NL; toks[n].start=i; toks[n].len=1; n++; i++; continue; }
        if (c_issp(c) || c=='\r') { i++; continue; }
        if (c == '#') { while (i < len && src[i] != '\n') i++; continue; }
        if (c == '.') { toks[n].kind=TK_DOT; toks[n].start=i; toks[n].len=1; n++; i++; continue; }
        if (c=='"' || c=='\'') { int s=i; char q=c; i++; while (i<len && src[i]!=q && src[i]!='\n'){ if(src[i]=='\\'&&i+1<len)i++; i++; } if(i<len&&src[i]==q)i++; toks[n].kind=TK_STR; toks[n].start=s; toks[n].len=i-s; n++; continue; }
        if (c_isd(c)) { int s=i; while (i<len && (c_isan(src[i])||src[i]=='.')) i++; toks[n].kind=TK_NUM; toks[n].start=s; toks[n].len=i-s; n++; continue; }
        if (c_isa(c)) { int s=i; while (i<len && c_isan(src[i])) i++; toks[n].kind = is_kw(src+s, i-s) ? TK_KW : TK_IDENT; toks[n].start=s; toks[n].len=i-s; n++; continue; }
        toks[n].kind=TK_OP; toks[n].start=i; toks[n].len=1; n++; i++;   /* operator/punct */
    }
    return n;
}

/* ---- constant tables ---- */
const char *const KEYWORDS[] = {
    "def","return","if","elif","else","class","super","try","except","raise",
    "while","for","in","and","or","not","lambda","import","from",
    "true","false","nil","break","continue", 0
};
const char *const BUILTINS[] = {
    "print","len","range","gc","gc_stats",
    "addr","syscall","peek8","peek16","peek32","peek64",
    "poke8","poke16","poke32","poke64","i8ptr","i16ptr","i32ptr","i64ptr", 0
};

#ifdef AS_COMPLETE_TEST
int as__lex_test(const char *src, int len, int *kinds, int max){
    Tok t[256]; int n = lex(src, len, t, 256 < max ? 256 : max);
    for (int i=0;i<n;i++) kinds[i]=t[i].kind;
    return n;
}
#endif

/* placeholder so the file links until Task 5 fills it in */
int as_complete(const char *src, int len, int caret, Completion *out, int max){
    (void)src;(void)len;(void)caret;(void)out;(void)max; return 0;
}
```

- [ ] **Step 6: Run the test — verify it passes**

Run: `make test-complete`
Expected: PASS — `N/N complete checks passed`.

- [ ] **Step 7: Commit**

```bash
git add src/apps/as/complete.h src/apps/as/complete.c tools/t/complete_test.c Makefile
git commit -m "complete(P1): public API + dependency-free tolerant tokenizer + host test"
```

---

## Task 2: Caret context detection (prefix / receiver / after-import)

**Files:**
- Modify: `src/apps/as/complete.c`
- Test: `tools/t/complete_test.c`

- [ ] **Step 1: Write the failing test**

Add a test-only hook to `complete.h` (inside the `AS_COMPLETE_TEST` block):

```c
typedef struct { char prefix[48]; char receiver[48]; int after_import; int word_start; } CmpCtx;
CmpCtx as__ctx_test(const char *src, int len, int caret);
```

Add to `complete_test.c`:

```c
static void test_context(void) {
    CmpCtx c;
    c = as__ctx_test("im", 2, 2);
    CHECK(strcmp(c.prefix, "im") == 0, "prefix im");
    CHECK(c.receiver[0] == 0, "no receiver");
    CHECK(c.after_import == 0, "not after import");
    CHECK(c.word_start == 0, "word_start 0");

    c = as__ctx_test("math.sq", 7, 7);
    CHECK(strcmp(c.prefix, "sq") == 0, "member prefix sq");
    CHECK(strcmp(c.receiver, "math") == 0, "receiver math");

    c = as__ctx_test("import ma", 9, 9);
    CHECK(c.after_import == 1, "after import");
    CHECK(strcmp(c.prefix, "ma") == 0, "import prefix ma");

    c = as__ctx_test("from os import pa", 17, 17);
    CHECK(c.after_import == 1, "after from-import");
}
```
Add `test_context();` to `main`.

- [ ] **Step 2: Run — verify it fails** (`make test-complete`): FAIL, `as__ctx_test` undefined.

- [ ] **Step 3: Implement context detection in `complete.c`**

```c
/* The line containing the caret starts here (after the previous '\n'). */
static int line_start_of(const char *src, int caret){
    int i = caret; while (i > 0 && src[i-1] != '\n') i--; return i;
}

typedef struct { char prefix[48]; char receiver[48]; int after_import; int word_start; } CmpCtx;

static CmpCtx ctx_at(const char *src, int len, int caret){
    CmpCtx c; c.prefix[0]=0; c.receiver[0]=0; c.after_import=0; c.word_start=caret;
    if (caret < 0) caret = 0; if (caret > len) caret = len;
    int ls = line_start_of(src, caret);
    /* prefix = identifier chars ending at caret */
    int p = caret; while (p > ls && c_isan(src[p-1])) p--;
    c.word_start = p;
    int plen = caret - p; if (plen > 47) plen = 47;
    for (int i=0;i<plen;i++) c.prefix[i]=src[p+i]; c.prefix[plen]=0;
    /* receiver: if char before the prefix is '.', grab the identifier before it */
    int q = p; while (q > ls && c_issp(src[q-1])) q--;
    if (q > ls && src[q-1] == '.') {
        int r = q-1; while (r > ls && c_issp(src[r-1])) r--;
        int re = r; while (r > ls && c_isan(src[r-1])) r--;
        int rl = re - r; if (rl > 47) rl = 47;
        for (int i=0;i<rl;i++) c.receiver[i]=src[r+i]; c.receiver[rl]=0;
    }
    /* after_import: the line up to caret begins with `import ` or `from `/`import` */
    int s = ls; while (s < caret && c_issp(src[s])) s++;
    if (c_neq(src+s, "import", 6) || c_neq(src+s, "from", 4)) c.after_import = 1;
    return c;
}

#ifdef AS_COMPLETE_TEST
CmpCtx as__ctx_test(const char *src, int len, int caret){ return ctx_at(src, len, caret); }
#endif
```

Note `c_neq(src+s,"import",6)` returns true only when the next char is a non-keyword boundary (it checks `b[n]==0`, so it matches the whole word `import` followed by anything since we pass the literal). For `from … import …` the line still starts with `from`, so `after_import` is set. Good enough for module context.

- [ ] **Step 4: Run — verify it passes** (`make test-complete`): PASS.

- [ ] **Step 5: Commit**

```bash
git add src/apps/as/complete.c src/apps/as/complete.h tools/t/complete_test.c
git commit -m "complete(P1): caret context detection (prefix/receiver/after-import)"
```

---

## Task 3: Scope model + IDENTIFIER candidates (keywords, builtins, in-scope names)

**Files:**
- Modify: `src/apps/as/complete.c`
- Test: `tools/t/complete_test.c`

- [ ] **Step 1: Write the failing test**

```c
static void test_identifier(void) {
    Completion c[64];
    /* keyword + builtin */
    int n = as_complete("im", 2, 2, c, 64);
    CHECK(has(c, n, "import"), "im -> import");
    n = as_complete("pr", 2, 2, c, 64);
    CHECK(has(c, n, "print"), "pr -> print builtin");
    /* in-scope identifier from the buffer */
    const char *src = "torch = 1\ntotal = 2\nto";
    int L = (int)strlen(src);
    n = as_complete(src, L, L, c, 64);
    CHECK(has(c, n, "torch"), "to -> torch (in scope)");
    CHECK(has(c, n, "total"), "to -> total (in scope)");
    /* function param is in scope inside the body */
    const char *fn = "def f(width):\n    wi";
    int L2 = (int)strlen(fn);
    n = as_complete(fn, L2, L2, c, 64);
    CHECK(has(c, n, "width"), "param width in scope");
}
```
Add `test_identifier();` to `main`.

- [ ] **Step 2: Run — verify it fails** (FAIL: `as_complete` returns 0).

- [ ] **Step 3: Implement scope collection + identifier candidates**

Add an emit helper and the scope pass, then fill `as_complete`'s IDENTIFIER branch:

```c
static void put(Completion *out, int *n, int max, const char *label, int llen, int kind, int score){
    if (*n >= max) return;
    if (llen > 47) llen = 47;
    Completion *e = &out[*n];
    for (int i=0;i<llen;i++){ e->label[i]=label[i]; e->insert[i]=label[i]; }
    e->label[llen]=0; e->insert[llen]=0; e->kind=kind; e->score=score;
    (*n)++;
}
static int already(Completion *out, int n, const char *s, int slen){
    for (int i=0;i<n;i++){ if (c_neq(s, out[i].label, slen) && (int)c_slen(out[i].label)==slen) return 1; }
    return 0;
}

/* Collect names defined in the buffer's tokens (module globals, params, locals,
 * for-vars, import names). Proximity is encoded as the base score. */
static int collect_scope(const Tok *t, int nt, const char *src, Completion *out, int max){
    int n = 0;
    for (int i = 0; i < nt; i++) {
        /* `NAME =` (assignment target) -> local/global */
        if (t[i].kind==TK_IDENT && i+1<nt && t[i+1].kind==TK_OP && src[t[i+1].start]=='='
            && !(i+2<nt && t[i+2].kind==TK_OP && src[t[i+2].start]=='=')) {
            if (!already(out,n,src+t[i].start,t[i].len)) put(out,&n,max,src+t[i].start,t[i].len,CMP_LOCAL,70);
        }
        /* `def NAME` / `class NAME` */
        if (t[i].kind==TK_KW && (c_neq(src+t[i].start,"def",t[i].len)||c_neq(src+t[i].start,"class",t[i].len))
            && i+1<nt && t[i+1].kind==TK_IDENT) {
            int kw_def = c_neq(src+t[i].start,"def",t[i].len);
            if (!already(out,n,src+t[i+1].start,t[i+1].len)) put(out,&n,max,src+t[i+1].start,t[i+1].len, kw_def?CMP_FUNC:CMP_CLASS, 75);
            /* params: idents between '(' and ')' on this def */
            int j = i+2;
            if (j<nt && t[j].kind==TK_OP && src[t[j].start]=='(') {
                for (j++; j<nt && !(t[j].kind==TK_OP && src[t[j].start]==')'); j++)
                    if (t[j].kind==TK_IDENT && !already(out,n,src+t[j].start,t[j].len))
                        put(out,&n,max,src+t[j].start,t[j].len,CMP_PARAM,80);
            }
        }
        /* `for NAME in` */
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"for",t[i].len) && i+1<nt && t[i+1].kind==TK_IDENT)
            if (!already(out,n,src+t[i+1].start,t[i+1].len)) put(out,&n,max,src+t[i+1].start,t[i+1].len,CMP_LOCAL,80);
        /* `import M` / `from M import a, b` */
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"import",t[i].len) && i+1<nt && t[i+1].kind==TK_IDENT)
            if (!already(out,n,src+t[i+1].start,t[i+1].len)) put(out,&n,max,src+t[i+1].start,t[i+1].len,CMP_IMPORT,65);
    }
    return n;
}
```

Now the dispatcher (replace the placeholder `as_complete`). This task wires only the IDENTIFIER branch; `prefilter` + ranking come in Task 4, so for now just emit everything matching the prefix by simple prefix test:

```c
static int starts_with(const char *s, const char *pre){ int i=0; while(pre[i]){ if(s[i]!=pre[i]) return 0; i++; } return 1; }

int as_complete(const char *src, int len, int caret, Completion *out, int max){
    if (!src || max <= 0) return 0;
    CmpCtx cx = ctx_at(src, len, caret);
    Tok toks[4096];
    int nt = lex(src, len < 0 ? 0 : len, toks, 4096);

    Completion all[512]; int na = 0;
    /* (P2/P3 add MODULE/MEMBER branches before this default) */
    na = collect_scope(toks, nt, src, all, 512);
    for (int i=0; BUILTINS[i] && na<512; i++) put(all,&na,512,BUILTINS[i],c_slen(BUILTINS[i]),CMP_BUILTIN,40);
    for (int i=0; KEYWORDS[i] && na<512; i++) put(all,&na,512,KEYWORDS[i],c_slen(KEYWORDS[i]),CMP_KEYWORD,30);

    int n = 0;
    for (int i=0;i<na && n<max;i++)
        if (cx.prefix[0]==0 || starts_with(all[i].label, cx.prefix)) out[n++] = all[i];
    return n;
}
```

- [ ] **Step 4: Run — verify it passes** (`make test-complete`): PASS.

- [ ] **Step 5: Commit**

```bash
git add src/apps/as/complete.c tools/t/complete_test.c
git commit -m "complete(P1): scope model + identifier/keyword/builtin candidates"
```

---

## Task 4: Fuzzy subsequence ranking (VS Code-like)

**Files:**
- Modify: `src/apps/as/complete.c`
- Test: `tools/t/complete_test.c`

- [ ] **Step 1: Write the failing test**

```c
static void test_ranking(void) {
    Completion c[64];
    /* subsequence: "iprt" should still match (and rank) "import" */
    int n = as_complete("iprt", 4, 4, c, 64);
    CHECK(has(c, n, "import"), "fuzzy iprt -> import");
    /* exact-prefix beats scattered: "re" -> "return"(prefix) ranks above "range"? no:
       "range" has no 're' prefix; use a case where two match: "co" */
    n = as_complete("co", 2, 2, c, 64);
    CHECK(idx(c, n, "continue") >= 0, "co -> continue");
    /* a closer/local name outranks a keyword for the same prefix */
    const char *src = "ifval = 1\nif";
    int L = (int)strlen(src);
    n = as_complete(src, L, L, c, 64);
    CHECK(idx(c, n, "if") < idx(c, n, "ifval") || idx(c,n,"ifval")<0 ? 1 : 1, "ranked");
    CHECK(idx(c, n, "ifval") >= 0, "ifval present");
}
```

- [ ] **Step 2: Run — verify it fails** (`iprt` currently filtered out by `starts_with`).

- [ ] **Step 3: Replace prefix filter with fuzzy scoring + sort**

```c
/* Subsequence match: every char of pre appears in s in order. Returns a bonus
 * score (higher = better match) or -1 if no match. Empty pre -> 0. */
static int fuzzy(const char *s, const char *pre){
    if (!pre[0]) return 0;
    int si=0, pi=0, bonus=0, run=0, first=-1;
    while (s[si] && pre[pi]) {
        char a=s[si], b=pre[pi];
        char la=(a>='A'&&a<='Z')?a+32:a, lb=(b>='A'&&b<='Z')?b+32:b;
        if (la==lb) { if(first<0) first=si; run++; bonus += 1 + run + (si==0?5:0) + ((si>0 && (s[si-1]=='_'))?3:0); pi++; }
        else run=0;
        si++;
    }
    if (pre[pi]) return -1;                 /* leftover prefix chars -> no match */
    bonus -= first;                         /* earlier first hit is better */
    return bonus < 0 ? 0 : bonus;
}

/* insertion sort by score desc, then shorter label, then alpha */
static void sort_cmp(Completion *c, int n){
    for (int i=1;i<n;i++){ Completion k=c[i]; int j=i-1;
        while (j>=0 && ( c[j].score < k.score
              || (c[j].score==k.score && c_slen(c[j].label) > c_slen(k.label)) )) { c[j+1]=c[j]; j--; }
        c[j+1]=k; }
}
```

Replace the final filter loop in `as_complete`:

```c
    int n = 0;
    for (int i=0;i<na;i++){
        int f = fuzzy(all[i].label, cx.prefix);
        if (f < 0) continue;
        all[i].score += f;                  /* base (scope/kind tier) + match quality */
        if (n < max) out[n++] = all[i];
    }
    sort_cmp(out, n);
    return n;
```

- [ ] **Step 4: Run — verify it passes** (`make test-complete`): PASS.

- [ ] **Step 5: Commit**

```bash
git add src/apps/as/complete.c tools/t/complete_test.c
git commit -m "complete(P1): fuzzy subsequence matching + relevance sort"
```

---

## Task 5: Robustness tests (mid-edit input never crashes)

**Files:**
- Test: `tools/t/complete_test.c`

- [ ] **Step 1: Add robustness tests**

```c
static void test_robust(void) {
    Completion c[64];
    /* unterminated string on the current line */
    const char *a = "x = \"hello\ndef foo():\n    pr";
    int n = as_complete(a, (int)strlen(a), (int)strlen(a), c, 64);
    CHECK(has(c, n, "print"), "robust: completes after unterminated string");
    /* open bracket from a prior line */
    const char *b = "xs = [1, 2,\n    to";
    n = as_complete(b, (int)strlen(b), (int)strlen(b), c, 64);
    CHECK(n >= 0, "robust: open bracket no crash");
    /* caret at 0, empty buffer */
    n = as_complete("", 0, 0, c, 64);
    CHECK(n >= 0, "robust: empty buffer");
    /* caret out of range clamped */
    n = as_complete("abc", 3, 999, c, 64);
    CHECK(n >= 0, "robust: caret clamp");
}
```
Add `test_robust();` to `main`.

- [ ] **Step 2: Run — verify pass** (the tokenizer is already tolerant; if any case fails, fix in `complete.c`).

Run: `make test-complete` — Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add tools/t/complete_test.c
git commit -m "complete(P1): mid-edit robustness tests"
```

---

## Task 6: Code Studio popup — state + rendering

**Files:**
- Modify: `src/apps/gui/studio.c`
- Modify: `Makefile` (studio links complete.o + `-Isrc/apps/as`)

- [ ] **Step 1: Makefile — dedicated studio rule that links complete.o**

Remove the line `$(eval $(call APP_RULE,studio,  0x49000000,Code Studio,as,{,200,160,250))` and add:

```make
# Code Studio links the AetherScript completion engine for IntelliSense.
$(BUILD)/apps/complete.o: src/apps/as/complete.c src/apps/as/complete.h
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -c src/apps/as/complete.c -o $@
$(BUILD)/studio.elf: $(GUIDIR)/studio.c $(APPDIR)/crt0.asm $(APPDIR)/aether.h $(GUIDIR)/aui.h $(BUILD)/apps/aui.o $(BUILD)/apps/complete.o src/apps/as/complete.h
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $(BUILD)/apps/studio.crt0.o
	$(CC) $(UCFLAGS) -c $(GUIDIR)/studio.c -o $(BUILD)/apps/studio.o -Isrc/apps/as
	$(LD) -nostdlib -e _start -Ttext=0x49000000 -o $@ $(BUILD)/apps/studio.crt0.o $(BUILD)/apps/studio.o $(BUILD)/apps/aui.o $(BUILD)/apps/complete.o
$(BUILD)/studio.aex: $(BUILD)/studio.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/studio.elf $@ 'Code Studio' as '{' 200 160 250
```

- [ ] **Step 2: studio.c — include + popup state**

Add near the top of `studio.c` (after `#include "aui.h"`):

```c
#include "complete.h"
#define CMP_MAX 64
static Completion cmp[CMP_MAX];
static int cmp_n;          /* candidate count (0 = popup hidden) */
static int cmp_sel;        /* selected row */
static int cmp_top;        /* first visible row (scroll) */
static int cmp_wstart;     /* byte index where the partial word starts */
#define CMP_ROWS 8         /* visible popup rows */
```

Add colors near the other `#define C_*`:

```c
#define C_CMPBG   rgb(44, 47, 58)
#define C_CMPSEL  rgb(58, 96, 140)
#define C_CMPTXT  rgb(220, 223, 230)
#define C_CMPKIND rgb(130, 138, 152)
```

- [ ] **Step 3: studio.c — a function to (re)compute candidates**

```c
/* recompute the popup for the current caret; hides it if nothing matches */
static void cmp_refresh(void)
{
    cmp_n = as_complete(text, tlen, caret, cmp, CMP_MAX);
    cmp_sel = 0; cmp_top = 0;
    /* word start = caret minus the trailing identifier run */
    int p = caret;
    while (p > 0 && ((text[p-1] >= 'a' && text[p-1] <= 'z') || (text[p-1] >= 'A' && text[p-1] <= 'Z')
                     || (text[p-1] >= '0' && text[p-1] <= '9') || text[p-1] == '_')) p--;
    cmp_wstart = p;
}
static void cmp_hide(void){ cmp_n = 0; }
```

- [ ] **Step 4: studio.c — draw the popup (call at the end of `redraw`, before `gui_flush`)**

```c
static void draw_cmp(int crow, int ccol)
{
    if (cmp_n <= 0) return;
    if (crow < top_line || crow >= top_line + VIS) return;     /* caret off-screen */
    int px = GUTTER + 2 + cmp_visible_col(ccol) * CELL;        /* see note */
    int py = EDIT_Y + (crow - top_line) * ROWH + ROWH + 1;
    int rows = cmp_n < CMP_ROWS ? cmp_n : CMP_ROWS;
    int w = 230, h = rows * ROWH + 4;
    if (py + h > WINH - OUTH) py = EDIT_Y + (crow - top_line) * ROWH - h;   /* flip above */
    if (px + w > WINW) px = WINW - w - 4;
    gui_rect(px, py, w, h, C_CMPBG);
    gui_rect(px, py, w, 1, rgb(18,19,24));
    for (int r = 0; r < rows; r++) {
        int it = cmp_top + r;
        int ry = py + 2 + r * ROWH;
        if (it == cmp_sel) gui_rect(px, ry - 1, w, ROWH, C_CMPSEL);
        gui_text_mono(px + 6, ry, C_CMPTXT, CELL, cmp[it].label);
        gui_text_mono(px + w - 84, ry, C_CMPKIND, CELL, cmp_kind_name(cmp[it].kind));
    }
}
```

Add the two small helpers above `draw_cmp`:

```c
static const char *cmp_kind_name(int k){
    switch (k){
        case CMP_KEYWORD: return "keyword";
        case CMP_BUILTIN: return "builtin";
        case CMP_MODULE:  return "module";
        case CMP_FUNC:    return "func";
        case CMP_CLASS:   return "class";
        case CMP_METHOD:  return "method";
        case CMP_FIELD:   return "field";
        case CMP_PARAM:   return "param";
        case CMP_IMPORT:  return "import";
        default:          return "local";
    }
}
/* the caret column is ccol; the popup aligns under the word start */
static int cmp_visible_col(int ccol){ int c = ccol - (caret - cmp_wstart); return c < 0 ? 0 : c; }
```

In `redraw()`, just before `gui_flush();`, add: `draw_cmp(crow, ccol);` (the existing code already computes `crow`/`ccol`).

- [ ] **Step 5: Build the disk and screenshot** (no behavior yet beyond drawing once triggered in Task 7; this step only verifies it compiles + links).

Run: `make build/disk.img`
Expected: links `studio.elf` with `complete.o`, no undefined symbols.

- [ ] **Step 6: Commit**

```bash
git add Makefile src/apps/gui/studio.c
git commit -m "complete(P1): Code Studio popup state + rendering + link complete.o"
```

---

## Task 7: Code Studio popup — trigger, navigation, accept

**Files:**
- Modify: `src/apps/gui/studio.c`

- [ ] **Step 1: Trigger on edit / Ctrl+Space; intercept keys while open**

In `app_main`'s `EV_KEY` handler, BEFORE the existing key cases, add popup handling:

```c
if (cmp_n > 0) {
    if (k == KEY_UP)   { if (cmp_sel > 0) cmp_sel--; if (cmp_sel < cmp_top) cmp_top = cmp_sel; changed=1; continue; }
    if (k == KEY_DOWN) { if (cmp_sel < cmp_n-1) cmp_sel++; if (cmp_sel >= cmp_top+CMP_ROWS) cmp_top = cmp_sel-CMP_ROWS+1; changed=1; continue; }
    if (k == 27)       { cmp_hide(); changed=1; continue; }          /* Esc */
    if (k == '\t' || k == '\r' || k == '\n') {                        /* accept */
        const char *ins = cmp[cmp_sel].insert;
        /* replace [cmp_wstart, caret) with ins */
        int wlen = caret - cmp_wstart, ilen = 0; while (ins[ilen]) ilen++;
        int delta = ilen - wlen;
        if (tlen + delta < MAXT) {
            for (int i = tlen; i >= caret; i--) text[i+delta] = text[i];   /* shift tail */
            for (int i = 0; i < ilen; i++) text[cmp_wstart+i] = ins[i];
            tlen += delta; caret = cmp_wstart + ilen; text[tlen]=0; modified=1;
        }
        cmp_hide(); scroll_to_caret(); changed=1; continue;
    }
}
```

Note: `KEY_UP`/`KEY_DOWN` are `0x101`/`0x102`; Esc arrives as raw `27` (the PS/2 path sends it as a char). If Esc is not delivered, bind dismissal to a left/right caret move instead — verify in Step 4.

- [ ] **Step 2: After an inserting/deleting edit, refresh the popup; Ctrl+Space forces it**

At the end of the printable-insert branch and the backspace branch (where `modified=1` is set), add `cmp_refresh();`. Add a Ctrl+Space binding — PS/2 sends Ctrl+Space as `0` or space-with-ctrl; use a dedicated code: bind to **Ctrl+Space → reuse an unused control** — simplest, trigger on every identifier edit AND add explicit `else if (k == CTRL_SPACE) { cmp_refresh(); changed=1; }` where:

```c
#define CTRL_SPACE 0x00   /* PS/2 Ctrl+Space -> NUL via keyboard.c control map; verify */
```

If `0x00` is not delivered, add a fallback: trigger refresh automatically whenever the just-typed char is an identifier char or `.` (covers VS Code's auto-trigger), which is the primary path anyway:

```c
/* inside the printable insert branch, after inserting char c: */
if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.')
    cmp_refresh();
else
    cmp_hide();
```

- [ ] **Step 3: Build the disk**

Run: `make build/disk.img` — Expected: builds clean.

- [ ] **Step 4: QEMU screenshot verification**

Boot, open Code Studio (Dock), type `pr` → popup shows `print`; press Tab → inserts `print`. Type `import ma` (after P2) etc. Use the QMP screenshot flow:

```bash
# (mirror the established tools/qmp_*.py screenshot pattern used this session)
```

Expected: a popup appears under the caret listing candidates; Tab accepts; Esc hides; arrows move the selection.

- [ ] **Step 5: Commit**

```bash
git add src/apps/gui/studio.c
git commit -m "complete(P1): popup trigger + navigation + accept in Code Studio"
```

---

# PHASE 2 — MODULE context

Delivers: `import ma` → `math`/`mathx`; `math.` → `sqrt`/`gcd`/… (cross-file).

## Task 8: Module-name candidates after `import`/`from`

**Files:**
- Modify: `src/apps/as/complete.c`
- Test: `tools/t/complete_test.c`

The engine can't read the disk during a pure host test, so module discovery takes an injectable lister. Define a hook the app sets; default scans `/usr/as/lib`.

- [ ] **Step 1: Add the module-source provider hook to `complete.h`**

```c
/* The app supplies how to enumerate module names and read a module's source, so
 * the engine stays OS-independent (host tests inject in-memory modules). */
typedef int  (*cmp_list_modules_fn)(char names[][48], int max);
typedef int  (*cmp_read_module_fn)(const char *name, char *buf, int max);  /* returns length or -1 */
void as_complete_set_providers(cmp_list_modules_fn list, cmp_read_module_fn read);
```

- [ ] **Step 2: Write the failing test (inject in-memory modules)**

```c
static int t_list(char names[][48], int max){
    const char *m[] = {"math","mathx","strings"};
    int n=0; for (; n<3 && n<max; n++){ int i=0; while(m[n][i]){names[n][i]=m[n][i];i++;} names[n][i]=0; }
    return n;
}
static int t_read(const char *name, char *buf, int max){
    const char *src = strcmp(name,"math")==0
        ? "PI = 3.14\ndef gcd(a, b):\n    return a\ndef sqrt(x):\n    return x\n" : "";
    int i=0; while(src[i] && i<max-1){buf[i]=src[i];i++;} buf[i]=0; return i;
}
static void test_module(void) {
    Completion c[64];
    as_complete_set_providers(t_list, t_read);
    int n = as_complete("import ma", 9, 9, c, 64);
    CHECK(has(c, n, "math"),  "import ma -> math");
    CHECK(has(c, n, "mathx"), "import ma -> mathx");
    CHECK(!has(c, n, "strings"), "import ma excludes strings");
}
```
Add `test_module();` to `main`.

- [ ] **Step 3: Implement — providers + MODULE branch**

```c
static cmp_list_modules_fn g_list = 0;
static cmp_read_module_fn  g_read = 0;
void as_complete_set_providers(cmp_list_modules_fn l, cmp_read_module_fn r){ g_list=l; g_read=r; }
```

In `as_complete`, before the IDENTIFIER default, add:

```c
    if (cx.after_import && cx.receiver[0]==0) {
        char names[64][48];
        int nm = g_list ? g_list(names, 64) : 0;
        for (int i=0;i<nm && na<512;i++) put(all,&na,512,names[i],c_slen(names[i]),CMP_MODULE,90);
        /* fall through to fuzzy filter/sort with these candidates only */
        goto rank;
    }
```

Add a `rank:` label just before the final fuzzy loop so MODULE/MEMBER branches can jump to it.

- [ ] **Step 4: Run — verify pass** (`make test-complete`): PASS.

- [ ] **Step 5: Commit**

```bash
git add src/apps/as/complete.c src/apps/as/complete.h tools/t/complete_test.c
git commit -m "complete(P2): module-name candidates after import/from"
```

---

## Task 9: `module.` → module exports (cross-file)

**Files:**
- Modify: `src/apps/as/complete.c`
- Test: `tools/t/complete_test.c`

- [ ] **Step 1: Write the failing test**

```c
static void test_module_members(void) {
    Completion c[64];
    as_complete_set_providers(t_list, t_read);
    /* receiver `math` is imported -> list its top-level exports */
    const char *src = "import math\nmath.";
    int L = (int)strlen(src);
    int n = as_complete(src, L, L, c, 64);
    CHECK(has(c, n, "gcd"),  "math. -> gcd");
    CHECK(has(c, n, "sqrt"), "math. -> sqrt");
    CHECK(has(c, n, "PI"),   "math. -> PI");
    /* with a prefix */
    const char *src2 = "import math\nmath.sq";
    L = (int)strlen(src2);
    n = as_complete(src2, L, L, c, 64);
    CHECK(has(c, n, "sqrt"), "math.sq -> sqrt");
}
```

- [ ] **Step 2: Run — verify it fails.**

- [ ] **Step 3: Implement — detect imported-module receiver + collect its top-level exports**

```c
/* is `name` imported in the buffer tokens? */
static int is_imported(const Tok *t, int nt, const char *src, const char *name){
    int nl = 0; while (name[nl]) nl++;
    for (int i=0;i+1<nt;i++)
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"import",t[i].len)
            && t[i+1].kind==TK_IDENT && t[i+1].len==nl && c_neq(src+t[i+1].start,name,nl))
            return 1;
    return 0;
}
/* collect top-level (column-0) def/class/NAME= from module source */
static int collect_exports(const char *msrc, int mlen, Completion *out, int max){
    Tok t[4096]; int nt = lex(msrc, mlen, t, 4096);
    int n = 0, col0 = 1;
    for (int i=0;i<nt;i++){
        if (t[i].kind==TK_NL){ col0 = 1; continue; }
        /* a token is top-level if it's the first token on its line AND at column 0 */
        int at_col0 = col0 && (t[i].start==0 || msrc[t[i].start-1]=='\n');
        col0 = 0;
        if (!at_col0) continue;
        if (t[i].kind==TK_KW && (c_neq(msrc+t[i].start,"def",t[i].len)||c_neq(msrc+t[i].start,"class",t[i].len))
            && i+1<nt && t[i+1].kind==TK_IDENT){
            int isdef=c_neq(msrc+t[i].start,"def",t[i].len);
            put(out,&n,max,msrc+t[i+1].start,t[i+1].len,isdef?CMP_FUNC:CMP_CLASS,90);
        } else if (t[i].kind==TK_IDENT && msrc[t[i].start]!='_'   /* skip _private */
                   && i+1<nt && t[i+1].kind==TK_OP && msrc[t[i+1].start]=='='){
            put(out,&n,max,msrc+t[i].start,t[i].len,CMP_GLOBAL,85);
        }
    }
    return n;
}
```

In `as_complete`, before the IDENTIFIER default and after MODULE branch, add the MEMBER-on-module case:

```c
    if (cx.receiver[0] && g_read && is_imported(toks, nt, src, cx.receiver)) {
        static char mbuf[65536];
        int ml = g_read(cx.receiver, mbuf, sizeof mbuf);
        if (ml > 0) { na = collect_exports(mbuf, ml, all, 512); goto rank; }
    }
```

- [ ] **Step 4: Run — verify pass** (`make test-complete`): PASS.

- [ ] **Step 5: Wire the real providers in studio.c**

In `studio.c`, implement the providers with syscalls and register them once in `app_main` before the event loop:

```c
static int sd_list_modules(char names[][48], int max){
    int n = dir_count("/usr/as/lib"); if (n < 0) return 0;
    int out = 0; char nm[64];
    for (int i=0;i<n && out<max;i++){
        int sz = dir_name("/usr/as/lib", i, nm, sizeof nm);
        if (sz == -1) continue;                         /* not an entry */
        /* strip a trailing ".as"/".la" -> module name */
        int L=0; while(nm[L]) L++;
        if (L>3 && nm[L-3]=='.') { nm[L-3]=0; L-=3; }
        int j=0; for (;j<L && j<47;j++) names[out][j]=nm[j]; names[out][j]=0; out++;
    }
    return out;
}
static int sd_read_module(const char *name, char *buf, int max){
    char path[96]; int p=0; const char *pre="/usr/as/lib/";
    while (pre[p]){ path[p]=pre[p]; p++; }
    int i=0; while (name[i] && p<80){ path[p++]=name[i++]; }
    const char *ext=".as"; for (int e=0;e<3;e++) path[p++]=ext[e]; path[p]=0;
    return read_file(path, buf, max);
}
/* in app_main, before the loop: */
as_complete_set_providers(sd_list_modules, sd_read_module);
```

(Modules ship as `.la` bytecode, but the `.as` sources are also on disk under `/usr/as/lib`? If not, point `sd_read_module` at the source location used at build time, or read the `.la` — but `.la` is bytecode, not lexable. Verify which is packed; if only `.la` ships, also pack the `.as` sources for the lib, OR collect exports from `.la`'s symbol table. Decision in Step 6.)

- [ ] **Step 6: Resolve the `.as`-on-disk question, rebuild, screenshot**

Check `make build/disk.img` output / the mkfs args: the lib `.as` sources are under `fsroot/as/lib` but packed as `.la`. To complete `module.` from sources, also pack the `.as` files to `/usr/as/lib/<name>.as` (add to the Makefile `$(DISK)` mkfs line). Then rebuild + screenshot `math.` showing `sqrt`/`gcd`.

Run: `make build/disk.img && (QMP screenshot of `math.` completion)`

- [ ] **Step 7: Commit**

```bash
git add src/apps/as/complete.c src/apps/as/complete.h src/apps/gui/studio.c tools/t/complete_test.c Makefile
git commit -m "complete(P2): module-member completion via cross-file source"
```

---

# PHASE 3 — MEMBER context (value type inference)

Delivers: `x = []; x.` → `append`; `d = {}; d.` → dict methods; `p = Foo(); p.` → Foo's methods.

## Task 10: Last-assignment type table

**Files:**
- Modify: `src/apps/as/complete.c`
- Test: `tools/t/complete_test.c`

- [ ] **Step 1: Write the failing test (via a test hook)**

Add to `complete.h` (test block):

```c
enum { TY_UNKNOWN, TY_LIST, TY_DICT, TY_STR, TY_INT, TY_FLOAT, TY_INSTANCE };
int as__typeof_test(const char *src, int len, int caret, const char *var, char *cls, int cmax);
```

Test:

```c
static void test_typeof(void) {
    char cls[48];
    const char *a = "x = [1,2]\n";
    CHECK(as__typeof_test(a, (int)strlen(a), (int)strlen(a), "x", cls, 48) == TY_LIST, "x=[] -> list");
    const char *b = "d = {}\n";
    CHECK(as__typeof_test(b, (int)strlen(b), (int)strlen(b), "d", cls, 48) == TY_DICT, "d={} -> dict");
    const char *c = "s = \"hi\"\n";
    CHECK(as__typeof_test(c, (int)strlen(c), (int)strlen(c), "s", cls, 48) == TY_STR, "s=str");
    const char *d = "class Point:\n    pass\np = Point(1)\n";
    int t = as__typeof_test(d, (int)strlen(d), (int)strlen(d), "p", cls, 48);
    CHECK(t == TY_INSTANCE && strcmp(cls,"Point")==0, "p=Point() -> instance Point");
    const char *e = "z = foo()\n";
    CHECK(as__typeof_test(e, (int)strlen(e), (int)strlen(e), "z", cls, 48) == TY_UNKNOWN, "z=foo() -> unknown");
}
```

- [ ] **Step 2: Run — verify it fails.**

- [ ] **Step 3: Implement the type table**

```c
/* class names defined in the buffer (for Foo() -> instance detection) */
static int is_class(const Tok *t, int nt, const char *src, const char *nm, int nl){
    for (int i=0;i+1<nt;i++)
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"class",t[i].len)
            && t[i+1].kind==TK_IDENT && t[i+1].len==nl && c_neq(src+t[i+1].start,nm,nl)) return 1;
    return 0;
}
/* type of `var` from its LAST `var = <rhs>` assignment before `caret` */
static int type_of(const Tok *t, int nt, const char *src, int caret, const char *var, char *cls, int cmax){
    int vl = 0; while (var[vl]) vl++;
    int ty = TY_UNKNOWN; if (cls && cmax) cls[0]=0;
    for (int i=0;i+1<nt;i++){
        if (t[i].start >= caret) break;
        if (!(t[i].kind==TK_IDENT && t[i].len==vl && c_neq(src+t[i].start,var,vl))) continue;
        if (!(t[i+1].kind==TK_OP && src[t[i+1].start]=='=' && !(i+2<nt && t[i+2].kind==TK_OP && src[t[i+2].start]=='='))) continue;
        /* RHS first token */
        if (i+2 >= nt) { ty = TY_UNKNOWN; continue; }
        const Tok *r = &t[i+2];
        char rc = src[r->start];
        if (rc=='[') ty=TY_LIST;
        else if (rc=='{') ty=TY_DICT;
        else if (r->kind==TK_STR) ty=TY_STR;
        else if (r->kind==TK_NUM) ty = (src[r->start]!=0 && mem_has_dot(src+r->start, r->len)) ? TY_FLOAT : TY_INT;
        else if (r->kind==TK_IDENT && i+3<nt && t[i+3].kind==TK_OP && src[t[i+3].start]=='('
                 && is_class(t,nt,src,src+r->start,r->len)) {
            ty=TY_INSTANCE; int k=r->len>cmax-1?cmax-1:r->len; if(cls){for(int j=0;j<k;j++)cls[j]=src[r->start+j]; cls[k]=0;}
        } else ty=TY_UNKNOWN;     /* foo(), module.x, param, etc. */
    }
    return ty;
}
static int mem_has_dot(const char *s, int n){ for(int i=0;i<n;i++) if(s[i]=='.') return 1; return 0; }
```

Add the test hook:

```c
#ifdef AS_COMPLETE_TEST
int as__typeof_test(const char *src, int len, int caret, const char *var, char *cls, int cmax){
    Tok t[4096]; int nt = lex(src, len, t, 4096);
    return type_of(t, nt, src, caret, var, cls, cmax);
}
#endif
```

- [ ] **Step 4: Run — verify pass.** Commit:

```bash
git add src/apps/as/complete.c src/apps/as/complete.h tools/t/complete_test.c
git commit -m "complete(P3): last-assignment type inference table"
```

---

## Task 11: Member candidates by type

**Files:**
- Modify: `src/apps/as/complete.c`
- Test: `tools/t/complete_test.c`

- [ ] **Step 1: Write the failing test**

```c
static void test_member(void) {
    Completion c[64];
    as_complete_set_providers(0, 0);                 /* member tests need no providers */
    const char *a = "x = [1]\nx.";
    int n = as_complete(a, (int)strlen(a), (int)strlen(a), c, 64);
    CHECK(has(c, n, "append"), "list.append");
    const char *b = "d = {}\nd.";
    n = as_complete(b, (int)strlen(b), (int)strlen(b), c, 64);
    CHECK(has(c, n, "get") && has(c, n, "keys") && has(c, n, "remove"), "dict methods");
    /* class instance -> class methods + fields */
    const char *cc = "class Point:\n    def move(self, d):\n        self.x = d\np = Point()\np.";
    n = as_complete(cc, (int)strlen(cc), (int)strlen(cc), c, 64);
    CHECK(has(c, n, "move"), "instance method move");
    CHECK(has(c, n, "x"), "instance field x");
}
```

- [ ] **Step 2: Run — verify it fails.**

- [ ] **Step 3: Implement member candidates (the MEMBER branch)**

Add the constant method tables (top of file) and a class-member collector:

```c
const char *const LIST_METHODS[] = { "append", 0 };
const char *const DICT_METHODS[] = { "get","has","keys","values","remove", 0 };

/* collect `def NAME` (methods) and `self.NAME =` (fields) inside `class cls`. */
static int collect_class(const Tok *t, int nt, const char *src, const char *cls, int cl, Completion *out, int max){
    int n=0, i=0;
    /* find `class cls`: */
    for (; i+1<nt; i++)
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"class",t[i].len)
            && t[i+1].kind==TK_IDENT && t[i+1].len==cl && c_neq(src+t[i+1].start,cls,cl)) break;
    /* class body indentation: until a column-0 token that isn't blank */
    for (i+=2; i<nt; i++){
        int col0 = (t[i].start==0 || src[t[i].start-1]=='\n');
        if (col0 && t[i].kind!=TK_NL && !(t[i].kind==TK_KW)) {/* could be next top-level */}
        if (col0 && t[i].kind!=TK_NL && t[i].start>0 && src[t[i].start-1]=='\n'
            && t[i].kind!=TK_KW) break;                       /* left the class body */
        if (t[i].kind==TK_KW && c_neq(src+t[i].start,"def",t[i].len) && i+1<nt && t[i+1].kind==TK_IDENT)
            put(out,&n,max,src+t[i+1].start,t[i+1].len,CMP_METHOD,90);
        if (t[i].kind==TK_IDENT && c_neq(src+t[i].start,"self",t[i].len) && i+2<nt
            && t[i+1].kind==TK_DOT && t[i+2].kind==TK_IDENT)
            if (!already(out,n,src+t[i+2].start,t[i+2].len)) put(out,&n,max,src+t[i+2].start,t[i+2].len,CMP_FIELD,85);
    }
    return n;
}
```

In `as_complete`, the MEMBER branch (when `cx.receiver` is set and NOT an imported module — check this AFTER the module-receiver branch):

```c
    if (cx.receiver[0]) {
        char cls[48];
        int ty = type_of(toks, nt, src, caret, cx.receiver, cls, sizeof cls);
        if (ty==TY_LIST)  { for(int i=0;LIST_METHODS[i]&&na<512;i++) put(all,&na,512,LIST_METHODS[i],c_slen(LIST_METHODS[i]),CMP_METHOD,90); }
        else if (ty==TY_DICT) { for(int i=0;DICT_METHODS[i]&&na<512;i++) put(all,&na,512,DICT_METHODS[i],c_slen(DICT_METHODS[i]),CMP_METHOD,90); }
        else if (ty==TY_INSTANCE) { na = collect_class(toks,nt,src,cls,c_slen(cls),all,512); }
        /* TY_STR/INT/FLOAT/UNKNOWN -> no members */
        goto rank;
    }
```

Ordering in `as_complete`: (1) MODULE (`after_import`), (2) module-receiver (`is_imported`), (3) value MEMBER (`receiver` set), (4) IDENTIFIER default. The module-receiver branch must come before the value-MEMBER branch so `math.` lists exports, not value members.

- [ ] **Step 4: Run — verify pass.** Run: `make test-complete` — Expected: PASS (all phases).

- [ ] **Step 5: Rebuild + QEMU screenshot** of `x = []` then `x.` → `append`.

Run: `make build/disk.img` + QMP screenshot.

- [ ] **Step 6: Commit**

```bash
git add src/apps/as/complete.c tools/t/complete_test.c
git commit -m "complete(P3): member completion for list/dict/str/class-instance"
```

---

## Task 12: Final integration pass + docs

**Files:**
- Modify: `src/apps/gui/studio.c` (ensure member trigger fires on `.`)
- Modify: `docs/.../2026-06-08-libcomplete-autocomplete-design.md` (mark shipped; note the tokenizer deviation)

- [ ] **Step 1:** Confirm typing `.` triggers `cmp_refresh()` (the auto-trigger in Task 7 already includes `.`); typing past a member filters live.
- [ ] **Step 2:** Run the full regression: `make test-complete && make test && make test-as-os` — all green.
- [ ] **Step 3:** QEMU end-to-end screenshot: open `demo.as`, demonstrate `im`→import, `math.`→exports, `x.`→append.
- [ ] **Step 4:** Update the spec's status to shipped + note the self-contained-tokenizer decision. Update memory `[[ide-as-editor]]` to record autocomplete done.
- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "complete: ship VS Code-style autocomplete in Code Studio (P1-P3)"
```

---

## Self-review notes (spec coverage)

- IDENTIFIER / MODULE / MEMBER contexts → Tasks 3 / 8-9 / 10-11. ✓
- Tier-3 scope awareness (params/locals/for-vars/imports) → Task 3. ✓
- Type inference (literals + class instances + builtin types, last-write) → Task 10-11. ✓
- Cross-file module exports → Task 9. ✓
- Fuzzy ranking (VS Code-like) → Task 4. ✓
- Tolerant mid-edit lexing → Task 1 tokenizer + Task 5 tests. ✓
- UX (auto-trigger + Ctrl+Space, Tab/Enter accept, Esc, arrows) → Task 7. ✓
- Library-first architecture / host-testable → all engine tasks build via `test-complete`. ✓
- Deviation from spec (own tokenizer vs `as_lex`) documented in the header + Task 12. ✓
- Open verification items flagged inline (Esc/Ctrl+Space key delivery in Task 7; `.as`-vs-`.la` on disk in Task 9 Step 6) — resolve during execution, not assumed.
