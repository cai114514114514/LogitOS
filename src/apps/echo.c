#include "aqua.h"

/* echo [args...] -- write the arguments separated by spaces, then a newline,
 * to stdout (fd 1). A real exec'able CLI program: crt0_cli passes argc/argv. */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        sys_write(1, argv[i], slen(argv[i]));
        sys_write(1, i + 1 < argc ? " " : "", i + 1 < argc ? 1 : 0);
    }
    sys_write(1, "\n", 1);
    return 0;
}
