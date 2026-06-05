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

int main(void)
{
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

    /* errors are reported, not crashed */
    err("undef",   "print(nope)\n");
    err("badindent", "x = 1\n  y = 2\n");
    err("arity",   "def g(a):\n    return a\nprint(g(1, 2))\n");

    printf(fails ? "\n%d/%d AquaScript A1 checks FAILED\n" : "\nall %d AquaScript A1 checks passed\n",
           fails ? fails : total, total);
    return fails ? 1 : 0;
}
