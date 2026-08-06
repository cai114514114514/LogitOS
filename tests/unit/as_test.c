/* AetherScript A1 host unit test.
 * Build: cc -o /tmp/as_test tools/t/as_test.c \
 *           src/apps/as/{value,as_io,lexer,compiler,vm,object}.c -Isrc/apps/as
 * Runs inline .as snippets and asserts their `print` output. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "as.h"

static int fails = 0, total = 0;

static void ok(const char *name, const char *src, const char *want)
{
    char buf[8192];
    as_capture(buf, sizeof buf);
    int r = as_interpret(src);
    as_capture(NULL, 0);
    total++;
    if (r != 0) { fails++; printf("FAIL %-10s interpret error: %s\n", name, as_err); as_free_objects(); return; }
    if (strcmp(buf, want) != 0) {
        fails++;
        printf("FAIL %-10s\n  want [%s]\n  got  [%s]\n", name, want, buf);
    }
    as_free_objects();
}

/* Expect a runtime error AND a particular call stack. The traceback is captured
 * by the library (as_traceback) and printed by the CLI, so this asserts the
 * capture without depending on as.c -- and pins the two properties that are easy
 * to get wrong: innermost frame first, and nothing left behind by an exception
 * the program caught. */
static void errtrace(const char *name, const char *src, const char *want_trace)
{
    char buf[1024];
    as_capture(buf, sizeof buf);
    int r = as_interpret(src);
    as_capture(NULL, 0);
    total++;
    /* Compare with the (+N) bytecode offsets stripped: which frames, in which
     * order, is the durable property. The offsets are for a human reading the
     * output next to `as -dis`, and pinning them here would make every codegen
     * change break these tests for no reason. */
    char got[512]; int gi = 0, skip = 0;
    for (const char *p = as_traceback(); *p && gi < (int)sizeof got - 1; p++) {
        if (p[0] == ' ' && p[1] == '(' && p[2] == '+') skip = 1;
        if (!skip) got[gi++] = *p;
        if (*p == ')') skip = 0;
    }
    got[gi] = 0;
    if (r == 0) { fails++; printf("FAIL %-10s expected an error, got none\n", name); }
    else if (strcmp(got, want_trace) != 0) {
        fails++;
        printf("FAIL %-10s traceback\n  want [%s]\n  got  [%s]\n", name, want_trace, got);
    }
    as_free_objects();
}

static void err(const char *name, const char *src)   /* expect a compile/runtime error */
{
    char buf[1024];
    as_capture(buf, sizeof buf);
    int r = as_interpret(src);
    as_capture(NULL, 0);
    total++;
    if (r == 0) { fails++; printf("FAIL %-10s expected an error, got none (out=[%s])\n", name, buf); }
    as_free_objects();
}

/* --- .la bytecode round-trip / fixpoint test ---
 * For a source: (A) compile + run, capturing output; (B) compile + as_dump to a
 * tmpfile, FREE all compile objects (proves the load is independent of them), read
 * the file back, as_load, stamp a fresh module, run, capturing output; assert
 * A == B. This exercises the whole serialize->deserialize->run path. */

/* Recursively stamp ->module on fn and every nested O_FN const (the loader/import
 * side does this; as_dump never serializes ->module). */
static void stamp_module(ObjFn *fn, ObjModule *m)
{
    fn->module = m;
    for (int i = 0; i < fn->kcount; i++)
        if (IS_FN(fn->consts[i])) stamp_module(AS_FN(fn->consts[i]), m);
}

static void roundtrip(const char *name, const char *src)
{
    char bufA[8192], bufB[8192];
    total++;

    /* (A) run the source directly */
    as_capture(bufA, sizeof bufA);
    int rA = as_interpret(src);
    as_capture(NULL, 0);
    if (rA != 0) { fails++; printf("FAIL %-12s (A) interpret error: %s\n", name, as_err); as_free_objects(); return; }
    as_free_objects();

    /* (B) compile -> dump -> [free] -> read back -> load -> run */
    ObjFn *fn = as_compile(src);
    if (!fn) { fails++; printf("FAIL %-12s compile error: %s\n", name, as_err); as_free_objects(); return; }
    FILE *t = tmpfile();
    if (!t) { fails++; printf("FAIL %-12s tmpfile() failed\n", name); as_free_objects(); return; }
    int dr = as_dump(fn, t);
    as_free_objects();                 /* drop all compile-side objects */
    if (dr != 0) { fails++; printf("FAIL %-12s as_dump error: %s\n", name, as_err); fclose(t); return; }

    fseek(t, 0, SEEK_END);
    long flen = ftell(t);
    rewind(t);
    if (flen < 8) { fails++; printf("FAIL %-12s dumped file too small (%ld)\n", name, flen); fclose(t); return; }
    uint8_t *blob = (uint8_t *)malloc((size_t)flen);
    size_t got = fread(blob, 1, (size_t)flen, t);
    fclose(t);
    if (got != (size_t)flen) { fails++; printf("FAIL %-12s short fread\n", name); free(blob); return; }

    if (memcmp(blob, "LAQ1", 4) != 0) { fails++; printf("FAIL %-12s missing LAQ1 magic\n", name); free(blob); return; }

    ObjFn *fn2 = as_load(blob, (int)flen);
    free(blob);
    if (!fn2) { fails++; printf("FAIL %-12s as_load returned NULL\n", name); as_free_objects(); return; }

    /* loader/import side stamps the module; mirror that here */
    as_gc_push_disable();
    ObjModule *m = as_module_new("__main__", 8);
    stamp_module(fn2, m);
    as_gc_pop_disable();

    as_capture(bufB, sizeof bufB);
    int rB = as_run(fn2);
    as_capture(NULL, 0);
    if (rB != 0) { fails++; printf("FAIL %-12s (B) run error: %s\n", name, as_err); as_free_objects(); return; }
    as_free_objects();

    if (strcmp(bufA, bufB) != 0) {
        fails++;
        printf("FAIL %-12s round-trip mismatch\n  src-run  [%s]\n  la-run   [%s]\n", name, bufA, bufB);
    }
}

static void bc_tests(void)
{
    roundtrip("bc_arith",   "print(1 + 2 * 3 - 4)\n");
    roundtrip("bc_floats",  "print(0.1 + 0.2)\nprint(2.5 * 4.0)\n");
    roundtrip("bc_strings", "print(\"a\" + \"b\" + \"c\", len(\"hello\"))\n");
    roundtrip("bc_list",    "xs = [10, 20, 30]\nxs.append(40)\nprint(xs, xs[2], len(xs))\n");
    roundtrip("bc_dict",    "d = {\"a\": 1, \"b\": 2}\nd[\"c\"] = 3\nprint(d[\"a\"], d[\"c\"], len(d))\n");
    roundtrip("bc_fib",
        "def fib(n):\n"
        "    if n < 2:\n"
        "        return n\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "print(fib(20))\n");
    roundtrip("bc_nestdef",            /* nested def -> FN const recursion */
        "def outer():\n"
        "    def inner(x):\n"
        "        return x * x\n"
        "    return inner(7)\n"
        "print(outer())\n");
    roundtrip("bc_closure",            /* closure: upvalue pairs ride in code stream */
        "def counter():\n"
        "    c = 0\n"
        "    def inc():\n"
        "        c = c + 1\n"
        "        return c\n"
        "    return inc\n"
        "f = counter()\n"
        "print(f(), f(), f())\n");
    roundtrip("bc_lambda",  "f = lambda x: x * 2\nprint(f(5), (lambda y: y + 1)(9))\n");
    roundtrip("bc_forloop",
        "total = 0\n"
        "for i in range(5):\n"
        "    total = total + i\n"
        "print(total)\n");
    roundtrip("bc_class",
        "class Box:\n"
        "    def init(self, v):\n"
        "        self.v = v\n"
        "    def get(self):\n"
        "        return self.v\n"
        "b = Box(42)\n"
        "print(b.get())\n");
    roundtrip("bc_mixed",
        "def squares(n):\n"
        "    out = []\n"
        "    for i in range(n):\n"
        "        out.append(i * i)\n"
        "    return out\n"
        "print(squares(5))\n");

    {   /* >255-const function round-trips through dump/load under AS_BC_VERSION 2
         * (proves the wider 16-bit operand survives the .la format). */
        char s[12000]; int p = 0;
        for (int i = 0; i < 300; i++) p += snprintf(s + p, sizeof s - p, "x%d = %d\n", i, i);
        snprintf(s + p, sizeof s - p, "print(x299)\n");
        roundtrip("bc_wide_const", s);
    }

    /* negatives: as_load must reject a bad magic and a bumped version */
    total++;
    {
        ObjFn *fn = as_compile("print(1)\n");
        FILE *t = tmpfile();
        as_dump(fn, t);
        as_free_objects();
        fseek(t, 0, SEEK_END);
        long flen = ftell(t);
        rewind(t);
        uint8_t *blob = (uint8_t *)malloc((size_t)flen);
        size_t got = fread(blob, 1, (size_t)flen, t);
        fclose(t);
        int bad = 0;
        if (got != (size_t)flen) bad = 1;
        if (!bad) {
            /* truncated buffer -> NULL */
            if (as_load(blob, 4) != NULL) bad = 1;
            /* corrupt magic -> NULL */
            uint8_t save = blob[0]; blob[0] = 'X';
            if (as_load(blob, (int)flen) != NULL) bad = 1;
            blob[0] = save;
            /* tamper the version byte -> NULL */
            uint8_t sv = blob[4]; blob[4] = (uint8_t)(blob[4] + 1);
            if (as_load(blob, (int)flen) != NULL) bad = 1;
            blob[4] = sv;
            /* sanity: the untouched blob still loads */
            ObjFn *good = as_load(blob, (int)flen);
            if (!good) bad = 1;
        }
        free(blob);
        as_free_objects();
        if (bad) { fails++; printf("FAIL %-12s negative-case check\n", "bc_reject"); }
    }
}

/* an in-memory module for the import tests. `quad` calls its module-mate `square`
 * -> proves a module function resolves globals in its own namespace. */
static const char *MATHX =
    "PI = 3\n"
    "def square(x):\n"
    "    return x * x\n"
    "def quad(x):\n"
    "    return square(square(x))\n";

/* Read a repo file whole (the harness runs from the repo root). Never freed:
 * as_add_module_source keeps the pointer for the whole run. */
static char *slurp_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FATAL: cannot open %s (run from the repo root)\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { printf("FATAL: cannot read %s\n", path); exit(1); }
    buf[n] = 0;
    fclose(f);
    return buf;
}

int main(void)
{
    as_add_module_source("mathx", MATHX);
    as_add_module_source("badmod", "x = 1 / 0\n");   /* item D: raises on import */
    as_add_module_source("goodmod", "v = 42\n");
    /* The REAL generated abi.as, not a copy of it: the wait driver in there is
     * the thing under test, and test-as-os does not reach it (sysdemo uses files
     * and processes, never the network). On the host as_ll_syscall stubs to -1,
     * so get_time() writes nothing and now_s() is a constant 0 -- which makes the
     * timeout path deterministic instead of a test that has to sleep. */
    as_add_module_source("abi", slurp_file("fsroot/as/lib/abi.as"));

    /* arithmetic + precedence */
    ok("prec",   "print(1 + 2 * 3)\n", "7\n");
    ok("paren",  "print((1 + 2) * 3)\n", "9\n");
    ok("lassoc", "print(10 - 3 - 2)\n", "5\n");
    ok("unary",  "print(-5 + 2)\n", "-3\n");
    ok("intdiv", "print(7 / 2)\n", "3\n");
    ok("fdiv",   "print(7.0 / 2)\n", "3.5\n");
    ok("mod",    "print(17 % 5)\n", "2\n");
    ok("mixed",  "print(2 + 3.5)\n", "5.5\n");

    /* float formatting: shortest round-trip + ".0" for whole floats (Python/JS-like) */
    ok("fwhole", "print(2.5 * 4.0)\n", "10.0\n");                 /* not "10" (looks like int) */
    ok("fzero",  "print(0.0)\n", "0.0\n");
    ok("fprec",  "print(0.1 + 0.2)\n", "0.30000000000000004\n"); /* not %g's "0.3" */
    ok("fthird", "print(1.0 / 3.0)\n", "0.3333333333333333\n");  /* not "0.333333" */
    ok("fpi",    "print(3.141592653589793)\n", "3.141592653589793\n");
    ok("fbig",   "print(1000000000000.0)\n", "1000000000000.0\n");
    ok("fsci",   "print(1.0e-8)\n", "1e-08\n");                   /* scientific only for extremes */

    /* comparisons, booleans, logic */
    ok("cmp",    "print(3 < 5)\n", "true\n");
    ok("eqmix",  "print(3 == 3.0)\n", "true\n");
    ok("ne",     "print(2 != 3)\n", "true\n");
    ok("and",    "print(1 and 2)\n", "2\n");
    ok("or",     "print(0 or 7)\n", "7\n");
    ok("notop",  "print(not 0)\n", "true\n");
    ok("shortc", "print(false and (1/0))\n", "false\n");   /* RHS must not evaluate */

    /* globals */
    ok("global", "x = 10\ny = x * 2\nprint(y)\n", "20\n");

    /* multi-arg print + literals */
    ok("multi",  "print(1, 2, 3)\n", "1 2 3\n");
    ok("string", "print(\"hello\")\n", "hello\n");
    ok("nilret", "def f():\n    return\nprint(f())\n", "nil\n");

    /* if / elif / else */
    ok("ifelse",
       "x = 5\n"
       "if x < 0:\n"
       "    print(\"neg\")\n"
       "elif x == 0:\n"
       "    print(\"zero\")\n"
       "else:\n"
       "    print(\"pos\")\n",
       "pos\n");

    /* while loop accumulator */
    ok("while",
       "def sum(n):\n"
       "    s = 0\n"
       "    i = 1\n"
       "    while i <= n:\n"
       "        s = s + i\n"
       "        i = i + 1\n"
       "    return s\n"
       "print(sum(100))\n",
       "5050\n");

    /* functions: value used in an expression, nested calls */
    ok("sqsum",
       "def sq(x):\n"
       "    return x * x\n"
       "print(sq(3) + sq(4))\n",
       "25\n");

    /* recursion: the A1 milestone target */
    ok("fib",
       "def fib(n):\n"
       "    if n < 2:\n"
       "        return n\n"
       "    return fib(n - 1) + fib(n - 2)\n"
       "print(fib(20))\n",
       "6765\n");

    /* comments + blank lines are ignored */
    ok("comment",
       "# a comment\n"
       "\n"
       "x = 3   # trailing comment\n"
       "print(x)\n",
       "3\n");

    /* ---- A2: strings, lists, for/range, builtins ---- */
    ok("listlit", "print([1, 2, 3])\n", "[1, 2, 3]\n");
    ok("listget", "xs = [10, 20, 30]\nprint(xs[1])\n", "20\n");
    ok("negidx",  "print([1, 2, 3][-1])\n", "3\n");
    ok("listset", "xs = [1, 2, 3]\nxs[0] = 99\nprint(xs)\n", "[99, 2, 3]\n");
    ok("append",  "xs = []\nxs.append(5)\nxs.append(6)\nprint(len(xs), xs)\n", "2 [5, 6]\n");
    ok("range1",  "print(range(3))\n", "[0, 1, 2]\n");
    ok("range2",  "print(range(2, 5))\n", "[2, 3, 4]\n");
    ok("range3",  "print(range(0, 10, 3))\n", "[0, 3, 6, 9]\n");
    ok("strcat",  "print(\"a\" + \"b\" + \"c\")\n", "abc\n");
    ok("stridx",  "print(\"hello\"[1])\n", "e\n");
    ok("strlen",  "print(len(\"hello\"))\n", "5\n");
    ok("reprlist","print([1, \"a\", true])\n", "[1, 'a', true]\n");

    ok("forrange",
       "total = 0\n"
       "for i in range(5):\n"
       "    total = total + i\n"
       "print(total)\n",
       "10\n");

    ok("forlist",
       "def f(xs):\n"
       "    s = 0\n"
       "    for x in xs:\n"
       "        s = s + x\n"
       "    return s\n"
       "print(f([1, 2, 3, 4]))\n",
       "10\n");

    ok("squares",
       "def squares(n):\n"
       "    out = []\n"
       "    for i in range(n):\n"
       "        out.append(i * i)\n"
       "    return out\n"
       "print(squares(5))\n",
       "[0, 1, 4, 9, 16]\n");

    ok("nestfor",
       "xs = []\n"
       "for i in range(3):\n"
       "    for j in range(3):\n"
       "        xs.append(i * 10 + j)\n"
       "print(xs)\n",
       "[0, 1, 2, 10, 11, 12, 20, 21, 22]\n");

    /* ---- A3: indirection (raw memory, addr, typed pointers) ---- */
    ok("peekpoke",
       "s = \"ABCD\"\n"
       "a = addr(s)\n"
       "print(peek8(a))\n"        /* 'A' */
       "poke8(a, 90)\n"          /* -> 'Z' */
       "print(peek8(a), s)\n",
       "65\n90 ZBCD\n");
    ok("i8ptr",
       "s = \"AAAA\"\n"
       "p = i8ptr(addr(s))\n"
       "p[0] = 66\n"             /* 'B' */
       "p[3] = 68\n"             /* 'D' */
       "print(p[0], s)\n",
       "66 BAAD\n");
    ok("i32ptr",
       "s = \"\\0\\0\\0\\0\\0\\0\\0\\0\"\n"
       "p = i32ptr(addr(s))\n"
       "p[0] = 305419896\n"      /* 0x12345678 */
       "p[1] = -1\n"             /* reads back as signed i32 -1 */
       "print(p[0], p[1])\n",
       "305419896 -1\n");
    ok("sysconst", "print(SYS_WRITE)\n", "1\n");
    ok("syscall_stub", "print(syscall(SYS_GETPID))\n", "-1\n");   /* host stub returns -1 */

    /* ---- modules: import / from-import / attribute access ---- */
    ok("import",
       "import mathx\n"
       "print(mathx.PI, mathx.square(5), mathx.quad(2))\n",   /* quad uses module-mate square */
       "3 25 16\n");
    ok("fromimport",
       "from mathx import square, PI\n"
       "print(square(7), PI)\n",
       "49 3\n");
    ok("modnamespace",
       "import mathx\n"
       "square = 99\n"                 /* local 'square' must not clash with mathx.square */
       "print(square, mathx.square(3))\n",
       "99 9\n");
    err("badimport", "import does_not_exist\n");
    err("badattr",   "import mathx\nprint(mathx.nope)\n");
    /* item D: a failed import drops its partial module entry, so it does not
       poison the table -> a subsequent good import still succeeds. */
    err("import_fails", "import badmod\n");
    ok("import_after_fail", "import goodmod\nprint(goodmod.v)\n", "42\n");
    /* same run: catch a failed import, then a different good import works. */
    ok("import_recover_same_run",
       "try:\n    import badmod\nexcept e:\n    print(\"caught\")\nimport goodmod\nprint(goodmod.v)\n",
       "caught\n42\n");
    /* same-name retry (the precise nmodules-- assertion): after the first
       import raises, the partial module must be dropped so a re-import of the
       SAME name re-attempts (and re-raises) instead of returning the dud. */
    ok("import_same_name_retry",
       "try:\n    import badmod\nexcept e:\n    print(\"first\")\n"
       "try:\n    import badmod\nexcept e:\n    print(\"second\")\n",
       "first\nsecond\n");

    /* ---- M21: dict ---- */
    ok("dictget",  "d = {\"a\": 1, \"b\": 2}\nprint(d[\"a\"], d[\"b\"])\n", "1 2\n");
    ok("dictlit1", "print({\"x\": 42})\n", "{'x': 42}\n");     /* single entry -> deterministic order */
    ok("dictempty","print(len({}))\n", "0\n");
    ok("dictlen",  "print(len({\"a\": 1, \"b\": 2, \"c\": 3}))\n", "3\n");
    ok("dictint",  "d = {1: \"one\", 2: \"two\"}\nprint(d[1], d[2])\n", "one two\n");
    ok("dictintlit","print({1: \"x\"})\n", "{1: 'x'}\n");       /* int-key print path */
    ok("dictnest", "d = {\"xs\": [1, 2]}\nprint(d[\"xs\"][1])\n", "2\n");
    err("dictmiss","d = {\"a\": 1}\nprint(d[\"z\"])\n");
    err("dictbadkey", "d = {}\nprint(d[true])\n");
    ok("dictset",   "d = {}\nd[\"a\"] = 1\nd[\"b\"] = 2\nprint(d[\"a\"], d[\"b\"], len(d))\n", "1 2 2\n");
    ok("dictover",  "d = {\"a\": 1}\nd[\"a\"] = 9\nprint(d[\"a\"], len(d))\n", "9 1\n");
    ok("dictintset","d = {}\nd[7] = 70\nprint(d[7])\n", "70\n");
    ok("dictgrow",  "d = {}\ni = 0\nwhile i < 50:\n    d[i] = i * i\n    i = i + 1\nprint(len(d), d[7], d[49])\n", "50 49 2401\n");
    ok("dicthas",  "d = {\"a\": 1}\nprint(d.has(\"a\"), d.has(\"z\"))\n", "true false\n");
    ok("dictgetm", "d = {\"a\": 1}\nprint(d.get(\"a\"), d.get(\"z\"))\n", "1 nil\n");
    ok("dictgetd", "d = {\"a\": 1}\nprint(d.get(\"z\", 99))\n", "99\n");
    ok("dictrm",   "d = {\"a\": 1, \"b\": 2}\nprint(d.remove(\"a\"), d.has(\"a\"), len(d))\n", "true false 1\n");
    ok("dictkeys", "d = {\"a\": 1}\nprint(d.keys())\n", "['a']\n");
    ok("dictvals", "d = {\"a\": 5}\nprint(d.values())\n", "[5]\n");
    err("dictnomethod", "d = {}\nd.frobnicate()\n");

    ok("indict", "d = {\"a\": 1}\nprint(\"a\" in d, \"z\" in d)\n", "true false\n");
    ok("inlist", "print(2 in [1, 2, 3], 9 in [1, 2, 3])\n", "true false\n");
    ok("instr",  "print(\"ell\" in \"hello\", \"xyz\" in \"hello\")\n", "true false\n");
    ok("inif",   "d = {\"k\": 1}\nif \"k\" in d:\n    print(\"yes\")\n", "yes\n");
    ok("inprec", "print(1 + 1 in [2, 3])\n", "true\n");
    ok("fordict",
       "d = {\"a\": 1, \"b\": 2, \"c\": 3}\n"
       "s = 0\n"
       "for k in d:\n"
       "    s = s + d[k]\n"
       "print(s)\n",
       "6\n");
    ok("fordictcount",
       "d = {1: 10, 2: 20}\n"
       "n = 0\n"
       "for k in d:\n"
       "    n = n + 1\n"
       "print(n)\n",
       "2\n");
    ok("dicttomb",  "d = {\"a\": 1}\nd.remove(\"a\")\nd[\"a\"] = 2\nprint(d[\"a\"], len(d))\n", "2 1\n");   /* tombstone reuse */
    ok("dictkeyty", "d = {1: \"i\"}\nd[\"1\"] = \"s\"\nprint(len(d), d[1], d[\"1\"])\n", "2 i s\n");        /* int 1 != str "1" */

    /* ---- M22.1: closures ---- */
    ok("clo_counter",
       "def counter():\n"
       "    c = 0\n"
       "    def inc():\n"
       "        c = c + 1\n"
       "        return c\n"
       "    return inc\n"
       "f = counter()\n"
       "print(f(), f(), f())\n",
       "1 2 3\n");
    ok("clo_adder",
       "def adder(n):\n"
       "    def add(x):\n"
       "        return x + n\n"
       "    return add\n"
       "a = adder(10)\n"
       "print(a(5), a(100))\n",
       "15 110\n");
    ok("clo_shared",
       "def make():\n"
       "    c = 0\n"
       "    def get():\n"
       "        return c\n"
       "    def inc():\n"
       "        c = c + 1\n"
       "        return c\n"
       "    return [get, inc]\n"
       "p = make()\n"
       "g = p[0]\n"
       "i = p[1]\n"
       "i()\n"
       "i()\n"
       "print(g())\n",
       "2\n");
    ok("clo_multi",
       "def a():\n"
       "    x = 7\n"
       "    def b():\n"
       "        def c():\n"
       "            return x\n"
       "        return c\n"
       "    return b\n"
       "print(a()()())\n",
       "7\n");
    ok("lambda_basic",  "f = lambda x: x * 2\nprint(f(5))\n", "10\n");
    ok("lambda_noargs", "f = lambda: 42\nprint(f())\n", "42\n");
    ok("lambda_inline", "print((lambda x: x * x)(6))\n", "36\n");
    ok("lambda_capture",
       "def adder(n):\n"
       "    return lambda x: x + n\n"
       "a = adder(3)\n"
       "print(a(4))\n",
       "7\n");
    ok("lambda_hof",
       "def apply(fn, v):\n"
       "    return fn(v)\n"
       "print(apply(lambda x: x + 100, 5))\n",
       "105\n");
    ok("clo_recur",
       "def outer():\n"
       "    def fact(n):\n"
       "        if n <= 1:\n"
       "            return 1\n"
       "        return n * fact(n - 1)\n"
       "    return fact\n"
       "f = outer()\n"
       "print(f(5))\n",
       "120\n");
    ok("clo_iter",
       "def make():\n"
       "    fns = []\n"
       "    i = 0\n"
       "    while i < 3:\n"
       "        x = i\n"
       "        def get():\n"
       "            return x\n"
       "        fns.append(get)\n"
       "        i = i + 1\n"
       "    return fns\n"
       "g = make()\n"
       "print(g[0](), g[1](), g[2]())\n",
       "0 1 2\n");

    /* errors are reported, not crashed */
    err("undef",   "print(nope)\n");
    err("badindent", "x = 1\n  y = 2\n");
    err("arity",   "def g(a):\n    return a\nprint(g(1, 2))\n");
    err("oob",     "print([1, 2][5])\n");
    err("nomethod","xs = []\nxs.frobnicate()\n");

    /* ---- M22.2: GC ---- */
    ok("gc_stats_live", "x = [1, 2, 3]\nprint(gc_stats() > 0)\n", "true\n");
    ok("gc_frees",
       "def garbage():\n"
       "    i = 0\n"
       "    while i < 500:\n"
       "        x = [i, i, i]\n"
       "        i = i + 1\n"
       "    return 0\n"
       "garbage()\n"
       "n1 = gc_stats()\n"
       "freed = gc()\n"
       "n2 = gc_stats()\n"
       "print(freed > 0, n2 < n1)\n",
       "true true\n");
    ok("gc_keeps_reachable",
       "keep = [1, 2, 3]\n"
       "def mk():\n"
       "    c = 0\n"
       "    def inc():\n"
       "        c = c + 1\n"
       "        return c\n"
       "    return inc\n"
       "f = mk()\n"
       "f()\n"
       "gc()\n"
       "print(keep, f())\n",
       "[1, 2, 3] 2\n");
    ok("gc_dict_survives",
       "d = {\"a\": [1, 2], \"b\": 3}\n"
       "gc()\n"
       "print(d[\"a\"][1], d[\"b\"])\n",
       "2 3\n");
    ok("gc_auto_bounded",
       "def churn():\n"
       "    i = 0\n"
       "    while i < 20000:\n"
       "        x = [i]\n"
       "        i = i + 1\n"
       "    return gc_stats()\n"
       "print(churn() < 10000)\n",
       "true\n");

    /* ---- M22.3: classes ---- */
    ok("class_decl",
       "class Point:\n"
       "    def dist(self):\n"
       "        return 0\n"
       "print(Point)\n",
       "<class Point>\n");
    ok("class_init",
       "class Box:\n"
       "    def init(self, v):\n"
       "        self.v = v\n"
       "b = Box(42)\n"
       "print(b.v)\n",
       "42\n");
    ok("class_method",
       "class Counter:\n"
       "    def init(self):\n"
       "        self.n = 0\n"
       "    def bump(self):\n"
       "        self.n = self.n + 1\n"
       "        return self.n\n"
       "c = Counter()\n"
       "print(c.bump(), c.bump(), c.bump())\n",
       "1 2 3\n");
    ok("class_field_set",
       "class P:\n"
       "    def init(self):\n"
       "        self.x = 1\n"
       "p = P()\n"
       "p.x = 99\n"
       "p.y = 7\n"
       "print(p.x, p.y)\n",
       "99 7\n");
    ok("class_bound",
       "class Greeter:\n"
       "    def init(self, who):\n"
       "        self.who = who\n"
       "    def hi(self):\n"
       "        return \"hi \" + self.who\n"
       "g = Greeter(\"sam\")\n"
       "f = g.hi\n"
       "print(f())\n",
       "hi sam\n");
    ok("class_noinit",
       "class Empty:\n"
       "    def tag(self):\n"
       "        return \"e\"\n"
       "e = Empty()\n"
       "print(e.tag())\n",
       "e\n");
    err("class_init_arity", "class C:\n    def init(self, a):\n        self.a = a\nc = C()\n");
    err("class_no_field",   "class C:\n    def m(self):\n        return 1\nc = C()\nprint(c.missing)\n");
    err("class_set_nonobj", "x = 5\nx.f = 1\n");

    /* ---- M22.3: inheritance + super ---- */
    ok("class_inherit",
       "class Animal:\n"
       "    def init(self, name):\n"
       "        self.name = name\n"
       "    def speak(self):\n"
       "        return self.name + \" makes a sound\"\n"
       "class Dog(Animal):\n"
       "    def speak(self):\n"
       "        return self.name + \" barks\"\n"
       "d = Dog(\"Rex\")\n"
       "print(d.speak(), d.name)\n",
       "Rex barks Rex\n");
    ok("class_inherit_method",
       "class Animal:\n"
       "    def init(self, n):\n"
       "        self.n = n\n"
       "    def kind(self):\n"
       "        return \"animal\"\n"
       "class Cat(Animal):\n"
       "    def meow(self):\n"
       "        return self.n + \" meows\"\n"
       "c = Cat(\"Tom\")\n"
       "print(c.kind(), c.meow())\n",
       "animal Tom meows\n");
    ok("class_super",
       "class A:\n"
       "    def greet(self):\n"
       "        return \"A\"\n"
       "class B(A):\n"
       "    def greet(self):\n"
       "        return super.greet() + \"B\"\n"
       "print(B().greet())\n",
       "AB\n");
    /* super must resolve the enclosing class even when a method parameter shadows the
     * class name (regression: named_variable would pick the local param, not the class). */
    ok("class_super_shadow",
       "class A:\n"
       "    def f(self):\n"
       "        return \"A\"\n"
       "class B(A):\n"
       "    def f(self, B):\n"
       "        return super.f() + B\n"
       "print(B().f(\"x\"))\n",
       "Ax\n");
    /* super inside a nested (local) class -> the class is reached as an upvalue. */
    ok("class_super_nested",
       "def make():\n"
       "    class P:\n"
       "        def who(self):\n"
       "            return \"P\"\n"
       "    class C(P):\n"
       "        def who(self):\n"
       "            return super.who() + \"C\"\n"
       "    return C\n"
       "k = make()\n"
       "print(k().who())\n",
       "PC\n");
    err("super_no_base", "class C:\n    def m(self):\n        return super.m()\nC().m()\n");

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

    /* ---- robustness Item A: value-stack overflow is a CATCHABLE error, not a crash ---- */
    /* Simple unbounded recursion: hits the call-depth cap (FRAMES_MAX) before the
     * value stack -- still a clean, catchable runtime error rather than a crash. */
    err("stack_overflow_recurse", "def f(n):\n    return f(n + 1)\nf(0)\n");
    ok("overflow_caught",
       "def f(n):\n    return f(n + 1)\ntry:\n    f(0)\nexcept e:\n    print(\"caught\")\n",
       "caught\n");
    /* Recursion with many live locals per frame overruns the VALUE stack
     * (STACK_MAX) BEFORE the frame cap -> exercises checked_push directly. Before
     * the guard this overran stack[] into adjacent statics (memory corruption). */
    {
        char s[16000]; int p = 0;
        p += snprintf(s + p, sizeof s - p, "def f(n):\n");
        for (int i = 0; i < 40; i++) p += snprintf(s + p, sizeof s - p, "    a%d = n\n", i);
        p += snprintf(s + p, sizeof s - p, "    return f(n + 1)\n");
        snprintf(s + p, sizeof s - p, "f(0)\n");
        err("value_stack_overflow", s);
    }
    {
        char s[16000]; int p = 0;
        p += snprintf(s + p, sizeof s - p, "def f(n):\n");
        for (int i = 0; i < 40; i++) p += snprintf(s + p, sizeof s - p, "    a%d = n\n", i);
        p += snprintf(s + p, sizeof s - p, "    return f(n + 1)\n");
        snprintf(s + p, sizeof s - p, "try:\n    f(0)\nexcept e:\n    print(\"caught\")\n");
        ok("value_stack_overflow_caught", s, "caught\n");
    }

    /* ---- robustness B: wide (16-bit) constant index ---- */
    {   /* >255 distinct constants in one (top-level) function now COMPILES + runs;
         * was previously "too many constants in one function". */
        char s[12000]; int p = 0;
        for (int i = 0; i < 300; i++) p += snprintf(s + p, sizeof s - p, "x%d = %d\n", i, i);
        snprintf(s + p, sizeof s - p, "print(x299)\n");
        ok("wide_const_300", s, "299\n");
    }

    /* ---- M23: bitwise / shift / power operators ---- */
    ok("bit_and",   "print(0xFF & 0x0F)\n", "15\n");
    ok("bit_or",    "print(5 | 2)\n", "7\n");
    ok("bit_xor",   "print(6 ^ 3)\n", "5\n");
    ok("bit_not",   "print(~0, ~5)\n", "-1 -6\n");
    ok("shl",       "print(1 << 8)\n", "256\n");
    ok("shr",       "print(1024 >> 3)\n", "128\n");
    ok("shr_signed","print(-8 >> 1)\n", "-4\n");
    ok("pow_int",   "print(2 ** 10)\n", "1024\n");
    ok("pow_rassoc","print(2 ** 2 ** 3)\n", "256\n");        /* right-assoc: 2**(2**3)=2**8 */
    ok("bit_prec",  "print(1 | 2 & 3)\n", "3\n");            /* & binds tighter than | -> 1|(2&3)=1|2=3 */
    ok("shift_prec","print(1 + 1 << 4)\n", "32\n");          /* +/- bind tighter than << -> (2)<<4 */
    ok("pow_prec",  "print(-2 ** 2)\n", "-4\n");             /* ** tighter than unary - -> -(2**2) */
    ok("mask_idiom","x = 0xABCD\nprint(x & 0xFF, (x >> 8) & 0xFF)\n", "205 171\n");
    err("band_float", "print(1.5 & 2)\n");                   /* bitwise needs ints */
    err("hex_empty",  "x = 0x\n");                           /* 0x with no digits -> lex error */
    roundtrip("bc_bitwise", "print(5 & 3, 5 | 2, 6 ^ 3, ~0, 1 << 4, 256 >> 2, 2 ** 8)\n");

    /* ---- M23: break / continue ---- */
    ok("for_break",   "s = 0\nfor i in range(10):\n    if i == 5:\n        break\n    s = s + i\nprint(s)\n", "10\n");
    ok("for_continue","s = 0\nfor i in range(6):\n    if i % 2 == 1:\n        continue\n    s = s + i\nprint(s)\n", "6\n");
    ok("while_break", "i = 0\nwhile true:\n    i = i + 1\n    if i >= 3:\n        break\nprint(i)\n", "3\n");
    ok("nested_break","c = 0\nfor i in range(3):\n    for j in range(3):\n        if j == 1:\n            break\n        c = c + 1\nprint(c)\n", "3\n");
    ok("cont_local",  "def f():\n    s = 0\n    for i in range(5):\n        x = i * 2\n        if i == 2:\n            continue\n        s = s + x\n    return s\nprint(f())\n", "16\n");
    ok("break_local", "def f():\n    s = 0\n    for i in range(9):\n        y = i\n        if i == 4:\n            break\n        s = s + y\n    return s\nprint(f())\n", "6\n");
    err("break_outside", "break\n");
    err("cont_outside",  "x = 1\ncontinue\n");

    /* ---- M23: compound assignment (name targets) + ';' separator ---- */
    ok("compound",    "x = 10\nx += 5\nx *= 2\nx -= 3\nx %= 7\nprint(x)\n", "6\n");   /* ((10+5)*2-3)%7 = 27%7 = 6 */
    ok("compound_loc","def f():\n    n = 1\n    for i in range(5):\n        n *= 2\n    return n\nprint(f())\n", "32\n");
    ok("semicolon",   "a = 1; b = 2; print(a + b)\n", "3\n");
    roundtrip("bc_loopctl", "s = 0\nfor i in range(8):\n    if i == 3:\n        continue\n    if i == 6:\n        break\n    s += i\nprint(s)\n");

    /* ---- M23 Stage A: string methods + str() + lazy hash ---- */
    ok("join",       "print(\",\".join([\"a\",\"b\",\"c\"]))\n", "a,b,c\n");
    ok("join_empty", "print(\"-\".join([]))\nprint(\"-\".join([\"x\"]))\n", "\nx\n");
    err("join_badarg",  "print(\",\".join(42))\n");
    err("join_baditem", "print(\",\".join([\"a\", 1]))\n");
    ok("split",      "print(\"a,,b\".split(\",\"))\n", "['a', '', 'b']\n");
    ok("split_trail","print(\"a,b,\".split(\",\"))\n", "['a', 'b', '']\n");
    ok("split_ws",   "print(\" a  b\\tc \".split())\n", "['a', 'b', 'c']\n");
    err("split_empty_sep", "print(\"ab\".split(\"\"))\n");
    /* split, class construction and args() no longer switch the collector off for
     * their duration -- they root the object under construction and let it run.
     * Under -DAS_GC_STRESS every one of the allocations below collects first, so a
     * missing root is a use-after-free here rather than a rare production crash. */
    ok("split_gc",   "s = \"\"\nfor i in range(200):\n    s = s + \"ab \"\n"
                     "p = s.split(\" \")\nprint(len(p), p[0], p[199])\n", "201 ab ab\n");
    ok("class_gc",   "class P:\n    def init(self, a):\n        self.a = a\n"
                     "t = 0\nfor i in range(200):\n    t = t + P(i).a\nprint(t)\n", "19900\n");
    ok("strip",      "print(\"  hi  \".strip())\nprint(len(\"\\t\\n\".strip()))\n", "hi\n0\n");
    ok("case",       "print(\"MiXed3\".upper())\nprint(\"MiXed3\".lower())\n", "MIXED3\nmixed3\n");
    ok("replace",    "print(\"aXaXa\".replace(\"X\",\"--\"))\nprint(\"aaa\".replace(\"a\",\"\"))\n", "a--a--a\n\n");
    err("replace_empty_old", "print(\"x\".replace(\"\",\"y\"))\n");
    ok("find",       "print(\"hello\".find(\"ll\"))\nprint(\"x\".find(\"z\"))\nprint(\"ab\".find(\"\"))\n", "2\n-1\n0\n");
    err("str_nomethod", "print(\"x\".nope())\n");
    /* join is O(total): build 2000 pieces and join -- must be instant + exact */
    ok("join_perf",  "p = []\nfor i in range(2000):\n    p.append(\"ab\")\ns = \",\".join(p)\nprint(len(s))\nprint(s[2])\n", "5999\n,\n");
    /* str() of every type (matches print's formatting) */
    ok("str_of",     "print(str(42) + \"|\" + str(3.5) + \"|\" + str(true) + \"|\" + str(nil) + \"|\" + str([1, 2]) + \"|\" + str(\"hi\"))\n",
                     "42|3.5|true|nil|[1, 2]|hi\n");
    /* lazy hash: a concat-BUILT key must behave identically to a literal key
     * (dict set/get + module var via import) */
    ok("lazy_hash",  "d = {}\nk = \"ab\" + \"cd\"\nd[k] = 7\nprint(d[\"abcd\"])\nimport goodmod\nprint(goodmod.v)\n", "7\n42\n");

    /* ---- M23 Stage B: f-strings ---- */
    ok("fstr",       "print(f\"x={1+2}\")\n", "x=3\n");
    ok("fstr_lit",   "print(f\"{{literal}}\")\n", "{literal}\n");
    ok("fstr_multi", "a = 7\nprint(f\"a={a} sq={a*a}!\")\n", "a=7 sq=49!\n");
    ok("fstr_str",   "print(f\"{'a'}{2}\")\n", "a2\n");
    ok("fstr_list",  "print(f\"v={[1,2]}\")\n", "v=[1, 2]\n");
    ok("fstr_dict",  "print(f\"{ {1: 2} }\")\n", "{1: 2}\n");
    ok("fstr_only",  "print(f\"{99}\")\nprint(len(f\"\"))\n", "99\n0\n");
    ok("fstr_nested","x = 3\nprint(f\"a{f'b{x}'}c\")\n", "ab3c\n");
    ok("fstr_call",  "def sq(n):\n    return n * n\nprint(f\"r={sq(4)}\")\n", "r=16\n");
    err("fstr_spec",  "x = 1\nprint(f\"{x:.2f}\")\n");
    err("fstr_empty", "print(f\"{}\")\n");
    err("fstr_unterm","print(f\"{1+2\")\n");

    /* ---- M23 Stage C: ternary + list comprehension ---- */
    ok("tern",       "print(1 if true else 2)\nprint(1 if false else 2)\n", "1\n2\n");
    ok("tern_expr",  "x = 5\ns = \"big\" if x > 3 else \"small\"\nprint(s)\n", "big\n");
    ok("tern_nest",  "def f(n):\n    return \"neg\" if n < 0 else (\"zero\" if n == 0 else \"pos\")\nprint(f(-1), f(0), f(2))\n", "neg zero pos\n");
    ok("tern_lazy_then", "d = {\"a\": 1}\nprint(d[\"x\"] if \"x\" in d else -1)\nprint(d[\"a\"] if \"a\" in d else -1)\n", "-1\n1\n");
    ok("tern_lazy",  "print(1 if true else 1 / 0)\nprint(2 if false else 9)\n", "1\n9\n");
    ok("tern_fstr",  "n = 4\nprint(f\"{'even' if n % 2 == 0 else 'odd'}\")\n", "even\n");
    err("tern_noelse","print(1 if true)\n");
    ok("comp",       "print([x * x for x in range(4)])\n", "[0, 1, 4, 9]\n");
    ok("comp_if",    "print([x for x in range(6) if x % 2 == 0])\n", "[0, 2, 4]\n");
    ok("comp_lit",   "print([s + \"!\" for s in [\"a\", \"b\"]])\n", "['a!', 'b!']\n");
    ok("comp_fn",    "def f():\n    return [i + 1 for i in range(3)]\nprint(f())\n", "[1, 2, 3]\n");
    ok("comp_tern",  "print([\"e\" if x % 2 == 0 else \"o\" for x in range(4)])\n", "['e', 'o', 'e', 'o']\n");
    ok("comp_noleak","x = 99\nprint(len([x * 2 for x in range(5)]))\nprint(x)\n", "5\n99\n");
    ok("comp_nested_src", "print([y for y in [10, 20]])\n", "[10, 20]\n");
    ok("comp_join",  "print(\",\".join([str(i) for i in range(4)]))\n", "0,1,2,3\n");
    err("comp_junk",  "print([x y for x in range(3)])\n");

    /* ---- M23 Stage D: multiple assignment / unpack ---- */
    ok("massign",    "a, b = 1, 2\nprint(a, b)\n", "1 2\n");
    ok("mswap",      "a, b = 1, 2\na, b = b, a\nprint(a, b)\n", "2 1\n");
    ok("munpack",    "a, b, c = [10, 20, 30]\nprint(a + b + c)\n", "60\n");
    ok("massign_fn", "def f():\n    x, y = 3, 4\n    x, y = y, x\n    return [x, y]\nprint(f())\n", "[4, 3]\n");
    ok("munpack_fn", "def f(l):\n    p, q = l\n    return p * q\nprint(f([6, 7]))\n", "42\n");
    ok("massign_mix","def f():\n    a = 1\n    a, b = 5, 6\n    return a + b\nprint(f())\n", "11\n");
    ok("massign_expr","a, b = 2 + 3, \"x\" + \"y\"\nprint(a, b)\n", "5 xy\n");
    err("mcount",     "a, b = 1, 2, 3\n");
    err("munpack_few","a, b, c = [1, 2]\n");

    /* ---- the generated wait driver -----------------------------------------
     * Logit has no blocking form for DNS or ping: the kernel starts, the caller
     * polls. Every call site used to write that loop with its own retry count,
     * and answer one value for "failed", "timed out" and "no interface" alike.
     * The protocol now lives in include/abi/logit_calls.abi and the loop is
     * generated once; these drive it with their own poll function, which is what
     * taking one as an argument buys.
     * Nothing else covers this: test-as-os never touches the network. */
    ok("await_value",   "from abi import await_\nc = [0]\ndef poll():\n    c[0] = c[0] + 1\n"
                        "    return 0 if c[0] < 3 else 42\n"
                        "print(await_(poll, false, 0, false, 0, 60, \"t\"), c[0])\n", "42 3\n");
    ok("await_pend_neg","from abi import await_\nc = [0]\ndef poll():\n    c[0] = c[0] + 1\n"
                        "    return -1 if c[0] < 3 else 9\n"
                        "print(await_(poll, true, 0, false, 0, 60, \"t\"))\n", "9\n");
    /* failure and timeout are DIFFERENT outcomes, which is the whole point */
    ok("await_fail",    "from abi import await_\ndef poll():\n    return 7\n"
                        "try:\n    await_(poll, false, 0, true, 7, 60, \"dns\")\nexcept e:\n    print(e)\n",
                        "dns: failed\n");
    ok("await_timeout", "from abi import await_\ndef poll():\n    return 0\n"
                        "try:\n    await_(poll, false, 0, true, 7, 0, \"dns\")\nexcept e:\n    print(e)\n",
                        "dns: timed out after 0s\n");
    /* a value that merely looks like the failure sentinel of ANOTHER operation
       is an ordinary result here */
    ok("await_no_fail", "from abi import await_\ndef poll():\n    return 7\n"
                        "print(await_(poll, false, 0, false, 0, 60, \"t\"))\n", "7\n");

    /* ---- tracebacks: a runtime error says WHERE, not just what ---------------
     * Every Frame already knew its function and its instruction pointer; the
     * error path simply never read them, so "undefined variable 'nope'" arrived
     * with no location at all. Captured at the point nothing is left to catch
     * the error -- which is where frames[] is still intact, and where we know it
     * is worth capturing. */
    errtrace("tb_depth",  "def inner(x):\n    return nope + x\ndef middle(x):\n    return inner(x)\n"
                          "def outer():\n    return middle(3)\nouter()\n",
                          "  in inner\n  in middle\n  in outer\n  in <script>\n");
    errtrace("tb_top",    "nope\n", "  in <script>\n");
    errtrace("tb_native", "def f():\n    return chr(999)\nf()\n", "  in f\n  in <script>\n");
    errtrace("tb_method", "class C:\n    def go(self):\n        return self.missing\n"
                          "def use(c):\n    return c.go()\nuse(C())\n",
                          "  in go\n  in use\n  in <script>\n");
    /* A caught exception must leave nothing behind: only the error that actually
     * escaped may be described. Here the first raise is handled and a different
     * failure escapes later -- the trace must be the second one's alone. */
    errtrace("tb_caught", "def a():\n    raise \"handled\"\ndef b():\n    return nope\n"
                          "try:\n    a()\nexcept e:\n    print(e)\nb()\n",
                          "  in b\n  in <script>\n");
    /* Deep recursion elides the repetitive middle rather than filling the buffer
     * with one frame repeated 250 times. */
    errtrace("tb_deep",   "def r(n):\n    return r(n + 1)\nr(0)\n",
                          "  in r\n  in r\n  in r\n  in r\n  in r\n  in r\n  in r\n  in r\n"
                          "  ... 245 more frames\n  in r\n  in r\n  in <script>\n");

    /* ---- layouts: a shape whose slots are machine words, not Values ----------
     * The point of the type is that addr(t) is exactly the pointer a syscall
     * receives, so these check the bytes, not just the accessors: writing a
     * field and reading it back through poke/peek at a raw address (and the
     * reverse) is what proves the two views are the same memory. The offsets
     * here are written by hand ON PURPOSE -- the generated abi.as gets its
     * numbers from logit_abi.h, but this file has to exercise the mechanism
     * without depending on any particular kernel struct. */
#define LAY_T "T = layout(\"t\", 28, [[\"year\",0,4,\"i\"], [\"month\",4,4,\"i\"], [\"hour\",12,4,\"i\"]])\n"
    ok("lay_rw",     LAY_T "t = T()\nt.year = 2026\nt.hour = 13\nprint(t.year, t.month, t.hour)\n",
                     "2026 0 13\n");
    ok("lay_print",  LAY_T "t = T()\nt.year = 7\nprint(t)\nprint(len(t))\n",
                     "t(year=7, month=0, hour=0)\n28\n");
    ok("lay_is_mem", LAY_T "t = T()\nt.year = 1\npoke32(addr(t) + 4, 9)\nprint(t.month)\n"
                     "t.hour = 5\nprint(peek32(addr(t) + 12))\n", "9\n5\n");
    ok("lay_signed", "W = layout(\"w\", 16, [[\"b\",0,1,\"i\"], [\"ub\",1,1,\"u\"], [\"h\",2,2,\"i\"], [\"q\",8,8,\"i\"]])\n"
                     "w = W()\nw.b = -1\nw.ub = 255\nw.h = -300\nw.q = -1\nprint(w.b, w.ub, w.h, w.q)\n",
                     "-1 255 -300 -1\n");
    ok("lay_span",   "N = layout(\"n\", 8, [[\"mac\",0,6,\"s\"]])\n"
                     "n = N()\nn.mac = \"abcdef\"\nprint(len(n.mac), n.mac)\n"
                     "n.mac = \"ab\"\nprint(len(n.mac), ord(n.mac[2]))\n",
                     "6 abcdef\n6 0\n");
    err("lay_span_long", "N = layout(\"n\", 8, [[\"mac\",0,6,\"s\"]])\nn = N()\nn.mac = \"toolongxx\"\n");
    err("lay_no_field",  LAY_T "print(T().nope)\n");
    err("lay_ctor_args", LAY_T "T(1)\n");
    err("lay_oob",       "layout(\"x\", 4, [[\"a\",2,4,\"i\"]])\n");   /* field past the end */
    err("lay_badwidth",  "layout(\"x\", 8, [[\"a\",0,3,\"i\"]])\n");   /* 3 is not a scalar width */
    /* One site, two different layouts, then a layout and an ordinary instance:
     * the cache is keyed by shape id and both must miss cleanly rather than
     * reuse the other's offset. */
    ok("lay_ic_poly", LAY_T "U = layout(\"u\", 8, [[\"hour\",4,4,\"i\"]])\n"
                      "def get(x):\n    return x.hour\n"
                      "t = T()\nt.hour = 13\nu = U()\nu.hour = 99\n"
                      "print(get(t), get(u), get(t), get(u))\n", "13 99 13 99\n");
    ok("lay_ic_mixed", LAY_T "class C:\n    def init(self):\n        self.hour = 1\n"
                      "def get(x):\n    return x.hour\n"
                      "t = T()\nt.hour = 13\nc = C()\n"
                      "print(get(t), get(c), get(t), get(c))\n", "13 1 13 1\n");
    /* buffer(): alloc() without the dealloc() -- the collector owns it, so the
     * loop below leaks nothing even though nothing is freed. */
    ok("buf_basic",  "b = buffer(16)\nprint(len(b), b)\npoke64(addr(b), 0x4142434445464748)\n"
                     "print(mem2str(b, 4))\n", "16 <buffer 16>\nHGFE\n");
    ok("buf_gc",     "n = 0\nfor i in range(3000):\n    b = buffer(64)\n    poke32(addr(b), i)\n"
                     "    n = n + peek32(addr(b))\nprint(n)\n", "4498500\n");
    err("buf_field", "b = buffer(8)\nprint(b.x)\n");

    /* ---- shapes + the property inline cache: the cases a cache gets wrong ----
     * Instance fields live in numbered slots now, with the layout ("shape")
     * shared by every instance built the same way, and each property site
     * remembering the shape and slot it last saw. All of these read naturally
     * and would pass without any cache at all -- they exist because each one
     * puts a *different* shape through a site that has already been armed. */
    ok("shape_order",  "class T:\n    def init(self, k):\n        if k:\n            self.p = 1\n            self.q = 2\n"
                       "        else:\n            self.q = 2\n            self.p = 1\n"
                       "def read(o):\n    return o.p\n"
                       "x = T(1)\ny = T(0)\nprint(read(x), read(y), read(x))\n", "1 1 1\n");
    ok("shape_grow",   "class B:\n    def init(self):\n        self.a = 1\n"
                       "b = B()\nb.z = 26\nb.m = 13\nprint(b.a, b.z, b.m)\nb.a = 99\nprint(b.a, b.z, b.m)\n",
                       "1 26 13\n99 26 13\n");
    ok("shape_poly",   "class A:\n    def init(self):\n        self.v = 1\n"
                       "class B:\n    def init(self):\n        self.w = 0\n        self.v = 2\n"
                       "def get(o):\n    return o.v\n"
                       "a = A()\nb = B()\nprint(get(a), get(b), get(a), get(b))\n", "1 2 1 2\n");
    ok("shape_field_over_method",
                       "class S:\n    def init(self):\n        self.go = 7\n    def go(self):\n        return \"m\"\n"
                       "print(S().go)\n", "7\n");
    ok("shape_many",   "class W:\n    def init(self):\n        self.a = 1\n"
                       "w = W()\nw.b = 2\nw.c = 3\nw.d = 4\nw.e = 5\nw.f = 6\n"
                       "print(w.a + w.b + w.c + w.d + w.e + w.f)\n", "21\n");
    err("shape_missing", "class E:\n    def init(self):\n        self.a = 1\nprint(E().nope)\n");

    /* ---- range is lazy: it must stay indistinguishable from the list it was ----
     * range() computes elements on demand instead of building an ObjList, so a
     * 2,000,000-iteration loop costs 1.7 MB instead of 33 MB -- which on Logit,
     * where /bin/as has a 24 MiB static arena, is the difference between running
     * and not. Nothing in the bytecode changed: `for x in seq` compiles to
     * OP_LEN + OP_INDEX_GET, and answering those two is what makes a type
     * iterable. These pin the rest of the surface a list used to provide. */
    ok("range_len",   "print(len(range(1000000)))\n", "1000000\n");
    ok("range_index", "print(range(10)[3], range(10)[-1])\n", "3 9\n");
    ok("range_neg",   "print(range(5, 0, -1))\n", "[5, 4, 3, 2, 1]\n");
    ok("range_in",    "print(3 in range(0, 10, 3), 4 in range(0, 10, 3), 12 in range(0, 10, 3))\n",
                      "true false false\n");
    ok("range_str",   "print(str(range(4)))\n", "[0, 1, 2, 3]\n");
    ok("range_seq",   "def f(xs):\n    t = 0\n    for x in xs:\n        t = t + x\n    return t\n"
                      "print(f(range(1, 5)))\n", "10\n");
    ok("range_huge",  "n = 0\nfor i in range(500000):\n    n = n + 1\nprint(n)\n", "500000\n");
    err("range_set",  "r = range(5)\nr[0] = 9\n");
    err("range_grow", "r = range(5)\nr.append(9)\n");
    err("range_oob",  "print(range(3)[3])\n");

    /* ---- global resolution: the cases a global-lookup cache gets wrong ----
     * A global read memoises where the name resolved (ObjFn.gcache). These pin
     * the two events that must invalidate it: a module-level def taking over a
     * name that previously resolved to a builtin (fsroot/as/lib/stats.as really
     * does define `range`), and a function compiled before the global it reads
     * exists. Both would pass trivially without a cache, which is the point --
     * they only ever fail when the invalidation is wrong. */
    ok("gshadow",    "def show():\n    return range(3)\nprint(show())\n"
                     "def range(n):\n    return \"shadowed\" + str(n)\nprint(show())\n",
                     "[0, 1, 2]\nshadowed3\n");
    ok("gshadow_len","print(len(\"abcd\"))\ndef len(x):\n    return -1\nprint(len(\"abcd\"))\n", "4\n-1\n");
    ok("gforward",   "def get():\n    return later\nlater = 41\nprint(get())\nlater = 42\nprint(get())\n", "41\n42\n");
    ok("gset_cached","def bump():\n    global_n = 0\nn = 1\ndef inc():\n    return n\nprint(inc())\nn = 2\nprint(inc())\n", "1\n2\n");

    /* ---- M23.5: memory natives for the sys/gui libraries ---- */
    ok("alloc_rt",   "b = alloc(16)\npoke32(addr(b), 0x41424344)\npoke8(addr(b) + 4, 0)\nprint(mem2cstr(b))\nprint(mem2str(b, 2))\ndealloc(b)\nprint(\"freed\")\n",
                     "DCBA\nDC\nfreed\n");
    ok("alloc_zero", "b = alloc(8)\nprint(peek64(addr(b)))\ndealloc(b)\n", "0\n");
    ok("argv_build", "args = [\"echo\", \"hi\"]\npv = alloc(8 * 3)\nfor i in range(2):\n    poke64(addr(pv) + 8 * i, addr(args[i]))\npoke64(addr(pv) + 16, 0)\nprint(mem2cstr(peek64(addr(pv) + 8)))\ndealloc(pv)\n", "hi\n");
    err("alloc_bad",  "alloc(0)\n");
    err("dealloc_bad","dealloc(42)\n");

    /* ---- M21-P3 S0: self-hosting natives ---- */
    ok("chr_ord",    "print(chr(65) + chr(66))\nprint(ord(\"a\"), ord(\"\\n\"))\nprint(chr(0) == \"\\0\")\n", "AB\n97 10\ntrue\n");
    err("chr_range",  "chr(256)\n");
    ok("chr_join",   "print(len(\"\".join([chr(b) for b in range(256)])))\n", "256\n");
    ok("f64bits",    "print(f64bits(1.0))\nprint(f64bits(2.0))\nprint(f64bits(0.5) == 0x3FE0000000000000)\n",
                     "4607182418800017408\n4611686018427387904\ntrue\n");
    ok("file_rt",    "p = \"/tmp/as_s0_test.bin\"\ndata = \"\".join([chr(b % 256) for b in range(1000)])\nprint(file_write(p, data))\nback = file_read(p)\nprint(len(back), back == data)\nprint(file_read(\"/no/such/file\") == nil)\n",
                     "1000\n1000 true\ntrue\n");
    ok("str_sub",    "s = \"hello world\"\nprint(s.sub(0, 5))\nprint(s.sub(6, 99))\nprint(s.sub(3, 3) == \"\")\n", "hello\nworld\ntrue\n");
    ok("prop_index_set", "class C:\n    def init(self):\n        self.d = {}\n        self.xs = [0, 0]\n    def go(self):\n        self.d[\"k\"] = 7\n        self.xs[1] = 9\n        return self.d[\"k\"] + self.xs[1]\nprint(C().go())\n", "16\n");
    ok("prop_compound", "class P:\n    def init(self):\n        self.i = 10\n    def bump(self):\n        self.i += 5\n        self.i *= 2\n        self.i -= 4\np = P()\np.bump()\nprint(p.i)\n", "26\n");
    ok("args_host",  "a = args()\nprint(a == nil or len(a) >= 0)\n", "true\n");

    /* ---- M21 phase 3: .la bytecode serialize/deserialize round-trip ---- */
    bc_tests();

    printf(fails ? "\n%d/%d AetherScript checks FAILED\n" : "\nall %d AetherScript checks passed\n",
           fails ? fails : total, total);
    return fails ? 1 : 0;
}
