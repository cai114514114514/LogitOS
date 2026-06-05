/* AquaScript A1 host unit test.
 * Build: cc -o /tmp/aqs_test tools/t/aqs_test.c \
 *           src/apps/aqs/{value,aqs_io,lexer,compiler,vm,object}.c -Isrc/apps/aqs
 * Runs inline .aqs snippets and asserts their `print` output. */
#include <stdio.h>
#include <string.h>
#include "aqs.h"

static int fails = 0, total = 0;

static void ok(const char *name, const char *src, const char *want)
{
    char buf[8192];
    aqs_capture(buf, sizeof buf);
    int r = aqs_interpret(src);
    aqs_capture(NULL, 0);
    total++;
    if (r != 0) { fails++; printf("FAIL %-10s interpret error: %s\n", name, aqs_err); aqs_free_objects(); return; }
    if (strcmp(buf, want) != 0) {
        fails++;
        printf("FAIL %-10s\n  want [%s]\n  got  [%s]\n", name, want, buf);
    }
    aqs_free_objects();
}

static void err(const char *name, const char *src)   /* expect a compile/runtime error */
{
    char buf[1024];
    aqs_capture(buf, sizeof buf);
    int r = aqs_interpret(src);
    aqs_capture(NULL, 0);
    total++;
    if (r == 0) { fails++; printf("FAIL %-10s expected an error, got none (out=[%s])\n", name, buf); }
    aqs_free_objects();
}

/* an in-memory module for the import tests. `quad` calls its module-mate `square`
 * -> proves a module function resolves globals in its own namespace. */
static const char *MATHX =
    "PI = 3\n"
    "def square(x):\n"
    "    return x * x\n"
    "def quad(x):\n"
    "    return square(square(x))\n";

int main(void)
{
    aqs_add_module_source("mathx", MATHX);

    /* arithmetic + precedence */
    ok("prec",   "print(1 + 2 * 3)\n", "7\n");
    ok("paren",  "print((1 + 2) * 3)\n", "9\n");
    ok("lassoc", "print(10 - 3 - 2)\n", "5\n");
    ok("unary",  "print(-5 + 2)\n", "-3\n");
    ok("intdiv", "print(7 / 2)\n", "3\n");
    ok("fdiv",   "print(7.0 / 2)\n", "3.5\n");
    ok("mod",    "print(17 % 5)\n", "2\n");
    ok("mixed",  "print(2 + 3.5)\n", "5.5\n");

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

    /* errors are reported, not crashed */
    err("undef",   "print(nope)\n");
    err("badindent", "x = 1\n  y = 2\n");
    err("arity",   "def g(a):\n    return a\nprint(g(1, 2))\n");
    err("oob",     "print([1, 2][5])\n");
    err("nomethod","xs = []\nxs.frobnicate()\n");

    printf(fails ? "\n%d/%d AquaScript checks FAILED\n" : "\nall %d AquaScript checks passed\n",
           fails ? fails : total, total);
    return fails ? 1 : 0;
}
