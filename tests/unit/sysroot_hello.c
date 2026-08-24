/* sysroot_hello.c -- the program tests/sysroot.mk compiles and links with tcc
 * against the packed sysroot, and the one tests/boot/run-sysroot-test.sh runs
 * on the device as /bin/hello-tcc.
 *
 * Every line is there to reach a different part of the sysroot, so that a
 * link which succeeds has demonstrably touched all of it:
 *
 *   printf            libc.a, the stdio members and the syscall layer under it
 *   sqrt              libc.a's libm half (musl's sqrt.o)
 *   sum(3, ...)       a VARIADIC function: tcc's stdarg.h expands va_start and
 *                     va_arg into calls to __va_start/__va_arg, which live in
 *                     libtcc1.a (third_party/tcc/lib/va_list.c) -- a program
 *                     that merely CALLS printf never pulls libtcc1 at all
 *   (double)big       an unsigned long long to double conversion: tcc has no
 *                     inline sequence for it and calls __floatundidf, the
 *                     other libtcc1 member (lib/libtcc1.c)
 *   argc              the argc/argv/envp layout crt0_cli.asm (crt1.o) builds
 *
 * The expected output is fixed so the device harness can grep for it:
 *   hello from tcc: sum=42 sqrt2=1.41421 big=9223372036854775808 argc=1
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int sum(int n, ...)
{
    va_list ap;
    int s = 0;
    va_start(ap, n);
    while (n-- > 0)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}

int main(int argc, char **argv)
{
    unsigned long long big = 1ULL << 63;
    double d = (double)big;
    (void)argv;
    printf("hello from tcc: sum=%d sqrt2=%.5f big=%.0f argc=%d\n",
           sum(3, 1, 2, 39), sqrt(2.0), d, argc);
    return 0;
}
