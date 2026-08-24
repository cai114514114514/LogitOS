/* The program tcc links for tests/tcc.mk: a bare ELF, compiled and LINKED by
 * tcc against mini-libc and crt0_cli, never wrapped by mkaex. On the device it
 * exercises the whole startup path under our libc -- crt0's argc/argv, stdio's
 * buffered printf, exit's flush -- and prints the one line the harness counts.
 *
 * argc and argv[1] are printed rather than a constant so the line can only be
 * produced by a process that was handed a real SysV stack: a wrong rsp at
 * entry gives a fault or garbage, not this string. */
#include <stdio.h>

int main(int argc, char **argv)
{
    printf("HELLO-ELF ok argc=%d argv1=%s\n", argc, argc > 1 ? argv[1] : "(none)");
    return 0;
}
