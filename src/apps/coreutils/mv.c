#include "clib.h"

/* mv SRC DST -- rename/move via the kernel's atomic rename syscall. */
int main(int argc, char **argv)
{
    if (argc != 3) { errs("usage: mv <src> <dst>\n"); return 1; }
    if (sys_rename(argv[1], argv[2]) < 0) { errs("mv: cannot rename\n"); return 1; }
    return 0;
}
