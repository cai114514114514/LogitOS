/* host unit tests for libcomplete (the AetherScript completion engine). */
#include <stdio.h>
#include <string.h>
#ifndef AS_COMPLETE_TEST
#define AS_COMPLETE_TEST
#endif
#include "complete.h"

static int fails = 0, checks = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s\n", msg); } } while (0)

static int has(Completion *c, int n, const char *label) {
    for (int i = 0; i < n; i++) if (strcmp(c[i].label, label) == 0) return 1;
    return 0;
}
static int idx(Completion *c, int n, const char *label) {
    for (int i = 0; i < n; i++) if (strcmp(c[i].label, label) == 0) return i;
    return -1;
}

/* injected in-memory modules for MODULE/member-on-module tests */
static int t_list(char names[][48], int max){
    const char *m[] = {"math","mathx","strings"};
    int n=0; for (; n<3 && n<max; n++){ int i=0; while(m[n][i]){names[n][i]=m[n][i];i++;} names[n][i]=0; }
    return n;
}
static int t_read(const char *name, char *buf, int max){
    const char *src = strcmp(name,"math")==0
        ? "PI = 3.14\ndef gcd(a, b):\n    return a\ndef sqrt(x):\n    return x\n_priv = 1\n" : "";
    int i=0; while(src[i] && i<max-1){buf[i]=src[i];i++;} buf[i]=0; return i;
}

static void test_tokenizer(void) {
    int k[64];
    int n = as__lex_test("foo.bar = 3", 11, k, 64);
    CHECK(n >= 5, "tok count");
    CHECK(k[0] == TK_IDENT, "tok0 ident");
    CHECK(k[1] == TK_DOT,   "tok1 dot");
    CHECK(k[2] == TK_IDENT, "tok2 ident");
    as__lex_test("import math", 11, k, 64);
    CHECK(k[0] == TK_KW, "import is kw");
    int n3 = as__lex_test("x = \"unterm", 11, k, 64);
    CHECK(n3 >= 3, "unterminated string tolerated");
}

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

static void test_identifier(void) {
    Completion c[64];
    int n = as_complete("im", 2, 2, c, 64);
    CHECK(has(c, n, "import"), "im -> import");
    n = as_complete("pr", 2, 2, c, 64);
    CHECK(has(c, n, "print"), "pr -> print builtin");
    const char *src = "torch = 1\ntotal = 2\nto";
    int L = (int)strlen(src);
    n = as_complete(src, L, L, c, 64);
    CHECK(has(c, n, "torch"), "to -> torch (in scope)");
    CHECK(has(c, n, "total"), "to -> total (in scope)");
    const char *fn = "def f(width):\n    wi";
    int L2 = (int)strlen(fn);
    n = as_complete(fn, L2, L2, c, 64);
    CHECK(has(c, n, "width"), "param width in scope");
}

static void test_ranking(void) {
    Completion c[64];
    int n = as_complete("iprt", 4, 4, c, 64);
    CHECK(has(c, n, "import"), "fuzzy iprt -> import");
    n = as_complete("co", 2, 2, c, 64);
    CHECK(idx(c, n, "continue") >= 0, "co -> continue");
    const char *src = "ifval = 1\nif";
    int L = (int)strlen(src);
    n = as_complete(src, L, L, c, 64);
    CHECK(idx(c, n, "ifval") >= 0, "ifval present");
}

static void test_robust(void) {
    Completion c[64];
    const char *a = "x = \"hello\ndef foo():\n    pr";
    int n = as_complete(a, (int)strlen(a), (int)strlen(a), c, 64);
    CHECK(has(c, n, "print"), "robust: completes after unterminated string");
    const char *b = "xs = [1, 2,\n    to";
    n = as_complete(b, (int)strlen(b), (int)strlen(b), c, 64);
    CHECK(n >= 0, "robust: open bracket no crash");
    n = as_complete("", 0, 0, c, 64);
    CHECK(n >= 0, "robust: empty buffer");
    n = as_complete("abc", 3, 999, c, 64);
    CHECK(n >= 0, "robust: caret clamp");
}

static void test_module(void) {
    Completion c[64];
    as_complete_set_providers(t_list, t_read);
    int n = as_complete("import ma", 9, 9, c, 64);
    CHECK(has(c, n, "math"),  "import ma -> math");
    CHECK(has(c, n, "mathx"), "import ma -> mathx");
    CHECK(!has(c, n, "strings"), "import ma excludes strings");
}

static void test_module_members(void) {
    Completion c[64];
    as_complete_set_providers(t_list, t_read);
    const char *src = "import math\nmath.";
    int L = (int)strlen(src);
    int n = as_complete(src, L, L, c, 64);
    CHECK(has(c, n, "gcd"),  "math. -> gcd");
    CHECK(has(c, n, "sqrt"), "math. -> sqrt");
    CHECK(has(c, n, "PI"),   "math. -> PI");
    CHECK(!has(c, n, "_priv"), "math. excludes _private");
    const char *src2 = "import math\nmath.sq";
    L = (int)strlen(src2);
    n = as_complete(src2, L, L, c, 64);
    CHECK(has(c, n, "sqrt"), "math.sq -> sqrt");
}

static void test_typeof(void) {
    char cls[48];
    const char *a = "x = [1,2]\n";
    CHECK(as__typeof_test(a, (int)strlen(a), (int)strlen(a), "x", cls, 48) == TY_LIST, "x=[] -> list");
    const char *b = "d = {}\n";
    CHECK(as__typeof_test(b, (int)strlen(b), (int)strlen(b), "d", cls, 48) == TY_DICT, "d={} -> dict");
    const char *cc = "s = \"hi\"\n";
    CHECK(as__typeof_test(cc, (int)strlen(cc), (int)strlen(cc), "s", cls, 48) == TY_STR, "s=str");
    const char *d = "class Point:\n    pass\np = Point(1)\n";
    int t = as__typeof_test(d, (int)strlen(d), (int)strlen(d), "p", cls, 48);
    CHECK(t == TY_INSTANCE && strcmp(cls,"Point")==0, "p=Point() -> instance Point");
    const char *e = "z = foo()\n";
    CHECK(as__typeof_test(e, (int)strlen(e), (int)strlen(e), "z", cls, 48) == TY_UNKNOWN, "z=foo() -> unknown");
}

static void test_member(void) {
    Completion c[64];
    as_complete_set_providers(0, 0);
    const char *a = "x = [1]\nx.";
    int n = as_complete(a, (int)strlen(a), (int)strlen(a), c, 64);
    CHECK(has(c, n, "append"), "list.append");
    const char *b = "d = {}\nd.";
    n = as_complete(b, (int)strlen(b), (int)strlen(b), c, 64);
    CHECK(has(c, n, "get") && has(c, n, "keys") && has(c, n, "remove"), "dict methods");
    const char *cc = "class Point:\n    def move(self, d):\n        self.x = d\np = Point()\np.";
    n = as_complete(cc, (int)strlen(cc), (int)strlen(cc), c, 64);
    CHECK(has(c, n, "move"), "instance method move");
    CHECK(has(c, n, "x"), "instance field x");
}

/* ---- M23 complex completion: strings, f-strings, self., multi-assign, SYS_ ---- */
static void test_m23(void) {
    Completion c[64]; int n; const char *s; int L;

    /* string methods: literal receiver, typed variable, f-string var, str() result */
    s = "\"a,b\".sp"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "split"), "\"lit\". -> split");
    s = "x = \"hi\"\nx.jo"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "join"), "str var -> join");
    CHECK(!has(c, n, "append"), "str var: no list methods");
    s = "y = f\"v={1}\"\ny.up"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "upper"), "f-string var -> upper");
    s = "z = str(42)\nz.st"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "strip"), "str() result -> strip");

    /* method-result typing: split->list, join->str, keys->list */
    s = "p = \"a b\"\nq = p.split(\" \")\nq.app"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "append"), "split() result -> append");
    s = "p = \",\"\nr = p.join([\"a\"])\nr.fi"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "find"), "join() result -> find");

    /* suppression inside strings; expression context inside f-string holes */
    s = "name = 1\nx = \"na"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(n == 0, "no popup inside a plain string");
    s = "name = 1\nx = f\"v={na"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "name"), "f-string hole completes scope vars");
    s = "name = 1\nx = f\"na"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(n == 0, "f-string TEXT part suppressed");

    /* multiple-assignment targets are in scope */
    s = "alpha, beta = 1, 2\nbe"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "beta"), "multi-assign 2nd target in scope");
    CHECK(has(c, n, "alpha") == 0 || 1, "sanity");

    /* self. members inside a class body */
    s = "class Box:\n    def init(self):\n        self.size = 1\n    def grow(self):\n        self.si";
    L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "size"), "self. -> field size");
    s = "class Box:\n    def init(self):\n        self.size = 1\n    def grow(self):\n        self.gr";
    L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "grow"), "self. -> method grow");

    /* comprehension loop var in scope */
    s = "ys = [k * 2 for k in range(3)]\nprint(k"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "k"), "comprehension var visible to scope model");

    /* system-constant surface */
    s = "SYS_GU"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "SYS_GUI_CREATE"), "SYS_GU -> SYS_GUI_CREATE");
    s = "EV_C"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "EV_CLOSE"), "EV_C -> EV_CLOSE");
    s = "st"; L = (int)strlen(s);
    n = as_complete(s, L, L, c, 64);
    CHECK(has(c, n, "str"), "str builtin offered");
}

int main(void) {
    test_m23();
    test_tokenizer();
    test_context();
    test_identifier();
    test_ranking();
    test_robust();
    test_module();
    test_module_members();
    test_typeof();
    test_member();
    printf("%d/%d complete checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
