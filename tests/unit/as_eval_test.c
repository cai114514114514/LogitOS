/* SPDX-License-Identifier: MIT */
/* THE GUEST EVALUATOR, WHICH NOTHING TESTED.
 *
 * c/apps/as/cli/eval.c is what `Run` in Code Studio actually executes: the
 * machine has no LLVM, so commands.c routes a freestanding build's Run to
 * as_typed_eval() and walks the checked tree. Every other A3 gate in this tree
 * drives the LLVM backend or the retired A2 VM, so the one path a person uses
 * by clicking a button had no gate at all -- and the way that surfaced is the
 * reason this file exists: fsroot/docs/demo.as, the demo shipped to be run
 * from that button, could not run.
 *
 * THE BAR IS THE PRINTED BYTES, not "it did not crash". Each case states the
 * exact stdout the program must produce, because the failure mode this
 * evaluator has is a WRONG ANSWER, not a refusal: it shares no arithmetic with
 * the LLVM backend, so an operator implemented with the wrong precedence, the
 * wrong signedness or the wrong associativity prints a plausible number.
 *
 * THE REFUSALS ARE ASSERTED TOO (refuse()). What this evaluator must never do
 * is execute half a program and exit 0 -- a construct it does not implement has
 * to name itself and fail, which is the property eval.c's own header claims
 * ("unsupported nodes refuse instead of inventing a VM"). A gate that only
 * checked the features that work would let a future edit turn a refusal into a
 * silent no-op, which is the one outcome worse than the error the user saw.
 */
#include "include/project.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, total;

/* Run `src` as a whole program and return its stdout in `out`, the exit status
 * in *status. stdout is captured through a real file rather than a pipe: the
 * evaluator prints with printf and a pipe would need draining while the child
 * runs, which this harness has no thread to do. */
static int run_source(const char *src, char *out, size_t cap, int *status)
{
    char path[] = "/tmp/as_eval_src_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, src, strlen(src)) != (long)strlen(src)) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    AsSourceOverlay overlay = {.path = path, .source = src, .bytes = strlen(src)};
    AsTypedProject *p = as_typed_check(path, &overlay, 1);
    if (!p || as_typed_errors(p)) {
        if (p) {
            as_typed_report(p, stdout, 0);
            as_typed_free(p);
        }
        unlink(path);
        return -1;
    }
    char cap_path[] = "/tmp/as_eval_out_XXXXXX";
    int cfd = mkstemp(cap_path);
    if (cfd < 0) {
        as_typed_free(p);
        unlink(path);
        return -1;
    }
    fflush(stdout);
    int saved = dup(1);
    dup2(cfd, 1);
    *status = as_typed_eval(p);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    lseek(cfd, 0, SEEK_SET);
    long n = read(cfd, out, cap - 1);
    out[n < 0 ? 0 : n] = 0;
    close(cfd);
    unlink(cap_path);
    as_typed_free(p);
    unlink(path);
    return 0;
}

static void ok(const char *name, const char *src, const char *want)
{
    char got[8192];
    int status = -1;
    total++;
    if (run_source(src, got, sizeof got, &status) < 0) {
        fails++;
        printf("FAIL %-28s did not compile\n", name);
        return;
    }
    if (status != 0) {
        fails++;
        printf("FAIL %-28s exit %d (expected 0)\n", name, status);
        return;
    }
    if (strcmp(got, want)) {
        fails++;
        printf("FAIL %-28s\n  want [%s]\n  got  [%s]\n", name, want, got);
    }
}

/* A construct the evaluator does not implement must FAIL, not return 0 having
 * run part of the program. */
static void refuse(const char *name, const char *src)
{
    char got[8192];
    int status = 0;
    total++;
    if (run_source(src, got, sizeof got, &status) < 0) {
        fails++;
        printf("FAIL %-28s did not compile\n", name);
        return;
    }
    if (status == 0) {
        fails++;
        printf("FAIL %-28s exited 0; a refusal was required\n", name);
    }
}

#define PROG(body) "# aether: 3.0\ndef main() -> None:\n" body

int main(void)
{
    /* --- what already worked, pinned so the additions cannot regress it --- */
    ok("print string", PROG("    print(\"hello world\")\n"), "hello world\n");
    ok("arithmetic", PROG("    print(2 + 3 * 4)\n"), "14\n");
    ok("if/else", PROG("    if 1 < 2:\n        print(\"yes\")\n    else:\n        print(\"no\")\n"),
       "yes\n");
    ok("while", PROG("    i = 0\n    while i < 3:\n        print(i)\n        i = i + 1\n"),
       "0\n1\n2\n");
    ok("for range", PROG("    for i in range(3):\n        print(i)\n"), "0\n1\n2\n");

    /* --- the five demo.as needs, none of which the evaluator implemented --- */
    /* THE LINE AFTER THE LOOP IS THE ASSERTION. A `break` that is the last
     * thing a function does is indistinguishable from a `return`: without the
     * trailing print, this case passes against AS_EVAL_NO_UNWIND -- the
     * control that turns break into return -- and the gate would have been
     * green on a build that could not tell them apart. */
    ok("break", PROG("    for i in range(8):\n        if i == 3:\n            break\n"
                     "        print(i)\n    print(\"after\")\n"),
       "0\n1\n2\nafter\n");
    ok("continue", PROG("    for i in range(4):\n        if i == 1:\n            continue\n"
                        "        print(i)\n"),
       "0\n2\n3\n");
    ok("break in while", PROG("    i = 0\n    while i < 9:\n        if i == 2:\n            break\n"
                              "        print(i)\n        i = i + 1\n    print(\"after\")\n"),
       "0\n1\nafter\n");
    ok("compound assignment",
       PROG("    total = 0\n    for i in range(4):\n        total += i\n    print(total)\n"),
       "6\n");
    ok("compound assignment ops",
       PROG("    n = 10\n    n -= 3\n    n *= 4\n    print(n)\n"), "28\n");
    ok("user call",
       "# aether: 3.0\ndef sq(n: i64) -> i64:\n    return n * n\n\n"
       "def main() -> None:\n    print(sq(7))\n",
       "49\n");
    ok("user call recursion",
       "# aether: 3.0\ndef fact(n: i64) -> i64:\n    if n < 2:\n        return 1\n"
       "    return n * fact(n - 1)\n\ndef main() -> None:\n    print(fact(6))\n",
       "720\n");
    ok("user call no result",
       "# aether: 3.0\ndef show(n: i64) -> None:\n    print(n)\n\n"
       "def main() -> None:\n    show(4)\n    show(5)\n",
       "4\n5\n");
    /* STRINGS ACROSS A FRAME BOUNDARY, which is where this evaluator's memory
     * discipline is actually tested: the argument is a borrow in the caller
     * and must be copied into the callee's slot, and the result is built in
     * the callee and must survive that frame being cleared. Both directions in
     * one case, run under ASan by test-as-eval-asan. */
    ok("string through a call",
       "# aether: 3.0\ndef greet(who: str) -> str:\n    return \"hello \" + who\n\n"
       "def main() -> None:\n    print(greet(\"world\"))\n",
       "hello world\n");
    ok("string local through a call",
       "# aether: 3.0\ndef twice(s: str) -> str:\n    return s + s\n\n"
       "def main() -> None:\n    a = \"ab\"\n    b = twice(a)\n    print(a, b)\n",
       "ab abab\n");
    ok("bit operators", PROG("    print(255 & 0x0f, 1 << 4, 2 ** 10)\n"), "15 16 1024\n");
    ok("bit or/xor/shr", PROG("    print(8 | 1, 6 ^ 3, 64 >> 3)\n"), "9 5 8\n");
    ok("unary minus/complement", PROG("    n = 5\n    print(-n, ~n)\n"), "-5 -6\n");
    ok("unary not", PROG("    print(not True, not False)\n"), "false true\n");

    /* --- top-level statements become main(): the shape a person types first -
     * AS3900 says so in as many words ("top-level statements that become
     * main()"), and those variables are module GLOBALS, not function locals.
     * The four-line program below is what a user actually ran in Studio; it
     * died on `guest execution does not implement this expression yet` -- the
     * fallback for an unknown NODE, reached because nothing here read a
     * global. */
    ok("top-level statements", "# aether: 3.0\na = 1\nb = 2\nc = a / b\nprint(c)\n", "0\n");
    ok("top-level string", "# aether: 3.0\ns = \"hi\"\nprint(s)\n", "hi\n");
    /* The shared jar: the module initializer WRITES a global and a function
     * called from main READS it -- two frames, one store. A frame-local
     * evaluator answers 0 here. (A module variable is assigned once at module
     * scope: AS3200 refuses a second one, and AS3206 refuses a compound
     * assignment in a module-scope loop. Both are checker flow rules and
     * neither has anything to do with execution.) */
    ok("global read by a function",
       "# aether: 3.0\nbase = 7\n\ndef twice() -> i64:\n    return base * 2\n\n"
       "def main() -> None:\n    print(twice())\n",
       "14\n");
    ok("integer division", PROG("    print(7 / 2, 7 % 2, -7 / 2)\n"), "3 1 -3\n");

    /* --- the demo itself, byte for byte (fsroot/docs/demo.as) ------------- */
    ok("demo.as",
       "# aether: 3.0\ndef sq(n: i64) -> i64:\n    return n * n\n\n\n"
       "def main() -> None:\n    total = 0\n    for i in range(8):\n        if i == 5:\n"
       "            break\n        total += sq(i)\n\n"
       "    print(\"sum of squares:\", total)\n"
       "    print(\"bits:\", 255 & 0x0f, 1 << 4, 2 ** 10)\n",
       "sum of squares: 30\nbits: 15 16 1024\n");

    /* --- the boundary still has to be a refusal, not a silent no-op ------- */
    refuse("array index is still refused",
           "# aether: 3.0\ndef main() -> None:\n    xs = [1, 2, 3]\n    print(xs[0])\n");

    printf("as-eval: %d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
