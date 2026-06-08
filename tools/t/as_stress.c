/* AetherScript stress / torture test.
 * Pushes the VM to its limits: deep recursion, huge allocations,
 * many locals, massive churn, boundary conditions, and crash resilience.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "as.h"

static int fails = 0, total = 0;
static double t_start;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void ok(const char *name, const char *src, const char *want)
{
    char buf[65536];
    as_capture(buf, sizeof(buf));
    int r = as_interpret(src);
    as_capture(NULL, 0);
    total++;
    if (r != 0) {
        fails++;
        printf("FAIL %-26s error: %s\n", name, as_err);
        as_free_objects();
        return;
    }
    if (strcmp(buf, want) != 0) {
        fails++;
        printf("FAIL %-26s want [%s] got [%s]\n", name, want, buf);
    } else {
        printf("PASS %-26s (%.3fs)\n", name, now() - t_start);
    }
    as_free_objects();
}

static void err(const char *name, const char *src)
{
    char buf[1024];
    as_capture(buf, sizeof(buf));
    int r = as_interpret(src);
    as_capture(NULL, 0);
    total++;
    if (r == 0) {
        fails++;
        printf("FAIL %-26s expected error, got [%s]\n", name, buf);
    } else {
        printf("PASS %-26s caught: %.60s\n", name, as_err);
    }
    as_free_objects();
}

static void gc_bounded(const char *name, const char *src, long max_live)
{
    char buf[256];
    as_capture(buf, sizeof(buf));
    int r = as_interpret(src);
    as_capture(NULL, 0);
    total++;
    if (r != 0) {
        fails++;
        printf("FAIL %-26s error: %s\n", name, as_err);
        as_free_objects();
        return;
    }
    long live = as_gc_live();
    as_free_objects();
    if (live > max_live) {
        fails++;
        printf("FAIL %-26s live=%ld > max=%ld\n", name, live, max_live);
    } else {
        printf("PASS %-26s live=%ld (%.3fs)\n", name, live, now() - t_start);
    }
}

int main(void)
{
    printf("AetherScript STRESS TEST\n");
    printf("======================\n\n");

    /* ---------- 1. MASSIVE ALLOCATION / GC CHURN ---------- */
    printf("--- GC churn ---\n");

    t_start = now();
    gc_bounded("gc_churn_100k",
        "def churn():\n"
        "    i = 0\n"
        "    while i < 100000:\n"
        "        x = [i, i + 1, i + 2]\n"
        "        i = i + 1\n"
        "    return 0\n"
        "churn()\n"
        "print(gc_stats())\n",
        2000);

    t_start = now();
    gc_bounded("gc_churn_1M",
        "def churn():\n"
        "    i = 0\n"
        "    while i < 1000000:\n"
        "        x = [i]\n"
        "        i = i + 1\n"
        "    return 0\n"
        "churn()\n"
        "print(gc_stats())\n",
        10000);

    t_start = now();
    gc_bounded("gc_dict_churn_50k",
        "def churn():\n"
        "    i = 0\n"
        "    while i < 50000:\n"
        "        d = {i: i * i}\n"
        "        i = i + 1\n"
        "    return 0\n"
        "churn()\n"
        "print(gc_stats())\n",
        2000);

    t_start = now();
    ok("gc_big_survivor",
        "big = []\n"
        "i = 0\n"
        "while i < 50000:\n"
        "    big.append(i)\n"
        "    i = i + 1\n"
        "gc()\n"
        "print(len(big), big[49999])\n",
        "50000 49999\n");

    /* ---------- 2. DEEP RECURSION / STACK ---------- */
    printf("\n--- stack / recursion ---\n");

    t_start = now();
    ok("recurse_200",
        "def f(n):\n"
        "    if n == 0:\n"
        "        return 0\n"
        "    return 1 + f(n - 1)\n"
        "print(f(200))\n",
        "200\n");

    t_start = now();
    ok("recurse_250",
        "def f(n):\n"
        "    if n == 0:\n"
        "        return 0\n"
        "    return 1 + f(n - 1)\n"
        "print(f(250))\n",
        "250\n");

    t_start = now();
    err("recurse_infinite",
        "def f():\n"
        "    return f()\n"
        "f()\n");

    t_start = now();
    ok("mutual_recurse",
        "def even(n):\n"
        "    if n == 0:\n"
        "        return true\n"
        "    return odd(n - 1)\n"
        "def odd(n):\n"
        "    if n == 0:\n"
        "        return false\n"
        "    return even(n - 1)\n"
        "print(even(100))\n",
        "true\n");

    /* ---------- 3. MANY LOCALS ---------- */
    printf("\n--- many locals ---\n");

    t_start = now();
    ok("locals_50",
        "def f():\n"
        "    a0 = 0\n"
        "    a1 = 1\n"
        "    a2 = 2\n"
        "    a3 = 3\n"
        "    a4 = 4\n"
        "    a5 = 5\n"
        "    a6 = 6\n"
        "    a7 = 7\n"
        "    a8 = 8\n"
        "    a9 = 9\n"
        "    b0 = 10\n"
        "    b1 = 11\n"
        "    b2 = 12\n"
        "    b3 = 13\n"
        "    b4 = 14\n"
        "    b5 = 15\n"
        "    b6 = 16\n"
        "    b7 = 17\n"
        "    b8 = 18\n"
        "    b9 = 19\n"
        "    c0 = 20\n"
        "    c1 = 21\n"
        "    c2 = 22\n"
        "    c3 = 23\n"
        "    c4 = 24\n"
        "    c5 = 25\n"
        "    c6 = 26\n"
        "    c7 = 27\n"
        "    c8 = 28\n"
        "    c9 = 29\n"
        "    d0 = 30\n"
        "    d1 = 31\n"
        "    d2 = 32\n"
        "    d3 = 33\n"
        "    d4 = 34\n"
        "    d5 = 35\n"
        "    d6 = 36\n"
        "    d7 = 37\n"
        "    d8 = 38\n"
        "    d9 = 39\n"
        "    e0 = 40\n"
        "    e1 = 41\n"
        "    e2 = 42\n"
        "    e3 = 43\n"
        "    e4 = 44\n"
        "    e5 = 45\n"
        "    e6 = 46\n"
        "    e7 = 47\n"
        "    e8 = 48\n"
        "    e9 = 49\n"
        "    t = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9\n"
        "    t = t + b0 + b1 + b2 + b3 + b4 + b5 + b6 + b7 + b8 + b9\n"
        "    t = t + c0 + c1 + c2 + c3 + c4 + c5 + c6 + c7 + c8 + c9\n"
        "    t = t + d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + d9\n"
        "    t = t + e0 + e1 + e2 + e3 + e4 + e5 + e6 + e7 + e8 + e9\n"
        "    return t\n"
        "print(f())\n",
        "1225\n");

    /* ---------- 4. HUGE CONTAINERS ---------- */
    printf("\n--- huge containers ---\n");

    t_start = now();
    ok("dict_100k",
        "d = {}\n"
        "i = 0\n"
        "while i < 100000:\n"
        "    d[i] = i\n"
        "    i = i + 1\n"
        "print(len(d), d[99999])\n",
        "100000 99999\n");

    t_start = now();
    ok("list_100k",
        "xs = []\n"
        "i = 0\n"
        "while i < 100000:\n"
        "    xs.append(i)\n"
        "    i = i + 1\n"
        "print(len(xs), xs[99999])\n",
        "100000 99999\n");

    t_start = now();
    ok("dict_remove_50k",
        "d = {}\n"
        "i = 0\n"
        "while i < 50000:\n"
        "    d[i] = i\n"
        "    i = i + 1\n"
        "i = 0\n"
        "while i < 50000:\n"
        "    d.remove(i)\n"
        "    i = i + 1\n"
        "print(len(d), d.has(0))\n",
        "0 false\n");

    /* ---------- 5. STRING STRESS ---------- */
    printf("\n--- string stress ---\n");

    t_start = now();
    ok("str_concat_5k",
        "s = \"\"\n"
        "i = 0\n"
        "while i < 5000:\n"
        "    s = s + \"x\"\n"
        "    i = i + 1\n"
        "print(len(s))\n",
        "5000\n");

    t_start = now();
    ok("str_concat_10k",
        "s = \"\"\n"
        "i = 0\n"
        "while i < 10000:\n"
        "    s = s + \"x\"\n"
        "    i = i + 1\n"
        "print(len(s))\n",
        "10000\n");

    /* ---------- 6. CLOSURE STRESS ---------- */
    printf("\n--- closure stress ---\n");

    t_start = now();
    ok("closure_array_1k",
        "def make(n):\n"
        "    fns = []\n"
        "    i = 0\n"
        "    while i < n:\n"
        "        x = i\n"
        "        def get():\n"
        "            return x\n"
        "        fns.append(get)\n"
        "        i = i + 1\n"
        "    return fns\n"
        "fs = make(1000)\n"
        "print(fs[0](), fs[500](), fs[999]())\n",
        "0 500 999\n");

    t_start = now();
    ok("closure_nested_deep",
        "def a(x):\n"
        "    def b(y):\n"
        "        def c(z):\n"
        "            def d(w):\n"
        "                return x + y + z + w\n"
        "            return d\n"
        "        return c\n"
        "    return b\n"
        "print(a(1)(2)(3)(4))\n",
        "10\n");

    t_start = now();
    ok("closure_shared_write",
        "def make():\n"
        "    c = 0\n"
        "    def inc():\n"
        "        c = c + 1\n"
        "        return c\n"
        "    def dec():\n"
        "        c = c - 1\n"
        "        return c\n"
        "    return [inc, dec]\n"
        "ops = make()\n"
        "i = 0\n"
        "while i < 10000:\n"
        "    ops[0]()\n"
        "    i = i + 1\n"
        "print(ops[1]())\n",
        "9999\n");

    /* ---------- 7. CLASS / INSTANCE STRESS ---------- */
    printf("\n--- class stress ---\n");

    t_start = now();
    ok("class_1k_instances",
        "class Box:\n"
        "    def init(self, v):\n"
        "        self.v = v\n"
        "xs = []\n"
        "i = 0\n"
        "while i < 1000:\n"
        "    xs.append(Box(i))\n"
        "    i = i + 1\n"
        "print(len(xs), xs[999].v)\n",
        "1000 999\n");

    t_start = now();
    ok("inherit_chain_5",
        "class A:\n"
        "    def f(self):\n"
        "        return 1\n"
        "class B(A):\n"
        "    def f(self):\n"
        "        return super.f() + 1\n"
        "class C(B):\n"
        "    def f(self):\n"
        "        return super.f() + 1\n"
        "class D(C):\n"
        "    def f(self):\n"
        "        return super.f() + 1\n"
        "class E(D):\n"
        "    def f(self):\n"
        "        return super.f() + 1\n"
        "print(E().f())\n",
        "5\n");

    /* ---------- 8. EXCEPTION STRESS ---------- */
    printf("\n--- exception stress ---\n");

    t_start = now();
    ok("except_loop_10k",
        "n = 0\n"
        "i = 0\n"
        "while i < 10000:\n"
        "    try:\n"
        "        if i % 2 == 0:\n"
        "            raise i\n"
        "    except e:\n"
        "        n = n + e\n"
        "    i = i + 1\n"
        "print(n)\n",
        "24995000\n");

    t_start = now();
    ok("except_nested_deep",
        "def boom(n):\n"
        "    if n == 0:\n"
        "        raise \"done\"\n"
        "    try:\n"
        "        return boom(n - 1)\n"
        "    except e:\n"
        "        raise e\n"
        "try:\n"
        "    boom(100)\n"
        "except e:\n"
        "    print(e)\n",
        "done\n");

    /* ---------- 9. BOUNDARY / EDGE CASES ---------- */
    printf("\n--- edge cases ---\n");

    t_start = now();
    ok("empty_list_ops",
        "xs = []\n"
        "print(len(xs))\n",
        "0\n");

    t_start = now();
    err("list_neg_oob",
        "print([1, 2][-3])\n");

    t_start = now();
    err("list_pos_oob",
        "print([1, 2][5])\n");

    t_start = now();
    ok("dict_empty_access",
        "d = {}\n"
        "print(len(d), d.has(\"x\"))\n",
        "0 false\n");

    t_start = now();
    ok("range_zero",
        "print(range(0))\n",
        "[]\n");

    t_start = now();
    ok("range_neg_step",
        "print(range(5, 0, -1))\n",
        "[5, 4, 3, 2, 1]\n");

    t_start = now();
    ok("nil_identity",
        "print(nil == nil)\n",
        "true\n");

    t_start = now();
    ok("bool_truthy",
        "print(true == true, false == false, true != false)\n",
        "true true true\n");

    /* ---------- 10. MODULE IMPORT STRESS ---------- */
    printf("\n--- module stress ---\n");

    {
        /* Must allocate on heap: as_add_module_source stores raw pointers. */
        char *names[32];
        char *srcs[32];
        for (int i = 0; i < 20; i++) {
            names[i] = (char *)malloc(16);
            srcs[i] = (char *)malloc(128);
            snprintf(names[i], 16, "mod%d", i);
            snprintf(srcs[i], 128, "val = %d\ndef get():\n    return val\n", i);
            as_add_module_source(names[i], srcs[i]);
        }

        t_start = now();
        ok("import_20_modules",
            "import mod0\n"
            "import mod1\n"
            "import mod2\n"
            "import mod3\n"
            "import mod4\n"
            "import mod5\n"
            "import mod6\n"
            "import mod7\n"
            "import mod8\n"
            "import mod9\n"
            "import mod10\n"
            "import mod11\n"
            "import mod12\n"
            "import mod13\n"
            "import mod14\n"
            "import mod15\n"
            "import mod16\n"
            "import mod17\n"
            "import mod18\n"
            "import mod19\n"
            "print(mod0.get(), mod19.get())\n",
            "0 19\n");

        for (int i = 0; i < 20; i++) {
            free(names[i]);
            free(srcs[i]);
        }
    }

    /* ---------- 11. COMPILE-TIME LIMITS ---------- */
    printf("\n--- compile limits ---\n");

    {
        char *src = (char *)malloc(32768);
        strcpy(src, "print(");
        int off = strlen(src);
        for (int i = 0; i < 300; i++) {
            off += snprintf(src + off, 32768 - off, "%d%s", i, i < 299 ? ", " : "");
        }
        strcat(src, ")\n");
        t_start = now();
        err("too_many_consts", src);
        free(src);
    }

    /* ---------- 12. MEMORY LEAK DETECTION ---------- */
    printf("\n--- memory leak check ---\n");

    t_start = now();
    {
        char buf[256];
        as_capture(buf, sizeof(buf));
        for (int run = 0; run < 100; run++) {
            int r = as_interpret(
                "def f(n):\n"
                "    if n == 0:\n"
                "        return 0\n"
                "    return 1 + f(n - 1)\n"
                "print(f(50))\n");
            as_free_objects();
            if (r != 0) {
                printf("FAIL leak_check crashed on run %d: %s\n", run, as_err);
                fails++;
                total++;
                break;
            }
        }
        as_capture(NULL, 0);
        total++;
        printf("PASS leak_check              100 runs survived (%.3fs)\n", now() - t_start);
    }

    printf(fails ? "\n%d/%d stress checks FAILED\n" : "\nall %d stress checks passed\n",
           fails ? fails : total, total);
    return fails ? 1 : 0;
}
